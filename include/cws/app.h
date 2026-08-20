#ifndef CWS_APP_H
#define CWS_APP_H

#include <stddef.h>
#include <stdint.h>

#include "server.h"
#include "route.h"
#include "config.h"
#include "request.h"
#include "response.h"
#include "middleware.h"
#include "env.h"

/*
 * High-level ergonomic API for the CWS framework.
 *
 * Typical usage:
 *
 *   #include "cws/cws.h"
 *
 *   CWS_HANDLER(hello) {
 *       cws_response_body(res, "hello\n", 6, CWS_MT_TEXT_PLAIN);
 *       cws_response_send(res);
 *   }
 *
 *   int main(void) {
 *       cws_app_t* app = cws_app_new();
 *       cws_app_port(app, 8080);
 *       cws_app_workers(app, 4);        // optional; defaults to physical cores
 *       cws_app_static_dir(app, "assets");
 *
 *       CWS_GET(app, "/",       hello);
 *       CWS_GET(app, "/healthz", health);
 *
 *       return cws_app_run(app);         // blocks until SIGINT/SIGTERM
 *   }
 *
 * The builder API is fluent:
 *   cws_app_t* app = cws_app_new()
 *       ->port(8080)
 *       ->bind("0.0.0.0")
 *       ->workers(0)
 *       ->pin(true)
 *       ->reuse_port(true)
 *       ->log_level(CWS_LOG_INFO)
 *       ->static_dir("assets");
 */

typedef struct cws_app cws_app_t;

/* Construction / teardown */
cws_app_t* cws_app_new(void);
void       cws_app_free(cws_app_t* app);

/* Builder setters (all return app for chaining). NULL/0 means default. */
cws_app_t* cws_app_port(cws_app_t* app, int port);
cws_app_t* cws_app_bind(cws_app_t* app, const char* addr);
cws_app_t* cws_app_workers(cws_app_t* app, int n);
cws_app_t* cws_app_pin(cws_app_t* app, int enabled);
cws_app_t* cws_app_reuse_port(cws_app_t* app, int enabled);
cws_app_t* cws_app_log_level(cws_app_t* app, int level);
cws_app_t* cws_app_static_dir(cws_app_t* app, const char* dir);
cws_app_t* cws_app_max_body(cws_app_t* app, size_t bytes);
cws_app_t* cws_app_keepalive_ms(cws_app_t* app, uint32_t ms);
cws_app_t* cws_app_backlog(cws_app_t* app, int n);
cws_app_t* cws_app_tls(cws_app_t* app, const char* cert, const char* key);

/*
 * Load a .env file (KEY=VALUE format). Multiple calls append/override.
 * Returns CWS_OK or CWS_ERR_IO.
 */
int  cws_app_env_file(cws_app_t* app, const char* path);

/* Get a .env variable loaded via cws_app_env_file. Returns NULL if missing. */
const char* cws_app_env_get(const cws_app_t* app, const char* key);
const char* cws_app_env_get_or(const cws_app_t* app, const char* key,
                              const char* default_value);

/*
 * Register a global middleware that runs before every route (Express-style
 * `app.use(mw)`). Returns app for chaining. Stable order = registration order.
 */
cws_app_t* cws_app_use(cws_app_t* app, cws_middleware_fn mw);

/*
 * Mount a sub-router under a prefix (Express-style `app.use('/videos', router)`).
 * The sub-router is owned by the caller; cws_app_free does NOT free it.
 */
cws_app_t* cws_app_mount(cws_app_t* app, const char* prefix,
                        cws_router_t* sub_router);

/* Access to underlying objects for advanced use */
const cws_config_t*  cws_app_config(const cws_app_t* app);
cws_router_t*        cws_app_router(const cws_app_t* app);
cws_server_t*        cws_app_server(const cws_app_t* app);
const cws_metrics_t* cws_app_metrics(const cws_app_t* app);

/*
 * Register handlers. Fluent: returns app.
 * Pattern language: /, /users/:id, /files/STAR, /api/:v/items/:id
 */
cws_app_t* cws_app_route(cws_app_t* app, cws_method_t method,
                         const char* pattern, cws_handler_t handler);

/*
 * Convenience: register a GET handler that serves a static file from
 * disk. The path resolution is performed at registration time; the
 * framework allocates and owns the binding context.
 */
cws_app_t* cws_app_static(cws_app_t* app, const char* url_path,
                          const char* fs_path, cws_mime_t mime);

/**
 * \brief Opciones para montar un servidor estático de directorio.
 *
 * Estilo express.static: sirve el árbol de archivos de \ref cws_static_options.dir
 * bajo el prefijo de URL \ref cws_static_options.prefix y permite configurar
 * headers por archivo vía el callback \ref cws_static_options.set_headers.
 */
typedef struct cws_static_options {
    const char* prefix;  /**< Prefijo de URL de montaje, p. ej. "/hls/videos".
                              Debe comenzar con '/'. */
    const char* dir;     /**< Raíz del filesystem que se sirve, p. ej.
                              "public/hls/videos". Los archivos se resuelven
                              concatenando este directorio con la ruta de la
                              petición. */
    const char* index;   /**< Archivo índice de directorio (default
                              "index.html"); NULL para no servir índices
                              (una petición a un directorio responde 404). */
    void (*set_headers)(cws_response_t* res, const char* file_path,
                        void* user); /**< Callback opcional que recibe la
                              respuesta y el path absoluto del archivo
                              resuelto, para agregar headers vía
                              cws_response_header() (p. ej. Content-Type y
                              Cache-Control según extensión). Puede ser NULL. */
    void* user;          /**< Contexto arbitrario que se pasa tal cual al
                              callback \ref cws_static_options.set_headers. */
} cws_static_options_t;

/**
 * \brief Monta un servidor estático de directorio configurable.
 *
 * Equivalente a:
 * \code
 *   app.use(prefix, express.static(dir, { setHeaders(res, path) {...} }))
 * \endcode
 *
 * Registra rutas GET para \p opts->prefix y para ${prefix}/STAR (sufijo
 * wildcard), sirviendo los archivos de \p opts->dir. Protección anti
 * traversal: no se resuelven segmentos "..". Si el archivo resuelto es un
 * directorio, se sirve \p opts->index cuando esté definido.
 *
 * \param[in] app  instancia de la aplicación cws.
 * \param[in] opts configuración del montaje: prefijo, directorio, índice y,
 *                 opcionalmente, un callback set_headers.
 * \return La propia instancia \p app para encadenar llamadas; si \p app o
 *         \p opts son NULL (o faltan prefix/dir) devuelve \p app sin cambios.
 */
cws_app_t* cws_app_static_mount(cws_app_t* app, const cws_static_options_t* opts);

/*
 * Blocks until SIGINT/SIGTERM (installs handlers automatically) or
 * cws_app_stop() is called from another thread. Returns the server exit
 * code (0 on success, <0 on fatal error).
 *
 * The first call also initializes logging, detects CPU topology, builds
 * the server, and starts the worker pool.
 *
 * Returns CWS_OK on graceful shutdown.
 */
int  cws_app_run(cws_app_t* app);

/*
 * Conversely, cws_app_run_async() starts the server in background and
 * returns immediately. Useful when embedding into an existing application
 * with its own event loop. The caller must call cws_app_wait() or
 * cws_app_stop() afterwards.
 */
int  cws_app_run_async(cws_app_t* app);
int  cws_app_wait(cws_app_t* app);    /* blocks until stop                 */
int  cws_app_stop(cws_app_t* app);   /* signal graceful shutdown          */

/*
 * Override ready callback. Optional.
 */
void cws_app_on_ready(cws_app_t* app, cws_ready_cb cb, void* user);

/* Handler signature sugar. Use in handler definitions:
 *   CWS_HANDLER(name) { ... }
 * expands to:  void name(cws_request_t* req, cws_response_t* res)
 */
#define CWS_HANDLER(name)  void name(cws_request_t* req, cws_response_t* res)

/* Route registration sugar macros. */
#define CWS_GET(app, path, handler)   cws_app_route(app, CWS_M_GET,    path, handler)
#define CWS_POST(app, path, handler)  cws_app_route(app, CWS_M_POST,   path, handler)
#define CWS_PUT(app, path, handler)    cws_app_route(app, CWS_M_PUT,    path, handler)
#define CWS_DELETE(app, path, handler) cws_app_route(app, CWS_M_DELETE, path, handler)
#define CWS_HEAD(app, path, handler)   cws_app_route(app, CWS_M_HEAD,   path, handler)
#define CWS_OPTIONS(app, path, handler) cws_app_route(app, CWS_M_OPTIONS, path, handler)
#define CWS_PATCH(app, path, handler)  cws_app_route(app, CWS_M_PATCH,  path, handler)

#endif
