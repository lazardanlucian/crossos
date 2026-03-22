#include <crossos/web.h>

#include "web_backend.h"

#include <stdlib.h>
#include <string.h>

extern void crossos__set_error(const char *fmt, ...);

crossos_result_t crossos_http_request(const crossos_http_request_t *request,
                                      crossos_http_response_t *out_response)
{
    if (!request || !request->url || !out_response) {
        crossos__set_error("http_request: invalid parameter");
        return CROSSOS_ERR_PARAM;
    }

    memset(out_response, 0, sizeof(*out_response));

    return crossos__web_backend_request(request, out_response);
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
