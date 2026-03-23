/**
 * crossos/bluetooth.h  –  Bluetooth I/O helpers.
 *
 * Provides Bluetooth device discovery, RFCOMM connection management, and
 * data transfer across Windows, Linux (BlueZ), and Android.
 *
 * Typical usage:
 *
 *   static void on_device(const crossos_bt_device_t *dev, void *ud) {
 *       printf("Found: %s  %s  RSSI=%d\n", dev->name, dev->address, dev->rssi);
 *   }
 *
 *   crossos_bt_init();
 *   if (!crossos_bt_is_available()) { ... }
 *
 *   crossos_bt_scan_start(on_device, NULL);
 *   // ... wait a few seconds, then:
 *   crossos_bt_scan_stop();
 *
 *   crossos_bt_socket_t *sock = NULL;
 *   crossos_bt_connect("AA:BB:CC:DD:EE:FF", 1, &sock);
 *   crossos_bt_send(sock, "hello", 5, NULL);
 *
 *   unsigned char buf[256];
 *   size_t n = 0;
 *   crossos_bt_recv(sock, buf, sizeof(buf), &n);
 *
 *   crossos_bt_disconnect(sock);
 *   crossos_bt_shutdown();
 */

#ifndef CROSSOS_BLUETOOTH_H
#define CROSSOS_BLUETOOTH_H

#include "types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Constants ───────────────────────────────────────────────────────── */

/** Maximum valid RFCOMM channel number (channels 1–30 are user-allocatable). */
#define CROSSOS_BT_RFCOMM_MAX_CHANNEL 30

/* ── Device descriptor ───────────────────────────────────────────────── */

/** Describes a discovered or paired Bluetooth device. */
typedef struct crossos_bt_device {
    char name[64];    /**< Human-readable device name (UTF-8, may be empty) */
    char address[18]; /**< Colon-separated address: "XX:XX:XX:XX:XX:XX"     */
    int  rssi;        /**< Signal strength in dBm; 0 if unavailable         */
    int  is_paired;   /**< Non-zero if the device is already paired         */
    int  is_connected;/**< Non-zero if a connection is currently open        */
} crossos_bt_device_t;

/* ── Callbacks ───────────────────────────────────────────────────────── */

/**
 * Invoked for each device found during a scan.
 * The pointer @p dev is only valid for the duration of the callback.
 */
typedef void (*crossos_bt_scan_cb_t)(const crossos_bt_device_t *dev,
                                     void                      *user_data);

/* ── Opaque socket handle ────────────────────────────────────────────── */

/** Opaque handle representing an open RFCOMM Bluetooth socket. */
typedef struct crossos_bt_socket crossos_bt_socket_t;

/* ── Subsystem lifecycle ─────────────────────────────────────────────── */

/**
 * Initialise the Bluetooth subsystem.
 *
 * Must be called once before any other crossos_bt_* function.
 * Safe to call multiple times; subsequent calls are no-ops.
 *
 * @return CROSSOS_OK on success, CROSSOS_ERR_BLUETOOTH if the adapter
 *         cannot be opened, or CROSSOS_ERR_UNSUPPORT on unsupported
 *         platforms.
 */
crossos_result_t crossos_bt_init(void);

/**
 * Shut down the Bluetooth subsystem and release all resources.
 *
 * Disconnects any open sockets and stops any active scan.
 * After this call crossos_bt_init() must be invoked again before using
 * any other BT function.
 */
void crossos_bt_shutdown(void);

/**
 * Returns non-zero if a Bluetooth adapter is present and powered on.
 *
 * crossos_bt_init() must have succeeded before calling this.
 */
int crossos_bt_is_available(void);

/* ── Device discovery ────────────────────────────────────────────────── */

/**
 * Start an asynchronous Bluetooth scan.
 *
 * @p cb is invoked from the SDK's event-pump thread (or the calling thread
 * on platforms without background threading) each time a device is found.
 * Multiple scans may report the same device.
 *
 * @param cb         Callback to invoke for each discovered device; must not
 *                   be NULL.
 * @param user_data  Opaque pointer forwarded to @p cb unchanged.
 * @return           CROSSOS_OK if the scan was started, or an error code.
 */
crossos_result_t crossos_bt_scan_start(crossos_bt_scan_cb_t cb,
                                       void                *user_data);

/**
 * Stop the current scan (no-op if no scan is running).
 */
void crossos_bt_scan_stop(void);

/**
 * Return a snapshot of already-paired devices into @p out_devices.
 *
 * @param out_devices  Array with room for at least @p max_count entries.
 * @param max_count    Capacity of @p out_devices.
 * @return             Number of paired devices written (may be 0).
 */
int crossos_bt_get_paired(crossos_bt_device_t *out_devices, int max_count);

/* ── Connection ──────────────────────────────────────────────────────── */

/**
 * Open an RFCOMM connection to a remote device.
 *
 * @param address   Colon-separated Bluetooth address ("XX:XX:XX:XX:XX:XX").
 * @param channel   RFCOMM channel number (1–30).  Use 1 for devices that
 *                  advertise a single serial port profile service.
 * @param out_sock  On success, receives a new socket handle.  The caller
 *                  is responsible for calling crossos_bt_disconnect().
 * @return          CROSSOS_OK on success; CROSSOS_ERR_BLUETOOTH on failure.
 */
crossos_result_t crossos_bt_connect(const char           *address,
                                    uint8_t               channel,
                                    crossos_bt_socket_t **out_sock);

/**
 * Close an open Bluetooth socket and release its resources.
 *
 * Safe to call with NULL.  After this call @p sock must not be used.
 */
void crossos_bt_disconnect(crossos_bt_socket_t *sock);

/* ── Data transfer ───────────────────────────────────────────────────── */

/**
 * Send bytes over an open Bluetooth socket (blocking).
 *
 * @param sock        Open socket returned by crossos_bt_connect().
 * @param data        Pointer to the bytes to send.
 * @param len         Number of bytes to send.
 * @param out_sent    If non-NULL, receives the number of bytes actually sent.
 * @return            CROSSOS_OK on success; CROSSOS_ERR_BLUETOOTH on error.
 */
crossos_result_t crossos_bt_send(crossos_bt_socket_t *sock,
                                 const void          *data,
                                 size_t               len,
                                 size_t              *out_sent);

/**
 * Receive bytes from an open Bluetooth socket (blocking).
 *
 * Blocks until at least one byte is available or the connection closes.
 *
 * @param sock          Open socket.
 * @param buf           Buffer to receive data into.
 * @param buf_len       Size of @p buf in bytes.
 * @param out_received  If non-NULL, receives the number of bytes read.
 * @return              CROSSOS_OK on success; CROSSOS_ERR_BLUETOOTH on error
 *                      or if the remote side closed the connection.
 */
crossos_result_t crossos_bt_recv(crossos_bt_socket_t *sock,
                                 void                *buf,
                                 size_t               buf_len,
                                 size_t              *out_received);

/**
 * Non-blocking receive.
 *
 * Returns immediately.  If no data is available, @p out_received is set to
 * 0 and CROSSOS_OK is returned.
 *
 * @param sock          Open socket.
 * @param buf           Buffer to receive data into.
 * @param buf_len       Size of @p buf in bytes.
 * @param out_received  Receives bytes read (may be 0).
 * @return              CROSSOS_OK; CROSSOS_ERR_BLUETOOTH on hard error.
 */
crossos_result_t crossos_bt_recv_nonblocking(crossos_bt_socket_t *sock,
                                             void                *buf,
                                             size_t               buf_len,
                                             size_t              *out_received);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CROSSOS_BLUETOOTH_H */
