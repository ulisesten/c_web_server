#ifndef CWS_ROUTE_H
#define CWS_ROUTE_H

#include <stddef.h>

#include <stddef.h>

#include "request.h"
#include "response.h"
#include "middleware.h"

typedef struct cws_router cws_router_t;

cws_router_t* cws_router_new(void);
void          cws_router_free(cws_router_t* router);

/* Attach a global middleware to be executed before every route in this
 * router. Stable order = registration order. Capped at CWS_MAX_PIPELINE.
 */
int  cws_router_use(cws_router_t* router, cws_middleware_fn middleware);

/*
 * Mount another router under a path prefix. Requests matching the prefix
 * are dispatched to the sub-router; route params captured on the prefix
 * are preserved in req->path_params. The prefix can contain :params
 * (e.g.  /api/:version ). Trailing slash on prefix is ignored.
 */
int  cws_router_mount(cws_router_t* router, const char* prefix,
                     cws_router_t* sub_router);

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
 * Match a (method, path) against the router. Fills `out` pipeline with
 * the per-router middleware stack (ordering: root router mws first, then
 * sub-router mws) plus the matched handler. Captures :param into the
 * provided params array. Returns CWS_OK on match, CWS_ERR_NOTFOUND if
 * no route matched, or another error code on internal failure.
 */
int cws_router_match_pipeline(cws_router_t* router, cws_method_t method,
                              const char* path, size_t path_len,
                              cws_query_kv_t* params, size_t params_cap,
                              size_t* params_count,
                              cws_pipeline_t* out);

/*
 * Legacy: return the handler only (no middleware). Useful for tests and
 * simple embedders. Returns NULL if no match.
 */
cws_handler_t cws_router_match(cws_router_t* router, cws_method_t method,
                               const char* path, size_t path_len,
                               cws_query_kv_t* params, size_t params_cap,
                               size_t* params_count);

size_t cws_router_size(const cws_router_t* router);

#endif
