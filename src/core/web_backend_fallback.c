/* getaddrinfo, struct addrinfo, etc. require _POSIX_C_SOURCE >= 200112L */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "web_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void crossos__set_error(const char *fmt, ...);

#if !defined(CROSSOS_HAS_CURL)

#if defined(_WIN32)

crossos_result_t crossos__web_backend_request(const crossos_http_request_t *request,
                                              crossos_http_response_t *out_response)
{
    (void)request;
    (void)out_response;
    crossos__set_error("http_request: no fallback transport on Windows without libcurl");
    return CROSSOS_ERR_UNSUPPORT;
}

#else

#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct crossos__url_parts {
    char host[256];
    char path[1024];
    int  port;
} crossos__url_parts_t;

static int crossos__parse_http_url(const char *url, crossos__url_parts_t *out)
{
    const char *prefix = "http://";
    size_t prefix_len = strlen(prefix);
    if (!url || !out || strncmp(url, prefix, prefix_len) != 0) {
        return 0;
    }

    const char *host_start = url + prefix_len;
    const char *path_start = strchr(host_start, '/');
    size_t host_len = path_start ? (size_t)(path_start - host_start) : strlen(host_start);

    if (host_len == 0 || host_len >= sizeof(out->host)) {
        return 0;
    }

    memcpy(out->host, host_start, host_len);
    out->host[host_len] = '\0';
    out->port = 80;

    char *colon = strchr(out->host, ':');
    if (colon) {
        *colon = '\0';
        int port = atoi(colon + 1);
        if (port <= 0 || port > 65535) {
            return 0;
        }
        out->port = port;
    }

    if (path_start && path_start[0] != '\0') {
        strncpy(out->path, path_start, sizeof(out->path) - 1);
        out->path[sizeof(out->path) - 1] = '\0';
    } else {
        strcpy(out->path, "/");
    }

    return 1;
}

static int crossos__send_all(int sock, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(sock, buf + sent, len - sent, 0);
        if (n <= 0) {
            return 0;
        }
        sent += (size_t)n;
    }
    return 1;
}

crossos_result_t crossos__web_backend_request(const crossos_http_request_t *request,
                                              crossos_http_response_t *out_response)
{
    crossos__url_parts_t parts;
    if (!crossos__parse_http_url(request->url, &parts)) {
        crossos__set_error("http_request: only plain http:// URLs are supported without libcurl");
        return CROSSOS_ERR_UNSUPPORT;
    }

    const char *method = request->method ? request->method : "GET";
    int timeout_ms = request->timeout_ms > 0 ? request->timeout_ms : 10000;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", parts.port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *result = NULL;
    int gai_rc = getaddrinfo(parts.host, port_str, &hints, &result);
    if (gai_rc != 0) {
        crossos__set_error("http_request: DNS lookup failed for %s", parts.host);
        return CROSSOS_ERR_NETWORK;
    }

    int sock = -1;
    struct addrinfo *rp;
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0) {
            continue;
        }
        if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        close(sock);
        sock = -1;
    }

    freeaddrinfo(result);

    if (sock < 0) {
        crossos__set_error("http_request: connect failed");
        return CROSSOS_ERR_NETWORK;
    }

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    const char *body = (const char *)request->body;
    size_t body_size = request->body_size;

    char header[2048];
    int header_len = 0;
    if (request->content_type && request->content_type[0] != '\0') {
        header_len = snprintf(header, sizeof(header),
                              "%s %s HTTP/1.1\r\n"
                              "Host: %s\r\n"
                              "Connection: close\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %zu\r\n\r\n",
                              method, parts.path, parts.host,
                              request->content_type, body_size);
    } else if (body && body_size > 0) {
        header_len = snprintf(header, sizeof(header),
                              "%s %s HTTP/1.1\r\n"
                              "Host: %s\r\n"
                              "Connection: close\r\n"
                              "Content-Length: %zu\r\n\r\n",
                              method, parts.path, parts.host, body_size);
    } else {
        header_len = snprintf(header, sizeof(header),
                              "%s %s HTTP/1.1\r\n"
                              "Host: %s\r\n"
                              "Connection: close\r\n\r\n",
                              method, parts.path, parts.host);
    }

    if (header_len <= 0 || (size_t)header_len >= sizeof(header)) {
        close(sock);
        crossos__set_error("http_request: request header too large");
        return CROSSOS_ERR_NETWORK;
    }

    if (!crossos__send_all(sock, header, (size_t)header_len) ||
        (body && body_size > 0 && !crossos__send_all(sock, body, body_size))) {
        close(sock);
        crossos__set_error("http_request: send failed");
        return CROSSOS_ERR_NETWORK;
    }

    unsigned char *raw = NULL;
    size_t raw_size = 0;
    unsigned char chunk[4096];

    for (;;) {
        ssize_t n = recv(sock, chunk, sizeof(chunk), 0);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            free(raw);
            close(sock);
            crossos__set_error("http_request: recv failed: %s", strerror(errno));
            return CROSSOS_ERR_NETWORK;
        }

        unsigned char *new_raw = (unsigned char *)realloc(raw, raw_size + (size_t)n + 1);
        if (!new_raw) {
            free(raw);
            close(sock);
            crossos__set_error("http_request: out of memory");
            return CROSSOS_ERR_OOM;
        }

        raw = new_raw;
        memcpy(raw + raw_size, chunk, (size_t)n);
        raw_size += (size_t)n;
        raw[raw_size] = 0;
    }

    close(sock);

    if (!raw || raw_size == 0) {
        free(raw);
        crossos__set_error("http_request: empty response");
        return CROSSOS_ERR_NETWORK;
    }

    long status = 0;
    sscanf((const char *)raw, "HTTP/%*d.%*d %ld", &status);

    unsigned char *body_start = (unsigned char *)strstr((const char *)raw, "\r\n\r\n");
    if (!body_start) {
        free(raw);
        crossos__set_error("http_request: malformed HTTP response");
        return CROSSOS_ERR_NETWORK;
    }

    body_start += 4;
    size_t body_len = raw_size - (size_t)(body_start - raw);

    void *body_copy = malloc(body_len + 1);
    if (!body_copy) {
        free(raw);
        crossos__set_error("http_request: out of memory");
        return CROSSOS_ERR_OOM;
    }

    memcpy(body_copy, body_start, body_len);
    ((unsigned char *)body_copy)[body_len] = 0;
    free(raw);

    out_response->status_code = status;
    out_response->body = body_copy;
    out_response->body_size = body_len;
    return CROSSOS_OK;
}

#endif

#endif
