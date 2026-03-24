/*
 * src/core/msgqueue.c  –  Thread-safe bounded FIFO message queue.
 *
 * Implemented as a ring buffer of fixed-size slots protected by a mutex and
 * two condition variables (not_empty, not_full).  Works on POSIX (pthreads)
 * and Windows Vista+ (CRITICAL_SECTION + CONDITION_VARIABLE).
 *
 * Slot layout inside the flat `slots` buffer:
 *   [ size_t actual_size | char data[max_msg_size] ]  × capacity
 *
 * The stride between slots is (sizeof(size_t) + max_msg_size).
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <crossos/msgqueue.h>

#include <stdlib.h>
#include <string.h>

/* ── Platform sync primitives ────────────────────────────────────────── */

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <synchapi.h>

typedef CRITICAL_SECTION   mq_mutex_t;
typedef CONDITION_VARIABLE mq_cond_t;

static void mq_mutex_init(mq_mutex_t *m)    { InitializeCriticalSection(m); }
static void mq_mutex_lock(mq_mutex_t *m)    { EnterCriticalSection(m); }
static void mq_mutex_unlock(mq_mutex_t *m)  { LeaveCriticalSection(m); }
static void mq_mutex_destroy(mq_mutex_t *m) { DeleteCriticalSection(m); }

static void mq_cond_init(mq_cond_t *c)      { InitializeConditionVariable(c); }
static void mq_cond_signal(mq_cond_t *c)    { WakeConditionVariable(c); }
static void mq_cond_broadcast(mq_cond_t *c) { WakeAllConditionVariable(c); }
static void mq_cond_destroy(mq_cond_t *c)   { (void)c; }

/* Returns 1 if signalled before timeout, 0 on timeout. */
static int mq_cond_timedwait(mq_cond_t *c, mq_mutex_t *m, int timeout_ms)
{
    DWORD ms = (timeout_ms < 0) ? INFINITE : (DWORD)timeout_ms;
    return SleepConditionVariableCS(c, m, ms) != 0;
}

#else /* POSIX */

#include <pthread.h>
#include <time.h>
#include <errno.h>

typedef pthread_mutex_t mq_mutex_t;
typedef pthread_cond_t  mq_cond_t;

static void mq_mutex_init(mq_mutex_t *m)    { pthread_mutex_init(m, NULL); }
static void mq_mutex_lock(mq_mutex_t *m)    { pthread_mutex_lock(m); }
static void mq_mutex_unlock(mq_mutex_t *m)  { pthread_mutex_unlock(m); }
static void mq_mutex_destroy(mq_mutex_t *m) { pthread_mutex_destroy(m); }

static void mq_cond_init(mq_cond_t *c)      { pthread_cond_init(c, NULL); }
static void mq_cond_signal(mq_cond_t *c)    { pthread_cond_signal(c); }
static void mq_cond_broadcast(mq_cond_t *c) { pthread_cond_broadcast(c); }
static void mq_cond_destroy(mq_cond_t *c)   { pthread_cond_destroy(c); }

static int mq_cond_timedwait(mq_cond_t *c, mq_mutex_t *m, int timeout_ms)
{
    if (timeout_ms < 0) {
        pthread_cond_wait(c, m);
        return 1;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(c, m, &ts) != ETIMEDOUT;
}

#endif /* POSIX */

/* ── Internal structure ──────────────────────────────────────────────── */

struct crossos_msgqueue {
    size_t      capacity;
    size_t      max_msg_size;
    size_t      stride;        /* sizeof(size_t) + max_msg_size */
    size_t      head;          /* next slot to read  */
    size_t      tail;          /* next slot to write */
    size_t      count;
    int         destroyed;     /* set to 1 by crossos_msgqueue_destroy  */
    char       *slots;         /* flat ring buffer                       */

    mq_mutex_t  mutex;
    mq_cond_t   not_empty;
    mq_cond_t   not_full;
};

/* ── Helpers ─────────────────────────────────────────────────────────── */

static char *slot_ptr(struct crossos_msgqueue *q, size_t idx)
{
    return q->slots + idx * q->stride;
}

static size_t slot_size(struct crossos_msgqueue *q, size_t idx)
{
    size_t v;
    memcpy(&v, slot_ptr(q, idx), sizeof(size_t));
    return v;
}

static void *slot_data(struct crossos_msgqueue *q, size_t idx)
{
    return slot_ptr(q, idx) + sizeof(size_t);
}

/* ── Public API ──────────────────────────────────────────────────────── */

crossos_result_t crossos_msgqueue_create(size_t               capacity,
                                         size_t               max_msg_size,
                                         crossos_msgqueue_t **out_queue)
{
    if (!out_queue || capacity == 0 || max_msg_size == 0)
        return CROSSOS_ERR_PARAM;
    *out_queue = NULL;

    struct crossos_msgqueue *q = calloc(1, sizeof(*q));
    if (!q) return CROSSOS_ERR_OOM;

    q->capacity     = capacity;
    q->max_msg_size = max_msg_size;
    q->stride       = sizeof(size_t) + max_msg_size;

    q->slots = calloc(capacity, q->stride);
    if (!q->slots) { free(q); return CROSSOS_ERR_OOM; }

    mq_mutex_init(&q->mutex);
    mq_cond_init(&q->not_empty);
    mq_cond_init(&q->not_full);

    *out_queue = q;
    return CROSSOS_OK;
}

void crossos_msgqueue_destroy(crossos_msgqueue_t *queue)
{
    if (!queue) return;

    mq_mutex_lock(&queue->mutex);
    queue->destroyed = 1;
    mq_cond_broadcast(&queue->not_empty);
    mq_cond_broadcast(&queue->not_full);
    mq_mutex_unlock(&queue->mutex);

    mq_cond_destroy(&queue->not_empty);
    mq_cond_destroy(&queue->not_full);
    mq_mutex_destroy(&queue->mutex);
    free(queue->slots);
    free(queue);
}

crossos_result_t crossos_msgqueue_push(crossos_msgqueue_t *queue,
                                       const void         *data,
                                       size_t              size,
                                       int                 timeout_ms)
{
    if (!queue || !data || size > queue->max_msg_size)
        return CROSSOS_ERR_PARAM;

    mq_mutex_lock(&queue->mutex);

    /* Wait for a free slot. */
    while (queue->count == queue->capacity && !queue->destroyed) {
        if (!mq_cond_timedwait(&queue->not_full, &queue->mutex, timeout_ms)) {
            mq_mutex_unlock(&queue->mutex);
            return CROSSOS_ERR_TIMEOUT;
        }
        /* If timeout_ms > 0 and we were woken for a reason other than space
         * becoming available, break out and re-check conditions. */
    }

    if (queue->destroyed) {
        mq_mutex_unlock(&queue->mutex);
        return CROSSOS_ERR_PARAM;
    }

    /* Write into tail slot. */
    char *slot = slot_ptr(queue, queue->tail);
    memcpy(slot, &size, sizeof(size_t));
    memcpy(slot + sizeof(size_t), data, size);

    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->count++;

    mq_cond_signal(&queue->not_empty);
    mq_mutex_unlock(&queue->mutex);
    return CROSSOS_OK;
}

crossos_result_t crossos_msgqueue_pop(crossos_msgqueue_t *queue,
                                      void               *buf,
                                      size_t              buf_size,
                                      size_t             *out_size,
                                      int                 timeout_ms)
{
    if (!queue || !buf || buf_size < queue->max_msg_size)
        return CROSSOS_ERR_PARAM;

    mq_mutex_lock(&queue->mutex);

    /* Wait for a message. */
    while (queue->count == 0 && !queue->destroyed) {
        if (!mq_cond_timedwait(&queue->not_empty, &queue->mutex, timeout_ms)) {
            mq_mutex_unlock(&queue->mutex);
            return CROSSOS_ERR_TIMEOUT;
        }
    }

    if (queue->count == 0) {
        /* destroyed + empty */
        mq_mutex_unlock(&queue->mutex);
        return CROSSOS_ERR_TIMEOUT;
    }

    /* Read from head slot. */
    size_t msg_size = slot_size(queue, queue->head);
    if (msg_size > buf_size) {
        mq_mutex_unlock(&queue->mutex);
        return CROSSOS_ERR_PARAM;
    }
    memcpy(buf, slot_data(queue, queue->head), msg_size);
    if (out_size) *out_size = msg_size;

    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;

    mq_cond_signal(&queue->not_full);
    mq_mutex_unlock(&queue->mutex);
    return CROSSOS_OK;
}

size_t crossos_msgqueue_count(const crossos_msgqueue_t *queue)
{
    if (!queue) return 0;
    /* Cast away const to lock; the read of count is atomic-enough for an
     * advisory value, but use the mutex for correctness. */
    mq_mutex_lock((mq_mutex_t *)&((struct crossos_msgqueue *)queue)->mutex);
    size_t c = queue->count;
    mq_mutex_unlock((mq_mutex_t *)&((struct crossos_msgqueue *)queue)->mutex);
    return c;
}

size_t crossos_msgqueue_capacity(const crossos_msgqueue_t *queue)
{
    return queue ? queue->capacity : 0;
}
