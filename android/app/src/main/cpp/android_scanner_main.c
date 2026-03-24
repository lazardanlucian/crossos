#include <crossos/crossos.h>

#include <android/log.h>
#include <android_native_app_glue.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LOG_TAG "CrossOSFilmScanner"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern void crossos_android_set_app(struct android_app *app);

typedef struct scanner_state
{
    crossos_window_t *window;
    crossos_surface_t *surface;
    int running;
    int scanner_ready;
    int scanner_count;
} scanner_state_t;

static int wait_for_native_window(struct android_app *app)
{
    while (!app->window)
    {
        int events;
        struct android_poll_source *source = NULL;
        if (ALooper_pollAll(-1, NULL, &events, (void **)&source) >= 0)
        {
            if (source)
            {
                source->process(app, source);
            }
        }
        if (app->destroyRequested)
        {
            LOGE("destroy requested before native window became available");
            return 0;
        }
    }
    return 1;
}

static void draw_status(const crossos_framebuffer_t *fb, const scanner_state_t *app)
{
    crossos_draw_clear(fb, (crossos_color_t){18, 18, 28, 255});
    crossos_draw_fill_rect(fb, 0, 0, fb->width, 44, (crossos_color_t){12, 12, 18, 255});
    crossos_draw_text(fb, 12, 14, "CrossOS Film Scanner (Android)",
                      (crossos_color_t){236, 143, 38, 255}, 1);

    if (!app->scanner_ready)
    {
        crossos_draw_text(fb, 12, 70,
                          "Scanner subsystem unavailable on Android yet.",
                          (crossos_color_t){220, 220, 230, 255}, 1);
        crossos_draw_text(fb, 12, 90,
                          "Backend stub is present; native USB implementation is next.",
                          (crossos_color_t){140, 140, 160, 255}, 1);
        return;
    }

    char msg[96];
    snprintf(msg, sizeof(msg), "Scanners detected: %d", app->scanner_count);
    crossos_draw_text(fb, 12, 70, msg, (crossos_color_t){220, 220, 230, 255}, 1);
    crossos_draw_text(fb, 12, 90,
                      "Use desktop Linux scanner app for full scan workflow for now.",
                      (crossos_color_t){140, 140, 160, 255}, 1);
}

static void render(scanner_state_t *app)
{
    crossos_framebuffer_t fb;
    if (!app->surface || crossos_surface_lock(app->surface, &fb) != CROSSOS_OK)
    {
        return;
    }

    draw_status(&fb, app);

    crossos_surface_unlock(app->surface);
    crossos_surface_present(app->surface);
}

static void handle_event(scanner_state_t *app, const crossos_event_t *ev)
{
    if (ev->type == CROSSOS_EVENT_QUIT || ev->type == CROSSOS_EVENT_WINDOW_CLOSE)
    {
        app->running = 0;
        crossos_quit();
        return;
    }

    if (ev->type == CROSSOS_EVENT_KEY_DOWN && ev->key.keycode == CROSSOS_KEY_ESCAPE)
    {
        app->running = 0;
        crossos_quit();
    }
}

void android_main(struct android_app *android_app)
{
    scanner_state_t app;

    crossos_android_set_app(android_app);
    if (!wait_for_native_window(android_app))
    {
        return;
    }

    if (crossos_init() != CROSSOS_OK)
    {
        LOGE("crossos_init failed: %s", crossos_get_error());
        return;
    }

    memset(&app, 0, sizeof(app));
    app.running = 1;

    app.window = crossos_window_create("CrossOS Film Scanner", 0, 0, 0);
    if (!app.window)
    {
        LOGE("crossos_window_create failed: %s", crossos_get_error());
        crossos_shutdown();
        return;
    }

    crossos_window_show(app.window);
    app.surface = crossos_surface_get(app.window);

    if (crossos_scanner_init() == CROSSOS_OK)
    {
        crossos_scanner_info_t devices[CROSSOS_SCANNER_MAX_DEVS];
        int count = crossos_scanner_enumerate(devices, CROSSOS_SCANNER_MAX_DEVS);
        app.scanner_ready = 1;
        app.scanner_count = count > 0 ? count : 0;
    }

    while (app.running)
    {
        crossos_event_t ev;
        while (crossos_poll_event(&ev))
        {
            handle_event(&app, &ev);
        }
        render(&app);
        usleep(16000);
    }

    if (app.scanner_ready)
    {
        crossos_scanner_shutdown();
    }
    crossos_window_destroy(app.window);
    crossos_shutdown();
}
