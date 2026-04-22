#include "http.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "utils.h"

enum {
    HTTP_HEADER_LIMIT = 16384
};

/* HTTP 계층 오류 메시지를 errbuf에 기록한다. */
static void set_error(char *errbuf, size_t errbuf_size, const char *fmt, ...)
{
    va_list args;

    if (errbuf == NULL || errbuf_size == 0U) {
        return;
    }

    va_start(args, fmt);
    vsnprintf(errbuf, errbuf_size, fmt, args);
    va_end(args);
}

/* buffer가 need 바이트 이상 담길 수 있게 용량을 늘린다. */
static int ensure_capacity(char **buffer,
                           size_t *capacity,
                           size_t need,
                           char *errbuf,
                           size_t errbuf_size)
{
    size_t new_capacity;
    char *grown;

    if (need <= *capacity) {
        return 0;
    }

    new_capacity = *capacity == 0U ? 4096U : *capacity;
    while (new_capacity < need) {
        new_capacity *= 2U;
    }

    grown = (char *)realloc(*buffer, new_capacity);
    if (grown == NULL) {
        set_error(errbuf, errbuf_size, "HTTP ERROR: out of memory");
        return -1;
    }

    *buffer = grown;
    *capacity = new_capacity;
    return 0;
}

/* ASCII case-insensitive header 이름 비교를 수행한다. */
static int equals_ignore_case(const char *left, const char *right)
{
    size_t i;

    if (left == NULL || right == NULL) {
        return 0;
    }

    for (i = 0U; left[i] != '\0' && right[i] != '\0'; ++i) {
        if (tolower((unsigned char)left[i]) != tolower((unsigned char)right[i])) {
            return 0;
        }
    }

    return left[i] == '\0' && right[i] == '\0';
}

/* 문자열 앞뒤 공백을 제거한 시작 포인터를 반환한다. */
static char *trim_spaces(char *text)
{
    char *end;

    if (text == NULL) {
        return NULL;
    }

    while (*text != '\0' && isspace((unsigned char)*text)) {
        text += 1;
    }

    if (*text == '\0') {
        return text;
    }

    end = text + strlen(text) - 1;
    while (end > text && isspace((unsigned char)*end)) {
        *end = '\0';
        end -= 1;
    }

    return text;
}

/* header 끝 위치와 구분자 길이를 찾는다. */
static int find_header_end(const char *buffer,
                           size_t length,
                           size_t *out_header_end,
                           size_t *out_delimiter_length)
{
    size_t i;

    for (i = 0U; i < length; ++i) {
        if (i + 3U < length &&
            buffer[i] == '\r' &&
            buffer[i + 1U] == '\n' &&
            buffer[i + 2U] == '\r' &&
            buffer[i + 3U] == '\n') {
            *out_header_end = i;
            *out_delimiter_length = 4U;
            return 1;
        }
        if (i + 1U < length &&
            buffer[i] == '\n' &&
            buffer[i + 1U] == '\n') {
            *out_header_end = i;
            *out_delimiter_length = 2U;
            return 1;
        }
    }

    return 0;
}

/* HTTP request line의 method/path/version을 파싱한다. */
static int parse_request_line(char *line,
                              HttpRequest *request,
                              char *errbuf,
                              size_t errbuf_size)
{
    char method[32];
    char path[1024];
    char version[32];

    if (sscanf(line, "%31s %1023s %31s", method, path, version) != 3) {
        set_error(errbuf, errbuf_size, "HTTP ERROR: invalid request line");
        return -1;
    }

    if (strncmp(version, "HTTP/", 5U) != 0) {
        set_error(errbuf, errbuf_size, "HTTP ERROR: unsupported protocol version");
        return -1;
    }

    request->method = strdup_safe(method);
    request->path = strdup_safe(path);
    if (request->method == NULL || request->path == NULL) {
        set_error(errbuf, errbuf_size, "HTTP ERROR: out of memory");
        return -1;
    }

    return 0;
}

/* header 블록에서 Content-Length만 읽고 Transfer-Encoding은 거절한다. */
static int parse_headers(char *headers,
                         size_t *out_content_length,
                         char *errbuf,
                         size_t errbuf_size)
{
    char *line = headers;

    *out_content_length = 0U;

    while (line != NULL && *line != '\0') {
        char *next = strchr(line, '\n');
        char *separator;
        char *name;
        char *value;
        char *endptr;
        unsigned long parsed_length;

        if (next != NULL) {
            *next = '\0';
        }
        if (line[0] != '\0' && line[strlen(line) - 1U] == '\r') {
            line[strlen(line) - 1U] = '\0';
        }

        separator = strchr(line, ':');
        if (separator == NULL) {
            set_error(errbuf, errbuf_size, "HTTP ERROR: malformed header");
            return -1;
        }

        *separator = '\0';
        name = trim_spaces(line);
        value = trim_spaces(separator + 1);

        if (equals_ignore_case(name, "Content-Length")) {
            errno = 0;
            parsed_length = strtoul(value, &endptr, 10);
            if (errno != 0 || endptr == value || *trim_spaces(endptr) != '\0') {
                set_error(errbuf, errbuf_size, "HTTP ERROR: invalid Content-Length");
                return -1;
            }
            *out_content_length = (size_t)parsed_length;
        } else if (equals_ignore_case(name, "Transfer-Encoding") &&
                   value[0] != '\0' &&
                   !equals_ignore_case(value, "identity")) {
            set_error(errbuf, errbuf_size, "HTTP ERROR: Transfer-Encoding is not supported");
            return -1;
        }

        line = next == NULL ? NULL : next + 1;
    }

    return 0;
}

/* data를 client_fd로 끝까지 보낸다. */
static int send_all(int client_fd, const char *data, size_t length)
{
    size_t total = 0U;

    while (total < length) {
        ssize_t written = send(client_fd, data + total, length - total, 0);

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (written == 0) {
            return -1;
        }

        total += (size_t)written;
    }

    return 0;
}

/* 현재 request가 소유한 동적 문자열을 모두 해제한다. */
void http_free_request(HttpRequest *request)
{
    if (request == NULL) {
        return;
    }

    free(request->method);
    free(request->path);
    free(request->body);
    memset(request, 0, sizeof(*request));
}

/* client_fd에서 단일 HTTP 요청을 읽고 method/path/body를 반환한다. */
HttpReadStatus http_read_request(int client_fd,
                                 size_t body_max_bytes,
                                 HttpRequest *out_request,
                                 char *errbuf,
                                 size_t errbuf_size)
{
    char *buffer = NULL;
    size_t buffer_length = 0U;
    size_t buffer_capacity = 0U;
    size_t header_end = 0U;
    size_t delimiter_length = 0U;
    size_t content_length = 0U;
    int found_header_end = 0;

    if (out_request == NULL) {
        set_error(errbuf, errbuf_size, "HTTP ERROR: invalid request output");
        return HTTP_READ_STATUS_INTERNAL_ERROR;
    }

    memset(out_request, 0, sizeof(*out_request));

    while (!found_header_end) {
        ssize_t received;

        if (buffer_length >= HTTP_HEADER_LIMIT) {
            free(buffer);
            set_error(errbuf, errbuf_size, "HTTP ERROR: header too large");
            return HTTP_READ_STATUS_BAD_REQUEST;
        }

        if (ensure_capacity(&buffer, &buffer_capacity, buffer_length + 4096U + 1U,
                            errbuf, errbuf_size) != 0) {
            free(buffer);
            return HTTP_READ_STATUS_INTERNAL_ERROR;
        }

        received = recv(client_fd, buffer + buffer_length, 4096U, 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buffer);
            set_error(errbuf, errbuf_size, "HTTP ERROR: failed to read request");
            return HTTP_READ_STATUS_BAD_REQUEST;
        }
        if (received == 0) {
            free(buffer);
            set_error(errbuf, errbuf_size, "HTTP ERROR: client closed connection early");
            return HTTP_READ_STATUS_BAD_REQUEST;
        }

        buffer_length += (size_t)received;
        buffer[buffer_length] = '\0';
        found_header_end = find_header_end(buffer, buffer_length, &header_end, &delimiter_length);
    }

    if (ensure_capacity(&buffer, &buffer_capacity, buffer_length + 1U, errbuf, errbuf_size) != 0) {
        free(buffer);
        return HTTP_READ_STATUS_INTERNAL_ERROR;
    }
    buffer[buffer_length] = '\0';

    {
        char *headers_copy = NULL;
        char *line_end;
        size_t headers_length = header_end;
        size_t body_bytes_in_buffer = buffer_length - (header_end + delimiter_length);

        headers_copy = (char *)malloc(headers_length + 1U);
        if (headers_copy == NULL) {
            free(buffer);
            set_error(errbuf, errbuf_size, "HTTP ERROR: out of memory");
            return HTTP_READ_STATUS_INTERNAL_ERROR;
        }

        memcpy(headers_copy, buffer, headers_length);
        headers_copy[headers_length] = '\0';

        line_end = strstr(headers_copy, "\r\n");
        if (line_end == NULL) {
            line_end = strchr(headers_copy, '\n');
        }
        if (line_end == NULL) {
            free(headers_copy);
            free(buffer);
            set_error(errbuf, errbuf_size, "HTTP ERROR: missing request line terminator");
            return HTTP_READ_STATUS_BAD_REQUEST;
        }

        *line_end = '\0';
        if (parse_request_line(headers_copy, out_request, errbuf, errbuf_size) != 0) {
            free(headers_copy);
            free(buffer);
            http_free_request(out_request);
            return HTTP_READ_STATUS_BAD_REQUEST;
        }

        if (*(line_end + 1) == '\n') {
            line_end += 1;
        }

        if (parse_headers(line_end + 1, &content_length, errbuf, errbuf_size) != 0) {
            free(headers_copy);
            free(buffer);
            http_free_request(out_request);
            return HTTP_READ_STATUS_BAD_REQUEST;
        }

        free(headers_copy);
        if (content_length > body_max_bytes) {
            free(buffer);
            http_free_request(out_request);
            set_error(errbuf, errbuf_size, "HTTP ERROR: request body exceeds limit");
            return HTTP_READ_STATUS_TOO_LARGE;
        }

        out_request->body = (char *)malloc(content_length + 1U);
        if (out_request->body == NULL) {
            free(buffer);
            http_free_request(out_request);
            set_error(errbuf, errbuf_size, "HTTP ERROR: out of memory");
            return HTTP_READ_STATUS_INTERNAL_ERROR;
        }

        out_request->content_length = content_length;
        out_request->body_length = body_bytes_in_buffer > content_length ? content_length : body_bytes_in_buffer;
        if (out_request->body_length > 0U) {
            memcpy(out_request->body, buffer + header_end + delimiter_length, out_request->body_length);
        }

        while (out_request->body_length < content_length) {
            ssize_t received = recv(client_fd,
                                    out_request->body + out_request->body_length,
                                    content_length - out_request->body_length,
                                    0);

            if (received < 0) {
                if (errno == EINTR) {
                    continue;
                }
                free(buffer);
                http_free_request(out_request);
                set_error(errbuf, errbuf_size, "HTTP ERROR: failed to read request body");
                return HTTP_READ_STATUS_BAD_REQUEST;
            }
            if (received == 0) {
                free(buffer);
                http_free_request(out_request);
                set_error(errbuf, errbuf_size, "HTTP ERROR: incomplete request body");
                return HTTP_READ_STATUS_BAD_REQUEST;
            }

            out_request->body_length += (size_t)received;
        }

        out_request->body[content_length] = '\0';
    }

    free(buffer);
    return HTTP_READ_STATUS_OK;
}

/* HTTP status code에 대응하는 reason phrase를 반환한다. */
const char *http_reason_phrase(int status_code)
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
        case 500:
            return "Internal Server Error";
        case 503:
            return "Service Unavailable";
        default:
            return "OK";
    }
}

/* JSON body와 함께 단일 HTTP 응답을 전송한다. */
int http_send_json_response(int client_fd,
                            int status_code,
                            const char *body,
                            char *errbuf,
                            size_t errbuf_size)
{
    char header[256];
    size_t body_length = body == NULL ? 0U : strlen(body);
    int header_length;

    header_length = snprintf(header, sizeof(header),
                             "HTTP/1.1 %d %s\r\n"
                             "Content-Type: application/json; charset=utf-8\r\n"
                             "Content-Length: %zu\r\n"
                             "Connection: close\r\n"
                             "\r\n",
                             status_code,
                             http_reason_phrase(status_code),
                             body_length);
    if (header_length < 0 || (size_t)header_length >= sizeof(header)) {
        set_error(errbuf, errbuf_size, "HTTP ERROR: failed to format response header");
        return -1;
    }

    if (send_all(client_fd, header, (size_t)header_length) != 0) {
        set_error(errbuf, errbuf_size, "HTTP ERROR: failed to send response header");
        return -1;
    }

    if (body_length > 0U && send_all(client_fd, body, body_length) != 0) {
        set_error(errbuf, errbuf_size, "HTTP ERROR: failed to send response body");
        return -1;
    }

    return 0;
}
