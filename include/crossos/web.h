/**
 * crossos/web.h  -  HTTP client helpers for API calls.
 */

#ifndef CROSSOS_WEB_H
#define CROSSOS_WEB_H

#include "types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct crossos_http_request {
    const char *method;        /**< "GET", "POST", etc. */
    const char *url;           /**< Absolute URL (http:// or https://) */
    const void *body;          /**< Optional request body */
    size_t      body_size;     /**< Request body size in bytes */
    const char *content_type;  /**< Optional content-type header */
    int         timeout_ms;    /**< 0 uses default timeout */
} crossos_http_request_t;

typedef struct crossos_http_response {
    long   status_code;
    void  *body;
    size_t body_size;
} crossos_http_response_t;

crossos_result_t crossos_http_request(const crossos_http_request_t *request,
                                      crossos_http_response_t *out_response);

crossos_result_t crossos_http_get(const char *url,
                                  int timeout_ms,
                                  crossos_http_response_t *out_response);

void crossos_http_response_free(crossos_http_response_t *response);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CROSSOS_WEB_H */
