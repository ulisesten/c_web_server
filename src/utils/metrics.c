#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "cws/metrics.h"
#include "cws/errors.h"

void cws_metrics_init(cws_metrics_t* m) {
    memset(m, 0, sizeof(*m));
    atomic_init(&m->requests_total, (uint_fast64_t)0);
    atomic_init(&m->requests_keepalive, (uint_fast64_t)0);
    atomic_init(&m->bytes_read, (uint_fast64_t)0);
    atomic_init(&m->bytes_written, (uint_fast64_t)0);
    atomic_init(&m->conn_accepted, (uint_fast64_t)0);
    atomic_init(&m->conn_closed, (uint_fast64_t)0);
    atomic_init(&m->conn_keepalive_reused, (uint_fast64_t)0);
    atomic_init(&m->errors_total, (uint_fast64_t)0);
    atomic_init(&m->status_2xx, (uint_fast64_t)0);
    atomic_init(&m->status_3xx, (uint_fast64_t)0);
    atomic_init(&m->status_4xx, (uint_fast64_t)0);
    atomic_init(&m->status_5xx, (uint_fast64_t)0);
}

void cws_metrics_inc_requests(cws_metrics_t* m, int keepalive) {
    atomic_fetch_add_explicit(&m->requests_total, (uint_fast64_t)1, memory_order_relaxed);
    if (keepalive) {
        atomic_fetch_add_explicit(&m->requests_keepalive, (uint_fast64_t)1, memory_order_relaxed);
    }
}

void cws_metrics_add_bytes_read(cws_metrics_t* m, size_t n) {
    atomic_fetch_add_explicit(&m->bytes_read, (uint_fast64_t)n, memory_order_relaxed);
}

void cws_metrics_add_bytes_written(cws_metrics_t* m, size_t n) {
    atomic_fetch_add_explicit(&m->bytes_written, (uint_fast64_t)n, memory_order_relaxed);
}

void cws_metrics_inc_conn_accepted(cws_metrics_t* m) {
    atomic_fetch_add_explicit(&m->conn_accepted, (uint_fast64_t)1, memory_order_relaxed);
}

void cws_metrics_inc_conn_closed(cws_metrics_t* m) {
    atomic_fetch_add_explicit(&m->conn_closed, (uint_fast64_t)1, memory_order_relaxed);
}

void cws_metrics_inc_conn_reused(cws_metrics_t* m) {
    atomic_fetch_add_explicit(&m->conn_keepalive_reused, (uint_fast64_t)1, memory_order_relaxed);
}

void cws_metrics_inc_error(cws_metrics_t* m) {
    atomic_fetch_add_explicit(&m->errors_total, (uint_fast64_t)1, memory_order_relaxed);
}

void cws_metrics_inc_status(cws_metrics_t* m, int status_class) {
    switch (status_class) {
        case 2:
            atomic_fetch_add_explicit(&m->status_2xx, (uint_fast64_t)1, memory_order_relaxed);
            break;
        case 3:
            atomic_fetch_add_explicit(&m->status_3xx, (uint_fast64_t)1, memory_order_relaxed);
            break;
        case 4:
            atomic_fetch_add_explicit(&m->status_4xx, (uint_fast64_t)1, memory_order_relaxed);
            break;
        case 5:
            atomic_fetch_add_explicit(&m->status_5xx, (uint_fast64_t)1, memory_order_relaxed);
            break;
        default:
            break;
    }
}

int cws_metrics_render(const cws_metrics_t* m, char** out_buf, size_t* out_len) {
    size_t cap = 4096;
    char* buf = (char*)malloc(cap);
    if (!buf) {
        return CWS_ERR_NOMEM;
    }
    size_t len = 0;
    int n;

#define CWS_METRICS_APPEND(...) do { \
        n = snprintf(buf + len, cap - len, __VA_ARGS__); \
        if (n < 0) { free(buf); return CWS_ERR_NOMEM; } \
        if ((size_t)n >= cap - len) { \
            size_t need = cap - len + (size_t)n + 1; \
            size_t newcap = cap; \
            while (newcap - len < need) { newcap *= 2; } \
            char* tmp = (char*)realloc(buf, newcap); \
            if (!tmp) { free(buf); return CWS_ERR_NOMEM; } \
            buf = tmp; \
            cap = newcap; \
            n = snprintf(buf + len, cap - len, __VA_ARGS__); \
            if (n < 0) { free(buf); return CWS_ERR_NOMEM; } \
        } \
        len += (size_t)n; \
    } while (0)

    uint_fast64_t v_requests = atomic_load_explicit(&m->requests_total, memory_order_relaxed);
    uint_fast64_t v_keepalive = atomic_load_explicit(&m->requests_keepalive, memory_order_relaxed);
    uint_fast64_t v_bytes_read = atomic_load_explicit(&m->bytes_read, memory_order_relaxed);
    uint_fast64_t v_bytes_written = atomic_load_explicit(&m->bytes_written, memory_order_relaxed);
    uint_fast64_t v_conn_accepted = atomic_load_explicit(&m->conn_accepted, memory_order_relaxed);
    uint_fast64_t v_conn_closed = atomic_load_explicit(&m->conn_closed, memory_order_relaxed);
    uint_fast64_t v_conn_reused = atomic_load_explicit(&m->conn_keepalive_reused, memory_order_relaxed);
    uint_fast64_t v_errors = atomic_load_explicit(&m->errors_total, memory_order_relaxed);
    uint_fast64_t v_2xx = atomic_load_explicit(&m->status_2xx, memory_order_relaxed);
    uint_fast64_t v_3xx = atomic_load_explicit(&m->status_3xx, memory_order_relaxed);
    uint_fast64_t v_4xx = atomic_load_explicit(&m->status_4xx, memory_order_relaxed);
    uint_fast64_t v_5xx = atomic_load_explicit(&m->status_5xx, memory_order_relaxed);

    CWS_METRICS_APPEND("# HELP cws_requests_total Total requests processed\n");
    CWS_METRICS_APPEND("# TYPE cws_requests_total counter\n");
    CWS_METRICS_APPEND("cws_requests_total %lu\n", (unsigned long)v_requests);

    CWS_METRICS_APPEND("# HELP cws_requests_keepalive_total Total requests served over reused keep-alive connections\n");
    CWS_METRICS_APPEND("# TYPE cws_requests_keepalive_total counter\n");
    CWS_METRICS_APPEND("cws_requests_keepalive_total %lu\n", (unsigned long)v_keepalive);

    CWS_METRICS_APPEND("# HELP cws_bytes_read_total Total bytes read from clients\n");
    CWS_METRICS_APPEND("# TYPE cws_bytes_read_total counter\n");
    CWS_METRICS_APPEND("cws_bytes_read_total %lu\n", (unsigned long)v_bytes_read);

    CWS_METRICS_APPEND("# HELP cws_bytes_written_total Total bytes written to clients\n");
    CWS_METRICS_APPEND("# TYPE cws_bytes_written_total counter\n");
    CWS_METRICS_APPEND("cws_bytes_written_total %lu\n", (unsigned long)v_bytes_written);

    CWS_METRICS_APPEND("# HELP cws_connections_accepted_total Total connections accepted\n");
    CWS_METRICS_APPEND("# TYPE cws_connections_accepted_total counter\n");
    CWS_METRICS_APPEND("cws_connections_accepted_total %lu\n", (unsigned long)v_conn_accepted);

    CWS_METRICS_APPEND("# HELP cws_connections_closed_total Total connections closed\n");
    CWS_METRICS_APPEND("# TYPE cws_connections_closed_total counter\n");
    CWS_METRICS_APPEND("cws_connections_closed_total %lu\n", (unsigned long)v_conn_closed);

    CWS_METRICS_APPEND("# HELP cws_connections_keepalive_reused_total Total keep-alive connections reused\n");
    CWS_METRICS_APPEND("# TYPE cws_connections_keepalive_reused_total counter\n");
    CWS_METRICS_APPEND("cws_connections_keepalive_reused_total %lu\n", (unsigned long)v_conn_reused);

    CWS_METRICS_APPEND("# HELP cws_errors_total Total errors encountered\n");
    CWS_METRICS_APPEND("# TYPE cws_errors_total counter\n");
    CWS_METRICS_APPEND("cws_errors_total %lu\n", (unsigned long)v_errors);

    CWS_METRICS_APPEND("# HELP cws_status_2xx_total Total HTTP responses with 2xx status\n");
    CWS_METRICS_APPEND("# TYPE cws_status_2xx_total counter\n");
    CWS_METRICS_APPEND("cws_status_2xx_total %lu\n", (unsigned long)v_2xx);

    CWS_METRICS_APPEND("# HELP cws_status_3xx_total Total HTTP responses with 3xx status\n");
    CWS_METRICS_APPEND("# TYPE cws_status_3xx_total counter\n");
    CWS_METRICS_APPEND("cws_status_3xx_total %lu\n", (unsigned long)v_3xx);

    CWS_METRICS_APPEND("# HELP cws_status_4xx_total Total HTTP responses with 4xx status\n");
    CWS_METRICS_APPEND("# TYPE cws_status_4xx_total counter\n");
    CWS_METRICS_APPEND("cws_status_4xx_total %lu\n", (unsigned long)v_4xx);

    CWS_METRICS_APPEND("# HELP cws_status_5xx_total Total HTTP responses with 5xx status\n");
    CWS_METRICS_APPEND("# TYPE cws_status_5xx_total counter\n");
    CWS_METRICS_APPEND("cws_status_5xx_total %lu\n", (unsigned long)v_5xx);

#undef CWS_METRICS_APPEND

    *out_buf = buf;
    *out_len = len;
    return CWS_OK;
}
