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
    char      header_buf[8192];   /* suficientes para auth con cookies grandes */
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
/**
 * \brief Envía un archivo estático tal cual está en disco vía sendfile(2).
 *
 * Función de conveniencia que envía el archivo \p path con el Content-Type
 * derivado de \p mime y sin headers adicionales. Para headers personalizados
 * usá cws_response_sendfile_ex().
 *
 * \param[out] res  respuesta en construcción (se completa y envía al socket).
 * \param[in]  path ruta del archivo en filesystem.
 * \param[in]  mime Content-Type a anunciar en el encabezado.
 * \return CWS_OK en éxito; CWS_ERR_IO si falla la escritura; o se envía 404/500
 *         si el archivo no existe o no es un archivo regular.
 */
int  cws_response_sendfile(cws_response_t* res, const char* path, cws_mime_t mime);
int  cws_response_send_error(cws_response_t* res, int status);

/**
 * \brief Envía un archivo estático permitiendo headers personalizados.
 *
 * Equivalente a express.static con un callback setHeaders: los headers
 * (Content-Type, Cache-Control, etc.) que ya hayan sido agregados a \p res vía
 * cws_response_header() se respetan; si no se agregó un "Content-Type", se usa
 * \p default_content_type como fallback.
 *
 * Aguanta el status line, Content-Type (si aplica), Content-Length y
 * Connection, preservando los headers ya presentes en header_buf.
 *
 * \param[out] res                 respuesta en construcción (se completa y
 *                                 envía al socket).
 * \param[in]  path                ruta del archivo a servir.
 * \param[in]  default_content_type Content-Type por defecto que se usa
 *                                 ÚNICAMENTE si el llamador no agregó ya un
 *                                 header "Content-Type" (p. ej. vía un
 *                                 callback set_headers). Puede ser NULL si no
 *                                 se desea ningún default.
 * \param[in]  content_length      tamaño del archivo si ya se conoce (ahorra
 *                                 un fstat); pasar 0 para que se calcule
 *                                 internamente vía fstat().
 * \return CWS_OK en éxito; CWS_ERR_IO si falla la escritura; o se envía una
 *         respuesta 404 si el archivo no existe o no es un archivo regular.
 */
int  cws_response_sendfile_ex(cws_response_t* res, const char* path,
                              const char* default_content_type,
                              size_t content_length);

const char* cws_mime_string(cws_mime_t mt);
const char* cws_status_string(int status);

#endif
