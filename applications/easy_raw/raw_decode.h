/**
 * applications/easy_raw/raw_decode.h
 *
 * Minimal decoder for common RAW image formats (PPM P6 as raw stand-in,
 * plus a simple 10-bit packed-RAW reader for Bayer RGGB data written by
 * cameras or produced by CrossOS scanner output).
 *
 * Supported input formats:
 *   • PPM P6  (binary RGB, 8 or 16-bit) — used for testing & scanner output
 *   • Raw Bayer  (.raw / .rw2-strip)    — 10-bit little-endian packed RGGB
 *
 * All decoded data is demosaiced into a 32-bit float planar buffer
 * (linear light, 0.0–1.0 range) for downstream processing.
 */

#ifndef EASYRAW_RAW_DECODE_H
#define EASYRAW_RAW_DECODE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* ── RAW image descriptor ───────────────────────────────────────────── */

    /** RAW Bayer pattern. */
    typedef enum er_bayer_pattern
    {
        ER_BAYER_RGGB = 0,
        ER_BAYER_BGGR,
        ER_BAYER_GRBG,
        ER_BAYER_GBRG,
    } er_bayer_pattern_t;

    /**
     * Decoded/demosaiced RAW image in float32 planar format.
     *
     * planes[0] = R  (width * height floats)
     * planes[1] = G  (width * height floats)
     * planes[2] = B  (width * height floats)
     *
     * Values are linear light, black-point subtracted, white-point normalised
     * to approximately 1.0.
     */
    typedef struct er_raw_image
    {
        float *planes[3]; /**< R, G, B float planes                              */
        int width;
        int height;
        /* Metadata (may be zero if unavailable) */
        double cam_matrix[3][3]; /**< Camera-to-XYZ colour matrix (D65)          */
        double black_level;      /**< Original sensor black level (normalised)   */
        double white_level;      /**< Saturation point before normalisation      */
        int iso;
        double exposure_time; /**< Shutter speed in seconds                   */
        double f_number;
        char camera_model[64];
    } er_raw_image_t;

    /* ── API ─────────────────────────────────────────────────────────────── */

    /**
     * Load a file and decode it into a float32 planar image.
     *
     * Supports:
     *   .ppm / .pgm   — PPM P5/P6 binary
     *   .raw           — 10-bit little-endian packed Bayer (requires width/height
     *                    embedded as first 8 bytes: uint32LE width, uint32LE height)
     *
     * @param  path     Absolute or relative path to the image file.
     * @param  out      Pointer to an er_raw_image_t to fill.  Must be freed with
     *                  er_raw_image_free() when no longer needed.
     * @return 0 on success, non-zero on error.
     */
    int er_raw_image_load(const char *path, er_raw_image_t *out);

    /** Release all memory owned by *img; does not free img itself. */
    void er_raw_image_free(er_raw_image_t *img);

#ifdef __cplusplus
}
#endif

#endif /* EASYRAW_RAW_DECODE_H */
