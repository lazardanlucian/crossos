/**
 * crossos/image.h  –  Image loading and rendering helpers.
 *
 * Supports reading BMP (24-bit or 32-bit uncompressed) and PPM (P6 binary)
 * image files from disk into an in-memory RGBA8888 pixel buffer.  The
 * loaded buffer can then be blitted – with or without scaling – to any
 * CrossOS software framebuffer.
 *
 * No external dependencies; all decoding is performed from scratch.
 *
 * Quick-start:
 *
 *   crossos_image_t *img = NULL;
 *   crossos_image_load_bmp("logo.bmp", &img);
 *
 *   crossos_framebuffer_t fb;
 *   crossos_surface_lock(surf, &fb);
 *   crossos_image_blit(&fb, img, 10, 10);
 *   crossos_surface_unlock(surf);
 *   crossos_surface_present(surf);
 *
 *   crossos_image_destroy(img);
 */

#ifndef CROSSOS_IMAGE_H
#define CROSSOS_IMAGE_H

#include "types.h"
#include "display.h"
#include "draw.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Image descriptor ────────────────────────────────────────────────── */

/**
 * An RGBA8888 image held in host (CPU) memory.
 * pixel[y * stride + x * 4 + 0] = R
 * pixel[y * stride + x * 4 + 1] = G
 * pixel[y * stride + x * 4 + 2] = B
 * pixel[y * stride + x * 4 + 3] = A
 */
typedef struct crossos_image {
    unsigned char *pixels; /**< Pointer to the first pixel (R of row 0, col 0) */
    int            width;  /**< Image width in pixels                           */
    int            height; /**< Image height in pixels                          */
    int            stride; /**< Row stride in bytes (>= width * 4)              */
} crossos_image_t;

/* ── Loading ─────────────────────────────────────────────────────────── */

/**
 * Load an uncompressed BMP file (24-bit or 32-bit) from disk.
 *
 * The image is decoded to RGBA8888.  BMP images with a 24-bit depth are
 * treated as fully opaque; 32-bit images preserve the alpha channel if
 * present (Windows BITMAPV4/V5 headers) or set alpha to 255 otherwise.
 *
 * @param path   Path to the .bmp file.
 * @param out    Receives a newly allocated crossos_image_t on success.
 *               The caller must call crossos_image_destroy() when done.
 * @return       CROSSOS_OK on success; CROSSOS_ERR_IO if the file cannot
 *               be opened; CROSSOS_ERR_PARAM for unsupported BMP variants.
 */
crossos_result_t crossos_image_load_bmp(const char       *path,
                                        crossos_image_t **out);

/**
 * Load a binary PPM file (P6 magic, 8-bit per channel, maxval <= 255).
 *
 * @param path   Path to the .ppm file.
 * @param out    Receives a newly allocated crossos_image_t on success.
 * @return       CROSSOS_OK; CROSSOS_ERR_IO or CROSSOS_ERR_PARAM on failure.
 */
crossos_result_t crossos_image_load_ppm(const char       *path,
                                        crossos_image_t **out);

/* ── Creation / destruction ──────────────────────────────────────────── */

/**
 * Allocate a blank RGBA image.  All pixels are initialised to (0,0,0,0).
 *
 * @param width   Image width in pixels (> 0).
 * @param height  Image height in pixels (> 0).
 * @param out     Receives the allocated image on success.
 * @return        CROSSOS_OK; CROSSOS_ERR_OOM or CROSSOS_ERR_PARAM on error.
 */
crossos_result_t crossos_image_create(int               width,
                                      int               height,
                                      crossos_image_t **out);

/**
 * Free an image returned by crossos_image_load_* or crossos_image_create().
 * Safe to call with NULL.
 */
void crossos_image_destroy(crossos_image_t *img);

/* ── Rendering ───────────────────────────────────────────────────────── */

/**
 * Blit the entire image to the framebuffer at (dst_x, dst_y).
 *
 * Pixels with alpha < 255 are alpha-blended onto the framebuffer.
 * The draw clip stack is respected.
 */
void crossos_image_blit(const crossos_framebuffer_t *fb,
                        const crossos_image_t       *img,
                        int                          dst_x,
                        int                          dst_y);

/**
 * Blit the image scaled to fill the rectangle (dst_x, dst_y, dst_w, dst_h).
 *
 * Nearest-neighbour sampling is used.  Aspect ratio is NOT preserved;
 * the image is stretched to exactly fill the destination rectangle.
 */
void crossos_image_blit_scaled(const crossos_framebuffer_t *fb,
                               const crossos_image_t       *img,
                               int                          dst_x,
                               int                          dst_y,
                               int                          dst_w,
                               int                          dst_h);

/**
 * Blit a rectangular region of the source image to (dst_x, dst_y).
 *
 * Useful for sprite-sheet / texture-atlas workflows.
 * The region is clamped to the image bounds; no scaling is applied.
 *
 * @param src_x,src_y  Top-left of the source region within @p img.
 * @param src_w,src_h  Width/height of the source region.
 * @param dst_x,dst_y  Top-left destination in the framebuffer.
 */
void crossos_image_blit_region(const crossos_framebuffer_t *fb,
                               const crossos_image_t       *img,
                               int                          src_x,
                               int                          src_y,
                               int                          src_w,
                               int                          src_h,
                               int                          dst_x,
                               int                          dst_y);

/**
 * Obtain a pointer to the RGBA pixel at (x, y) within @p img.
 *
 * Returns NULL if @p img is NULL or the coordinates are out of bounds.
 * The returned pointer is valid until crossos_image_destroy() is called.
 */
unsigned char *crossos_image_pixel_at(const crossos_image_t *img, int x, int y);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CROSSOS_IMAGE_H */
