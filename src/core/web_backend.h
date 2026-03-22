#ifndef CROSSOS_WEB_BACKEND_H
#define CROSSOS_WEB_BACKEND_H

#include <crossos/web.h>

crossos_result_t crossos__web_backend_request(const crossos_http_request_t *request,
                                              crossos_http_response_t *out_response);

#endif /* CROSSOS_WEB_BACKEND_H */
