/**
 * crossos/websocket.h  –  RFC 6455 WebSocket client.
 *
 * Provides a callback-driven, full-duplex WebSocket client for use in games,
 * chat applications, cloud-backed UIs, and any real-time cross-platform
 * application built on CrossOS.
 *
 * Threading model
 * ───────────────
 * crossos_ws_connect() performs the TCP connection and HTTP Upgrade handshake
 * synchronously on the calling thread, then spawns a background receive
 * thread.  On success, on_open() fires before crossos_ws_connect() returns.
 * Subsequent callbacks (on_message, on_close, on_error) are called from the
 * background thread.  Use crossos_msgqueue to safely forward messages to the
 * main / rendering thread.
 *
 * Callback constraints
 * ────────────────────
 * • Do NOT call crossos_ws_destroy() from inside a callback – it will
 *   deadlock.  Set a flag and call destroy from the main thread instead.
 * • crossos_ws_send_text() and crossos_ws_send_binary() are thread-safe and
 *   may be called from any thread including a callback.
 *
 * Supported URLs
 * ──────────────
 * ws://host[:port]/path   – plain TCP (always available)
 * wss://host[:port]/path  – TLS (requires the library to be compiled with
 *                           OpenSSL or a platform TLS backend; otherwise
 *                           CROSSOS_ERR_UNSUPPORT is returned)
 *
 * Quick-start:
 *
 *   static void on_open(crossos_ws_t *ws, void *ud) {
 *       crossos_ws_send_text(ws, "hello server");
 *   }
 *   static void on_message(crossos_ws_t *ws,
 *                           const void *data, size_t size, int is_binary,
 *                           void *ud) {
 *       printf("recv: %.*s\n", (int)size, (const char *)data);
 *   }
 *   static void on_close(crossos_ws_t *ws, int code,
 *                         const char *reason, void *ud) { ... }
 *   static void on_error(crossos_ws_t *ws,
 *                         const char *msg, void *ud) { ... }
 *
 *   crossos_ws_t *ws = NULL;
 *   crossos_ws_callbacks_t cb = {on_open, on_message, on_close, on_error, NULL};
 *   if (crossos_ws_connect("ws://example.com/chat", &cb, &ws) == CROSSOS_OK) {
 *       // ...event loop...
 *       crossos_ws_destroy(ws);
 *   }
 */

#ifndef CROSSOS_WEBSOCKET_H
#define CROSSOS_WEBSOCKET_H

#include "types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Opaque handle ───────────────────────────────────────────────────── */

/** Opaque handle for a WebSocket connection. */
typedef struct crossos_ws crossos_ws_t;

/* ── Callbacks ───────────────────────────────────────────────────────── */

/**
 * Called once when the WebSocket handshake completes and the connection
 * is ready to send and receive messages.  Fires on the calling thread
 * (from within crossos_ws_connect).
 */
typedef void (*crossos_ws_on_open_cb)(crossos_ws_t *ws, void *user_data);

/**
 * Called each time a complete message frame (or reassembled fragmented
 * message) is received.  Fires from the background receive thread.
 *
 * @param ws        The connection.
 * @param data      Pointer to the message payload.  Valid only for the
 *                  duration of this callback; copy if you need it later.
 * @param size      Payload size in bytes.
 * @param is_binary Non-zero for binary frames (opcode 0x2);
 *                  zero for text frames (opcode 0x1, UTF-8).
 * @param user_data Value passed to crossos_ws_callbacks_t.
 */
typedef void (*crossos_ws_on_message_cb)(crossos_ws_t       *ws,
                                         const void         *data,
                                         size_t              size,
                                         int                 is_binary,
                                         void               *user_data);

/**
 * Called when the connection is closed, either by the remote peer or after
 * crossos_ws_close() completes the close handshake.
 *
 * @param code    WebSocket close status code (RFC 6455 §7.4) or 0 if unknown.
 * @param reason  NUL-terminated UTF-8 reason string (may be empty).
 */
typedef void (*crossos_ws_on_close_cb)(crossos_ws_t *ws,
                                       int           code,
                                       const char   *reason,
                                       void         *user_data);

/**
 * Called when a network or protocol error occurs.
 *
 * @param message  Human-readable description of the error.
 */
typedef void (*crossos_ws_on_error_cb)(crossos_ws_t *ws,
                                       const char   *message,
                                       void         *user_data);

/** Bundle of callbacks passed to crossos_ws_connect(). */
typedef struct crossos_ws_callbacks {
    crossos_ws_on_open_cb    on_open;    /**< May be NULL               */
    crossos_ws_on_message_cb on_message; /**< May be NULL               */
    crossos_ws_on_close_cb   on_close;   /**< May be NULL               */
    crossos_ws_on_error_cb   on_error;   /**< May be NULL               */
    void                    *user_data;  /**< Passed to every callback  */
} crossos_ws_callbacks_t;

/* ── Connection ──────────────────────────────────────────────────────── */

/**
 * Connect to a WebSocket server and perform the RFC 6455 opening handshake.
 *
 * Blocks until the TCP connection is established and the HTTP Upgrade
 * response is received.  On success, on_open fires before this function
 * returns, and a background receive thread is started.
 *
 * @param url        WebSocket URL – ws://host[:port]/path or
 *                   wss://host[:port]/path (TLS, requires optional OpenSSL).
 * @param callbacks  Event callbacks (and user_data pointer).  The struct is
 *                   copied internally; the caller need not keep it alive.
 * @param out_ws     Receives the connection handle on success.  The caller
 *                   must call crossos_ws_destroy() when done.
 * @return           CROSSOS_OK on success.
 *                   CROSSOS_ERR_NETWORK if the TCP/TLS connection failed.
 *                   CROSSOS_ERR_WS if the HTTP Upgrade was rejected.
 *                   CROSSOS_ERR_UNSUPPORT for wss:// without TLS support.
 *                   CROSSOS_ERR_PARAM for a NULL or malformed URL.
 *                   CROSSOS_ERR_OOM if allocation failed.
 */
crossos_result_t crossos_ws_connect(const char                   *url,
                                    const crossos_ws_callbacks_t *callbacks,
                                    crossos_ws_t                **out_ws);

/* ── Sending ─────────────────────────────────────────────────────────── */

/**
 * Send a UTF-8 text frame to the server.
 *
 * Thread-safe; may be called from any thread including a message callback.
 *
 * @param ws    Connection handle (must not be NULL).
 * @param text  NUL-terminated UTF-8 string.
 * @return      CROSSOS_OK, CROSSOS_ERR_PARAM, or CROSSOS_ERR_NETWORK.
 */
crossos_result_t crossos_ws_send_text(crossos_ws_t *ws, const char *text);

/**
 * Send a binary frame to the server.
 *
 * Thread-safe; may be called from any thread including a message callback.
 *
 * @param ws    Connection handle.
 * @param data  Pointer to the bytes to send.
 * @param size  Number of bytes.
 * @return      CROSSOS_OK, CROSSOS_ERR_PARAM, or CROSSOS_ERR_NETWORK.
 */
crossos_result_t crossos_ws_send_binary(crossos_ws_t *ws,
                                        const void   *data,
                                        size_t        size);

/* ── Close & destroy ─────────────────────────────────────────────────── */

/**
 * Initiate a graceful close handshake.
 *
 * Sends a Close frame with the given status code and reason, then waits
 * for the server's echoed Close frame or a network error.  The on_close
 * callback fires once the handshake completes (or immediately on error).
 *
 * Safe to call from any thread except a WebSocket callback.
 *
 * @param ws     Connection handle.
 * @param code   RFC 6455 status code (e.g. 1000 = normal closure).
 *               Pass 0 to omit the status code payload.
 * @param reason NUL-terminated close reason (may be NULL or empty).
 * @return       CROSSOS_OK or CROSSOS_ERR_NETWORK.
 */
crossos_result_t crossos_ws_close(crossos_ws_t *ws,
                                  int           code,
                                  const char   *reason);

/**
 * Destroy a WebSocket handle and release all associated resources.
 *
 * Forces the connection closed if it is still open, waits for the background
 * receive thread to exit, then frees all memory.
 *
 * Do NOT call this from inside a WebSocket callback.  The handle must not
 * be used after this call.  Safe to call with NULL.
 */
void crossos_ws_destroy(crossos_ws_t *ws);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CROSSOS_WEBSOCKET_H */
