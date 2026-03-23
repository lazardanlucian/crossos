/*
 * src/core/bluetooth.c  –  Bluetooth subsystem core.
 *
 * This file provides the platform-independent glue and dispatches to the
 * correct backend selected at compile time:
 *
 *   Windows  → bluetooth_win32.c  (WS2_32 + ws2bth.h)
 *   Linux    → bluetooth_linux.c  (BlueZ AF_BLUETOOTH sockets)
 *   Android  → bluetooth_android.c (JNI stub)
 *   Other    → stub (CROSSOS_ERR_UNSUPPORT)
 */

#include <crossos/bluetooth.h>

#include <string.h>
#include <stdlib.h>

/* ── Platform backend declarations ─────────────────────────────────── */

#if defined(_WIN32)

crossos_result_t bt_platform_init(void);
void             bt_platform_shutdown(void);
int              bt_platform_is_available(void);
crossos_result_t bt_platform_scan_start(crossos_bt_scan_cb_t cb, void *user);
void             bt_platform_scan_stop(void);
int              bt_platform_get_paired(crossos_bt_device_t *out, int max);
crossos_result_t bt_platform_connect(const char *addr, uint8_t channel,
                                     crossos_bt_socket_t **out_sock);
void             bt_platform_disconnect(crossos_bt_socket_t *sock);
crossos_result_t bt_platform_send(crossos_bt_socket_t *sock,
                                  const void *data, size_t len,
                                  size_t *out_sent);
crossos_result_t bt_platform_recv(crossos_bt_socket_t *sock,
                                  void *buf, size_t buf_len,
                                  size_t *out_recv, int nonblocking);

#elif defined(__ANDROID__)

crossos_result_t bt_platform_init(void);
void             bt_platform_shutdown(void);
int              bt_platform_is_available(void);
crossos_result_t bt_platform_scan_start(crossos_bt_scan_cb_t cb, void *user);
void             bt_platform_scan_stop(void);
int              bt_platform_get_paired(crossos_bt_device_t *out, int max);
crossos_result_t bt_platform_connect(const char *addr, uint8_t channel,
                                     crossos_bt_socket_t **out_sock);
void             bt_platform_disconnect(crossos_bt_socket_t *sock);
crossos_result_t bt_platform_send(crossos_bt_socket_t *sock,
                                  const void *data, size_t len,
                                  size_t *out_sent);
crossos_result_t bt_platform_recv(crossos_bt_socket_t *sock,
                                  void *buf, size_t buf_len,
                                  size_t *out_recv, int nonblocking);

#elif defined(__linux__)

crossos_result_t bt_platform_init(void);
void             bt_platform_shutdown(void);
int              bt_platform_is_available(void);
crossos_result_t bt_platform_scan_start(crossos_bt_scan_cb_t cb, void *user);
void             bt_platform_scan_stop(void);
int              bt_platform_get_paired(crossos_bt_device_t *out, int max);
crossos_result_t bt_platform_connect(const char *addr, uint8_t channel,
                                     crossos_bt_socket_t **out_sock);
void             bt_platform_disconnect(crossos_bt_socket_t *sock);
crossos_result_t bt_platform_send(crossos_bt_socket_t *sock,
                                  const void *data, size_t len,
                                  size_t *out_sent);
crossos_result_t bt_platform_recv(crossos_bt_socket_t *sock,
                                  void *buf, size_t buf_len,
                                  size_t *out_recv, int nonblocking);

#else /* Unsupported platform – stub everything out */

static crossos_result_t bt_platform_init(void) {
    return CROSSOS_ERR_UNSUPPORT;
}
static void bt_platform_shutdown(void) {}
static int  bt_platform_is_available(void) { return 0; }
static crossos_result_t bt_platform_scan_start(crossos_bt_scan_cb_t cb,
                                                void *user) {
    (void)cb; (void)user; return CROSSOS_ERR_UNSUPPORT;
}
static void bt_platform_scan_stop(void) {}
static int  bt_platform_get_paired(crossos_bt_device_t *out, int max) {
    (void)out; (void)max; return 0;
}
static crossos_result_t bt_platform_connect(const char *addr, uint8_t ch,
                                             crossos_bt_socket_t **s) {
    (void)addr; (void)ch; (void)s; return CROSSOS_ERR_UNSUPPORT;
}
static void bt_platform_disconnect(crossos_bt_socket_t *s) { (void)s; }
static crossos_result_t bt_platform_send(crossos_bt_socket_t *s,
                                          const void *d, size_t l,
                                          size_t *sent) {
    (void)s; (void)d; (void)l; (void)sent; return CROSSOS_ERR_UNSUPPORT;
}
static crossos_result_t bt_platform_recv(crossos_bt_socket_t *s,
                                          void *b, size_t bl,
                                          size_t *r, int nb) {
    (void)s; (void)b; (void)bl; (void)r; (void)nb; return CROSSOS_ERR_UNSUPPORT;
}

#endif /* platform selection */

/* ── Public API ──────────────────────────────────────────────────────── */

crossos_result_t crossos_bt_init(void)
{
    return bt_platform_init();
}

void crossos_bt_shutdown(void)
{
    bt_platform_shutdown();
}

int crossos_bt_is_available(void)
{
    return bt_platform_is_available();
}

crossos_result_t crossos_bt_scan_start(crossos_bt_scan_cb_t cb, void *user_data)
{
    if (!cb) return CROSSOS_ERR_PARAM;
    return bt_platform_scan_start(cb, user_data);
}

void crossos_bt_scan_stop(void)
{
    bt_platform_scan_stop();
}

int crossos_bt_get_paired(crossos_bt_device_t *out_devices, int max_count)
{
    if (!out_devices || max_count <= 0) return 0;
    return bt_platform_get_paired(out_devices, max_count);
}

crossos_result_t crossos_bt_connect(const char          *address,
                                    uint8_t              channel,
                                    crossos_bt_socket_t **out_sock)
{
    if (!address || !out_sock) return CROSSOS_ERR_PARAM;
    if (channel == 0 || channel > CROSSOS_BT_RFCOMM_MAX_CHANNEL) return CROSSOS_ERR_PARAM;
    return bt_platform_connect(address, channel, out_sock);
}

void crossos_bt_disconnect(crossos_bt_socket_t *sock)
{
    if (!sock) return;
    bt_platform_disconnect(sock);
}

crossos_result_t crossos_bt_send(crossos_bt_socket_t *sock,
                                 const void          *data,
                                 size_t               len,
                                 size_t              *out_sent)
{
    if (!sock || !data) return CROSSOS_ERR_PARAM;
    return bt_platform_send(sock, data, len, out_sent);
}

crossos_result_t crossos_bt_recv(crossos_bt_socket_t *sock,
                                 void                *buf,
                                 size_t               buf_len,
                                 size_t              *out_received)
{
    if (!sock || !buf || buf_len == 0) return CROSSOS_ERR_PARAM;
    return bt_platform_recv(sock, buf, buf_len, out_received, 0);
}

crossos_result_t crossos_bt_recv_nonblocking(crossos_bt_socket_t *sock,
                                             void                *buf,
                                             size_t               buf_len,
                                             size_t              *out_received)
{
    if (!sock || !buf || buf_len == 0) return CROSSOS_ERR_PARAM;
    return bt_platform_recv(sock, buf, buf_len, out_received, 1);
}
