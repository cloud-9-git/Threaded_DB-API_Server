#include "http.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "db_api.h"
#include "time_ms.h"

#define HTTP_HEAD_MAX 8192
#define HTTP_BODY_MAX 8192

typedef struct {
    char method[8];
    char path[128];
    char body[HTTP_BODY_MAX + 1];
    int body_len;
} HttpReq;

static const char *status_text(int code)
{
    switch (code) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 404:
            return "Not Found";
        case 413:
            return "Payload Too Large";
        case 503:
            return "Service Unavailable";
        default:
            return "OK";
    }
}

static void send_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0U;

    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) {
            return;
        }
        sent += (size_t)n;
    }
}

void http_send_json(int fd, int code, const char *body)
{
    char head[512];
    int n;
    size_t len = strlen(body);

    n = snprintf(head, sizeof(head),
                 "HTTP/1.1 %d %s\r\n"
                 "Content-Type: application/json\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "Access-Control-Allow-Methods: GET,POST,OPTIONS\r\n"
                 "Access-Control-Allow-Headers: Content-Type\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 code, status_text(code), len);
    if (n > 0) {
        send_all(fd, head, (size_t)n);
        send_all(fd, body, len);
    }
}

void http_send_html(int fd, int code, const char *body)
{
    char head[512];
    int n;
    size_t len = strlen(body);

    n = snprintf(head, sizeof(head),
                 "HTTP/1.1 %d %s\r\n"
                 "Content-Type: text/html; charset=utf-8\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "Access-Control-Allow-Methods: GET,POST,OPTIONS\r\n"
                 "Access-Control-Allow-Headers: Content-Type\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 code, status_text(code), len);
    if (n > 0) {
        send_all(fd, head, (size_t)n);
        send_all(fd, body, len);
    }
}

static int find_head_end(const char *buf, int len, int *term)
{
    int i;

    for (i = 0; i + 3 < len; ++i) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            *term = i;
            return i + 4;
        }
    }
    for (i = 0; i + 1 < len; ++i) {
        if (buf[i] == '\n' && buf[i + 1] == '\n') {
            *term = i;
            return i + 2;
        }
    }
    return -1;
}

static int content_length(const char *head)
{
    const char *p = head;

    while (*p != '\0') {
        const char *line_end = strchr(p, '\n');
        size_t line_len = line_end == NULL ? strlen(p) : (size_t)(line_end - p);

        if (line_len >= 15U && strncasecmp(p, "Content-Length:", 15U) == 0) {
            const char *v = p + 15;

            while (*v != '\0' && isspace((unsigned char)*v)) {
                v += 1;
            }
            return atoi(v);
        }
        if (line_end == NULL) {
            break;
        }
        p = line_end + 1;
    }
    return 0;
}

static int read_req(int fd, HttpReq *req, int *status)
{
    char buf[HTTP_HEAD_MAX + HTTP_BODY_MAX + 1];
    int used = 0;
    int body_at = -1;
    int term = -1;
    int clen;
    int have;

    memset(req, 0, sizeof(*req));
    *status = 400;
    while (body_at < 0 && used < (int)sizeof(buf) - 1) {
        ssize_t n = recv(fd, buf + used, sizeof(buf) - 1U - (size_t)used, 0);

        if (n <= 0) {
            return 0;
        }
        used += (int)n;
        body_at = find_head_end(buf, used, &term);
        if (body_at < 0 && used > HTTP_HEAD_MAX) {
            *status = 413;
            return 0;
        }
    }
    if (body_at < 0) {
        *status = 400;
        return 0;
    }

    buf[used] = '\0';
    buf[term] = '\0';
    if (sscanf(buf, "%7s %127s", req->method, req->path) != 2) {
        return 0;
    }
    clen = content_length(buf);
    if (clen < 0 || clen > HTTP_BODY_MAX) {
        *status = 413;
        return 0;
    }

    have = used - body_at;
    while (have < clen) {
        ssize_t n = recv(fd, buf + used, sizeof(buf) - 1U - (size_t)used, 0);

        if (n <= 0) {
            return 0;
        }
        used += (int)n;
        have += (int)n;
    }
    if (clen > 0) {
        memcpy(req->body, buf + body_at, (size_t)clen);
    }
    req->body[clen] = '\0';
    req->body_len = clen;
    *status = 200;
    return 1;
}

static const char *find_json_key(const char *body, const char *key)
{
    char needle[64];
    const char *p;

    snprintf(needle, sizeof(needle), "\"%s\"", key);
    p = strstr(body, needle);
    if (p == NULL) {
        return NULL;
    }
    p += strlen(needle);
    while (*p != '\0' && isspace((unsigned char)*p)) {
        p += 1;
    }
    if (*p != ':') {
        return NULL;
    }
    p += 1;
    while (*p != '\0' && isspace((unsigned char)*p)) {
        p += 1;
    }
    return p;
}

static int json_get_str(const char *body, const char *key, char *out, size_t max)
{
    const char *p = find_json_key(body, key);
    size_t len = 0U;

    if (p == NULL || *p != '"') {
        return -1;
    }
    p += 1;
    while (*p != '\0') {
        char ch = *p++;

        if (ch == '"') {
            out[len] = '\0';
            return 1;
        }
        if (ch == '\\') {
            ch = *p++;
            if (ch == '\0') {
                return -1;
            }
            if (ch == 'n') {
                ch = '\n';
            } else if (ch == 'r') {
                ch = '\r';
            } else if (ch == 't') {
                ch = '\t';
            }
        }
        if (len + 1U >= max) {
            return -2;
        }
        out[len++] = ch;
    }
    return -1;
}

static int json_get_long(const char *body, const char *key, long *out)
{
    const char *p = find_json_key(body, key);
    char *end;
    long val;

    if (p == NULL) {
        return 0;
    }
    val = strtol(p, &end, 10);
    if (end == p) {
        return 0;
    }
    *out = val;
    return 1;
}

static char *read_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    long size;
    char *buf;

    if (fp == NULL) {
        return NULL;
    }
    if (fseek(fp, 0L, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    size = ftell(fp);
    if (size < 0L) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    buf = (char *)malloc((size_t)size + 1U);
    if (buf == NULL) {
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1U, (size_t)size, fp) != (size_t)size) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    buf[size] = '\0';
    fclose(fp);
    return buf;
}

static void handle_direct_get(int fd, Pool *pool, const HttpReq *req)
{
    if (strcmp(req->path, "/health") == 0) {
        http_send_json(fd, 200, "{\"ok\":true}");
    } else if (strcmp(req->path, "/stats") == 0) {
        char body[256];

        pool_stats_json(pool, body, sizeof(body));
        http_send_json(fd, 200, body);
    } else if (strcmp(req->path, "/chart") == 0) {
        char *html = read_file("bench/chart.html");

        if (html == NULL) {
            http_send_html(fd, 404, "<!doctype html><title>not found</title><p>chart not found</p>");
        } else {
            http_send_html(fd, 200, html);
            free(html);
        }
    } else if (strcmp(req->path, "/") == 0) {
        http_send_html(fd, 200,
                       "<!doctype html><title>Threaded DB</title>"
                       "<h1>Threaded DB API Server</h1>"
                       "<p>GET /health, GET /stats, POST /sql, POST /bench, GET /chart</p>");
    } else {
        http_send_json(fd, 404, "{\"ok\":false,\"err\":\"not found\"}");
    }
    close(fd);
}

static void handle_req(int fd, Pool *pool, const HttpReq *req)
{
    if (strcmp(req->method, "OPTIONS") == 0) {
        http_send_json(fd, 200, "{\"ok\":true}");
        close(fd);
        return;
    }
    if (strcmp(req->method, "GET") == 0) {
        handle_direct_get(fd, pool, req);
        return;
    }
    if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/sql") == 0) {
        char sql[DB_SQL_MAX];
        int got = json_get_str(req->body, "sql", sql, sizeof(sql));

        if (req->body_len == 0 || got == -1) {
            http_send_json(fd, 400, "{\"ok\":false,\"err\":\"bad json\"}");
            close(fd);
            return;
        }
        if (got == -2) {
            http_send_json(fd, 413, "{\"ok\":false,\"err\":\"sql too long\"}");
            close(fd);
            return;
        }
        if (!pool_submit_sql(pool, fd, sql, now_ms())) {
            http_send_json(fd, 503, "{\"ok\":false,\"err\":\"queue full\"}");
            close(fd);
        }
        return;
    }
    if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/bench") == 0) {
        char mode[16];
        long count = 0L;

        if (json_get_str(req->body, "mode", mode, sizeof(mode)) != 1 ||
            !json_get_long(req->body, "count", &count)) {
            http_send_json(fd, 400, "{\"ok\":false,\"err\":\"bad json\"}");
            close(fd);
            return;
        }
        if (!pool_submit_bench(pool, fd, mode, count, now_ms())) {
            http_send_json(fd, 503, "{\"ok\":false,\"err\":\"queue full\"}");
            close(fd);
        }
        return;
    }
    http_send_json(fd, 404, "{\"ok\":false,\"err\":\"not found\"}");
    close(fd);
}

int http_run(int port, Pool *pool)
{
    int sfd;
    int yes = 1;
    struct sockaddr_in addr;

    sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) {
        perror("socket");
        return 1;
    }
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(sfd);
        return 1;
    }
    if (listen(sfd, 128) != 0) {
        perror("listen");
        close(sfd);
        return 1;
    }

    printf("server listening on port %d\n", port);
    fflush(stdout);
    for (;;) {
        int cfd = accept(sfd, NULL, NULL);
        HttpReq req;
        int status = 400;

        if (cfd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }
        if (!read_req(cfd, &req, &status)) {
            http_send_json(cfd, status, "{\"ok\":false,\"err\":\"bad request\"}");
            close(cfd);
            continue;
        }
        handle_req(cfd, pool, &req);
    }
    close(sfd);
    return 1;
}
