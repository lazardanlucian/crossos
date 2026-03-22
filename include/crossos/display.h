/**
 * crossos/display.h  –  Software framebuffer / display surface API.
 *
 * CrossOS presents a simple software-rendering model: the application writes
 * RGBA pixels into a locked framebuffer and then calls _present() to blit
 * them to the screen.  This deliberately keeps the SDK dependency-free –
 * for hardware-accelerated rendering, retrieve the native window handle via
 * crossos_window_get_native_handle() and create a Vulkan/OpenGL surface
 * directly.
 *
 * Typical usage:
 *
 *   crossos_surface_t *surf = crossos_surface_get(win);
 *
 *   crossos_framebuffer_t fb;
 *   crossos_surface_lock(surf, &fb);
 *   // ... write pixels into fb.pixels ...
 *   crossos_surface_unlock(surf);
 *   crossos_surface_present(surf);
 */

#ifndef CROSSOS_DISPLAY_H
#define CROSSOS_DISPLAY_H

#include "types.h"
#include "window.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Framebuffer descriptor ───────────────────────────────────────────── */

/**
 * Describes a CPU-accessible pixel buffer returned by crossos_surface_lock().
 */
typedef struct crossos_framebuffer {
    void                  *pixels; /**< Pointer to the first pixel             */
    int                    width;  /**< Width  of the buffer in pixels         */
    int                    height; /**< Height of the buffer in pixels         */
    int                    stride; /**< Row stride in *bytes* (>= width * bpp) */
    crossos_pixel_format_t format; /**< Pixel layout                           */
} crossos_framebuffer_t;

/* ── Surface lifecycle ────────────────────────────────────────────────── */

/**
 * Obtain the display surface associated with @p win.
 *
 * The returned pointer is owned by the window and remains valid until
 * crossos_window_destroy() is called.  Do NOT free it yourself.
 *
 * Returns NULL if @p win is NULL or if no surface is available yet (e.g.,
 * on Android before the SurfaceView is ready).
 */
crossos_surface_t *crossos_surface_get(crossos_window_t *win);

/* ── Framebuffer access ───────────────────────────────────────────────── */

/**
 * Lock the surface's framebuffer for CPU write access.
 *
 * @param surf  Surface to lock.
 * @param fb    Out: filled with width, height, stride, pixel pointer and format.
 * @return      CROSSOS_OK on success; CROSSOS_ERR_DISPLAY if the surface
 *              cannot be locked right now (try again next frame).
 *
 * Between lock and unlock no other call on @p surf is permitted.
 */
crossos_result_t crossos_surface_lock(crossos_surface_t   *surf,
                                      crossos_framebuffer_t *fb);

/**
 * Unlock the framebuffer after writing.
 *
 * Must be paired with a successful crossos_surface_lock().
 */
void crossos_surface_unlock(crossos_surface_t *surf);

/**
 * Blit the current framebuffer contents to the screen.
 *
 * On most platforms this is a double-buffer swap or a native blit; the
 * exact timing depends on vsync settings.
 *
 * @return CROSSOS_OK or CROSSOS_ERR_DISPLAY.
 */
crossos_result_t crossos_surface_present(crossos_surface_t *surf);

/* ── Display / monitor information ───────────────────────────────────── */

/**
 * Query the physical display size (full screen, not the window).
 *
 * @param display_index  Zero-based monitor index (0 = primary).
 * @param width          Out: width  in pixels (may be NULL).
 * @param height         Out: height in pixels (may be NULL).
 * @return               CROSSOS_OK, or CROSSOS_ERR_PARAM if the index is
 *                       out of range.
 */
crossos_result_t crossos_display_get_size(int display_index,
                                          int *width,
                                          int *height);

/**
 * Return the number of physical displays/monitors connected.
 * Always returns at least 1 when crossos_init() has succeeded.
 */
int crossos_display_count(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CROSSOS_DISPLAY_H */
