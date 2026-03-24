/**
 * crossos/font.h  –  OTF/TTF font loading and Unicode text rendering.
 *
 * Provides full TrueType/OpenType font support backed by stb_truetype
 * (public domain, Sean Barrett / RAD Game Tools).  Any .ttf or .otf font
 * file can be loaded; an anti-aliased glyph is rasterised on demand and
 * alpha-blended onto a CrossOS framebuffer.
 *
 * A high-quality free sans-serif font (Liberation Sans Regular, SIL OFL 1.1)
 * is bundled and accessible via crossos_typeface_load_builtin().
 *
 * System font discovery is also supported via crossos_typeface_load_system():
 *   Windows  – searches %WINDIR%\Fonts\ by common face-name patterns
 *   Linux    – uses fontconfig (if available) then /usr/share/fonts fallback
 *   Android  – scans /system/fonts and /data/fonts
 *
 * Quick-start:
 *
 *   crossos_typeface_t *face = NULL;
 *   crossos_typeface_load_builtin(&face);          // or load_file / load_system
 *
 *   crossos_framebuffer_t fb;
 *   crossos_surface_lock(surf, &fb);
 *
 *   crossos_color_t white = {255, 255, 255, 255};
 *   crossos_typeface_draw_text(&fb, face, 20, 40, "Hello, 世界! 🌍", 24.0f, white);
 *
 *   crossos_surface_unlock(surf);
 *   crossos_surface_present(surf);
 *
 *   crossos_typeface_destroy(face);
 */

#ifndef CROSSOS_FONT_H
#define CROSSOS_FONT_H

#include "types.h"
#include "display.h"
#include "draw.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Typeface style flags ─────────────────────────────────────────────── */

/**
 * Hint passed to crossos_typeface_load_system() to select a font variant.
 * These are advisory – the platform will match the best available style.
 */
typedef enum crossos_typeface_style {
    CROSSOS_TYPEFACE_STYLE_NORMAL = 0x00, /**< Regular upright weight         */
    CROSSOS_TYPEFACE_STYLE_BOLD   = 0x01, /**< Bold weight                    */
    CROSSOS_TYPEFACE_STYLE_ITALIC = 0x02, /**< Italic / oblique               */
} crossos_typeface_style_t;

/* ── Text alignment ───────────────────────────────────────────────────── */

typedef enum crossos_text_align {
    CROSSOS_TEXT_ALIGN_LEFT   = 0, /**< Origin is the left edge of the string  */
    CROSSOS_TEXT_ALIGN_CENTER = 1, /**< Origin is the horizontal centre        */
    CROSSOS_TEXT_ALIGN_RIGHT  = 2, /**< Origin is the right edge               */
} crossos_text_align_t;

/* ── Text metrics ─────────────────────────────────────────────────────── */

/**
 * Pixel-space measurements for a string rendered at a given size.
 * All values are rounded to the nearest integer pixel.
 */
typedef struct crossos_text_metrics {
    int width;   /**< Total advance width in pixels                           */
    int height;  /**< Line height (ascent + descent) in pixels                */
    int ascent;  /**< Distance from baseline to topmost pixel (positive up)   */
    int descent; /**< Distance from baseline to bottom (positive down)        */
} crossos_text_metrics_t;

/* ── Opaque typeface handle ───────────────────────────────────────────── */

/** Opaque handle representing a loaded TrueType / OpenType font face. */
typedef struct crossos_typeface crossos_typeface_t;

/* ── Loading ─────────────────────────────────────────────────────────── */

/**
 * Load a TrueType or OpenType font from a file path.
 *
 * Both .ttf and .otf files are accepted.  The file is read into memory and
 * the handle remains valid independently of the file on disk.
 *
 * @param path     Path to the .ttf or .otf file.
 * @param out_face Receives a new typeface handle on success.  The caller
 *                 must call crossos_typeface_destroy() when done.
 * @return         CROSSOS_OK on success; CROSSOS_ERR_IO if the file cannot
 *                 be read; CROSSOS_ERR_PARAM if the file is not a valid font.
 */
crossos_result_t crossos_typeface_load_file(const char         *path,
                                            crossos_typeface_t **out_face);

/**
 * Load a TrueType or OpenType font from an in-memory buffer.
 *
 * The caller retains ownership of @p data; CrossOS makes an internal copy.
 *
 * @param data     Pointer to the raw font bytes (.ttf / .otf).
 * @param size     Size of @p data in bytes.
 * @param out_face Receives a new typeface handle on success.
 * @return         CROSSOS_OK on success; CROSSOS_ERR_PARAM for invalid data.
 */
crossos_result_t crossos_typeface_load_memory(const void         *data,
                                              size_t              size,
                                              crossos_typeface_t **out_face);

/**
 * Load the SDK's bundled sans-serif font (Liberation Sans Regular).
 *
 * This function always succeeds as long as memory is available.  No file
 * system access is needed – the font bytes are compiled into the library.
 *
 * Liberation Sans is licensed under the SIL Open Font License 1.1.
 * See https://scripts.sil.org/OFL for details.
 *
 * @param out_face Receives the builtin typeface handle.
 * @return         CROSSOS_OK on success; CROSSOS_ERR_OOM if allocation fails.
 */
crossos_result_t crossos_typeface_load_builtin(crossos_typeface_t **out_face);

/**
 * Load a named system font.
 *
 * The @p family_name is a font family name such as "sans-serif", "Arial",
 * "Helvetica Neue", "Roboto", etc.  CrossOS searches the platform's system
 * font directories (see platform notes in this header's module doc).
 *
 * If an exact match is not found, the closest available variant is returned.
 * If no suitable font is found at all, CROSSOS_ERR_IO is returned and the
 * caller should fall back to crossos_typeface_load_builtin().
 *
 * @param family_name  Font family name (UTF-8, case-insensitive on most
 *                     platforms).  Pass "sans-serif" to get the default
 *                     system sans-serif font.
 * @param style        Desired style (normal / bold / italic).
 * @param out_face     Receives the typeface handle on success.
 * @return             CROSSOS_OK on success; CROSSOS_ERR_IO if not found.
 */
crossos_result_t crossos_typeface_load_system(const char               *family_name,
                                              crossos_typeface_style_t  style,
                                              crossos_typeface_t       **out_face);

/**
 * Free a typeface handle.
 * Safe to call with NULL.  After this call the handle must not be used.
 */
void crossos_typeface_destroy(crossos_typeface_t *face);

/* ── Metrics ─────────────────────────────────────────────────────────── */

/**
 * Measure the pixel dimensions of a UTF-8 string at the given point size.
 *
 * The @p pixel_size parameter is the font size in pixels (equivalent to CSS
 * `font-size` in px).  Typical values: 12–16 for body text, 24–48 for titles.
 *
 * @param face        Loaded typeface.
 * @param text        NUL-terminated UTF-8 string.
 * @param pixel_size  Desired render size in pixels (e.g. 16.0f).
 * @param out         Receives width, height, ascent, descent.
 * @return            CROSSOS_OK; CROSSOS_ERR_PARAM if any pointer is NULL.
 */
crossos_result_t crossos_typeface_measure(const crossos_typeface_t *face,
                                          const char               *text,
                                          float                     pixel_size,
                                          crossos_text_metrics_t   *out);

/* ── Rendering ───────────────────────────────────────────────────────── */

/**
 * Render a UTF-8 string into a CrossOS framebuffer.
 *
 * The text is alpha-blended onto @p fb.  The baseline is at y + ascent, so
 * the topmost pixel of most capitals is at approximately (x, y).
 *
 * Supports the full Unicode Basic Multilingual Plane and any codepoint whose
 * glyph is present in the loaded font file.  Codepoints with no glyph in the
 * font are silently skipped.
 *
 * @param fb          Target framebuffer (locked via crossos_surface_lock).
 * @param face        Loaded typeface.
 * @param x           Left edge of the text in framebuffer pixels.
 * @param y           Top edge of the text in framebuffer pixels.
 * @param text        NUL-terminated UTF-8 string.
 * @param pixel_size  Font size in pixels.
 * @param color       Foreground colour; the alpha channel of each rendered
 *                    glyph pixel is combined with color.a before blending.
 * @return            CROSSOS_OK on success; CROSSOS_ERR_PARAM if a required
 *                    pointer is NULL; CROSSOS_ERR_OOM if glyph buffers cannot
 *                    be allocated.
 */
crossos_result_t crossos_typeface_draw_text(const crossos_framebuffer_t *fb,
                                            const crossos_typeface_t    *face,
                                            int                          x,
                                            int                          y,
                                            const char                  *text,
                                            float                        pixel_size,
                                            crossos_color_t              color);

/**
 * Render a UTF-8 string with horizontal alignment.
 *
 * Convenience wrapper around crossos_typeface_draw_text() that adjusts the
 * X origin according to @p align.
 *
 * @param fb          Target framebuffer.
 * @param face        Loaded typeface.
 * @param x           Reference X position (meaning depends on @p align).
 * @param y           Top edge of the text.
 * @param text        NUL-terminated UTF-8 string.
 * @param pixel_size  Font size in pixels.
 * @param color       Foreground colour.
 * @param align       Horizontal alignment relative to @p x.
 * @return            CROSSOS_OK or an error code.
 */
crossos_result_t crossos_typeface_draw_text_aligned(
                                    const crossos_framebuffer_t *fb,
                                    const crossos_typeface_t    *face,
                                    int                          x,
                                    int                          y,
                                    const char                  *text,
                                    float                        pixel_size,
                                    crossos_color_t              color,
                                    crossos_text_align_t         align);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CROSSOS_FONT_H */
