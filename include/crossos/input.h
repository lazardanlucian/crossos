/**
 * crossos/input.h  –  Input and event polling API.
 *
 * The SDK delivers all input through a unified event queue.  Callers can
 * either poll events one-at-a-time (crossos_poll_event) or register a
 * callback and hand control to the SDK's run-loop (crossos_run_loop).
 *
 * Touch-specific helpers allow querying the current multi-touch state
 * without going through the event queue.
 */

#ifndef CROSSOS_INPUT_H
#define CROSSOS_INPUT_H

#include "types.h"
#include "window.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Platform-independent virtual key codes ──────────────────────────── */

typedef enum crossos_key {
    CROSSOS_KEY_UNKNOWN = 0,

    /* Printable ASCII range (codes match ASCII values where possible) */
    CROSSOS_KEY_SPACE     = 32,
    CROSSOS_KEY_APOSTROPHE= 39,
    CROSSOS_KEY_COMMA     = 44,
    CROSSOS_KEY_MINUS     = 45,
    CROSSOS_KEY_PERIOD    = 46,
    CROSSOS_KEY_SLASH     = 47,
    CROSSOS_KEY_0 = 48, CROSSOS_KEY_1, CROSSOS_KEY_2, CROSSOS_KEY_3,
    CROSSOS_KEY_4, CROSSOS_KEY_5, CROSSOS_KEY_6, CROSSOS_KEY_7,
    CROSSOS_KEY_8, CROSSOS_KEY_9,
    CROSSOS_KEY_SEMICOLON = 59,
    CROSSOS_KEY_EQUAL     = 61,
    CROSSOS_KEY_A = 65, CROSSOS_KEY_B, CROSSOS_KEY_C, CROSSOS_KEY_D,
    CROSSOS_KEY_E, CROSSOS_KEY_F, CROSSOS_KEY_G, CROSSOS_KEY_H,
    CROSSOS_KEY_I, CROSSOS_KEY_J, CROSSOS_KEY_K, CROSSOS_KEY_L,
    CROSSOS_KEY_M, CROSSOS_KEY_N, CROSSOS_KEY_O, CROSSOS_KEY_P,
    CROSSOS_KEY_Q, CROSSOS_KEY_R, CROSSOS_KEY_S, CROSSOS_KEY_T,
    CROSSOS_KEY_U, CROSSOS_KEY_V, CROSSOS_KEY_W, CROSSOS_KEY_X,
    CROSSOS_KEY_Y, CROSSOS_KEY_Z,

    /* Function keys */
    CROSSOS_KEY_F1  = 290, CROSSOS_KEY_F2,  CROSSOS_KEY_F3,  CROSSOS_KEY_F4,
    CROSSOS_KEY_F5,        CROSSOS_KEY_F6,  CROSSOS_KEY_F7,  CROSSOS_KEY_F8,
    CROSSOS_KEY_F9,        CROSSOS_KEY_F10, CROSSOS_KEY_F11, CROSSOS_KEY_F12,

    /* Navigation */
    CROSSOS_KEY_ESCAPE    = 256,
    CROSSOS_KEY_ENTER     = 257,
    CROSSOS_KEY_TAB       = 258,
    CROSSOS_KEY_BACKSPACE = 259,
    CROSSOS_KEY_INSERT    = 260,
    CROSSOS_KEY_DELETE    = 261,
    CROSSOS_KEY_RIGHT     = 262,
    CROSSOS_KEY_LEFT      = 263,
    CROSSOS_KEY_DOWN      = 264,
    CROSSOS_KEY_UP        = 265,
    CROSSOS_KEY_PAGE_UP   = 266,
    CROSSOS_KEY_PAGE_DOWN = 267,
    CROSSOS_KEY_HOME      = 268,
    CROSSOS_KEY_END       = 269,

    /* Modifiers */
    CROSSOS_KEY_LEFT_SHIFT  = 340,
    CROSSOS_KEY_LEFT_CTRL   = 341,
    CROSSOS_KEY_LEFT_ALT    = 342,
    CROSSOS_KEY_LEFT_SUPER  = 343,
    CROSSOS_KEY_RIGHT_SHIFT = 344,
    CROSSOS_KEY_RIGHT_CTRL  = 345,
    CROSSOS_KEY_RIGHT_ALT   = 346,
    CROSSOS_KEY_RIGHT_SUPER = 347,
} crossos_key_t;

/* ── Event queue ──────────────────────────────────────────────────────── */

/**
 * Poll for a single pending event.
 *
 * Fills @p event and returns non-zero if an event was available;
 * returns 0 and leaves @p event untouched if the queue is empty.
 *
 * This function never blocks.
 */
int crossos_poll_event(crossos_event_t *event);

/**
 * Block until an event is available, then fill @p event and return.
 *
 * Returns non-zero on success; returns 0 if the loop should be terminated
 * (e.g., CROSSOS_EVENT_QUIT was received).
 */
int crossos_wait_event(crossos_event_t *event);

/**
 * Run the platform event loop until a CROSSOS_EVENT_QUIT (or window-close)
 * event is received, calling @p cb for every event.
 *
 * This call blocks.  Control returns only after the loop ends.
 *
 * @param win        Primary window (used for focus handling and resize); may
 *                   be NULL when managing multiple windows.
 * @param cb         Callback invoked for every event; must not be NULL.
 * @param user_data  Opaque pointer forwarded unchanged to @p cb.
 */
void crossos_run_loop(crossos_window_t    *win,
                      crossos_event_cb_t   cb,
                      void                *user_data);

/* ── Touch queries ────────────────────────────────────────────────────── */

/**
 * Copy the current multi-touch state into @p points.
 *
 * @param win        Window to query.
 * @param points     Output array with room for at least CROSSOS_MAX_TOUCH_POINTS
 *                   entries.
 * @return           Number of active touch contacts (0 … CROSSOS_MAX_TOUCH_POINTS).
 *
 * Returns 0 on platforms without a touch screen or when no fingers are down.
 */
int crossos_touch_get_active(const crossos_window_t  *win,
                              crossos_touch_point_t    points[CROSSOS_MAX_TOUCH_POINTS]);

/**
 * Returns non-zero if the current platform / device has a touch screen.
 */
int crossos_touch_is_supported(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CROSSOS_INPUT_H */
