#include "http.h"
#include <sys/time.h>


void use_route(const char* path, RouteHandler handler) {
    if (route_count < MAX_ROUTES) {
        routes[route_count].path = path;
        routes[route_count].handler = handler;
        route_count++;
    }
}

RouteHandler find_route(const char* path) {
    for (int i = 0; i < route_count; i++) {
        if (strcmp(routes[i].path, path) == 0) {
            return routes[i].handler;
        }
    }
    return NULL;
}

void handle_sigint(int sig) {
    close(server_socket);
    printf("\nServidor cerrado. Puerto liberado.\n");
    exit(0);
}

short http_validate_path(const char* path, int client_fd) {
    if (!request_path_is_safe(path)) {
        const char *forbidden = "HTTP/1.1 403 Forbidden\r\n\r\n";
        send(client_fd, forbidden, strlen(forbidden), 0);
        close(client_fd);
        return -1;
    }

    return 0;
}

static int init_socket(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        perror("socket");
        return 1;
    }

    signal(SIGINT, handle_sigint);

    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };

    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(s);
        return -1;
    }

    if (listen(s, 10) < 0) {
        perror("listen");
        close(s);
        return -1;
    }

    return s;
}

int http_listen(int port, void (*callback)(int err_code, int port)) {
    server_socket = init_socket(port);
    if (server_socket != 0) {
        callback(server_socket, port);
    }

    while (1) {
        int client_fd = accept(server_socket, NULL, NULL);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        struct timeval timeout = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        while (1) {
            char buffer[1024] = {0};
            ssize_t received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
            if (received <= 0) {
                break;
            }

            char* path = NULL;
            if (parse_request_path(buffer, &path) != 0) {
                const char *bad_request =
                    "HTTP/1.1 400 Bad Request\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: keep-alive\r\n"
                    "\r\n";
                send(client_fd, bad_request, strlen(bad_request), 0);
                continue;
            }

            short handled = http_validate_path(path, client_fd);
            if (handled != 0) {
                break;
            }

            handled = http_dispatch_request(path, client_fd);
            if (handled == 0) {
                const char *not_found =
                    "HTTP/1.1 404 Not Found\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: keep-alive\r\n"
                    "\r\n";
                send(client_fd, not_found, strlen(not_found), 0);
            }
        }
        close(client_fd);
    }

    close(server_socket);
    return 0;
}

short http_dispatch_request(const char* path, int client_fd) {
    RouteHandler handler = find_route(path);
    if (handler) {
        Request req = { .path = (char*)path };
        Response res = {0, .client_fd = client_fd };
        handler(&req, &res);
        return 1;
    }
    return 0;
}
