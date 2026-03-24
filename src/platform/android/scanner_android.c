/*
 * src/platform/android/scanner_android.c
 *
 * CrossOS scanner backend stub for Android.
 *
 * ── Implementation path ────────────────────────────────────────────────
 *
 * Android supports USB Host mode (OTG) via the android.hardware.usb API.
 * Communicating with a Plustek OpticFilm scanner requires:
 *
 *   1. Requesting USB device permission at runtime
 *      android.hardware.usb.action.USB_DEVICE_ATTACHED intent +
 *      UsbManager.requestPermission()
 *
 *   2. Opening a bulk interface on the scanner
 *      UsbDeviceConnection.bulkTransfer() for control + data
 *
 *   3. Speaking the Plustek USB protocol directly
 *      The protocol is documented (reverse-engineered) in the SANE
 *      plustek backend.  Key source files:
 *        backends/plustek.c       – main state machine
 *        backends/plustek-usbio.c – USB command layer
 *        backends/plustek-usbscan.c – scan sequence
 *      USB Vendor ID: 0x07B3 (Plustek), Product IDs vary per model.
 *
 * Alternative: port the SANE plustek backend to Android NDK via
 * libusb-android (https://github.com/libusb/libusb) and call into
 * the existing sane backend C code from JNI.
 *
 * ── Plustek Vendor/Product IDs (partial list) ─────────────────────────
 *   0x07B3 : 0x0006  OpticFilm 7200 / 7200i
 *   0x07B3 : 0x000C  OpticFilm 7300
 *   0x07B3 : 0x000D  OpticFilm 7400
 *   0x07B3 : 0x000F  OpticFilm 7500i
 *   0x07B3 : 0x0014  OpticFilm 8100
 *   0x07B3 : 0x0017  OpticFilm 8200i
 *   0x07B3 : 0x0030  OpticFilm 135
 *   0x07B3 : 0x0032  OpticFilm 135 Plus
 *
 * ── Current status: stub ──────────────────────────────────────────────
 */

#include <crossos/scanner.h>
#include <string.h>

static crossos_result_t android_scan_init(void)
{
    /*
     * TODO:
     *   jclass  usbClass  = env->FindClass("android/hardware/usb/UsbManager");
     *   jobject usbMgr    = ... getSystemService(Context.USB_SERVICE);
     *   jobject deviceMap = usbMgr->getDeviceList();
     *   Iterate map, check VID 0x07B3 for Plustek devices.
     */
    return CROSSOS_ERR_UNSUPPORT;
}

static void android_scan_shutdown(void) {}

static int android_scan_enumerate(crossos_scanner_info_t *out, int max)
{
    (void)out;
    (void)max;
    /*
     * TODO: Query UsbManager.getDeviceList(), filter by VID 0x07B3.
     *       For each matching device fill out crossos_scanner_info_t:
     *         .vendor     = "Plustek"
     *         .model      = model name from product ID lookup table
     *         .type       = "film"
     *         .device_path= "usb:VID:PID:BUS:ADDR"
     *         .is_usb     = 1
     *         .is_plustek = 1
     */
    return 0;
}

static crossos_result_t android_scan_open(const char *path, void **out_handle)
{
    (void)path;
    (void)out_handle;
    /*
     * TODO:
     *   1. UsbManager.requestPermission(device, pendingIntent) — async
     *   2. On permission granted: UsbManager.openDevice(device)
     *   3. Find bulk-in (0x81) and bulk-out (0x02) endpoints
     *   4. UsbDeviceConnection.claimInterface(interface, true)
     *   5. Store connection + endpoints in handle
     */
    return CROSSOS_ERR_UNSUPPORT;
}

static void android_scan_close(void *handle)
{
    (void)handle;
    /*
     * TODO:
     *   UsbDeviceConnection.releaseInterface(interface)
     *   UsbDeviceConnection.close()
     */
}

static crossos_result_t android_scan_get_default_params(
    void *handle, crossos_scanner_params_t *out)
{
    (void)handle;
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

static crossos_result_t android_scan_scan(void *handle,
                                          const crossos_scanner_params_t *params,
                                          crossos_scan_result_t *out)
{
    (void)handle;
    (void)params;
    (void)out;
    /*
     * TODO (mirrors SANE plustek-usbscan.c state machine):
     *   1. Send SCAN_CMD_INIT bulk-out packet (Plustek vendor command set)
     *   2. Set lamp mode, resolution, scan area via vendor commands
     *   3. Send SCAN_CMD_START
     *   4. Read scan rows in chunks via UsbDeviceConnection.bulkTransfer()
     *      on endpoint 0x81 (bulk-in, 512-byte packets typical)
     *   5. Assemble raw RGB rows → decode to RGBA8888
     *   6. Send SCAN_CMD_STOP
     */
    return CROSSOS_ERR_UNSUPPORT;
}

static void android_scan_cancel(void *handle)
{
    (void)handle;
    /*
     * TODO: Send SCAN_CMD_STOP vendor command and drain endpoint.
     */
}

/* ════════════════════════════════════════════════════════════════════════
 *  Backend registration
 * ════════════════════════════════════════════════════════════════════════ */

static const crossos_scanner_backend_t s_android_backend = {
    .init = android_scan_init,
    .shutdown = android_scan_shutdown,
    .enumerate = android_scan_enumerate,
    .open = android_scan_open,
    .close = android_scan_close,
    .get_default_params = android_scan_get_default_params,
    .scan = android_scan_scan,
    .cancel = android_scan_cancel,
};

__attribute__((constructor)) static void register_android_scanner_backend(void)
{
    crossos_scanner_register_backend(&s_android_backend);
}
