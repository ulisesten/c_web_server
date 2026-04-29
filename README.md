# C Web Server

Servidor HTTP básico en C con enrutamiento simple y separación por módulos:

- `http`: ciclo del servidor, sockets y dispatch de rutas
- `request`: parseo y validación de request
- `response`: utilidades para responder HTML y JSON

## Estructura

- `main.c`: define rutas y arranca el servidor
- `lib/http/http.h`, `lib/http/http.c`: listener HTTP y router
- `lib/request/request.h`, `lib/request/request.c`: parseo de request GET y validación de path
- `lib/response/response.h`, `lib/response/response.c`: respuestas HTTP (`send_html`, `send_json`)

## Build

### Con CMake (recomendado)

```bash
cmake -S . -B build
cmake --build build
```

Ejecutar:

```bash
./build/main
```

## Uso

Servidor por defecto en `http://localhost:8080`.

Rutas actuales:

- `GET /` -> sirve `index.html`
- `GET /custom` -> responde JSON

## API de Módulos

### HTTP (`lib/http/http.h`)

- `void use_route(const char* path, RouteHandler handler);`
- `int http_listen(int port, void (*callback)(int err_code, int port));`

`RouteHandler` recibe:

- `Request* req`
- `Response* res`

### Request (`lib/request/request.h`)

- `int parse_request_path(const char* buffer, char** out_path);`
  - Espera request `GET ...`
  - Retorna `0` si pudo extraer path
- `int request_path_is_safe(const char* path);`
  - Rechaza rutas con `..`

### Response (`lib/response/response.h`)

- `int send_html(int client_fd, const char* p_html);`
- `int send_json(int client_fd, const char* json_str);`

## Keep-Alive

El servidor mantiene conexiones activas (`HTTP/1.1 keep-alive`):

- Respuestas incluyen `Connection: keep-alive`
- Se envía `Content-Length` en HTML y JSON
- Cada conexión cliente acepta múltiples requests
- Timeout de inactividad de 5 segundos por socket cliente

## Ejemplo de handler

```c
void custom_handler(Request* req, Response* res) {
    (void)req;
    send_json(res->client_fd, "{\"message\": \"Hello\", \"error\": false}");
}
```

## Notas

- Este servidor implementa un subconjunto simple de HTTP.
- Actualmente el parseo está orientado a requests `GET`.
