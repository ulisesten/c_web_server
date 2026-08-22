#ifndef CWS_MIDDLEWARE_H
#define CWS_MIDDLEWARE_H

#include <stddef.h>

#include "request.h"
#include "response.h"

/* cws_handler_t lives in route.h, but we declare it here to break the
 * include cycle (route.h includes us). */
typedef void (*cws_handler_t)(cws_request_t* req, cws_response_t* res);

/*
 * Express.js-style middleware pipeline.
 *
 * A middleware receives the request, the response builder, and a `next`
 * continuation. It can:
 *   - mutate req/res (attach typed state via req->__user),
 *   - short-circuit by sending a response (and NOT calling next),
 *   - pass control down the chain by calling next(req, res).
 *
 * Pipelines exist at two levels:
 *   1. global   (cws_app_use(mw))         applied to every request first
 *   2. per-router (cws_router_use(mw))    applied to that router's routes
 *
 * Built-ins shipped: cws_mw_logger, cws_mw_recover, cws_mw_cors,
 * cws_mw_ratelimit_simple, cws_mw_require_header.
 */
typedef void (*cws_next_fn)(cws_request_t* req, cws_response_t* res);
typedef void (*cws_middleware_fn)(cws_request_t* req, cws_response_t* res,
                                  cws_next_fn next);

#define CWS_MAX_PIPELINE 16

typedef struct cws_pipeline {
    cws_middleware_fn mws[CWS_MAX_PIPELINE];
    int               count;
    cws_handler_t     handler;
} cws_pipeline_t;

/*
 * Build a pipeline of (mws, n_mws, handler). The executor stores the
 * output chain on the connection (the framework allocates / copies).
 * Returns the count of stacked middleware (= n_mws capped).
 */
int cws_pipeline_init(cws_pipeline_t* p, const cws_middleware_fn* mws,
                      int n_mws, cws_handler_t handler);

/*
 * Execute the pipeline. Calls mws[0]; the last component calls handler.
 * If pipeline is empty and handler is set, calls handler. If both empty,
 * does nothing (caller should 404).
 *
 * The continuation mechanism is implemented here: each middleware receives
 * a closure that, when invoked, advances the chain. Closures are stored on
 * the stack so no allocation is needed per request.
 *
 * IMPORTANTE (contrato síncrono): el middleware DEBE llamar a `next(req,res)`
 * (o enviar respuesta) ANTES de retornar. El closure vive en el stack del
 * ejecutor; invocar `next` de forma asíncrona (en otro hilo/timer/callback
 * después de retornar la función) es uso inválido: el puntero quedaría
 * colgando y el comportamiento es indefinido.
 */
void cws_pipeline_run(const cws_pipeline_t* p, cws_request_t* req,
                      cws_response_t* res);

/*
 * Built-in middlewares. Stateless; consume state via req->__user with
 * prefixed keys to avoid collisions.
 */

/* Log every request: method, path, status, latency_ms. */
void cws_mw_logger(cws_request_t* req, cws_response_t* res, cws_next_fn next);

/* CORS: adds Access-Control-Allow-Origin: * and short-circuits OPTIONS. */
void cws_mw_cors(cws_request_t* req, cws_response_t* res, cws_next_fn next);

/* Simple per-IP rate limiter (max req/min default 600). Configure with
 * cws_mw_ratelimit_configure() before app runs.
 */
void cws_mw_ratelimit_simple(cws_request_t* req, cws_response_t* res,
                             cws_next_fn next);
void cws_mw_ratelimit_configure(int requests_per_minute);

/* Reject requests without an Authorization header; otherwise continue. */
void cws_mw_require_auth(cws_request_t* req, cws_response_t* res,
                        cws_next_fn next);

#endif
