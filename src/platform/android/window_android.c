/**
 * platform/android/window_android.c  –  Android NDK window implementation.
 *
 * Uses:
 *   • android_native_app_glue for the app lifecycle and main loop
 *   • ANativeWindow for the display surface
 *   • ANativeWindow_lock / _unlockAndPost for software framebuffer
 *   • AInputEvent for touch input
 *
 * On Android the "window" object wraps the ANativeWindow* provided by the
 * android_app activity rather than creating a new OS window.
 */

#ifdef __ANDROID__

#include <crossos/crossos.h>

#include "android_internal.h"

#include <android/native_window.h>
#include <android/log.h>
#include <android_native_app_glue.h>

#include <stdlib.h>
#include <string.h>

#define LOG_TAG "CrossOS"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* ── External references ──────────────────────────────────────────────── */
extern void         crossos__set_error(const char *fmt, ...);
extern volatile int crossos__quit_requested;
extern void         crossos__push_event(const crossos_event_t *ev);
extern crossos_result_t crossos_android_optical_backend_init(void);
extern void crossos_android_optical_backend_shutdown(void);

/* ── Module state ─────────────────────────────────────────────────────── */

/* Set by the JNI entry point before crossos_init() is called. */
struct android_app *s_app = NULL;

/* Called from the JNI bridge (android_main equivalent). */
void crossos_android_set_app(struct android_app *app)
{
    s_app = app;
}

/* Global reference to the active window, used for surface re-init on resume. */
static crossos_window_t *s_window = NULL;

/* ── Internal structures ──────────────────────────────────────────────── */

struct crossos_window {
    ANativeWindow *native_win;
    int            width;
    int            height;
    struct crossos_surface *surface;
};

struct crossos_surface {
    crossos_window_t *win;
    int               locked;
    ANativeWindow_Buffer buffer;
};

/* ── Platform init / shutdown ─────────────────────────────────────────── */

crossos_result_t crossos__platform_init(void)
{
    if (!s_app) {
        crossos__set_error("Android: crossos_android_set_app() not called");
        return CROSSOS_ERR_INIT;
    }
    if (crossos_android_optical_backend_init() != CROSSOS_OK) {
        LOGI("CrossOS Android optical backend disabled: %s", crossos_get_error());
    }
    LOGI("CrossOS Android platform init");
    return CROSSOS_OK;
}

void crossos__platform_shutdown(void)
{
    crossos_android_optical_backend_shutdown();
    LOGI("CrossOS Android platform shutdown");
}

/* ── Window lifecycle ─────────────────────────────────────────────────── */

crossos_window_t *crossos_window_create(const char *title,
                                        int width, int height,
                                        uint32_t flags)
{
    (void)title; (void)flags;

    if (!s_app || !s_app->window) {
        crossos__set_error("Android: ANativeWindow not ready");
        return NULL;
    }

    crossos_window_t *win = calloc(1, sizeof(*win));
    if (!win) { crossos__set_error("Android: OOM"); return NULL; }

    win->native_win = s_app->window;
    ANativeWindow_acquire(win->native_win);

    win->width  = width  > 0 ? width  : ANativeWindow_getWidth(win->native_win);
    win->height = height > 0 ? height : ANativeWindow_getHeight(win->native_win);

    ANativeWindow_setBuffersGeometry(win->native_win,
                                     win->width, win->height,
                                     WINDOW_FORMAT_RGBA_8888);

    win->surface = calloc(1, sizeof(*win->surface));
    if (win->surface) win->surface->win = win;

    s_window = win;
    return win;
}

void crossos_window_destroy(crossos_window_t *win)
{
    if (!win) return;
    if (s_window == win) s_window = NULL;
    free(win->surface);
    if (win->native_win) ANativeWindow_release(win->native_win);
    free(win);
}

void crossos_window_show(crossos_window_t *win)  { (void)win; /* no-op */ }
void crossos_window_hide(crossos_window_t *win)  { (void)win; /* no-op */ }

/* ── Surface lifecycle helpers (called by input_android.c on APP_CMD_*) ─ */

/* Called when APP_CMD_INIT_WINDOW fires – acquires/re-acquires the surface. */
void crossos_android_on_window_init(ANativeWindow *new_win)
{
    if (!s_window || !new_win) return;
    if (s_window->native_win && s_window->native_win != new_win) {
        ANativeWindow_release(s_window->native_win);
    }
    s_window->native_win = new_win;
    ANativeWindow_acquire(s_window->native_win);
    s_window->width  = ANativeWindow_getWidth(new_win);
    s_window->height = ANativeWindow_getHeight(new_win);
    ANativeWindow_setBuffersGeometry(s_window->native_win,
                                     s_window->width, s_window->height,
                                     WINDOW_FORMAT_RGBA_8888);
}

/* Called when APP_CMD_TERM_WINDOW fires – releases the surface so render
 * calls skip gracefully while the app is in the background. */
void crossos_android_on_window_term(void)
{
    if (!s_window) return;
    if (s_window->native_win) {
        ANativeWindow_release(s_window->native_win);
        s_window->native_win = NULL;
    }
}

crossos_result_t crossos_window_set_fullscreen(crossos_window_t *win, int fs)
{
    (void)win; (void)fs;
    return CROSSOS_OK; /* Android is always full-screen */
}

crossos_result_t crossos_window_resize(crossos_window_t *win, int w, int h)
{
    (void)win; (void)w; (void)h;
    return CROSSOS_ERR_UNSUPPORT; /* Android window size is OS-controlled */
}

crossos_result_t crossos_window_set_title(crossos_window_t *win, const char *t)
{
    (void)win; (void)t;
    return CROSSOS_OK; /* No title bar on Android */
}

void crossos_window_get_size(const crossos_window_t *win, int *w, int *h)
{
    if (!win) { if (w) *w = 0; if (h) *h = 0; return; }
    if (w) *w = win->width;
    if (h) *h = win->height;
}

int crossos_window_is_fullscreen(const crossos_window_t *win)
{
    (void)win;
    return 1; /* Always true on Android */
}

void *crossos_window_get_native_handle(const crossos_window_t *win)
{
    return win ? win->native_win : NULL;
}

/* ── Display information ──────────────────────────────────────────────── */

crossos_result_t crossos_display_get_size(int idx, int *w, int *h)
{
    if (!s_app || !s_app->window || idx != 0) return CROSSOS_ERR_PARAM;
    if (w) *w = ANativeWindow_getWidth(s_app->window);
    if (h) *h = ANativeWindow_getHeight(s_app->window);
    return CROSSOS_OK;
}

int crossos_display_count(void)
{
    return 1;
}

/* ── Surface / framebuffer ────────────────────────────────────────────── */

crossos_surface_t *crossos_surface_get(crossos_window_t *win)
{
    return win ? win->surface : NULL;
}

crossos_result_t crossos_surface_lock(crossos_surface_t   *surf,
                                      crossos_framebuffer_t *fb)
{
    if (!surf || !fb) return CROSSOS_ERR_PARAM;
    if (!surf->win || !surf->win->native_win) {
        /* Surface is temporarily unavailable (e.g. app backgrounded). */
        return CROSSOS_ERR_DISPLAY;
    }
    if (surf->locked)  return CROSSOS_ERR_DISPLAY;

    if (ANativeWindow_lock(surf->win->native_win, &surf->buffer, NULL) < 0) {
        crossos__set_error("Android: ANativeWindow_lock failed");
        return CROSSOS_ERR_DISPLAY;
    }
    surf->locked = 1;

    fb->pixels = surf->buffer.bits;
    fb->width  = surf->buffer.width;
    fb->height = surf->buffer.height;
    fb->stride = surf->buffer.stride * 4; /* stride is in pixels */
    fb->format = CROSSOS_PIXEL_FMT_RGBA8888;
    return CROSSOS_OK;
}

void crossos_surface_unlock(crossos_surface_t *surf)
{
    if (surf && surf->locked) {
        /* Don't blit yet; wait for present */
        surf->locked = 0;
    }
}

crossos_result_t crossos_surface_present(crossos_surface_t *surf)
{
    if (!surf || !surf->win || !surf->win->native_win) return CROSSOS_ERR_PARAM;
    ANativeWindow_unlockAndPost(surf->win->native_win);
    return CROSSOS_OK;
}

#endif /* __ANDROID__ */
