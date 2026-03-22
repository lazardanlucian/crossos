/**
 * crossos/draw.h  -  Software drawing helpers for CrossOS framebuffers.
 *
 * All drawing is clipped to the active clip region (managed via
 * crossos_draw_push_clip / crossos_draw_pop_clip).  Operations outside the
 * framebuffer bounds are silently ignored.
 */

#ifndef CROSSOS_DRAW_H
#define CROSSOS_DRAW_H

#include "display.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Color ──────────────────────────────────────────────────────────── */

typedef struct crossos_color {
    unsigned char r, g, b, a;
} crossos_color_t;

/** Mix fg over bg using `alpha` (0 = fully bg, 255 = fully fg). */
crossos_color_t crossos_color_lerp(crossos_color_t bg,
                                   crossos_color_t fg,
                                   unsigned char   alpha);

/** Lighten a colour by `amount` (0-255). */
crossos_color_t crossos_color_lighten(crossos_color_t c, int amount);

/** Darken a colour by `amount` (0-255). */
crossos_color_t crossos_color_darken(crossos_color_t c, int amount);

/* ── Clip stack ─────────────────────────────────────────────────────── */

/**
 * Push an axis-aligned clip rectangle.  Subsequent draw calls will be
 * clipped to the intersection of this rect and any parent clip.  The stack
 * depth is limited to 8; excess pushes are ignored.
 */
void crossos_draw_push_clip(int x, int y, int w, int h);

/** Pop the most-recently pushed clip rectangle. */
void crossos_draw_pop_clip(void);

/* ── Primitives ─────────────────────────────────────────────────────── */

void crossos_draw_clear(const crossos_framebuffer_t *fb,
                        crossos_color_t color);

void crossos_draw_fill_rect(const crossos_framebuffer_t *fb,
                            int x, int y, int w, int h,
                            crossos_color_t color);

void crossos_draw_stroke_rect(const crossos_framebuffer_t *fb,
                              int x, int y, int w, int h,
                              int thickness,
                              crossos_color_t color);

/** Filled rectangle with rounded corners (radius clamped to half min-side). */
void crossos_draw_fill_rounded_rect(const crossos_framebuffer_t *fb,
                                    int x, int y, int w, int h,
                                    int radius,
                                    crossos_color_t color);

/** Outline rectangle with rounded corners. */
void crossos_draw_stroke_rounded_rect(const crossos_framebuffer_t *fb,
                                      int x, int y, int w, int h,
                                      int radius, int thickness,
                                      crossos_color_t color);

/** Filled circle. */
void crossos_draw_fill_circle(const crossos_framebuffer_t *fb,
                              int cx, int cy, int radius,
                              crossos_color_t color);

/** Bresenham line. */
void crossos_draw_line(const crossos_framebuffer_t *fb,
                       int x0, int y0, int x1, int y1,
                       crossos_color_t color);

/* ── Text ───────────────────────────────────────────────────────────── */

/**
 * Draw `text` at (x, y) using the built-in 5×7 pixel font.
 * `scale` multiplies every pixel; 1 = native, 2 = double-size.
 * Supports printable ASCII (both cases).
 */
void crossos_draw_text(const crossos_framebuffer_t *fb,
                       int x, int y,
                       const char *text,
                       crossos_color_t color,
                       int scale);

/** Width in pixels of `text` at the given scale. */
int crossos_draw_text_width(const char *text, int scale);

/** Height of one line of text at the given scale (always 7*scale). */
int crossos_draw_text_height(int scale);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CROSSOS_DRAW_H */
