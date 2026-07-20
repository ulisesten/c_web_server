#ifndef CWS_ROUTE_H
#define CWS_ROUTE_H

#include <stddef.h>

#include "request.h"
#include "response.h"

typedef void (*cws_handler_t)(cws_request_t* req, cws_response_t* res);
typedef int  (*cws_middleware_t)(cws_request_t* req, cws_response_t* res, cws_handler_t next);

typedef struct cws_router cws_router_t;

cws_router_t* cws_router_new(void);
void          cws_router_free(cws_router_t* router);

/*
 * Register a route. Supported patterns:
 *   /static/foo                      exact match
 *   /users/:id                       capture as req->path_params
 *   /files/STAR                       wildcard (suffix)
 *   /api/:version/users/:id          mixed
 *
 * Duplicate registrations replace the previous handler.
 */
int cws_router_add(cws_router_t* router, cws_method_t method,
                   const char* pattern, cws_handler_t handler);

/*
 * Match a (method, path) pair against the router.
 * Returns the handler or NULL. Captures :param into the provided params
 * array (caller-provided, must have capacity for path segment count).
 */
cws_handler_t cws_router_match(cws_router_t* router, cws_method_t method,
                               const char* path, size_t path_len,
                               cws_query_kv_t* params, size_t params_cap,
                               size_t* params_count);

/*
 * Walk all routes (for /metrics exposure and debugging).
 */
size_t cws_router_size(const cws_router_t* router);

#endif
