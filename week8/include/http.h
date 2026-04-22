#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

typedef struct {
    char method[16];
    char path[256];
    char *body;
    size_t body_length;
} HttpRequest;

typedef enum {
    HTTP_READ_OK = 0,
    HTTP_READ_BAD_REQUEST = 400,
    HTTP_READ_BODY_TOO_LARGE = 413,
    HTTP_READ_INTERNAL_ERROR = 500
} HttpReadStatus;

HttpReadStatus http_read_request(int client_fd, size_t max_body_size, HttpRequest *out_request);
void http_request_free(HttpRequest *request);
int http_send_json(int client_fd, int status_code, const char *json_body);
int http_send_error(int client_fd, int status_code, const char *error_code, const char *message);

#endif
