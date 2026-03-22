/**
 * platform/android/input_android.c  –  Android touch and input event handling.
 *
 * Processes AInputEvent touch events forwarded from the android_app_glue
 * main loop and translates them into crossos_event_t.
 */

#ifdef __ANDROID__

#include <crossos/crossos.h>

#include "android_internal.h"

#include <android/input.h>
#include <android/log.h>
#include <android_native_app_glue.h>

#include <string.h>
#include <stdlib.h>

#define LOG_TAG "CrossOS"

/* ── External references ──────────────────────────────────────────────── */
/* s_app is declared extern in android_internal.h */
extern volatile int         crossos__quit_requested;
extern void                 crossos__set_error(const char *fmt, ...);

/* ── Event queue ──────────────────────────────────────────────────────── */

#define QUEUE_CAP 256

static crossos_event_t s_queue[QUEUE_CAP];
static int             s_head = 0;
static int             s_tail = 0;

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

/* ── AInputEvent → crossos_event ─────────────────────────────────────── */

static void process_input_event(AInputEvent *aev)
{
    if (AInputEvent_getType(aev) != AINPUT_EVENT_TYPE_MOTION) return;

    int32_t action     = AMotionEvent_getAction(aev);
    int32_t action_idx = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)
                         >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    int32_t masked     = action & AMOTION_EVENT_ACTION_MASK;

    crossos_event_t ev;
    memset(&ev, 0, sizeof(ev));

    switch (masked) {
    case AMOTION_EVENT_ACTION_DOWN:
    case AMOTION_EVENT_ACTION_POINTER_DOWN:
        ev.type = CROSSOS_EVENT_TOUCH_BEGIN;  break;
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_POINTER_UP:
        ev.type = CROSSOS_EVENT_TOUCH_END;    break;
    case AMOTION_EVENT_ACTION_MOVE:
        ev.type = CROSSOS_EVENT_TOUCH_UPDATE; break;
    case AMOTION_EVENT_ACTION_CANCEL:
        ev.type = CROSSOS_EVENT_TOUCH_CANCEL; break;
    default: return;
    }

    int32_t count = (int32_t)AMotionEvent_getPointerCount(aev);
    if (count > CROSSOS_MAX_TOUCH_POINTS) count = CROSSOS_MAX_TOUCH_POINTS;
    ev.touch.count = (int)count;

    for (int32_t i = 0; i < count; i++) {
        ev.touch.points[i].id       = AMotionEvent_getPointerId(aev, (size_t)i);
        ev.touch.points[i].x        = AMotionEvent_getX(aev, (size_t)i);
        ev.touch.points[i].y        = AMotionEvent_getY(aev, (size_t)i);
        ev.touch.points[i].pressure = AMotionEvent_getPressure(aev, (size_t)i);
    }

    (void)action_idx; /* used to identify which pointer triggered the event */
    crossos__push_event(&ev);
}

/* ── android_app command / input callbacks ───────────────────────────── */

static void handle_cmd(struct android_app *app, int32_t cmd)
{
    crossos_event_t ev;
    memset(&ev, 0, sizeof(ev));

    switch (cmd) {
    case APP_CMD_DESTROY:
    case APP_CMD_TERM_WINDOW:
        ev.type = CROSSOS_EVENT_WINDOW_CLOSE;
        crossos__push_event(&ev);
        break;
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONFIG_CHANGED:
        if (app->window) {
            ev.type          = CROSSOS_EVENT_WINDOW_RESIZE;
            ev.resize.width  = ANativeWindow_getWidth(app->window);
            ev.resize.height = ANativeWindow_getHeight(app->window);
            crossos__push_event(&ev);
        }
        break;
    case APP_CMD_GAINED_FOCUS:
        ev.type = CROSSOS_EVENT_WINDOW_FOCUS;
        crossos__push_event(&ev);
        break;
    case APP_CMD_LOST_FOCUS:
        ev.type = CROSSOS_EVENT_WINDOW_BLUR;
        crossos__push_event(&ev);
        break;
    }
}

static int32_t handle_input(struct android_app *app, AInputEvent *aev)
{
    (void)app;
    process_input_event(aev);
    return 1; /* consumed */
}

/* ── Public input API ─────────────────────────────────────────────────── */

int crossos_poll_event(crossos_event_t *ev)
{
    if (!ev) return 0;

    /* Let android_app_glue dispatch pending callbacks */
    if (s_app) {
        int events;
        struct android_poll_source *source;
        s_app->onAppCmd    = handle_cmd;
        s_app->onInputEvent = handle_input;

        while (ALooper_pollAll(0, NULL, &events, (void **)&source) >= 0) {
            if (source) source->process(s_app, source);
            if (s_app->destroyRequested) { crossos__quit_requested = 1; break; }
        }
    }

    if (crossos__quit_requested && s_tail == s_head) {
        ev->type = CROSSOS_EVENT_QUIT;
        return 1;
    }
    return pop_event(ev);
}

int crossos_wait_event(crossos_event_t *ev)
{
    if (!ev) return 0;
    for (;;) {
        if (pop_event(ev)) return (ev->type != CROSSOS_EVENT_QUIT);

        if (s_app) {
            int events;
            struct android_poll_source *source;
            s_app->onAppCmd     = handle_cmd;
            s_app->onInputEvent = handle_input;

            if (ALooper_pollAll(-1, NULL, &events, (void **)&source) >= 0) {
                if (source) source->process(s_app, source);
                if (s_app->destroyRequested) {
                    crossos__quit_requested = 1;
                    ev->type = CROSSOS_EVENT_QUIT;
                    return 0;
                }
            }
        }
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
                              crossos_touch_point_t    pts[CROSSOS_MAX_TOUCH_POINTS])
{
    (void)win; (void)pts;
    return 0; /* Snapshot not available via NDK; use events */
}

int crossos_touch_is_supported(void)
{
    return 1; /* Android devices always have a touch screen */
}

#endif /* __ANDROID__ */
