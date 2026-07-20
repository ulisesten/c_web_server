#ifndef CWS_CPU_H
#define CWS_CPU_H

#include <stdint.h>
#include <stddef.h>

/*
 * CPU topology + NUMA detection tuned for Intel Xeon and AMD EPYC.
 *
 * Both architectures expose multiple sockets / NUMA nodes with tens to
 * hundreds of logical CPUs. We expose:
 *   - socket count
 *   - NUMA node count
 *   - cores per socket
 *   - logical CPUs (hardware threads)
 *   - L1/L2/L3 cache sizes (for arena sizing)
 *
 * All detection is best-effort via sysfs/lscpu and does not depend on
 * libnuma being installed. On fallback we assume a single NUMA node.
 */

typedef struct {
    int     cpu_id;          /* logical CPU id                                         */
    int     socket_id;       /* physical package/socket                                */
    int     core_id;         /* physical core within socket                            */
    int     numa_node;       /* NUMA node owning this CPU                             */
    int     thread_id;       /* SMT sibling index within the core (0 = primary)       */
    int     l1d_kb;          /* L1 data cache size (KB)                                */
    int     l1i_kb;          /* L1 instruction cache size (KB)                         */
    int     l2_kb;           /* L2 cache size (KB)                                     */
    int     l3_kb;           /* L3 cache size (KB) shared per socket                   */
} cws_cpu_info_t;

typedef struct {
    int            n_sockets;
    int            n_numa_nodes;
    int            cores_per_socket;
    int            threads_per_core;   /* hyper-threads / SMT            */
    int            n_logical;          /* n_sockets * cores * smt       */
    size_t         page_size;
    size_t         cache_line;        /* sizeof(void*) * ... min align */
    cws_cpu_info_t* cpus;              /* array of size n_logical        */
} cws_cpu_topology_t;

/*
 * Detect the system topology. Returns 0 on success, <0 on error.
 * Writes a newly allocated topology (use cws_cpu_topology_free).
 */
int  cws_cpu_topology_detect(cws_cpu_topology_t* out);
void cws_cpu_topology_free(cws_cpu_topology_t* topo);

/*
 * Pin the current thread to a specific logical CPU.
 * Returns 0 on success, <0 on error.
 */
int  cws_cpu_pin(int cpu_id);

/*
 * Recommended worker count for an HTTP server given the topology:
 *   - Default to number of physical cores (best throughput per core)
 *   - Caller can override with n_workers_override > 0
 */
int  cws_cpu_recommended_workers(const cws_cpu_topology_t* topo);

/*
 * Recommend a placement array (length = n_workers) of logical CPU ids,
 * spreading workers across sockets/NUMA nodes to maximize memory
 * bandwidth and distribute accept load.
 *
 * Caller frees the returned array with cws_cpu_placement_free.
 */
int* cws_cpu_placement(const cws_cpu_topology_t* topo,
                      int                        n_workers,
                      int                       *out_n_placed);
void cws_cpu_placement_free(int* placement);

/*
 * Return the NUMA node id for a given logical CPU (from already-detected topo).
 */
int  cws_cpu_numa_for(const cws_cpu_topology_t* topo, int cpu_id);

#endif
