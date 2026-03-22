/**
 * examples/hello_world/main.c
 *
 * Minimal CrossOS application that:
 *   1. Opens an 800×600 window with a dark-blue background.
 *   2. Renders a simple colour gradient into the software framebuffer.
 *   3. Prints every input event (keyboard, pointer, touch) to stdout.
 *   4. Exits cleanly when the window is closed or Escape is pressed.
 *
 * Build:
 *   mkdir build && cd build
 *   cmake .. -DCROSSOS_BUILD_EXAMPLES=ON
 *   cmake --build .
 *   ./examples/hello_world/hello_world
 */

#include <crossos/crossos.h>

#include <stdio.h>
#include <string.h>

/* ── Framebuffer helper ──────────────────────────────────────────────── */

static void draw_gradient(const crossos_framebuffer_t *fb)
{
    unsigned char *row = (unsigned char *)fb->pixels;
    for (int y = 0; y < fb->height; y++) {
        unsigned char *px = row;
        for (int x = 0; x < fb->width; x++) {
            /* BGRA layout (Windows / Linux little-endian) */
            px[0] = (unsigned char)(x * 255 / fb->width);  /* B */
            px[1] = (unsigned char)(y * 255 / fb->height); /* G */
            px[2] = 0x20;                                   /* R */
            px[3] = 0xFF;                                   /* A */
            px += 4;
        }
        row += fb->stride;
    }
}

/* ── Event callback ──────────────────────────────────────────────────── */

typedef struct app_state {
    crossos_window_t  *win;
    crossos_surface_t *surf;
    int                running;
} app_state_t;

static void on_event(const crossos_event_t *ev, void *ud)
{
    app_state_t *app = (app_state_t *)ud;

    switch (ev->type) {
    case CROSSOS_EVENT_QUIT:
    case CROSSOS_EVENT_WINDOW_CLOSE:
        printf("[event] window close / quit\n");
        app->running = 0;
        crossos_quit();
        break;

    case CROSSOS_EVENT_WINDOW_RESIZE:
        printf("[event] resize %d × %d\n",
               ev->resize.width, ev->resize.height);
        break;

    case CROSSOS_EVENT_KEY_DOWN:
        printf("[event] key_down  keycode=%d  mods=%d  repeat=%d\n",
               ev->key.keycode, ev->key.mods, ev->key.repeat);
        if (ev->key.keycode == CROSSOS_KEY_ESCAPE) {
            app->running = 0;
            crossos_quit();
        }
        break;

    case CROSSOS_EVENT_KEY_UP:
        printf("[event] key_up    keycode=%d\n", ev->key.keycode);
        break;

    case CROSSOS_EVENT_POINTER_DOWN:
        printf("[event] pointer_down  (%.0f, %.0f)  btn=%d\n",
               ev->pointer.x, ev->pointer.y, ev->pointer.button);
        break;

    case CROSSOS_EVENT_POINTER_UP:
        printf("[event] pointer_up    (%.0f, %.0f)  btn=%d\n",
               ev->pointer.x, ev->pointer.y, ev->pointer.button);
        break;

    case CROSSOS_EVENT_POINTER_MOVE:
        /* Suppress move spam – only print every 20th event */
        { static int n = 0; if (++n % 20 == 0)
            printf("[event] pointer_move  (%.0f, %.0f)\n",
                   ev->pointer.x, ev->pointer.y); }
        break;

    case CROSSOS_EVENT_POINTER_SCROLL:
        printf("[event] scroll dx=%.1f dy=%.1f\n",
               ev->pointer.scroll_x, ev->pointer.scroll_y);
        break;

    case CROSSOS_EVENT_TOUCH_BEGIN:
        printf("[event] touch_begin  %d points\n", ev->touch.count);
        for (int i = 0; i < ev->touch.count; i++)
            printf("         id=%d  (%.0f, %.0f)  pressure=%.2f\n",
                   ev->touch.points[i].id,
                   ev->touch.points[i].x, ev->touch.points[i].y,
                   ev->touch.points[i].pressure);
        break;

    case CROSSOS_EVENT_TOUCH_UPDATE:
        printf("[event] touch_update %d points\n", ev->touch.count);
        break;

    case CROSSOS_EVENT_TOUCH_END:
        printf("[event] touch_end    %d points\n", ev->touch.count);
        break;

    default:
        break;
    }

    /* Re-render every frame */
    if (app->running && app->surf) {
        crossos_framebuffer_t fb;
        if (crossos_surface_lock(app->surf, &fb) == CROSSOS_OK) {
            draw_gradient(&fb);
            crossos_surface_unlock(app->surf);
            crossos_surface_present(app->surf);
        }
    }
}

/* ── Entry point ─────────────────────────────────────────────────────── */

int main(void)
{
    printf("CrossOS %s  platform=%s\n",
           CROSSOS_VERSION_STRING, crossos_platform_name());

    if (crossos_init() != CROSSOS_OK) {
        fprintf(stderr, "crossos_init failed: %s\n", crossos_get_error());
        return 1;
    }

    crossos_window_t *win = crossos_window_create(
        "CrossOS – Hello World",
        800, 600,
        CROSSOS_WINDOW_RESIZABLE);

    if (!win) {
        fprintf(stderr, "window_create failed: %s\n", crossos_get_error());
        crossos_shutdown();
        return 1;
    }

    printf("Window created (%d × %d)  touch=%s\n",
           800, 600,
           crossos_touch_is_supported() ? "yes" : "no");

    crossos_window_show(win);

    app_state_t app;
    memset(&app, 0, sizeof(app));
    app.win     = win;
    app.surf    = crossos_surface_get(win);
    app.running = 1;

    /* Initial render */
    if (app.surf) {
        crossos_framebuffer_t fb;
        if (crossos_surface_lock(app.surf, &fb) == CROSSOS_OK) {
            draw_gradient(&fb);
            crossos_surface_unlock(app.surf);
            crossos_surface_present(app.surf);
        }
    }

    crossos_run_loop(win, on_event, &app);

    crossos_window_destroy(win);
    crossos_shutdown();

    printf("Goodbye.\n");
    return 0;
}
