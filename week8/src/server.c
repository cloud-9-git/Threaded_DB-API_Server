#include "server.h"

#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "http.h"
#include "json_parser.h"

#define DEFAULT_PORT 8080
#define DEFAULT_WORKERS 4U
#define DEFAULT_QUEUE_CAPACITY 64U
#define DEFAULT_BODY_MAX 8192U

static void set_error(char *errbuf, size_t errbuf_size, const char *message)
{
    if (errbuf != NULL && errbuf_size > 0U) {
        snprintf(errbuf, errbuf_size, "%s", message);
    }
}

static void close_fd(int *fd)
{
    if (fd != NULL && *fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void handle_health(int client_fd, const HttpRequest *request)
{
    if (strcmp(request->method, "GET") != 0) {
        http_send_error(client_fd, 405, "INTERNAL_ERROR", "method not allowed");
        return;
    }

    http_send_json(client_fd, 200, "{\"success\":true,\"service\":\"mini_db_server\"}");
}

static void handle_query(int client_fd, Server *server, const HttpRequest *request)
{
    char *sql = NULL;
    char errbuf[256] = {0};
    JsonSqlStatus json_status;
    DbApiResult api_result = {0};

    if (strcmp(request->method, "POST") != 0) {
        http_send_error(client_fd, 405, "INTERNAL_ERROR", "method not allowed");
        return;
    }

    json_status = json_extract_sql(request->body, &sql, errbuf, sizeof(errbuf));
    if (json_status == JSON_SQL_INVALID) {
        http_send_error(client_fd, 400, "INVALID_JSON", errbuf);
        return;
    }
    if (json_status == JSON_SQL_MISSING) {
        http_send_error(client_fd, 400, "MISSING_SQL_FIELD", errbuf);
        return;
    }

    if (!db_api_execute_sql(&server->db_api, sql, &api_result)) {
        http_send_error(client_fd, 500, "INTERNAL_ERROR", "failed to execute SQL");
        free(sql);
        return;
    }

    http_send_json(client_fd, api_result.http_status, api_result.json_body);
    db_api_result_free(&api_result);
    free(sql);
}

static void handle_client_task(Task task, void *user_data)
{
    Server *server = (Server *)user_data;
    HttpRequest request = {0};
    HttpReadStatus read_status;

    read_status = http_read_request(task.client_fd, server->config.max_body_size, &request);
    if (read_status == HTTP_READ_BODY_TOO_LARGE) {
        http_send_error(task.client_fd, 413, "INVALID_JSON", "request body exceeds limit");
        close(task.client_fd);
        return;
    }
    if (read_status == HTTP_READ_BAD_REQUEST) {
        http_send_error(task.client_fd, 400, "INVALID_JSON", "malformed HTTP request");
        close(task.client_fd);
        return;
    }
    if (read_status != HTTP_READ_OK) {
        http_send_error(task.client_fd, 500, "INTERNAL_ERROR", "failed to read HTTP request");
        close(task.client_fd);
        return;
    }

    if (strcmp(request.path, "/health") == 0) {
        handle_health(task.client_fd, &request);
    } else if (strcmp(request.path, "/query") == 0) {
        handle_query(task.client_fd, server, &request);
    } else {
        http_send_error(task.client_fd, 404, "INTERNAL_ERROR", "not found");
    }

    http_request_free(&request);
    close(task.client_fd);
}

static int create_listen_socket(int port, char *errbuf, size_t errbuf_size)
{
    int listen_fd;
    int opt = 1;
    struct sockaddr_in addr;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        set_error(errbuf, errbuf_size, "failed to create socket");
        return -1;
    }

    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
        close(listen_fd);
        set_error(errbuf, errbuf_size, "failed to set SO_REUSEADDR");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7f000001UL);
    addr.sin_port = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        char message[256];

        close(listen_fd);
        snprintf(message, sizeof(message), "failed to bind listen socket: %s", strerror(errno));
        set_error(errbuf, errbuf_size, message);
        return -1;
    }

    if (listen(listen_fd, 128) != 0) {
        close(listen_fd);
        set_error(errbuf, errbuf_size, "failed to listen");
        return -1;
    }

    return listen_fd;
}

void server_config_init(ServerConfig *config)
{
    if (config == NULL) {
        return;
    }

    config->db_dir = NULL;
    config->port = DEFAULT_PORT;
    config->worker_count = DEFAULT_WORKERS;
    config->queue_capacity = DEFAULT_QUEUE_CAPACITY;
    config->max_body_size = DEFAULT_BODY_MAX;
}

int server_init(Server *server, const ServerConfig *config, char *errbuf, size_t errbuf_size)
{
    int status;

    if (server == NULL || config == NULL || config->db_dir == NULL ||
        config->worker_count == 0U || config->queue_capacity == 0U || config->port <= 0) {
        set_error(errbuf, errbuf_size, "invalid server configuration");
        return 0;
    }

    memset(server, 0, sizeof(*server));
    server->listen_fd = -1;
    server->config = *config;

    status = db_api_init(&server->db_api, config->db_dir, errbuf, errbuf_size);
    if (status != 0) {
        return 0;
    }

    if (!thread_pool_init(&server->pool,
                          config->worker_count,
                          config->queue_capacity,
                          handle_client_task,
                          server)) {
        db_api_destroy(&server->db_api);
        set_error(errbuf, errbuf_size, "failed to initialize thread pool");
        return 0;
    }

    server->listen_fd = create_listen_socket(config->port, errbuf, errbuf_size);
    if (server->listen_fd < 0) {
        thread_pool_destroy(&server->pool);
        db_api_destroy(&server->db_api);
        return 0;
    }

    server->initialized = 1;
    return 1;
}

int server_run(Server *server)
{
    if (server == NULL || !server->initialized) {
        return 1;
    }

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = (socklen_t)sizeof(client_addr);
        int client_fd;
        Task task;
        int push_status;

        client_fd = accept(server->listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 1;
        }

        task.client_fd = client_fd;
        push_status = thread_pool_submit(&server->pool, task);
        if (push_status <= 0) {
            http_send_error(client_fd, 503, "QUEUE_FULL", "request queue is full");
            close(client_fd);
        }
    }
}

void server_destroy(Server *server)
{
    if (server == NULL || !server->initialized) {
        return;
    }

    close_fd(&server->listen_fd);
    thread_pool_destroy(&server->pool);
    db_api_destroy(&server->db_api);
    server->initialized = 0;
}
