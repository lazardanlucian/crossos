/**
 * applications/easy_raw/raw_process.h
 *
 * Non-destructive RAW processing pipeline for EasyRaw.
 *
 * The pipeline matches the architecture of RawTherapee / RapidRAW:
 *
 *   float RAW planes  →  [Black/White clip]
 *                      →  [Exposure]
 *                      →  [White Balance]
 *                      →  [Cam→sRGB colour matrix]
 *                      →  [Tone Curve (individual R/G/B + combined)]
 *                      →  [Highlight Recovery]
 *                      →  [Shadows/Highlights]
 *                      →  [HSL adjustments]
 *                      →  [Sharpening]
 *                      →  [Noise Reduction (simple Gaussian)]
 *                      →  [Gamma encode to sRGB]
 *                      →  8-bit RGBA output for display
 *
 * All adjustments are stored in an er_params_t struct.  Calling
 * er_process() re-runs the full pipeline from the decoded float planes.
 */

#ifndef EASYRAW_RAW_PROCESS_H
#define EASYRAW_RAW_PROCESS_H

#include "raw_decode.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* ── Tone curve point ───────────────────────────────────────────────── */

#define ER_CURVE_MAX_PTS 16

    typedef struct er_curve_point
    {
        float in;  /**< Input value  [0, 1] */
        float out; /**< Output value [0, 1] */
    } er_curve_point_t;

    typedef struct er_curve
    {
        er_curve_point_t pts[ER_CURVE_MAX_PTS];
        int count; /**< Number of active control points (>= 2) */
    } er_curve_t;

    /* ── Processing parameters ──────────────────────────────────────────── */

    typedef struct er_params
    {
        /* ── Exposure & White Balance ── */
        float exposure_ev; /**< EV stops: -5.0 .. +5.0, default 0         */
        float wb_temp;     /**< Colour temperature in K, e.g. 5500        */
        float wb_tint;     /**< Green↔Magenta tint: -100 .. +100, 0=none  */

        /* ── Tone ── */
        float blacks;     /**< Shadow lift: -100 .. +100               */
        float shadows;    /**< Shadows:     -100 .. +100               */
        float midtones;   /**< Midtones:    -100 .. +100               */
        float highlights; /**< Highlights:  -100 .. +100               */
        float whites;     /**< Spec. clip:  -100 .. +100               */

        float contrast; /**< Contrast:    -100 .. +100               */

        /* ── Colour ── */
        float vibrance;   /**< Vibrance:    -100 .. +100               */
        float saturation; /**< Saturation:  -100 .. +100               */

        /* Per-channel HSL hue shift (degrees -180..+180) */
        float hsl_hue_shift[6]; /**< R, O, Y, G, Cy, B slices               */
        float hsl_sat_shift[6]; /**< as above, saturation delta              */
        float hsl_lum_shift[6]; /**< as above, luminance delta               */

        /* ── Tone curves ── */
        er_curve_t curve_luma; /**< Combined luminance curve                */
        er_curve_t curve_r;    /**< Per-channel red                         */
        er_curve_t curve_g;    /**< Per-channel green                       */
        er_curve_t curve_b;    /**< Per-channel blue                        */

        /* ── Detail ── */
        float sharpening;      /**< 0 = none, 1 = max (unsharp mask amount) */
        float noise_reduction; /**< 0 = none, 1 = max (Gaussian sigma)      */

        float highlight_recovery; /**< 0 = off, 1 = full highlight reconstruction */

        /* ── Output ── */
        float output_gamma; /**< Gamma exponent; 0 = use standard sRGB   */
    } er_params_t;

    /**
     * Initialise params to sensible defaults (identity / no adjustments).
     */
    void er_params_init(er_params_t *p);

    /* ── Processed output ───────────────────────────────────────────────── */

    /**
     * 8-bit RGBA packed image ready for display / blit.
     * pixels = R G B A R G B A ... row-major.
     */
    typedef struct er_output
    {
        uint8_t *pixels;
        int width;
        int height;
        int stride; /**< bytes per row */
    } er_output_t;

    /* ── API ─────────────────────────────────────────────────────────────── */

    /**
     * Run the full processing pipeline on `src` with `params` and write the
     * result into `out`.
     *
     * `out->pixels` is (re)allocated by this function; pass a zero-initialised
     * er_output_t on the first call, and the same struct on subsequent calls to
     * reuse the allocation when dimensions are unchanged.
     *
     * @return 0 on success, non-zero on OOM.
     */
    int er_process(const er_raw_image_t *src,
                   const er_params_t *params,
                   er_output_t *out);

    /** Free the pixel buffer owned by `out`. */
    void er_output_free(er_output_t *out);

#ifdef __cplusplus
}
#endif

#endif /* EASYRAW_RAW_PROCESS_H */
