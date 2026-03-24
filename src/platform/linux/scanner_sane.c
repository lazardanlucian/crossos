/*
 * src/platform/linux/scanner_sane.c
 *
 * CrossOS scanner backend for Linux using SANE (Scanner Access Now Easy).
 *
 * SANE is the de-facto standard scanner API on Linux and supports hundreds
 * of USB film scanners including the entire Plustek OpticFilm range:
 *   - OpticFilm 7200, 7200i, 7300, 7400, 7500i
 *   - OpticFilm 8100, 8200i, 8300i
 *   - OpticFilm 135 Plus, 135i
 *
 * The Plustek SANE backend is typically at:
 *   /usr/lib/sane/libsane-plustek.so
 *   Device strings: "plustek:libusb:001:004"
 *
 * Required package: libsane-dev  (apt install libsane-dev)
 * Link against:     -lsane
 *
 * ── Plustek OpticFilm 35mm film scan area ────────────────────────────────
 *   The 135-format (35mm) negative/slide holder covers a maximum of
 *   ~36.8 mm × ~25.0 mm.  A single frame is 36 mm × 24 mm.
 *   SANE option names (Plustek backend):
 *     tl-x, tl-y   – top-left corner (mm, Float)
 *     br-x, br-y   – bottom-right corner (mm, Float)
 *     resolution   – DPI (Int, value list: 50/75/150/300/600/1200/…)
 *     mode         – "Color"/"Gray"/"Lineart" (String)
 *     source       – "Film" (some models)
 *     depth        – 8 or 16 (Int)
 *
 * ── Reverse-engineering notes (WinPCAP / USB protocol) ────────────────────
 *   When WinPCAP probes arrive, the USB URBs from the Windows driver can be
 *   compared against the open-source plustek SANE backend source at:
 *     https://gitlab.com/sane-project/backends/-/tree/master/backend/plustek*
 *   Key files: plustek.c, plustek-usbio.c, plustek-usbdevs.c
 *   The protocol is mostly proprietary vendor commands on bulk endpoint 0x02
 *   (OUT) and 0x81 (IN).  The SANE backend already reverse-engineered it.
 */

#include <crossos/scanner.h>

#include <sane/sane.h>
#include <sane/saneopts.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ════════════════════════════════════════════════════════════════════════
 *  Option-lookup helpers
 * ════════════════════════════════════════════════════════════════════════ */

static int find_option(SANE_Handle h, const char *name)
{
    SANE_Int n = 0;
    SANE_Status st = sane_control_option(h, 0, SANE_ACTION_GET_VALUE, &n, NULL);
    if (st != SANE_STATUS_GOOD)
        return -1;

    for (int i = 1; i < (int)n; i++)
    {
        const SANE_Option_Descriptor *d = sane_get_option_descriptor(h, i);
        if (d && d->name && strcmp(d->name, name) == 0)
            return i;
    }
    return -1;
}

static SANE_Status set_string_option(SANE_Handle h, const char *name,
                                     const char *value)
{
    int idx = find_option(h, name);
    if (idx < 0)
        return SANE_STATUS_UNSUPPORTED;

    char buf[128];
    strncpy(buf, value, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    return sane_control_option(h, idx, SANE_ACTION_SET_VALUE, buf, NULL);
}

static SANE_Status set_int_option(SANE_Handle h, const char *name, int value)
{
    int idx = find_option(h, name);
    if (idx < 0)
        return SANE_STATUS_UNSUPPORTED;

    SANE_Int v = (SANE_Int)value;
    return sane_control_option(h, idx, SANE_ACTION_SET_VALUE, &v, NULL);
}

static SANE_Status set_float_option(SANE_Handle h, const char *name, float value)
{
    int idx = find_option(h, name);
    if (idx < 0)
        return SANE_STATUS_UNSUPPORTED;

    /* SANE uses SANE_Fixed (fixed-point, SANE_FIX macro). */
    SANE_Fixed v = SANE_FIX((double)value);
    return sane_control_option(h, idx, SANE_ACTION_SET_VALUE, &v, NULL);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Backend state
 * ════════════════════════════════════════════════════════════════════════ */

typedef struct
{
    SANE_Handle handle;
    int scanning; /* 1 if sane_start() has been called */
} sane_device_t;

static int s_sane_init = 0;

/* ════════════════════════════════════════════════════════════════════════
 *  Backend implementation
 * ════════════════════════════════════════════════════════════════════════ */

static crossos_result_t sane_backend_init(void)
{
    if (s_sane_init)
        return CROSSOS_OK;

    SANE_Int ver = 0;
    SANE_Status st = sane_init(&ver, NULL);
    if (st != SANE_STATUS_GOOD)
        return CROSSOS_ERR_SCANNER;

    s_sane_init = 1;
    return CROSSOS_OK;
}

static void sane_backend_shutdown(void)
{
    if (!s_sane_init)
        return;
    sane_exit();
    s_sane_init = 0;
}

static int sane_backend_enumerate(crossos_scanner_info_t *out, int max)
{
    if (!s_sane_init)
        return 0;

    const SANE_Device **devs = NULL;
    SANE_Status st = sane_get_devices(&devs, SANE_FALSE);
    if (st != SANE_STATUS_GOOD || !devs)
        return 0;

    int count = 0;
    for (int i = 0; devs[i] && count < max; i++, count++)
    {
        crossos_scanner_info_t *info = &out[count];
        memset(info, 0, sizeof(*info));
        info->index = count;

        if (devs[i]->name)
            strncpy(info->device_path, devs[i]->name,
                    CROSSOS_SCANNER_PATH_MAX - 1);

        if (devs[i]->vendor)
            strncpy(info->vendor, devs[i]->vendor,
                    CROSSOS_SCANNER_VENDOR_MAX - 1);

        if (devs[i]->model)
            strncpy(info->model, devs[i]->model,
                    CROSSOS_SCANNER_MODEL_MAX - 1);

        /* Compose a human-readable name: "Vendor Model" */
        if (devs[i]->vendor && devs[i]->model)
            snprintf(info->name, CROSSOS_SCANNER_NAME_MAX, "%s %s",
                     devs[i]->vendor, devs[i]->model);
        else if (devs[i]->model)
            strncpy(info->name, devs[i]->model, CROSSOS_SCANNER_NAME_MAX - 1);
        else
            strncpy(info->name, info->device_path, CROSSOS_SCANNER_NAME_MAX - 1);

        if (devs[i]->type)
            strncpy(info->type, devs[i]->type, CROSSOS_SCANNER_TYPE_MAX - 1);
        else
            strncpy(info->type, CROSSOS_SCANNER_TYPE_UNKNOWN,
                    CROSSOS_SCANNER_TYPE_MAX - 1);

        /* Mark USB devices (Plustek scanners are all USB) */
        if (strstr(info->device_path, "libusb") ||
            strstr(info->device_path, "usb"))
            info->is_usb = 1;

        /* Mark Plustek devices for caller convenience */
        if (strstr(info->device_path, "plustek") ||
            (devs[i]->vendor && strstr(devs[i]->vendor, "Plustek")) ||
            (devs[i]->vendor && strstr(devs[i]->vendor, "PLUSTEK")))
            info->is_plustek = 1;
    }
    return count;
}

static crossos_result_t sane_backend_open(const char *path, void **out_handle)
{
    sane_device_t *dev = calloc(1, sizeof(*dev));
    if (!dev)
        return CROSSOS_ERR_OOM;

    SANE_Status st = sane_open(path, &dev->handle);
    if (st != SANE_STATUS_GOOD)
    {
        free(dev);
        return CROSSOS_ERR_SCANNER;
    }
    *out_handle = dev;
    return CROSSOS_OK;
}

static void sane_backend_close(void *handle)
{
    if (!handle)
        return;
    sane_device_t *dev = (sane_device_t *)handle;
    if (dev->scanning)
    {
        sane_cancel(dev->handle);
        dev->scanning = 0;
    }
    sane_close(dev->handle);
    free(dev);
}

static crossos_result_t sane_backend_get_default_params(
    void *handle,
    crossos_scanner_params_t *out)
{
    (void)handle; /* Could query device constraints; use fixed defaults for now */

    out->resolution = 1200;
    out->color_mode = CROSSOS_SCANNER_COLOR_MODE_COLOR;
    out->bit_depth = CROSSOS_SCANNER_DEPTH_8;
    out->area.x = 0.0f;
    out->area.y = 0.0f;
    out->area.width = 36.0f;
    out->area.height = 24.0f;
    out->preview = 0;
    return CROSSOS_OK;
}

static crossos_result_t sane_backend_scan(void *handle,
                                          const crossos_scanner_params_t *params,
                                          crossos_scan_result_t *out)
{
    if (!handle || !params || !out)
        return CROSSOS_ERR_PARAM;
    sane_device_t *dev = (sane_device_t *)handle;

    /* ── Apply scan parameters ──────────────────────────────────────── */

    /* Resolution */
    set_int_option(dev->handle, SANE_NAME_SCAN_RESOLUTION, params->resolution);

    /* Colour mode */
    switch (params->color_mode)
    {
    case CROSSOS_SCANNER_COLOR_MODE_COLOR:
        set_string_option(dev->handle, SANE_NAME_SCAN_MODE, SANE_VALUE_SCAN_MODE_COLOR);
        break;
    case CROSSOS_SCANNER_COLOR_MODE_GRAY:
        set_string_option(dev->handle, SANE_NAME_SCAN_MODE, SANE_VALUE_SCAN_MODE_GRAY);
        break;
    case CROSSOS_SCANNER_COLOR_MODE_LINEART:
        set_string_option(dev->handle, SANE_NAME_SCAN_MODE, SANE_VALUE_SCAN_MODE_LINEART);
        break;
    }

    /* Bit depth */
    set_int_option(dev->handle, SANE_NAME_BIT_DEPTH, params->bit_depth);

    /* Scan area */
    set_float_option(dev->handle, SANE_NAME_SCAN_TL_X, params->area.x);
    set_float_option(dev->handle, SANE_NAME_SCAN_TL_Y, params->area.y);
    set_float_option(dev->handle, SANE_NAME_SCAN_BR_X,
                     params->area.x + params->area.width);
    set_float_option(dev->handle, SANE_NAME_SCAN_BR_Y,
                     params->area.y + params->area.height);

    /* Preview mode — some backends use a dedicated "preview" option */
    if (params->preview)
        set_int_option(dev->handle, "preview", SANE_TRUE);

    /* ── Start scan ─────────────────────────────────────────────────── */

    SANE_Status st = sane_start(dev->handle);
    if (st != SANE_STATUS_GOOD)
        return CROSSOS_ERR_SCANNER;
    dev->scanning = 1;

    /* ── Read scan parameters ───────────────────────────────────────── */

    SANE_Parameters sp;
    st = sane_get_parameters(dev->handle, &sp);
    if (st != SANE_STATUS_GOOD)
    {
        sane_cancel(dev->handle);
        dev->scanning = 0;
        return CROSSOS_ERR_SCANNER;
    }

    int w = sp.pixels_per_line;
    int h = sp.lines; /* may be -1 for streaming scanners */
    if (w <= 0 || h < 0)
    {
        sane_cancel(dev->handle);
        dev->scanning = 0;
        return CROSSOS_ERR_SCANNER;
    }

    /* Handle h == -1 (streaming) by reading until EOF and growing buffer */
    int streaming = (h == 0);
    if (streaming)
        h = 4096; /* initial guess */

    int bytes_per_pixel = (sp.format == SANE_FRAME_RGB) ? 3 : 1;
    int row_bytes = w * bytes_per_pixel * (sp.depth > 8 ? 2 : 1);

    /* Allocate raw scan buffer */
    size_t raw_cap = (size_t)row_bytes * (size_t)h;
    uint8_t *raw = malloc(raw_cap);
    if (!raw)
    {
        sane_cancel(dev->handle);
        dev->scanning = 0;
        return CROSSOS_ERR_OOM;
    }

    /* ── Read loop ──────────────────────────────────────────────────── */

    size_t raw_len = 0;
    while (1)
    {
        SANE_Int got = 0;
        if (raw_len >= raw_cap)
        {
            /* Grow buffer by 50% if streaming */
            size_t new_cap = raw_cap + raw_cap / 2;
            uint8_t *tmp = realloc(raw, new_cap);
            if (!tmp)
            {
                free(raw);
                sane_cancel(dev->handle);
                dev->scanning = 0;
                return CROSSOS_ERR_OOM;
            }
            raw = tmp;
            raw_cap = new_cap;
        }
        size_t chunk = raw_cap - raw_len;
        if (chunk > 65536)
            chunk = 65536;

        st = sane_read(dev->handle, raw + raw_len, (SANE_Int)chunk, &got);
        if (got > 0)
            raw_len += (size_t)got;

        if (st == SANE_STATUS_EOF)
            break;
        if (st != SANE_STATUS_GOOD)
        {
            free(raw);
            sane_cancel(dev->handle);
            dev->scanning = 0;
            return CROSSOS_ERR_SCANNER;
        }
    }

    sane_cancel(dev->handle);
    dev->scanning = 0;

    /* Recompute true height from bytes read if streaming */
    if (streaming && row_bytes > 0)
        h = (int)(raw_len / (size_t)row_bytes);
    if (h <= 0)
    {
        free(raw);
        return CROSSOS_ERR_SCANNER;
    }

    /* ── Convert raw to RGBA8888 ─────────────────────────────────────── */

    uint8_t *rgba = malloc((size_t)w * (size_t)h * 4);
    if (!rgba)
    {
        free(raw);
        return CROSSOS_ERR_OOM;
    }

    for (int row = 0; row < h; row++)
    {
        for (int col = 0; col < w; col++)
        {
            uint8_t r, g, b;
            int src_off = row * row_bytes + col * bytes_per_pixel * (sp.depth > 8 ? 2 : 1);

            if (sp.format == SANE_FRAME_RGB)
            {
                if (sp.depth > 8)
                {
                    /* 16-bit: high byte first (big-endian SANE) */
                    r = raw[src_off + 0];
                    g = raw[src_off + 2];
                    b = raw[src_off + 4];
                }
                else
                {
                    r = raw[src_off + 0];
                    g = raw[src_off + 1];
                    b = raw[src_off + 2];
                }
            }
            else
            {
                /* Grey / Lineart → replicate to RGB */
                uint8_t luma = sp.depth > 8 ? raw[src_off] : raw[src_off];
                r = g = b = luma;
            }

            int dst_off = (row * w + col) * 4;
            rgba[dst_off + 0] = r;
            rgba[dst_off + 1] = g;
            rgba[dst_off + 2] = b;
            rgba[dst_off + 3] = 255;
        }
    }

    /* Store 16-bit linear buffer if the scan was 16-bit */
    uint16_t *raw16_out = NULL;
    if (sp.depth > 8 && sp.format == SANE_FRAME_RGB)
    {
        raw16_out = malloc((size_t)w * (size_t)h * 3 * sizeof(uint16_t));
        if (raw16_out)
        {
            /* raw16: interleaved RGB, 16-bit little-endian */
            /* SANE delivers big-endian; swap bytes */
            size_t px = (size_t)w * (size_t)h;
            for (size_t i = 0; i < px * 3; i++)
            {
                size_t src = i * 2;
                raw16_out[i] = (uint16_t)((raw[src] << 8) | raw[src + 1]);
            }
        }
    }

    free(raw);

    out->pixels = rgba;
    out->width = w;
    out->height = h;
    out->stride = w * 4;
    out->resolution_dpi = params->resolution;
    out->bits_per_channel = sp.depth;
    out->raw16 = raw16_out;

    return CROSSOS_OK;
}

static void sane_backend_cancel(void *handle)
{
    if (!handle)
        return;
    sane_device_t *dev = (sane_device_t *)handle;
    if (dev->scanning)
    {
        sane_cancel(dev->handle);
        dev->scanning = 0;
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  Backend registration (called at library init time via constructor)
 * ════════════════════════════════════════════════════════════════════════ */

static const crossos_scanner_backend_t s_sane_backend = {
    .init = sane_backend_init,
    .shutdown = sane_backend_shutdown,
    .enumerate = sane_backend_enumerate,
    .open = sane_backend_open,
    .close = sane_backend_close,
    .get_default_params = sane_backend_get_default_params,
    .scan = sane_backend_scan,
    .cancel = sane_backend_cancel,
};

__attribute__((constructor)) static void register_sane_backend(void)
{
    crossos_scanner_register_backend(&s_sane_backend);
}
