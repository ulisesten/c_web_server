#ifndef RESPONSE_H
#define RESPONSE_H

#include <netinet/in.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    int status;
    int client_fd;
} Response;

int send_html(int client_fd, const char* p_html);
int send_json(int client_fd, const char* json_str);

#endif // RESPONSE_H
