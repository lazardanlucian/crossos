/**
 * crossos/msgqueue.h  –  Thread-safe bounded FIFO message queue.
 *
 * A crossos_msgqueue_t is a ring-buffer of fixed-capacity message slots.
 * Each slot stores a copy of up to max_msg_size bytes.  Multiple producers
 * and consumers may operate concurrently on the same queue.
 *
 * Typical pattern: push from a network / WebSocket callback thread and pop
 * from the main / rendering thread.  This decouples real-time I/O from UI
 * updates without introducing shared mutable state in application code.
 *
 * Example (producer thread):
 *
 *   static void on_message(crossos_ws_t *ws,
 *                           const void *data, size_t size,
 *                           int is_binary, void *ud) {
 *       crossos_msgqueue_t *q = (crossos_msgqueue_t *)ud;
 *       crossos_msgqueue_push(q, data, size, 0);  // non-blocking
 *   }
 *
 * Example (main thread inside draw loop):
 *
 *   char buf[4096];
 *   size_t sz;
 *   while (crossos_msgqueue_pop(q, buf, sizeof(buf), &sz, 0) == CROSSOS_OK) {
 *       // handle message
 *   }
 */

#ifndef CROSSOS_MSGQUEUE_H
#define CROSSOS_MSGQUEUE_H

#include "types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Opaque handle ───────────────────────────────────────────────────── */

/** Opaque handle for a message queue. */
typedef struct crossos_msgqueue crossos_msgqueue_t;

/* ── Lifecycle ───────────────────────────────────────────────────────── */

/**
 * Create a new message queue.
 *
 * @param capacity     Maximum number of messages that can be queued at once.
 *                     Must be ≥ 1.
 * @param max_msg_size Maximum size in bytes of any single message.
 *                     Must be ≥ 1.
 * @param out_queue    Receives the queue handle on success.
 * @return             CROSSOS_OK on success.
 *                     CROSSOS_ERR_PARAM if capacity or max_msg_size is 0.
 *                     CROSSOS_ERR_OOM if allocation fails.
 */
crossos_result_t crossos_msgqueue_create(size_t               capacity,
                                         size_t               max_msg_size,
                                         crossos_msgqueue_t **out_queue);

/**
 * Destroy a message queue and free all resources.
 *
 * Any threads blocked in crossos_msgqueue_push() or crossos_msgqueue_pop()
 * will be unblocked and receive CROSSOS_ERR_PARAM.
 *
 * Safe to call with NULL.  Must not be called while push or pop calls are
 * still running on the same queue from other threads.
 */
void crossos_msgqueue_destroy(crossos_msgqueue_t *queue);

/* ── Producers ───────────────────────────────────────────────────────── */

/**
 * Push a message onto the queue.
 *
 * Copies exactly @p size bytes from @p data into the queue.
 *
 * @param queue       The message queue.
 * @param data        Pointer to the message payload.
 * @param size        Payload size in bytes; must be ≤ max_msg_size.
 * @param timeout_ms  How long to wait if the queue is full:
 *                      -1  = block indefinitely until space is available,
 *                       0  = return immediately (non-blocking),
 *                      >0  = wait up to this many milliseconds.
 * @return            CROSSOS_OK on success.
 *                    CROSSOS_ERR_TIMEOUT if the queue remained full for the
 *                    entire timeout period.
 *                    CROSSOS_ERR_PARAM for NULL pointers or oversized @p size.
 */
crossos_result_t crossos_msgqueue_push(crossos_msgqueue_t *queue,
                                       const void         *data,
                                       size_t              size,
                                       int                 timeout_ms);

/* ── Consumers ───────────────────────────────────────────────────────── */

/**
 * Pop the oldest message from the queue.
 *
 * Copies the message payload into the caller-supplied buffer @p buf.
 *
 * @param queue       The message queue.
 * @param buf         Destination buffer; must be at least max_msg_size bytes.
 * @param buf_size    Size of @p buf in bytes.
 * @param out_size    Receives the number of bytes copied into @p buf.
 *                    May be NULL if the caller doesn't need the size.
 * @param timeout_ms  How long to wait if the queue is empty:
 *                      -1  = block indefinitely,
 *                       0  = non-blocking (returns CROSSOS_ERR_TIMEOUT
 *                            immediately if empty),
 *                      >0  = wait up to this many milliseconds.
 * @return            CROSSOS_OK on success.
 *                    CROSSOS_ERR_TIMEOUT if the queue remained empty.
 *                    CROSSOS_ERR_PARAM for NULL pointers or undersized @p buf.
 */
crossos_result_t crossos_msgqueue_pop(crossos_msgqueue_t *queue,
                                      void               *buf,
                                      size_t              buf_size,
                                      size_t             *out_size,
                                      int                 timeout_ms);

/* ── Inspection ──────────────────────────────────────────────────────── */

/**
 * Return the number of messages currently in the queue.
 *
 * The value may be stale by the time the caller reads it; use only as an
 * approximation (e.g., for UI indicators).
 */
size_t crossos_msgqueue_count(const crossos_msgqueue_t *queue);

/**
 * Return the maximum number of messages the queue can hold (the capacity
 * value passed to crossos_msgqueue_create).
 */
size_t crossos_msgqueue_capacity(const crossos_msgqueue_t *queue);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CROSSOS_MSGQUEUE_H */
