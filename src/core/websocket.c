/*
 * src/core/websocket.c  –  RFC 6455 WebSocket client.
 *
 * Implements:
 *   • URL parsing  (ws:// and wss://)
 *   • TCP socket connect (POSIX + WinSock2)
 *   • HTTP/1.1 Upgrade handshake
 *   • SHA-1 and Base64 for Sec-WebSocket-Accept verification
 *   • Frame serialiser (client→server, masked per RFC 6455 §5.3)
 *   • Frame deserialiser with fragmentation reassembly
 *   • Ping/Pong auto-response
 *   • Close handshake
 *   • Background receive thread (pthreads / CreateThread)
 *   • Thread-safe send via a mutex
 *
 * wss:// (TLS) is stubbed – returns CROSSOS_ERR_UNSUPPORT unless compiled
 * with CROSSOS_HAS_OPENSSL defined (future extension point).
 */

/* ── Platform detection ──────────────────────────────────────────────── */

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

typedef SOCKET    ws_fd_t;
#define WS_BAD_FD  INVALID_SOCKET
#define ws_fd_close(s) closesocket(s)

/* Thread + mutex */
typedef HANDLE             ws_thread_t;
typedef CRITICAL_SECTION   ws_mutex_t;

#define ws_mutex_init(m)    InitializeCriticalSection(m)
#define ws_mutex_lock(m)    EnterCriticalSection(m)
#define ws_mutex_unlock(m)  LeaveCriticalSection(m)
#define ws_mutex_destroy(m) DeleteCriticalSection(m)

static ws_thread_t ws_thread_spawn(LPTHREAD_START_ROUTINE fn, void *arg)
{
    return CreateThread(NULL, 0, fn, arg, 0, NULL);
}
static void ws_thread_join(ws_thread_t t)
{
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

#else /* POSIX */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>

typedef int ws_fd_t;
#define WS_BAD_FD  (-1)
#define ws_fd_close(s) close(s)

typedef pthread_t       ws_thread_t;
typedef pthread_mutex_t ws_mutex_t;

#define ws_mutex_init(m)    pthread_mutex_init(m, NULL)
#define ws_mutex_lock(m)    pthread_mutex_lock(m)
#define ws_mutex_unlock(m)  pthread_mutex_unlock(m)
#define ws_mutex_destroy(m) pthread_mutex_destroy(m)

static ws_thread_t ws_thread_spawn(void *(*fn)(void *), void *arg)
{
    ws_thread_t t;
    if (pthread_create(&t, NULL, fn, arg) != 0) {
        /* Return an invalid sentinel on failure (pthread uses 0 for success) */
        memset(&t, 0, sizeof(t));
    }
    return t;
}
static void ws_thread_join(ws_thread_t t) { pthread_join(t, NULL); }

#endif /* POSIX */

/* ── Standard includes ───────────────────────────────────────────────── */

#include <crossos/websocket.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

/* ── SHA-1 (compact, public domain) ─────────────────────────────────── */
/*
 * Minimal RFC 3174 SHA-1 sufficient for the WebSocket handshake.
 * Not intended for security-sensitive use outside of WS key verification.
 */

#define SHA1_ROT(v,n) (((v)<<(n))|((v)>>(32-(n))))

static void sha1_block(uint32_t h[5], const uint8_t blk[64])
{
    uint32_t w[80], a, b, c, d, e, t;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)blk[i*4]<<24)|((uint32_t)blk[i*4+1]<<16)
              |((uint32_t)blk[i*4+2]<<8)|(uint32_t)blk[i*4+3];
    for (i = 16; i < 80; i++)
        w[i] = SHA1_ROT(w[i-3]^w[i-8]^w[i-14]^w[i-16], 1);
    a=h[0]; b=h[1]; c=h[2]; d=h[3]; e=h[4];
    for (i = 0; i < 80; i++) {
        if      (i < 20) t = SHA1_ROT(a,5)+((b&c)|((~b)&d))+e+w[i]+0x5A827999U;
        else if (i < 40) t = SHA1_ROT(a,5)+(b^c^d)+e+w[i]+0x6ED9EBA1U;
        else if (i < 60) t = SHA1_ROT(a,5)+((b&c)|(b&d)|(c&d))+e+w[i]+0x8F1BBCDCU;
        else             t = SHA1_ROT(a,5)+(b^c^d)+e+w[i]+0xCA62C1D6U;
        e=d; d=c; c=SHA1_ROT(b,30); b=a; a=t;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
}

static void sha1(const uint8_t *data, size_t len, uint8_t out[20])
{
    uint32_t h[5] = {
        0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U
    };
    uint64_t bitlen  = (uint64_t)len * 8;
    size_t   blocks  = (len + 9 + 63) / 64;
    size_t   b, i;
    uint8_t  blk[64];

    for (b = 0; b < blocks; b++) {
        memset(blk, 0, 64);
        for (i = 0; i < 64; i++) {
            size_t pos = b * 64 + i;
            if      (pos < len)  blk[i] = data[pos];
            else if (pos == len) blk[i] = 0x80;
        }
        if (b == blocks - 1) {
            blk[56] = (uint8_t)(bitlen >> 56); blk[57] = (uint8_t)(bitlen >> 48);
            blk[58] = (uint8_t)(bitlen >> 40); blk[59] = (uint8_t)(bitlen >> 32);
            blk[60] = (uint8_t)(bitlen >> 24); blk[61] = (uint8_t)(bitlen >> 16);
            blk[62] = (uint8_t)(bitlen >>  8); blk[63] = (uint8_t)(bitlen);
        }
        sha1_block(h, blk);
    }
    for (i = 0; i < 5; i++) {
        out[i*4]   = (uint8_t)(h[i] >> 24); out[i*4+1] = (uint8_t)(h[i] >> 16);
        out[i*4+2] = (uint8_t)(h[i] >>  8); out[i*4+3] = (uint8_t)(h[i]);
    }
}

/* ── Base64 ──────────────────────────────────────────────────────────── */

static const char B64C[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* out must be at least ceil(inlen / 3) * 4 + 1 bytes. */
static void b64_encode(const uint8_t *in, size_t inlen, char *out)
{
    size_t i, j = 0;
    for (i = 0; i + 2 < inlen; i += 3) {
        out[j++] = B64C[in[i] >> 2];
        out[j++] = B64C[((in[i] & 3) << 4) | (in[i+1] >> 4)];
        out[j++] = B64C[((in[i+1] & 0xF) << 2) | (in[i+2] >> 6)];
        out[j++] = B64C[in[i+2] & 0x3F];
    }
    if (i < inlen) {
        out[j++] = B64C[in[i] >> 2];
        if (i + 1 < inlen) {
            out[j++] = B64C[((in[i] & 3) << 4) | (in[i+1] >> 4)];
            out[j++] = B64C[(in[i+1] & 0xF) << 2];
        } else {
            out[j++] = B64C[(in[i] & 3) << 4];
            out[j++] = '=';
        }
        out[j++] = '=';
    }
    out[j] = '\0';
}

/* Compare two base64 strings ignoring trailing whitespace/newlines. */
static int b64_eq(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    while (la > 0 && (a[la-1] == '\r' || a[la-1] == '\n' || a[la-1] == ' '))
        la--;
    while (lb > 0 && (b[lb-1] == '\r' || b[lb-1] == '\n' || b[lb-1] == ' '))
        lb--;
    return la == lb && memcmp(a, b, la) == 0;
}

/* ── URL parser ──────────────────────────────────────────────────────── */

typedef struct {
    char host[256];
    char path[1024];
    int  port;
    int  tls;       /* 1 for wss://, 0 for ws:// */
} ws_url_t;

static int ws_parse_url(const char *url, ws_url_t *out)
{
    if (!url || !out) return 0;
    memset(out, 0, sizeof(*out));

    const char *p = url;
    if (strncmp(p, "wss://", 6) == 0) {
        out->tls = 1; p += 6; out->port = 443;
    } else if (strncmp(p, "ws://", 5) == 0) {
        out->tls = 0; p += 5; out->port = 80;
    } else {
        return 0;
    }

    /* host[:port] */
    const char *path_start = strchr(p, '/');
    size_t host_len = path_start ? (size_t)(path_start - p) : strlen(p);
    if (host_len == 0 || host_len >= sizeof(out->host)) return 0;
    memcpy(out->host, p, host_len);
    out->host[host_len] = '\0';

    /* Optional port */
    char *colon = strchr(out->host, ':');
    if (colon) {
        *colon = '\0';
        int port = atoi(colon + 1);
        if (port <= 0 || port > 65535) return 0;
        out->port = port;
    }

    if (path_start && path_start[0] != '\0') {
        size_t plen = strlen(path_start);
        if (plen >= sizeof(out->path)) return 0;
        memcpy(out->path, path_start, plen + 1);
    } else {
        out->path[0] = '/'; out->path[1] = '\0';
    }

    return 1;
}

/* ── Socket helpers ──────────────────────────────────────────────────── */

static ws_fd_t ws_tcp_connect(const char *host, int port)
{
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints, *result, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_str, &hints, &result) != 0)
        return WS_BAD_FD;

    ws_fd_t fd = WS_BAD_FD;
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == WS_BAD_FD) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        ws_fd_close(fd);
        fd = WS_BAD_FD;
    }
    freeaddrinfo(result);
    return fd;
}

/* Receive exactly n bytes; returns 1 on success, 0 on error/EOF. */
static int ws_recv_exact(ws_fd_t fd, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
#if defined(_WIN32)
        int r = recv(fd, (char *)(buf + got), (int)(n - got), 0);
#else
        ssize_t r = recv(fd, buf + got, n - got, 0);
#endif
        if (r <= 0) return 0;
        got += (size_t)r;
    }
    return 1;
}

/* Send all bytes; returns 1 on success, 0 on error. */
static int ws_send_all(ws_fd_t fd, const uint8_t *buf, size_t n)
{
    size_t sent = 0;
    while (sent < n) {
#if defined(_WIN32)
        int s = send(fd, (const char *)(buf + sent), (int)(n - sent), 0);
#else
        ssize_t s = send(fd, buf + sent, n - sent, 0);
#endif
        if (s <= 0) return 0;
        sent += (size_t)s;
    }
    return 1;
}

/* ── HTTP Upgrade handshake ──────────────────────────────────────────── */

/* The magic GUID from RFC 6455 §1.3 */
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* Generate a 16-byte nonce and base64-encode it (returns the encoded key). */
static void ws_gen_key(char out_key[25])
{
    uint8_t nonce[16];
    uint32_t r = (uint32_t)time(NULL) ^ (uint32_t)(size_t)out_key;
    for (int i = 0; i < 16; i++) {
        r = r * 1664525u + 1013904223u;
        nonce[i] = (uint8_t)(r >> 16);
    }
    b64_encode(nonce, 16, out_key);
}

/* Compute the expected Sec-WebSocket-Accept for a given client key. */
static void ws_expected_accept(const char *client_key, char out_accept[29])
{
    char cat[64 + 40];
    snprintf(cat, sizeof(cat), "%s%s", client_key, WS_GUID);

    uint8_t hash[20];
    sha1((const uint8_t *)cat, strlen(cat), hash);
    b64_encode(hash, 20, out_accept);
}

/* Perform the HTTP Upgrade handshake over an already-connected socket.
 * Returns 1 on success, 0 on failure. */
static int ws_do_handshake(ws_fd_t fd, const ws_url_t *url,
                           char used_key[25])
{
    ws_gen_key(used_key);

    /* Build request */
    char req[2048];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        url->path, url->host, used_key);

    if (req_len <= 0 || req_len >= (int)sizeof(req)) return 0;
    if (!ws_send_all(fd, (const uint8_t *)req, (size_t)req_len)) return 0;

    /* Read response headers (up to 4096 bytes looking for \r\n\r\n) */
    char resp[4096];
    size_t resp_len = 0;
    while (resp_len < sizeof(resp) - 1) {
        uint8_t ch;
        if (!ws_recv_exact(fd, &ch, 1)) return 0;
        resp[resp_len++] = (char)ch;
        if (resp_len >= 4 &&
            resp[resp_len-4] == '\r' && resp[resp_len-3] == '\n' &&
            resp[resp_len-2] == '\r' && resp[resp_len-1] == '\n') {
            break;
        }
    }
    resp[resp_len] = '\0';

    /* Must be 101 Switching Protocols */
    if (!strstr(resp, "101")) return 0;

    /* Verify Sec-WebSocket-Accept */
    char expected[29];
    ws_expected_accept(used_key, expected);

    /* Case-insensitive search for the Accept header */
    const char *accept_hdr = NULL;
    {
        static const char needle[] = "sec-websocket-accept:";
        size_t nlen = sizeof(needle) - 1;
        for (size_t i = 0; i + nlen <= resp_len; i++) {
            size_t j;
            for (j = 0; j < nlen; j++) {
                char a = (char)(resp[i + j] | 0x20);
                if (a != needle[j]) break;
            }
            if (j == nlen) { accept_hdr = resp + i + nlen; break; }
        }
    }
    if (!accept_hdr) return 0;
    while (*accept_hdr == ' ') accept_hdr++;

    const char *eol = strstr(accept_hdr, "\r\n");
    size_t alen = eol ? (size_t)(eol - accept_hdr) : strlen(accept_hdr);
    char server_accept[64];
    if (alen >= sizeof(server_accept)) return 0;
    memcpy(server_accept, accept_hdr, alen);
    server_accept[alen] = '\0';

    return b64_eq(expected, server_accept);
}

/* ── Frame write ─────────────────────────────────────────────────────── */

/* Write a single (client → server) WebSocket frame.
 * Clients MUST mask every frame per RFC 6455 §5.3.
 * opcode: 0x1=text, 0x2=binary, 0x8=close, 0x9=ping, 0xA=pong.
 * Returns 1 on success, 0 on failure. */
static int ws_write_frame(ws_fd_t fd, ws_mutex_t *send_mutex,
                          uint8_t opcode, const uint8_t *payload, size_t plen)
{
    /* Header: 2–10 bytes + 4 masking key bytes */
    uint8_t header[14];
    size_t  hlen = 0;

    header[hlen++] = 0x80u | (opcode & 0x0Fu); /* FIN=1 */

    /* Payload length + MASK bit */
    if (plen < 126) {
        header[hlen++] = 0x80u | (uint8_t)plen;
    } else if (plen <= 0xFFFFu) {
        header[hlen++] = 0x80u | 126u;
        header[hlen++] = (uint8_t)(plen >> 8);
        header[hlen++] = (uint8_t)(plen);
    } else {
        header[hlen++] = 0x80u | 127u;
        header[hlen++] = 0; header[hlen++] = 0; header[hlen++] = 0; header[hlen++] = 0;
        header[hlen++] = (uint8_t)(plen >> 24);
        header[hlen++] = (uint8_t)(plen >> 16);
        header[hlen++] = (uint8_t)(plen >> 8);
        header[hlen++] = (uint8_t)(plen);
    }

    /* Masking key – use a simple PRNG (masking is not a security feature) */
    uint32_t rng = (uint32_t)time(NULL) ^ (uint32_t)(size_t)payload ^ (uint32_t)plen;
    rng = rng * 1664525u + 1013904223u;
    uint8_t mask[4];
    mask[0] = (uint8_t)(rng >> 24);
    mask[1] = (uint8_t)(rng >> 16);
    mask[2] = (uint8_t)(rng >> 8);
    mask[3] = (uint8_t)(rng);
    header[hlen++] = mask[0]; header[hlen++] = mask[1];
    header[hlen++] = mask[2]; header[hlen++] = mask[3];

    /* Masked payload (copy so we don't mutate the caller's buffer) */
    uint8_t *masked = NULL;
    if (plen > 0) {
        masked = (uint8_t *)malloc(plen);
        if (!masked) return 0;
        for (size_t i = 0; i < plen; i++)
            masked[i] = payload[i] ^ mask[i & 3u];
    }

    ws_mutex_lock(send_mutex);
    int ok = ws_send_all(fd, header, hlen);
    if (ok && plen > 0)
        ok = ws_send_all(fd, masked, plen);
    ws_mutex_unlock(send_mutex);

    free(masked);
    return ok;
}

/* ── WebSocket connection handle ─────────────────────────────────────── */

struct crossos_ws {
    ws_fd_t                fd;
    ws_mutex_t             send_mutex;
    ws_thread_t            recv_thread;
    crossos_ws_callbacks_t cbs;
    volatile int           closing;   /* set to 1 once close frame sent     */
    volatile int           destroyed; /* set to 1 by ws_destroy (from main) */
};

/* ── Frame reader (runs in background thread) ────────────────────────── */

/* Fragment reassembly state */
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    uint8_t  opcode; /* opcode of the first fragment */
} frag_t;

static void frag_reset(frag_t *f) { f->len = 0; f->opcode = 0; }

static int frag_append(frag_t *f, const uint8_t *data, size_t n)
{
    if (f->len + n > f->cap) {
        size_t new_cap = (f->len + n) * 2 + 4096;
        uint8_t *nb = (uint8_t *)realloc(f->buf, new_cap);
        if (!nb) return 0;
        f->buf = nb; f->cap = new_cap;
    }
    memcpy(f->buf + f->len, data, n);
    f->len += n;
    return 1;
}

/* The background receive thread entry point */
#ifdef _WIN32
static DWORD WINAPI ws_recv_thread(LPVOID arg)
#else
static void *ws_recv_thread(void *arg)
#endif
{
    struct crossos_ws *ws = (struct crossos_ws *)arg;

    frag_t frag;
    memset(&frag, 0, sizeof(frag));

    for (;;) {
        if (ws->destroyed) break;

        /* Read 2-byte frame header */
        uint8_t hdr[2];
        if (!ws_recv_exact(ws->fd, hdr, 2)) {
            if (!ws->destroyed && !ws->closing) {
                if (ws->cbs.on_error)
                    ws->cbs.on_error(ws, "connection lost", ws->cbs.user_data);
                if (ws->cbs.on_close)
                    ws->cbs.on_close(ws, 0, "", ws->cbs.user_data);
            }
            break;
        }

        int  fin    = (hdr[0] >> 7) & 1;
        uint8_t opcode = hdr[0] & 0x0Fu;
        int  masked = (hdr[1] >> 7) & 1;
        uint64_t plen = hdr[1] & 0x7Fu;

        /* Extended payload length */
        if (plen == 126) {
            uint8_t ext[2];
            if (!ws_recv_exact(ws->fd, ext, 2)) break;
            plen = ((uint64_t)ext[0] << 8) | ext[1];
        } else if (plen == 127) {
            uint8_t ext[8];
            if (!ws_recv_exact(ws->fd, ext, 8)) break;
            plen = 0;
            for (int i = 0; i < 8; i++) plen = (plen << 8) | ext[i];
        }

        /* Server frames MUST NOT be masked, but read mask if present */
        uint8_t mask[4] = {0, 0, 0, 0};
        if (masked) {
            if (!ws_recv_exact(ws->fd, mask, 4)) break;
        }

        /* Sanity: reject unreasonably large frames (>64MB) */
        if (plen > 67108864ULL) {
            if (ws->cbs.on_error)
                ws->cbs.on_error(ws, "frame too large", ws->cbs.user_data);
            break;
        }

        /* Read payload */
        uint8_t *payload = NULL;
        if (plen > 0) {
            payload = (uint8_t *)malloc((size_t)plen + 1);
            if (!payload) {
                if (ws->cbs.on_error)
                    ws->cbs.on_error(ws, "out of memory", ws->cbs.user_data);
                break;
            }
            if (!ws_recv_exact(ws->fd, payload, (size_t)plen)) {
                free(payload); break;
            }
            payload[plen] = 0;
            if (masked) {
                for (uint64_t i = 0; i < plen; i++)
                    payload[i] ^= mask[i & 3u];
            }
        }

        /* Dispatch by opcode */
        switch (opcode) {

        case 0x0: /* continuation */
            if (!frag_append(&frag, payload ? payload : (uint8_t *)"",
                             (size_t)plen)) {
                if (ws->cbs.on_error)
                    ws->cbs.on_error(ws, "out of memory in fragment reassembly",
                                     ws->cbs.user_data);
                free(payload); goto done;
            }
            if (fin) {
                if (ws->cbs.on_message)
                    ws->cbs.on_message(ws, frag.buf, frag.len,
                                       frag.opcode == 0x2,
                                       ws->cbs.user_data);
                frag_reset(&frag);
            }
            break;

        case 0x1: /* text */
        case 0x2: /* binary */
            if (fin) {
                /* Complete single-frame message */
                if (ws->cbs.on_message)
                    ws->cbs.on_message(ws,
                                       payload ? payload : (uint8_t *)"",
                                       (size_t)plen,
                                       opcode == 0x2,
                                       ws->cbs.user_data);
            } else {
                /* First fragment */
                frag_reset(&frag);
                frag.opcode = opcode;
                if (!frag_append(&frag, payload ? payload : (uint8_t *)"",
                                 (size_t)plen)) {
                    if (ws->cbs.on_error)
                        ws->cbs.on_error(ws, "out of memory", ws->cbs.user_data);
                    free(payload); goto done;
                }
            }
            break;

        case 0x8: /* close */
        {
            int code = 0;
            const char *reason = "";
            if (plen >= 2) {
                code = ((int)payload[0] << 8) | payload[1];
                if (plen > 2) reason = (const char *)(payload + 2);
            }
            /* Echo close frame back */
            ws->closing = 1;
            ws_write_frame(ws->fd, &ws->send_mutex, 0x8,
                           payload, (size_t)plen);
            if (ws->cbs.on_close)
                ws->cbs.on_close(ws, code, reason, ws->cbs.user_data);
            free(payload);
            goto done;
        }

        case 0x9: /* ping → pong */
            ws_write_frame(ws->fd, &ws->send_mutex, 0xA,
                           payload, (size_t)plen);
            break;

        case 0xA: /* pong – ignore */
            break;

        default:
            /* Unknown opcode — ignore */
            break;
        }

        free(payload);
    }

done:
    free(frag.buf);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ── Public API ──────────────────────────────────────────────────────── */

crossos_result_t crossos_ws_connect(const char                   *url,
                                    const crossos_ws_callbacks_t *callbacks,
                                    crossos_ws_t                **out_ws)
{
    if (!url || !out_ws) return CROSSOS_ERR_PARAM;
    *out_ws = NULL;

    ws_url_t parsed;
    if (!ws_parse_url(url, &parsed)) return CROSSOS_ERR_PARAM;

    if (parsed.tls) return CROSSOS_ERR_UNSUPPORT; /* wss:// requires OpenSSL */

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    ws_fd_t fd = ws_tcp_connect(parsed.host, parsed.port);
    if (fd == WS_BAD_FD) {
#ifdef _WIN32
        WSACleanup();
#endif
        return CROSSOS_ERR_NETWORK;
    }

    char key[25];
    if (!ws_do_handshake(fd, &parsed, key)) {
        ws_fd_close(fd);
#ifdef _WIN32
        WSACleanup();
#endif
        return CROSSOS_ERR_WS;
    }

    struct crossos_ws *ws = (struct crossos_ws *)calloc(1, sizeof(*ws));
    if (!ws) {
        ws_fd_close(fd);
#ifdef _WIN32
        WSACleanup();
#endif
        return CROSSOS_ERR_OOM;
    }

    ws->fd = fd;
    ws_mutex_init(&ws->send_mutex);
    if (callbacks) ws->cbs = *callbacks;

    /* Fire on_open synchronously before spawning the recv thread. */
    if (ws->cbs.on_open)
        ws->cbs.on_open(ws, ws->cbs.user_data);

    /* Start background receive thread. */
    ws->recv_thread = ws_thread_spawn(ws_recv_thread, ws);

    *out_ws = ws;
    return CROSSOS_OK;
}

crossos_result_t crossos_ws_send_text(crossos_ws_t *ws, const char *text)
{
    if (!ws || !text) return CROSSOS_ERR_PARAM;
    size_t len = strlen(text);
    if (!ws_write_frame(ws->fd, &ws->send_mutex, 0x1,
                        (const uint8_t *)text, len))
        return CROSSOS_ERR_NETWORK;
    return CROSSOS_OK;
}

crossos_result_t crossos_ws_send_binary(crossos_ws_t *ws,
                                        const void   *data,
                                        size_t        size)
{
    if (!ws || !data) return CROSSOS_ERR_PARAM;
    if (!ws_write_frame(ws->fd, &ws->send_mutex, 0x2,
                        (const uint8_t *)data, size))
        return CROSSOS_ERR_NETWORK;
    return CROSSOS_OK;
}

crossos_result_t crossos_ws_close(crossos_ws_t *ws,
                                  int           code,
                                  const char   *reason)
{
    if (!ws) return CROSSOS_ERR_PARAM;
    if (ws->closing) return CROSSOS_OK;
    ws->closing = 1;

    /* Build close payload: 2-byte big-endian code + optional reason */
    uint8_t payload[128];
    size_t  plen = 0;
    if (code > 0) {
        payload[0] = (uint8_t)(code >> 8);
        payload[1] = (uint8_t)(code);
        plen = 2;
        if (reason && reason[0]) {
            size_t rlen = strlen(reason);
            if (rlen + 2 > sizeof(payload)) rlen = sizeof(payload) - 2;
            memcpy(payload + 2, reason, rlen);
            plen += rlen;
        }
    }

    if (!ws_write_frame(ws->fd, &ws->send_mutex, 0x8, payload, plen))
        return CROSSOS_ERR_NETWORK;
    return CROSSOS_OK;
}

void crossos_ws_destroy(crossos_ws_t *ws)
{
    if (!ws) return;
    ws->destroyed = 1;
    /* Closing the socket will unblock the recv thread's recv() call. */
    ws_fd_close(ws->fd);
    ws->fd = WS_BAD_FD;
    ws_thread_join(ws->recv_thread);
    ws_mutex_destroy(&ws->send_mutex);
#ifdef _WIN32
    WSACleanup();
#endif
    free(ws);
}
