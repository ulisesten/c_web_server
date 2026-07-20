#define _GNU_SOURCE

#include "cws/config.h"
#include "cws/errors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

cws_config_t cws_config_default(void) {
    cws_config_t c;
    memset(&c, 0, sizeof(c));

    c.port             = 8080;
    snprintf(c.bind_addr, sizeof(c.bind_addr), "0.0.0.0");
    c.listen_backlog   = 4096;
    c.reuse_port       = 1;
    c.tcp_nodelay      = 1;
    c.tcp_quickack     = 1;
    c.tcp_cork         = 1;
    c.tcp_fastopen     = 16;

    c.n_workers        = 0;
    c.pin_workers      = 1;

    c.recv_timeout_ms      = 5000;
    c.send_timeout_ms      = 5000;
    c.keepalive_idle_ms    = 15000;

    c.max_header_size      = 8192;
    c.max_body_size        = 16 * 1024 * 1024;
    c.max_headers_count    = 64;
    c.max_conns_per_worker = 2048;

    snprintf(c.static_dir, sizeof(c.static_dir), "assets");

    c.log_level       = 2;
    c.log_file[0]     = '\0';

    return c;
}

int cws_config_validate(cws_config_t* cfg, const cws_cpu_topology_t* topo) {
    if (!cfg) return CWS_ERR_INVALID;

    if (cfg->port < 1 || cfg->port > 65535) return CWS_ERR_INVALID;
    if (cfg->listen_backlog < 1)  cfg->listen_backlog = 128;
    if (cfg->recv_timeout_ms == 0)    cfg->recv_timeout_ms = 5000;
    if (cfg->send_timeout_ms == 0)    cfg->send_timeout_ms = 5000;
    if (cfg->keepalive_idle_ms == 0)  cfg->keepalive_idle_ms = 15000;

    if (cfg->max_header_size < 256)   cfg->max_header_size = 256;
    if (cfg->max_body_size == 0)      cfg->max_body_size = 16 * 1024 * 1024;
    if (cfg->max_headers_count < 8)   cfg->max_headers_count = 8;
    if (cfg->max_conns_per_worker < 64) cfg->max_conns_per_worker = 64;

    if (cfg->n_workers < 0) cfg->n_workers = 0;
    if (cfg->n_workers == 0) {
        if (topo) cfg->n_workers = cws_cpu_recommended_workers(topo);
        else      cfg->n_workers = (int)sysconf(_SC_NPROCESSORS_ONLN);
        if (cfg->n_workers < 1) cfg->n_workers = 1;
    }

    if (cfg->pin_workers && topo) {
        int max_pinnable = topo->n_logical;
        if (cfg->n_workers > max_pinnable) return CWS_ERR_INVALID;
    }

    if (cfg->bind_addr[0] == '\0')
        snprintf(cfg->bind_addr, sizeof(cfg->bind_addr), "0.0.0.0");

    return CWS_OK;
}
