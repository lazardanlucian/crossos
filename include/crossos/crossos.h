/**
 * crossos/crossos.h  –  Main umbrella header for the CrossOS SDK.
 *
 * Including this single header is sufficient to access the entire public API.
 *
 * Quick-start:
 *
 *   #include <crossos/crossos.h>
 *
 *   static void on_event(const crossos_event_t *ev, void *ud) {
 *       if (ev->type == CROSSOS_EVENT_QUIT ||
 *           ev->type == CROSSOS_EVENT_WINDOW_CLOSE) {
 *           crossos_quit();
 *       }
 *   }
 *
 *   int main(void) {
 *       crossos_init();
 *       crossos_window_t *win = crossos_window_create("Hello", 800, 600, 0);
 *       crossos_window_show(win);
 *       crossos_run_loop(win, on_event, NULL);
 *       crossos_window_destroy(win);
 *       crossos_shutdown();
 *       return 0;
 *   }
 */

#ifndef CROSSOS_H
#define CROSSOS_H

/* Pull in all sub-headers so the application only needs one include. */
#include "types.h"
#include "window.h"
#include "input.h"
#include "display.h"
#include "draw.h"
#include "ui.h"
#include "file.h"
#include "dialog.h"
#include "web.h"
#include "audio.h"
#include "optical.h"
#include "bluetooth.h"
#include "image.h"
#include "camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── SDK version ──────────────────────────────────────────────────────── */

#define CROSSOS_VERSION_MAJOR 0
#define CROSSOS_VERSION_MINOR 3
#define CROSSOS_VERSION_PATCH 0

#define CROSSOS_VERSION_STRING "0.3.0"

/* ── SDK lifecycle ────────────────────────────────────────────────────── */

/**
 * Initialise the CrossOS SDK.
 *
 * Must be called once before any other CrossOS function.  On Android this
 * is called automatically from the JNI bridge; applications should still
 * call it to be portable.
 *
 * @return CROSSOS_OK on success, or a negative error code.
 */
crossos_result_t crossos_init(void);

/**
 * Signal the event loop to terminate on next iteration.
 *
 * Safe to call from any thread, including an event callback.
 * After calling this function, crossos_run_loop() will return on its next
 * pass, and crossos_poll_event() / crossos_wait_event() will return 0.
 */
void crossos_quit(void);

/**
 * Shut down the CrossOS SDK and release all platform resources.
 *
 * Call this after the event loop has returned and all windows have been
 * destroyed.  After this point no other CrossOS function may be called
 * until crossos_init() is invoked again.
 */
void crossos_shutdown(void);

/* ── Diagnostics ──────────────────────────────────────────────────────── */

/**
 * Returns a human-readable string describing the last error that occurred
 * on the calling thread, or an empty string if no error is recorded.
 *
 * The pointer is valid until the next CrossOS call on this thread.
 */
const char *crossos_get_error(void);

/**
 * Returns the name of the platform the SDK was compiled for.
 * One of: "windows", "linux", "android".
 */
const char *crossos_platform_name(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CROSSOS_H */
