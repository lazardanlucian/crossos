/**
 * platform/linux/x11_internal.h
 *
 * Internal header shared by window_x11.c and input_x11.c.
 * NOT part of the public API; do not include from application code.
 */

#ifndef CROSSOS_X11_INTERNAL_H
#define CROSSOS_X11_INTERNAL_H

#if defined(__linux__) && !defined(__ANDROID__)

#include <X11/Xlib.h>
#include <X11/Xatom.h>

/* Module-level X11 state – defined in window_x11.c, used in input_x11.c */
extern Display *s_display;
extern int      s_screen;
extern int      s_xi2_opcode;

extern Atom s_wm_delete_window;
extern Atom s_net_wm_state;
extern Atom s_net_wm_state_fullscreen;

#endif /* __linux__ && !__ANDROID__ */
#endif /* CROSSOS_X11_INTERNAL_H */
