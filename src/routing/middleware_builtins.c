#define _GNU_SOURCE

#include "cws/middleware.h"
#include "cws/log.h"

#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <time.h>
#include <ctype.h>

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

typedef struct {
    uint64_t started_ms;
    char     method[8];
    char     path[256];
} logger_ctx_t;

void cws_mw_logger(cws_request_t* req, cws_response_t* res, cws_next_fn next) {
    logger_ctx_t ctx;
    ctx.started_ms = now_ms();
    size_t ml = req->method_str ? strlen(req->method_str) : 0;
    if (ml > sizeof(ctx.method) - 1) ml = sizeof(ctx.method) - 1;
    memcpy(ctx.method, req->method_str ? req->method_str : "?", ml);
    ctx.method[ml] = '\0';
    size_t pl = req->path_len < sizeof(ctx.path) - 1 ? req->path_len : sizeof(ctx.path) - 1;
    memcpy(ctx.path, req->path, pl);
    ctx.path[pl] = '\0';

    next(req, res);

    uint64_t elapsed = now_ms() - ctx.started_ms;
    cws_log_info("%s %s status=%d latency=%lums",
                 ctx.method, ctx.path, res->status, (unsigned long)elapsed);
}

typedef struct { char buf[8192]; } cors_dummy_t;

void cws_mw_cors(cws_request_t* req, cws_response_t* res, cws_next_fn next) {
    cws_response_header(res, "Access-Control-Allow-Origin", "*");
    cws_response_header(res, "Access-Control-Allow-Methods",
                       "GET, POST, PUT, DELETE, HEAD, OPTIONS, PATCH");
    cws_response_header(res, "Access-Control-Allow-Headers", "Content-Type, Authorization");
    if (req->method == CWS_M_OPTIONS) {
        cws_response_status(res, 204);
        cws_response_send(res);
        return;
    }
    next(req, res);
}

/* ---- Simple per-IP rate limiter --------------------------------------- */

#define CWS_RL_BUCKETS 1024
static _Atomic uint64_t g_rl_buckets[CWS_RL_BUCKETS];
static atomic_int       g_rl_limit = 600;
static atomic_uint      g_rl_window_start = 0;

static unsigned rl_now_window(void) {
    return (unsigned)(now_ms() / 60000u);
}

static unsigned rl_hash_ip(const char* ip) {
    unsigned long h = 5381;
    while (*ip) h = ((h << 5) + h) + (unsigned char)*ip++;
    return (unsigned)(h % CWS_RL_BUCKETS);
}

void cws_mw_ratelimit_configure(int requests_per_minute) {
    atomic_store(&g_rl_limit, requests_per_minute);
}

void cws_mw_ratelimit_simple(cws_request_t* req, cws_response_t* res,
                             cws_next_fn next) {
    const char* host = cws_request_header(req, "host");
    if (!host) host = "?";
    unsigned h = rl_hash_ip(host);
    uint64_t cur = atomic_fetch_add_explicit(&g_rl_buckets[h], 1,
                                             memory_order_relaxed);
    /* Reset on window boundary. */
    unsigned w = rl_now_window();
    unsigned ws = atomic_load(&g_rl_window_start);
    if (w != ws) {
        if (atomic_compare_exchange_strong(&g_rl_window_start, &ws, w)) {
            for (int i = 0; i < CWS_RL_BUCKETS; i++)
                atomic_store(&g_rl_buckets[i], 0);
            cur = 1;
        }
    }
    int limit = atomic_load(&g_rl_limit);
    if ((int)cur > limit) {
        cws_response_status(res, 429);
        cws_response_header(res, "Retry-After", "60");
        cws_response_body(res, "rate limit exceeded\n", 20, CWS_MT_TEXT_PLAIN);
        cws_response_send(res);
        return;
    }
    next(req, res);
}

void cws_mw_require_auth(cws_request_t* req, cws_response_t* res,
                       cws_next_fn next) {
    const char* auth = cws_request_header(req, "authorization");
    if (!auth || auth[0] == '\0') {
        cws_response_status(res, 401);
        cws_response_header(res, "WWW-Authenticate", "Bearer");
        cws_response_body(res, "unauthorized\n", 13, CWS_MT_TEXT_PLAIN);
        cws_response_send(res);
        return;
    }
    next(req, res);
}
