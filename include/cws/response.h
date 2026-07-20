#ifndef CWS_RESPONSE_H
#define CWS_RESPONSE_H

#include <stddef.h>
#include <stdint.h>

#include "request.h"

/*
 * Chainable Response builder.
 *
 *   cws_response_t res;
 *   cws_response_init(&res, fd, req ? req->keep_alive : 0);
 *   cws_response_status(&res, 200);
 *   cws_response_header(&res, "Cache-Control", "no-store");
 *   cws_response_body(&res, body_ptr, body_len, CWS_MT_APPLICATION_JSON);
 *   cws_response_send(&res);     // writev(header+body) in one syscall
 *
 * For static files:
 *   cws_response_sendfile(&res, "/assets/index.html", CWS_MT_TEXT_HTML);
 *
 * Lifecycle: stack-allocated per request; no malloc required for typical
 * use. Falls back to dynamic allocation only for very large header bundles.
 */

typedef enum {
    CWS_MT_UNKNOWN = 0,
    CWS_MT_TEXT_HTML,
    CWS_MT_TEXT_PLAIN,
    CWS_MT_APPLICATION_JSON,
    CWS_MT_APPLICATION_OCTET_STREAM,
    CWS_MT_TEXT_CSS,
    CWS_MT_APPLICATION_JAVASCRIPT,
    CWS_MT_IMAGE_PNG,
    CWS_MT_IMAGE_JPEG,
    CWS_MT_IMAGE_SVG,
    CWS_MT_APPLICATION_OCTET,
} cws_mime_t;

typedef struct {
    int       status;
    int       client_fd;
    int       keep_alive;
    char      header_buf[2048];
    size_t    header_len;
    size_t    body_len;
    const void* body_ptr;
    int       body_owned;        /* 1 if we must free(body_ptr)         */
    cws_mime_t body_mime;
    int       sent;              /* 1 once cws_response_send succeeded */
} cws_response_t;

void cws_response_init(cws_response_t* res, int client_fd, int keep_alive);
void cws_response_status(cws_response_t* res, int status);
int  cws_response_header(cws_response_t* res, const char* name, const char* value_fmt, ...)
     __attribute__((format(printf, 3, 4)));
void cws_response_body(cws_response_t* res, const void* body, size_t len, cws_mime_t mime);
void cws_response_body_owned(cws_response_t* res, void* body, size_t len, cws_mime_t mime);
int  cws_response_send(cws_response_t* res);
int  cws_response_sendfile(cws_response_t* res, const char* path, cws_mime_t mime);
int  cws_response_send_error(cws_response_t* res, int status);

const char* cws_mime_string(cws_mime_t mt);
const char* cws_status_string(int status);

#endif
