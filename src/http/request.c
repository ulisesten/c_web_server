#define _GNU_SOURCE

#include "cws/request.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

cws_method_t cws_method_from_str(const char* s, size_t len) {
    switch (len) {
        case 3:
            if (s[0]=='G' && s[1]=='E' && s[2]=='T') return CWS_M_GET;
            if (s[0]=='P' && s[1]=='U' && s[2]=='T') return CWS_M_PUT;
            break;
        case 4:
            if (s[0]=='P' && s[1]=='O' && s[2]=='S' && s[3]=='T') return CWS_M_POST;
            if (s[0]=='H' && s[1]=='E' && s[2]=='A' && s[3]=='D') return CWS_M_HEAD;
            break;
        case 5:
            if (s[0]=='P' && s[1]=='A' && s[2]=='T' && s[3]=='C' && s[4]=='H')
                return CWS_M_PATCH;
            break;
        case 6:
            if (s[0]=='D' && s[1]=='E' && s[2]=='L' && s[3]=='E' && s[4]=='T' && s[5]=='E')
                return CWS_M_DELETE;
            break;
        case 7:
            if (s[0]=='O' && s[1]=='P' && s[2]=='T' && s[3]=='I' && s[4]=='O' && s[5]=='N' && s[6]=='S')
                return CWS_M_OPTIONS;
            break;
        default: break;
    }
    return CWS_M_UNKNOWN;
}

const char* cws_method_to_str(cws_method_t m) {
    switch (m) {
        case CWS_M_GET:     return "GET";
        case CWS_M_POST:    return "POST";
        case CWS_M_PUT:     return "PUT";
        case CWS_M_DELETE:  return "DELETE";
        case CWS_M_HEAD:    return "HEAD";
        case CWS_M_OPTIONS: return "OPTIONS";
        case CWS_M_PATCH:   return "PATCH";
        default:            return "UNKNOWN";
    }
}

static int str_ieq(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    }
    return 1;
}

const char* cws_request_header(const cws_request_t* req, const char* name) {
    if (!req || !name) return NULL;
    size_t nl = strlen(name);
    for (size_t i = 0; i < req->headers_count; i++) {
        if (req->headers[i].name_len == nl &&
            str_ieq(req->headers[i].name, name, nl)) {
            return req->headers[i].value;
        }
    }
    return NULL;
}

const char* cws_request_query(const cws_request_t* req, const char* key) {
    if (!req || !key) return NULL;
    size_t kl = strlen(key);
    for (size_t i = 0; i < req->query_params_count; i++) {
        if (req->query_params[i].key_len == kl &&
            memcmp(req->query_params[i].key, key, kl) == 0) {
            return req->query_params[i].value;
        }
    }
    return NULL;
}

const char* cws_request_param(const cws_request_t* req, const char* name) {
    if (!req || !name) return NULL;
    size_t kl = strlen(name);
    for (size_t i = 0; i < req->path_params_count; i++) {
        if (req->path_params[i].key_len == kl &&
            memcmp(req->path_params[i].key, name, kl) == 0) {
            return req->path_params[i].value;
        }
    }
    return NULL;
}
