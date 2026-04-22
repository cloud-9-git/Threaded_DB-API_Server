#ifndef HTTP_H
#define HTTP_H

#include "pool.h"

int http_run(int port, Pool *pool);
void http_send_json(int fd, int code, const char *body);
void http_send_html(int fd, int code, const char *body);

#endif
