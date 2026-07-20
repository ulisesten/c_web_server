#ifndef CWS_ERRORS_H
#define CWS_ERRORS_H

typedef enum {
    CWS_OK            =  0,
    CWS_ERR_GENERIC   = -1,
    CWS_ERR_NOMEM     = -2,
    CWS_ERR_INVALID   = -3,
    CWS_ERR_IO        = -4,
    CWS_ERR_SOCKET    = -5,
    CWS_ERR_BIND      = -6,
    CWS_ERR_LISTEN    = -7,
    CWS_ERR_OVERFLOW  = -8,
    CWS_ERR_TIMEOUT   = -9,
    CWS_ERR_CLOSED    = -10,
    CWS_ERR_PROTOCOL  = -11,
    CWS_ERR_NOTFOUND  = -12,
} cws_err_t;

const char* cws_strerror(int err);

#endif
