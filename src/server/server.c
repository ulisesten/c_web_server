#define _GNU_SOURCE

#include "cws/server.h"
#include "cws/errors.h"
#include "cws/log.h"
#include "cws/config.h"
#include "cws/route.h"
#include "cws/middleware.h"
#include "cws/parser.h"
#include "cws/response.h"
#include "cws/metrics.h"
#include "cws/cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define CWS_EPOLL_EVENTS 64
#define CWS_RECV_BUF     8192

/* Tope de conexiones por worker: el pool se indexa por fd y cada entrada
 * lleva ~8 KB de buffer de recepción. Sin tope, OPEN_MAX (~1M) haría reservar
 * varios GB virtuales por worker. Al superar el tope la conexión se cierra. */
#define CWS_MAX_CONNS_PER_WORKER 4096

typedef struct cws_conn {
    int           fd;
    cws_parser_t   parser;
    char          recv_buf[CWS_RECV_BUF];
    size_t        recv_used;
    uint64_t      last_active_ms;
    int           worker_idx;
} cws_conn_t;

typedef struct cws_worker {
    int            idx;
    pthread_t      thread;
    int            epoll_fd;
    int            accept_sock;     /* duplicated listen socket for SO_REUSEPORT */
    cws_conn_t*    conns;          /* pool indexed by fd slot */
    int            max_conns;
    cws_server_t*  srv;
} cws_worker_t;

struct cws_server {
    cws_config_t        cfg;
    cws_cpu_topology_t  topo;
    cws_router_t*       router;
    cws_metrics_t       metrics;
    int                 listen_fd;
    int                 n_workers;
    int*                placement;
    cws_worker_t*       workers;
    _Atomic int         running;
    _Atomic int         stop;
    pthread_t           acceptor_thread;
    int                 acceptor_started;
    cws_ready_cb        on_ready;
    void*               ready_user;
};

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static int make_listen_socket(const cws_config_t* cfg) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { cws_log_error("socket: %s", strerror(errno)); return -1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (cfg->reuse_port)
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    if (cfg->tcp_nodelay) {
        int on = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)cfg->port);
    if (inet_pton(AF_INET, cfg->bind_addr, &addr.sin_addr) != 1) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        cws_log_error("bind %s:%d: %s", cfg->bind_addr, cfg->port, strerror(errno));
        close(fd); return -1;
    }
    if (listen(fd, cfg->listen_backlog) < 0) {
        cws_log_error("listen: %s", strerror(errno));
        close(fd); return -1;
    }
    return fd;
}

static void apply_conn_sockopts(int fd, const cws_config_t* cfg) {
    if (cfg->tcp_nodelay)  { int on=1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)); }
    if (cfg->tcp_quickack) { int on=1; setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &on, sizeof(on)); }
    if (cfg->tcp_cork)     { int on=1; setsockopt(fd, IPPROTO_TCP, TCP_CORK, &on, sizeof(on)); }
    struct timeval tv;
    tv.tv_sec = cfg->recv_timeout_ms / 1000;
    tv.tv_usec = (cfg->recv_timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    tv.tv_sec = cfg->send_timeout_ms / 1000;
    tv.tv_usec = (cfg->send_timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static int setup_conn(cws_worker_t* w, int fd) {
    if (fd >= w->max_conns) {
        cws_log_error("fd %d exceeds pool capacity %d", fd, w->max_conns);
        return CWS_ERR_OVERFLOW;
    }
    cws_conn_t* c = &w->conns[fd];
    memset(c, 0, sizeof(*c));
    c->fd = fd;
    c->worker_idx = w->idx;
    c->last_active_ms = now_ms();
    cws_parser_init(&c->parser);

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
    ev.data.fd = fd;
    if (epoll_ctl(w->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        cws_log_error("epoll_ctl ADD fd=%d: %s", fd, strerror(errno));
        return CWS_ERR_IO;
    }
    cws_metrics_inc_conn_accepted(&w->srv->metrics);
    return CWS_OK;
}

static int rearm(cws_worker_t* w, cws_conn_t* c);
static int handle_recv(cws_worker_t* w, cws_conn_t* c) {
    cws_server_t* srv = w->srv;
    while (1) {
        ssize_t r = recv(c->fd, c->recv_buf + c->recv_used,
                         sizeof(c->recv_buf) - c->recv_used - 1, 0);
        if (r > 0) {
            c->recv_used += (size_t)r;
            c->last_active_ms = now_ms();
            cws_metrics_add_bytes_read(&srv->metrics, (size_t)r);

            int prc = cws_parser_feed(&c->parser, c->recv_buf, c->recv_used,
                                      srv->cfg.max_header_size,
                                      srv->cfg.max_body_size);
            if (prc != CWS_OK) {
                cws_metrics_inc_error(&srv->metrics);
                cws_response_t res; cws_response_init(&res, c->fd, 0);
                cws_response_send_error(&res, 400);
                close(c->fd);
                return -1;
            }
            if (cws_parser_state(&c->parser) == CWS_PARSER_DONE) {
                const cws_request_t* preq = cws_parser_request(&c->parser);
                if (!preq) {
                    cws_metrics_inc_error(&srv->metrics);
                    close(c->fd); return -1;
                }
                cws_request_t req = *preq;
                req.path_params_count = 0;
                req.worker_idx = w->idx;

                char path_nul[1024];
                size_t pl = req.path_len < sizeof(path_nul)-1 ? req.path_len : sizeof(path_nul)-1;
                memcpy(path_nul, req.path, pl); path_nul[pl] = '\0';

                cws_query_kv_t params[16];
                size_t params_count = 0;
                cws_pipeline_t pipe;
                int mrc = cws_router_match_pipeline(srv->router, req.method,
                                                    path_nul, pl,
                                                    params, 16, &params_count,
                                                    &pipe);
                if (mrc == CWS_OK && pipe.handler) {
                    for (size_t i = 0; i < params_count && i < 16; i++) {
                        req.path_params[i] = params[i];
                    }
                    req.path_params_count = params_count;
                    cws_metrics_inc_requests(&srv->metrics, req.keep_alive);
                    cws_response_t res;
                    cws_response_init(&res, c->fd, req.keep_alive);
                    cws_pipeline_run(&pipe, &req, &res);
                    int sc = res.status / 100;
                    if (sc >= 2 && sc <= 5) {
                        switch (sc) {
                            case 2: cws_metrics_inc_status(&srv->metrics, 2); break;
                            case 3: cws_metrics_inc_status(&srv->metrics, 3); break;
                            case 4: cws_metrics_inc_status(&srv->metrics, 4); break;
                            case 5: cws_metrics_inc_status(&srv->metrics, 5); break;
                            default: break;
                        }
                    }
                    cws_metrics_add_bytes_written(&srv->metrics, res.header_len + res.body_len);
                    if (!res.keep_alive) { close(c->fd); return 0; }
                    cws_metrics_inc_conn_reused(&srv->metrics);
                    cws_parser_reset(&c->parser);
                    c->recv_used = 0;
                } else {
                    cws_response_t res; cws_response_init(&res, c->fd, req.keep_alive);
                    cws_response_send_error(&res, 404);
                    cws_metrics_inc_status(&srv->metrics, 4);
                    close(c->fd);
                    return 0;
                }
            }
            if ((size_t)r < sizeof(c->recv_buf) - c->recv_used - 1) {
                return rearm(w, c);
            }
            continue;
        }
        if (r == 0) {
            close(c->fd);
            return 0;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return rearm(w, c);
        }
        if (errno == EINTR) continue;
        cws_log_warn("recv fd=%d: %s", c->fd, strerror(errno));
        close(c->fd);
        return -1;
    }
}

static int rearm(cws_worker_t* w, cws_conn_t* c) {
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
    ev.data.fd = c->fd;
    if (epoll_ctl(w->epoll_fd, EPOLL_CTL_MOD, c->fd, &ev) < 0) {
        cws_log_warn("epoll_ctl MOD fd=%d: %s", c->fd, strerror(errno));
        close(c->fd);
        return -1;
    }
    return 0;
}

static void* worker_main(void* arg) {
    cws_worker_t* w = arg;
    cws_server_t* srv = w->srv;

    if (srv->cfg.pin_workers && srv->placement) {
        int cpu = srv->placement[w->idx];
        if (cws_cpu_pin(cpu) == 0) {
            cws_log_info("worker %d pinned to CPU %d", w->idx, cpu);
        } else {
            cws_log_warn("worker %d: pinning to CPU %d failed", w->idx, cpu);
        }
    }

    struct epoll_event events[CWS_EPOLL_EVENTS];
    while (!atomic_load(&srv->stop)) {
        int n = epoll_wait(w->epoll_fd, events, CWS_EPOLL_EVENTS, 100);
        if (n < 0) {
            if (errno == EINTR) continue;
            cws_log_error("epoll_wait: %s", strerror(errno));
            break;
        }
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd >= w->max_conns) continue;
            cws_conn_t* c = &w->conns[fd];
            if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                epoll_ctl(w->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                close(fd);
                cws_metrics_inc_conn_closed(&srv->metrics);
                continue;
            }
            if (events[i].events & EPOLLIN) {
                if (handle_recv(w, c) < 0) {
                    epoll_ctl(w->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                    cws_metrics_inc_conn_closed(&srv->metrics);
                }
            }
        }
    }
    return NULL;
}

cws_server_t* cws_server_new(const cws_config_t* cfg) {
    if (!cfg) return NULL;
    cws_server_t* srv = calloc(1, sizeof(*srv));
    if (!srv) return NULL;
    srv->cfg = *cfg;
    if (cws_cpu_topology_detect(&srv->topo) != CWS_OK) {
        cws_log_warn("CPU topology detection failed; falling back to defaults");
        memset(&srv->topo, 0, sizeof(srv->topo));
        srv->topo.n_logical = (int)sysconf(_SC_NPROCESSORS_ONLN);
    }
    int rc = cws_config_validate(&srv->cfg, &srv->topo);
    if (rc != CWS_OK) {
        cws_log_error("config validation failed: %s", cws_strerror(rc));
        free(srv); return NULL;
    }
    atomic_store(&srv->running, 0);
    atomic_store(&srv->stop, 0);
    cws_metrics_init(&srv->metrics);
    return srv;
}

void cws_server_free(cws_server_t* srv) {
    if (!srv) return;
    if (srv->workers) {
        for (int i = 0; i < srv->n_workers; i++) {
            if (srv->workers[i].conns) free(srv->workers[i].conns);
            if (srv->workers[i].epoll_fd >= 0) close(srv->workers[i].epoll_fd);
        }
        free(srv->workers);
    }
    if (srv->placement) cws_cpu_placement_free(srv->placement);
    if (srv->listen_fd >= 0) close(srv->listen_fd);
    cws_cpu_topology_free(&srv->topo);
    /* Router is owned by the caller, not by the server. */
    free(srv);
}

void     cws_server_set_router(cws_server_t* srv, cws_router_t* router) { if (srv) srv->router = router; }
cws_router_t* cws_server_router(const cws_server_t* srv) { return srv ? srv->router : NULL; }
const cws_metrics_t* cws_server_metrics(const cws_server_t* srv) { return srv ? &srv->metrics : NULL; }
const cws_config_t*  cws_server_config(const cws_server_t* srv)  { return srv ? &srv->cfg  : NULL; }

static int dummy_ready_called = 0;
static void dummy_ready(const cws_server_t* srv, int err, void* user) {
    (void)srv; (void)user;
    dummy_ready_called = err;
}
static void* acceptor_loop(void* arg) {
    cws_server_t* srv = arg;
    static _Atomic int rr = 0;
    while (!atomic_load(&srv->stop)) {
        int cfd = accept4(srv->listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct timespec ts = { 0, 1000 * 1000 };
                nanosleep(&ts, NULL);
                continue;
            }
            cws_log_error("accept4: %s", strerror(errno));
            continue;
        }
        apply_conn_sockopts(cfd, &srv->cfg);

        int wi = atomic_fetch_add(&rr, 1) % srv->n_workers;
        cws_worker_t* w = &srv->workers[wi];
        if (setup_conn(w, cfd) != CWS_OK) { close(cfd); continue; }
    }
    return NULL;
}

static int server_start(cws_server_t* srv, cws_ready_cb on_ready, void* user) {
    if (!srv || srv->cfg.n_workers < 1) return CWS_ERR_INVALID;
    if (!on_ready) on_ready = dummy_ready;
    srv->on_ready = on_ready;
    srv->ready_user = user;

    srv->n_workers = srv->cfg.n_workers;
    if (srv->cfg.pin_workers) {
        srv->placement = cws_cpu_placement(&srv->topo, srv->n_workers, &srv->n_workers);
        if (!srv->placement) {
            cws_log_warn("CPU placement failed; workers will not be pinned");
        }
    }

    srv->listen_fd = make_listen_socket(&srv->cfg);
    if (srv->listen_fd < 0) {
        on_ready(srv, CWS_ERR_SOCKET, user);
        return CWS_ERR_SOCKET;
    }

    int pool_cap = CWS_MAX_CONNS_PER_WORKER;
    long sc = sysconf(_SC_OPEN_MAX);
    if (sc > 0 && sc < pool_cap) pool_cap = (int)sc;
    if (pool_cap < 1024) pool_cap = 1024;

    srv->workers = calloc((size_t)srv->n_workers, sizeof(cws_worker_t));
    if (!srv->workers) { on_ready(srv, CWS_ERR_NOMEM, user); return CWS_ERR_NOMEM; }

    for (int i = 0; i < srv->n_workers; i++) {
        cws_worker_t* w = &srv->workers[i];
        w->idx  = i;
        w->srv  = srv;
        w->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
        w->max_conns = pool_cap;
        w->conns = calloc((size_t)pool_cap, sizeof(cws_conn_t));
        if (!w->conns || w->epoll_fd < 0) {
            on_ready(srv, CWS_ERR_NOMEM, user);
            return CWS_ERR_NOMEM;
        }
        w->accept_sock = -1;
    }

    on_ready(srv, CWS_OK, user);
    atomic_store(&srv->running, 1);

    for (int i = 0; i < srv->n_workers; i++) {
        pthread_create(&srv->workers[i].thread, NULL, worker_main, &srv->workers[i]);
    }
    return CWS_OK;
}

static int server_drain_and_join(cws_server_t* srv) {
    if (srv->acceptor_started) {
        pthread_join(srv->acceptor_thread, NULL);
        srv->acceptor_started = 0;
    }
    for (int i = 0; i < srv->n_workers; i++) {
        if (srv->workers && srv->workers[i].conns) {
            pthread_join(srv->workers[i].thread, NULL);
        }
    }
    atomic_store(&srv->running, 0);
    return CWS_OK;
}

int cws_server_run(cws_server_t* srv, cws_ready_cb on_ready, void* user) {
    int rc = server_start(srv, on_ready, user);
    if (rc != CWS_OK) return rc;

    acceptor_loop(srv);

    return server_drain_and_join(srv);
}

int cws_server_run_async(cws_server_t* srv, cws_ready_cb on_ready, void* user) {
    int rc = server_start(srv, on_ready, user);
    if (rc != CWS_OK) return rc;

    if (pthread_create(&srv->acceptor_thread, NULL, acceptor_loop, srv) != 0) {
        cws_log_error("pthread_create acceptor: %s", strerror(errno));
        atomic_store(&srv->stop, 1);
        server_drain_and_join(srv);
        return CWS_ERR_GENERIC;
    }
    srv->acceptor_started = 1;
    return CWS_OK;
}

int cws_server_wait(cws_server_t* srv) {
    if (!srv) return CWS_ERR_INVALID;
    return server_drain_and_join(srv);
}

int cws_server_stop(cws_server_t* srv) {
    if (!srv) return CWS_ERR_INVALID;
    atomic_store(&srv->stop, 1);
    return CWS_OK;
}