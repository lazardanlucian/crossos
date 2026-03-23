#include <crossos/crossos.h>

#include "iso_image.h"

#include <android/log.h>
#include <android_native_app_glue.h>
#include <jni.h>

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG_TAG "CrossOSBurner"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define MAX_FILES 256
#define MAX_QUEUE 64
#define MAX_DEVICES 16

extern void crossos_android_set_app(struct android_app *app);
extern int  crossos_android_poll_usb_changed(void);

typedef struct file_entry {
    char name[256];
    char path[1024];
    int is_dir;
    uint64_t size;
} file_entry_t;

typedef struct app_state {
    struct android_app *android_app;
    crossos_window_t *window;
    crossos_surface_t *surface;
    int running;

    char cwd[1024];
    file_entry_t files[MAX_FILES];
    size_t file_count;
    size_t selected_index;

    char queue[MAX_QUEUE][1024];
    size_t queue_count;

    crossos_optical_device_t devices[MAX_DEVICES];
    size_t device_count;
    size_t selected_device;

    crossos_optical_burn_job_t *burn_job;
    crossos_optical_burn_progress_t burn;
    char status[256];
    char burn_source_path[1024];
    uint64_t burn_source_size;
    int burn_source_is_temp;

    crossos_ui_input_t ui_input;
    crossos_ui_text_buf_t search_buf;
    int search_focus_prev;
    int search_dialog_open;
    crossos_ui_scroll_t file_scroll;
    crossos_ui_scroll_t queue_scroll;
    int show_device_dropdown;
    int dragging_over;
} app_state_t;

static app_state_t *g_active_app = NULL;
static char g_pending_filter[CROSSOS_UI_TEXT_MAX];
static volatile int g_pending_filter_set = 0;

static int ci_cmp(const char *a, const char *b);
static void open_selected(app_state_t *app);

static void set_status(app_state_t *app, const char *msg)
{
    snprintf(app->status, sizeof(app->status), "%s", msg ? msg : "");
}

static void format_bytes(uint64_t bytes, char *out, size_t out_size)
{
    const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = (double)bytes;
    size_t unit = 0;

    while (value >= 1024.0 && unit + 1 < sizeof(units) / sizeof(units[0])) {
        value /= 1024.0;
        unit++;
    }

    snprintf(out, out_size, "%.1f %s", value, units[unit]);
}

static int has_disc_image_ext(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) {
        return 0;
    }
    return ci_cmp(ext, ".iso") == 0 || ci_cmp(ext, ".img") == 0 || ci_cmp(ext, ".bin") == 0;
}

static int stat_path_size(const char *path, uint64_t *out_size)
{
    struct stat st;
    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) {
        return 0;
    }
    *out_size = (uint64_t)st.st_size;
    return 1;
}

static void clear_burn_source(app_state_t *app)
{
    if (app->burn_source_is_temp) {
        disc_burner_delete_temp_image(app->burn_source_path);
    }
    app->burn_source_path[0] = '\0';
    app->burn_source_size = 0;
    app->burn_source_is_temp = 0;
}

static int prepare_burn_source(app_state_t *app)
{
    if (app->queue_count == 1 && has_disc_image_ext(app->queue[0])) {
        if (!stat_path_size(app->queue[0], &app->burn_source_size)) {
            set_status(app, "Selected image file is unavailable");
            return 0;
        }
        snprintf(app->burn_source_path, sizeof(app->burn_source_path), "%s", app->queue[0]);
        app->burn_source_is_temp = 0;
        return 1;
    }

    {
        const char *paths[MAX_QUEUE];
        crossos_result_t rc;

        for (size_t i = 0; i < app->queue_count; i++) {
            paths[i] = app->queue[i];
        }

        rc = disc_burner_create_iso_image(paths,
                                          app->queue_count,
                                          app->burn_source_path,
                                          sizeof(app->burn_source_path),
                                          &app->burn_source_size);
        if (rc != CROSSOS_OK) {
            set_status(app, crossos_get_error());
            return 0;
        }
        app->burn_source_is_temp = 1;
    }

    return 1;
}

static int preflight_selected_device(app_state_t *app)
{
    const crossos_optical_device_t *dev;

    if (app->device_count == 0) {
        set_status(app, "No optical drive available");
        clear_burn_source(app);
        return 0;
    }

    if (app->selected_device >= app->device_count) {
        set_status(app, "Selected optical drive is invalid");
        clear_burn_source(app);
        return 0;
    }

    dev = &app->devices[app->selected_device];
    if (!dev->can_write) {
        set_status(app, "Selected drive does not report write support");
        clear_burn_source(app);
        return 0;
    }

    if (!dev->has_media) {
        set_status(app, "Insert writable media before starting the burn");
        clear_burn_source(app);
        return 0;
    }

    if (app->burn_source_size > 0 && dev->media_free_bytes > 0 && app->burn_source_size > dev->media_free_bytes) {
        char needed[32];
        char available[32];
        char message[160];
        format_bytes(app->burn_source_size, needed, sizeof(needed));
        format_bytes(dev->media_free_bytes, available, sizeof(available));
        snprintf(message, sizeof(message), "Queued data needs %s but media has %s free", needed, available);
        set_status(app, message);
        clear_burn_source(app);
        return 0;
    }

    if (app->burn_source_size > 0 && dev->media_free_bytes == 0 && dev->media_capacity_bytes > 0 &&
        app->burn_source_size > dev->media_capacity_bytes) {
        char needed[32];
        char available[32];
        char message[160];
        format_bytes(app->burn_source_size, needed, sizeof(needed));
        format_bytes(dev->media_capacity_bytes, available, sizeof(available));
        snprintf(message, sizeof(message), "Queued data needs %s but media capacity is %s", needed, available);
        set_status(app, message);
        clear_burn_source(app);
        return 0;
    }

    return 1;
}

static void sleep_ms(int ms)
{
    usleep((useconds_t)(ms * 1000));
}

static int ci_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = toupper((unsigned char)*a);
        int cb = toupper((unsigned char)*b);
        if (ca != cb) {
            return ca - cb;
        }
        a++;
        b++;
    }
    return toupper((unsigned char)*a) - toupper((unsigned char)*b);
}

static int file_entry_cmp(const void *a, const void *b)
{
    const file_entry_t *fa = (const file_entry_t *)a;
    const file_entry_t *fb = (const file_entry_t *)b;
    if (fa->is_dir != fb->is_dir) {
        return fb->is_dir - fa->is_dir;
    }
    return ci_cmp(fa->name, fb->name);
}

static int dir_is_readable(const char *path)
{
    DIR *dir;
    if (!path || path[0] == '\0') {
        return 0;
    }
    dir = opendir(path);
    if (!dir) {
        return 0;
    }
    closedir(dir);
    return 1;
}

static int get_env(struct android_app *app, JNIEnv **out_env, int *did_attach)
{
    JavaVM *vm;
    JNIEnv *env;
    jint rc;

    if (did_attach) {
        *did_attach = 0;
    }
    if (!app || !app->activity || !app->activity->vm || !out_env) {
        return 0;
    }

    vm = app->activity->vm;
    env = NULL;
    rc = (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6);
    if (rc == JNI_OK) {
        *out_env = env;
        return 1;
    }
    if (rc != JNI_EDETACHED) {
        return 0;
    }
    if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) {
        return 0;
    }
    if (did_attach) {
        *did_attach = 1;
    }
    *out_env = env;
    return 1;
}

static void detach_env(struct android_app *app, int did_attach)
{
    if (did_attach && app && app->activity && app->activity->vm) {
        (*app->activity->vm)->DetachCurrentThread(app->activity->vm);
    }
}

static int get_external_files_dir(struct android_app *app, char *out_path, size_t out_path_size)
{
    JNIEnv *env;
    int did_attach = 0;
    jobject activity;
    jclass activity_class;
    jclass file_class;
    jmethodID mid_get_external;
    jmethodID mid_get_absolute_path;
    jobject file_obj;
    jstring path_obj;
    const char *path_utf;

    if (!get_env(app, &env, &did_attach)) {
        return 0;
    }

    activity = app->activity->clazz;
    activity_class = (*env)->GetObjectClass(env, activity);
    if (!activity_class) {
        detach_env(app, did_attach);
        return 0;
    }

    mid_get_external = (*env)->GetMethodID(env,
                                           activity_class,
                                           "getExternalFilesDir",
                                           "(Ljava/lang/String;)Ljava/io/File;");
    if (!mid_get_external) {
        (*env)->DeleteLocalRef(env, activity_class);
        (*env)->ExceptionClear(env);
        detach_env(app, did_attach);
        return 0;
    }

    file_obj = (*env)->CallObjectMethod(env, activity, mid_get_external, NULL);
    (*env)->DeleteLocalRef(env, activity_class);
    if ((*env)->ExceptionCheck(env) || !file_obj) {
        (*env)->ExceptionClear(env);
        detach_env(app, did_attach);
        return 0;
    }

    file_class = (*env)->GetObjectClass(env, file_obj);
    mid_get_absolute_path = (*env)->GetMethodID(env,
                                                file_class,
                                                "getAbsolutePath",
                                                "()Ljava/lang/String;");
    (*env)->DeleteLocalRef(env, file_class);
    if (!mid_get_absolute_path) {
        (*env)->DeleteLocalRef(env, file_obj);
        (*env)->ExceptionClear(env);
        detach_env(app, did_attach);
        return 0;
    }

    path_obj = (jstring)(*env)->CallObjectMethod(env, file_obj, mid_get_absolute_path);
    (*env)->DeleteLocalRef(env, file_obj);
    if ((*env)->ExceptionCheck(env) || !path_obj) {
        (*env)->ExceptionClear(env);
        detach_env(app, did_attach);
        return 0;
    }

    path_utf = (*env)->GetStringUTFChars(env, path_obj, NULL);
    if (!path_utf) {
        (*env)->DeleteLocalRef(env, path_obj);
        detach_env(app, did_attach);
        return 0;
    }

    snprintf(out_path, out_path_size, "%s", path_utf);
    (*env)->ReleaseStringUTFChars(env, path_obj, path_utf);
    (*env)->DeleteLocalRef(env, path_obj);
    detach_env(app, did_attach);
    return 1;
}

static void request_filter_dialog(app_state_t *app)
{
    JNIEnv *env;
    int did_attach = 0;
    jobject activity;
    jclass activity_class;
    jmethodID mid_show_filter;
    jstring initial;

    if (!app || !app->android_app || app->search_dialog_open) {
        return;
    }
    if (!get_env(app->android_app, &env, &did_attach)) {
        return;
    }

    activity = app->android_app->activity->clazz;
    activity_class = (*env)->GetObjectClass(env, activity);
    if (!activity_class) {
        detach_env(app->android_app, did_attach);
        return;
    }

    mid_show_filter = (*env)->GetMethodID(env,
                                          activity_class,
                                          "showFilterInput",
                                          "(Ljava/lang/String;)V");
    if (!mid_show_filter) {
        (*env)->DeleteLocalRef(env, activity_class);
        (*env)->ExceptionClear(env);
        detach_env(app->android_app, did_attach);
        return;
    }

    initial = (*env)->NewStringUTF(env, app->search_buf.buf);
    (*env)->CallVoidMethod(env, activity, mid_show_filter, initial);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    }

    if (initial) {
        (*env)->DeleteLocalRef(env, initial);
    }
    (*env)->DeleteLocalRef(env, activity_class);
    detach_env(app->android_app, did_attach);
    app->search_dialog_open = 1;
}

JNIEXPORT void JNICALL
Java_io_crossos_hello_CrossOSNativeActivity_nativeSetFilterText(JNIEnv *env,
                                                                 jclass clazz,
                                                                 jstring text)
{
    const char *utf = NULL;
    (void)clazz;

    if (!g_active_app) {
        return;
    }

    if (!text) {
        g_active_app->search_dialog_open = 0;
        return;
    }

    utf = (*env)->GetStringUTFChars(env, text, NULL);
    if (!utf) {
        g_active_app->search_dialog_open = 0;
        return;
    }

    snprintf(g_pending_filter, sizeof(g_pending_filter), "%s", utf);
    g_pending_filter_set = 1;

    (*env)->ReleaseStringUTFChars(env, text, utf);
    g_active_app->search_dialog_open = 0;
}

static void init_start_directory(struct android_app *android_app, app_state_t *app)
{
    char external_dir[1024];
    const char *candidates[6];
    size_t candidate_count = 0;

    external_dir[0] = '\0';
    if (get_external_files_dir(android_app, external_dir, sizeof(external_dir))) {
        candidates[candidate_count++] = external_dir;
    }
    candidates[candidate_count++] = "/storage/emulated/0/Download";
    candidates[candidate_count++] = "/sdcard/Download";
    candidates[candidate_count++] = "/storage/emulated/0";
    candidates[candidate_count++] = "/sdcard";
    candidates[candidate_count++] = "/";

    for (size_t i = 0; i < candidate_count; i++) {
        if (dir_is_readable(candidates[i])) {
            snprintf(app->cwd, sizeof(app->cwd), "%s", candidates[i]);
            return;
        }
    }

    snprintf(app->cwd, sizeof(app->cwd), "/");
}

static void refresh_files(app_state_t *app)
{
    app->file_count = 0;
    app->selected_index = 0;

    DIR *dir = opendir(app->cwd);
    if (!dir) {
        set_status(app, "Cannot open folder");
        return;
    }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL && app->file_count < MAX_FILES) {
        file_entry_t *entry;
        struct stat st;

        if (strcmp(de->d_name, ".") == 0) {
            continue;
        }

        entry = &app->files[app->file_count];
        memset(entry, 0, sizeof(*entry));
        snprintf(entry->name, sizeof(entry->name), "%s", de->d_name);
        if (strcmp(app->cwd, "/") == 0) {
            snprintf(entry->path, sizeof(entry->path), "/%s", de->d_name);
        } else {
            snprintf(entry->path, sizeof(entry->path), "%s/%s", app->cwd, de->d_name);
        }

        if (stat(entry->path, &st) == 0) {
            entry->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
            entry->size = (uint64_t)st.st_size;
        }

        app->file_count++;
    }

    closedir(dir);
    qsort(app->files, app->file_count, sizeof(app->files[0]), file_entry_cmp);
}

static void queue_remove_at(app_state_t *app, size_t index)
{
    if (index >= app->queue_count) {
        return;
    }
    for (size_t i = index; i + 1 < app->queue_count; i++) {
        snprintf(app->queue[i], sizeof(app->queue[i]), "%s", app->queue[i + 1]);
    }
    app->queue_count--;
}

static void refresh_devices(app_state_t *app)
{
    size_t count = 0;
    crossos_result_t rc = crossos_optical_list_devices(app->devices, MAX_DEVICES, &count);
    if (rc != CROSSOS_OK) {
        app->device_count = 0;
        set_status(app, "Device scan failed");
        return;
    }

    app->device_count = count;
    if (app->selected_device >= app->device_count && app->device_count > 0) {
        app->selected_device = 0;
    }
}

static void add_selected_to_queue(app_state_t *app)
{
    const file_entry_t *entry;

    if (app->file_count == 0 || app->queue_count >= MAX_QUEUE) {
        set_status(app, "Queue full or empty folder");
        return;
    }

    entry = &app->files[app->selected_index];
    for (size_t i = 0; i < app->queue_count; i++) {
        if (strcmp(app->queue[i], entry->path) == 0) {
            set_status(app, "Already in queue");
            return;
        }
    }

    snprintf(app->queue[app->queue_count], sizeof(app->queue[0]), "%s", entry->path);
    app->queue_count++;
    set_status(app, "Added to burn queue");
}

static void add_path_to_queue(app_state_t *app, const char *path)
{
    if (!path || path[0] == '\0') {
        return;
    }

    if (app->queue_count >= MAX_QUEUE) {
        set_status(app, "Queue full");
        return;
    }

    for (size_t i = 0; i < app->queue_count; i++) {
        if (strcmp(app->queue[i], path) == 0) {
            return;
        }
    }

    snprintf(app->queue[app->queue_count], sizeof(app->queue[0]), "%s", path);
    app->queue_count++;
}

static void pick_files_to_queue(app_state_t *app)
{
    crossos_dialog_file_list_t files;
    crossos_result_t rc;

    memset(&files, 0, sizeof(files));
    rc = crossos_dialog_pick_files("Select files to burn", 1, &files);
    if (rc != CROSSOS_OK) {
        if (rc == CROSSOS_ERR_UNSUPPORT) {
            if (app->file_count > 0) {
                const file_entry_t *entry = &app->files[app->selected_index];
                if (entry->is_dir) {
                    open_selected(app);
                    set_status(app, "Opened selected folder");
                } else {
                    add_selected_to_queue(app);
                }
            } else {
                set_status(app, "Picker unavailable. Browse storage and tap Add instead");
            }
        } else {
            set_status(app, crossos_get_error());
        }
        return;
    }

    for (size_t i = 0; i < files.count; i++) {
        add_path_to_queue(app, files.items[i]);
    }

    char msg[96];
    snprintf(msg, sizeof(msg), "Added %zu file(s) from picker", files.count);
    set_status(app, msg);
    crossos_dialog_file_list_free(&files);
}

static void go_parent(app_state_t *app)
{
    size_t len = strlen(app->cwd);
    if (len == 0) {
        return;
    }

    while (len > 1 && app->cwd[len - 1] == '/') {
        app->cwd[--len] = '\0';
    }
    while (len > 1 && app->cwd[len - 1] != '/') {
        app->cwd[--len] = '\0';
    }
    if (len > 1) {
        app->cwd[len - 1] = '\0';
    }
    if (app->cwd[0] == '\0') {
        snprintf(app->cwd, sizeof(app->cwd), "/");
    }

    refresh_files(app);
}

static void open_selected(app_state_t *app)
{
    const file_entry_t *entry;

    if (app->file_count == 0) {
        return;
    }

    entry = &app->files[app->selected_index];
    if (!entry->is_dir) {
        return;
    }
    if (strcmp(entry->name, "..") == 0) {
        go_parent(app);
        return;
    }

    snprintf(app->cwd, sizeof(app->cwd), "%s", entry->path);
    refresh_files(app);
}

static void start_burn(app_state_t *app)
{
    const char *paths[1];
    const char *dev;
    crossos_result_t rc;

    if (app->burn_job) {
        set_status(app, "Burn already running");
        return;
    }
    if (app->queue_count == 0) {
        set_status(app, "Queue is empty");
        return;
    }

    clear_burn_source(app);
    if (!prepare_burn_source(app)) {
        return;
    }
    if (!preflight_selected_device(app)) {
        return;
    }

    paths[0] = app->burn_source_path;
    dev = app->devices[app->selected_device].id;
    rc = crossos_optical_burn_start(paths, 1, dev, &app->burn_job);
    if (rc != CROSSOS_OK) {
        clear_burn_source(app);
        set_status(app, crossos_get_error());
        return;
    }

    memset(&app->burn, 0, sizeof(app->burn));
    set_status(app, app->burn_source_is_temp ? "Prepared temporary ISO and started burn" : "Burn started");
}

static void update_burn(app_state_t *app)
{
    if (!app->burn_job) {
        return;
    }

    if (crossos_optical_burn_poll(app->burn_job, &app->burn) != CROSSOS_OK) {
        set_status(app, crossos_get_error());
        return;
    }

    if (app->burn.state == CROSSOS_OPTICAL_BURN_DONE ||
        app->burn.state == CROSSOS_OPTICAL_BURN_FAILED ||
        app->burn.state == CROSSOS_OPTICAL_BURN_CANCELED) {
        if (app->burn.message[0] != '\0') {
            set_status(app, app->burn.message);
        } else if (app->burn.state == CROSSOS_OPTICAL_BURN_DONE) {
            set_status(app, "Burn completed");
        } else if (app->burn.state == CROSSOS_OPTICAL_BURN_CANCELED) {
            set_status(app, "Burn canceled");
        } else {
            set_status(app, "Burn failed");
        }
        crossos_optical_burn_free(app->burn_job);
        app->burn_job = NULL;
        clear_burn_source(app);
    }
}

static const char *burn_state_name(crossos_optical_burn_state_t state)
{
    switch (state) {
    case CROSSOS_OPTICAL_BURN_IDLE: return "IDLE";
    case CROSSOS_OPTICAL_BURN_PREPARING: return "PREPARING";
    case CROSSOS_OPTICAL_BURN_BURNING: return "BURNING";
    case CROSSOS_OPTICAL_BURN_FINALIZING: return "FINALIZING";
    case CROSSOS_OPTICAL_BURN_DONE: return "DONE";
    case CROSSOS_OPTICAL_BURN_FAILED: return "FAILED";
    case CROSSOS_OPTICAL_BURN_CANCELED: return "CANCELED";
    default: return "UNKNOWN";
    }
}

static void render(app_state_t *app)
{
    crossos_framebuffer_t fb;
    crossos_ui_context_t ui;
    int scale;
    int pad;
    int width;
    int height;
    int header_h;
    int footer_h;
    int body_y;
    int body_h;
    int left_w;
    int right_w;
    int left_x;
    int right_x;
    int search_h;
    int search_y;
    int list_y;
    int list_h;
    int row_h;
    int visible[MAX_FILES];
    int vis_count = 0;
    int drop_h;
    int q_scroll_h;
    int q_scroll_y;
    int drop_y;
    int q_row_h;
    int q_cont_h;
    int footer_y;
    int dev_x;
    int dev_y;
    int dev_w;
    int prog_x;
    int prog_w;
    int btn_y;
    int btn_h;
    char line[256];
    char hdr[96];
    const char *filter;
    int search_focused;

    if (!app->surface || crossos_surface_lock(app->surface, &fb) != CROSSOS_OK) {
        return;
    }

    crossos_ui_begin(&ui, &fb, &app->ui_input);
    crossos_ui_set_theme(&ui, crossos_ui_theme_dark());

    scale = ui.scale;
    pad = 12 * scale;
    width = fb.width;
    height = fb.height;
    header_h = 44 * scale;
    footer_h = 72 * scale;
    body_y = pad + header_h + pad;
    body_h = height - body_y - footer_h - pad;
    left_w = (width - pad * 3) * 2 / 5;
    right_w = width - pad * 3 - left_w;
    left_x = pad;
    right_x = left_x + left_w + pad;

    crossos_draw_clear(&fb, ui.theme.bg);
    crossos_ui_panel(&ui, 0, 0, width, body_y - pad / 2, ui.theme.surface);
    crossos_ui_separator(&ui, 0, body_y - pad / 2, width);

    crossos_ui_label(&ui, pad + 4 * scale, pad + 4 * scale, "CrossOS Disc Burn Studio", ui.theme.text);
    snprintf(hdr, sizeof(hdr), "%zu file(s) queued  |  %.48s", app->queue_count, app->cwd);
    crossos_ui_label(&ui, pad + 4 * scale, pad + 18 * scale, hdr, ui.theme.text_dim);

    if (app->burn_job) {
        crossos_ui_spinner(&ui, width - pad - 8 * scale, pad + 10 * scale, 8 * scale);
    }

    search_h = 18 * scale;
    search_y = body_y;
    list_y = search_y + search_h + 4 * scale;
    list_h = body_h - search_h - 4 * scale;
    row_h = 16 * scale;

    search_focused = crossos_ui_text_input(&ui,
                                           left_x,
                                           search_y,
                                           left_w,
                                           search_h,
                                           &app->search_buf,
                                           "Filter files...");
    if (search_focused && !app->search_focus_prev && !app->search_dialog_open) {
        request_filter_dialog(app);
    }
    app->search_focus_prev = search_focused;

    filter = app->search_buf.buf;
    for (size_t fi = 0; fi < app->file_count; fi++) {
        if (filter[0] == '\0') {
            visible[vis_count++] = (int)fi;
            continue;
        }
        const char *name = app->files[fi].name;
        int found = 0;
        for (const char *p = name; *p && !found; p++) {
            int match = 1;
            for (int k = 0; filter[k]; k++) {
                if (!*(p + k) || toupper((unsigned char)*(p + k)) != toupper((unsigned char)filter[k])) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                found = 1;
            }
        }
        if (found) {
            visible[vis_count++] = (int)fi;
        }
    }

    {
        int content_h = vis_count * row_h;
        int scroll_off;

        crossos_ui_scroll_begin(&ui, left_x, list_y, left_w, list_h, &app->file_scroll,
                                content_h > list_h ? content_h : list_h);
        scroll_off = (int)app->file_scroll.offset;
        for (int vi = 0; vi < vis_count; vi++) {
            int fi = visible[vi];
            const file_entry_t *entry = &app->files[fi];
            int row_y = list_y + vi * row_h - scroll_off;
            if (row_y + row_h < list_y || row_y > list_y + list_h) {
                continue;
            }
            snprintf(line, sizeof(line), "%s  %.40s", entry->is_dir ? "/" : " ", entry->name);
            if (crossos_ui_selectable(&ui, left_x, row_y, left_w - 10 * scale, row_h - 1,
                                      line, (size_t)fi == app->selected_index)) {
                app->selected_index = (size_t)fi;
                if (entry->is_dir) {
                    open_selected(app);
                } else {
                    add_selected_to_queue(app);
                }
            }
        }
        if (vis_count == 0) {
            crossos_ui_label_centered(&ui, left_x, list_y, left_w, list_h,
                                      filter[0] ? "No matches" : "Empty folder",
                                      ui.theme.text_dim);
        }
        crossos_ui_scroll_end(&ui, left_x, list_y, left_w, list_h, &app->file_scroll);
    }

    drop_h = 44 * scale;
    q_scroll_h = body_h - drop_h - 4 * scale;
    q_scroll_y = body_y;
    drop_y = body_y + q_scroll_h + 4 * scale;

    if (crossos_ui_drop_zone(&ui, right_x, drop_y, right_w, drop_h,
                             "Drop files here or tap to browse", app->dragging_over)) {
        pick_files_to_queue(app);
    }

    q_row_h = 18 * scale;
    q_cont_h = (int)app->queue_count * q_row_h;
    crossos_ui_scroll_begin(&ui, right_x, q_scroll_y, right_w, q_scroll_h,
                            &app->queue_scroll, q_cont_h > q_scroll_h ? q_cont_h : q_scroll_h);
    {
        int q_off = (int)app->queue_scroll.offset;
        for (size_t qi = 0; qi < app->queue_count; qi++) {
            int row_y = q_scroll_y + (int)qi * q_row_h - q_off;
            const char *base;
            if (row_y + q_row_h < q_scroll_y || row_y > q_scroll_y + q_scroll_h) {
                continue;
            }
            base = strrchr(app->queue[qi], '/');
            base = base ? base + 1 : app->queue[qi];
            snprintf(line, sizeof(line), "%02zu  %s", qi + 1, base);
            if (crossos_ui_selectable(&ui, right_x, row_y, right_w - 10 * scale, q_row_h - 1, line, 0)) {
                queue_remove_at(app, qi);
                set_status(app, "Removed from queue");
            }
        }
        if (app->queue_count == 0) {
            crossos_ui_label_centered(&ui, right_x, q_scroll_y, right_w, q_scroll_h,
                                      "Queue is empty - add files from browser or picker",
                                      ui.theme.text_dim);
        }
    }
    crossos_ui_scroll_end(&ui, right_x, q_scroll_y, right_w, q_scroll_h, &app->queue_scroll);

    footer_y = height - footer_h;
    crossos_ui_panel(&ui, 0, footer_y, width, footer_h, ui.theme.surface);
    crossos_ui_separator(&ui, 0, footer_y, width);

    dev_x = pad;
    dev_y = footer_y + 6 * scale;
    dev_w = 200 * scale;

    if (crossos_ui_dropdown_header(&ui, dev_x, dev_y, dev_w, 16 * scale, "Drive", app->show_device_dropdown)) {
        app->show_device_dropdown = !app->show_device_dropdown;
    }
    if (app->show_device_dropdown) {
        for (size_t di = 0; di < app->device_count && di < 4; di++) {
            int item_y = dev_y + 18 * scale + (int)di * 15 * scale;
            snprintf(line, sizeof(line), "%.36s", app->devices[di].label);
            if (crossos_ui_selectable(&ui, dev_x, item_y, dev_w, 14 * scale,
                                      line, di == app->selected_device)) {
                app->selected_device = di;
                app->show_device_dropdown = 0;
            }
        }
        if (app->device_count == 0) {
            crossos_ui_label(&ui, dev_x + 4, dev_y + 18 * scale, "No optical drive found", ui.theme.text_dim);
        }
    } else {
        snprintf(line, sizeof(line), "%.36s",
                 app->device_count > 0 ? app->devices[app->selected_device].label : "No drive found");
        crossos_ui_label(&ui, dev_x, dev_y + 18 * scale, line, ui.theme.text_dim);
    }

    prog_x = dev_x + dev_w + pad;
    prog_w = width / 2 - dev_w - pad * 2;
    snprintf(line, sizeof(line), "%s  %.1f%%  %.1f MiB/s",
             burn_state_name(app->burn.state), app->burn.percent, app->burn.speed_mib_s);
    crossos_ui_label(&ui, prog_x, dev_y, line, ui.theme.text);
    crossos_ui_progress_bar(&ui, prog_x, dev_y + 14 * scale, prog_w, 10 * scale, app->burn.percent, "");

    btn_y = footer_y + 40 * scale;
    btn_h = 18 * scale;
    {
        crossos_ui_layout_t row;
        crossos_rect_t rect;

        crossos_ui_layout_begin_row(&row, pad, btn_y, width - pad * 2, btn_h, 6 * scale);

        if (crossos_ui_layout_row_next(&row, 58 * scale, &rect)) {
            if (crossos_ui_button(&ui, rect.x, rect.y, rect.width, rect.height, "Open",
                                  app->file_count > 0 && app->files[app->selected_index].is_dir)) {
                open_selected(app);
            }
        }
        if (crossos_ui_layout_row_next(&row, 48 * scale, &rect)) {
            if (crossos_ui_button(&ui, rect.x, rect.y, rect.width, rect.height, "Add", app->file_count > 0)) {
                add_selected_to_queue(app);
            }
        }
        if (crossos_ui_layout_row_next(&row, 48 * scale, &rect)) {
            if (crossos_ui_button_ghost(&ui, rect.x, rect.y, rect.width, rect.height, "Pick", 1)) {
                pick_files_to_queue(app);
            }
        }
        if (crossos_ui_layout_row_next(&row, 40 * scale, &rect)) {
            if (crossos_ui_button_ghost(&ui, rect.x, rect.y, rect.width, rect.height, "Up", 1)) {
                go_parent(app);
            }
        }
        if (crossos_ui_layout_row_next(&row, 66 * scale, &rect)) {
            if (crossos_ui_button_ghost(&ui, rect.x, rect.y, rect.width, rect.height, "Refresh", 1)) {
                refresh_files(app);
                refresh_devices(app);
                set_status(app, "Refreshed");
            }
        }
        if (crossos_ui_layout_row_next(&row, 60 * scale, &rect)) {
            if (crossos_ui_button(&ui, rect.x, rect.y, rect.width, rect.height, "Burn",
                                  app->queue_count > 0 && !app->burn_job)) {
                start_burn(app);
            }
        }
        if (crossos_ui_layout_row_next(&row, 66 * scale, &rect)) {
            if (crossos_ui_button_danger(&ui, rect.x, rect.y, rect.width, rect.height, "Cancel",
                                         app->burn_job != NULL)) {
                if (app->burn_job) {
                    crossos_optical_burn_cancel(app->burn_job);
                    set_status(app, "Cancel requested");
                }
            }
        }
    }

    crossos_ui_label(&ui, pad, btn_y + btn_h + 4 * scale, app->status, ui.theme.text_dim);

    crossos_surface_unlock(app->surface);
    crossos_surface_present(app->surface);
}

static void handle_event(app_state_t *app, const crossos_event_t *ev)
{
    if (ev->type == CROSSOS_EVENT_QUIT || ev->type == CROSSOS_EVENT_WINDOW_CLOSE) {
        app->running = 0;
        crossos_quit();
        return;
    }

    if (ev->type == CROSSOS_EVENT_KEY_DOWN) {
        int key = ev->key.keycode;
        app->ui_input.key_pressed = key;
        app->ui_input.key_mods = (int)ev->key.mods;
        if (app->search_buf.focused) {
            return;
        }
        if (key == CROSSOS_KEY_ESCAPE) {
            app->running = 0;
            crossos_quit();
        } else if (key == CROSSOS_KEY_UP) {
            if (app->selected_index > 0) {
                app->selected_index--;
            }
        } else if (key == CROSSOS_KEY_DOWN) {
            if (app->selected_index + 1 < app->file_count) {
                app->selected_index++;
            }
        } else if (key == CROSSOS_KEY_ENTER) {
            open_selected(app);
        } else if (key == CROSSOS_KEY_SPACE) {
            add_selected_to_queue(app);
        } else if (key == CROSSOS_KEY_BACKSPACE) {
            go_parent(app);
        } else if (key == CROSSOS_KEY_TAB) {
            if (app->device_count > 0) {
                app->selected_device = (app->selected_device + 1) % app->device_count;
            }
        } else if (key == CROSSOS_KEY_B) {
            start_burn(app);
        } else if (key == CROSSOS_KEY_C) {
            if (app->burn_job) {
                crossos_optical_burn_cancel(app->burn_job);
                set_status(app, "Cancel requested");
            }
        } else if (key == CROSSOS_KEY_R) {
            refresh_files(app);
            refresh_devices(app);
            set_status(app, "Refreshed");
        }
    } else if (ev->type == CROSSOS_EVENT_POINTER_MOVE) {
        app->ui_input.pointer_x = (int)ev->pointer.x;
        app->ui_input.pointer_y = (int)ev->pointer.y;
    } else if (ev->type == CROSSOS_EVENT_POINTER_DOWN) {
        app->ui_input.pointer_x = (int)ev->pointer.x;
        app->ui_input.pointer_y = (int)ev->pointer.y;
        app->ui_input.pointer_down = 1;
        app->ui_input.pointer_pressed = 1;
    } else if (ev->type == CROSSOS_EVENT_POINTER_UP) {
        app->ui_input.pointer_x = (int)ev->pointer.x;
        app->ui_input.pointer_y = (int)ev->pointer.y;
        app->ui_input.pointer_down = 0;
        app->ui_input.pointer_released = 1;
    } else if (ev->type == CROSSOS_EVENT_POINTER_SCROLL) {
        app->ui_input.scroll_dy += ev->pointer.scroll_y;
        app->ui_input.scroll_dx += ev->pointer.scroll_x;
    } else if (ev->type == CROSSOS_EVENT_CHAR) {
        app->ui_input.char_input = ev->character.codepoint;
    } else if (ev->type == CROSSOS_EVENT_DROP_FILES) {
        for (int i = 0; i < ev->drop.count; i++) {
            add_path_to_queue(app, ev->drop.paths[i]);
        }
        if (ev->drop.count > 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Dropped %d file(s)", ev->drop.count);
            set_status(app, msg);
        }
        app->dragging_over = 0;
    }
}

static void reset_frame_input(app_state_t *app)
{
    app->ui_input.pointer_pressed = 0;
    app->ui_input.pointer_released = 0;
    app->ui_input.scroll_dx = 0;
    app->ui_input.scroll_dy = 0;
    app->ui_input.char_input = 0;
    app->ui_input.key_pressed = 0;
    app->ui_input.key_mods = 0;
    app->ui_input.drop_count = 0;
}

static int wait_for_native_window(struct android_app *app)
{
    while (!app->window) {
        int events;
        struct android_poll_source *source = NULL;
        if (ALooper_pollAll(-1, NULL, &events, (void **)&source) >= 0) {
            if (source) {
                source->process(app, source);
            }
        }
        if (app->destroyRequested) {
            LOGE("destroy requested before native window became available");
            return 0;
        }
    }
    return 1;
}

void android_main(struct android_app *android_app)
{
    app_state_t app;

    crossos_android_set_app(android_app);
    if (!wait_for_native_window(android_app)) {
        return;
    }
    if (crossos_init() != CROSSOS_OK) {
        LOGE("crossos_init failed: %s", crossos_get_error());
        return;
    }

    memset(&app, 0, sizeof(app));
    app.android_app = android_app;
    app.running = 1;
    app.burn.state = CROSSOS_OPTICAL_BURN_IDLE;
    init_start_directory(android_app, &app);
    g_active_app = &app;

    app.window = crossos_window_create("CrossOS Disc Burner", 0, 0, 0);
    if (!app.window) {
        LOGE("crossos_window_create failed: %s", crossos_get_error());
        crossos_shutdown();
        return;
    }

    crossos_window_show(app.window);
    app.surface = crossos_surface_get(app.window);

    refresh_files(&app);
    refresh_devices(&app);
    set_status(&app, "Ready. Browse storage, queue files, then start burn.");

    while (app.running) {
        crossos_event_t ev;
        if (g_pending_filter_set) {
            g_pending_filter_set = 0;
            snprintf(app.search_buf.buf, sizeof(app.search_buf.buf), "%s", g_pending_filter);
            app.search_buf.len = (int)strlen(app.search_buf.buf);
            app.search_buf.cursor = app.search_buf.len;
            app.search_buf.focused = 0;
            app.search_focus_prev = 0;
        }
        while (crossos_poll_event(&ev)) {
            handle_event(&app, &ev);
        }
        if (crossos_android_poll_usb_changed()) {
            refresh_devices(&app);
        }
        update_burn(&app);
        render(&app);
        reset_frame_input(&app);
        sleep_ms(16);
    }

    if (app.burn_job) {
        crossos_optical_burn_free(app.burn_job);
        app.burn_job = NULL;
    }
    clear_burn_source(&app);
    g_active_app = NULL;
    crossos_window_destroy(app.window);
    crossos_shutdown();
}
