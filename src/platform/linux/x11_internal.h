/**
 * platform/linux/x11_internal.h
 *
 * Internal header shared by window_x11.c and input_x11.c.
 * NOT part of the public API; do not include from application code.
 */

#ifndef CROSSOS_X11_INTERNAL_H
#define CROSSOS_X11_INTERNAL_H

#if defined(__linux__) && !defined(__ANDROID__)

#include <crossos/crossos.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

/* Module-level X11 state – defined in window_x11.c, used in input_x11.c */
extern Display *s_display;
extern int s_screen;
extern int s_xi2_opcode;

extern Atom s_wm_delete_window;
extern Atom s_net_wm_state;
extern Atom s_net_wm_state_fullscreen;

int x11_poll_event(crossos_event_t *ev);
int x11_wait_event(crossos_event_t *ev);
void x11_run_loop(crossos_window_t *win,
                  crossos_event_cb_t cb,
                  void *user_data);
int x11_touch_get_active(const crossos_window_t *win,
                         crossos_touch_point_t pts[CROSSOS_MAX_TOUCH_POINTS]);
int x11_touch_is_supported(void);

#endif /* __linux__ && !__ANDROID__ */
#endif /* CROSSOS_X11_INTERNAL_H */
