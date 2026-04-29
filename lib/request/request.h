#ifndef REQUEST_H
#define REQUEST_H

#include <string.h>

typedef struct {
    char* path;
} Request;

int parse_request_path(const char* buffer, char** out_path);
int request_path_is_safe(const char* path);

#endif // REQUEST_H
