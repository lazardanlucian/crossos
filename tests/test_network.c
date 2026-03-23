/**
 * tests/test_network.c  –  Unit tests for crossos_msgqueue and WebSocket API.
 *
 * The msgqueue tests are fully headless and self-contained.  The WebSocket
 * tests only exercise parameter validation (no server is required).
 */

#include <crossos/crossos.h>

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>

/* ── Tiny harness ────────────────────────────────────────────────────── */

static int s_passed = 0;
static int s_failed = 0;

#define TEST(name)  static void name(void)
#define RUN(name)   do { printf("  %-48s", #name); name(); } while(0)
#define CHECK(expr)                                              \
    do {                                                         \
        if (expr) {                                              \
            printf("PASS\n"); s_passed++;                        \
        } else {                                                 \
            printf("FAIL  (%s:%d)\n", __FILE__, __LINE__);      \
            s_failed++;                                          \
        }                                                        \
    } while(0)

/* ═══════════════════════════════════════════════════════════════════════
 * msgqueue tests
 * ═════════════════════════════════════════════════════════════════════ */

TEST(test_mq_create_invalid_params)
{
    crossos_msgqueue_t *q = NULL;
    /* capacity 0 */
    CHECK(crossos_msgqueue_create(0,  64, &q) == CROSSOS_ERR_PARAM);
    /* max_msg_size 0 */
    CHECK(crossos_msgqueue_create(8,   0, &q) == CROSSOS_ERR_PARAM);
    /* null out pointer */
    CHECK(crossos_msgqueue_create(8,  64, NULL) == CROSSOS_ERR_PARAM);
    CHECK(q == NULL);
}

TEST(test_mq_create_destroy)
{
    crossos_msgqueue_t *q = NULL;
    CHECK(crossos_msgqueue_create(4, 64, &q) == CROSSOS_OK);
    CHECK(q != NULL);
    CHECK(crossos_msgqueue_capacity(q) == 4);
    CHECK(crossos_msgqueue_count(q) == 0);
    crossos_msgqueue_destroy(q);
    crossos_msgqueue_destroy(NULL); /* must not crash */
    CHECK(1); /* reached here without crash */
}

TEST(test_mq_push_pop_roundtrip)
{
    crossos_msgqueue_t *q = NULL;
    crossos_msgqueue_create(8, 64, &q);

    const char *msg = "hello-queue";
    CHECK(crossos_msgqueue_push(q, msg, strlen(msg) + 1, 0) == CROSSOS_OK);
    CHECK(crossos_msgqueue_count(q) == 1);

    char buf[64];
    size_t sz = 0;
    CHECK(crossos_msgqueue_pop(q, buf, sizeof(buf), &sz, 0) == CROSSOS_OK);
    CHECK(sz == strlen(msg) + 1);
    CHECK(strcmp(buf, msg) == 0);
    CHECK(crossos_msgqueue_count(q) == 0);

    crossos_msgqueue_destroy(q);
}

TEST(test_mq_fifo_ordering)
{
    crossos_msgqueue_t *q = NULL;
    crossos_msgqueue_create(16, sizeof(int), &q);

    for (int i = 0; i < 8; i++)
        crossos_msgqueue_push(q, &i, sizeof(int), 0);

    CHECK(crossos_msgqueue_count(q) == 8);

    int ok = 1;
    for (int i = 0; i < 8; i++) {
        int v = -1;
        size_t sz;
        crossos_msgqueue_pop(q, &v, sizeof(int), &sz, 0);
        if (v != i) ok = 0;
    }
    CHECK(ok);
    crossos_msgqueue_destroy(q);
}

TEST(test_mq_nonblocking_empty)
{
    crossos_msgqueue_t *q = NULL;
    crossos_msgqueue_create(4, 32, &q);

    char buf[32];
    /* Non-blocking pop on empty queue must time out immediately */
    CHECK(crossos_msgqueue_pop(q, buf, sizeof(buf), NULL, 0) == CROSSOS_ERR_TIMEOUT);
    crossos_msgqueue_destroy(q);
}

TEST(test_mq_nonblocking_full)
{
    crossos_msgqueue_t *q = NULL;
    crossos_msgqueue_create(2, 4, &q);

    int v = 1;
    CHECK(crossos_msgqueue_push(q, &v, sizeof(int), 0) == CROSSOS_OK);
    CHECK(crossos_msgqueue_push(q, &v, sizeof(int), 0) == CROSSOS_OK);
    /* Queue is full: non-blocking push must return TIMEOUT */
    CHECK(crossos_msgqueue_push(q, &v, sizeof(int), 0) == CROSSOS_ERR_TIMEOUT);
    crossos_msgqueue_destroy(q);
}

TEST(test_mq_oversized_message)
{
    crossos_msgqueue_t *q = NULL;
    crossos_msgqueue_create(4, 8, &q);

    char big[32];
    memset(big, 0, sizeof(big));
    /* Message larger than max_msg_size must be rejected */
    CHECK(crossos_msgqueue_push(q, big, sizeof(big), 0) == CROSSOS_ERR_PARAM);
    crossos_msgqueue_destroy(q);
}

TEST(test_mq_wrap_around)
{
    /* Exercise ring-buffer wrap-around by pushing then popping more than
     * capacity entries in batches smaller than capacity. */
    crossos_msgqueue_t *q = NULL;
    crossos_msgqueue_create(4, sizeof(int), &q);

    int ok = 1;
    int counter_in = 0, counter_out = 0;

    /* Fill to capacity, drain, repeat three times to trigger wrap */
    for (int cycle = 0; cycle < 3; cycle++) {
        for (int i = 0; i < 4; i++) {
            if (crossos_msgqueue_push(q, &counter_in, sizeof(int), 0) != CROSSOS_OK)
                ok = 0;
            counter_in++;
        }
        for (int i = 0; i < 4; i++) {
            int v = -1; size_t sz;
            if (crossos_msgqueue_pop(q, &v, sizeof(int), &sz, 0) != CROSSOS_OK)
                ok = 0;
            if (v != counter_out) ok = 0;
            counter_out++;
        }
    }
    CHECK(ok);
    crossos_msgqueue_destroy(q);
}

/* ── Threaded producer/consumer test ─────────────────────────────────── */

#if !defined(_WIN32)
#include <pthread.h>

typedef struct {
    crossos_msgqueue_t *q;
    int                 n;        /* number of messages to produce */
    int                 received; /* consumer fills this in        */
    int                 ok;
} mq_thread_args_t;

static void *mq_producer(void *arg)
{
    mq_thread_args_t *a = (mq_thread_args_t *)arg;
    for (int i = 0; i < a->n; i++) {
        if (crossos_msgqueue_push(a->q, &i, sizeof(int), -1) != CROSSOS_OK)
            a->ok = 0;
    }
    return NULL;
}

static void *mq_consumer(void *arg)
{
    mq_thread_args_t *a = (mq_thread_args_t *)arg;
    for (int i = 0; i < a->n; i++) {
        int v = -1; size_t sz;
        if (crossos_msgqueue_pop(a->q, &v, sizeof(int), &sz, 5000) != CROSSOS_OK) {
            a->ok = 0; break;
        }
        a->received++;
    }
    return NULL;
}
#endif

TEST(test_mq_threaded_producer_consumer)
{
#if defined(_WIN32)
    /* Skip on Windows in this test runner */
    CHECK(1);
#else
    crossos_msgqueue_t *q = NULL;
    crossos_msgqueue_create(16, sizeof(int), &q);

    mq_thread_args_t args = { q, 64, 0, 1 };

    pthread_t prod, cons;
    pthread_create(&prod, NULL, mq_producer, &args);
    pthread_create(&cons, NULL, mq_consumer, &args);
    pthread_join(prod, NULL);
    pthread_join(cons, NULL);

    CHECK(args.ok == 1);
    CHECK(args.received == 64);
    crossos_msgqueue_destroy(q);
#endif
}

/* ═══════════════════════════════════════════════════════════════════════
 * WebSocket API tests (parameter validation only – no server needed)
 * ═════════════════════════════════════════════════════════════════════ */

TEST(test_ws_null_url)
{
    crossos_ws_t *ws = NULL;
    CHECK(crossos_ws_connect(NULL, NULL, &ws) == CROSSOS_ERR_PARAM);
    CHECK(ws == NULL);
}

TEST(test_ws_null_out_param)
{
    CHECK(crossos_ws_connect("ws://localhost/", NULL, NULL) == CROSSOS_ERR_PARAM);
}

TEST(test_ws_bad_url_scheme)
{
    crossos_ws_t *ws = NULL;
    /* HTTP URL is not a valid WebSocket URL */
    CHECK(crossos_ws_connect("http://example.com/", NULL, &ws) == CROSSOS_ERR_PARAM);
    CHECK(ws == NULL);
}

TEST(test_ws_wss_unsupported)
{
    crossos_ws_t *ws = NULL;
    /* wss:// requires TLS – must return UNSUPPORT when compiled without OpenSSL */
    crossos_result_t rc = crossos_ws_connect("wss://example.com/", NULL, &ws);
    CHECK(rc == CROSSOS_ERR_UNSUPPORT || rc == CROSSOS_OK);
    if (ws) crossos_ws_destroy(ws);
}

TEST(test_ws_send_null_handle)
{
    CHECK(crossos_ws_send_text(NULL, "hello") == CROSSOS_ERR_PARAM);
    CHECK(crossos_ws_send_binary(NULL, "x", 1) == CROSSOS_ERR_PARAM);
    CHECK(crossos_ws_close(NULL, 1000, "bye") == CROSSOS_ERR_PARAM);
}

TEST(test_ws_destroy_null)
{
    crossos_ws_destroy(NULL); /* must not crash */
    CHECK(1);
}

/* ═══════════════════════════════════════════════════════════════════════
 * New error code tests
 * ═════════════════════════════════════════════════════════════════════ */

TEST(test_new_error_codes)
{
    CHECK(CROSSOS_ERR_WS      < 0);
    CHECK(CROSSOS_ERR_TIMEOUT < 0);
    /* All error codes must be distinct */
    CHECK(CROSSOS_ERR_WS      != CROSSOS_ERR_NETWORK);
    CHECK(CROSSOS_ERR_TIMEOUT != CROSSOS_ERR_WS);
    CHECK(CROSSOS_ERR_TIMEOUT != CROSSOS_ERR_NETWORK);
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("=== CrossOS network / messaging unit tests ===\n\n");

    printf("--- msgqueue ---\n");
    RUN(test_mq_create_invalid_params);
    RUN(test_mq_create_destroy);
    RUN(test_mq_push_pop_roundtrip);
    RUN(test_mq_fifo_ordering);
    RUN(test_mq_nonblocking_empty);
    RUN(test_mq_nonblocking_full);
    RUN(test_mq_oversized_message);
    RUN(test_mq_wrap_around);
    RUN(test_mq_threaded_producer_consumer);

    printf("\n--- websocket (param validation) ---\n");
    RUN(test_ws_null_url);
    RUN(test_ws_null_out_param);
    RUN(test_ws_bad_url_scheme);
    RUN(test_ws_wss_unsupported);
    RUN(test_ws_send_null_handle);
    RUN(test_ws_destroy_null);

    printf("\n--- error codes ---\n");
    RUN(test_new_error_codes);

    printf("\n%d passed, %d failed\n", s_passed, s_failed);
    return s_failed > 0 ? 1 : 0;
}
