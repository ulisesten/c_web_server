#define _GNU_SOURCE

#include "cws/cws.h"

#include <string.h>

static cws_app_t* g_app = NULL;

static CWS_HANDLER(index_handler) {
    (void)req;
    cws_response_sendfile(res, "assets/index.html", CWS_MT_TEXT_HTML);
}

static CWS_HANDLER(custom_handler) {
    (void)req;
    const char* body = "{\"message\":\"Hello from custom handler!\",\"error\":false}";
    cws_response_body(res, body, strlen(body), CWS_MT_APPLICATION_JSON);
    cws_response_send(res);
}

static CWS_HANDLER(health_handler) {
    (void)req;
    const char* body = "{\"status\":\"ok\"}";
    cws_response_body(res, body, strlen(body), CWS_MT_APPLICATION_JSON);
    cws_response_send(res);
}

static CWS_HANDLER(metrics_handler) {
    (void)req;
    const cws_metrics_t* m = cws_app_metrics(g_app);
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

int main(void) {
    g_app = cws_app_new();
    if (!g_app) return 1;

    cws_app_port(g_app, 8080);
    cws_app_bind(g_app, "0.0.0.0");
    cws_app_workers(g_app, 0);
    cws_app_pin(g_app, 1);
    cws_app_reuse_port(g_app, 1);
    cws_app_static_dir(g_app, "assets");
    cws_app_log_level(g_app, CWS_LOG_INFO);

    CWS_GET(g_app, "/",         index_handler);
    CWS_GET(g_app, "/custom",   custom_handler);
    CWS_GET(g_app, "/healthz",  health_handler);
    CWS_GET(g_app, "/metrics",  metrics_handler);

    int rc = cws_app_run(g_app);
    cws_app_free(g_app);
    return rc == CWS_OK ? 0 : 1;
}
