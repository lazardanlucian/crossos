/*
 * src/platform/linux/bluetooth_linux.c
 *
 * Linux Bluetooth backend using BlueZ kernel sockets.
 *
 * This backend uses the raw AF_BLUETOOTH / BTPROTO_RFCOMM socket API
 * available in the Linux kernel via the BlueZ stack.  No external
 * library (libbluetooth / dbus) is required at runtime – only kernel
 * headers are needed at compile time.
 *
 * Device discovery uses the HCI ioctl interface directly through an
 * HCI socket (BTPROTO_HCI), which is available without D-Bus.
 *
 * Note: scanning for remote devices requires CAP_NET_ADMIN or root on
 *       most distributions unless the kernel has been configured to
 *       allow unprivileged HCI access.
 *
 * If libbluetooth-dev is not installed, CROSSOS_HAS_BLUEZ will not be
 * defined and this file compiles to a set of unsupported stubs.
 */

#if defined(__linux__) && !defined(__ANDROID__)

#include <crossos/bluetooth.h>

#ifdef CROSSOS_HAS_BLUEZ

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

/* BlueZ kernel headers */
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/rfcomm.h>

/* ── Socket handle ────────────────────────────────────────────────────── */

struct crossos_bt_socket {
    int fd;
};

/* ── Subsystem state ──────────────────────────────────────────────────── */

static int s_hci_dev    = -1;  /* HCI device id of the default adapter */
static int s_initialised = 0;

/* ── Helpers ──────────────────────────────────────────────────────────── */

/* Convert "XX:XX:XX:XX:XX:XX" string to bdaddr_t. */
static void str_to_bdaddr(const char *str, bdaddr_t *ba)
{
    unsigned int b[6] = {0};
    sscanf(str, "%x:%x:%x:%x:%x:%x",
           &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
    for (int i = 0; i < 6; i++)
        ba->b[i] = (uint8_t)b[5 - i];
}

/* Convert bdaddr_t to "XX:XX:XX:XX:XX:XX" string. */
static void bdaddr_to_str(const bdaddr_t *ba, char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             ba->b[5], ba->b[4], ba->b[3], ba->b[2], ba->b[1], ba->b[0]);
}

/* ── Platform functions ───────────────────────────────────────────────── */

crossos_result_t bt_platform_init(void)
{
    if (s_initialised) return CROSSOS_OK;
    s_hci_dev = hci_get_route(NULL);
    if (s_hci_dev < 0) s_hci_dev = -1;
    s_initialised = 1;
    return CROSSOS_OK;
}

void bt_platform_shutdown(void)
{
    s_hci_dev    = -1;
    s_initialised = 0;
}

int bt_platform_is_available(void)
{
    return (s_initialised && s_hci_dev >= 0) ? 1 : 0;
}

crossos_result_t bt_platform_scan_start(crossos_bt_scan_cb_t cb, void *user)
{
    if (!cb) return CROSSOS_ERR_PARAM;
    if (s_hci_dev < 0) return CROSSOS_ERR_BLUETOOTH;

    int sock = hci_open_dev(s_hci_dev);
    if (sock < 0) return CROSSOS_ERR_BLUETOOTH;

    const int max_rsp = 255;
    const int inq_len = 8;
    const int flags   = IREQ_CACHE_FLUSH;

    inquiry_info *ii = malloc((size_t)max_rsp * sizeof(inquiry_info));
    if (!ii) { hci_close_dev(sock); return CROSSOS_ERR_OOM; }

    int num_rsp = hci_inquiry(s_hci_dev, inq_len, max_rsp, NULL, &ii, flags);
    if (num_rsp < 0) {
        free(ii);
        hci_close_dev(sock);
        return CROSSOS_ERR_BLUETOOTH;
    }

    for (int i = 0; i < num_rsp; i++) {
        crossos_bt_device_t dev;
        memset(&dev, 0, sizeof(dev));
        bdaddr_to_str(&ii[i].bdaddr, dev.address);
        hci_read_remote_name(sock, &ii[i].bdaddr,
                             (int)sizeof(dev.name) - 1,
                             dev.name, 2000);
        cb(&dev, user);
    }

    free(ii);
    hci_close_dev(sock);
    return CROSSOS_OK;
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
    int fd = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (fd < 0) return CROSSOS_ERR_BLUETOOTH;

    struct sockaddr_rc sa;
    memset(&sa, 0, sizeof(sa));
    sa.rc_family  = AF_BLUETOOTH;
    sa.rc_channel = channel;
    str_to_bdaddr(addr, &sa.rc_bdaddr);

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return CROSSOS_ERR_BLUETOOTH;
    }

    struct crossos_bt_socket *s = malloc(sizeof(*s));
    if (!s) { close(fd); return CROSSOS_ERR_OOM; }
    s->fd     = fd;
    *out_sock = s;
    return CROSSOS_OK;
}

void bt_platform_disconnect(crossos_bt_socket_t *sock)
{
    if (!sock) return;
    close(sock->fd);
    free(sock);
}

crossos_result_t bt_platform_send(crossos_bt_socket_t *sock,
                                  const void *data, size_t len,
                                  size_t *out_sent)
{
    ssize_t n = write(sock->fd, data, len);
    if (n < 0) return CROSSOS_ERR_BLUETOOTH;
    if (out_sent) *out_sent = (size_t)n;
    return CROSSOS_OK;
}

crossos_result_t bt_platform_recv(crossos_bt_socket_t *sock,
                                  void *buf, size_t buf_len,
                                  size_t *out_recv, int nonblocking)
{
    if (nonblocking) {
        int fl = fcntl(sock->fd, F_GETFL, 0);
        fcntl(sock->fd, F_SETFL, fl | O_NONBLOCK);
        ssize_t n = read(sock->fd, buf, buf_len);
        fcntl(sock->fd, F_SETFL, fl);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (out_recv) *out_recv = 0;
                return CROSSOS_OK;
            }
            return CROSSOS_ERR_BLUETOOTH;
        }
        if (out_recv) *out_recv = (size_t)n;
        return CROSSOS_OK;
    }
    ssize_t n = read(sock->fd, buf, buf_len);
    if (n < 0) return CROSSOS_ERR_BLUETOOTH;
    if (out_recv) *out_recv = (size_t)n;
    return CROSSOS_OK;
}

#else  /* CROSSOS_HAS_BLUEZ not defined – provide unsupported stubs */

#include <stddef.h>

struct crossos_bt_socket { int placeholder; };

crossos_result_t bt_platform_init(void)     { return CROSSOS_ERR_UNSUPPORT; }
void             bt_platform_shutdown(void) {}
int              bt_platform_is_available(void) { return 0; }

crossos_result_t bt_platform_scan_start(crossos_bt_scan_cb_t cb, void *u)
{ (void)cb; (void)u; return CROSSOS_ERR_UNSUPPORT; }

void bt_platform_scan_stop(void) {}

int bt_platform_get_paired(crossos_bt_device_t *o, int m)
{ (void)o; (void)m; return 0; }

crossos_result_t bt_platform_connect(const char *a, uint8_t c,
                                     crossos_bt_socket_t **s)
{ (void)a; (void)c; (void)s; return CROSSOS_ERR_UNSUPPORT; }

void bt_platform_disconnect(crossos_bt_socket_t *s) { (void)s; }

crossos_result_t bt_platform_send(crossos_bt_socket_t *s, const void *d,
                                  size_t l, size_t *sent)
{ (void)s; (void)d; (void)l; (void)sent; return CROSSOS_ERR_UNSUPPORT; }

crossos_result_t bt_platform_recv(crossos_bt_socket_t *s, void *b,
                                  size_t bl, size_t *r, int nb)
{ (void)s; (void)b; (void)bl; (void)r; (void)nb; return CROSSOS_ERR_UNSUPPORT; }

#endif /* CROSSOS_HAS_BLUEZ */

#endif /* __linux__ && !__ANDROID__ */
