/**
 * crossos/scanner.h  –  Flatbed / film-scanner abstraction.
 *
 * Provides cross-platform scanner enumeration, control, and image acquisition
 * for flatbed and film scanners.  On Linux the implementation is backed by
 * SANE (Scanner Access Now Easy); Windows uses WIA/TWAIN; Android is exposed
 * via USB Host with a libusb-based SANE-compatible command set (in progress).
 *
 * Plustek OpticFilm (35 mm film) is the primary target device, but the API
 * is generic enough to drive any SANE-supported scanner.
 *
 * ── Film processing ──────────────────────────────────────────────────────
 *
 * Film scanning requires post-processing to convert a raw linear capture into
 * a viewable image.  This module also exposes a lightweight tone-curve engine
 * and a library of presets for popular colour-negative, slide, and black &
 * white film stocks.
 *
 * Quick-start:
 *
 *   crossos_scanner_init();
 *
 *   crossos_scanner_info_t devs[8];
 *   int n = crossos_scanner_enumerate(devs, 8);
 *   // dev[0].name → "Plustek OpticFilm 135"
 *
 *   crossos_scanner_t *sc = NULL;
 *   crossos_scanner_open(0, &sc);
 *
 *   crossos_scanner_params_t p;
 *   crossos_scanner_get_default_params(sc, &p);
 *   p.resolution = 1200;   // DPI
 *
 *   crossos_scan_result_t result = {0};
 *   crossos_scanner_scan(sc, &p, &result);
 *
 *   // Apply Kodak Portra 400 tone curve
 *   crossos_film_curve_t curve;
 *   crossos_film_curve_get_preset(CROSSOS_FILM_STOCK_KODAK_PORTRA_400, &curve);
 *   crossos_film_apply_curve(&result, &curve, 0.0f);
 *
 *   // result.pixels is now RGBA8888, ready for display
 *   crossos_scanner_free_result(&result);
 *   crossos_scanner_close(sc);
 *   crossos_scanner_shutdown();
 */

#ifndef CROSSOS_SCANNER_H
#define CROSSOS_SCANNER_H

#include "types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* ════════════════════════════════════════════════════════════════════════
     *  Device info
     * ════════════════════════════════════════════════════════════════════════ */

#define CROSSOS_SCANNER_NAME_MAX 128
#define CROSSOS_SCANNER_VENDOR_MAX 64
#define CROSSOS_SCANNER_MODEL_MAX 64
#define CROSSOS_SCANNER_TYPE_MAX 32
#define CROSSOS_SCANNER_PATH_MAX 256
#define CROSSOS_SCANNER_MAX_DEVS 16

/** Type descriptor string values (stored in crossos_scanner_info_t::type). */
#define CROSSOS_SCANNER_TYPE_FLATBED "flatbed"
#define CROSSOS_SCANNER_TYPE_FILM "film"
#define CROSSOS_SCANNER_TYPE_SHEETFED "sheetfed"
#define CROSSOS_SCANNER_TYPE_UNKNOWN "unknown"

    /**
     * Describes a discovered scanner device.
     */
    typedef struct crossos_scanner_info
    {
        int index;                                  /**< 0-based ordinal               */
        char name[CROSSOS_SCANNER_NAME_MAX];        /**< Full human-readable name      */
        char vendor[CROSSOS_SCANNER_VENDOR_MAX];    /**< Device vendor string          */
        char model[CROSSOS_SCANNER_MODEL_MAX];      /**< Device model string           */
        char type[CROSSOS_SCANNER_TYPE_MAX];        /**< "flatbed", "film", ...        */
        char device_path[CROSSOS_SCANNER_PATH_MAX]; /**< Backend-specific path     */
        int is_usb;                                 /**< Non-zero if USB-attached       */
        int is_plustek;                             /**< Non-zero for Plustek devices   */
    } crossos_scanner_info_t;

    /* ════════════════════════════════════════════════════════════════════════
     *  Scan parameters
     * ════════════════════════════════════════════════════════════════════════ */

    /** Colour acquisition mode. */
    typedef enum crossos_scanner_color_mode
    {
        CROSSOS_SCANNER_COLOR_MODE_COLOR = 0,   /**< 24-bit (or 48-bit) RGB scan   */
        CROSSOS_SCANNER_COLOR_MODE_GRAY = 1,    /**< Greyscale scan                */
        CROSSOS_SCANNER_COLOR_MODE_LINEART = 2, /**< 1-bit black & white           */
    } crossos_scanner_color_mode_t;

    /** Bit depth selection.  Not all devices support both. */
    typedef enum crossos_scanner_bit_depth
    {
        CROSSOS_SCANNER_DEPTH_8 = 8,
        CROSSOS_SCANNER_DEPTH_16 = 16,
    } crossos_scanner_bit_depth_t;

    /**
     * Scan area in millimetres, relative to the top-left origin of the device's
     * scannable region.
     *
     * For a Plustek OpticFilm 135 negative holder:
     *   x=0, y=0, width=36.0, height=24.0  → single 35 mm frame
     */
    typedef struct crossos_scanner_area
    {
        float x;      /**< Left edge (mm from device origin)  */
        float y;      /**< Top edge  (mm from device origin)  */
        float width;  /**< Scan width  in mm                  */
        float height; /**< Scan height in mm                  */
    } crossos_scanner_area_t;

    /**
     * Complete parameter set for a scan job.
     * Populate via crossos_scanner_get_default_params() then modify fields as
     * needed before calling crossos_scanner_scan().
     */
    typedef struct crossos_scanner_params
    {
        int resolution;                          /**< DPI (e.g. 300, 1200, 3600)  */
        crossos_scanner_color_mode_t color_mode; /**< Color / Gray / Lineart       */
        crossos_scanner_bit_depth_t bit_depth;   /**< 8 or 16 bits per channel     */
        crossos_scanner_area_t area;             /**< Scan area in mm              */
        int preview;                             /**< 1 = quick low-res preview     */
    } crossos_scanner_params_t;

    /* ════════════════════════════════════════════════════════════════════════
     *  Scan result
     * ════════════════════════════════════════════════════════════════════════ */

    /**
     * Raw scan output held in host memory.
     *
     * Always decoded to RGBA8888 (pixels[y*stride + x*4] = R,G,B,A).
     * Alpha is always 255.  For 16-bit-depth scans the high 8 bits of each
     * channel are stored (i.e. the same RGBA8888 layout; use raw16 for the
     * original 16-bit linear data when available).
     *
     * Memory is owned by the CrossOS scanner subsystem; call
     * crossos_scanner_free_result() when done.
     */
    typedef struct crossos_scan_result
    {
        uint8_t *pixels;      /**< RGBA8888 pixel buffer                     */
        int width;            /**< Image width  in pixels                    */
        int height;           /**< Image height in pixels                    */
        int stride;           /**< Row stride in bytes (>= width * 4)        */
        int resolution_dpi;   /**< Actual DPI used by the device             */
        int bits_per_channel; /**< 8 or 16 (original device bit depth)       */

        /** Optional 16-bit linear buffer (NULL for 8-bit scans).
         *  Stored as interleaved RGB (no alpha): raw16[y*width*3 + x*3] = R.   */
        uint16_t *raw16;
    } crossos_scan_result_t;

    /* ════════════════════════════════════════════════════════════════════════
     *  Opaque scanner handle
     * ════════════════════════════════════════════════════════════════════════ */

    typedef struct crossos_scanner crossos_scanner_t;

    /* ════════════════════════════════════════════════════════════════════════
     *  Lifecycle & enumeration
     * ════════════════════════════════════════════════════════════════════════ */

    /**
     * Initialise the scanner subsystem.
     * Must be called before any other scanner function.
     * Safe to call multiple times; reference-counted internally.
     */
    crossos_result_t crossos_scanner_init(void);

    /**
     * Shut down the scanner subsystem and release all internal resources.
     */
    void crossos_scanner_shutdown(void);

    /**
     * Enumerate available scanner devices.
     *
     * @param out_infos  Caller-supplied array to receive device descriptors.
     * @param max_count  Capacity of out_infos[].
     * @return           Number of devices found, or negative crossos_result_t on
     *                   fatal error.
     */
    int crossos_scanner_enumerate(crossos_scanner_info_t *out_infos,
                                  int max_count);

    /**
     * Open a scanner device by its enumeration index.
     *
     * @param index       0-based index as returned by crossos_scanner_enumerate().
     * @param out_scanner Receives the opaque scanner handle on success.
     */
    crossos_result_t crossos_scanner_open(int index,
                                          crossos_scanner_t **out_scanner);

    /**
     * Open a scanner device directly by its platform path string
     * (e.g. "plustek:libusb:001:004" on Linux).
     */
    crossos_result_t crossos_scanner_open_path(const char *device_path,
                                               crossos_scanner_t **out_scanner);

    /** Close a scanner handle and release its resources. */
    void crossos_scanner_close(crossos_scanner_t *scanner);

    /* ════════════════════════════════════════════════════════════════════════
     *  Parameter negotiation
     * ════════════════════════════════════════════════════════════════════════ */

    /**
     * Populate *out_params with sensible defaults for the given scanner.
     * For Plustek film scanners this chooses 1200 DPI, colour, 35 mm frame area.
     */
    crossos_result_t crossos_scanner_get_default_params(
        crossos_scanner_t *scanner,
        crossos_scanner_params_t *out_params);

    /* ════════════════════════════════════════════════════════════════════════
     *  Acquisition
     * ════════════════════════════════════════════════════════════════════════ */

    /**
     * Perform a synchronous scan and return the decoded image.
     *
     * The function blocks until the scan is complete or cancelled.
     * On success *out_result is populated; call crossos_scanner_free_result()
     * when the pixel data is no longer needed.
     *
     * @param scanner    Open scanner handle.
     * @param params     Scan parameters (may be NULL → device defaults).
     * @param out_result Receives the decoded scan image.
     */
    crossos_result_t crossos_scanner_scan(crossos_scanner_t *scanner,
                                          const crossos_scanner_params_t *params,
                                          crossos_scan_result_t *out_result);

    /**
     * Perform a fast low-resolution preview scan (convenience wrapper around
     * crossos_scanner_scan with preview=1 and a reduced DPI).
     */
    crossos_result_t crossos_scanner_preview(crossos_scanner_t *scanner,
                                             crossos_scan_result_t *out_result);

    /**
     * Cancel an in-progress scan.
     * Safe to call even when no scan is running.
     */
    void crossos_scanner_cancel(crossos_scanner_t *scanner);

    /** Free memory owned by a scan result.  Sets pixels/raw16 to NULL. */
    void crossos_scanner_free_result(crossos_scan_result_t *result);

    /* ════════════════════════════════════════════════════════════════════════
     *  Film tone-curve processing
     * ════════════════════════════════════════════════════════════════════════ */

    /**
     * Per-channel lookup-table tone curve.
     *
     * Each channel maps input [0–255] → output [0–255].
     * An identity curve is {0,1,2,…,255} for every channel.
     *
     * When @p invert is non-zero the curve is applied AFTER inverting the raw
     * pixel values (i.e. treating the scan as a colour-negative).
     */
    typedef struct crossos_film_curve
    {
        uint8_t r[256]; /**< Red channel lookup table   */
        uint8_t g[256]; /**< Green channel lookup table */
        uint8_t b[256]; /**< Blue channel lookup table  */
        int invert;     /**< Non-zero → colour-negative inversion before curve */
        char name[64];  /**< Descriptive label                                  */
    } crossos_film_curve_t;

    /**
     * Known film stock identifiers.
     *
     * Colour negatives (C-41) and black-and-white negatives require @p invert=1.
     * Slide/reversal films (E-6) use @p invert=0.
     */
    typedef enum crossos_film_stock
    {
        CROSSOS_FILM_STOCK_NONE = 0, /**< Flat / no correction     */
        /* ── Colour negatives (C-41) ── */
        CROSSOS_FILM_STOCK_KODAK_PORTRA_400 = 1,
        CROSSOS_FILM_STOCK_KODAK_PORTRA_160 = 2,
        CROSSOS_FILM_STOCK_KODAK_GOLD_200 = 3,
        CROSSOS_FILM_STOCK_KODAK_EKTAR_100 = 4,
        CROSSOS_FILM_STOCK_KODAK_COLORPLUS = 5,
        CROSSOS_FILM_STOCK_FUJI_400H = 6,
        CROSSOS_FILM_STOCK_FUJI_SUPERIA_400 = 7,
        CROSSOS_FILM_STOCK_FUJI_C200 = 8,
        /* ── Slide / E-6 (positive) ── */
        CROSSOS_FILM_STOCK_FUJI_PROVIA_100F = 9,
        CROSSOS_FILM_STOCK_FUJI_VELVIA_50 = 10,
        CROSSOS_FILM_STOCK_FUJI_VELVIA_100 = 11,
        CROSSOS_FILM_STOCK_KODAK_EKTACHROME = 12,
        /* ── Black & white negatives ── */
        CROSSOS_FILM_STOCK_KODAK_TRIX_400 = 13,
        CROSSOS_FILM_STOCK_KODAK_TMAX_400 = 14,
        CROSSOS_FILM_STOCK_ILFORD_HP5 = 15,
        CROSSOS_FILM_STOCK_ILFORD_DELTA_400 = 16,
        CROSSOS_FILM_STOCK_ILFORD_FP4 = 17,
        CROSSOS_FILM_STOCK_COUNT,
    } crossos_film_stock_t;

    /**
     * Load a preset tone curve for a known film stock into *out_curve.
     *
     * @param stock     Film stock identifier.
     * @param out_curve Receives the preset curve values.
     */
    void crossos_film_curve_get_preset(crossos_film_stock_t stock,
                                       crossos_film_curve_t *out_curve);

    /**
     * Return a human-readable name string for a film stock identifier.
     * e.g. "Kodak Portra 400", "Fuji Velvia 50", …
     */
    const char *crossos_film_stock_name(crossos_film_stock_t stock);

    /**
     * Apply a tone curve (and optional negative inversion) to a scan result
     * in-place.
     *
     * @param result    Scan result whose pixel buffer will be modified in-place.
     * @param curve     Tone curve to apply.  NULL → no curve (identity).
     * @param exposure  Exposure adjustment in EV stops (positive = brighter,
     *                  negative = darker; clamped to [-4, +4]).  Applied after
     *                  the tone curve.
     */
    void crossos_film_apply_curve(crossos_scan_result_t *result,
                                  const crossos_film_curve_t *curve,
                                  float exposure);

    /**
     * Initialise curve to the identity (no change, no inversion).
     */
    void crossos_film_curve_identity(crossos_film_curve_t *curve);

    /* ════════════════════════════════════════════════════════════════════════
     *  Platform backend (internal – do not use directly)
     * ════════════════════════════════════════════════════════════════════════ */

    typedef struct crossos_scanner_backend
    {
        crossos_result_t (*init)(void);
        void (*shutdown)(void);
        int (*enumerate)(crossos_scanner_info_t *out, int max);
        crossos_result_t (*open)(const char *path, void **out_handle);
        void (*close)(void *handle);
        crossos_result_t (*get_default_params)(void *handle,
                                               crossos_scanner_params_t *out);
        crossos_result_t (*scan)(void *handle,
                                 const crossos_scanner_params_t *params,
                                 crossos_scan_result_t *out);
        void (*cancel)(void *handle);
    } crossos_scanner_backend_t;

    /** Called by platform init code to register the platform backend. */
    void crossos_scanner_register_backend(const crossos_scanner_backend_t *backend);

#ifdef __cplusplus
}
#endif

#endif /* CROSSOS_SCANNER_H */
