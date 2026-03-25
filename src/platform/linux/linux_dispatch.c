#if defined(__linux__) && !defined(__ANDROID__)

#include <crossos/crossos.h>

#include "linux_backend.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern void crossos__set_error(const char *fmt, ...);

static const crossos_linux_backend_vtable_t *s_backend = NULL;

static int terminal_candidate_available(void)
{
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
}

static int env_equals(const char *value, const char *expected)
{
    return value && expected && strcmp(value, expected) == 0;
}

crossos_result_t crossos__platform_init(void)
{
    const char *requested = getenv("CROSSOS_LINUX_BACKEND");

    s_backend = NULL;

    if (env_equals(requested, "terminal") || env_equals(requested, "tty"))
    {
        s_backend = &crossos__linux_terminal_backend;
        return s_backend->platform_init();
    }

#ifdef CROSSOS_HAS_X11_BACKEND
    if (env_equals(requested, "x11"))
    {
        s_backend = &crossos__linux_x11_backend;
        return s_backend->platform_init();
    }
#endif

    if (requested && !env_equals(requested, "auto"))
    {
        crossos__set_error("Linux backend '%s' is not supported", requested);
        return CROSSOS_ERR_INIT;
    }

#ifdef CROSSOS_HAS_X11_BACKEND
    {
        const char *display = getenv("DISPLAY");
        if (display && display[0] != '\0')
        {
            s_backend = &crossos__linux_x11_backend;
            if (s_backend->platform_init() == CROSSOS_OK)
            {
                return CROSSOS_OK;
            }
            s_backend = NULL;
        }
    }
#endif

    if (terminal_candidate_available())
    {
        s_backend = &crossos__linux_terminal_backend;
        return s_backend->platform_init();
    }

#ifdef CROSSOS_HAS_X11_BACKEND
    crossos__set_error("Linux backend auto-selection failed: no X11 display and no interactive terminal");
#else
    crossos__set_error("Linux terminal backend requires an interactive TTY");
#endif
    return CROSSOS_ERR_INIT;
}

void crossos__platform_shutdown(void)
{
    if (s_backend)
    {
        s_backend->platform_shutdown();
        s_backend = NULL;
    }
}

crossos_window_t *crossos_window_create(const char *title,
                                        int width,
                                        int height,
                                        uint32_t flags)
{
    if (!s_backend)
    {
        crossos__set_error("Linux backend not initialised");
        return NULL;
    }
    return s_backend->window_create(title, width, height, flags);
}

void crossos_window_destroy(crossos_window_t *win)
{
    if (s_backend)
        s_backend->window_destroy(win);
}

void crossos_window_show(crossos_window_t *win)
{
    if (s_backend)
        s_backend->window_show(win);
}

void crossos_window_hide(crossos_window_t *win)
{
    if (s_backend)
        s_backend->window_hide(win);
}

crossos_result_t crossos_window_set_fullscreen(crossos_window_t *win, int fullscreen)
{
    if (!win)
        return CROSSOS_ERR_PARAM;
    if (!s_backend)
        return CROSSOS_ERR_INIT;
    return s_backend->window_set_fullscreen(win, fullscreen);
}

crossos_result_t crossos_window_resize(crossos_window_t *win, int width, int height)
{
    if (!win)
        return CROSSOS_ERR_PARAM;
    if (!s_backend)
        return CROSSOS_ERR_INIT;
    return s_backend->window_resize(win, width, height);
}

crossos_result_t crossos_window_set_title(crossos_window_t *win, const char *title)
{
    if (!win)
        return CROSSOS_ERR_PARAM;
    if (!s_backend)
        return CROSSOS_ERR_INIT;
    return s_backend->window_set_title(win, title);
}

void crossos_window_get_size(const crossos_window_t *win, int *width, int *height)
{
    if (!s_backend)
    {
        if (width)
            *width = 0;
        if (height)
            *height = 0;
        return;
    }
    s_backend->window_get_size(win, width, height);
}

int crossos_window_is_fullscreen(const crossos_window_t *win)
{
    if (!s_backend)
        return 0;
    return s_backend->window_is_fullscreen(win);
}

void *crossos_window_get_native_handle(const crossos_window_t *win)
{
    if (!s_backend)
        return NULL;
    return s_backend->window_get_native_handle(win);
}

crossos_result_t crossos_display_get_size(int display_index, int *width, int *height)
{
    if (!s_backend)
        return CROSSOS_ERR_INIT;
    return s_backend->display_get_size(display_index, width, height);
}

int crossos_display_count(void)
{
    if (!s_backend)
        return 0;
    return s_backend->display_count();
}

crossos_surface_t *crossos_surface_get(crossos_window_t *win)
{
    if (!s_backend)
        return NULL;
    return s_backend->surface_get(win);
}

crossos_result_t crossos_surface_lock(crossos_surface_t *surf,
                                      crossos_framebuffer_t *fb)
{
    if (!s_backend)
        return CROSSOS_ERR_INIT;
    return s_backend->surface_lock(surf, fb);
}

void crossos_surface_unlock(crossos_surface_t *surf)
{
    if (s_backend)
        s_backend->surface_unlock(surf);
}

crossos_result_t crossos_surface_present(crossos_surface_t *surf)
{
    if (!s_backend)
        return CROSSOS_ERR_INIT;
    return s_backend->surface_present(surf);
}

int crossos_poll_event(crossos_event_t *event)
{
    if (!s_backend)
        return 0;
    return s_backend->poll_event(event);
}

int crossos_wait_event(crossos_event_t *event)
{
    if (!s_backend)
        return 0;
    return s_backend->wait_event(event);
}

void crossos_run_loop(crossos_window_t *win,
                      crossos_event_cb_t cb,
                      void *user_data)
{
    if (s_backend)
        s_backend->run_loop(win, cb, user_data);
}

int crossos_touch_get_active(const crossos_window_t *win,
                             crossos_touch_point_t pts[CROSSOS_MAX_TOUCH_POINTS])
{
    if (!s_backend)
        return 0;
    return s_backend->touch_get_active(win, pts);
}

int crossos_touch_is_supported(void)
{
    if (!s_backend)
        return 0;
    return s_backend->touch_is_supported();
}

#endif /* __linux__ && !__ANDROID__ */