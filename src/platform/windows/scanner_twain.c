/*
 * src/platform/windows/scanner_twain.c
 *
 * CrossOS scanner backend stub for Windows.
 *
 * ── Implementation path ────────────────────────────────────────────────
 *
 * Windows exposes scanners through two APIs:
 *
 *   1. Windows Image Acquisition (WIA) 2.0  — preferred, Vista+
 *      Headers:  <wia.h>, <wia_lh.h>
 *      Link:     wiaguid.lib, ole32.lib, oleaut32.lib
 *      Access:   IWiaDevMgr2 / IWiaItem2 COM interfaces
 *
 *   2. TWAIN 2.x  — legacy but universal; Plustek ships a TWAIN driver
 *      Header:   twain.h  (available from https://github.com/twain/twain-cs)
 *      DLL:      TWAIN_32.DLL or twaindsm.dll (Data Source Manager)
 *
 * For the Plustek OpticFilm on Windows:
 *   - The official driver ships a WIA minidriver AND a TWAIN datasource.
 *   - WIA path recommended for modern apps.
 *   - TWAIN path needed for older software compatibility.
 *
 * ── WinPCAP reverse-engineering plan ──────────────────────────────────
 *
 * When WinPCAP USB probes arrive:
 *   1. Capture traffic with USBPcap + Wireshark (dissector: USB, class 0xFF
 *      vendor-specific for Plustek bulk transfers)
 *   2. Identify control transfers on endpoint 0 (device setup / status)
 *   3. Identify bulk OUT (ep 0x02) commands and bulk IN (ep 0x81) responses
 *   4. Map to SANE plustek backend source for cross-reference:
 *      https://gitlab.com/sane-project/backends/-/blob/master/backend/plustek.c
 *   5. Implement raw USB access via WinUSB or libusb-win32 as a fallback
 *
 * ── Current status: stub ──────────────────────────────────────────────
 * All functions return CROSSOS_ERR_UNSUPPORT until the WIA/TWAIN
 * implementation is wired in.
 */

#include <crossos/scanner.h>
#include <string.h>

/* ════════════════════════════════════════════════════════════════════════
 *  Backend implementation (stubs)
 * ════════════════════════════════════════════════════════════════════════ */

static crossos_result_t twain_init(void)
{
    /*
     * TODO: CoInitializeEx(NULL, COINIT_MULTITHREADED);
     *       Create IWiaDevMgr2 via CoCreateInstance(CLSID_WiaDevMgr2, ...)
     */
    return CROSSOS_ERR_UNSUPPORT;
}

static void twain_shutdown(void)
{
    /* TODO: Release IWiaDevMgr2, CoUninitialize */
}

static int twain_enumerate(crossos_scanner_info_t *out, int max)
{
    (void)out;
    (void)max;
    /*
     * TODO: IWiaDevMgr2::EnumDeviceInfo(WIA_DEVINFO_ENUM_ALL, &pEnum)
     *       For each IWiaPropertyStorage: read WIA_DIP_DEV_NAME,
     *       WIA_DIP_DEV_TYPE, WIA_DIP_DEV_DESC.
     *
     * Plustek devices appear as:
     *   WIA_DIP_DEV_NAME  = "PLUSTEK OpticFilm 8100"
     *   WIA_DIP_DEV_TYPE  = StiDeviceTypeScanner
     */
    return 0;
}

static crossos_result_t twain_open(const char *path, void **out_handle)
{
    (void)path;
    (void)out_handle;
    /*
     * TODO: IWiaDevMgr2::CreateDevice(device_id, &pWiaDevice)
     *       Store IWiaItem2* in handle
     */
    return CROSSOS_ERR_UNSUPPORT;
}

static void twain_close(void *handle)
{
    (void)handle;
    /* TODO: Release IWiaItem2* */
}

static crossos_result_t twain_get_default_params(void *handle,
                                                 crossos_scanner_params_t *out)
{
    (void)handle;
    /* Sensible defaults for a Plustek 35mm film scanner */
    out->resolution = 1200;
    out->color_mode = CROSSOS_SCANNER_COLOR_MODE_COLOR;
    out->bit_depth = CROSSOS_SCANNER_DEPTH_8;
    out->area.x = 0.0f;
    out->area.y = 0.0f;
    out->area.width = 36.0f;
    out->area.height = 24.0f;
    out->preview = 0;
    return CROSSOS_ERR_UNSUPPORT;
}

static crossos_result_t twain_scan(void *handle,
                                   const crossos_scanner_params_t *params,
                                   crossos_scan_result_t *out)
{
    (void)handle;
    (void)params;
    (void)out;
    /*
     * TODO:
     *   1. IWiaItem2::QueryInterface(IID_IWiaTransfer, &pTransfer)
     *   2. Set scan properties via IWiaPropertyStorage:
     *      WIA_IPS_XRES, WIA_IPS_YRES (resolution)
     *      WIA_IPS_XPOS, WIA_IPS_YPOS, WIA_IPS_XEXTENT, WIA_IPS_YEXTENT
     *      WIA_IPS_DATATYPE (WIA_DATA_COLOR / WIA_DATA_GRAYSCALE)
     *      WIA_IPS_DEPTH (8 or 16)
     *   3. IWiaTransfer::Download(0, &callback)
     *   4. In callback::GetNextStream: write incoming bytes to buffer
     *   5. After transfer: decode DIB/BMP to RGBA8888
     */
    return CROSSOS_ERR_UNSUPPORT;
}

static void twain_cancel(void *handle)
{
    (void)handle;
    /* TODO: Set cancel flag; WIA callbacks check it between chunks */
}

/* ════════════════════════════════════════════════════════════════════════
 *  Backend registration
 * ════════════════════════════════════════════════════════════════════════ */

static const crossos_scanner_backend_t s_twain_backend = {
    .init = twain_init,
    .shutdown = twain_shutdown,
    .enumerate = twain_enumerate,
    .open = twain_open,
    .close = twain_close,
    .get_default_params = twain_get_default_params,
    .scan = twain_scan,
    .cancel = twain_cancel,
};

/* Called before main() via MSVC pragma init_seg or GNU constructor */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((constructor))
#endif
static void register_twain_backend(void)
{
    crossos_scanner_register_backend(&s_twain_backend);
}

#if defined(_MSC_VER)
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) static void (*_twain_init_ptr)(void) = register_twain_backend;
#endif
