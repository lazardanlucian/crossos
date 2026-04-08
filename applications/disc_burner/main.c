#include <crossos/crossos.h>

#include "iso_image.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#define MAX_FILES 256
#define MAX_QUEUE 64
#define MAX_DEVICES 16

typedef struct file_entry {
    char name[256];
    char path[1024];
    int is_dir;
    uint64_t size;
} file_entry_t;

typedef struct app_state {
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

    crossos_ui_input_t    ui_input;
    crossos_ui_text_buf_t search_buf;   /* file browser filter */
    crossos_ui_scroll_t   file_scroll;  /* left panel scroll   */
    crossos_ui_scroll_t   queue_scroll; /* right panel scroll  */
    int show_device_dropdown;
    int dragging_over;                  /* Xdnd / WM_DROPFILES hover */
} app_state_t;

static void set_status(app_state_t *app, const char *msg)
{
    snprintf(app->status, sizeof(app->status), "%s", msg ? msg : "");
}

static int ci_cmp(const char *a, const char *b);

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
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    select(0, NULL, NULL, NULL, &tv);
#endif
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

static void refresh_files(app_state_t *app)
{
    app->file_count = 0;
    app->selected_index = 0;

    DIR *d = opendir(app->cwd);
    if (!d) {
        set_status(app, "Cannot open folder");
        return;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL && app->file_count < MAX_FILES) {
        if (strcmp(de->d_name, ".") == 0) {
            continue;
        }

        file_entry_t *e = &app->files[app->file_count];
        memset(e, 0, sizeof(*e));
        snprintf(e->name, sizeof(e->name), "%s", de->d_name);
        if (strcmp(app->cwd, "/") == 0) {
            snprintf(e->path, sizeof(e->path), "/%s", de->d_name);
        } else {
            snprintf(e->path, sizeof(e->path), "%s/%s", app->cwd, de->d_name);
        }

        struct stat st;
        if (stat(e->path, &st) == 0) {
            e->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
            e->size = (uint64_t)st.st_size;
        }

        app->file_count++;
    }

    closedir(d);

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
    if (app->file_count == 0 || app->queue_count >= MAX_QUEUE) {
        set_status(app, "Queue full or empty folder");
        return;
    }

    const file_entry_t *e = &app->files[app->selected_index];
    for (size_t i = 0; i < app->queue_count; i++) {
        if (strcmp(app->queue[i], e->path) == 0) {
            set_status(app, "Already in queue");
            return;
        }
    }

    snprintf(app->queue[app->queue_count], sizeof(app->queue[0]), "%s", e->path);
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
    memset(&files, 0, sizeof(files));

    crossos_result_t rc = crossos_dialog_pick_files("Select files to burn", 1, &files);
    if (rc != CROSSOS_OK) {
        if (rc == CROSSOS_ERR_UNSUPPORT) {
            set_status(app, "File picker not available on this platform yet");
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
    if (app->file_count == 0) {
        return;
    }

    const file_entry_t *e = &app->files[app->selected_index];
    if (!e->is_dir) {
        return;
    }

    if (strcmp(e->name, "..") == 0) {
        go_parent(app);
        return;
    }

    snprintf(app->cwd, sizeof(app->cwd), "%s", e->path);
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

static const char *burn_state_name(crossos_optical_burn_state_t s)
{
    switch (s) {
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
    if (!app->surface || crossos_surface_lock(app->surface, &fb) != CROSSOS_OK) {
        return;
    }

    crossos_ui_context_t ui;
    crossos_ui_begin(&ui, &fb, &app->ui_input);
    crossos_ui_set_theme(&ui, crossos_ui_theme_dark());

    int s       = ui.scale;
    int pad     = 12 * s;
    int W       = fb.width;
    int H       = fb.height;

    /* ── Layout constants ─────────────────────────────────────────────── */
    int header_h = 44 * s;
    int footer_h = 72 * s;
    int body_y   = pad + header_h + pad;
    int body_h   = H - body_y - footer_h - pad;
    int left_w   = (W - pad * 3) * 2 / 5;
    int right_w  = W - pad * 3 - left_w;
    int left_x   = pad;
    int right_x  = left_x + left_w + pad;

    /* ── Background ───────────────────────────────────────────────────── */
    crossos_draw_clear(&fb, ui.theme.bg);

    /* ── Header bar ───────────────────────────────────────────────────── */
    crossos_ui_panel(&ui, 0, 0, W, body_y - pad / 2, ui.theme.surface);
    crossos_ui_separator(&ui, 0, body_y - pad / 2, W);

    crossos_ui_label(&ui, pad + 4 * s, pad + 4 * s, "CrossOS Disc Burn Studio",
                     ui.theme.text);
    char hdr[80];
    snprintf(hdr, sizeof(hdr), "%zu file(s) queued  |  %.40s", app->queue_count, app->cwd);
    crossos_ui_label(&ui, pad + 4 * s, pad + 18 * s, hdr, ui.theme.text_dim);

    if (app->burn_job) {
        crossos_ui_spinner(&ui, W - pad - 8 * s, pad + 10 * s, 8 * s);
    }

    /* ── Left panel: File Browser ─────────────────────────────────────── */
    int search_h  = 18 * s;
    int search_y  = body_y;
    int list_y    = search_y + search_h + 4 * s;
    int list_h    = body_h - search_h - 4 * s;
    int row_h     = 16 * s;

    /* Filter field */
    crossos_ui_text_input(&ui, left_x, search_y, left_w, search_h,
                          &app->search_buf, "Filter files...");

    /* Build filtered index list */
    int visible[MAX_FILES];
    int vis_count = 0;
    const char *filter = app->search_buf.buf;
    for (size_t fi = 0; fi < app->file_count; fi++) {
        if (filter[0] == '\0') {
            visible[vis_count++] = (int)fi;
        } else {
            const char *name = app->files[fi].name;
            int found = 0;
            for (const char *p = name; *p && !found; p++) {
                int match = 1;
                for (int k = 0; filter[k]; k++) {
                    if (!*(p + k) ||
                        toupper((unsigned char)*(p + k)) !=
                        toupper((unsigned char)filter[k])) {
                        match = 0;
                        break;
                    }
                }
                if (match) found = 1;
            }
            if (found) visible[vis_count++] = (int)fi;
        }
    }

    int fc_h = vis_count * row_h;
    crossos_ui_scroll_begin(&ui, left_x, list_y, left_w, list_h,
                            &app->file_scroll, fc_h > list_h ? fc_h : list_h);

    char line[256];
    int sb_off = (int)app->file_scroll.offset;
    for (int vi = 0; vi < vis_count; vi++) {
        int fi = visible[vi];
        const file_entry_t *e = &app->files[fi];
        int ry = list_y + vi * row_h - sb_off;
        if (ry + row_h < list_y || ry > list_y + list_h) continue;
        snprintf(line, sizeof(line), "%s  %.40s",
                 e->is_dir ? "/" : " ", e->name);
        if (crossos_ui_selectable(&ui, left_x, ry, left_w - 10 * s, row_h - 1,
                                  line, (size_t)fi == app->selected_index)) {
            app->selected_index = (size_t)fi;
            if (app->ui_input.pointer_pressed) {
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

    /* ── Right panel: Burn Queue ──────────────────────────────────────── */
    int drop_h     = 44 * s;
    int q_scroll_h = body_h - drop_h - 4 * s;
    int q_scroll_y = body_y;
    int drop_y     = body_y + q_scroll_h + 4 * s;

    /* Drop zone */
    if (crossos_ui_drop_zone(&ui, right_x, drop_y, right_w, drop_h,
                             "Drop files here or tap to browse",
                             app->dragging_over)) {
        pick_files_to_queue(app);
    }

    /* Queue list */
    int q_row_h   = 18 * s;
    int q_cont_h  = (int)app->queue_count * q_row_h;
    crossos_ui_scroll_begin(&ui, right_x, q_scroll_y, right_w, q_scroll_h,
                            &app->queue_scroll,
                            q_cont_h > q_scroll_h ? q_cont_h : q_scroll_h);

    int q_off = (int)app->queue_scroll.offset;
    for (size_t qi = 0; qi < app->queue_count; qi++) {
        int ry = q_scroll_y + (int)qi * q_row_h - q_off;
        if (ry + q_row_h < q_scroll_y || ry > q_scroll_y + q_scroll_h) continue;
        const char *base = strrchr(app->queue[qi], '/');
        base = base ? base + 1 : app->queue[qi];
        snprintf(line, sizeof(line), "%02zu  %s", qi + 1, base);
        if (crossos_ui_selectable(&ui, right_x, ry, right_w - 10 * s,
                                  q_row_h - 1, line, 0)) {
            queue_remove_at(app, qi);
            set_status(app, "Removed from queue");
        }
    }
    if (app->queue_count == 0) {
        crossos_ui_label_centered(&ui, right_x, q_scroll_y, right_w, q_scroll_h,
                                  "Queue is empty \xe2\x80\x94 add files from browser or drop",
                                  ui.theme.text_dim);
    }

    crossos_ui_scroll_end(&ui, right_x, q_scroll_y, right_w, q_scroll_h,
                          &app->queue_scroll);

    /* ── Footer ───────────────────────────────────────────────────────── */
    int footer_y = H - footer_h;
    crossos_ui_panel(&ui, 0, footer_y, W, footer_h, ui.theme.surface);
    crossos_ui_separator(&ui, 0, footer_y, W);

    /* Device selector */
    int dev_x = pad;
    int dev_y = footer_y + 6 * s;
    int dev_w = 200 * s;

    if (crossos_ui_dropdown_header(&ui, dev_x, dev_y, dev_w, 16 * s,
                                   "Drive", app->show_device_dropdown)) {
        app->show_device_dropdown = !app->show_device_dropdown;
    }
    if (app->show_device_dropdown) {
        for (size_t di = 0; di < app->device_count && di < 4; di++) {
            int dy = dev_y + 18 * s + (int)di * 15 * s;
            snprintf(line, sizeof(line), "%.36s", app->devices[di].label);
            if (crossos_ui_selectable(&ui, dev_x, dy, dev_w, 14 * s,
                                      line, di == app->selected_device)) {
                app->selected_device = di;
                app->show_device_dropdown = 0;
            }
        }
        if (app->device_count == 0) {
            crossos_ui_label(&ui, dev_x + 4, dev_y + 18 * s,
                             "No optical drive found", ui.theme.text_dim);
        }
    } else {
        snprintf(line, sizeof(line), "%.36s",
                 app->device_count > 0
                     ? app->devices[app->selected_device].label
                     : "No drive found");
        crossos_ui_label(&ui, dev_x, dev_y + 18 * s, line, ui.theme.text_dim);
    }

    /* Progress bar */
    int prog_x = dev_x + dev_w + pad;
    int prog_w = W / 2 - dev_w - pad * 2;
    snprintf(line, sizeof(line), "%s  %.1f%%  %.1f MiB/s",
             burn_state_name(app->burn.state),
             app->burn.percent, app->burn.speed_mib_s);
    crossos_ui_label(&ui, prog_x, dev_y, line, ui.theme.text);
    crossos_ui_progress_bar(&ui, prog_x, dev_y + 14 * s,
                            prog_w, 10 * s, app->burn.percent, "");

    /* Button row */
    int btn_y   = footer_y + 40 * s;
    int btn_h   = 18 * s;
    crossos_ui_layout_t brow;
    crossos_ui_layout_begin_row(&brow, pad, btn_y, W - pad * 2, btn_h, 6 * s);
    crossos_rect_t br;

    if (crossos_ui_layout_row_next(&brow, 58 * s, &br))
        if (crossos_ui_button(&ui, br.x, br.y, br.width, br.height, "Open",
                              app->file_count > 0 &&
                              app->files[app->selected_index].is_dir))
            open_selected(app);

    if (crossos_ui_layout_row_next(&brow, 48 * s, &br))
        if (crossos_ui_button(&ui, br.x, br.y, br.width, br.height, "Add",
                              app->file_count > 0))
            add_selected_to_queue(app);

    if (crossos_ui_layout_row_next(&brow, 48 * s, &br))
        if (crossos_ui_button_ghost(&ui, br.x, br.y, br.width, br.height, "Pick", 1))
            pick_files_to_queue(app);

    if (crossos_ui_layout_row_next(&brow, 40 * s, &br))
        if (crossos_ui_button_ghost(&ui, br.x, br.y, br.width, br.height, "Up", 1))
            go_parent(app);

    if (crossos_ui_layout_row_next(&brow, 66 * s, &br))
        if (crossos_ui_button_ghost(&ui, br.x, br.y, br.width, br.height, "Refresh", 1)) {
            refresh_files(app);
            refresh_devices(app);
            set_status(app, "Refreshed");
        }

    if (crossos_ui_layout_row_next(&brow, 60 * s, &br))
        if (crossos_ui_button(&ui, br.x, br.y, br.width, br.height, "Burn",
                              app->queue_count > 0 && !app->burn_job))
            start_burn(app);

    if (crossos_ui_layout_row_next(&brow, 66 * s, &br))
        if (crossos_ui_button_danger(&ui, br.x, br.y, br.width, br.height, "Cancel",
                                     app->burn_job != NULL)) {
            if (app->burn_job) {
                crossos_optical_burn_cancel(app->burn_job);
                set_status(app, "Cancel requested");
            }
        }

    /* Status message */
    crossos_ui_label(&ui, pad, btn_y + btn_h + 4 * s, app->status, ui.theme.text_dim);

    crossos_surface_unlock(app->surface);
    crossos_surface_present(app->surface);
}

static void handle_event(app_state_t *app, const crossos_event_t *ev)
{
    if (ev->type == CROSSOS_EVENT_QUIT || ev->type == CROSSOS_EVENT_WINDOW_CLOSE) {
        app->running = 0;
        return;
    }

    if (ev->type == CROSSOS_EVENT_KEY_DOWN) {
        app->ui_input.key_pressed = ev->key.keycode;
        app->ui_input.key_mods    = (int)ev->key.mods;
        int key = ev->key.keycode;
        /* Only fire app-level shortcuts when a text field is not focused. */
        if (app->search_buf.focused) return;
        if (key == CROSSOS_KEY_ESCAPE) {
            app->running = 0;
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

int main(void)
{
    if (crossos_init() != CROSSOS_OK) {
        fprintf(stderr, "crossos_init failed: %s\n", crossos_get_error());
        return 1;
    }

    app_state_t app;
    memset(&app, 0, sizeof(app));
    app.running = 1;
    app.burn.state = CROSSOS_OPTICAL_BURN_IDLE;

#if defined(_WIN32)
    snprintf(app.cwd, sizeof(app.cwd), "C:/");
#else
    if (!getcwd(app.cwd, sizeof(app.cwd))) {
        snprintf(app.cwd, sizeof(app.cwd), "/");
    }
#endif

    app.window = crossos_window_create("CrossOS Disc Burner", 1180, 740, CROSSOS_WINDOW_RESIZABLE);
    if (!app.window) {
        fprintf(stderr, "window create failed: %s\n", crossos_get_error());
        crossos_shutdown();
        return 1;
    }

    crossos_window_show(app.window);
    app.surface = crossos_surface_get(app.window);

    refresh_files(&app);
    refresh_devices(&app);
    set_status(&app, "Ready. Add files/folders then press B to burn.");

    while (app.running) {
        crossos_event_t ev;
        while (crossos_poll_event(&ev)) {
            handle_event(&app, &ev);
        }

        update_burn(&app);
        render(&app);
        /* Reset single-frame / transient input fields */
        app.ui_input.pointer_pressed  = 0;
        app.ui_input.pointer_released = 0;
        app.ui_input.scroll_dx        = 0;
        app.ui_input.scroll_dy        = 0;
        app.ui_input.char_input       = 0;
        app.ui_input.key_pressed      = 0;
        app.ui_input.key_mods         = 0;
        app.ui_input.drop_count       = 0;
        sleep_ms(16);
    }

    if (app.burn_job) {
        crossos_optical_burn_free(app.burn_job);
        app.burn_job = NULL;
    }

    clear_burn_source(&app);

    crossos_window_destroy(app.window);
    crossos_shutdown();
    return 0;
}
