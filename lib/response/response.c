#include "response.h"

int send_html(int client_fd, const char* p_html) {
    int html = open(p_html, O_RDONLY);
    if (html < 0) {
        const char *not_found =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Length: 0\r\n"
            "Connection: keep-alive\r\n"
            "\r\n";
        send(client_fd, not_found, strlen(not_found), 0);
        return -1;
    }

    struct stat stat_buf;
    if (fstat(html, &stat_buf) < 0) {
        const char *internal_error =
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Content-Length: 0\r\n"
            "Connection: keep-alive\r\n"
            "\r\n";
        send(client_fd, internal_error, strlen(internal_error), 0);
        close(html);
        return -1;
    }

    off_t offset = 0;
    char header[256];
    int header_len = snprintf(
        header,
        sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %lld\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        (long long)stat_buf.st_size
    );
    if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
        close(html);
        return -1;
    }
    send(client_fd, header, (size_t)header_len, 0);
    sendfile(client_fd, html, &offset, stat_buf.st_size);
    close(html);
    return 0;
}

int send_json(int client_fd, const char* json_str) {
    if (!json_str) {
        json_str = "{}";
    }

    size_t body_len = strlen(json_str);
    char header[256];
    int header_len = snprintf(
        header,
        sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        body_len
    );

    if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
        const char *internal_error =
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Content-Length: 0\r\n"
            "Connection: keep-alive\r\n"
            "\r\n";
        send(client_fd, internal_error, strlen(internal_error), 0);
        return -1;
    }

    send(client_fd, header, (size_t)header_len, 0);
    send(client_fd, json_str, body_len, 0);
    return 0;
}
