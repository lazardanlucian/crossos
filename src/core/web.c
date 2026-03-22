#include <crossos/web.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void crossos__set_error(const char *fmt, ...);

#if defined(CROSSOS_HAS_CURL)
#include <curl/curl.h>

static int s_curl_global_init_done = 0;

typedef struct crossos__web_buffer {
    unsigned char *data;
    size_t size;
} crossos__web_buffer_t;

static size_t crossos__write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t total = size * nmemb;
    crossos__web_buffer_t *buf = (crossos__web_buffer_t *)userp;

    unsigned char *new_data = (unsigned char *)realloc(buf->data, buf->size + total + 1);
    if (!new_data) {
        return 0;
    }

    buf->data = new_data;
    memcpy(buf->data + buf->size, contents, total);
    buf->size += total;
    buf->data[buf->size] = 0;
    return total;
}
#endif

#if !defined(CROSSOS_HAS_CURL)
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
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
#endif

crossos_result_t crossos_http_request(const crossos_http_request_t *request,
                                      crossos_http_response_t *out_response)
{
    if (!request || !request->url || !out_response) {
        crossos__set_error("http_request: invalid parameter");
        return CROSSOS_ERR_PARAM;
    }

    memset(out_response, 0, sizeof(*out_response));

#if defined(CROSSOS_HAS_CURL)
    if (!s_curl_global_init_done) {
        CURLcode init_rc = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (init_rc != CURLE_OK) {
            crossos__set_error("http_request: curl_global_init failed: %s", curl_easy_strerror(init_rc));
            return CROSSOS_ERR_NETWORK;
        }
        s_curl_global_init_done = 1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        crossos__set_error("http_request: curl_easy_init failed");
        return CROSSOS_ERR_NETWORK;
    }

    crossos__web_buffer_t response = {0};
    struct curl_slist *headers = NULL;

    const char *method = request->method ? request->method : "GET";
    int timeout_ms = request->timeout_ms > 0 ? request->timeout_ms : 10000;

    curl_easy_setopt(curl, CURLOPT_URL, request->url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, crossos__write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    if (strcmp(method, "GET") != 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    }

    if (request->body && request->body_size > 0) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request->body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)request->body_size);
    }

    if (request->content_type && request->content_type[0] != '\0') {
        char header_line[256];
        snprintf(header_line, sizeof(header_line), "Content-Type: %s", request->content_type);
        headers = curl_slist_append(headers, header_line);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        crossos__set_error("http_request: curl failed: %s", curl_easy_strerror(res));
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(response.data);
        return CROSSOS_ERR_NETWORK;
    }

    long status_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    out_response->status_code = status_code;
    out_response->body = response.data;
    out_response->body_size = response.size;
    return CROSSOS_OK;
#else
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
#endif
}

crossos_result_t crossos_http_get(const char *url,
                                  int timeout_ms,
                                  crossos_http_response_t *out_response)
{
    crossos_http_request_t request;
    memset(&request, 0, sizeof(request));
    request.method = "GET";
    request.url = url;
    request.timeout_ms = timeout_ms;
    return crossos_http_request(&request, out_response);
}

void crossos_http_response_free(crossos_http_response_t *response)
{
    if (!response) {
        return;
    }
    free(response->body);
    response->body = NULL;
    response->body_size = 0;
    response->status_code = 0;
}
