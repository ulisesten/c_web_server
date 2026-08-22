#define _GNU_SOURCE

#include "cws/parser.h"
#include "cws/errors.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void cws_parser_init(cws_parser_t* p) {
    memset(p, 0, sizeof(*p));
    p->state = CWS_PARSER_START;
}

void cws_parser_reset(cws_parser_t* p) {
    cws_parser_t fresh;
    cws_parser_init(&fresh);
    fresh.state = CWS_PARSER_START;
    *p = fresh;
}

static int parse_version(cws_parser_t* p, const char* s, size_t n) {
    if (n != 8) return CWS_ERR_PROTOCOL;
    if (memcmp(s, "HTTP/1.", 7) != 0) return CWS_ERR_PROTOCOL;
    char c = s[7];
    if (c == '0')      { p->request.http_major = 1; p->request.http_minor = 0; }
    else if (c == '1') { p->request.http_major = 1; p->request.http_minor = 1; }
    else               return CWS_ERR_PROTOCOL;
    return CWS_OK;
}

static int commit_headers(cws_parser_t* p) {
    p->request.method_str = cws_method_to_str(p->request.method);

    int ka = (p->request.http_major == 1 && p->request.http_minor == 1) ? 1 : 0;
    const char* conn = cws_request_header(&p->request, "connection");
    if (conn) {
        if (strcasecmp(conn, "close") == 0)        ka = 0;
        else if (strcasecmp(conn, "keep-alive") == 0) ka = 1;
    }
    p->request.keep_alive = ka;

    const char* cl = cws_request_header(&p->request, "content-length");
    if (cl) {
        char* end = NULL;
        unsigned long long v = strtoull(cl, &end, 10);
        if (end == cl) return CWS_ERR_PROTOCOL;
        p->content_length_seen = 1;
        p->body_target = (size_t)v;
    }
    const char* te = cws_request_header(&p->request, "transfer-encoding");
    /* No se implementan bodies con Transfer-Encoding (chunked); aceptarlos
     * dejaría el body sin consumir (ambiguo con Content-Length → riesgo de
     * request smuggling). Se rechaza con 400. */
    if (te) return CWS_ERR_PROTOCOL;

    return CWS_OK;
}

int cws_parser_feed(cws_parser_t* p, const char* buf, size_t len,
                    size_t max_header_size, size_t max_body_size) {
    if (!p || (!buf && len)) return CWS_ERR_INVALID;
    if (p->state == CWS_PARSER_DONE || p->state == CWS_PARSER_ERROR) return CWS_OK;

    size_t i = 0;
    while (i < len) {
        char c = buf[i];
        switch (p->state) {
            case CWS_PARSER_START:
                p->request.method = CWS_M_UNKNOWN;
                p->state = CWS_PARSER_METHOD;
                /* fallthrough */
            case CWS_PARSER_METHOD: {
                const char* start = buf + i;
                size_t avail = len - i;
                const char* sp = memchr(start, ' ', avail);
                if (!sp) {
                    return CWS_OK;
                }
                size_t ml = (size_t)(sp - start);
                char tmp[17];
                if (ml == 0 || ml >= sizeof(tmp)) { p->state = CWS_PARSER_ERROR; return CWS_ERR_PROTOCOL; }
                memcpy(tmp, start, ml);
                tmp[ml] = '\0';
                p->request.method = cws_method_from_str(tmp, ml);
                if (p->request.method == CWS_M_UNKNOWN) { p->state = CWS_PARSER_ERROR; return CWS_ERR_PROTOCOL; }
                i += ml + 1;
                p->header_bytes += ml + 1;
                p->state = CWS_PARSER_PATH;
                break;
            }
            case CWS_PARSER_PATH: {
                const char* start = buf + i;
                size_t avail = len - i;
                const char* sp = memchr(start, ' ', avail);
                if (!sp) {
                    return CWS_OK;
                }
                size_t pl = (size_t)(sp - start);
                if (pl == 0) { p->state = CWS_PARSER_ERROR; return CWS_ERR_PROTOCOL; }
                const char* q = memchr(start, '?', pl);
                size_t path_len = q ? (size_t)(q - start) : pl;
                p->request.path = (char*)start;
                p->request.path_len = path_len;
                if (q) {
                    p->request.query_raw = (char*)(q + 1);
                    p->request.query_raw_len = pl - path_len - 1;
                } else {
                    p->request.query_raw = NULL;
                    p->request.query_raw_len = 0;
                }
                i += pl + 1;
                p->header_bytes += pl + 1;
                p->state = CWS_PARSER_VERSION;
                break;
            }
            case CWS_PARSER_VERSION: {
                const char* start = buf + i;
                size_t avail = len - i;
                const char* nl = memchr(start, '\n', avail);
                if (!nl) return CWS_OK;
                size_t vlen = (size_t)(nl - start);
                if (vlen >= 1 && start[vlen-1] == '\r') vlen--;
                int rc = parse_version(p, start, vlen);
                if (rc != CWS_OK) { p->state = CWS_PARSER_ERROR; return rc; }
                i += (size_t)(nl - start) + 1;
                p->header_bytes += (size_t)(nl - start) + 1;
                p->state = CWS_PARSER_HEADER_NAME;
                break;
            }
            case CWS_PARSER_HEADER_NAME: {
                if (c == '\r') {
                    p->state = CWS_PARSER_R2;
                    i++;
                    p->header_bytes++;
                    break;
                }
                if (c == '\n') {
                    p->header_bytes++;
                    i++;
                    int rc = commit_headers(p);
                    if (rc != CWS_OK) { p->state = CWS_PARSER_ERROR; return rc; }
                    p->state = (p->content_length_seen || p->chunked) ? CWS_PARSER_BODY : CWS_PARSER_DONE;
                    break;
                }
                {
                    const char* start = buf + i;
                    size_t avail = len - i;
                    const char* colon = memchr(start, ':', avail);
                    if (!colon) return CWS_OK;
                    size_t nl = (size_t)(colon - start);
                    if (nl == 0) { p->state = CWS_PARSER_ERROR; return CWS_ERR_PROTOCOL; }
                    if (p->request.headers_count >= CWS_MAX_HEADERS) { p->state = CWS_PARSER_ERROR; return CWS_ERR_OVERFLOW; }
                    cws_header_t* h = &p->request.headers[p->request.headers_count++];
                    h->name = (char*)start;
                    h->name_len = nl;
                    h->value = NULL;
                    h->value_len = 0;
                    i += nl + 1;
                    p->header_bytes += nl + 1;
                    p->state = CWS_PARSER_HEADER_VALUE;
                    break;
                }
            }
            case CWS_PARSER_HEADER_VALUE: {
                while (i < len && (buf[i] == ' ' || buf[i] == '\t')) { i++; p->header_bytes++; }
                if (i >= len) return CWS_OK;
                const char* start = buf + i;
                size_t avail = len - i;
                const char* nl = memchr(start, '\n', avail);
                if (!nl) return CWS_OK;
                size_t vlen = (size_t)(nl - start);
                if (vlen >= 1 && start[vlen-1] == '\r') vlen--;
                cws_header_t* h = &p->request.headers[p->request.headers_count - 1];
                h->value = (char*)start;
                h->value_len = vlen;
                i += (size_t)(nl - start) + 1;
                p->header_bytes += (size_t)(nl - start) + 1;
                if (p->header_bytes > max_header_size) { p->state = CWS_PARSER_ERROR; return CWS_ERR_OVERFLOW; }
                p->state = CWS_PARSER_HEADER_NAME;
                break;
            }
            case CWS_PARSER_R2: {
                if (c == '\n') {
                    p->header_bytes++;
                    i++;
                    int rc = commit_headers(p);
                    if (rc != CWS_OK) { p->state = CWS_PARSER_ERROR; return rc; }
                    p->state = (p->content_length_seen || p->chunked) ? CWS_PARSER_BODY : CWS_PARSER_DONE;
                    break;
                }
                p->state = CWS_PARSER_ERROR;
                return CWS_ERR_PROTOCOL;
            }
            case CWS_PARSER_BODY: {
                if (!p->content_length_seen) {
                    p->state = CWS_PARSER_DONE;
                    break;
                }
                size_t avail = len - i;
                size_t need = p->body_target - p->body_bytes;
                size_t take = avail < need ? avail : need;
                if (p->body_bytes == 0) p->request.body = (char*)(buf + i);
                p->body_bytes += take;
                p->request.body_len = p->body_bytes;
                i += take;
                if (p->body_bytes > max_body_size) { p->state = CWS_PARSER_ERROR; return CWS_ERR_OVERFLOW; }
                if (p->body_bytes >= p->body_target) {
                    p->state = CWS_PARSER_DONE;
                }
                break;
            }
            case CWS_PARSER_HEADERS_DONE:
            case CWS_PARSER_DONE:
                return CWS_OK;
            case CWS_PARSER_ERROR:
                return CWS_ERR_PROTOCOL;
            default:
                p->state = CWS_PARSER_ERROR;
                return CWS_ERR_PROTOCOL;
        }
    }
    return CWS_OK;
}

cws_parser_state_t cws_parser_state(const cws_parser_t* p) { return p->state; }
const cws_request_t* cws_parser_request(const cws_parser_t* p) {
    if (p->state == CWS_PARSER_DONE) return &p->request;
    return NULL;
}
