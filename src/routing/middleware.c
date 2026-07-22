#define _GNU_SOURCE

#include "cws/middleware.h"

#include <string.h>

typedef struct link {
    const cws_pipeline_t* p;
    int                   idx;
    cws_request_t*        req;
    cws_response_t*       res;
} link_t;

static void advance(link_t* l);

static void next_trampoline(cws_request_t* req, cws_response_t* res) {
    link_t* l = (link_t*)req->__next_ctx;
    (void)res;
    if (!l) return;
    advance(l);
}

static void advance(link_t* l) {
    if (l->idx >= l->p->count) {
        if (l->p->handler) {
            l->p->handler(l->req, l->res);
        }
        return;
    }
    cws_middleware_fn mw = l->p->mws[l->idx];
    link_t child = {
        .p   = l->p,
        .idx = l->idx + 1,
        .req = l->req,
        .res = l->res,
    };
    cws_request_t saved = *l->req;
    l->req->__next_ctx = &child;
    mw(l->req, l->res, next_trampoline);
    *l->req = saved;  /* restore __next_ctx (user might have mutated fields) */
}

int cws_pipeline_init(cws_pipeline_t* p, const cws_middleware_fn* mws,
                      int n_mws, cws_handler_t handler) {
    if (!p) return 0;
    memset(p, 0, sizeof(*p));
    if (n_mws > CWS_MAX_PIPELINE) n_mws = CWS_MAX_PIPELINE;
    if (n_mws < 0) n_mws = 0;
    for (int i = 0; i < n_mws; i++) {
        if (mws && mws[i]) p->mws[p->count++] = mws[i];
    }
    p->handler = handler;
    return p->count;
}

void cws_pipeline_run(const cws_pipeline_t* p, cws_request_t* req,
                      cws_response_t* res) {
    if (!p || !req || !res) return;
    if (p->count == 0) {
        if (p->handler) p->handler(req, res);
        return;
    }
    link_t root = { .p = p, .idx = 0, .req = req, .res = res };
    advance(&root);
}
