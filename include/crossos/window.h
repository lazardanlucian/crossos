/**
 * crossos/window.h  –  Window management API.
 *
 * A "window" is the top-level drawable surface presented to the user.  On
 * desktop platforms it maps to a native OS window; on Android it maps to the
 * ANativeWindow / SurfaceView provided by the activity.
 */

#ifndef CROSSOS_WINDOW_H
#define CROSSOS_WINDOW_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Window creation flags ────────────────────────────────────────────── */

typedef enum crossos_window_flags {
    CROSSOS_WINDOW_NONE       = 0x00,
    CROSSOS_WINDOW_RESIZABLE  = 0x01, /**< Allow the user to resize the window */
    CROSSOS_WINDOW_FULLSCREEN = 0x02, /**< Start in full-screen mode           */
    CROSSOS_WINDOW_BORDERLESS = 0x04, /**< Remove title-bar / decorations      */
    CROSSOS_WINDOW_HIDDEN     = 0x08, /**< Create invisible; call _show later  */
} crossos_window_flags_t;

/* ── Window lifecycle ─────────────────────────────────────────────────── */

/**
 * Create a new window.
 *
 * @param title   UTF-8 window title (ignored on Android).
 * @param width   Requested client-area width in pixels.
 * @param height  Requested client-area height in pixels.
 * @param flags   Bitwise-OR of crossos_window_flags_t values.
 * @return        Opaque window handle, or NULL on failure.
 *
 * @note crossos_init() must be called before the first crossos_window_create().
 */
crossos_window_t *crossos_window_create(const char *title,
                                        int         width,
                                        int         height,
                                        uint32_t    flags);

/**
 * Destroy a window and release all associated OS resources.
 *
 * After this call @p win must not be used.
 */
void crossos_window_destroy(crossos_window_t *win);

/** Make the window visible (no-op if already visible). */
void crossos_window_show(crossos_window_t *win);

/** Hide the window without destroying it. */
void crossos_window_hide(crossos_window_t *win);

/** Request the window to enter full-screen mode. */
crossos_result_t crossos_window_set_fullscreen(crossos_window_t *win, int fullscreen);

/**
 * Resize the window's client area.
 *
 * Has no effect on Android where the surface size is controlled by the OS.
 */
crossos_result_t crossos_window_resize(crossos_window_t *win, int width, int height);

/** Update the window title bar text (no-op on Android). */
crossos_result_t crossos_window_set_title(crossos_window_t *win, const char *title);

/* ── Window queries ───────────────────────────────────────────────────── */

/** Returns the current client-area dimensions through the out-parameters. */
void crossos_window_get_size(const crossos_window_t *win, int *width, int *height);

/**
 * Returns non-zero if the window is currently in full-screen mode.
 */
int crossos_window_is_fullscreen(const crossos_window_t *win);

/* ── Native handle escape hatch ──────────────────────────────────────── */

/**
 * Retrieve the underlying platform window handle for interoperability with
 * native rendering APIs (Vulkan surface, OpenGL context, etc.).
 *
 * Platform mapping:
 *  - Windows : returns HWND cast to (void*)
 *  - Linux   : returns (Window) XID cast to (void*)
 *  - Android : returns ANativeWindow*
 *
 * @return Platform-specific handle, or NULL if @p win is NULL.
 */
void *crossos_window_get_native_handle(const crossos_window_t *win);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CROSSOS_WINDOW_H */
