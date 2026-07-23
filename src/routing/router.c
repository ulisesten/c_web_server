#define _GNU_SOURCE

#include "cws/route.h"
#include "cws/errors.h"
#include "cws/log.h"

#include <stdlib.h>
#include <string.h>

typedef struct cws_route_node {
    char                  segment[64];
    size_t                segment_len;
    int                   is_param;       /* segment == ":name"     */
    int                   is_wildcard;     /* segment == "*"         */
    cws_method_t          method;          /* 0 if no handler at this node */
    cws_handler_t         handler;

    /* Per-router middleware stack at THIS node (empty for non-root nodes
     * unless explicitly mounted). The root node carries the router's
     * global middleware. Sub-router mount points store the sub-router's
     * root node mws + the sub-router subtree.
     */
    cws_middleware_fn     mws[CWS_MAX_PIPELINE];
    int                   mws_count;

    /* Sub-router mount: if non-NULL, requests reaching this node with a
     * longer path are delegated to sub_router. */
    cws_router_t*          sub_router;

    struct cws_route_node* children;
    int                   children_count;
    int                   children_cap;
} cws_route_node_t;

struct cws_router {
    cws_route_node_t* root;
    size_t            size;
};

static cws_route_node_t* node_new(const char* seg, size_t seglen) {
    cws_route_node_t* n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    if (seglen >= sizeof(n->segment)) seglen = sizeof(n->segment) - 1;
    memcpy(n->segment, seg, seglen);
    n->segment[seglen] = '\0';
    n->segment_len = seglen;
    if (seglen == 1 && seg[0] == '*') n->is_wildcard = 1;
    else if (seglen >= 1 && seg[0] == ':') n->is_param = 1;
    return n;
}

static void node_free(cws_route_node_t* n) {
    if (!n) return;
    for (int i = 0; i < n->children_count; i++) node_free(n->children + i);
    free(n->children);
}

static cws_route_node_t* node_add_child(cws_route_node_t* n, const char* seg, size_t seglen) {
    if (n->children_count == n->children_cap) {
        int ncap = n->children_cap ? n->children_cap * 2 : 4;
        cws_route_node_t* arr = realloc(n->children, (size_t)ncap * sizeof(*arr));
        if (!arr) return NULL;
        n->children = arr;
        n->children_cap = ncap;
    }
    cws_route_node_t* c = &n->children[n->children_count++];
    memset(c, 0, sizeof(*c));
    if (seglen >= sizeof(c->segment)) seglen = sizeof(c->segment) - 1;
    memcpy(c->segment, seg, seglen);
    c->segment[seglen] = '\0';
    c->segment_len = seglen;
    if (seglen == 1 && seg[0] == '*') c->is_wildcard = 1;
    else if (seglen >= 1 && seg[0] == ':') c->is_param = 1;
    return c;
}

cws_router_t* cws_router_new(void) {
    cws_router_t* r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->root = node_new("", 0);
    if (!r->root) { free(r); return NULL; }
    return r;
}

void cws_router_free(cws_router_t* router) {
    if (!router) return;
    /* We do NOT free sub_routers: they're owned by the caller (cws_app
     * or whoever created them). */
    node_free(router->root);
    free(router->root);
    free(router);
}

int cws_router_use(cws_router_t* router, cws_middleware_fn middleware) {
    if (!router || !middleware) return CWS_ERR_INVALID;
    cws_route_node_t* root = router->root;
    if (root->mws_count >= CWS_MAX_PIPELINE) return CWS_ERR_OVERFLOW;
    root->mws[root->mws_count++] = middleware;
    return CWS_OK;
}

int cws_router_add(cws_router_t* router, cws_method_t method,
                   const char* pattern, cws_handler_t handler) {
    if (!router || !pattern || !handler) return CWS_ERR_INVALID;
    cws_route_node_t* cur = router->root;
    const char* p = pattern;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char* slash = strchr(p, '/');
        size_t slen = slash ? (size_t)(slash - p) : strlen(p);
        cws_route_node_t* child = NULL;
        for (int i = 0; i < cur->children_count; i++) {
            cws_route_node_t* c = &cur->children[i];
            if ((c->is_param || c->is_wildcard)) {
                if ((slen == 1 && p[0] == '*') || (slen >= 1 && p[0] == ':')) {
                    if (c->segment_len == slen || (c->is_wildcard && slen == 1)) { child = c; break; }
                }
            }
            if (!c->is_param && !c->is_wildcard &&
                c->segment_len == slen && memcmp(c->segment, p, slen) == 0) { child = c; break; }
        }
        if (!child) {
            child = node_add_child(cur, p, slen);
            if (!child) return CWS_ERR_NOMEM;
        }
        cur = child;
        p += slen;
    }
    cur->method  = method;
    cur->handler = handler;
    router->size++;
    return CWS_OK;
}

int cws_router_mount(cws_router_t* router, const char* prefix,
                    cws_router_t* sub_router) {
    if (!router || !prefix || !sub_router) return CWS_ERR_INVALID;
    cws_route_node_t* cur = router->root;
    const char* p = prefix;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char* slash = strchr(p, '/');
        size_t slen = slash ? (size_t)(slash - p) : strlen(p);
        cws_route_node_t* child = NULL;
        for (int i = 0; i < cur->children_count; i++) {
            cws_route_node_t* c = &cur->children[i];
            if ((c->is_param || c->is_wildcard)) {
                if ((slen == 1 && p[0] == '*') || (slen >= 1 && p[0] == ':')) {
                    if (c->segment_len == slen || (c->is_wildcard && slen == 1)) { child = c; break; }
                }
            }
            if (!c->is_param && !c->is_wildcard &&
                c->segment_len == slen && memcmp(c->segment, p, slen) == 0) { child = c; break; }
        }
        if (!child) {
            child = node_add_child(cur, p, slen);
            if (!child) return CWS_ERR_NOMEM;
        }
        cur = child;
        p += slen;
    }
    cur->sub_router = sub_router;
    return CWS_OK;
}

typedef struct {
    cws_route_node_t* node;
    size_t            consumed;
    int                hit_leaf;
} cws_match_state_t;

static void match_node(cws_route_node_t* cur, const char* path, size_t path_len,
                       size_t i, cws_query_kv_t* params, size_t params_cap,
                       size_t* params_count, cws_match_state_t* win) {
    while (i < path_len) {
        while (i < path_len && path[i] == '/') i++;
        if (i >= path_len) break;
        size_t start = i;
        while (i < path_len && path[i] != '/') i++;
        size_t slen = i - start;
        cws_route_node_t* next = NULL;
        for (int k = 0; k < cur->children_count; k++) {
            cws_route_node_t* c = &cur->children[k];
            if (!c->is_param && !c->is_wildcard &&
                c->segment_len == slen && memcmp(c->segment, path + start, slen) == 0) {
                next = c; break;
            }
        }
        if (!next) {
            for (int k = 0; k < cur->children_count; k++) {
                cws_route_node_t* c = &cur->children[k];
                if (c->is_wildcard) { next = c; break; }
                if (c->is_param) {
                    next = c;
                    if (params && *params_count < params_cap) {
                        cws_query_kv_t* kv = &params[(*params_count)++];
                        kv->key = c->segment + 1;
                        kv->key_len = c->segment_len - 1;
                        kv->value = (char*)(path + start);
                        kv->value_len = slen;
                    }
                    break;
                }
            }
        }
        if (!next) return;
        cur = next;
        /* Stop traversing if this node has a sub-router mounted; the
         * remaining path will be matched by the sub-router. */
        if (cur->sub_router) {
            win->node    = cur;
            win->consumed = i;
            win->hit_leaf = 1;
            return;
        }
    }
    win->node    = cur;
    win->consumed = i;
    win->hit_leaf = 1;
}

int cws_router_match_pipeline(cws_router_t* router, cws_method_t method,
                              const char* path, size_t path_len,
                              cws_query_kv_t* params, size_t params_cap,
                              size_t* params_count,
                              cws_pipeline_t* out) {
    if (!router || !path || !out) return CWS_ERR_INVALID;
    *params_count = 0;
    memset(out, 0, sizeof(*out));

    /* Root router global middleware always runs first. */
    cws_route_node_t* root = router->root;
    for (int k = 0; k < root->mws_count && out->count < CWS_MAX_PIPELINE; k++) {
        out->mws[out->count++] = root->mws[k];
    }

    cws_match_state_t win = {0};
    match_node(router->root, path, path_len, 0, params, params_cap,
               params_count, &win);
    if (!win.hit_leaf) return CWS_ERR_NOTFOUND;

    cws_route_node_t* cur = win.node;

    /* Descend into sub-routers while remaining path has segments or is
     * empty (handles /videos/ -> sub-router route "/" ). */
    while (cur && cur->sub_router) {
        const char* rest = path + win.consumed;
        size_t rest_len = path_len - win.consumed;
        while (rest_len > 0 && rest[0] == '/') { rest++; rest_len--; }
        if (rest_len == 0 && win.consumed >= path_len) {
            /* Path fully consumed by prefix; sub-router must match "/". */
            cws_route_node_t* sub_root = cur->sub_router->root;
            for (int k = 0; k < sub_root->mws_count && out->count < CWS_MAX_PIPELINE; k++) {
                out->mws[out->count++] = sub_root->mws[k];
            }
            if (sub_root->handler && (sub_root->method == method || sub_root->method == CWS_M_UNKNOWN)) {
                out->handler = sub_root->handler;
                return CWS_OK;
            }
            cur = sub_root;
            break;
        }
        cws_route_node_t* sub_root = cur->sub_router->root;
        for (int k = 0; k < sub_root->mws_count && out->count < CWS_MAX_PIPELINE; k++) {
            out->mws[out->count++] = sub_root->mws[k];
        }
        cws_match_state_t sub_win = {0};
        match_node(sub_root, rest, rest_len, 0,
                   params, params_cap, params_count, &sub_win);
        if (!sub_win.hit_leaf) return CWS_ERR_NOTFOUND;
        cur = sub_win.node;
        win.consumed += sub_win.consumed;
        if (win.consumed > path_len) win.consumed = path_len;
    }

    if (cur && cur->handler && (cur->method == method || cur->method == CWS_M_UNKNOWN)) {
        out->handler = cur->handler;
        return CWS_OK;
    }
    return CWS_ERR_NOTFOUND;
}

cws_handler_t cws_router_match(cws_router_t* router, cws_method_t method,
                               const char* path, size_t path_len,
                               cws_query_kv_t* params, size_t params_cap,
                               size_t* params_count) {
    cws_pipeline_t p;
    int rc = cws_router_match_pipeline(router, method, path, path_len,
                                       params, params_cap, params_count, &p);
    if (rc == CWS_OK) return p.handler;
    return NULL;
}

size_t cws_router_size(const cws_router_t* router) {
    return router ? router->size : 0;
}
