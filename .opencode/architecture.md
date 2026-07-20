# Arquitectura — C Web Server

> Documento de referencia para guiar el desarrollo futuro del servidor HTTP en C.
> Objetivos: **rendimiento máximo**, **código limpio**, **estructura escalable**.

---

## 1. Estado actual (baseline)

Servidor HTTP/1.1 monohilo, bloqueante, con keep-alive por socket.

| Módulo | Archivo | Responsabilidad | Estado |
|--------|---------|-----------------|--------|
| http   | `lib/http/http.c` | Listener, accept loop, router, dispatch | Funcional, bloqueante, single-thread |
| request | `lib/request/request.c` | Parseo de path (`GET <path> ...`) | Mínimo, sin headers/body |
| response | `lib/response/response.c` | `send_html` (sendfile), `send_json` | Funcional |
| main   | `main.c` | Registro de rutas y arranque | Acoplado a globals |

### Limitaciones detectadas

- **Concurrencia ausente**: un solo `accept` + `recv` bloqueante. Todas las conexiones se serializan.
- **Router O(n) lineal** con `strcmp` sobre array estático (`MAX_ROUTES = 128`). No escala en número de rutas ni soporta rutas con parámetros (`/users/:id`).
- **Parseo HTTP primitivo**: solo `GET`, sin method, headers, querystring, body, ni HTTP/1.1 `Host` obligatorio.
- **Buffer fijo de 1024 bytes**: una request con headers grandes se trunca, generando parseo silencioso incorrecto.
- **Mutación del buffer**: `parse_request_path` rompe el buffer original (`*end = '\0'`), complicando reusos.
- **Estado global mutable** (`server_socket`, `route_count`, `routes[]`) en `main.c`: impide múltiples instancias de servidor y dificulta testing.
- **Sin manejo de backpressure**: `send` y `sendfile` no verifican envío completo ni `EAGAIN`.
- **Sin timeouts robustos**: solo `SO_RCVTIMEO` de 5s; no hay `SO_SNDTIMEO` ni cierre por keep-alive inactivo.
- **Sin logging estructurado**: solo `printf`/`perror`.
- **Manejo de errores parcial**: `init_socket` retorna `1` o `-1`; `http_listen` trata `!= 0` como error — frágil.
- **Sin tests**, sin sanitizadores, sin CI.

---

## 2. Objetivos no funcionales

1. **Rendimiento**: ≥50k req/s en una sola máquina medianamente potente para rutas en memoria.
2. **Concurrencia**: aprovechar todos los núcleos sin bloqueo en I/O.
3. **Latencia determinista**: p99 < 5 ms con keep-alive en carga normal.
4. **Robustez**: zero leaks, zero buffer overruns, validación estricta del protocolo.
5. **Mantenibilidad**: módulos pequeños, interfaces estables, sin estado global.
6. **Observabilidad**: logs estructurados, métricas (req/s, latencia, errores).

---

## 3. Arquitectura objetivo

### 3.1 Modelo de concurrencia

Patrón **"acceptor + worker pool + I/O event loop por worker"**:

```
                 ┌─────────────────────┐
accept() ───────▶│  Connection Queue   │  (lock-free ring por worker)
                 └──────────┬──────────┘
                            │ round-robin / least-conn
       ┌──────────┬─────────┼──────────┬──────────┐
       ▼          ▼         ▼          ▼          ▼
   ┌──────┐  ┌──────┐  ┌──────┐  ┌──────┐  ┌──────┐
   │ Wkr0 │  │ Wkr1 │  │ Wkr2 │  │ Wkr3 │  │ WkrN │
   │ epoll│  │ epoll│  │ epoll│  │ epoll│  │ epoll│
   │ keep │  │ keep │  │ keep │  │ keep │  │ keep │
   └──────┘  └──────┘  └──────┘  └──────┘  └──────┘
```

- 1 hilo aceptador (o `accept4` con `SO_REUSEPORT` para eliminar el bottleneck).
- N workers (por defecto = núcleos), cada uno con su **epoll loop** en modo pozi-bloqueante (`EPOLLET` + `EPOLLONESHOT` para evitar carreras).
- Cada worker mantiene su propio pool de conexiones keep-alive con timeout (RB-tree o timing-wheel ordenado por expiración).
- Buffers de recepción reusables (arena/pool) — cero `malloc` por request en hot path.

**Migración incremental** (no romper nada):
1. **Fase 0**: hoy (monohilo bloqueante).
2. **Fase 1**: thread-pool con `pthread_create` por conexión + `pthread_mutex` en router. Mantiene la API.
3. **Fase 2**: workers dedicados + cola de conexiones (`mq` o `pipe2` de pass-the-fd).
4. **Fase 3**: `epoll` por worker + I/O no bloqueante, pool de buffers.
5. **Fase 4**: `SO_REUSEPORT` sin acceptor dedicado; cero copias con `sendfile`/`MSG_ZEROCOPY`.

### 3.2 Capas lógicas

```
┌─────────────────────────────────────────────────┐
│  App (main.c / handlers)                         │
├─────────────────────────────────────────────────┤
│  Routing layer  — árbol radix de rutas + params  │
├─────────────────────────────────────────────────┤
│  HTTP codec     — parser streaming (estado fin.) │
├─────────────────────────────────────────────────┤
│  Connection mgr — fd lifecycle, timeouts, keep   │
├─────────────────────────────────────────────────┤
│  Event loop     — epoll/kqueue abstraction      │
├─────────────────────────────────────────────────┤
│  Net utils / logging / config / metrics         │
└─────────────────────────────────────────────────┘
```

Cada capa es reemplazable e independiente de `main.c`.

---

## 4. Estructura de directorios propuesta

```
.
├── CMakeLists.txt                  # build + targets (lib, main, tests, sanitizers)
├── cmake/                          # scripts cmake reutilizables
├── certs/                         # TLS (cuando se añada)
├── config/
│   └── server.conf                 # (opcional) config en clave-valor
├── docs/
│   └── architecture.md             # esta guía vive en ARCHIVOS, no en .opencode
├── assets/                         # estáticos servidos (index.html, custom.html)
├── examples/
│   └── routes.c                    # ejemplos de handlers
├── include/                        # headers públicos del framework
│   └── cws/
│       ├── cws.h                   # umbrella include
│       ├── server.h
│       ├── request.h
│       ├── response.h
│       ├── route.h
│       ├── log.h
│       ├── config.h
│       ├── metrics.h
│       └── errors.h
├── src/                            # implementación del framework
│   ├── server/
│   │   ├── server.c                # Server struct sin globales
│   │   ├── acceptor.c
│   │   ├── worker.c                 # worker thread + event loop
│   │   ├── connection.c            # lifecycle del fd, timeouts, keep-alive
│   │   └── event_loop.c            # wrapper epoll/kqueue
│   ├── http/
│   │   ├── parser.c                # parser streaming (state machine)
│   │   ├── parser.h
│   │   ├── request.c                # Request struct rellenable
│   │   ├── response.c               # builder de Response (chain-style API)
│   │   ├── status.c                 # strings de status code
│   │   └── headers.c                # hash table de headers
│   ├── routing/
│   │   ├── route.c                  # Route + RouteHandler
│   │   ├── router.c                 # árbol radix con params
│   │   └── middleware.c             # pipeline pre-handler
│   ├── io/
│   │   ├── buffer.c                # buffer ring reutilizable
│   │   ├── sendfile.c
│   │   └── writev_helpers.c
│   ├── utils/
│   │   ├── log.c                    # logger estructurado (levels, sink)
│   │   ├── config.c
│   │   ├── metrics.c                # counters + histograms (prom text)
│   │   ├── time.c
│   │   └── strings.c
│   └── platform/
│       ├── linux.c                  # epoll
│       └── posix.c                  # fallback portable
├── tests/
│   ├── unit/
│   │   ├── test_parser.c
│   │   ├── test_router.c
│   │   ├── test_routes.c
│   │   └── test_headers.c
│   ├── integration/
│   │   └── test_keepalive.c
│   ├── perf/
│   │   └── bench_routes.sh          # wrk httpress
│   └── CMakeLists.txt              # CTest config
├── scripts/
│   ├── ci.sh
│   └── bench.sh
└── main.c                          # app demo (pasa a usar la API nueva)
```

Razonamiento:
- **`include/cws/`**: API pública limpia con namespace `cws_` cuarto de los proyectos C serios (similar a libuv). Permite usar el framework como librería estática.
- **`src/<layer>/`**: separación por concepto, no por tipo (`request.c` + `request.h` juntos mejora localidad, headers públicos en `include/`).
- **`tests/` paralelo**:	tests guían la forma porque nada >> unidad sin build.
- **`assets/` separado de la raíz**: evita servir accidentalmente código fuente.

---

## 5. Componentes clave: diseño

### 5.1 Parser HTTP (state machine)

Reemplaza el `strncmp` manual por un parser determinista, inspirado en `picohttparser`/`llhttp`:

- **Streaming**: consume el buffer actual, mantiene estado entre lecturas.
- **Tolerante a fragmentación**: una request puede llegar en varios `recv`.
- **Soporta**: method, path, query (parseada a kvs), headers (API de hashtable), body con `Content-Length` y `chunked`.
- **Cota de tamaño**: `MAX_HEADER_SIZE`, `MAX_HEADER_COUNT`, `MAX_BODY_SIZE` configurables.
- **API propuesta**:
  ```c
  typedef enum { CWS_PARSE_OK, CWS_PARSE_NEED_MORE, CWS_PARSE_ERR } cws_parse_status;
  cws_parse_status cws_parser_feed(cws_parser_t* p, const char* buf, size_t len);
  const cws_request_t* cws_parser_done(cws_parser_t* p);  // NULL si no completo
  ```
- Sin `malloc` por request: Request es un slab/arena reutilizado por conexión.

### 5.2 Router — árbol radix con parámetros

```c
typedef void (*cws_handler_t)(cws_request_t* req, cws_response_t* res);

void cws_router_add(cws_router_t*, const char* method, const char* path, cws_handler_t);
cws_handler_t cws_router_match(cws_router_t*, const char* method, const char* path, cws_params_t*);

// paths:
//   GET  /users/:id
//   POST /users
//   GET  /static/*               // wildcard
//   GET  /health                 // exacto
```

- **Complejidad O(m)** donde m = longitud del path (no número de rutas). Con hashing opcional para 1er segmento.
- Permite **middleware por ruta**:
  ```c
  cws_router_add(r, "GET", "/api/*", auth_middleware, api_handler);
  ```
- Válido: árbol radix trie comprimido por prefijos comunes, hoja con handler y capturadores de parámetros.

### 5.3 Server API sin estado global

Reemplazar `server_socket` / `routes[]` / `route_count` por:

```c
typedef struct cws_server  cws_server_t;
typedef struct cws_router   cws_router_t;
typedef struct cws_config   cws_config_t;

cws_config_t   cws_config_default(void);            // sane defaults
cws_server_t*  cws_server_new(const cws_config_t*); // alloc + setup
void           cws_server_set_router(cws_server_t*, cws_router_t*);
int            cws_server_run(cws_server_t*);       // bloquea
int            cws_server_stop(cws_server_t*);      // shutdown graceful

// callback de ready con error tipado, no `int err_code`
typedef void (*cws_ready_cb)(const cws_server_t*, int err, void* user);
```

- **Config incluye**: puerto, workers, backlog, timeouts (read/write/keepalive), max body, log_level, bind addr.
- **Shutdown graceful**: drain de conexiones (timeout configurable) antes de cerrar fds.
- **Múltiples servidores en un proceso** posible (sin globals).
- **Testing**: constructor `cws_server_new`.LoggerFactory`-como para inyectar mocks.

### 5.4 Response builder

Reemplazar `send_html`/`send_json` por builder más flexible (chainable, sin generar el header a mano cada vez):

```c
cws_response_t* res = cws_response_begin(req);
cws_response_status(res, 200);
cws_response_header(res, "Cache-Control", "no-store");
cws_response_body(res, body_ptr, body_len, CWS_MIME_JSON);  // zero-copy si const
cws_response_send(res);    // usa writev para header+body en un syscall
```

- **`writev`**: reduce syscalls combinando headers + body.
- **`sendfile`** para archivos grandes con `MSG_ZEROCOPY` en Linux moderno.
- **`Content-Length` automática** salvo `Transfer-Encoding: chunked` si se strream.
- **Cache de status strings** y de `Date` headers.

### 5.5 Conexiones y event loop

```c
typedef struct cws_conn {
    int fd;
    int worker_idx;
    cws_parser_t parser;
    cws_arena_t  arena;        // buffer de request reutilizado
    uint64_t     last_active;  // para timeout
    int          shutting_down;
    // ...
} cws_conn_t;
```

- Pool de `cws_conn_t` por worker (free-list), sin `malloc/free` por request.
- Timer heap (min-heap) por worker para timeouts.
- `EPOLLET` edge-triggered: leer hasta `EAGAIN`, escribir completo con `writev`.
- **Sin mutex en hot path** salvo entre workers (solo queue de aceptar).

### 5.6 Logging y métricas

```c
typedef enum { CWS_LOG_TRACE, CWS_LOG_DEBUG, CWS_LOG_INFO, CWS_LOG_WARN, CWS_LOG_ERROR } cws_log_level;

void cws_log(cws_log_level, const char* fmt, ...);
// CWS_LOG_INFO("request method=%s path=%s fd=%d status=%d latency_us=%lld",
//              method, path, fd, status, latency);
```

- **Logger async con ring buffer** (lock-free SPSC) para no bloquear hot path; flush periódico.
- **Métricas**: counters e histogramas en atomic uint64s. Endpoint `/metrics` formato Prometheus text exposition.

### 5.7 Configuración

- Cargar de archivo `config/server.conf` (formato `clave valor` o JSON minimal) con override por CLI (`--port`, `--workers`, ...).
- Validación exhaustiva en boot: si config inválida, aborta con mensaje claro.

---

## 6. Pipeline de una request (futuro)

```
accept() ─▶ enqueue fd ─▶ worker.take()
                         │
                         ▼
        ┌────────────────────────────────────────┐
        │ epoll_wait ─▶ EPOLLIN fd               │
        │   recv → parser.feed                   │
        │   if done:                              │
        │     router.match(method, path)          │
        │     middleware_chain → handler           │
        │     response.build → writev(header,body)│
        │   if keep-alive: re-arm EPOLLONESHOT     │
        │   else: close(fd), return conn to pool   │
        └────────────────────────────────────────┘
```

---

## 7. Seguridad

- Validación de path con `..` (mantener `request_path_is_safe`) + **canonicalización**.
- **Límites duros**: max header, max body, max headers count, max uri length.
- **No ejecución arbitraria del FS**: mapa explícito de rutas estáticas a directorios allow-list.
- **TLS opcional** (fase avanzada) con lugar reservado en `src/io/tls.c` (TLS via OpenSSL o BearSSL).
- **So_REUSEPORT** + bind a 127.0.0.1 por defecto (cambiar en config).
- **Signals**: SIGINT/SIGTERM → graceful shutdown; SIGPIPE → ignorado; SIGHUP → reload de logfd.

---

## 8. Build, tests, CI

### 8.1 CMake objetivo

```cmake
option(CWS_BUILD_TESTS    "Build tests" ON)
option(CWS_BUILD_BENCH    "Build benchmarks" OFF)
option(CWS_USE_ASAN       "AddressSanitizer" OFF)
option(CWS_USE_MSAN       "MemorySanitizer" OFF)
option(CWS_USE_UBSAN      "UBSan" OFF)
option(CWS_USE_TSAN       "ThreadSanitizer" OFF)
option(CWS_STATIC_ANALYSIS "Run clang-tidy on build" OFF)

add_library(cws STATIC ${CWS_SOURCES})
target_include_directories(cws PUBLIC include)

add_executable(cws_main main.c)
target_link_libraries(cws_main PRIVATE cws)

if(CWS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)   # CTest con Cmocka/Unity
endif()
```

- **Sanitizers** en CI para cazar bugsiciones/UB.
- **clang-tidy** + **cppcheck** corren en CI.
- **valgrind/memcheck** semanal.

### 8.2 Tests (prioridad)

1. **Unit**: parser (varias requests fragmentadas), router (parámetros, wildcards, 404), headers (multilínea, case-insensitive), buffer pool, time helpers.
2. **Integration**: keep-alive multi-request, timeouts, backpressure, shutdown graceful, multiples workers.
3. **Benchmarks**: `wrk -t${N} -c1000 -d30s http://127.0.0.1:8080/` y `httpress`. Regresiones vs commit anterior vía `bench.sh` outputs CSV JSON en `/tmp/cws-bench`.

### 8.3 CI (GitHub Actions)

Trabajo en pipeline matrix:
- GCC 11 + Clang 14
- Ubuntu 22.04 + (opcional) Alpine
- Steps: `cmake -S . -B build -DCWS_USE_ASAN=ON && cmake --build build && ctest --output-on-failure`
- Build con `-Werror -Wall -Wextra -Wpedantic -Wconversion`.

---

## 9. Plan de ejecución incremental

Mantener el servidor funcional en cada paso (cada fase = PR revisable).

### Fase 1 — Refactor interno (sin cambio de comportamiento)
- [ ] Mover archivos a `src/`, `include/cws/`, `tests/` según el árbol.
- [ ] CMake modular (object library + exe + tests).
- [ ] Eliminar globales: introducir `cws_server_t` (puede aún ser mono-hilo).
- [ ] Renombrar todo con prefijo `cws_`.
- [ ] Añadir `Werror` + sanitizers por defecto en debug.
- [ ] Tests unitarios para parser y router actuales.
- **Resultado**: mismo comportamiento, código reorganizado, primera red de seguridad.

### Fase 2 — Parser HTTP completo
- [ ] State machine собственно HTTP/1.1 (`request-line`, headers, body).
- [ ] `cws_request_t` con method, path, query (kvs), headers (hashtable).
- [ ] Soporta GET/POST/PUT/DELETE/HEAD/OPTIONS.
- [ ] Límites de tamaño configurables.
- [ ] Tests: fragmentación, afirmaciones con golden samples.
- **Resultado**: API fácil de usar para handlers, sin romper actuales.

### Fase 3 — Router escalable y middleware
- [ ] Implementar árbol radix.
- [ ] Soporte de `:param`, `*wildcard`, methods.
- [ ] Pipeline de middleware (auth, log, cors, rate-limit optional).
- [ ] Bench: comparar vs array lineal en 1000 rutas.
- **Resultado**: millón de rutas sin degradación.

### Fase 4 — Thread-pool por conexión
- [ ] `pthread_create` por `accept`, mutex en router (router read-only tras boot → lectura libre).
- [ ] Pool de threads fijo en lugar de spawn por conexión (crecer hasta N max, queue).
- [ ] Logging async.
- **Resultado**: concurrencia real, throughput × cores sin cambiar API.

### Fase 5 — Event loop + I/O no bloqueante
- [ ] `epoll` (Linux) / `kqueue` (BSD) en cada worker.
- [ ] `O_NONBLOCK`, `EPOLLET`, `EPOLLONESHOT`.
- [ ] Timeout management (min-heap por worker).
- [ ] Pool de buffers por worker (reuse arena).
- **Resultado**: 50k+ req/s, latencia p99 baja, sin tirar conexiones en picos.

### Fase 6 — Zero-copy y hiper-tuning
- [ ] `SO_REUSEPORT` sin acceptor dedicado (kernel balancea).
- [ ] `sendfile`+`MSG_ZEROCOPY` para archivos.
- [ ] `TCP_QUICKACK`, `TCP_NODELAY`, `TCP_CORK` estratégicos.
- [ ] `io_uring` experimental (alt path opcional).
- **Resultado**: cuello de botella = syscall, no aplicación.

### Fase 7 — Operabilidad
- [ ] `/metrics` (Prometheus).
- [ ] Graceful shutdown con drain.
- [ ] Reload de configuración en caliente (SIGHUP).
- [ ] Healthcheck `/healthz`.
- [ ] TLS opcional (BearSSL u OpenSSL).
- **Resultado**: listo para producción.

---

## 10. Decisiones arquitecturales anotadas (ADR-style)

- **ADR-001 — C puro, no C++**: máximo control, ABI limpia, cero runtime.
- **ADR-002 — Sin dependencias externas mandatory**: el núcleo compila con libc. OpenSSL/es opcional.
- **ADR-003 — Worker-per-core con epoll edge-triggered**: mejor relación throughput / simpleza para Linux servidores.
- **ADR-004 — Árbol radix sobre hash table**: soporta parámetros de path sin estructuras extra.
- **ADR-005 — Builder API para Response**: evita formatos de string причине ad-hoc por cada handler.
- **ADR-006 — Sin globals**: cualquier estado vive en `cws_server_t*` pasado explícitamente.
- **ADR-007 — Fases incrementales**: nunca romper el servidor durante el refactor.

---

## 11. Convetions de código

- **Naming**: prefijo `cws_` para símbolos públicos; `cws__` para internos.
- **Headers**: include guards `CWS_<MODULE>_H`, no pragma once (portabilidad C99).
- **Tipos**: `typedef struct cws_foo cws_foo_t;` siempre con sufijo `_t`.
- **Errores**: retornar `int` (0 ok, <0 error con `errno`) o enum tipada para APIs críticas.
- **Memoria**: ownership explícita. Quien crea, libera; o pasar ownership al framework si documentado.
- **Estilo**: 4 espacios, K&R braces, sin tabuladores. `.clang-format` con `BasedOnStyle: LLVM` ajustado.
- **Comentarios**: solo para "por qué", no para "qué". Compatible Doxygen para headers públicos.
- **Includes mínimo**: en archivos `.c` incluir lo estrictamente necesario. En headers `.h` solo lo expuesto.

---

## 12. Métricas objetivas de éxito

| Métrica | Hoy | Objetivo fase 5 | Objetivo fase 6 |
|---------|-----|-----------------|-----------------|
| req/s (1kB JSON) en 4-core | ~5-10k single-thread | 50k | 100k+ |
| p99 latency con 1k conns | n/d | < 5 ms | < 2 ms |
| LeakSanitizer | n/a | limpio | limpio |
| Buncho de tests | 0 | > 100 unit + 20 integ | idem |
| Memoria por conexión | sin límite | < 32 kB | < 16 kB |
| Líneas de código en `main.c` | 38 | < 20 | < 20 |

---

## 13. References internas (archivos actuales)

- `main.c:9` — `Route routes[MAX_ROUTES];` global que debe desaparecer.
- `lib/http/http.c:13-20` — `find_route` O(n) que se reemplaza por árbol radix.
- `lib/http/http.c:78-122` — bucle bloqueante que se reemplaza por worker + epoll.
- `lib/http/http.c:88-90` — buffer 1024 fijo que se substituye por pool de arenas.
- `lib/request/request.c:13-15` — mutación del buffer; reemplazado por parser.
- `lib/response/response.c:43` — `send` sin verificación de bytes enviados; reemplazado por writev con loop.
- `CMakeLists.txt` —备用Targets modular + tests + sanitizers por defecto.

---

*Última revisión: fecha de actualización de este documento. Mantener sincronizado con cualquier refactor estructural.*
