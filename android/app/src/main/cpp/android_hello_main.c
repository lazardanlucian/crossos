#include <crossos/crossos.h>

#include <android/log.h>
#include <android_native_app_glue.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LOG_TAG "CrossOSHello"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern void crossos_android_set_app(struct android_app *app);

typedef struct hello_state {
    crossos_window_t *window;
    crossos_surface_t *surface;
    int running;
} hello_state_t;

static void draw_gradient(const crossos_framebuffer_t *fb)
{
    unsigned char *row = (unsigned char *)fb->pixels;
    for (int y = 0; y < fb->height; y++) {
        unsigned char *px = row;
        for (int x = 0; x < fb->width; x++) {
            px[0] = (unsigned char)(x * 255 / (fb->width > 0 ? fb->width : 1));
            px[1] = (unsigned char)(y * 255 / (fb->height > 0 ? fb->height : 1));
            px[2] = 0x20;
            px[3] = 0xFF;
            px += 4;
        }
        row += fb->stride;
    }
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

static void render(hello_state_t *app)
{
    crossos_framebuffer_t fb;
    if (!app->surface || crossos_surface_lock(app->surface, &fb) != CROSSOS_OK) {
        return;
    }
    draw_gradient(&fb);
    crossos_surface_unlock(app->surface);
    crossos_surface_present(app->surface);
}

static void handle_event(hello_state_t *app, const crossos_event_t *ev)
{
    if (ev->type == CROSSOS_EVENT_QUIT || ev->type == CROSSOS_EVENT_WINDOW_CLOSE) {
        app->running = 0;
        crossos_quit();
        return;
    }

    if (ev->type == CROSSOS_EVENT_KEY_DOWN && ev->key.keycode == CROSSOS_KEY_ESCAPE) {
        app->running = 0;
        crossos_quit();
    }
}

void android_main(struct android_app *android_app)
{
    hello_state_t app;

    crossos_android_set_app(android_app);
    if (!wait_for_native_window(android_app)) {
        return;
    }

    if (crossos_init() != CROSSOS_OK) {
        LOGE("crossos_init failed: %s", crossos_get_error());
        return;
    }

    memset(&app, 0, sizeof(app));
    app.running = 1;

    app.window = crossos_window_create("CrossOS Hello World", 0, 0, 0);
    if (!app.window) {
        LOGE("crossos_window_create failed: %s", crossos_get_error());
        crossos_shutdown();
        return;
    }

    crossos_window_show(app.window);
    app.surface = crossos_surface_get(app.window);

    while (app.running) {
        crossos_event_t ev;
        while (crossos_poll_event(&ev)) {
            handle_event(&app, &ev);
        }
        render(&app);
        usleep(16000);
    }

    crossos_window_destroy(app.window);
    crossos_shutdown();
}
