#ifndef CWS_REQUEST_H
#define CWS_REQUEST_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    CWS_M_UNKNOWN = 0,
    CWS_M_GET,
    CWS_M_POST,
    CWS_M_PUT,
    CWS_M_DELETE,
    CWS_M_HEAD,
    CWS_M_OPTIONS,
    CWS_M_PATCH,
} cws_method_t;

typedef struct {
    char*  name;
    size_t name_len;
    char*  value;
    size_t value_len;
} cws_header_t;

typedef struct {
    char*  key;
    size_t key_len;
    char*  value;
    size_t value_len;
} cws_query_kv_t;

/*
 * Request struct. Strings reference slices in a per-connection arena
 * (no per-request allocations on the hot path).
 */
typedef struct {
    cws_method_t    method;
    const char*     method_str;     /* "GET", ... NUL-terminated */
    char*           path;           /* without query             */
    size_t          path_len;
    char*           query_raw;       /* after '?' (or NULL)       */
    size_t          query_raw_len;
    cws_query_kv_t* query_params;
    size_t          query_params_count;
    int             http_major;     /* always 1                 */
    int             http_minor;     /* 0 or 1                   */
    cws_header_t    headers[64];
    size_t          headers_count;
    char*           body;
    size_t          body_len;
    int             keep_alive;     /* 1 if Connection: keep-alive / HTTP/1.1 default */
    cws_query_kv_t  path_params[16];
    size_t          path_params_count;
    int             worker_idx;     /* filled by worker before handler */

    /*
     * Opaque slots for middleware/handlers. The framework uses __next_ctx
     * internally during pipeline execution; NEVER touch it from user code.
     * Applications/middlewares may store typed state in __user (with the
     * prefix-key convention appropriate to the middleware).
     */
    void*           __next_ctx;
    void*           __user;
} cws_request_t;

#define CWS_MAX_HEADERS     (64)
#define CWS_MAX_PATH_PARAMS (16)

cws_method_t cws_method_from_str(const char* s, size_t len);
const char*  cws_method_to_str(cws_method_t m);

const char*  cws_request_header(const cws_request_t* req, const char* name);
const char*  cws_request_query(const cws_request_t* req, const char* key);
const char*  cws_request_param(const cws_request_t* req, const char* name);

#endif
