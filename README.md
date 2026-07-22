# CWS — High-performance HTTP library for C

`cws` is a tiny but fast HTTP/1.1 server library for C, designed for modern
multicore server hardware (Intel Xeon Scalable, AMD EPYC). It ships with:

- An HTTP/1.1 streaming parser (state machine, no per-request allocations)
- A worker pool with one `epoll` event loop per CPU core and worker pinning
- A radix router with `:param` and `*wildcard` capture
- A response builder with `writev` (single syscall for headers + body)
- `sendfile()` for zero-copy static files
- Prometheus text metrics built-in
- Async logger that never blocks the hot path
- CPU/NUMA topology detection to size worker pools automatically

## Use it as a library

CWS is a single static library (`libcws.a`) plus headers, with both
**CMake config** and **pkg-config** install support. Three ways to consume:

### 1) Git sub-module + `add_subdirectory`

```bash
git submodule add https://github.com/anomalyco/c_web_server third_party/cws
```

```cmake
add_subdirectory(third_party/cws)
target_link_libraries(your_app PRIVATE cws)
```

### 2) `find_package(cws)` after install

```bash
cd third_party/cws && cmake -S . -B build && cmake --install build --prefix /usr/local
```

```cmake
find_package(cws REQUIRED CONFIG)
target_link_libraries(your_app PRIVATE cws::cws)
```

### 3) Plain Makefile with pkg-config

```makefile
CFLAGS += $(shell pkg-config --cflags cws)
LDLIBS += $(shell pkg-config --libs cws)
app: main.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
```

## Minimal app

```c
#include "cws/cws.h"

static CWS_HANDLER(hello) {
    cws_response_body(res, "hello\n", 6, CWS_MT_TEXT_PLAIN);
    cws_response_send(res);
}

int main(void) {
    cws_app_t* app = cws_app_new();
    cws_app_port(app, 8080);

    CWS_GET(app, "/",      hello);
    CWS_GET(app, "/healthz", hello);

    return cws_app_run(app) == CWS_OK ? 0 : 1;
}
```

Compile: `gcc main.c -lcws -lpthread -o app`.

## Embedding into an existing process

```c
cws_app_t* app = cws_app_new();
/* ...configure and register routes... */
cws_app_run_async(app);   /* starts workers + acceptor in background */

/* your own event loop or other work here */

cws_app_wait(app);        /* or cws_app_stop() to trigger shutdown */
```

`cws_app_stop()` is signal-handler safe.

## Configuration

```c
cws_app_t* app = cws_app_new();
cws_app_port(app, 8080);
cws_app_bind(app, "127.0.0.1");     /* lock to loopback               */
cws_app_workers(app, 0);            /* 0 = auto (one per physical core) */
cws_app_pin(app, 1);                /* pin workers to CPUs (NUMA aware) */
cws_app_reuse_port(app, 1);         /* SO_REUSEPORT on multi-socket    */
cws_app_keepalive_ms(app, 30000);   /* keep-alive idle timeout         */
cws_app_max_body(app, 1 << 24);     /* 16 MiB cap                      */
cws_app_backlog(app, 4096);         /* listen() backlog                */
cws_app_log_level(app, CWS_LOG_INFO);
cws_app_static_dir(app, "assets");
```

For advanced needs use the lower-level API in `include/cws/server.h`,
`include/cws/route.h`, `include/cws/request.h`, `include/cws/response.h`.

## Performance notes (Xeon / EPYC)

- Compiled with `-march=x86-64-v3` (AVX2/BMI2/FMA) and `-falign-{functions,loops}=32`
  to match the 32-byte L1 line of both Intel Xeon Scalable and AMD EPYC cores.
- Worker count defaults to the number of physical cores (one worker per core,
  avoiding SMT contention on hot paths). Override with `cws_app_workers()`.
- SO_REUSEPORT round-robins accepts across sockets when enabled.
- Worker threads are pinned via `sched_setaffinity` and spread round-robin
  across sockets/NUMA nodes to balance memory bandwidth.

## License

MIT — see `LICENSE`.
