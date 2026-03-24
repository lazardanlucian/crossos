/*
 * src/core/scanner.c – CrossOS scanner subsystem core.
 *
 * Dispatches to the platform backend registered via
 * crossos_scanner_register_backend().  Also provides the film tone-curve
 * engine and all built-in film-stock presets (pure C, no dependencies).
 *
 * Platform backends:
 *   Linux   → src/platform/linux/scanner_sane.c   (SANE 1.x)
 *   Windows → src/platform/windows/scanner_twain.c (WIA/TWAIN stub)
 *   Android → src/platform/android/scanner_android.c (USB Host stub)
 */

#include <crossos/scanner.h>

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ════════════════════════════════════════════════════════════════════════
 *  Internal scanner handle
 * ════════════════════════════════════════════════════════════════════════ */

struct crossos_scanner
{
    void *platform_handle;
    crossos_scanner_info_t info;
    const crossos_scanner_backend_t *backend;
};

/* ════════════════════════════════════════════════════════════════════════
 *  Backend registry (one per process)
 * ════════════════════════════════════════════════════════════════════════ */

static const crossos_scanner_backend_t *s_backend = NULL;
static int s_init_count = 0;

void crossos_scanner_register_backend(const crossos_scanner_backend_t *backend)
{
    s_backend = backend;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Lifecycle
 * ════════════════════════════════════════════════════════════════════════ */

crossos_result_t crossos_scanner_init(void)
{
    if (s_init_count++ > 0)
        return CROSSOS_OK;

    if (!s_backend || !s_backend->init)
        return CROSSOS_ERR_UNSUPPORT;

    return s_backend->init();
}

void crossos_scanner_shutdown(void)
{
    if (s_init_count <= 0)
        return;
    if (--s_init_count == 0 && s_backend && s_backend->shutdown)
        s_backend->shutdown();
}

/* ════════════════════════════════════════════════════════════════════════
 *  Enumeration
 * ════════════════════════════════════════════════════════════════════════ */

int crossos_scanner_enumerate(crossos_scanner_info_t *out_infos, int max_count)
{
    if (!out_infos || max_count <= 0)
        return CROSSOS_ERR_PARAM;
    if (!s_backend || !s_backend->enumerate)
        return 0;
    return s_backend->enumerate(out_infos, max_count);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Open / close
 * ════════════════════════════════════════════════════════════════════════ */

crossos_result_t crossos_scanner_open(int index, crossos_scanner_t **out_scanner)
{
    if (!out_scanner)
        return CROSSOS_ERR_PARAM;
    *out_scanner = NULL;

    if (!s_backend)
        return CROSSOS_ERR_UNSUPPORT;

    /* Enumerate so we can look up the device path by index. */
    crossos_scanner_info_t infos[CROSSOS_SCANNER_MAX_DEVS];
    int count = crossos_scanner_enumerate(infos, CROSSOS_SCANNER_MAX_DEVS);
    if (count <= 0)
        return CROSSOS_ERR_SCANNER;
    if (index < 0 || index >= count)
        return CROSSOS_ERR_PARAM;

    return crossos_scanner_open_path(infos[index].device_path, out_scanner);
}

crossos_result_t crossos_scanner_open_path(const char *device_path,
                                           crossos_scanner_t **out_scanner)
{
    if (!device_path || !out_scanner)
        return CROSSOS_ERR_PARAM;
    *out_scanner = NULL;

    if (!s_backend || !s_backend->open)
        return CROSSOS_ERR_UNSUPPORT;

    crossos_scanner_t *sc = calloc(1, sizeof(*sc));
    if (!sc)
        return CROSSOS_ERR_OOM;

    sc->backend = s_backend;

    crossos_result_t rc = s_backend->open(device_path, &sc->platform_handle);
    if (rc != CROSSOS_OK)
    {
        free(sc);
        return rc;
    }

    strncpy(sc->info.device_path, device_path, CROSSOS_SCANNER_PATH_MAX - 1);
    *out_scanner = sc;
    return CROSSOS_OK;
}

void crossos_scanner_close(crossos_scanner_t *scanner)
{
    if (!scanner)
        return;
    if (scanner->backend && scanner->backend->close)
        scanner->backend->close(scanner->platform_handle);
    free(scanner);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Parameter negotiation
 * ════════════════════════════════════════════════════════════════════════ */

crossos_result_t crossos_scanner_get_default_params(
    crossos_scanner_t *scanner,
    crossos_scanner_params_t *out_params)
{
    if (!scanner || !out_params)
        return CROSSOS_ERR_PARAM;

    if (scanner->backend && scanner->backend->get_default_params)
        return scanner->backend->get_default_params(scanner->platform_handle,
                                                    out_params);

    /* Sane built-in defaults for a 35 mm film frame. */
    out_params->resolution = 1200;
    out_params->color_mode = CROSSOS_SCANNER_COLOR_MODE_COLOR;
    out_params->bit_depth = CROSSOS_SCANNER_DEPTH_8;
    out_params->area.x = 0.0f;
    out_params->area.y = 0.0f;
    out_params->area.width = 36.0f;  /* 35 mm film frame width  */
    out_params->area.height = 24.0f; /* 35 mm film frame height */
    out_params->preview = 0;
    return CROSSOS_OK;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Acquisition
 * ════════════════════════════════════════════════════════════════════════ */

crossos_result_t crossos_scanner_scan(crossos_scanner_t *scanner,
                                      const crossos_scanner_params_t *params,
                                      crossos_scan_result_t *out_result)
{
    if (!scanner || !out_result)
        return CROSSOS_ERR_PARAM;

    if (!scanner->backend || !scanner->backend->scan)
        return CROSSOS_ERR_UNSUPPORT;

    crossos_scanner_params_t defaults;
    if (!params)
    {
        crossos_scanner_get_default_params(scanner, &defaults);
        params = &defaults;
    }

    memset(out_result, 0, sizeof(*out_result));
    return scanner->backend->scan(scanner->platform_handle, params, out_result);
}

crossos_result_t crossos_scanner_preview(crossos_scanner_t *scanner,
                                         crossos_scan_result_t *out_result)
{
    if (!scanner || !out_result)
        return CROSSOS_ERR_PARAM;

    crossos_scanner_params_t p;
    crossos_result_t rc = crossos_scanner_get_default_params(scanner, &p);
    if (rc != CROSSOS_OK)
        return rc;

    p.preview = 1;
    p.resolution = 75; /* low-res for a fast preview */

    return crossos_scanner_scan(scanner, &p, out_result);
}

void crossos_scanner_cancel(crossos_scanner_t *scanner)
{
    if (!scanner)
        return;
    if (scanner->backend && scanner->backend->cancel)
        scanner->backend->cancel(scanner->platform_handle);
}

void crossos_scanner_free_result(crossos_scan_result_t *result)
{
    if (!result)
        return;
    free(result->pixels);
    free(result->raw16);
    result->pixels = NULL;
    result->raw16 = NULL;
    result->width = 0;
    result->height = 0;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Film curve helpers
 * ════════════════════════════════════════════════════════════════════════ */

void crossos_film_curve_identity(crossos_film_curve_t *curve)
{
    if (!curve)
        return;
    for (int i = 0; i < 256; i++)
    {
        curve->r[i] = (uint8_t)i;
        curve->g[i] = (uint8_t)i;
        curve->b[i] = (uint8_t)i;
    }
    curve->invert = 0;
    curve->name[0] = '\0';
}

/* Build a gamma-adjusted LUT: out = pow(in/255, g) * 255. */
static void build_gamma_lut(uint8_t *lut, float gamma)
{
    for (int i = 0; i < 256; i++)
    {
        float v = (float)i / 255.0f;
        v = powf(v, gamma);
        int out = (int)(v * 255.0f + 0.5f);
        if (out < 0)
            out = 0;
        if (out > 255)
            out = 255;
        lut[i] = (uint8_t)out;
    }
}

/* Build an S-curve LUT using a simple cubic Bezier-like approximation.
 * contrast in [0,1]: 0 = identity, 1 = strong S-curve.               */
static void build_scurve_lut(uint8_t *lut, float contrast, float gain)
{
    for (int i = 0; i < 256; i++)
    {
        float t = (float)i / 255.0f;
        /* Smooth step: 3t²-2t³ */
        float s = t * t * (3.0f - 2.0f * t);
        /* Blend between linear and smooth-step by contrast amount */
        float v = t * (1.0f - contrast) + s * contrast;
        /* Apply gain (brightens mid-tones) */
        v *= gain;
        if (v < 0.0f)
            v = 0.0f;
        if (v > 1.0f)
            v = 1.0f;
        lut[i] = (uint8_t)(v * 255.0f + 0.5f);
    }
}

/* Build a per-channel LUT with independent gamma per channel. */
static void build_color_lut(uint8_t *r_lut, uint8_t *g_lut, uint8_t *b_lut,
                            float gr, float gg, float gb)
{
    build_gamma_lut(r_lut, gr);
    build_gamma_lut(g_lut, gg);
    build_gamma_lut(b_lut, gb);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Film stock preset database
 *
 *  All colour-negative stocks include invert=1 because the scanner
 *  captures them as negatives (orange mask + inverted tones). The
 *  curves here approximate the characteristic curve of each stock
 *  after inversion and approximate cross-processing adjustments.
 *
 *  Slide films (E-6) use invert=0.
 *
 *  These are artistic approximations; the exact response depends on
 *  exposure, development, and scanning conditions.
 * ════════════════════════════════════════════════════════════════════════ */

void crossos_film_curve_get_preset(crossos_film_stock_t stock,
                                   crossos_film_curve_t *out_curve)
{
    if (!out_curve)
        return;
    crossos_film_curve_identity(out_curve);

    switch (stock)
    {
    case CROSSOS_FILM_STOCK_NONE:
        strncpy(out_curve->name, "None (flat)", sizeof(out_curve->name) - 1);
        break;

    /* ── Kodak Portra 400 ─────────────────────────────────────────────
     * Famous for warm skin tones, gentle contrast, wide latitude.
     * Slightly warmer (lower blue gamma), lifted shadows (gain > 1.0 for
     * mids).  Negative stock: invert=1.
     * ──────────────────────────────────────────────────────────────── */
    case CROSSOS_FILM_STOCK_KODAK_PORTRA_400:
        out_curve->invert = 1;
        build_color_lut(out_curve->r, out_curve->g, out_curve->b,
                        0.90f,  /* red:  slight warm lift  */
                        0.95f,  /* green: near-neutral     */
                        1.05f); /* blue: slightly cool off */
        /* Gentle S-curve over all channels for that Portra look */
        build_scurve_lut(out_curve->r, 0.20f, 1.0f);
        build_scurve_lut(out_curve->g, 0.18f, 1.0f);
        build_scurve_lut(out_curve->b, 0.15f, 0.95f);
        strncpy(out_curve->name, "Kodak Portra 400", sizeof(out_curve->name) - 1);
        break;

    /* ── Kodak Portra 160 ─────────────────────────────────────────────
     * Finer grain, slightly more saturated than 400.
     * ──────────────────────────────────────────────────────────────── */
    case CROSSOS_FILM_STOCK_KODAK_PORTRA_160:
        out_curve->invert = 1;
        build_color_lut(out_curve->r, out_curve->g, out_curve->b,
                        0.88f, 0.94f, 1.10f);
        build_scurve_lut(out_curve->r, 0.25f, 1.0f);
        build_scurve_lut(out_curve->g, 0.22f, 1.0f);
        build_scurve_lut(out_curve->b, 0.18f, 0.93f);
        strncpy(out_curve->name, "Kodak Portra 160", sizeof(out_curve->name) - 1);
        break;

    /* ── Kodak Gold 200 ───────────────────────────────────────────────
     * Consumer stock; warm, punchy highlights, moderate saturation.
     * ──────────────────────────────────────────────────────────────── */
    case CROSSOS_FILM_STOCK_KODAK_GOLD_200:
        out_curve->invert = 1;
        build_color_lut(out_curve->r, out_curve->g, out_curve->b,
                        0.88f, 0.96f, 1.15f);
        build_scurve_lut(out_curve->r, 0.30f, 1.02f);
        build_scurve_lut(out_curve->g, 0.26f, 1.0f);
        build_scurve_lut(out_curve->b, 0.20f, 0.90f);
        strncpy(out_curve->name, "Kodak Gold 200", sizeof(out_curve->name) - 1);
        break;

    /* ── Kodak Ektar 100 ──────────────────────────────────────────────
     * Ultra-fine grain, vivid saturation, especially in reds.
     * ──────────────────────────────────────────────────────────────── */
    case CROSSOS_FILM_STOCK_KODAK_EKTAR_100:
        out_curve->invert = 1;
        build_color_lut(out_curve->r, out_curve->g, out_curve->b,
                        0.82f, 0.92f, 1.18f);
        build_scurve_lut(out_curve->r, 0.38f, 1.03f);
        build_scurve_lut(out_curve->g, 0.32f, 1.0f);
        build_scurve_lut(out_curve->b, 0.28f, 0.88f);
        strncpy(out_curve->name, "Kodak Ektar 100", sizeof(out_curve->name) - 1);
        break;

    /* ── Kodak ColorPlus 200 ──────────────────────────────────────────
     * Budget consumer film; warm, slightly faded palette.
     * ──────────────────────────────────────────────────────────────── */
    case CROSSOS_FILM_STOCK_KODAK_COLORPLUS:
        out_curve->invert = 1;
        build_color_lut(out_curve->r, out_curve->g, out_curve->b,
                        0.90f, 0.97f, 1.12f);
        build_scurve_lut(out_curve->r, 0.22f, 1.01f);
        build_scurve_lut(out_curve->g, 0.20f, 0.99f);
        build_scurve_lut(out_curve->b, 0.15f, 0.91f);
        strncpy(out_curve->name, "Kodak ColorPlus 200", sizeof(out_curve->name) - 1);
        break;

    /* ── Fujifilm 400H ────────────────────────────────────────────────
     * Gorgeous pastel palette, cool-green cast, slightly low contrast.
     * Beloved for wedding/portrait work.
     * ──────────────────────────────────────────────────────────────── */
    case CROSSOS_FILM_STOCK_FUJI_400H:
        out_curve->invert = 1;
        build_color_lut(out_curve->r, out_curve->g, out_curve->b,
                        1.02f, 0.90f, 0.96f);
        build_scurve_lut(out_curve->r, 0.15f, 0.96f);
        build_scurve_lut(out_curve->g, 0.20f, 1.04f);
        build_scurve_lut(out_curve->b, 0.18f, 1.01f);
        strncpy(out_curve->name, "Fujifilm 400H", sizeof(out_curve->name) - 1);
        break;

    /* ── Fujifilm Superia 400 ─────────────────────────────────────────
     * Consumer film; natural colours, good sharpness.
     * ──────────────────────────────────────────────────────────────── */
    case CROSSOS_FILM_STOCK_FUJI_SUPERIA_400:
        out_curve->invert = 1;
        build_color_lut(out_curve->r, out_curve->g, out_curve->b,
                        1.00f, 0.93f, 0.98f);
        build_scurve_lut(out_curve->r, 0.22f, 0.98f);
        build_scurve_lut(out_curve->g, 0.24f, 1.02f);
        build_scurve_lut(out_curve->b, 0.20f, 1.00f);
        strncpy(out_curve->name, "Fujifilm Superia 400", sizeof(out_curve->name) - 1);
        break;

    /* ── Fujifilm C200 ────────────────────────────────────────────────
     * Budget stock; fine grain for slow film, punchy greens.
     * ──────────────────────────────────────────────────────────────── */
    case CROSSOS_FILM_STOCK_FUJI_C200:
        out_curve->invert = 1;
        build_color_lut(out_curve->r, out_curve->g, out_curve->b,
                        1.03f, 0.88f, 0.97f);
        build_scurve_lut(out_curve->r, 0.20f, 0.97f);
        build_scurve_lut(out_curve->g, 0.28f, 1.06f);
        build_scurve_lut(out_curve->b, 0.22f, 1.00f);
        strncpy(out_curve->name, "Fujifilm C200", sizeof(out_curve->name) - 1);
        break;

    /* ── Fujifilm Provia 100F (E-6 slide) ────────────────────────────
     * Neutral, accurate colours, high sharpness.  Positive film – no
     * inversion needed.
     * ──────────────────────────────────────────────────────────────── */
    case CROSSOS_FILM_STOCK_FUJI_PROVIA_100F:
        out_curve->invert = 0;
        build_color_lut(out_curve->r, out_curve->g, out_curve->b,
                        1.00f, 0.97f, 0.97f);
        build_scurve_lut(out_curve->r, 0.18f, 1.0f);
        build_scurve_lut(out_curve->g, 0.16f, 1.0f);
        build_scurve_lut(out_curve->b, 0.16f, 0.98f);
        strncpy(out_curve->name, "Fujifilm Provia 100F", sizeof(out_curve->name) - 1);
        break;

    /* ── Fujifilm Velvia 50 (E-6 slide) ──────────────────────────────
     * Ultra-saturated, punchy contrast, beloved for landscapes.
     * ──────────────────────────────────────────────────────────────── */
    case CROSSOS_FILM_STOCK_FUJI_VELVIA_50:
        out_curve->invert = 0;
        build_color_lut(out_curve->r, out_curve->g, out_curve->b,
                        0.85f, 0.82f, 0.88f);
        build_scurve_lut(out_curve->r, 0.45f, 1.0f);
        build_scurve_lut(out_curve->g, 0.50f, 1.0f);
        build_scurve_lut(out_curve->b, 0.40f, 1.02f);
        strncpy(out_curve->name, "Fujifilm Velvia 50", sizeof(out_curve->name) - 1);
        break;

    /* ── Fujifilm Velvia 100 (E-6 slide) ─────────────────────────────
     * Slightly less contrasty than Velvia 50, better highlight roll-off.
     * ──────────────────────────────────────────────────────────────── */
    case CROSSOS_FILM_STOCK_FUJI_VELVIA_100:
        out_curve->invert = 0;
        build_color_lut(out_curve->r, out_curve->g, out_curve->b,
                        0.87f, 0.84f, 0.90f);
        build_scurve_lut(out_curve->r, 0.40f, 1.0f);
        build_scurve_lut(out_curve->g, 0.44f, 1.0f);
        build_scurve_lut(out_curve->b, 0.36f, 1.01f);
        strncpy(out_curve->name, "Fujifilm Velvia 100", sizeof(out_curve->name) - 1);
        break;

    /* ── Kodak Ektachrome E100 (E-6 slide) ───────────────────────────
     * Neutral-to-cool, high resolution, retro / cinematic look.
     * ──────────────────────────────────────────────────────────────── */
    case CROSSOS_FILM_STOCK_KODAK_EKTACHROME:
        out_curve->invert = 0;
        build_color_lut(out_curve->r, out_curve->g, out_curve->b,
                        0.95f, 0.94f, 0.88f);
        build_scurve_lut(out_curve->r, 0.22f, 0.98f);
        build_scurve_lut(out_curve->g, 0.20f, 0.99f);
        build_scurve_lut(out_curve->b, 0.24f, 1.04f);
        strncpy(out_curve->name, "Kodak Ektachrome E100", sizeof(out_curve->name) - 1);
        break;

    /* ── Kodak Tri-X 400 (B&W negative) ─────────────────────────────
     * Classic grain, great tonal range. Scan as greyscale or colour;
     * either way invert the negative.
     * ──────────────────────────────────────────────────────────────── */
    case CROSSOS_FILM_STOCK_KODAK_TRIX_400:
        out_curve->invert = 1;
        build_gamma_lut(out_curve->r, 0.88f);
        memcpy(out_curve->g, out_curve->r, 256);
        memcpy(out_curve->b, out_curve->r, 256);
        /* S-curve for Tri-X characteristic 'bite' */
        build_scurve_lut(out_curve->r, 0.32f, 1.0f);
        memcpy(out_curve->g, out_curve->r, 256);
        memcpy(out_curve->b, out_curve->r, 256);
        strncpy(out_curve->name, "Kodak Tri-X 400", sizeof(out_curve->name) - 1);
        break;

    /* ── Kodak T-Max 400 (B&W negative) ─────────────────────────────
     * Very fine grain for a 400-speed film, more neutral tonal scale.
     * ──────────────────────────────────────────────────────────────── */
    case CROSSOS_FILM_STOCK_KODAK_TMAX_400:
        out_curve->invert = 1;
        build_gamma_lut(out_curve->r, 0.92f);
        memcpy(out_curve->g, out_curve->r, 256);
        memcpy(out_curve->b, out_curve->r, 256);
        build_scurve_lut(out_curve->r, 0.24f, 1.0f);
        memcpy(out_curve->g, out_curve->r, 256);
        memcpy(out_curve->b, out_curve->r, 256);
        strncpy(out_curve->name, "Kodak T-Max 400", sizeof(out_curve->name) - 1);
        break;

    /* ── Ilford HP5 Plus (B&W negative) ─────────────────────────────
     * Versatile, good contrast, traditional grain.
     * ──────────────────────────────────────────────────────────────── */
    case CROSSOS_FILM_STOCK_ILFORD_HP5:
        out_curve->invert = 1;
        build_gamma_lut(out_curve->r, 0.90f);
        memcpy(out_curve->g, out_curve->r, 256);
        memcpy(out_curve->b, out_curve->r, 256);
        build_scurve_lut(out_curve->r, 0.28f, 1.0f);
        memcpy(out_curve->g, out_curve->r, 256);
        memcpy(out_curve->b, out_curve->r, 256);
        strncpy(out_curve->name, "Ilford HP5 Plus", sizeof(out_curve->name) - 1);
        break;

    /* ── Ilford Delta 400 (B&W negative) ────────────────────────────
     * Tabular grain, slightly smoother tonal scale than HP5.
     * ──────────────────────────────────────────────────────────────── */
    case CROSSOS_FILM_STOCK_ILFORD_DELTA_400:
        out_curve->invert = 1;
        build_gamma_lut(out_curve->r, 0.94f);
        memcpy(out_curve->g, out_curve->r, 256);
        memcpy(out_curve->b, out_curve->r, 256);
        build_scurve_lut(out_curve->r, 0.20f, 1.0f);
        memcpy(out_curve->g, out_curve->r, 256);
        memcpy(out_curve->b, out_curve->r, 256);
        strncpy(out_curve->name, "Ilford Delta 400", sizeof(out_curve->name) - 1);
        break;

    /* ── Ilford FP4 Plus (B&W negative, ISO 125) ─────────────────────
     * Very fine grain, high resolution, good mid-tone separation.
     * ──────────────────────────────────────────────────────────────── */
    case CROSSOS_FILM_STOCK_ILFORD_FP4:
        out_curve->invert = 1;
        build_gamma_lut(out_curve->r, 0.96f);
        memcpy(out_curve->g, out_curve->r, 256);
        memcpy(out_curve->b, out_curve->r, 256);
        build_scurve_lut(out_curve->r, 0.16f, 1.0f);
        memcpy(out_curve->g, out_curve->r, 256);
        memcpy(out_curve->b, out_curve->r, 256);
        strncpy(out_curve->name, "Ilford FP4 Plus", sizeof(out_curve->name) - 1);
        break;

    default:
        strncpy(out_curve->name, "Unknown", sizeof(out_curve->name) - 1);
        break;
    }
}

const char *crossos_film_stock_name(crossos_film_stock_t stock)
{
    switch (stock)
    {
    case CROSSOS_FILM_STOCK_NONE:
        return "None";
    case CROSSOS_FILM_STOCK_KODAK_PORTRA_400:
        return "Kodak Portra 400";
    case CROSSOS_FILM_STOCK_KODAK_PORTRA_160:
        return "Kodak Portra 160";
    case CROSSOS_FILM_STOCK_KODAK_GOLD_200:
        return "Kodak Gold 200";
    case CROSSOS_FILM_STOCK_KODAK_EKTAR_100:
        return "Kodak Ektar 100";
    case CROSSOS_FILM_STOCK_KODAK_COLORPLUS:
        return "Kodak ColorPlus 200";
    case CROSSOS_FILM_STOCK_FUJI_400H:
        return "Fujifilm 400H";
    case CROSSOS_FILM_STOCK_FUJI_SUPERIA_400:
        return "Fujifilm Superia 400";
    case CROSSOS_FILM_STOCK_FUJI_C200:
        return "Fujifilm C200";
    case CROSSOS_FILM_STOCK_FUJI_PROVIA_100F:
        return "Fujifilm Provia 100F";
    case CROSSOS_FILM_STOCK_FUJI_VELVIA_50:
        return "Fujifilm Velvia 50";
    case CROSSOS_FILM_STOCK_FUJI_VELVIA_100:
        return "Fujifilm Velvia 100";
    case CROSSOS_FILM_STOCK_KODAK_EKTACHROME:
        return "Kodak Ektachrome E100";
    case CROSSOS_FILM_STOCK_KODAK_TRIX_400:
        return "Kodak Tri-X 400";
    case CROSSOS_FILM_STOCK_KODAK_TMAX_400:
        return "Kodak T-Max 400";
    case CROSSOS_FILM_STOCK_ILFORD_HP5:
        return "Ilford HP5 Plus";
    case CROSSOS_FILM_STOCK_ILFORD_DELTA_400:
        return "Ilford Delta 400";
    case CROSSOS_FILM_STOCK_ILFORD_FP4:
        return "Ilford FP4 Plus";
    default:
        return "Unknown";
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Film curve application
 * ════════════════════════════════════════════════════════════════════════ */

void crossos_film_apply_curve(crossos_scan_result_t *result,
                              const crossos_film_curve_t *curve,
                              float exposure)
{
    if (!result || !result->pixels)
        return;

    /* Build exposure lookup (output = clamp(input * 2^exposure, 0, 255)). */
    float exp_gain = powf(2.0f, exposure);
    uint8_t exp_lut[256];
    for (int i = 0; i < 256; i++)
    {
        int v = (int)((float)i * exp_gain + 0.5f);
        if (v < 0)
            v = 0;
        if (v > 255)
            v = 255;
        exp_lut[i] = (uint8_t)v;
    }

    uint8_t *p = result->pixels;
    int n = result->width * result->height;

    for (int i = 0; i < n; i++, p += 4)
    {
        uint8_t r = p[0];
        uint8_t g = p[1];
        uint8_t b = p[2];
        /* alpha p[3] is unchanged */

        /* Step 1: invert negative if required */
        if (curve && curve->invert)
        {
            r = (uint8_t)(255 - r);
            g = (uint8_t)(255 - g);
            b = (uint8_t)(255 - b);
        }

        /* Step 2: tone curve lookup */
        if (curve)
        {
            r = curve->r[r];
            g = curve->g[g];
            b = curve->b[b];
        }

        /* Step 3: exposure adjustment */
        p[0] = exp_lut[r];
        p[1] = exp_lut[g];
        p[2] = exp_lut[b];
    }
}
