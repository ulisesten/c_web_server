#define _GNU_SOURCE

#include "cws/app.h"
#include "cws/errors.h"
#include "cws/log.h"
#include "cws/cpu.h"
#include "cws/env.h"
#include "cws/middleware.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <stdio.h>
#include <limits.h>
#include <sys/stat.h>

typedef struct cws_static_map {
    char*       fs_path;
    cws_mime_t  mime;
    struct cws_static_map* next;
} cws_static_map_t;

struct cws_app {
    cws_config_t       cfg;
    cws_cpu_topology_t topo;
    cws_router_t*      router;
    cws_server_t*      srv;
    cws_env_t*         env;
    cws_static_map_t*  statics;
    int                topology_detected;
    int                log_inited;
    cws_ready_cb       on_ready;
    void*              on_ready_user;
    int                running;
};

cws_app_t* cws_app_new(void) {
    cws_app_t* app = calloc(1, sizeof(*app));
    if (!app) return NULL;
    app->cfg = cws_config_default();
    app->router = cws_router_new();
    app->env = cws_env_new();
    if (!app->router || !app->env) {
        cws_router_free(app->router);
        cws_env_free(app->env);
        free(app);
        return NULL;
    }
    return app;
}

void cws_app_free(cws_app_t* app) {
    if (!app) return;
    if (app->srv)     cws_server_free(app->srv);
    if (app->router)  cws_router_free(app->router);
    if (app->env)     cws_env_free(app->env);
    if (app->topology_detected) cws_cpu_topology_free(&app->topo);
    if (app->log_inited)        cws_log_shutdown();
    free(app);
}

cws_app_t* cws_app_port(cws_app_t* app, int port) {
    if (app && port > 0 && port < 65536) app->cfg.port = port;
    return app;
}

cws_app_t* cws_app_bind(cws_app_t* app, const char* addr) {
    if (app && addr) {
        snprintf(app->cfg.bind_addr, sizeof(app->cfg.bind_addr), "%s", addr);
    }
    return app;
}

cws_app_t* cws_app_workers(cws_app_t* app, int n) {
    if (app) app->cfg.n_workers = n < 0 ? 0 : n;
    return app;
}

cws_app_t* cws_app_pin(cws_app_t* app, int enabled) {
    if (app) app->cfg.pin_workers = enabled ? 1 : 0;
    return app;
}

cws_app_t* cws_app_reuse_port(cws_app_t* app, int enabled) {
    if (app) app->cfg.reuse_port = enabled ? 1 : 0;
    return app;
}

cws_app_t* cws_app_log_level(cws_app_t* app, int level) {
    if (app) app->cfg.log_level = level;
    return app;
}

cws_app_t* cws_app_static_dir(cws_app_t* app, const char* dir) {
    if (app && dir) {
        snprintf(app->cfg.static_dir, sizeof(app->cfg.static_dir), "%s", dir);
    }
    return app;
}

cws_app_t* cws_app_max_body(cws_app_t* app, size_t bytes) {
    if (app) app->cfg.max_body_size = bytes;
    return app;
}

cws_app_t* cws_app_keepalive_ms(cws_app_t* app, uint32_t ms) {
    if (app) app->cfg.keepalive_idle_ms = ms;
    return app;
}

cws_app_t* cws_app_backlog(cws_app_t* app, int n) {
    if (app && n > 0) app->cfg.listen_backlog = n;
    return app;
}

cws_app_t* cws_app_tls(cws_app_t* app, const char* cert, const char* key) {
    if (!app) return NULL;
    if (cert) snprintf(app->cfg.tls_cert, sizeof(app->cfg.tls_cert), "%s", cert);
    if (key)  snprintf(app->cfg.tls_key,  sizeof(app->cfg.tls_key),  "%s", key);
    return app;
}

int cws_app_env_file(cws_app_t* app, const char* path) {
    if (!app || !path || !app->env) return CWS_ERR_INVALID;
    return cws_env_load_file(app->env, path);
}

const char* cws_app_env_get(const cws_app_t* app, const char* key) {
    return app ? cws_env_get(app->env, key) : NULL;
}

const char* cws_app_env_get_or(const cws_app_t* app, const char* key,
                              const char* default_value) {
    if (!app) return default_value;
    return cws_env_get_or(app->env, key, default_value);
}

cws_app_t* cws_app_use(cws_app_t* app, cws_middleware_fn mw) {
    if (app && mw) cws_router_use(app->router, mw);
    return app;
}

cws_app_t* cws_app_mount(cws_app_t* app, const char* prefix,
                        cws_router_t* sub_router) {
    if (app && prefix && sub_router) {
        cws_router_mount(app->router, prefix, sub_router);
    }
    return app;
}

const cws_config_t*  cws_app_config(const cws_app_t* app)  { return app ? &app->cfg  : NULL; }
cws_router_t*        cws_app_router(const cws_app_t* app) { return app ? app->router : NULL; }
cws_server_t*        cws_app_server(const cws_app_t* app) { return app ? app->srv   : NULL; }
const cws_metrics_t* cws_app_metrics(const cws_app_t* app){
    return (app && app->srv) ? cws_server_metrics(app->srv) : NULL;
}

cws_app_t* cws_app_route(cws_app_t* app, cws_method_t method,
                         const char* pattern, cws_handler_t handler) {
    if (app && pattern && handler) {
        cws_router_add(app->router, method, pattern, handler);
    }
    return app;
}

typedef struct cws_static_binding {
    char*       url_path;
    char*       fs_path;
    cws_mime_t  mime;
} cws_static_binding_t;

static cws_static_binding_t* g_static_bindings = NULL;
static size_t                g_static_count     = 0;
static size_t                g_static_cap       = 0;

static void static_serve_handler(cws_request_t* req, cws_response_t* res) {
    for (size_t i = 0; i < g_static_count; i++) {
        if (strcmp(g_static_bindings[i].url_path, req->path) == 0) {
            cws_response_sendfile(res, g_static_bindings[i].fs_path,
                                  g_static_bindings[i].mime);
            return;
        }
    }
    cws_response_send_error(res, 404);
}

cws_app_t* cws_app_static(cws_app_t* app, const char* url_path,
                          const char* fs_path, cws_mime_t mime) {
    if (!app || !url_path || !fs_path) return app;
    if (g_static_count == g_static_cap) {
        size_t ncap = g_static_cap ? g_static_cap * 2 : 8;
        cws_static_binding_t* arr = realloc(g_static_bindings,
                                           ncap * sizeof(*arr));
        if (!arr) return app;
        g_static_bindings = arr;
        g_static_cap = ncap;
    }
    cws_static_binding_t* b = &g_static_bindings[g_static_count++];
    b->url_path = strdup(url_path);
    b->fs_path  = strdup(fs_path);
    b->mime     = mime;
    return cws_app_route(app, CWS_M_GET, url_path, static_serve_handler);
}

/* ------------------------------------------------------------------------- */
/* Servidor estático de directorio (express.static-like)                      */
/* ------------------------------------------------------------------------- */

typedef struct cws_static_mount {
    char*  prefix;
    size_t prefix_len;
    char*  dir;
    char*  index;
    void (*set_headers)(cws_response_t* res, const char* file_path, void* user);
    void*  user;
} cws_static_mount_t;

static cws_static_mount_t* g_static_mounts  = NULL;
static size_t              g_mount_count    = 0;
static size_t              g_mount_cap      = 0;

static char* strip_trailing_slash(const char* s) {
    size_t n = strlen(s);
    while (n > 1 && s[n - 1] == '/') n--;
    char* out = (char*)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

/* Mime por defecto según extensión (usado si set_headers no define
 * Content-Type). */
static const char* static_mime_type(const char* p) {
    const char* dot = strrchr(p, '.');
    if (!dot) return "application/octet-stream";
    if (!strcasecmp(dot, ".m3u8")) return "application/vnd.apple.mpegurl";
    if (!strcasecmp(dot, ".ts")) return "video/mp2t";
    if (!strcasecmp(dot, ".mp4")) return "video/mp4";
    if (!strcasecmp(dot, ".html") || !strcasecmp(dot, ".htm")) return "text/html; charset=utf-8";
    if (!strcasecmp(dot, ".css")) return "text/css; charset=utf-8";
    if (!strcasecmp(dot, ".js")) return "application/javascript; charset=utf-8";
    if (!strcasecmp(dot, ".json")) return "application/json; charset=utf-8";
    if (!strcasecmp(dot, ".png")) return "image/png";
    if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(dot, ".svg")) return "image/svg+xml";
    if (!strcasecmp(dot, ".txt")) return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

/* Une dir + ruta URL relativa (de longitud rel_len, no NUL-terminada)
 * evitando escapes de directorio ("..") y dotfiles. */
static int static_join(const char* dir, const char* rel, size_t rel_len,
                       char* out, size_t outsz) {
    if (snprintf(out, outsz, "%s", dir) >= (int)outsz) return CWS_ERR_OVERFLOW;
    size_t p = 0;
    while (p < rel_len) {
        while (p < rel_len && rel[p] == '/') p++;
        if (p >= rel_len) break;
        const char* seg = rel + p;
        while (p < rel_len && rel[p] != '/') p++;
        size_t slen = (size_t)(rel + p - seg);
        /* Bloquea "..", "." y cualquier dotfile ("." inicial). */
        if (seg[0] == '.') return CWS_ERR_INVALID;
        size_t cur = strlen(out);
        if (cur + 1 + slen + 1 >= outsz) return CWS_ERR_OVERFLOW;
        out[cur++] = '/';
        memcpy(out + cur, seg, slen);
        out[cur + slen] = '\0';
    }
    return CWS_OK;
}

/* ¿Está `sub` dentro de `root` (o es el propio root) sin salirse? */
static int path_is_within(const char* sub, const char* root) {
    size_t rl = strlen(root);
    if (strncmp(sub, root, rl) != 0) return 0;
    if (sub[rl] == '\0') return 1;
    if (rl > 0 && root[rl - 1] == '/') return 1;
    return sub[rl] == '/';
}

/* Resuelve `path` a su camino canónico (realpath, sigue symlinks) y verifica
 * que quede dentro de la raíz `dir`. En éxito llena `out` con el camino
 * canónico y devuelve 0; si escapa, no se puede resolver o es demasiado
 * largo devuelve -1. */
static int static_resolve(const char* dir, const char* path, char* out,
                          size_t outsz) {
    char root[PATH_MAX];
    char resolved[PATH_MAX];
    if (!realpath(dir, root)) return -1;
    if (!realpath(path, resolved)) return -1;
    if (!path_is_within(resolved, root)) return -1;
    if (strlen(resolved) >= outsz) return -1;
    strcpy(out, resolved);
    return 0;
}

static void static_mount_handler(cws_request_t* req, cws_response_t* res) {
    for (size_t i = 0; i < g_mount_count; i++) {
        cws_static_mount_t* m = &g_static_mounts[i];
        if (req->path_len < m->prefix_len) continue;
        if (strncmp(req->path, m->prefix, m->prefix_len) != 0) continue;
        size_t rem = req->path_len - m->prefix_len;
        const char* after = req->path + m->prefix_len;
        /* límite de segmentos: /hls/videos no debe matchear /hls/videosxyz */
        if (rem > 0 && after[0] != '/') continue;
        if (rem > 0 && after[0] == '/') { after++; rem--; }

        char path[PATH_MAX];
        if (static_join(m->dir, after, rem, path, sizeof(path)) != CWS_OK) {
            cws_response_send_error(res, 404);
            return;
        }

        /* Resolución canónica + contención (anti symlink-escape). */
        char resolved[PATH_MAX];
        if (static_resolve(m->dir, path, resolved, sizeof(resolved)) != 0) {
            cws_response_send_error(res, 404);
            return;
        }

        struct stat st;
        if (stat(resolved, &st) != 0) {
            cws_response_send_error(res, 404);
            return;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!m->index) {
                cws_response_send_error(res, 404);
                return;
            }
            size_t len = strlen(resolved);
            int nn = snprintf(resolved + len, sizeof(resolved) - len, "/%s",
                              m->index);
            if (nn < 0 || (size_t)nn >= sizeof(resolved) - len) {
                cws_response_send_error(res, 404);
                return;
            }
            /* Re-verificar contención para el índice (podría ser symlink). */
            char index_resolved[PATH_MAX];
            if (static_resolve(m->dir, resolved, index_resolved,
                               sizeof(index_resolved)) != 0 ||
                stat(index_resolved, &st) != 0) {
                cws_response_send_error(res, 404);
                return;
            }
            strcpy(resolved, index_resolved);
        }
        if (!S_ISREG(st.st_mode)) {
            cws_response_send_error(res, 404);
            return;
        }
        size_t size = (size_t)st.st_size;

        if (m->set_headers) m->set_headers(res, resolved, m->user);
        cws_response_status(res, 200);
        cws_response_sendfile_ex(res, resolved, static_mime_type(resolved), size);
        return;
    }
    cws_response_send_error(res, 404);
}

cws_app_t* cws_app_static_mount(cws_app_t* app, const cws_static_options_t* opts) {
    if (!app || !opts || !opts->prefix || !opts->dir) return app;
    char* prefix = strip_trailing_slash(opts->prefix);
    if (!prefix || !*prefix) {
        free(prefix);
        return app;
    }
    if (g_mount_count == g_mount_cap) {
        size_t ncap = g_mount_cap ? g_mount_cap * 2 : 8;
        cws_static_mount_t* arr = realloc(g_static_mounts, ncap * sizeof(*arr));
        if (!arr) {
            free(prefix);
            return app;
        }
        g_static_mounts = arr;
        g_mount_cap = ncap;
    }
    cws_static_mount_t* m = &g_static_mounts[g_mount_count++];
    memset(m, 0, sizeof(*m));
    m->prefix = prefix;
    m->prefix_len = strlen(prefix);
    m->dir = strdup(opts->dir);
    m->index = opts->index ? strdup(opts->index) : strdup("index.html");
    m->set_headers = opts->set_headers;
    m->user = opts->user;
    if (!m->dir || !m->index) {
        free(m->prefix); free(m->dir); free(m->index);
        m->dir = m->index = m->prefix = NULL;
        g_mount_count--;
        return app;
    }

    /* Rutas GET para la raíz del prefijo y para todo lo que cuelga de él. */
    size_t wcap = strlen(prefix) + 3;
    char* wild = (char*)malloc(wcap);
    if (wild) {
        snprintf(wild, wcap, "%s/*", prefix);
        cws_app_route(app, CWS_M_GET, prefix, static_mount_handler);
        cws_app_route(app, CWS_M_GET, wild, static_mount_handler);
        free(wild);
    }
    return app;
}

void cws_app_on_ready(cws_app_t* app, cws_ready_cb cb, void* user) {
    if (!app) return;
    app->on_ready = cb;
    app->on_ready_user = user;
}

static void app_default_ready(const cws_server_t* srv, int err, void* user) {
    (void)user;
    if (err != CWS_OK) {
        fprintf(stderr, "cws_app: failed to start: %s\n", cws_strerror(err));
        return;
    }
    const cws_config_t* cfg = cws_server_config(srv);
    cws_log_info("cws_app listening on %s:%d  workers=%d",
                cfg->bind_addr, cfg->port, cfg->n_workers);
}

static cws_app_t* g_app_for_signal = NULL;
static void app_signal_handler(int sig) {
    (void)sig;
    if (g_app_for_signal) cws_app_stop(g_app_for_signal);
}

static int app_bootstrap(cws_app_t* app) {
    if (!app || app->running) return CWS_ERR_INVALID;

    if (!app->topology_detected) {
        if (cws_cpu_topology_detect(&app->topo) != CWS_OK) {
            cws_log_warn("CPU topology detection failed; using defaults");
            memset(&app->topo, 0, sizeof(app->topo));
            app->topo.n_logical = (int)sysconf(_SC_NPROCESSORS_ONLN);
        }
        app->topology_detected = 1;
    }

    if (cws_config_validate(&app->cfg, &app->topo) != CWS_OK) {
        return CWS_ERR_INVALID;
    }

    if (!app->log_inited) {
        cws_log_init((cws_log_level_t)app->cfg.log_level, cws_log_default_sink, NULL);
        app->log_inited = 1;
    }
    return CWS_OK;
}

int cws_app_run(cws_app_t* app) {
    int rc = app_bootstrap(app);
    if (rc != CWS_OK) return rc;

    app->srv = cws_server_new(&app->cfg);
    if (!app->srv) return CWS_ERR_NOMEM;
    cws_server_set_router(app->srv, app->router);

    signal(SIGPIPE, SIG_IGN);
    g_app_for_signal = app;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = app_signal_handler;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    cws_ready_cb cb = app->on_ready ? app->on_ready : app_default_ready;
    app->running = 1;
    rc = cws_server_run(app->srv, cb, app->on_ready_user);
    app->running = 0;
    return rc;
}

int cws_app_run_async(cws_app_t* app) {
    int rc = app_bootstrap(app);
    if (rc != CWS_OK) return rc;

    app->srv = cws_server_new(&app->cfg);
    if (!app->srv) return CWS_ERR_NOMEM;
    cws_server_set_router(app->srv, app->router);

    cws_ready_cb cb = app->on_ready ? app->on_ready : app_default_ready;
    rc = cws_server_run_async(app->srv, cb, app->on_ready_user);
    if (rc == CWS_OK) app->running = 1;
    return rc;
}

int cws_app_wait(cws_app_t* app) {
    if (!app || !app->srv) return CWS_ERR_INVALID;
    int rc = cws_server_wait(app->srv);
    app->running = 0;
    return rc;
}

int cws_app_stop(cws_app_t* app) {
    if (!app || !app->srv) return CWS_ERR_INVALID;
    return cws_server_stop(app->srv);
}
