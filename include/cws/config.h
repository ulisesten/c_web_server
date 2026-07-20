#ifndef CWS_CONFIG_H
#define CWS_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "cpu.h"

typedef struct {
    /* Networking */
    int       port;
    char      bind_addr[64];    /* "0.0.0.0", "127.0.0.1", IPv4/IPv6                  */
    int       listen_backlog;   /* SOMAXCONN recommended                              */
    int       reuse_port;       /* SO_REUSEPORT: 1 = enable (Xeon/EPYC recommended)  */
    int       tcp_nodelay;      /* disable Nagle                                       */
    int       tcp_quickack;     /* TCP_QUICKACK on Linux                               */
    int       tcp_cork;         /* TCP_CORK during header+body send                   */
    int       tcp_fastopen;     /* TCP_FASTOPEN queue depth (0=disabled)              */

    /* Workers / concurrency */
    int       n_workers;        /* 0 = auto (recommended from topology)              */
    int       pin_workers;      /* 1 = pin each worker thread to a specific CPU      */

    /* Timeouts (ms) */
    uint32_t  recv_timeout_ms;
    uint32_t  send_timeout_ms;
    uint32_t  keepalive_idle_ms;  /* idle keep-alive before close                    */

    /* Limits */
    size_t    max_header_size;    /* per-request bytes (cota)                         */
    size_t    max_body_size;
    int       max_headers_count;
    int       max_conns_per_worker;

    /* Static files */
    char      static_dir[256];

    /* Logging */
    int       log_level;         /* cws_log_level_t cast                            */
    char      log_file[256];     /* empty -> stderr                                  */
} cws_config_t;

cws_config_t cws_config_default(void);

/*
 * Validate a config struct, applying topology-aware defaults when n_workers==0
 * or pin_workers requested. Returns CWS_OK or CWS_ERR_INVALID.
 */
int  cws_config_validate(cws_config_t* cfg, const cws_cpu_topology_t* topo);

#endif
