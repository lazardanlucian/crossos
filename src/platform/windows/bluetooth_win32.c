/*
 * src/platform/windows/bluetooth_win32.c
 *
 * Windows Bluetooth backend using the WinSock2 Bluetooth socket API
 * (ws2bth.h / bthsdpdef.h).  Requires linking against ws2_32.dll.
 *
 * Discovery uses BluetoothFindFirstDevice / BluetoothFindNextDevice from
 * bthprops.dll (linked via BluetoothAPIs).
 */

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2bth.h>
#include <BluetoothAPIs.h>

#include <crossos/bluetooth.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "Bthprops.lib")

/* ── Socket handle ────────────────────────────────────────────────────── */

struct crossos_bt_socket {
    SOCKET fd;
};

/* ── Subsystem state ──────────────────────────────────────────────────── */

static int s_initialised = 0;

/* ── Helpers ──────────────────────────────────────────────────────────── */

/* Convert BLUETOOTH_ADDRESS to "XX:XX:XX:XX:XX:XX" string. */
static void ba_to_str(const BLUETOOTH_ADDRESS *ba, char out[18])
{
    BYTE *b = (BYTE *)ba->rgBytes;
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             b[5], b[4], b[3], b[2], b[1], b[0]);
}

/* Parse "XX:XX:XX:XX:XX:XX" into a BTH_ADDR (64-bit value). */
static BTH_ADDR str_to_bthaddr(const char *str)
{
    unsigned int b[6] = {0};
    sscanf(str, "%x:%x:%x:%x:%x:%x",
           &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
    BTH_ADDR addr = 0;
    for (int i = 0; i < 6; i++) {
        addr = (addr << 8) | (b[i] & 0xFF);
    }
    return addr;
}

/* ── Platform functions ───────────────────────────────────────────────── */

crossos_result_t bt_platform_init(void)
{
    if (s_initialised) return CROSSOS_OK;
    WSADATA wsd;
    if (WSAStartup(MAKEWORD(2, 2), &wsd) != 0) return CROSSOS_ERR_BLUETOOTH;
    s_initialised = 1;
    return CROSSOS_OK;
}

void bt_platform_shutdown(void)
{
    if (!s_initialised) return;
    WSACleanup();
    s_initialised = 0;
}

int bt_platform_is_available(void)
{
    if (!s_initialised) return 0;
    /* Try to open a BT socket – if it fails there is no adapter. */
    SOCKET test = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (test == INVALID_SOCKET) return 0;
    closesocket(test);
    return 1;
}

crossos_result_t bt_platform_scan_start(crossos_bt_scan_cb_t cb, void *user)
{
    if (!cb) return CROSSOS_ERR_PARAM;

    BLUETOOTH_DEVICE_SEARCH_PARAMS params;
    memset(&params, 0, sizeof(params));
    params.dwSize               = sizeof(params);
    params.fReturnAuthenticated = TRUE;
    params.fReturnRemembered    = TRUE;
    params.fReturnUnknown       = TRUE;
    params.fReturnConnected     = TRUE;
    params.fIssueInquiry        = TRUE;
    params.cTimeoutMultiplier   = 4;  /* ~5 seconds inquiry */

    BLUETOOTH_DEVICE_INFO info;
    memset(&info, 0, sizeof(info));
    info.dwSize = sizeof(info);

    HBLUETOOTH_DEVICE_FIND hFind = BluetoothFindFirstDevice(&params, &info);
    if (hFind == NULL) return CROSSOS_OK; /* no devices found, not an error */

    do {
        crossos_bt_device_t dev;
        memset(&dev, 0, sizeof(dev));

        /* Convert wide name to narrow */
        WideCharToMultiByte(CP_UTF8, 0,
                            info.szName, -1,
                            dev.name, (int)sizeof(dev.name) - 1,
                            NULL, NULL);
        ba_to_str(&info.Address, dev.address);
        dev.is_paired    = info.fAuthenticated ? 1 : 0;
        dev.is_connected = info.fConnected      ? 1 : 0;
        dev.rssi         = 0; /* WinBT API doesn't expose RSSI here */

        cb(&dev, user);
    } while (BluetoothFindNextDevice(hFind, &info));

    BluetoothFindDeviceClose(hFind);
    return CROSSOS_OK;
}

void bt_platform_scan_stop(void)
{
    /* Synchronous scan – nothing to stop. */
}

int bt_platform_get_paired(crossos_bt_device_t *out, int max)
{
    if (!out || max <= 0) return 0;

    BLUETOOTH_DEVICE_SEARCH_PARAMS params;
    memset(&params, 0, sizeof(params));
    params.dwSize               = sizeof(params);
    params.fReturnAuthenticated = TRUE;
    params.fReturnRemembered    = TRUE;
    params.fIssueInquiry        = FALSE;

    BLUETOOTH_DEVICE_INFO info;
    memset(&info, 0, sizeof(info));
    info.dwSize = sizeof(info);

    int count = 0;
    HBLUETOOTH_DEVICE_FIND hFind = BluetoothFindFirstDevice(&params, &info);
    if (!hFind) return 0;

    do {
        if (count >= max) break;
        memset(&out[count], 0, sizeof(out[count]));
        WideCharToMultiByte(CP_UTF8, 0,
                            info.szName, -1,
                            out[count].name, (int)sizeof(out[count].name) - 1,
                            NULL, NULL);
        ba_to_str(&info.Address, out[count].address);
        out[count].is_paired    = 1;
        out[count].is_connected = info.fConnected ? 1 : 0;
        count++;
    } while (BluetoothFindNextDevice(hFind, &info));

    BluetoothFindDeviceClose(hFind);
    return count;
}

crossos_result_t bt_platform_connect(const char          *addr,
                                     uint8_t              channel,
                                     crossos_bt_socket_t **out_sock)
{
    SOCKET fd = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (fd == INVALID_SOCKET) return CROSSOS_ERR_BLUETOOTH;

    SOCKADDR_BTH sa;
    memset(&sa, 0, sizeof(sa));
    sa.addressFamily = AF_BTH;
    sa.btAddr        = str_to_bthaddr(addr);
    sa.serviceClassId = RFCOMM_PROTOCOL_UUID;
    sa.port          = channel;

    if (connect(fd, (SOCKADDR *)&sa, sizeof(sa)) != 0) {
        closesocket(fd);
        return CROSSOS_ERR_BLUETOOTH;
    }

    struct crossos_bt_socket *s = malloc(sizeof(*s));
    if (!s) { closesocket(fd); return CROSSOS_ERR_OOM; }
    s->fd    = fd;
    *out_sock = s;
    return CROSSOS_OK;
}

void bt_platform_disconnect(crossos_bt_socket_t *sock)
{
    if (!sock) return;
    closesocket(sock->fd);
    free(sock);
}

crossos_result_t bt_platform_send(crossos_bt_socket_t *sock,
                                  const void *data, size_t len,
                                  size_t *out_sent)
{
    int n = send(sock->fd, (const char *)data, (int)len, 0);
    if (n < 0) return CROSSOS_ERR_BLUETOOTH;
    if (out_sent) *out_sent = (size_t)n;
    return CROSSOS_OK;
}

crossos_result_t bt_platform_recv(crossos_bt_socket_t *sock,
                                  void *buf, size_t buf_len,
                                  size_t *out_recv, int nonblocking)
{
    if (nonblocking) {
        /* Set non-blocking temporarily */
        u_long mode = 1;
        ioctlsocket(sock->fd, FIONBIO, &mode);
    }

    int n = recv(sock->fd, (char *)buf, (int)buf_len, 0);

    if (nonblocking) {
        u_long mode = 0;
        ioctlsocket(sock->fd, FIONBIO, &mode);
    }

    if (n < 0) {
        int err = WSAGetLastError();
        if (nonblocking && (err == WSAEWOULDBLOCK)) {
            if (out_recv) *out_recv = 0;
            return CROSSOS_OK;
        }
        return CROSSOS_ERR_BLUETOOTH;
    }
    if (out_recv) *out_recv = (size_t)n;
    return CROSSOS_OK;
}

#endif /* _WIN32 */
