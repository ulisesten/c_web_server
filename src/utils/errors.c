#include "cws/errors.h"

const char* cws_strerror(int err) {
    switch (err) {
        case CWS_OK:           return "success";
        case CWS_ERR_GENERIC:  return "generic error";
        case CWS_ERR_NOMEM:    return "out of memory";
        case CWS_ERR_INVALID:  return "invalid argument";
        case CWS_ERR_IO:       return "I/O error";
        case CWS_ERR_SOCKET:   return "socket error";
        case CWS_ERR_BIND:     return "bind error";
        case CWS_ERR_LISTEN:   return "listen error";
        case CWS_ERR_OVERFLOW: return "overflow";
        case CWS_ERR_TIMEOUT:  return "timeout";
        case CWS_ERR_CLOSED:   return "connection closed";
        case CWS_ERR_PROTOCOL: return "protocol error";
        case CWS_ERR_NOTFOUND: return "not found";
        default:               return "unknown error";
    }
}
