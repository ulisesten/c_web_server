#ifndef CWS_METRICS_H
#define CWS_METRICS_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

/*
 * Lightweight metrics counters for the hot path.
 * All counters are lock-free stdatomic_uint_fast64.
 *
 * Prometheus text exposition at /metrics is rendered by cws_metrics_render.
 */

typedef struct {
    _Atomic uint_fast64_t requests_total;
    _Atomic uint_fast64_t requests_keepalive;
    _Atomic uint_fast64_t bytes_read;
    _Atomic uint_fast64_t bytes_written;
    _Atomic uint_fast64_t conn_accepted;
    _Atomic uint_fast64_t conn_closed;
    _Atomic uint_fast64_t conn_keepalive_reused;
    _Atomic uint_fast64_t errors_total;
    _Atomic uint_fast64_t status_2xx;
    _Atomic uint_fast64_t status_3xx;
    _Atomic uint_fast64_t status_4xx;
    _Atomic uint_fast64_t status_5xx;
} cws_metrics_t;

void cws_metrics_init(cws_metrics_t* m);
void cws_metrics_inc_requests(cws_metrics_t* m, int keepalive);
void cws_metrics_add_bytes_read(cws_metrics_t* m, size_t n);
void cws_metrics_add_bytes_written(cws_metrics_t* m, size_t n);
void cws_metrics_inc_conn_accepted(cws_metrics_t* m);
void cws_metrics_inc_conn_closed(cws_metrics_t* m);
void cws_metrics_inc_conn_reused(cws_metrics_t* m);
void cws_metrics_inc_error(cws_metrics_t* m);
void cws_metrics_inc_status(cws_metrics_t* m, int status_class); /* 2,3,4,5 */

/*
 * Returns Prometheus text representation into a malloc'd buffer.
 * Caller must free(*out_buf).
 */
int cws_metrics_render(const cws_metrics_t* m, char** out_buf, size_t* out_len);

#endif
