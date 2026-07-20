#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <stdint.h>
#include "cws/cpu.h"
#include "cws/errors.h"

static int read_int_file(const char* path, int* out) {
    FILE* f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    int n = 0;
    if (fscanf(f, "%d", &n) != 1) {
        fclose(f);
        return -1;
    }
    fclose(f);
    *out = n;
    return 0;
}

static int parse_size_str(const char* s) {
    if (!s || !*s) {
        return 0;
    }
    char* end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s) {
        return 0;
    }
    while (*end == ' ' || *end == '\t') {
        end++;
    }
    if (*end == 'K' || *end == 'k') {
        return (int)v;
    } else if (*end == 'M' || *end == 'm') {
        return (int)(v * 1024);
    }
    return (int)(v / 1024);
}

static int read_size_file(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    char buf[64];
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    return parse_size_str(buf);
}

static int read_str_file(const char* path, char* out, size_t outsz) {
    FILE* f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    if (!fgets(out, (int)outsz, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' ||
                      out[len - 1] == ' ' || out[len - 1] == '\t')) {
        out[--len] = '\0';
    }
    return 0;
}

static int cmp_cpu_socket_core(const void* a, const void* b) {
    const cws_cpu_info_t* x = (const cws_cpu_info_t*)a;
    const cws_cpu_info_t* y = (const cws_cpu_info_t*)b;
    if (x->socket_id != y->socket_id) {
        return x->socket_id - y->socket_id;
    }
    if (x->core_id != y->core_id) {
        return x->core_id - y->core_id;
    }
    return x->thread_id - y->thread_id;
}

static int dir_is_cpu(const struct dirent* d) {
    const char* p = d->d_name;
    if (strncmp(p, "cpu", 3) != 0) {
        return 0;
    }
    if (!p[3]) {
        return 0;
    }
    const char* q = p + 3;
    while (*q) {
        if (*q < '0' || *q > '9') {
            return 0;
        }
        q++;
    }
    return 1;
}

static int dir_is_node(const struct dirent* d) {
    const char* p = d->d_name;
    if (strncmp(p, "node", 4) != 0) {
        return 0;
    }
    if (!p[4]) {
        return 0;
    }
    const char* q = p + 4;
    while (*q) {
        if (*q < '0' || *q > '9') {
            return 0;
        }
        q++;
    }
    return 1;
}

static void parse_cpulist(const char* s, int* map, int map_max, int value) {
    if (!s) {
        return;
    }
    const char* p = s;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') {
            p++;
        }
        if (!*p) {
            break;
        }
        char* end = NULL;
        long a = strtol(p, &end, 10);
        if (end == p) {
            break;
        }
        p = end;
        long b = a;
        if (*p == '-') {
            p++;
            b = strtol(p, &end, 10);
            if (end == p) {
                break;
            }
            p = end;
        }
        for (long i = a; i <= b; i++) {
            if (i >= 0 && i < map_max) {
                map[i] = value;
            }
        }
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            p++;
        }
        if (*p == ',') {
            p++;
        }
    }
}

static void apply_fallback(cws_cpu_topology_t* out) {
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc < 1) {
        nproc = 1;
    }
    int n = (int)nproc;
    out->n_sockets = 1;
    out->n_numa_nodes = 1;
    out->cores_per_socket = n;
    out->threads_per_core = 1;
    out->n_logical = n;
    out->page_size = (size_t)sysconf(_SC_PAGESIZE);
    long cl = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    out->cache_line = (cl > 0) ? (size_t)cl : (size_t)64;
    out->cpus = calloc((size_t)n, sizeof(cws_cpu_info_t));
    if (!out->cpus) {
        return;
    }
    for (int i = 0; i < n; i++) {
        cws_cpu_info_t* c = &out->cpus[i];
        c->cpu_id = i;
        c->socket_id = 0;
        c->core_id = i;
        c->numa_node = 0;
        c->thread_id = 0;
        c->l1d_kb = 32;
        c->l1i_kb = 32;
        c->l2_kb = 1024;
        c->l3_kb = 32768;
    }
}

int cws_cpu_topology_detect(cws_cpu_topology_t* out) {
    if (!out) {
        return CWS_ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));

    DIR* cpudir = opendir("/sys/devices/system/cpu");
    if (!cpudir) {
        apply_fallback(out);
        return (out->cpus) ? CWS_OK : CWS_ERR_GENERIC;
    }

    int kernel_max = 0;
    if (read_int_file("/sys/devices/system/cpu/kernel_max", &kernel_max) != 0) {
        kernel_max = 1023;
    }
    if (kernel_max < 0) {
        kernel_max = 1023;
    }
    int map_max = kernel_max + 1;
    if (map_max < 1) {
        map_max = 1;
    }
    if (map_max > 8192) {
        map_max = 8192;
    }

    int* numa_map = (int*)calloc((size_t)map_max, sizeof(int));
    if (!numa_map) {
        closedir(cpudir);
        apply_fallback(out);
        return (out->cpus) ? CWS_OK : CWS_ERR_GENERIC;
    }
    for (int i = 0; i < map_max; i++) {
        numa_map[i] = -1;
    }

    DIR* nodedir = opendir("/sys/devices/system/node");
    if (nodedir) {
        struct dirent* ne;
        int max_node = -1;
        while ((ne = readdir(nodedir)) != NULL) {
            if (!dir_is_node(ne)) {
                continue;
            }
            int node_id = atoi(ne->d_name + 4);
            if (node_id > max_node) {
                max_node = node_id;
            }
            char p[512];
            snprintf(p, sizeof(p),
                     "/sys/devices/system/node/%s/cpulist", ne->d_name);
            char buf[1024];
            if (read_str_file(p, buf, sizeof(buf)) == 0) {
                parse_cpulist(buf, numa_map, map_max, node_id);
            }
        }
        closedir(nodedir);
        out->n_numa_nodes = max_node + 1;
        if (out->n_numa_nodes < 1) {
            out->n_numa_nodes = 1;
        }
    } else {
        out->n_numa_nodes = 1;
    }

    cws_cpu_info_t* tmp = (cws_cpu_info_t*)calloc((size_t)map_max,
                                                  sizeof(cws_cpu_info_t));
    if (!tmp) {
        free(numa_map);
        closedir(cpudir);
        apply_fallback(out);
        return (out->cpus) ? CWS_OK : CWS_ERR_GENERIC;
    }

    int n = 0;
    int max_socket = -1;
    struct dirent* ce;
    while ((ce = readdir(cpudir)) != NULL) {
        if (!dir_is_cpu(ce)) {
            continue;
        }
        int cpu = atoi(ce->d_name + 3);
        if (cpu < 0 || cpu >= map_max) {
            continue;
        }
        char b[512];
        int sock = 0, core = 0;
        snprintf(b, sizeof(b),
                 "/sys/devices/system/cpu/%s/topology/physical_package_id",
                 ce->d_name);
        if (read_int_file(b, &sock) != 0) {
            sock = 0;
        }
        snprintf(b, sizeof(b),
                 "/sys/devices/system/cpu/%s/topology/core_id",
                 ce->d_name);
        if (read_int_file(b, &core) != 0) {
            core = cpu;
        }
        if (sock > max_socket) {
            max_socket = sock;
        }

        cws_cpu_info_t info;
        memset(&info, 0, sizeof(info));
        info.cpu_id = cpu;
        info.socket_id = sock;
        info.core_id = core;
        info.numa_node = numa_map[cpu];
        info.thread_id = 0;
        info.l1d_kb = 0;
        info.l1i_kb = 0;
        info.l2_kb = 0;
        info.l3_kb = 0;

        for (int idx = 0; idx < 8; idx++) {
            char ip[512];
            snprintf(ip, sizeof(ip),
                     "/sys/devices/system/cpu/%s/cache/index%d/level",
                     ce->d_name, idx);
            int level = 0;
            if (read_int_file(ip, &level) != 0) {
                break;
            }
            char tp[512];
            snprintf(tp, sizeof(tp),
                     "/sys/devices/system/cpu/%s/cache/index%d/type",
                     ce->d_name, idx);
            char ttype[32];
            if (read_str_file(tp, ttype, sizeof(ttype)) != 0) {
                ttype[0] = '\0';
            }
            char sp[512];
            snprintf(sp, sizeof(sp),
                     "/sys/devices/system/cpu/%s/cache/index%d/size",
                     ce->d_name, idx);
            int sz = read_size_file(sp);
            if (!ttype[0]) {
                continue;
            }
            if (level == 1) {
                if (strcmp(ttype, "Data") == 0) {
                    info.l1d_kb = sz;
                } else if (strcmp(ttype, "Instruction") == 0) {
                    info.l1i_kb = sz;
                } else {
                    if (!info.l1d_kb) {
                        info.l1d_kb = sz;
                    }
                    if (!info.l1i_kb) {
                        info.l1i_kb = sz;
                    }
                }
            } else if (level == 2) {
                info.l2_kb = sz;
            } else if (level == 3) {
                info.l3_kb = sz;
            }
        }

        tmp[n++] = info;
        if (n >= map_max) {
            break;
        }
    }
    closedir(cpudir);
    free(numa_map);

    if (n == 0) {
        free(tmp);
        apply_fallback(out);
        return (out->cpus) ? CWS_OK : CWS_ERR_GENERIC;
    }

    qsort(tmp, (size_t)n, sizeof(cws_cpu_info_t), cmp_cpu_socket_core);

    for (int i = 0; i < n; i++) {
        int tid = 0;
        int j = i - 1;
        while (j >= 0 && tmp[j].socket_id == tmp[i].socket_id &&
               tmp[j].core_id == tmp[i].core_id) {
            tid = tmp[j].thread_id + 1;
            break;
        }
        tmp[i].thread_id = tid;
    }

    int distinct_cores = 0;
    int last_sock = -1, last_core = -1;
    for (int i = 0; i < n; i++) {
        if (tmp[i].socket_id != last_sock || tmp[i].core_id != last_core) {
            distinct_cores++;
            last_sock = tmp[i].socket_id;
            last_core = tmp[i].core_id;
        }
    }

    out->n_sockets = max_socket + 1;
    if (out->n_sockets < 1) {
        out->n_sockets = 1;
    }
    out->n_logical = n;
    out->cores_per_socket = distinct_cores / out->n_sockets;
    if (out->cores_per_socket < 1) {
        out->cores_per_socket = 1;
    }
    if (distinct_cores > 0) {
        out->threads_per_core = n / distinct_cores;
    } else {
        out->threads_per_core = 1;
    }
    if (out->threads_per_core < 1) {
        out->threads_per_core = 1;
    }
    out->page_size = (size_t)sysconf(_SC_PAGESIZE);
    long cl = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    out->cache_line = (cl > 0) ? (size_t)cl : (size_t)64;

    out->cpus = (cws_cpu_info_t*)calloc((size_t)n, sizeof(cws_cpu_info_t));
    if (!out->cpus) {
        free(tmp);
        return CWS_ERR_NOMEM;
    }
    memcpy(out->cpus, tmp, (size_t)n * sizeof(cws_cpu_info_t));
    free(tmp);

    return CWS_OK;
}

void cws_cpu_topology_free(cws_cpu_topology_t* topo) {
    if (!topo) {
        return;
    }
    free(topo->cpus);
    memset(topo, 0, sizeof(*topo));
}

int cws_cpu_pin(int cpu_id) {
#ifdef __linux__
    cpu_set_t set;
    CPU_ZERO(&set);
    unsigned cidx = (unsigned)cpu_id;
    CPU_SET(cidx, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        return -1;
    }
    return 0;
#else
    errno = ENOTSUP;
    (void)cpu_id;
    return -1;
#endif
}

int cws_cpu_recommended_workers(const cws_cpu_topology_t* topo) {
    if (!topo) {
        return 1;
    }
    int total = topo->n_sockets * topo->cores_per_socket;
    if (total < 1) {
        total = 1;
    }
    return total;
}

int* cws_cpu_placement(const cws_cpu_topology_t* topo,
                       int                        n_workers,
                       int                       *out_n_placed) {
    if (!topo || n_workers <= 0 || !out_n_placed) {
        return NULL;
    }
    if (n_workers > topo->n_logical) {
        return NULL;
    }
    int n_phys = topo->n_sockets * topo->cores_per_socket;
    if (n_phys < 1) {
        n_phys = topo->n_logical;
    }

    int* placement = (int*)calloc((size_t)n_workers, sizeof(int));
    if (!placement) {
        return NULL;
    }

    int* phys = (int*)calloc((size_t)topo->n_logical, sizeof(int));
    int* smt = (int*)calloc((size_t)topo->n_logical, sizeof(int));
    if (!phys || !smt) {
        free(phys);
        free(smt);
        free(placement);
        return NULL;
    }
    int n_phys_ids = 0;
    int n_smt_ids = 0;
    for (int i = 0; i < topo->n_logical; i++) {
        if (topo->cpus[i].thread_id == 0) {
            phys[n_phys_ids++] = topo->cpus[i].cpu_id;
        } else {
            smt[n_smt_ids++] = topo->cpus[i].cpu_id;
        }
    }

    int** per_socket = (int**)calloc((size_t)topo->n_sockets, sizeof(int*));
    int* per_socket_cnt = (int*)calloc((size_t)topo->n_sockets, sizeof(int));
    if (!per_socket || !per_socket_cnt) {
        free(phys); free(smt); free(placement);
        free(per_socket); free(per_socket_cnt);
        return NULL;
    }
    int* caps = (int*)calloc((size_t)topo->n_sockets, sizeof(int));
    if (!caps) {
        free(phys); free(smt); free(placement);
        free(per_socket); free(per_socket_cnt);
        return NULL;
    }
    for (int s = 0; s < topo->n_sockets; s++) {
        per_socket[s] = (int*)calloc((size_t)n_phys_ids + 1, sizeof(int));
        if (!per_socket[s]) {
            for (int k = 0; k < s; k++) {
                free(per_socket[k]);
            }
            free(phys); free(smt); free(placement);
            free(per_socket); free(per_socket_cnt); free(caps);
            return NULL;
        }
    }
    for (int i = 0; i < n_phys_ids; i++) {
        int cid = phys[i];
        int sk = -1;
        for (int j = 0; j < topo->n_logical; j++) {
            if (topo->cpus[j].cpu_id == cid) {
                sk = topo->cpus[j].socket_id;
                break;
            }
        }
        if (sk < 0 || sk >= topo->n_sockets) {
            sk = 0;
        }
        per_socket[sk][per_socket_cnt[sk]++] = cid;
    }
    free(phys);

    int placed = 0;
    for (int i = 0; i < n_phys_ids && placed < n_workers; i++) {
        int s = i % topo->n_sockets;
        if (caps[s] < per_socket_cnt[s]) {
            placement[placed++] = per_socket[s][caps[s]];
            caps[s]++;
        } else {
            for (int s2 = 0; s2 < topo->n_sockets && placed < n_workers; s2++) {
                if (caps[s2] < per_socket_cnt[s2]) {
                    placement[placed++] = per_socket[s2][caps[s2]];
                    caps[s2]++;
                    break;
                }
            }
        }
    }

    int smt_idx = 0;
    while (placed < n_workers && smt_idx < n_smt_ids) {
        placement[placed++] = smt[smt_idx++];
    }

    for (int s = 0; s < topo->n_sockets; s++) {
        free(per_socket[s]);
    }
    free(per_socket);
    free(per_socket_cnt);
    free(caps);
    free(smt);

    if (placed < n_workers) {
        free(placement);
        return NULL;
    }

    *out_n_placed = n_workers;
    return placement;
}

void cws_cpu_placement_free(int* placement) {
    free(placement);
}

int cws_cpu_numa_for(const cws_cpu_topology_t* topo, int cpu_id) {
    if (!topo || !topo->cpus) {
        return -1;
    }
    for (int i = 0; i < topo->n_logical; i++) {
        if (topo->cpus[i].cpu_id == cpu_id) {
            return topo->cpus[i].numa_node;
        }
    }
    return -1;
}
