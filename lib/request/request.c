#include "request.h"

int parse_request_path(const char* buffer, char** out_path) {
    if (strncmp(buffer, "GET ", 4) != 0) {
        return -1;
    }

    char* path = (char*)buffer + 4;
    char* end = strchr(path, ' ');
    if (!end) {
        return -1;
    }

    *end = '\0';
    *out_path = path;
    return 0;
}

int request_path_is_safe(const char* path) {
    return strstr(path, "..") == NULL;
}
