#include "http.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "json_writer.h"

#define HTTP_HEADER_MAX 8192U

static int send_all(int fd, const char *data, size_t length)
{
    size_t sent = 0U;

    while (sent < length) {
        ssize_t n = send(fd, data + sent, length - sent, 0);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 0;
        }
        if (n == 0) {
            return 0;
        }
        sent += (size_t)n;
    }

    return 1;
}

static const char *reason_phrase(int status_code)
{
    switch (status_code) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 413:
            return "Payload Too Large";
        case 503:
            return "Service Unavailable";
        default:
            return "Internal Server Error";
    }
}

static int starts_with_header_name(const char *line, const char *name)
{
    size_t i;

    for (i = 0U; name[i] != '\0'; ++i) {
        char lhs = line[i];
        char rhs = name[i];

        if (lhs >= 'A' && lhs <= 'Z') {
            lhs = (char)(lhs - 'A' + 'a');
        }
        if (rhs >= 'A' && rhs <= 'Z') {
            rhs = (char)(rhs - 'A' + 'a');
        }
        if (lhs != rhs) {
            return 0;
        }
    }

    return line[i] == ':';
}

static int parse_content_length(const char *headers, size_t *out_length)
{
    const char *line;
    const char *next;

    *out_length = 0U;
    line = strstr(headers, "\r\n");
    if (line == NULL) {
        return 1;
    }
    line += 2;

    while (*line != '\0' && !(line[0] == '\r' && line[1] == '\n')) {
        next = strstr(line, "\r\n");
        if (next == NULL) {
            return 0;
        }

        if (starts_with_header_name(line, "Content-Length")) {
            const char *cursor = line + strlen("Content-Length") + 1U;
            size_t value = 0U;

            while (cursor < next && (*cursor == ' ' || *cursor == '\t')) {
                ++cursor;
            }
            if (cursor == next) {
                return 0;
            }
            while (cursor < next) {
                if (*cursor < '0' || *cursor > '9') {
                    while (cursor < next && (*cursor == ' ' || *cursor == '\t')) {
                        ++cursor;
                    }
                    if (cursor != next) {
                        return 0;
                    }
                    break;
                }
                if (value > ((size_t)-1 - (size_t)(*cursor - '0')) / 10U) {
                    return 0;
                }
                value = value * 10U + (size_t)(*cursor - '0');
                ++cursor;
            }
            *out_length = value;
            return 1;
        }

        line = next + 2;
    }

    return 1;
}

static int read_exact(int fd, char *buffer, size_t length)
{
    size_t read_count = 0U;

    while (read_count < length) {
        ssize_t n = recv(fd, buffer + read_count, length - read_count, 0);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 0;
        }
        if (n == 0) {
            return 0;
        }
        read_count += (size_t)n;
    }

    return 1;
}

static int header_is_complete(const char *header, size_t length)
{
    if (length < 4U) {
        return 0;
    }

    return header[length - 4U] == '\r' &&
           header[length - 3U] == '\n' &&
           header[length - 2U] == '\r' &&
           header[length - 1U] == '\n';
}

HttpReadStatus http_read_request(int client_fd, size_t max_body_size, HttpRequest *out_request)
{
    char header[HTTP_HEADER_MAX + 1U];
    size_t header_length = 0U;
    size_t content_length = 0U;
    char version[16];

    if (out_request == NULL) {
        return HTTP_READ_INTERNAL_ERROR;
    }
    memset(out_request, 0, sizeof(*out_request));
    memset(version, 0, sizeof(version));

    while (header_length < HTTP_HEADER_MAX) {
        ssize_t n = recv(client_fd, header + header_length, 1U, 0);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return HTTP_READ_BAD_REQUEST;
        }
        if (n == 0) {
            return HTTP_READ_BAD_REQUEST;
        }

        header_length += (size_t)n;
        if (header_is_complete(header, header_length)) {
            break;
        }
    }

    if (!header_is_complete(header, header_length)) {
        return HTTP_READ_BAD_REQUEST;
    }
    header[header_length] = '\0';

    if (sscanf(header, "%15s %255s %15s", out_request->method, out_request->path, version) != 3) {
        return HTTP_READ_BAD_REQUEST;
    }

    if (!parse_content_length(header, &content_length)) {
        return HTTP_READ_BAD_REQUEST;
    }
    if (content_length > max_body_size) {
        return HTTP_READ_BODY_TOO_LARGE;
    }

    out_request->body = (char *)malloc(content_length + 1U);
    if (out_request->body == NULL) {
        return HTTP_READ_INTERNAL_ERROR;
    }
    out_request->body[content_length] = '\0';
    out_request->body_length = content_length;

    if (content_length > 0U && !read_exact(client_fd, out_request->body, content_length)) {
        http_request_free(out_request);
        return HTTP_READ_BAD_REQUEST;
    }

    return HTTP_READ_OK;
}

void http_request_free(HttpRequest *request)
{
    if (request == NULL) {
        return;
    }

    free(request->body);
    request->body = NULL;
    request->body_length = 0U;
}

int http_send_json(int client_fd, int status_code, const char *json_body)
{
    char header[512];
    size_t body_length;
    int header_length;

    if (json_body == NULL) {
        json_body = "{\"success\":false,\"error_code\":\"INTERNAL_ERROR\",\"message\":\"empty response\"}";
        status_code = 500;
    }

    body_length = strlen(json_body);
    header_length = snprintf(header, sizeof(header),
                             "HTTP/1.1 %d %s\r\n"
                             "Content-Type: application/json\r\n"
                             "Content-Length: %zu\r\n"
                             "Connection: close\r\n"
                             "\r\n",
                             status_code,
                             reason_phrase(status_code),
                             body_length);

    if (header_length < 0 || (size_t)header_length >= sizeof(header)) {
        return 0;
    }

    return send_all(client_fd, header, (size_t)header_length) &&
           send_all(client_fd, json_body, body_length);
}

int http_send_error(int client_fd, int status_code, const char *error_code, const char *message)
{
    char *body = json_make_error(error_code, message);
    int ok;

    if (body == NULL) {
        body = json_make_error("INTERNAL_ERROR", "failed to build JSON error response");
        if (body == NULL) {
            return 0;
        }
        status_code = 500;
    }

    ok = http_send_json(client_fd, status_code, body);
    free(body);
    return ok;
}
