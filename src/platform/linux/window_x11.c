/**
 * platform/linux/window_x11.c  –  X11 window and software-framebuffer.
 *
 * Uses:
 *   • Xlib for window creation
 *   • XInput2 (XI2) for multi-touch via XI_TouchBegin/Update/End events
 *   • XShmImage (MIT-SHM) when available for zero-copy framebuffer present;
 *     falls back to XPutImage.
 */

#if defined(__linux__) && !defined(__ANDROID__)

#include <crossos/crossos.h>

#include "linux_backend.h"
#include "x11_internal.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xrandr.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* ── External references ──────────────────────────────────────────────── */
extern void crossos__set_error(const char *fmt, ...);
extern volatile int crossos__quit_requested;
extern void crossos__push_event(const crossos_event_t *ev);

/* ── Module-level X11 state ───────────────────────────────────────────── */
Display *s_display = NULL;
int s_screen = 0;
int s_xi2_opcode = -1;

/* Atoms */
Atom s_wm_delete_window;
Atom s_net_wm_state;
Atom s_net_wm_state_fullscreen;

/* ── Internal structures ──────────────────────────────────────────────── */

struct crossos_window
{
    Window xwin;
    GC gc;
    XImage *ximage;
    void *fb_pixels;
    int width;
    int height;
    int is_fullscreen;
    struct crossos_surface *surface;
};

struct crossos_surface
{
    crossos_window_t *win;
    int locked;
};

static crossos_result_t x11_window_set_fullscreen(crossos_window_t *win, int fs);

/* ── Platform init / shutdown ─────────────────────────────────────────── */

static crossos_result_t x11_platform_init(void)
{
    s_display = XOpenDisplay(NULL);
    if (!s_display)
    {
        crossos__set_error("X11: XOpenDisplay failed – is DISPLAY set?");
        return CROSSOS_ERR_DISPLAY;
    }
    s_screen = DefaultScreen(s_display);

    /* Atoms for WM_DELETE_WINDOW and _NET_WM_STATE */
    s_wm_delete_window = XInternAtom(s_display, "WM_DELETE_WINDOW", False);
    s_net_wm_state = XInternAtom(s_display, "_NET_WM_STATE", False);
    s_net_wm_state_fullscreen =
        XInternAtom(s_display, "_NET_WM_STATE_FULLSCREEN", False);

    /* Xdnd atoms for file-drop support – exposed via extern to input_x11.c */
    extern Atom s_xdnd_aware;
    extern Atom s_xdnd_enter;
    extern Atom s_xdnd_position;
    extern Atom s_xdnd_status;
    extern Atom s_xdnd_drop;
    extern Atom s_xdnd_finished;
    extern Atom s_xdnd_selection;
    extern Atom s_xdnd_type_list;
    extern Atom s_uri_list;
    s_xdnd_aware = XInternAtom(s_display, "XdndAware", False);
    s_xdnd_enter = XInternAtom(s_display, "XdndEnter", False);
    s_xdnd_position = XInternAtom(s_display, "XdndPosition", False);
    s_xdnd_status = XInternAtom(s_display, "XdndStatus", False);
    s_xdnd_drop = XInternAtom(s_display, "XdndDrop", False);
    s_xdnd_finished = XInternAtom(s_display, "XdndFinished", False);
    s_xdnd_selection = XInternAtom(s_display, "XdndSelection", False);
    s_xdnd_type_list = XInternAtom(s_display, "XdndTypeList", False);
    s_uri_list = XInternAtom(s_display, "text/uri-list", False);

    /* Probe XInput2 for multi-touch */
    int event_base, error_base;
    if (XQueryExtension(s_display, "XInputExtension",
                        &s_xi2_opcode, &event_base, &error_base))
    {
        int major = 2, minor = 2;
        XIQueryVersion(s_display, &major, &minor);
        if (major < 2)
            s_xi2_opcode = -1; /* XI2 too old */
    }

    return CROSSOS_OK;
}

static void x11_platform_shutdown(void)
{
    if (s_display)
    {
        XCloseDisplay(s_display);
        s_display = NULL;
    }
}

/* ── Window lifecycle ─────────────────────────────────────────────────── */

static crossos_window_t *x11_window_create(const char *title,
                                           int width, int height,
                                           uint32_t flags)
{
    if (!s_display)
    {
        crossos__set_error("X11: not initialised");
        return NULL;
    }

    crossos_window_t *win = calloc(1, sizeof(*win));
    if (!win)
    {
        crossos__set_error("X11: OOM");
        return NULL;
    }
    win->width = width;
    win->height = height;

    /* Allocate framebuffer */
    int stride = width * 4;
    win->fb_pixels = calloc((size_t)stride * (size_t)height, 1);
    if (!win->fb_pixels)
    {
        crossos__set_error("X11: framebuffer OOM");
        free(win);
        return NULL;
    }

    /* Create X window */
    unsigned long black = BlackPixel(s_display, s_screen);
    unsigned long white = WhitePixel(s_display, s_screen);

    unsigned long mask = 0;
    XSetWindowAttributes attr;
    memset(&attr, 0, sizeof(attr));
    attr.background_pixel = black;
    attr.border_pixel = white;
    attr.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask | FocusChangeMask;
    mask = CWBackPixel | CWBorderPixel | CWEventMask;

    win->xwin = XCreateWindow(
        s_display, RootWindow(s_display, s_screen),
        0, 0, (unsigned)width, (unsigned)height, 1,
        DefaultDepth(s_display, s_screen),
        InputOutput,
        DefaultVisual(s_display, s_screen),
        mask, &attr);

    if (!win->xwin)
    {
        free(win->fb_pixels);
        free(win);
        crossos__set_error("X11: XCreateWindow failed");
        return NULL;
    }

    XStoreName(s_display, win->xwin, title ? title : "");
    XSetWMProtocols(s_display, win->xwin, &s_wm_delete_window, 1);

    /* Announce Xdnd version 5 so drag sources can send us files */
    {
        extern Atom s_xdnd_aware;
        long xdnd_ver = 5;
        XChangeProperty(s_display, win->xwin, s_xdnd_aware, XA_ATOM,
                        32, PropModeReplace, (unsigned char *)&xdnd_ver, 1);
    }

    /* XInput2 multi-touch */
    if (s_xi2_opcode >= 0)
    {
        XIEventMask xi_mask;
        unsigned char bits[XIMaskLen(XI_LASTEVENT)];
        memset(bits, 0, sizeof(bits));
        XISetMask(bits, XI_TouchBegin);
        XISetMask(bits, XI_TouchUpdate);
        XISetMask(bits, XI_TouchEnd);
        xi_mask.deviceid = XIAllMasterDevices;
        xi_mask.mask_len = sizeof(bits);
        xi_mask.mask = bits;
        XISelectEvents(s_display, win->xwin, &xi_mask, 1);
    }

    /* Create GC and XImage for software blit */
    win->gc = XCreateGC(s_display, win->xwin, 0, NULL);
    win->ximage = XCreateImage(
        s_display,
        DefaultVisual(s_display, s_screen),
        (unsigned)DefaultDepth(s_display, s_screen),
        ZPixmap, 0,
        (char *)win->fb_pixels,
        (unsigned)width, (unsigned)height,
        32, stride);

    /* Associate our window pointer with the XID.
     * Using XUniqueContext() (which allocates a unique context ID) would be
     * ideal for a multi-window scenario; we use 0 here for simplicity since
     * the typical CrossOS use-case is a single primary window.  Callers that
     * need multi-window support should call XUniqueContext() once at init and
     * store the result globally. */
    XSaveContext(s_display, win->xwin, 0,
                 (XPointer)win);

    if (flags & CROSSOS_WINDOW_FULLSCREEN)
        x11_window_set_fullscreen(win, 1);

    /* Attached surface */
    win->surface = calloc(1, sizeof(*win->surface));
    if (win->surface)
        win->surface->win = win;

    if (!(flags & CROSSOS_WINDOW_HIDDEN))
        XMapWindow(s_display, win->xwin);

    XFlush(s_display);
    return win;
}

static void x11_window_destroy(crossos_window_t *win)
{
    if (!win)
        return;
    free(win->surface);
    if (win->ximage)
    {
        win->ximage->data = NULL; /* we own fb_pixels */
        XDestroyImage(win->ximage);
    }
    if (win->gc)
        XFreeGC(s_display, win->gc);
    if (win->xwin)
        XDestroyWindow(s_display, win->xwin);
    free(win->fb_pixels);
    free(win);
}

static void x11_window_show(crossos_window_t *win)
{
    if (win && s_display)
    {
        XMapWindow(s_display, win->xwin);
        XFlush(s_display);
    }
}

static void x11_window_hide(crossos_window_t *win)
{
    if (win && s_display)
    {
        XUnmapWindow(s_display, win->xwin);
        XFlush(s_display);
    }
}

static crossos_result_t x11_window_set_fullscreen(crossos_window_t *win, int fs)
{
    if (!win)
        return CROSSOS_ERR_PARAM;
    win->is_fullscreen = fs;
    XClientMessageEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ClientMessage;
    ev.window = win->xwin;
    ev.message_type = s_net_wm_state;
    ev.format = 32;
    ev.data.l[0] = fs ? 1 : 0; /* _NET_WM_STATE_ADD / REMOVE */
    ev.data.l[1] = (long)s_net_wm_state_fullscreen;
    XSendEvent(s_display, RootWindow(s_display, s_screen), False,
               SubstructureNotifyMask | SubstructureRedirectMask,
               (XEvent *)&ev);
    XFlush(s_display);
    return CROSSOS_OK;
}

static crossos_result_t x11_window_resize(crossos_window_t *win, int w, int h)
{
    if (!win)
        return CROSSOS_ERR_PARAM;
    XResizeWindow(s_display, win->xwin, (unsigned)w, (unsigned)h);
    XFlush(s_display);
    return CROSSOS_OK;
}

static crossos_result_t x11_window_set_title(crossos_window_t *win, const char *t)
{
    if (!win)
        return CROSSOS_ERR_PARAM;
    XStoreName(s_display, win->xwin, t ? t : "");
    XFlush(s_display);
    return CROSSOS_OK;
}

static void x11_window_get_size(const crossos_window_t *win, int *w, int *h)
{
    if (!win)
    {
        if (w)
            *w = 0;
        if (h)
            *h = 0;
        return;
    }
    if (w)
        *w = win->width;
    if (h)
        *h = win->height;
}

static int x11_window_is_fullscreen(const crossos_window_t *win)
{
    return win ? win->is_fullscreen : 0;
}

static void *x11_window_get_native_handle(const crossos_window_t *win)
{
    return win ? (void *)(uintptr_t)win->xwin : NULL;
}

/* ── Display information ──────────────────────────────────────────────── */

static crossos_result_t x11_display_get_size(int idx, int *w, int *h)
{
    if (!s_display || idx != 0)
        return CROSSOS_ERR_PARAM;
    if (w)
        *w = DisplayWidth(s_display, s_screen);
    if (h)
        *h = DisplayHeight(s_display, s_screen);
    return CROSSOS_OK;
}

static int x11_display_count(void)
{
    if (!s_display)
        return 0;
    int nscreens = ScreenCount(s_display);
    return nscreens > 0 ? nscreens : 1;
}

/* ── Surface / framebuffer ────────────────────────────────────────────── */

static crossos_surface_t *x11_surface_get(crossos_window_t *win)
{
    return win ? win->surface : NULL;
}

static crossos_result_t x11_surface_lock(crossos_surface_t *surf,
                                         crossos_framebuffer_t *fb)
{
    if (!surf || !fb)
        return CROSSOS_ERR_PARAM;
    if (surf->locked)
        return CROSSOS_ERR_DISPLAY;
    surf->locked = 1;
    crossos_window_t *win = surf->win;
    fb->pixels = win->fb_pixels;
    fb->width = win->width;
    fb->height = win->height;
    fb->stride = win->ximage ? win->ximage->bytes_per_line : win->width * 4;
    fb->format = CROSSOS_PIXEL_FMT_BGRA8888; /* X11 ZPixmap on little-endian */
    return CROSSOS_OK;
}

static void x11_surface_unlock(crossos_surface_t *surf)
{
    if (surf)
        surf->locked = 0;
}

static crossos_result_t x11_surface_present(crossos_surface_t *surf)
{
    if (!surf || !surf->win)
        return CROSSOS_ERR_PARAM;
    crossos_window_t *win = surf->win;
    if (!win->ximage)
        return CROSSOS_ERR_DISPLAY;
    XPutImage(s_display, win->xwin, win->gc, win->ximage,
              0, 0, 0, 0, (unsigned)win->width, (unsigned)win->height);
    XFlush(s_display);
    return CROSSOS_OK;
}

const crossos_linux_backend_vtable_t crossos__linux_x11_backend = {
    x11_platform_init,
    x11_platform_shutdown,
    x11_window_create,
    x11_window_destroy,
    x11_window_show,
    x11_window_hide,
    x11_window_set_fullscreen,
    x11_window_resize,
    x11_window_set_title,
    x11_window_get_size,
    x11_window_is_fullscreen,
    x11_window_get_native_handle,
    x11_display_get_size,
    x11_display_count,
    x11_surface_get,
    x11_surface_lock,
    x11_surface_unlock,
    x11_surface_present,
    x11_poll_event,
    x11_wait_event,
    x11_run_loop,
    x11_touch_get_active,
    x11_touch_is_supported,
};

#endif /* __linux__ && !__ANDROID__ */
