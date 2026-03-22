#include "web_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

extern void crossos__set_error(const char *fmt, ...);

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

crossos_result_t crossos__web_backend_request(const crossos_http_request_t *request,
                                              crossos_http_response_t *out_response)
{
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
}
