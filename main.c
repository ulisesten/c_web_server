#include <stdio.h>
#include <stdlib.h>

#include "lib/http/http.h"
#include "lib/response/response.h"

int server_socket;
int route_count = 0;
Route routes[MAX_ROUTES];

void index_handler(Request* req, Response* res) {
    (void)req;
    send_html(res->client_fd, "index.html");
}

void custom_handler(Request* req, Response* res) {
    (void)req;
    send_json(res->client_fd, "{\"message\": \"Hello from custom handler!\", \"error\": false}");
}

void on_server_ready(int err_code, int port) {
    if (err_code < 0) {
        fprintf(stderr, "Error al iniciar el servidor: %d\n", err_code);
        exit(1);
    }
    printf("Servidor iniciado correctamente en el puerto %d\n", port);
}

int main() {
    int port = 8080;

    use_route("/", index_handler);
    use_route("/custom", custom_handler);

    http_listen(port, on_server_ready);

    return 0;
}
