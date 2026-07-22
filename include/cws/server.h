#ifndef CWS_SERVER_H
#define CWS_SERVER_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "route.h"
#include "metrics.h"

/*
 * The server holds no global state. Multiple instances can coexist in
 * one process (e.g., for testing).
 */

typedef struct cws_server  cws_server_t;
typedef struct cws_router  cws_router_t;

typedef void (*cws_ready_cb)(const cws_server_t* srv, int err, void* user);

cws_server_t* cws_server_new(const cws_config_t* cfg);
void          cws_server_free(cws_server_t* srv);

/*
 * Attach a router. The router is owned by the caller and must outlive
 * the server; cws_server_free() does NOT free it.
 */
void          cws_server_set_router(cws_server_t* srv, cws_router_t* router);
cws_router_t* cws_server_router(const cws_server_t* srv);

const cws_metrics_t* cws_server_metrics(const cws_server_t* srv);
const cws_config_t*  cws_server_config(const cws_server_t* srv);

/*
 * Start the acceptor + worker pool. Blocks until cws_server_stop() is
 * called (e.g., from signal handler) or fatal error.
 */
int cws_server_run(cws_server_t* srv, cws_ready_cb on_ready, void* user);

/*
 * Start everything in background; returns immediately.
 * Caller must call cws_server_wait() to join, or cws_server_stop() to
 * signal shutdown from any thread.
 */
int  cws_server_run_async(cws_server_t* srv, cws_ready_cb on_ready, void* user);
int  cws_server_wait(cws_server_t* srv);

/*
 * Request graceful shutdown. Flushes in-flight responses, drains
 * keep-alive connections within cws_config.keepalive_idle_ms.
 * Safe to call from a signal handler.
 */
int cws_server_stop(cws_server_t* srv);

#endif
