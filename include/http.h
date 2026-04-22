#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

typedef struct {
    char *method;
    char *path;
    char *body;
    size_t body_length;
    size_t content_length;
} HttpRequest;

typedef enum {
    HTTP_READ_STATUS_OK = 0,
    HTTP_READ_STATUS_BAD_REQUEST = 1,
    HTTP_READ_STATUS_TOO_LARGE = 2,
    HTTP_READ_STATUS_INTERNAL_ERROR = 3
} HttpReadStatus;

HttpReadStatus http_read_request(int client_fd,
                                 size_t body_max_bytes,
                                 HttpRequest *out_request,
                                 char *errbuf,
                                 size_t errbuf_size);
void http_free_request(HttpRequest *request);
int http_send_json_response(int client_fd,
                            int status_code,
                            const char *body,
                            char *errbuf,
                            size_t errbuf_size);
const char *http_reason_phrase(int status_code);

#endif
