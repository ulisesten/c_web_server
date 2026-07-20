#define _GNU_SOURCE

#include "cws/cws.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static cws_server_t* g_srv = NULL;

static void index_handler(cws_request_t* req, cws_response_t* res) {
    (void)req;
    cws_response_sendfile(res, "assets/index.html", CWS_MT_TEXT_HTML);
}

static void custom_handler(cws_request_t* req, cws_response_t* res) {
    (void)req;
    const char* body = "{\"message\":\"Hello from custom handler!\",\"error\":false}";
    cws_response_body(res, body, strlen(body), CWS_MT_APPLICATION_JSON);
    cws_response_send(res);
}

static void health_handler(cws_request_t* req, cws_response_t* res) {
    (void)req;
    const char* body = "{\"status\":\"ok\"}";
    cws_response_body(res, body, strlen(body), CWS_MT_APPLICATION_JSON);
    cws_response_send(res);
}

static void metrics_handler(cws_request_t* req, cws_response_t* res) {
    (void)req;
    const cws_metrics_t* m = cws_server_metrics(g_srv);
    if (!m) {
        cws_response_send_error(res, 500);
        return;
    }
    char* buf = NULL; size_t len = 0;
    if (cws_metrics_render(m, &buf, &len) != CWS_OK || !buf) {
        cws_response_send_error(res, 500);
        return;
    }
    cws_response_body_owned(res, buf, len, CWS_MT_TEXT_PLAIN);
    cws_response_send(res);
}

static void on_ready(const cws_server_t* srv, int err, void* user) {
    (void)user;
    if (err != CWS_OK) {
        fprintf(stderr, "Server failed to start: %s\n", cws_strerror(err));
        return;
    }
    const cws_config_t* cfg = cws_server_config(srv);
    cws_log_info("CWS listening on %s:%d  workers=%d",
                cfg->bind_addr, cfg->port, cfg->n_workers);
}

static void on_sigint(int sig) {
    (void)sig;
    if (g_srv) cws_server_stop(g_srv);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    cws_cpu_topology_t topo;
    if (cws_cpu_topology_detect(&topo) != CWS_OK)
        cws_log_warn("CPU topology detection failed; using defaults");

    cws_config_t cfg = cws_config_default();
    cfg.pin_workers = 1;
    cfg.reuse_port = (topo.n_logical > 1) ? 1 : 0;
    if (cws_config_validate(&cfg, &topo) != CWS_OK) {
        fprintf(stderr, "invalid config\n");
        return 1;
    }
    cws_log_init((cws_log_level_t)cfg.log_level, cws_log_default_sink, NULL);

    cws_router_t* router = cws_router_new();
    cws_router_add(router, CWS_M_GET, "/",         index_handler);
    cws_router_add(router, CWS_M_GET, "/custom",   custom_handler);
    cws_router_add(router, CWS_M_GET, "/healthz",  health_handler);
    cws_router_add(router, CWS_M_GET, "/metrics",  metrics_handler);

    cws_server_t* srv = cws_server_new(&cfg);
    if (!srv) { fprintf(stderr, "server_new failed\n"); return 1; }
    cws_server_set_router(srv, router);
    g_srv = srv;

    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int rc = cws_server_run(srv, on_ready, NULL);
    cws_log_info("shutdown: rc=%d", rc);
    cws_server_free(srv);
    cws_cpu_topology_free(&topo);
    cws_log_shutdown();
    return rc == CWS_OK ? 0 : 1;
}
