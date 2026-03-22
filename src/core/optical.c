#include <crossos/optical.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

extern void crossos__set_error(const char *fmt, ...);

struct crossos_optical_burn_job {
    char target_device_id[CROSSOS_OPTICAL_DEVICE_ID_MAX];
    uint64_t total_bytes;
    uint64_t bytes_written;
    uint64_t started_ms;
    float speed_mib_s;
    int canceled;
    int failed;
    int done;
};

static uint64_t crossos__now_ms(void)
{
#if defined(_WIN32)
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
#endif
}

#if defined(_WIN32)
static uint64_t crossos__path_size_recursive(const char *path)
{
    DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return 0;
    }

    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) {
            return 0;
        }
        ULARGE_INTEGER size;
        size.HighPart = fad.nFileSizeHigh;
        size.LowPart = fad.nFileSizeLow;
        return (uint64_t)size.QuadPart;
    }

    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);

    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA(pattern, &data);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }

    uint64_t total = 0;
    do {
        if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) {
            continue;
        }

        char child[MAX_PATH];
        snprintf(child, sizeof(child), "%s\\%s", path, data.cFileName);
        total += crossos__path_size_recursive(child);
    } while (FindNextFileA(h, &data));

    FindClose(h);
    return total;
}
#else
static uint64_t crossos__path_size_recursive(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) {
        return 0;
    }

    if (!S_ISDIR(st.st_mode)) {
        return (uint64_t)st.st_size;
    }

    DIR *d = opendir(path);
    if (!d) {
        return 0;
    }

    uint64_t total = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }

        char child[PATH_MAX];
        snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        total += crossos__path_size_recursive(child);
    }

    closedir(d);
    return total;
}
#endif

crossos_result_t crossos_optical_list_devices(crossos_optical_device_t *devices,
                                              size_t max_devices,
                                              size_t *out_count)
{
    if (!out_count) {
        crossos__set_error("optical_list_devices: invalid parameter");
        return CROSSOS_ERR_PARAM;
    }

    *out_count = 0;
    if (max_devices > 0 && !devices) {
        crossos__set_error("optical_list_devices: devices buffer is NULL");
        return CROSSOS_ERR_PARAM;
    }

#if defined(_WIN32)
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26 && *out_count < max_devices; i++) {
        if ((mask & (1u << i)) == 0) {
            continue;
        }

        char root[4] = {(char)('A' + i), ':', '\\', '\0'};
        if (GetDriveTypeA(root) != DRIVE_CDROM) {
            continue;
        }

        crossos_optical_device_t *dev = &devices[*out_count];
        memset(dev, 0, sizeof(*dev));
        snprintf(dev->id, sizeof(dev->id), "%c:", 'A' + i);
        snprintf(dev->label, sizeof(dev->label), "Windows optical drive %c:", 'A' + i);
        dev->can_read = 1;
        dev->can_write = 1;

        char volname[64];
        dev->has_media = GetVolumeInformationA(root, volname, sizeof(volname), NULL, NULL, NULL, NULL, 0) ? 1 : 0;

        ULARGE_INTEGER avail, total, free_bytes;
        if (GetDiskFreeSpaceExA(root, &avail, &total, &free_bytes)) {
            dev->media_capacity_bytes = (uint64_t)total.QuadPart;
            dev->media_free_bytes = (uint64_t)free_bytes.QuadPart;
        }

        (*out_count)++;
    }
#else
    const char *candidates[] = {
        "/dev/sr0", "/dev/sr1", "/dev/cdrom", "/dev/dvd"
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]) && *out_count < max_devices; i++) {
        if (access(candidates[i], F_OK) != 0) {
            continue;
        }

        crossos_optical_device_t *dev = &devices[*out_count];
        memset(dev, 0, sizeof(*dev));
        snprintf(dev->id, sizeof(dev->id), "%s", candidates[i]);
        snprintf(dev->label, sizeof(dev->label), "Optical device %s", candidates[i]);
        dev->can_read = 1;
#if defined(__ANDROID__)
        dev->can_write = 0;
#else
        dev->can_write = 1;
#endif
        dev->has_media = 0;

        (*out_count)++;
    }
#endif

    return CROSSOS_OK;
}

crossos_result_t crossos_optical_burn_start_simulated(const char *const *paths,
                                                      size_t path_count,
                                                      const char *target_device_id,
                                                      crossos_optical_burn_job_t **out_job)
{
    if (!paths || path_count == 0 || !out_job) {
        crossos__set_error("optical_burn_start_simulated: invalid parameter");
        return CROSSOS_ERR_PARAM;
    }

    uint64_t total = 0;
    for (size_t i = 0; i < path_count; i++) {
        if (!paths[i] || paths[i][0] == '\0') {
            continue;
        }
        total += crossos__path_size_recursive(paths[i]);
    }

    if (total == 0) {
        total = 8ULL * 1024ULL * 1024ULL;
    }

    crossos_optical_burn_job_t *job = (crossos_optical_burn_job_t *)calloc(1, sizeof(*job));
    if (!job) {
        crossos__set_error("optical_burn_start_simulated: out of memory");
        return CROSSOS_ERR_OOM;
    }

    if (target_device_id) {
        snprintf(job->target_device_id, sizeof(job->target_device_id), "%s", target_device_id);
    }

    job->total_bytes = total;
    job->speed_mib_s = 8.0f;
    job->started_ms = crossos__now_ms();

    *out_job = job;
    return CROSSOS_OK;
}

crossos_result_t crossos_optical_burn_poll(crossos_optical_burn_job_t *job,
                                           crossos_optical_burn_progress_t *out_progress)
{
    if (!job || !out_progress) {
        crossos__set_error("optical_burn_poll: invalid parameter");
        return CROSSOS_ERR_PARAM;
    }

    memset(out_progress, 0, sizeof(*out_progress));
    out_progress->total_bytes = job->total_bytes;
    out_progress->speed_mib_s = job->speed_mib_s;

    if (job->canceled) {
        out_progress->state = CROSSOS_OPTICAL_BURN_CANCELED;
        out_progress->bytes_written = job->bytes_written;
        out_progress->percent = job->total_bytes > 0
            ? ((float)job->bytes_written * 100.0f / (float)job->total_bytes)
            : 0.0f;
        snprintf(out_progress->message, sizeof(out_progress->message), "Canceled by user");
        return CROSSOS_OK;
    }

    if (job->failed) {
        out_progress->state = CROSSOS_OPTICAL_BURN_FAILED;
        out_progress->bytes_written = job->bytes_written;
        snprintf(out_progress->message, sizeof(out_progress->message), "Burn failed");
        return CROSSOS_OK;
    }

    uint64_t now_ms = crossos__now_ms();
    uint64_t elapsed_ms = (now_ms > job->started_ms) ? (now_ms - job->started_ms) : 0;

    const uint64_t prepare_ms = 900;
    const uint64_t finalize_ms = 700;
    uint64_t burn_elapsed_ms = 0;

    if (elapsed_ms < prepare_ms) {
        out_progress->state = CROSSOS_OPTICAL_BURN_PREPARING;
        snprintf(out_progress->message, sizeof(out_progress->message), "Preparing session");
        out_progress->bytes_written = 0;
        out_progress->percent = 0.0f;
        return CROSSOS_OK;
    }

    burn_elapsed_ms = elapsed_ms - prepare_ms;
    double bytes_per_sec = (double)job->speed_mib_s * 1024.0 * 1024.0;
    uint64_t written = (uint64_t)(bytes_per_sec * ((double)burn_elapsed_ms / 1000.0));
    if (written > job->total_bytes) {
        written = job->total_bytes;
    }
    job->bytes_written = written;

    if (job->bytes_written < job->total_bytes) {
        out_progress->state = CROSSOS_OPTICAL_BURN_BURNING;
        out_progress->bytes_written = job->bytes_written;
        out_progress->percent = (float)job->bytes_written * 100.0f / (float)job->total_bytes;
        snprintf(out_progress->message, sizeof(out_progress->message), "Burning track");
        return CROSSOS_OK;
    }

    uint64_t finished_burn_at = prepare_ms + (uint64_t)((double)job->total_bytes / bytes_per_sec * 1000.0);
    if (elapsed_ms < finished_burn_at + finalize_ms) {
        out_progress->state = CROSSOS_OPTICAL_BURN_FINALIZING;
        out_progress->bytes_written = job->total_bytes;
        out_progress->percent = 100.0f;
        snprintf(out_progress->message, sizeof(out_progress->message), "Finalizing disc");
        return CROSSOS_OK;
    }

    job->done = 1;
    out_progress->state = CROSSOS_OPTICAL_BURN_DONE;
    out_progress->bytes_written = job->total_bytes;
    out_progress->percent = 100.0f;
    snprintf(out_progress->message, sizeof(out_progress->message), "Completed");
    return CROSSOS_OK;
}

crossos_result_t crossos_optical_burn_cancel(crossos_optical_burn_job_t *job)
{
    if (!job) {
        crossos__set_error("optical_burn_cancel: invalid parameter");
        return CROSSOS_ERR_PARAM;
    }

    job->canceled = 1;
    return CROSSOS_OK;
}

void crossos_optical_burn_free(crossos_optical_burn_job_t *job)
{
    free(job);
}
