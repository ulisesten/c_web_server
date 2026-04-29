#ifndef HTTP_H
#define HTTP_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/sendfile.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <signal.h>
#include "../request/request.h"
#include "../response/response.h"

#define MAX_ROUTES 128

extern int server_socket;
extern int route_count;

typedef void (*RouteHandler)(Request*, Response*);

typedef struct {
    const char* path;
    RouteHandler handler;
} Route;

extern Route routes[MAX_ROUTES];

void use_route(const char* path, RouteHandler handler);
RouteHandler find_route(const char* path);
short http_validate_path(const char* path, int client_fd);
int http_listen(int port, void (*callback)(int err_code, int port));
short http_dispatch_request(const char* path, int client_fd);

#endif // HTTP_H
