/**
 * platform/windows/input_win32.c  –  Win32 event queue and input.
 *
 * Implements the platform event queue using a fixed ring-buffer so that
 * events produced inside the WndProc (on the GUI thread) can be consumed
 * by crossos_poll_event() / crossos_wait_event() on the same thread.
 */

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <crossos/crossos.h>

#include <string.h>

/* ── External references ──────────────────────────────────────────────── */
extern volatile int crossos__quit_requested;
extern void         crossos__set_error(const char *fmt, ...);

/* ── Event ring-buffer ────────────────────────────────────────────────── */

#define QUEUE_CAP 256

static crossos_event_t s_queue[QUEUE_CAP];
static volatile int    s_head = 0; /* producer (WndProc) writes here */
static volatile int    s_tail = 0; /* consumer (app)    reads here   */

void crossos__push_event(const crossos_event_t *ev)
{
    int next = (s_head + 1) % QUEUE_CAP;
    if (next == s_tail) return; /* queue full – drop incoming event */
    s_queue[s_head] = *ev;
    s_head = next;
}

static int pop_event(crossos_event_t *ev)
{
    if (s_tail == s_head) return 0;
    *ev    = s_queue[s_tail];
    s_tail = (s_tail + 1) % QUEUE_CAP;
    return 1;
}

/* ── Input API ────────────────────────────────────────────────────────── */

int crossos_poll_event(crossos_event_t *ev)
{
    if (!ev) return 0;
    /* Drain the Win32 message queue first so WndProc can fill our ring */
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            crossos__quit_requested = 1;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (crossos__quit_requested && s_tail == s_head) {
        ev->type   = CROSSOS_EVENT_QUIT;
        ev->window = NULL;
        return 1;
    }
    return pop_event(ev);
}

int crossos_wait_event(crossos_event_t *ev)
{
    if (!ev) return 0;
    for (;;) {
        if (crossos_poll_event(ev)) return (ev->type != CROSSOS_EVENT_QUIT);
        WaitMessage();
    }
}

void crossos_run_loop(crossos_window_t    *win,
                      crossos_event_cb_t   cb,
                      void                *user_data)
{
    (void)win;
    crossos_event_t ev;
    while (!crossos__quit_requested) {
        if (!crossos_wait_event(&ev)) break;
        if (ev.type == CROSSOS_EVENT_QUIT ||
            ev.type == CROSSOS_EVENT_WINDOW_CLOSE) {
            crossos__quit_requested = 1;
        }
        if (cb) cb(&ev, user_data);
    }
}

/* ── Touch queries ────────────────────────────────────────────────────── */

int crossos_touch_get_active(const crossos_window_t  *win,
                              crossos_touch_point_t    points[CROSSOS_MAX_TOUCH_POINTS])
{
    (void)win;
    (void)points;
    /* Instantaneous touch state snapshot is not exposed by Win32;
     * use the event stream instead. */
    return 0;
}

int crossos_touch_is_supported(void)
{
    return (GetSystemMetrics(SM_DIGITIZER) & NID_READY) ? 1 : 0;
}

#endif /* _WIN32 */
