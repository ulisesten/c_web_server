#ifndef CWS_PARSER_H
#define CWS_PARSER_H

#include <stddef.h>
#include <stdint.h>

#include "request.h"

/*
 * Incremental HTTP/1.1 request parser (deterministic state machine).
 *
 * Feed bytes as they arrive from recv(). The parser does NOT own the
 * input buffer; it references slices into a caller-provided arena.
 *
 * Lifecycle per connection:
 *   cws_parser_init(&p);
 *   for each recv chunk: cws_parser_feed(&p, buf, len);
 *   if cws_parser_state(&p) == CWS_PARSER_DONE: use cws_parser_request(&p)
 *   ... after dispatch & keep-alive:
 *   cws_parser_reset(&p);   // keep arena, clear state for next request
 */

typedef enum {
    CWS_PARSER_START = 0,    /* reading method   */
    CWS_PARSER_METHOD,       /* got method, reading path  */
    CWS_PARSER_PATH,
    CWS_PARSER_VERSION,
    CWS_PARSER_HEADER_NAME,
    CWS_PARSER_HEADER_VALUE,
    CWS_PARSER_R2,           /* after \r, expecting \n to end headers */
    CWS_PARSER_HEADERS_DONE, /* \r\n\r\n seen     */
    CWS_PARSER_BODY,
    CWS_PARSER_DONE,
    CWS_PARSER_ERROR,
} cws_parser_state_t;

typedef struct {
    cws_parser_state_t state;
    cws_request_t      request;
    int                error_code;       /* CWS_ERR_*            */
    size_t             header_bytes;
    size_t             body_bytes;
    size_t             body_target;     /* Content-Length; 0 if none */
    int                content_length_seen;
    int                chunked;          /* 1 if Transfer-Encoding: chunked */
    int                method_known;    /* CWS_METHOD_*         */
} cws_parser_t;

void cws_parser_init(cws_parser_t* p);
void cws_parser_reset(cws_parser_t* p);

/*
 * Feed a chunk of bytes. Returns:
 *   CWS_OK              if more data needed (state != DONE/ERROR)
 *   CWS_OK + state DONE if request fully parsed
 *   CWS_ERR_PROTOCOL    on parse error
 *   CWS_ERR_OVERFLOW    on size limit violation
 *
 * Callers must check cws_parser_state(&p) after each feed.
 */
int cws_parser_feed(cws_parser_t* p, const char* buf, size_t len,
                    size_t max_header_size, size_t max_body_size);

cws_parser_state_t cws_parser_state(const cws_parser_t* p);
const cws_request_t* cws_parser_request(const cws_parser_t* p);

#endif
