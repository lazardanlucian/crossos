#ifndef CROSSOS_LINUX_BACKEND_H
#define CROSSOS_LINUX_BACKEND_H

#if defined(__linux__) && !defined(__ANDROID__)

#include <crossos/crossos.h>

typedef struct crossos_linux_backend_vtable
{
    crossos_result_t (*platform_init)(void);
    void (*platform_shutdown)(void);

    crossos_window_t *(*window_create)(const char *title,
                                       int width,
                                       int height,
                                       uint32_t flags);
    void (*window_destroy)(crossos_window_t *win);
    void (*window_show)(crossos_window_t *win);
    void (*window_hide)(crossos_window_t *win);
    crossos_result_t (*window_set_fullscreen)(crossos_window_t *win, int fullscreen);
    crossos_result_t (*window_resize)(crossos_window_t *win, int width, int height);
    crossos_result_t (*window_set_title)(crossos_window_t *win, const char *title);
    void (*window_get_size)(const crossos_window_t *win, int *width, int *height);
    int (*window_is_fullscreen)(const crossos_window_t *win);
    void *(*window_get_native_handle)(const crossos_window_t *win);

    crossos_result_t (*display_get_size)(int display_index, int *width, int *height);
    int (*display_count)(void);

    crossos_surface_t *(*surface_get)(crossos_window_t *win);
    crossos_result_t (*surface_lock)(crossos_surface_t *surf,
                                     crossos_framebuffer_t *fb);
    void (*surface_unlock)(crossos_surface_t *surf);
    crossos_result_t (*surface_present)(crossos_surface_t *surf);

    int (*poll_event)(crossos_event_t *event);
    int (*wait_event)(crossos_event_t *event);
    void (*run_loop)(crossos_window_t *win,
                     crossos_event_cb_t cb,
                     void *user_data);
    int (*touch_get_active)(const crossos_window_t *win,
                            crossos_touch_point_t pts[CROSSOS_MAX_TOUCH_POINTS]);
    int (*touch_is_supported)(void);
} crossos_linux_backend_vtable_t;

#ifdef CROSSOS_HAS_X11_BACKEND
extern const crossos_linux_backend_vtable_t crossos__linux_x11_backend;
#endif

extern const crossos_linux_backend_vtable_t crossos__linux_terminal_backend;

#endif /* __linux__ && !__ANDROID__ */

#endif /* CROSSOS_LINUX_BACKEND_H */