/*
 * src/platform/android/bluetooth_android.c
 *
 * Android Bluetooth stub.
 *
 * Full Bluetooth support on Android requires calling into the
 * android.bluetooth Java API through JNI.  The native socket layer
 * (AF_BLUETOOTH) is not exposed to NDK applications by the Android
 * runtime.
 *
 * This stub returns CROSSOS_ERR_UNSUPPORT for all operations.  To add
 * real Bluetooth on Android:
 *   1. Implement the Java-side helpers in CrossOSNativeActivity.java
 *      (or a companion service) using BluetoothAdapter / BluetoothSocket.
 *   2. Forward calls from JNI into the CrossOS event queue via
 *      crossos_event_push() (see src/platform/android/android_internal.h).
 *   3. Replace the stubs below with JNI calls to those helpers.
 */

#ifdef __ANDROID__

#include <crossos/bluetooth.h>

struct crossos_bt_socket {
    int placeholder;
};

crossos_result_t bt_platform_init(void)
{
    return CROSSOS_ERR_UNSUPPORT;
}

void bt_platform_shutdown(void) {}

int bt_platform_is_available(void)
{
    return 0;
}

crossos_result_t bt_platform_scan_start(crossos_bt_scan_cb_t cb, void *user)
{
    (void)cb; (void)user;
    return CROSSOS_ERR_UNSUPPORT;
}

void bt_platform_scan_stop(void) {}

int bt_platform_get_paired(crossos_bt_device_t *out, int max)
{
    (void)out; (void)max;
    return 0;
}

crossos_result_t bt_platform_connect(const char          *addr,
                                     uint8_t              channel,
                                     crossos_bt_socket_t **out_sock)
{
    (void)addr; (void)channel; (void)out_sock;
    return CROSSOS_ERR_UNSUPPORT;
}

void bt_platform_disconnect(crossos_bt_socket_t *sock)
{
    (void)sock;
}

crossos_result_t bt_platform_send(crossos_bt_socket_t *sock,
                                  const void *data, size_t len,
                                  size_t *out_sent)
{
    (void)sock; (void)data; (void)len; (void)out_sent;
    return CROSSOS_ERR_UNSUPPORT;
}

crossos_result_t bt_platform_recv(crossos_bt_socket_t *sock,
                                  void *buf, size_t buf_len,
                                  size_t *out_recv, int nonblocking)
{
    (void)sock; (void)buf; (void)buf_len; (void)out_recv; (void)nonblocking;
    return CROSSOS_ERR_UNSUPPORT;
}

#endif /* __ANDROID__ */

/* Suppress ISO C "empty translation unit" pedantic warning. */
typedef int crossos_bt_android_dummy_t;
