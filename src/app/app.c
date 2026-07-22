#define _GNU_SOURCE

#include "cws/app.h"
#include "cws/errors.h"
#include "cws/log.h"
#include "cws/cpu.h"

#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdio.h>

typedef struct cws_static_map {
    char*       fs_path;
    cws_mime_t  mime;
    struct cws_static_map* next;
} cws_static_map_t;

struct cws_app {
    cws_config_t       cfg;
    cws_cpu_topology_t topo;
    cws_router_t*      router;
    cws_server_t*      srv;
    cws_static_map_t*  statics;
    int                topology_detected;
    int                log_inited;
    cws_ready_cb       on_ready;
    void*              on_ready_user;
    int                running;
};

cws_app_t* cws_app_new(void) {
    cws_app_t* app = calloc(1, sizeof(*app));
    if (!app) return NULL;
    app->cfg = cws_config_default();
    app->router = cws_router_new();
    if (!app->router) {
        free(app);
        return NULL;
    }
    return app;
}

void cws_app_free(cws_app_t* app) {
    if (!app) return;
    if (app->srv)     cws_server_free(app->srv);
    if (app->router)  cws_router_free(app->router);
    if (app->topology_detected) cws_cpu_topology_free(&app->topo);
    if (app->log_inited)        cws_log_shutdown();
    free(app);
}

cws_app_t* cws_app_port(cws_app_t* app, int port) {
    if (app && port > 0 && port < 65536) app->cfg.port = port;
    return app;
}

cws_app_t* cws_app_bind(cws_app_t* app, const char* addr) {
    if (app && addr) {
        snprintf(app->cfg.bind_addr, sizeof(app->cfg.bind_addr), "%s", addr);
    }
    return app;
}

cws_app_t* cws_app_workers(cws_app_t* app, int n) {
    if (app) app->cfg.n_workers = n < 0 ? 0 : n;
    return app;
}

cws_app_t* cws_app_pin(cws_app_t* app, int enabled) {
    if (app) app->cfg.pin_workers = enabled ? 1 : 0;
    return app;
}

cws_app_t* cws_app_reuse_port(cws_app_t* app, int enabled) {
    if (app) app->cfg.reuse_port = enabled ? 1 : 0;
    return app;
}

cws_app_t* cws_app_log_level(cws_app_t* app, int level) {
    if (app) app->cfg.log_level = level;
    return app;
}

cws_app_t* cws_app_static_dir(cws_app_t* app, const char* dir) {
    if (app && dir) {
        snprintf(app->cfg.static_dir, sizeof(app->cfg.static_dir), "%s", dir);
    }
    return app;
}

cws_app_t* cws_app_max_body(cws_app_t* app, size_t bytes) {
    if (app) app->cfg.max_body_size = bytes;
    return app;
}

cws_app_t* cws_app_keepalive_ms(cws_app_t* app, uint32_t ms) {
    if (app) app->cfg.keepalive_idle_ms = ms;
    return app;
}

cws_app_t* cws_app_backlog(cws_app_t* app, int n) {
    if (app && n > 0) app->cfg.listen_backlog = n;
    return app;
}

cws_app_t* cws_app_tls(cws_app_t* app, const char* cert, const char* key) {
    if (!app) return NULL;
    if (cert) snprintf(app->cfg.tls_cert, sizeof(app->cfg.tls_cert), "%s", cert);
    if (key)  snprintf(app->cfg.tls_key,  sizeof(app->cfg.tls_key),  "%s", key);
    return app;
}

const cws_config_t*  cws_app_config(const cws_app_t* app)  { return app ? &app->cfg  : NULL; }
cws_router_t*        cws_app_router(const cws_app_t* app) { return app ? app->router : NULL; }
cws_server_t*        cws_app_server(const cws_app_t* app) { return app ? app->srv   : NULL; }
const cws_metrics_t* cws_app_metrics(const cws_app_t* app){
    return (app && app->srv) ? cws_server_metrics(app->srv) : NULL;
}

cws_app_t* cws_app_route(cws_app_t* app, cws_method_t method,
                         const char* pattern, cws_handler_t handler) {
    if (app && pattern && handler) {
        cws_router_add(app->router, method, pattern, handler);
    }
    return app;
}

typedef struct cws_static_binding {
    char*       url_path;
    char*       fs_path;
    cws_mime_t  mime;
} cws_static_binding_t;

static cws_static_binding_t* g_static_bindings = NULL;
static size_t                g_static_count     = 0;
static size_t                g_static_cap       = 0;

static void static_serve_handler(cws_request_t* req, cws_response_t* res) {
    for (size_t i = 0; i < g_static_count; i++) {
        if (strcmp(g_static_bindings[i].url_path, req->path) == 0) {
            cws_response_sendfile(res, g_static_bindings[i].fs_path,
                                  g_static_bindings[i].mime);
            return;
        }
    }
    cws_response_send_error(res, 404);
}

cws_app_t* cws_app_static(cws_app_t* app, const char* url_path,
                          const char* fs_path, cws_mime_t mime) {
    if (!app || !url_path || !fs_path) return app;
    if (g_static_count == g_static_cap) {
        size_t ncap = g_static_cap ? g_static_cap * 2 : 8;
        cws_static_binding_t* arr = realloc(g_static_bindings,
                                           ncap * sizeof(*arr));
        if (!arr) return app;
        g_static_bindings = arr;
        g_static_cap = ncap;
    }
    cws_static_binding_t* b = &g_static_bindings[g_static_count++];
    b->url_path = strdup(url_path);
    b->fs_path  = strdup(fs_path);
    b->mime     = mime;
    return cws_app_route(app, CWS_M_GET, url_path, static_serve_handler);
}

void cws_app_on_ready(cws_app_t* app, cws_ready_cb cb, void* user) {
    if (!app) return;
    app->on_ready = cb;
    app->on_ready_user = user;
}

static void app_default_ready(const cws_server_t* srv, int err, void* user) {
    (void)user;
    if (err != CWS_OK) {
        fprintf(stderr, "cws_app: failed to start: %s\n", cws_strerror(err));
        return;
    }
    const cws_config_t* cfg = cws_server_config(srv);
    cws_log_info("cws_app listening on %s:%d  workers=%d",
                cfg->bind_addr, cfg->port, cfg->n_workers);
}

static cws_app_t* g_app_for_signal = NULL;
static void app_signal_handler(int sig) {
    (void)sig;
    if (g_app_for_signal) cws_app_stop(g_app_for_signal);
}

static int app_bootstrap(cws_app_t* app) {
    if (!app || app->running) return CWS_ERR_INVALID;

    if (!app->topology_detected) {
        if (cws_cpu_topology_detect(&app->topo) != CWS_OK) {
            cws_log_warn("CPU topology detection failed; using defaults");
            memset(&app->topo, 0, sizeof(app->topo));
            app->topo.n_logical = (int)sysconf(_SC_NPROCESSORS_ONLN);
        }
        app->topology_detected = 1;
    }

    if (cws_config_validate(&app->cfg, &app->topo) != CWS_OK) {
        return CWS_ERR_INVALID;
    }

    if (!app->log_inited) {
        cws_log_init((cws_log_level_t)app->cfg.log_level, cws_log_default_sink, NULL);
        app->log_inited = 1;
    }
    return CWS_OK;
}

int cws_app_run(cws_app_t* app) {
    int rc = app_bootstrap(app);
    if (rc != CWS_OK) return rc;

    app->srv = cws_server_new(&app->cfg);
    if (!app->srv) return CWS_ERR_NOMEM;
    cws_server_set_router(app->srv, app->router);

    signal(SIGPIPE, SIG_IGN);
    g_app_for_signal = app;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = app_signal_handler;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    cws_ready_cb cb = app->on_ready ? app->on_ready : app_default_ready;
    app->running = 1;
    rc = cws_server_run(app->srv, cb, app->on_ready_user);
    app->running = 0;
    return rc;
}

int cws_app_run_async(cws_app_t* app) {
    int rc = app_bootstrap(app);
    if (rc != CWS_OK) return rc;

    app->srv = cws_server_new(&app->cfg);
    if (!app->srv) return CWS_ERR_NOMEM;
    cws_server_set_router(app->srv, app->router);

    cws_ready_cb cb = app->on_ready ? app->on_ready : app_default_ready;
    rc = cws_server_run_async(app->srv, cb, app->on_ready_user);
    if (rc == CWS_OK) app->running = 1;
    return rc;
}

int cws_app_wait(cws_app_t* app) {
    if (!app || !app->srv) return CWS_ERR_INVALID;
    int rc = cws_server_wait(app->srv);
    app->running = 0;
    return rc;
}

int cws_app_stop(cws_app_t* app) {
    if (!app || !app->srv) return CWS_ERR_INVALID;
    return cws_server_stop(app->srv);
}
