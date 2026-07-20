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
    node_free(router->root);
    free(router->root);
    free(router);
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

cws_handler_t cws_router_match(cws_router_t* router, cws_method_t method,
                               const char* path, size_t path_len,
                               cws_query_kv_t* params, size_t params_cap,
                               size_t* params_count) {
    if (!router || !path) return NULL;
    *params_count = 0;
    cws_route_node_t* cur = router->root;
    size_t i = 0;
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
        if (!next) return NULL;
        cur = next;
    }
    if (cur->handler && (cur->method == method || cur->method == CWS_M_UNKNOWN)) {
        return cur->handler;
    }
    return NULL;
}

size_t cws_router_size(const cws_router_t* router) {
    return router ? router->size : 0;
}
