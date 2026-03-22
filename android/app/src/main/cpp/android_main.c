#include <crossos/crossos.h>

#include <android/log.h>
#include <android_native_app_glue.h>

#include <string.h>

#define LOG_TAG "CrossOSHello"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* Internal entry point exposed by CrossOS Android backend. */
extern void crossos_android_set_app(struct android_app *app);

static void draw_gradient(const crossos_framebuffer_t *fb)
{
    unsigned char *row = (unsigned char *)fb->pixels;
    for (int y = 0; y < fb->height; y++) {
        unsigned char *px = row;
        for (int x = 0; x < fb->width; x++) {
            px[0] = (unsigned char)(x * 255 / fb->width);  /* B */
            px[1] = (unsigned char)(y * 255 / fb->height); /* G */
            px[2] = 0x20;                                  /* R */
            px[3] = 0xFF;                                  /* A */
            px += 4;
        }
        row += fb->stride;
    }
}

typedef struct app_state {
    crossos_surface_t *surf;
    int running;
} app_state_t;

static void on_event(const crossos_event_t *ev, void *ud)
{
    app_state_t *app = (app_state_t *)ud;

    if (ev->type == CROSSOS_EVENT_QUIT || ev->type == CROSSOS_EVENT_WINDOW_CLOSE) {
        LOGI("quit requested");
        app->running = 0;
        crossos_quit();
    }

    if (app->running && app->surf) {
        crossos_framebuffer_t fb;
        if (crossos_surface_lock(app->surf, &fb) == CROSSOS_OK) {
            draw_gradient(&fb);
            crossos_surface_unlock(app->surf);
            crossos_surface_present(app->surf);
        }
    }
}

void android_main(struct android_app *app)
{
    crossos_android_set_app(app);

    if (crossos_init() != CROSSOS_OK) {
        LOGE("crossos_init failed: %s", crossos_get_error());
        return;
    }

    crossos_window_t *win = crossos_window_create("CrossOS - Hello", 0, 0, 0);
    if (!win) {
        LOGE("crossos_window_create failed: %s", crossos_get_error());
        crossos_shutdown();
        return;
    }

    crossos_window_show(win);

    app_state_t state;
    memset(&state, 0, sizeof(state));
    state.surf = crossos_surface_get(win);
    state.running = 1;

    if (state.surf) {
        crossos_framebuffer_t fb;
        if (crossos_surface_lock(state.surf, &fb) == CROSSOS_OK) {
            draw_gradient(&fb);
            crossos_surface_unlock(state.surf);
            crossos_surface_present(state.surf);
        }
    }

    crossos_run_loop(win, on_event, &state);

    crossos_window_destroy(win);
    crossos_shutdown();
}
