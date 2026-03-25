/**
 * platform/linux/input_x11.c  –  X11 event queue and XInput2 touch input.
 */

#if defined(__linux__) && !defined(__ANDROID__)

#include <crossos/crossos.h>

#include "linux_backend.h"
#include "x11_internal.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XInput2.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/input.h> /* for touch-device probing */
#include <dirent.h>
#include <sys/ioctl.h>

/* ── External references ──────────────────────────────────────────────── */
extern volatile int crossos__quit_requested;
extern void crossos__set_error(const char *fmt, ...);

/* ── Module-level X11 state ──────────────────────────────────────────── */
extern Display *s_display; /* defined in window_x11.c */
extern Atom s_wm_delete_window;

/* ── Xdnd atoms ──────────────────────────────────────────────────────── */
Atom s_xdnd_aware;
Atom s_xdnd_enter;
Atom s_xdnd_position;
Atom s_xdnd_status;
Atom s_xdnd_drop;
Atom s_xdnd_finished;
Atom s_xdnd_selection;
Atom s_xdnd_type_list;
Atom s_uri_list;
static Window s_xdnd_src = None;
static unsigned long s_xdnd_src_ver = 0;

/* Static drop buffer */
#define DROP_MAX_FILES 32
#define DROP_PATH_BUF 512
static char s_drop_bufs[DROP_MAX_FILES][DROP_PATH_BUF];
static const char *s_drop_ptrs[DROP_MAX_FILES];
static int s_drop_count = 0;

/* ── Event queue ring-buffer ──────────────────────────────────────────── */

#define QUEUE_CAP 256

static crossos_event_t s_queue[QUEUE_CAP];
static int s_head = 0;
static int s_tail = 0;

void crossos__push_event(const crossos_event_t *ev)
{
    int next = (s_head + 1) % QUEUE_CAP;
    if (next == s_tail)
        return; /* queue full – drop incoming event */
    s_queue[s_head] = *ev;
    s_head = next;
}

static int pop_event(crossos_event_t *ev)
{
    if (s_tail == s_head)
        return 0;
    *ev = s_queue[s_tail];
    s_tail = (s_tail + 1) % QUEUE_CAP;
    return 1;
}

/* ── XEvent → crossos_event ──────────────────────────────────────────── */

static crossos_key_mod_t x_state_to_mods(unsigned int state)
{
    crossos_key_mod_t m = CROSSOS_MOD_NONE;
    if (state & ShiftMask)
        m |= CROSSOS_MOD_SHIFT;
    if (state & ControlMask)
        m |= CROSSOS_MOD_CTRL;
    if (state & Mod1Mask)
        m |= CROSSOS_MOD_ALT;
    if (state & Mod4Mask)
        m |= CROSSOS_MOD_SUPER;
    return m;
}

/* Map a KeySym to a CrossOS key code (subset). */
static int keysym_to_crossos(KeySym ks)
{
    if (ks >= XK_space && ks <= XK_asciitilde)
        return (int)ks;
    switch (ks)
    {
    case XK_Escape:
        return CROSSOS_KEY_ESCAPE;
    case XK_Return:
        return CROSSOS_KEY_ENTER;
    case XK_Tab:
        return CROSSOS_KEY_TAB;
    case XK_BackSpace:
        return CROSSOS_KEY_BACKSPACE;
    case XK_Insert:
        return CROSSOS_KEY_INSERT;
    case XK_Delete:
        return CROSSOS_KEY_DELETE;
    case XK_Right:
        return CROSSOS_KEY_RIGHT;
    case XK_Left:
        return CROSSOS_KEY_LEFT;
    case XK_Down:
        return CROSSOS_KEY_DOWN;
    case XK_Up:
        return CROSSOS_KEY_UP;
    case XK_Page_Up:
        return CROSSOS_KEY_PAGE_UP;
    case XK_Page_Down:
        return CROSSOS_KEY_PAGE_DOWN;
    case XK_Home:
        return CROSSOS_KEY_HOME;
    case XK_End:
        return CROSSOS_KEY_END;
    case XK_F1:
    case XK_F2:
    case XK_F3:
    case XK_F4:
    case XK_F5:
    case XK_F6:
    case XK_F7:
    case XK_F8:
    case XK_F9:
    case XK_F10:
    case XK_F11:
    case XK_F12:
        return CROSSOS_KEY_F1 + (int)(ks - XK_F1);
    case XK_Shift_L:
        return CROSSOS_KEY_LEFT_SHIFT;
    case XK_Shift_R:
        return CROSSOS_KEY_RIGHT_SHIFT;
    case XK_Control_L:
        return CROSSOS_KEY_LEFT_CTRL;
    case XK_Control_R:
        return CROSSOS_KEY_RIGHT_CTRL;
    case XK_Alt_L:
        return CROSSOS_KEY_LEFT_ALT;
    case XK_Alt_R:
        return CROSSOS_KEY_RIGHT_ALT;
    case XK_Super_L:
        return CROSSOS_KEY_LEFT_SUPER;
    case XK_Super_R:
        return CROSSOS_KEY_RIGHT_SUPER;
    default:
        return CROSSOS_KEY_UNKNOWN;
    }
}

static void process_xevent(XEvent *xev)
{
    crossos_event_t ev;
    memset(&ev, 0, sizeof(ev));

    /* NOTE: ev.window is left NULL in this implementation.  A full
     * multi-window implementation would call XFindContext() here to look up
     * the crossos_window_t* associated with xev->xany.window.  For the
     * single-window case this is sufficient as the caller already holds the
     * window handle. */

    switch (xev->type)
    {
    case ClientMessage:
    {
        Atom msg = (Atom)xev->xclient.message_type;
        if ((Atom)xev->xclient.data.l[0] == s_wm_delete_window)
        {
            ev.type = CROSSOS_EVENT_WINDOW_CLOSE;
            crossos__push_event(&ev);
        }
        else if (msg == s_xdnd_enter)
        {
            s_xdnd_src = (Window)xev->xclient.data.l[0];
            s_xdnd_src_ver = (unsigned long)(xev->xclient.data.l[1] >> 24);
        }
        else if (msg == s_xdnd_position)
        {
            /* Accept the drop by sending XdndStatus */
            XClientMessageEvent status;
            memset(&status, 0, sizeof(status));
            status.type = ClientMessage;
            status.display = s_display;
            status.window = s_xdnd_src;
            status.message_type = s_xdnd_status;
            status.format = 32;
            status.data.l[0] = xev->xclient.window;
            status.data.l[1] = 1; /* accept */
            status.data.l[4] = (long)XInternAtom(s_display, "XdndActionCopy", False);
            XSendEvent(s_display, s_xdnd_src, False, 0, (XEvent *)&status);
            XFlush(s_display);
        }
        else if (msg == s_xdnd_drop)
        {
            /* Request the selection to get file URIs */
            Time drop_time = (Time)(unsigned long)xev->xclient.data.l[2];
            XConvertSelection(s_display, s_xdnd_selection, s_uri_list,
                              s_uri_list, xev->xclient.window, drop_time);
        }
        break;
    }
    case SelectionNotify:
    {
        if (xev->xselection.property != None)
        {
            Atom actual_type;
            int actual_fmt;
            unsigned long nitems, bytes_after;
            unsigned char *data = NULL;
            XGetWindowProperty(s_display, xev->xselection.requestor,
                               xev->xselection.property,
                               0, 65536, True, AnyPropertyType,
                               &actual_type, &actual_fmt,
                               &nitems, &bytes_after, &data);
            if (data)
            {
                /* Parse newline-separated "file:///path" URIs */
                s_drop_count = 0;
                char *p = (char *)data;
                while (*p && s_drop_count < DROP_MAX_FILES)
                {
                    /* Skip whitespace / \r */
                    while (*p == '\r' || *p == '\n')
                        p++;
                    if (!*p)
                        break;
                    /* Find end of line */
                    char *end = p;
                    while (*end && *end != '\r' && *end != '\n')
                        end++;
                    int len = (int)(end - p);
                    /* Strip "file://" prefix */
                    const char *path = p;
                    if (len > 7 && memcmp(p, "file://", 7) == 0)
                    {
                        path = p + 7;
                        len -= 7;
                    }
                    if (len > 0 && len < DROP_PATH_BUF)
                    {
                        memcpy(s_drop_bufs[s_drop_count], path, (size_t)len);
                        s_drop_bufs[s_drop_count][len] = '\0';
                        s_drop_ptrs[s_drop_count] = s_drop_bufs[s_drop_count];
                        s_drop_count++;
                    }
                    p = end;
                }
                XFree(data);

                if (s_drop_count > 0)
                {
                    crossos_event_t dev;
                    memset(&dev, 0, sizeof(dev));
                    dev.type = CROSSOS_EVENT_DROP_FILES;
                    dev.drop.count = s_drop_count;
                    dev.drop.paths = s_drop_ptrs;
                    crossos__push_event(&dev);
                }

                /* Send XdndFinished */
                if (s_xdnd_src != None)
                {
                    XClientMessageEvent fin;
                    memset(&fin, 0, sizeof(fin));
                    fin.type = ClientMessage;
                    fin.display = s_display;
                    fin.window = s_xdnd_src;
                    fin.message_type = s_xdnd_finished;
                    fin.format = 32;
                    fin.data.l[0] = xev->xselection.requestor;
                    fin.data.l[1] = 1;
                    fin.data.l[2] = (long)XInternAtom(s_display, "XdndActionCopy", False);
                    XSendEvent(s_display, s_xdnd_src, False, 0, (XEvent *)&fin);
                    XFlush(s_display);
                    s_xdnd_src = None;
                }
            }
        }
        break;
    }
    case ConfigureNotify:
    {
        ev.type = CROSSOS_EVENT_WINDOW_RESIZE;
        ev.resize.width = xev->xconfigure.width;
        ev.resize.height = xev->xconfigure.height;
        crossos__push_event(&ev);
        break;
    }
    case FocusIn:
        ev.type = CROSSOS_EVENT_WINDOW_FOCUS;
        crossos__push_event(&ev);
        break;
    case FocusOut:
        ev.type = CROSSOS_EVENT_WINDOW_BLUR;
        crossos__push_event(&ev);
        break;

    case KeyPress:
    {
        KeySym ks = XkbKeycodeToKeysym(s_display, (KeyCode)xev->xkey.keycode,
                                       0, 0);
        ev.type = CROSSOS_EVENT_KEY_DOWN;
        ev.key.keycode = keysym_to_crossos(ks);
        ev.key.scancode = (int)xev->xkey.keycode;
        ev.key.mods = x_state_to_mods(xev->xkey.state);
        crossos__push_event(&ev);

        /* Also emit a CHAR event for printable characters */
        char cbuf[8];
        memset(cbuf, 0, sizeof(cbuf));
        int cl = XLookupString(&xev->xkey, cbuf, sizeof(cbuf) - 1, NULL, NULL);
        if (cl > 0 && (unsigned char)cbuf[0] >= 32 && (unsigned char)cbuf[0] != 127)
        {
            crossos_event_t cev;
            memset(&cev, 0, sizeof(cev));
            cev.type = CROSSOS_EVENT_CHAR;
            cev.character.codepoint = (unsigned)(unsigned char)cbuf[0];
            crossos__push_event(&cev);
        }
        break;
    }
    case KeyRelease:
    {
        KeySym ks = XkbKeycodeToKeysym(s_display, (KeyCode)xev->xkey.keycode,
                                       0, 0);
        ev.type = CROSSOS_EVENT_KEY_UP;
        ev.key.keycode = keysym_to_crossos(ks);
        ev.key.scancode = (int)xev->xkey.keycode;
        ev.key.mods = x_state_to_mods(xev->xkey.state);
        crossos__push_event(&ev);
        break;
    }

    case ButtonPress:
        if (xev->xbutton.button == Button4)
        {
            ev.type = CROSSOS_EVENT_POINTER_SCROLL;
            ev.pointer.x = (float)xev->xbutton.x;
            ev.pointer.y = (float)xev->xbutton.y;
            ev.pointer.scroll_y = 1.0f;
            crossos__push_event(&ev);
        }
        else if (xev->xbutton.button == Button5)
        {
            ev.type = CROSSOS_EVENT_POINTER_SCROLL;
            ev.pointer.x = (float)xev->xbutton.x;
            ev.pointer.y = (float)xev->xbutton.y;
            ev.pointer.scroll_y = -1.0f;
            crossos__push_event(&ev);
        }
        else
        {
            ev.type = CROSSOS_EVENT_POINTER_DOWN;
            ev.pointer.x = (float)xev->xbutton.x;
            ev.pointer.y = (float)xev->xbutton.y;
            ev.pointer.button = (int)xev->xbutton.button;
            crossos__push_event(&ev);
        }
        break;

    case ButtonRelease:
        if (xev->xbutton.button != Button4 &&
            xev->xbutton.button != Button5)
        {
            ev.type = CROSSOS_EVENT_POINTER_UP;
            ev.pointer.x = (float)xev->xbutton.x;
            ev.pointer.y = (float)xev->xbutton.y;
            ev.pointer.button = (int)xev->xbutton.button;
            crossos__push_event(&ev);
        }
        break;

    case MotionNotify:
        ev.type = CROSSOS_EVENT_POINTER_MOVE;
        ev.pointer.x = (float)xev->xmotion.x;
        ev.pointer.y = (float)xev->xmotion.y;
        crossos__push_event(&ev);
        break;

    case GenericEvent:
        if (s_xi2_opcode >= 0 && xev->xcookie.extension == s_xi2_opcode)
        {
            XGetEventData(s_display, &xev->xcookie);
            XIDeviceEvent *xi = (XIDeviceEvent *)xev->xcookie.data;
            crossos_event_t tev;
            memset(&tev, 0, sizeof(tev));
            tev.touch.count = 1;
            tev.touch.points[0].id = (int)xi->detail;
            tev.touch.points[0].x = (float)xi->event_x;
            tev.touch.points[0].y = (float)xi->event_y;
            tev.touch.points[0].pressure = 1.0f;
            switch (xev->xcookie.evtype)
            {
            case XI_TouchBegin:
                tev.type = CROSSOS_EVENT_TOUCH_BEGIN;
                break;
            case XI_TouchUpdate:
                tev.type = CROSSOS_EVENT_TOUCH_UPDATE;
                break;
            case XI_TouchEnd:
                tev.type = CROSSOS_EVENT_TOUCH_END;
                break;
            default:
                XFreeEventData(s_display, &xev->xcookie);
                return;
            }
            crossos__push_event(&tev);
            XFreeEventData(s_display, &xev->xcookie);
        }
        break;

    default:
        break;
    }
}

/* ── Public input API ─────────────────────────────────────────────────── */

int x11_poll_event(crossos_event_t *ev)
{
    if (!ev || !s_display)
        return 0;
    while (XPending(s_display) > 0)
    {
        XEvent xev;
        XNextEvent(s_display, &xev);
        process_xevent(&xev);
    }
    if (crossos__quit_requested && s_tail == s_head)
    {
        ev->type = CROSSOS_EVENT_QUIT;
        return 1;
    }
    return pop_event(ev);
}

int x11_wait_event(crossos_event_t *ev)
{
    if (!ev || !s_display)
        return 0;
    for (;;)
    {
        if (pop_event(ev))
            return (ev->type != CROSSOS_EVENT_QUIT);
        XEvent xev;
        XNextEvent(s_display, &xev);
        process_xevent(&xev);
    }
}

void x11_run_loop(crossos_window_t *win,
                  crossos_event_cb_t cb,
                  void *user_data)
{
    (void)win;
    crossos_event_t ev;
    while (!crossos__quit_requested)
    {
        if (!x11_wait_event(&ev))
            break;
        if (ev.type == CROSSOS_EVENT_QUIT ||
            ev.type == CROSSOS_EVENT_WINDOW_CLOSE)
        {
            crossos__quit_requested = 1;
        }
        if (cb)
            cb(&ev, user_data);
    }
}

/* ── Touch queries ────────────────────────────────────────────────────── */

int x11_touch_get_active(const crossos_window_t *win,
                         crossos_touch_point_t pts[CROSSOS_MAX_TOUCH_POINTS])
{
    (void)win;
    (void)pts;
    return 0; /* Instantaneous snapshot not provided by Xlib; use events */
}

int x11_touch_is_supported(void)
{
    /* Check /dev/input for touch devices */
    DIR *d = opendir("/dev/input");
    if (!d)
        return 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL)
    {
        if (de->d_name[0] == '.')
            continue;
        char path[320];
        snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;
        unsigned long evbit = 0;
        ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), &evbit);
        close(fd);
        if (evbit & (1UL << EV_ABS))
        {
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

#endif /* __linux__ && !__ANDROID__ */
