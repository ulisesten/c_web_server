#define _GNU_SOURCE

#include "cws/response.h"
#include "cws/log.h"
#include "cws/errors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <sys/uio.h>

const char* cws_mime_string(cws_mime_t mt) {
    switch (mt) {
        case CWS_MT_TEXT_HTML:             return "text/html; charset=utf-8";
        case CWS_MT_TEXT_PLAIN:            return "text/plain; charset=utf-8";
        case CWS_MT_APPLICATION_JSON:      return "application/json; charset=utf-8";
        case CWS_MT_APPLICATION_OCTET_STREAM:
        case CWS_MT_APPLICATION_OCTET:     return "application/octet-stream";
        case CWS_MT_TEXT_CSS:              return "text/css; charset=utf-8";
        case CWS_MT_APPLICATION_JAVASCRIPT:return "application/javascript; charset=utf-8";
        case CWS_MT_IMAGE_PNG:             return "image/png";
        case CWS_MT_IMAGE_JPEG:            return "image/jpeg";
        case CWS_MT_IMAGE_SVG:             return "image/svg+xml";
        default:                           return "application/octet-stream";
    }
}

const char* cws_status_string(int status) {
    switch (status) {
        case 100: return "Continue";
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default:  return "Unknown";
    }
}

void cws_response_init(cws_response_t* res, int client_fd, int keep_alive) {
    memset(res, 0, sizeof(*res));
    res->client_fd   = client_fd;
    res->keep_alive  = keep_alive ? 1 : 0;
    res->status      = 200;
}

void cws_response_status(cws_response_t* res, int status) {
    if (status < 100 || status > 599) status = 500;
    res->status = status;
}

int cws_response_header(cws_response_t* res, const char* name, const char* value_fmt, ...) {
    if (res->header_len >= sizeof(res->header_buf)) return CWS_ERR_OVERFLOW;
    va_list ap;
    va_start(ap, value_fmt);

    char tmp[1024];
    int vlen = vsnprintf(tmp, sizeof(tmp), value_fmt, ap);
    va_end(ap);
    if (vlen < 0) return CWS_ERR_INVALID;

    int n = snprintf(res->header_buf + res->header_len,
                     sizeof(res->header_buf) - res->header_len,
                     "%s: %s\r\n", name, tmp);
    if (n < 0 || (size_t)n >= sizeof(res->header_buf) - res->header_len) {
        return CWS_ERR_OVERFLOW;
    }
    res->header_len += (size_t)n;
    return CWS_OK;
}

void cws_response_body(cws_response_t* res, const void* body, size_t len, cws_mime_t mime) {
    res->body_ptr  = body;
    res->body_len  = len;
    res->body_mime = mime;
    res->body_owned = 0;
}

void cws_response_body_owned(cws_response_t* res, void* body, size_t len, cws_mime_t mime) {
    res->body_ptr  = body;
    res->body_len  = len;
    res->body_mime = mime;
    res->body_owned = 1;
}

static int writev_all(int fd, struct iovec* iov, int iovcnt) {
    for (int i = 0; i < iovcnt; ) {
        if (iov[i].iov_len == 0) { i++; continue; }
        ssize_t w = writev(fd, &iov[i], iovcnt - i);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return CWS_ERR_TIMEOUT;
            return CWS_ERR_IO;
        }
        size_t wu = (size_t)w;
        while (wu > 0 && i < iovcnt) {
            if (iov[i].iov_len <= wu) {
                wu -= iov[i].iov_len;
                iov[i].iov_len = 0;
                i++;
            } else {
                iov[i].iov_base = (char*)iov[i].iov_base + wu;
                iov[i].iov_len -= wu;
                wu = 0;
            }
        }
    }
    return CWS_OK;
}

static int build_status_line(cws_response_t* res, size_t body_len,
                             const char* ct, int include_body) {
    char tmp[1024];
    int n;
    if (include_body) {
        if (ct)
            n = snprintf(tmp, sizeof(tmp),
                "HTTP/1.1 %d %s\r\n"
                "Content-Type: %s\r\n"
                "Content-Length: %zu\r\n"
                "Connection: %s\r\n",
                res->status,
                cws_status_string(res->status),
                ct,
                body_len,
                res->keep_alive ? "keep-alive" : "close");
        else
            n = snprintf(tmp, sizeof(tmp),
                "HTTP/1.1 %d %s\r\n"
                "Content-Length: %zu\r\n"
                "Connection: %s\r\n",
                res->status,
                cws_status_string(res->status),
                body_len,
                res->keep_alive ? "keep-alive" : "close");
    } else {
        n = snprintf(tmp, sizeof(tmp),
            "HTTP/1.1 %d %s\r\n"
            "Content-Length: 0\r\n"
            "Connection: %s\r\n",
            res->status,
            cws_status_string(res->status),
            res->keep_alive ? "keep-alive" : "close");
    }
    if (n < 0 || (size_t)n >= sizeof(tmp)) return CWS_ERR_OVERFLOW;
    size_t status_len = (size_t)n;

    /* Middleware headers are already in header_buf[0..header_len).
     * Shift them right to make room for the status line at the front. */
    if (res->header_len + status_len + 2 > sizeof(res->header_buf))
        return CWS_ERR_OVERFLOW;
    memmove(res->header_buf + status_len, res->header_buf, res->header_len);
    memcpy(res->header_buf, tmp, status_len);
    res->header_len += status_len;
    res->header_buf[res->header_len++] = '\r';
    res->header_buf[res->header_len++] = '\n';
    return CWS_OK;
}

/* ¿Ya hay un header con este nombre en header_buf? (case-insensitive). */
static int response_has_header(const cws_response_t* res, const char* name) {
    size_t nlen = strlen(name);
    for (size_t i = 0; i + nlen + 1 <= res->header_len; i++) {
        if (res->header_buf[i + nlen] == ':' &&
            strncasecmp(res->header_buf + i, name, nlen) == 0)
            return 1;
    }
    return 0;
}

int cws_response_send(cws_response_t* res) {
    if (res->sent) return CWS_ERR_INVALID;
    int rc;
    if (res->body_ptr && res->body_len > 0) {
        size_t prev = res->header_len;
        rc = build_status_line(res, res->body_len, cws_mime_string(res->body_mime), 1);
        if (rc != CWS_OK) {
            res->header_len = prev;
            return rc;
        }
        struct iovec iov[2] = {
            { .iov_base = res->header_buf, .iov_len = res->header_len },
            { .iov_base = (void*)res->body_ptr, .iov_len = res->body_len },
        };
        rc = writev_all(res->client_fd, iov, 2);
    } else {
        rc = build_status_line(res, 0, NULL, 0);
        if (rc != CWS_OK) return rc;
        struct iovec iov[1] = {
            { .iov_base = res->header_buf, .iov_len = res->header_len },
        };
        rc = writev_all(res->client_fd, iov, 1);
    }
    if (rc == CWS_OK) res->sent = 1;
    if (res->body_owned && res->body_ptr) {
        free((void*)res->body_ptr);
        res->body_ptr = NULL;
    }
    return rc;
}

int cws_response_sendfile(cws_response_t* res, const char* path, cws_mime_t mt) {
    return cws_response_sendfile_ex(res, path, cws_mime_string(mt), 0);
}

int cws_response_sendfile_ex(cws_response_t* res, const char* path,
                             const char* default_content_type,
                             size_t content_length) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        cws_response_status(res, 404);
        const char* body = "404 Not Found\n";
        cws_response_body(res, body, strlen(body), CWS_MT_TEXT_PLAIN);
        return cws_response_send(res);
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return cws_response_send_error(res, 404);
    }
    size_t body_len = content_length ? content_length : (size_t)st.st_size;

    /* Si el llamador ya agregó un Content-Type (p. ej. vía set_headers) lo
     * respetamos; si no, usamos el default. */
    const char* ct = response_has_header(res, "Content-Type")
                         ? NULL
                         : default_content_type;

    size_t prev = res->header_len;
    int rc = build_status_line(res, body_len, ct, 1);
    if (rc != CWS_OK) {
        res->header_len = prev;
        close(fd);
        return rc;
    }

    ssize_t hw = write(res->client_fd, res->header_buf, res->header_len);
    if (hw < 0 || (size_t)hw != res->header_len) {
        close(fd);
        return CWS_ERR_IO;
    }
    off_t off = 0;
    size_t left = body_len;
    while (left > 0) {
        size_t this_round = left;
        if (this_round > (size_t)(64 * 1024 * 1024)) this_round = 64 * 1024 * 1024;
        ssize_t s = sendfile(res->client_fd, fd, &off, this_round);
        if (s <= 0) {
            if (s < 0 && errno == EINTR) continue;
            close(fd);
            return CWS_ERR_IO;
        }
        left -= (size_t)s;
    }
    close(fd);
    res->sent = 1;
    return CWS_OK;
}

int cws_response_send_error(cws_response_t* res, int status) {
    char body[128];
    int n = snprintf(body, sizeof(body), "%d %s\n", status, cws_status_string(status));
    if (n < 0) n = 0;
    cws_response_status(res, status);
    cws_response_body(res, body, (size_t)n, CWS_MT_TEXT_PLAIN);
    return cws_response_send(res);
}
