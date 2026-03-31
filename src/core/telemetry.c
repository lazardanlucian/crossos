/**
 * core/telemetry.c  –  Debug telemetry and analytics implementation.
 *
 * Compiled only when CROSSOS_TELEMETRY=1.
 *
 * Architecture:
 *   1. Entries (logs + analytics events) are queued in memory up to
 *      TELEMETRY_QUEUE_MAX entries.
 *   2. The queue is flushed (HTTP POST) when full, when
 *      crossos_telemetry_flush() is called, or on shutdown.
 *   3. Each POST is authenticated with a time-windowed SHA-256 token so the
 *      PHP server can reject unauthenticated payloads without storing any
 *      state on the client.
 *
 * Auth token:
 *   window = unix_time_seconds / 300          (5-minute bucket)
 *   token  = sha256_hex( secret + ":" + decimal(window) )
 *   The server accepts current window ± 1 to tolerate clock skew.
 */

#if defined(CROSSOS_TELEMETRY) && CROSSOS_TELEMETRY

#include <crossos/telemetry.h>
#include <crossos/web.h>

#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Forward declarations ──────────────────────────────────────────────── */
extern void crossos__set_error(const char *fmt, ...);
extern const char *crossos_platform_name(void);

/* ══════════════════════════════════════════════════════════════════════════
 *  Compact public-domain SHA-256  (FIPS 180-4)
 * ══════════════════════════════════════════════════════════════════════════ */

#define SHA256_BLOCK_SIZE  64u
#define SHA256_DIGEST_SIZE 32u

typedef struct {
    uint32_t state[8];
    uint8_t  buf[SHA256_BLOCK_SIZE];
    uint32_t buf_len;
    uint64_t total_bits;
} sha256_ctx_t;

static const uint32_t sha256_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

#define SHA_ROTR(x, n) (((x) >> (n)) | ((x) << (32u - (n))))
#define SHA_CH(x,y,z)  (((x) & (y)) ^ (~(x) & (z)))
#define SHA_MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SHA_EP0(x)     (SHA_ROTR(x,2)  ^ SHA_ROTR(x,13) ^ SHA_ROTR(x,22))
#define SHA_EP1(x)     (SHA_ROTR(x,6)  ^ SHA_ROTR(x,11) ^ SHA_ROTR(x,25))
#define SHA_SIG0(x)    (SHA_ROTR(x,7)  ^ SHA_ROTR(x,18) ^ ((x) >> 3))
#define SHA_SIG1(x)    (SHA_ROTR(x,17) ^ SHA_ROTR(x,19) ^ ((x) >> 10))

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t *block)
{
    uint32_t m[64], a, b, c, d, e, f, g, h, t1, t2;
    unsigned i;

    for (i = 0; i < 16u; i++) {
        m[i] = ((uint32_t)block[i * 4u    ] << 24)
             | ((uint32_t)block[i * 4u + 1] << 16)
             | ((uint32_t)block[i * 4u + 2] <<  8)
             | ((uint32_t)block[i * 4u + 3]);
    }
    for (i = 16u; i < 64u; i++) {
        m[i] = SHA_SIG1(m[i - 2]) + m[i - 7] + SHA_SIG0(m[i - 15]) + m[i - 16];
    }

    a = ctx->state[0]; b = ctx->state[1];
    c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5];
    g = ctx->state[6]; h = ctx->state[7];

    for (i = 0; i < 64u; i++) {
        t1 = h + SHA_EP1(e) + SHA_CH(e,f,g) + sha256_K[i] + m[i];
        t2 = SHA_EP0(a) + SHA_MAJ(a,b,c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b;
    ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f;
    ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(sha256_ctx_t *ctx)
{
    ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
    ctx->buf_len   = 0;
    ctx->total_bits = 0;
}

static void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        ctx->buf[ctx->buf_len++] = data[i];
        if (ctx->buf_len == SHA256_BLOCK_SIZE) {
            sha256_transform(ctx, ctx->buf);
            ctx->buf_len = 0;
        }
    }
    ctx->total_bits += (uint64_t)len * 8u;
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t digest[SHA256_DIGEST_SIZE])
{
    uint32_t i;
    ctx->buf[ctx->buf_len++] = 0x80u;

    if (ctx->buf_len > 56u) {
        while (ctx->buf_len < SHA256_BLOCK_SIZE) ctx->buf[ctx->buf_len++] = 0;
        sha256_transform(ctx, ctx->buf);
        ctx->buf_len = 0;
    }
    while (ctx->buf_len < 56u) ctx->buf[ctx->buf_len++] = 0;

    uint64_t bits = ctx->total_bits;
    for (i = 8; i > 0; i--) {
        ctx->buf[55 + i] = (uint8_t)(bits & 0xffu);
        bits >>= 8;
    }
    sha256_transform(ctx, ctx->buf);

    for (i = 0; i < 8u; i++) {
        digest[i * 4u    ] = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4u + 1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4u + 2] = (uint8_t)(ctx->state[i] >>  8);
        digest[i * 4u + 3] = (uint8_t)(ctx->state[i]);
    }
}

/** Compute SHA-256 of (data, len) and write lowercase hex into out (65 bytes). */
static void sha256_hex(const uint8_t *data, size_t len, char out[65])
{
    uint8_t digest[SHA256_DIGEST_SIZE];
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, digest);

    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < SHA256_DIGEST_SIZE; i++) {
        out[i * 2    ] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0fu];
    }
    out[64] = '\0';
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Auth token generation
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * Derive a 64-character hex auth token.
 * token = sha256( secret + ":" + decimal(unix_time / 300) )
 * Returns 0 on success, -1 on failure (caller should abort flush).
 */
static int make_auth_token(char token_out[65])
{
    const char *secret = CROSSOS_TELEMETRY_SECRET_DEFAULT;
    uint64_t window = (uint64_t)time(NULL) / 300u;

    char input[256];
    int n = snprintf(input, sizeof(input), "%s:%llu", secret, (unsigned long long)window);
    if (n <= 0 || (size_t)n >= sizeof(input)) {
        token_out[0] = '\0';
        return -1;
    }
    sha256_hex((const uint8_t *)input, (size_t)n, token_out);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  JSON helpers
 * ══════════════════════════════════════════════════════════════════════════ */

/** Append a JSON-escaped string (without surrounding quotes) to buf. */
static size_t json_escape_append(char *buf, size_t pos, size_t cap,
                                  const char *s)
{
    if (!s) return pos;
    for (; *s && pos + 6 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
            case '"':  buf[pos++] = '\\'; buf[pos++] = '"';  break;
            case '\\': buf[pos++] = '\\'; buf[pos++] = '\\'; break;
            case '\n': buf[pos++] = '\\'; buf[pos++] = 'n';  break;
            case '\r': buf[pos++] = '\\'; buf[pos++] = 'r';  break;
            case '\t': buf[pos++] = '\\'; buf[pos++] = 't';  break;
            default:
                if (c < 0x20u) {
                    /* Control characters → \uXXXX */
                    pos += (size_t)snprintf(buf + pos, cap - pos,
                                            "\\u%04x", (unsigned)c);
                } else {
                    buf[pos++] = (char)c;
                }
                break;
        }
    }
    return pos;
}

/** Append a JSON key-value pair: ,"key":"value" */
static size_t json_kv(char *buf, size_t pos, size_t cap,
                       const char *key, const char *value)
{
    if (!value) return pos;
    if (pos + 3 < cap) buf[pos++] = ',';
    if (pos + 1 < cap) buf[pos++] = '"';
    pos = json_escape_append(buf, pos, cap, key);
    if (pos + 4 < cap) { buf[pos++] = '"'; buf[pos++] = ':'; buf[pos++] = '"'; }
    pos = json_escape_append(buf, pos, cap, value);
    if (pos + 1 < cap) buf[pos++] = '"';
    return pos;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Entry queue
 * ══════════════════════════════════════════════════════════════════════════ */

#define TELEMETRY_QUEUE_MAX   16
#define TELEMETRY_STR_MAX    512

typedef enum { ENTRY_LOG, ENTRY_EVENT } entry_type_t;

typedef struct {
    entry_type_t type;
    long         timestamp;
    char         level[8];        /* "debug" / "info" / "warn" / "error" */
    char         tag[64];
    char         message[TELEMETRY_STR_MAX];
    char         extra[TELEMETRY_STR_MAX];   /* props JSON for events */
} telemetry_entry_t;

/* ── Buffer sizing constants ──────────────────────────────────────────── */
#define TELEMETRY_ENTRY_SIZE_ESTIMATE  1024   /* bytes allocated per queued entry */
#define TELEMETRY_OVERHEAD_BYTES        512   /* auth + device header overhead */

static struct {
    int    ready;
    char   url[512];
    char   device_id[128];
    char   app_version[64];

    telemetry_entry_t queue[TELEMETRY_QUEUE_MAX];
    int               queue_len;
} s_tel;

/* ══════════════════════════════════════════════════════════════════════════
 *  HTTP flush
 * ══════════════════════════════════════════════════════════════════════════ */

static void flush_queue(void)
{
    if (!s_tel.ready || s_tel.queue_len == 0 || s_tel.url[0] == '\0') {
        s_tel.queue_len = 0;
        return;
    }

    char token[65];
    if (make_auth_token(token) != 0) {
        /* Secret too long to fit in the snprintf buffer – skip flush. */
        s_tel.queue_len = 0;
        return;
    }

    /* Build JSON body:
     * { "auth":"...", "device_id":"...", "platform":"...",
     *   "app_version":"...", "batch":[ {…}, … ] }
     */
    char *body = (char *)malloc(TELEMETRY_QUEUE_MAX * TELEMETRY_ENTRY_SIZE_ESTIMATE + TELEMETRY_OVERHEAD_BYTES);
    if (!body) {
        s_tel.queue_len = 0;
        return;
    }

    const char *platform = crossos_platform_name();
    size_t pos = 0;
    size_t cap = (size_t)(TELEMETRY_QUEUE_MAX * TELEMETRY_ENTRY_SIZE_ESTIMATE + TELEMETRY_OVERHEAD_BYTES);

    int hdr = snprintf(body + pos, cap - pos, "{\"auth\":\"%s\"", token);
    if (hdr > 0) pos += (size_t)hdr;
    pos = json_kv(body, pos, cap, "device_id",   s_tel.device_id);
    pos = json_kv(body, pos, cap, "platform",    platform);
    pos = json_kv(body, pos, cap, "app_version", s_tel.app_version[0] ? s_tel.app_version : NULL);

    if (pos + 10 < cap) {
        memcpy(body + pos, ",\"batch\":[", 10);
        pos += 10;
    }

    for (int i = 0; i < s_tel.queue_len && pos + 256 < cap; i++) {
        const telemetry_entry_t *e = &s_tel.queue[i];
        if (i > 0 && pos + 1 < cap) body[pos++] = ',';

        if (pos + 1 < cap) body[pos++] = '{';
        if (e->type == ENTRY_LOG) {
            int n = snprintf(body + pos, cap - pos,
                             "\"type\":\"log\",\"ts\":%ld", e->timestamp);
            if (n > 0) pos += (size_t)n;
            pos = json_kv(body, pos, cap, "level",   e->level);
            pos = json_kv(body, pos, cap, "tag",     e->tag[0] ? e->tag : NULL);
            pos = json_kv(body, pos, cap, "message", e->message);
        } else {
            int n = snprintf(body + pos, cap - pos,
                             "\"type\":\"event\",\"ts\":%ld", e->timestamp);
            if (n > 0) pos += (size_t)n;
            pos = json_kv(body, pos, cap, "name",  e->tag);    /* event name in tag field */
            if (e->extra[0] != '\0' && pos + strlen(e->extra) + 12 < cap) {
                /* Embed props as raw JSON value (not re-escaped). */
                memcpy(body + pos, ",\"props\":", 9); pos += 9;
                size_t elen = strlen(e->extra);
                memcpy(body + pos, e->extra, elen); pos += elen;
            }
        }
        if (pos + 1 < cap) body[pos++] = '}';
    }

    if (pos + 3 < cap) { body[pos++] = ']'; body[pos++] = '}'; body[pos] = '\0'; }

    crossos_http_request_t req;
    memset(&req, 0, sizeof(req));
    req.method       = "POST";
    req.url          = s_tel.url;
    req.body         = body;
    req.body_size    = strlen(body);
    req.content_type = "application/json";
    req.timeout_ms   = 3000;

    crossos_http_response_t resp;
    memset(&resp, 0, sizeof(resp));
    crossos_http_request(&req, &resp);   /* fire-and-forget; errors ignored */
    crossos_http_response_free(&resp);

    free(body);
    s_tel.queue_len = 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Public API
 * ══════════════════════════════════════════════════════════════════════════ */

crossos_result_t crossos_telemetry_init(const char *server_url,
                                        const char *device_id,
                                        const char *app_version)
{
    if (!device_id) {
        crossos__set_error("telemetry_init: device_id must not be NULL");
        return CROSSOS_ERR_PARAM;
    }

    memset(&s_tel, 0, sizeof(s_tel));

    const char *url = (server_url && server_url[0]) ? server_url
                                                     : CROSSOS_TELEMETRY_URL_DEFAULT;
    strncpy(s_tel.url,         url,          sizeof(s_tel.url)         - 1);
    strncpy(s_tel.device_id,   device_id,    sizeof(s_tel.device_id)   - 1);
    if (app_version) {
        strncpy(s_tel.app_version, app_version, sizeof(s_tel.app_version) - 1);
    }

    s_tel.ready = 1;
    return CROSSOS_OK;
}

void crossos_telemetry_shutdown(void)
{
    flush_queue();
    s_tel.ready = 0;
}

void crossos_telemetry_log(const char *level,
                           const char *tag,
                           const char *message)
{
    if (!s_tel.ready || !level || !message) return;

    if (s_tel.queue_len == TELEMETRY_QUEUE_MAX) {
        flush_queue();
    }

    telemetry_entry_t *e = &s_tel.queue[s_tel.queue_len++];
    memset(e, 0, sizeof(*e));
    e->type      = ENTRY_LOG;
    e->timestamp = (long)time(NULL);
    strncpy(e->level,   level,   sizeof(e->level)   - 1);
    if (tag)     strncpy(e->tag, tag, sizeof(e->tag) - 1);
    strncpy(e->message, message, sizeof(e->message) - 1);
}

void crossos_telemetry_logf(const char *level,
                            const char *tag,
                            const char *fmt, ...)
{
    if (!s_tel.ready || !level || !fmt) return;

    char buf[TELEMETRY_STR_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    crossos_telemetry_log(level, tag, buf);
}

void crossos_telemetry_event(const char *name, const char *props_json)
{
    if (!s_tel.ready || !name) return;

    if (s_tel.queue_len == TELEMETRY_QUEUE_MAX) {
        flush_queue();
    }

    telemetry_entry_t *e = &s_tel.queue[s_tel.queue_len++];
    memset(e, 0, sizeof(*e));
    e->type      = ENTRY_EVENT;
    e->timestamp = (long)time(NULL);
    strncpy(e->tag, name, sizeof(e->tag) - 1);    /* event name in tag field */
    if (props_json) {
        strncpy(e->extra, props_json, sizeof(e->extra) - 1);
    }
}

#endif /* CROSSOS_TELEMETRY */
