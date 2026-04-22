#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "errors.h"
#include "http.h"
#include "json_parser.h"
#include "json_writer.h"
#include "utils.h"

static volatile sig_atomic_t g_stop_requested = 0;

/* server 계층 오류 메시지를 errbuf에 기록한다. */
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

/* SIGINT/SIGTERM 수신 시 accept loop를 종료하도록 플래그만 세운다. */
static void handle_stop_signal(int signum)
{
    (void)signum;
    g_stop_requested = 1;
}

/* 서버가 필요한 signal handler를 설치한다. */
static int install_signal_handlers(char *errbuf, size_t errbuf_size)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_stop_signal;
    sigemptyset(&action.sa_mask);

    if (sigaction(SIGINT, &action, NULL) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0) {
        set_error(errbuf, errbuf_size, "SERVER ERROR: failed to install signal handlers");
        return -1;
    }

    signal(SIGPIPE, SIG_IGN);
    return 0;
}

/* fd를 non-blocking listen socket으로 설정한다. */
static int set_nonblocking(int fd, char *errbuf, size_t errbuf_size)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        set_error(errbuf, errbuf_size, "SERVER ERROR: failed to set non-blocking socket");
        return -1;
    }

    return 0;
}

/* 클라이언트 연결 소켓에 macOS SIGPIPE 방지 옵션을 적용한다. */
static void configure_client_socket(int client_fd)
{
    int flags = fcntl(client_fd, F_GETFL, 0);

    if (flags >= 0) {
        fcntl(client_fd, F_SETFL, flags & ~O_NONBLOCK);
    }

#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void)client_fd;
#endif
}

/* 공통 에러 JSON 문자열을 생성한다. */
static char *build_error_json_literal(const char *error_code,
                                      const char *message,
                                      char *errbuf,
                                      size_t errbuf_size)
{
    JsonWriter writer;

    if (json_writer_init(&writer, errbuf, errbuf_size) != 0) {
        return NULL;
    }

    if (json_writer_append_raw(&writer, "{\"success\":false,\"error_code\":", errbuf, errbuf_size) != 0 ||
        json_writer_append_json_string(&writer, error_code, errbuf, errbuf_size) != 0 ||
        json_writer_append_raw(&writer, ",\"message\":", errbuf, errbuf_size) != 0 ||
        json_writer_append_json_string(&writer, message, errbuf, errbuf_size) != 0 ||
        json_writer_append_char(&writer, '}', errbuf, errbuf_size) != 0) {
        json_writer_destroy(&writer);
        return NULL;
    }

    return json_writer_take_string(&writer);
}

/* 명세된 /health 응답 JSON을 생성한다. */
static char *build_health_json(char *errbuf, size_t errbuf_size)
{
    JsonWriter writer;

    if (json_writer_init(&writer, errbuf, errbuf_size) != 0) {
        return NULL;
    }

    if (json_writer_append_raw(&writer, "{\"success\":true,\"service\":\"mini_db_server\"}",
                               errbuf, errbuf_size) != 0) {
        json_writer_destroy(&writer);
        return NULL;
    }

    return json_writer_take_string(&writer);
}

/* 최소 보장 가능한 fallback 에러 JSON을 반환한다. */
static const char *fallback_internal_error_json(void)
{
    return "{\"success\":false,\"error_code\":\"INTERNAL_ERROR\",\"message\":\"internal error\"}";
}

/* status/message에 맞는 JSON 에러 응답을 전송한다. */
static void send_error_response(int client_fd,
                                int http_status,
                                const char *error_code,
                                const char *message)
{
    char errbuf[256] = {0};
    char *json_body = build_error_json_literal(error_code, message, errbuf, sizeof(errbuf));
    const char *body = json_body == NULL ? fallback_internal_error_json() : json_body;

    http_send_json_response(client_fd, http_status, body, errbuf, sizeof(errbuf));
    free(json_body);
}

/* worker가 단일 연결을 읽고 /health 또는 /query를 처리한다. */
static void server_handle_client_task(ClientTask task, void *user_data)
{
    Server *server = (Server *)user_data;
    HttpRequest request = {0};
    char errbuf[256] = {0};
    char *json_body = NULL;
    char *sql_text = NULL;
    int http_status = 500;
    HttpReadStatus read_status;

    read_status = http_read_request(task.client_fd,
                                    server->config.body_max_bytes,
                                    &request,
                                    errbuf,
                                    sizeof(errbuf));
    if (read_status == HTTP_READ_STATUS_TOO_LARGE) {
        send_error_response(task.client_fd, 413, "INTERNAL_ERROR",
                            errbuf[0] != '\0' ? errbuf : "request body exceeds limit");
        goto cleanup;
    }
    if (read_status == HTTP_READ_STATUS_BAD_REQUEST) {
        send_error_response(task.client_fd, 400, "INVALID_JSON",
                            errbuf[0] != '\0' ? errbuf : "invalid HTTP request");
        goto cleanup;
    }
    if (read_status != HTTP_READ_STATUS_OK) {
        send_error_response(task.client_fd, 500, "INTERNAL_ERROR", "internal error");
        goto cleanup;
    }

    if (strcmp(request.path, "/health") == 0) {
        if (strcmp(request.method, "GET") != 0) {
            send_error_response(task.client_fd, 405, "INTERNAL_ERROR", "method not allowed");
            goto cleanup;
        }

        json_body = build_health_json(errbuf, sizeof(errbuf));
        if (json_body == NULL) {
            send_error_response(task.client_fd, 500, "INTERNAL_ERROR", "internal error");
            goto cleanup;
        }

        http_send_json_response(task.client_fd, 200, json_body, errbuf, sizeof(errbuf));
        goto cleanup;
    }

    if (strcmp(request.path, "/query") == 0) {
        JsonSqlStatus json_status;

        if (strcmp(request.method, "POST") != 0) {
            send_error_response(task.client_fd, 405, "INTERNAL_ERROR", "method not allowed");
            goto cleanup;
        }

        json_status = json_extract_sql_field(request.body, &sql_text, errbuf, sizeof(errbuf));
        if (json_status == JSON_SQL_STATUS_INVALID) {
            json_body = db_api_build_error_json(DB_API_INVALID_JSON, errbuf, errbuf, sizeof(errbuf));
            if (json_body == NULL) {
                send_error_response(task.client_fd, 500, "INTERNAL_ERROR", "internal error");
                goto cleanup;
            }
            http_send_json_response(task.client_fd, 400, json_body, errbuf, sizeof(errbuf));
            goto cleanup;
        }
        if (json_status == JSON_SQL_STATUS_MISSING) {
            json_body = db_api_build_error_json(DB_API_MISSING_SQL_FIELD, errbuf, errbuf, sizeof(errbuf));
            if (json_body == NULL) {
                send_error_response(task.client_fd, 500, "INTERNAL_ERROR", "internal error");
                goto cleanup;
            }
            http_send_json_response(task.client_fd, 400, json_body, errbuf, sizeof(errbuf));
            goto cleanup;
        }

        db_api_execute_sql(&server->db_api,
                           sql_text,
                           &json_body,
                           &http_status,
                           errbuf,
                           sizeof(errbuf));
        if (json_body == NULL) {
            send_error_response(task.client_fd, 500, "INTERNAL_ERROR", "internal error");
            goto cleanup;
        }

        http_send_json_response(task.client_fd, http_status, json_body, errbuf, sizeof(errbuf));
        goto cleanup;
    }

    send_error_response(task.client_fd, 404, "INTERNAL_ERROR", "path not found");

cleanup:
    free(json_body);
    free(sql_text);
    http_free_request(&request);
    close(task.client_fd);
}

/* listen socket, DB API, queue, thread pool을 한 번에 초기화한다. */
int server_init(Server *server,
                const ServerConfig *config,
                char *errbuf,
                size_t errbuf_size)
{
    struct sockaddr_in address;
    int reuse_addr = 1;
    int backlog;

    if (server == NULL || config == NULL || config->db_dir == NULL ||
        config->worker_count == 0U || config->queue_size == 0U) {
        set_error(errbuf, errbuf_size, "SERVER ERROR: invalid server configuration");
        return STATUS_INVALID_ARGS;
    }

    memset(server, 0, sizeof(*server));
    server->listen_fd = -1;
    server->config = *config;

    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd < 0) {
        set_error(errbuf, errbuf_size, "SERVER ERROR: failed to create listen socket");
        return STATUS_EXEC_ERROR;
    }

    if (setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr)) != 0) {
        set_error(errbuf, errbuf_size, "SERVER ERROR: failed to set SO_REUSEADDR");
        server_destroy(server);
        return STATUS_EXEC_ERROR;
    }

    if (set_nonblocking(server->listen_fd, errbuf, errbuf_size) != 0) {
        server_destroy(server);
        return STATUS_EXEC_ERROR;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)config->port);

    if (bind(server->listen_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        set_error(errbuf, errbuf_size, "SERVER ERROR: failed to bind listen socket");
        server_destroy(server);
        return STATUS_EXEC_ERROR;
    }

    backlog = config->queue_size > (size_t)INT_MAX ? INT_MAX : (int)config->queue_size;
    if (listen(server->listen_fd, backlog) != 0) {
        set_error(errbuf, errbuf_size, "SERVER ERROR: failed to listen on socket");
        server_destroy(server);
        return STATUS_EXEC_ERROR;
    }

    if (db_api_init(&server->db_api, config->db_dir, errbuf, errbuf_size) != STATUS_OK) {
        server_destroy(server);
        return STATUS_EXEC_ERROR;
    }

    if (task_queue_init(&server->queue, config->queue_size, errbuf, errbuf_size) != 0) {
        server_destroy(server);
        return STATUS_EXEC_ERROR;
    }

    if (thread_pool_init(&server->thread_pool,
                         config->worker_count,
                         &server->queue,
                         server_handle_client_task,
                         server,
                         errbuf,
                         errbuf_size) != 0) {
        server_destroy(server);
        return STATUS_EXEC_ERROR;
    }

    return STATUS_OK;
}

/* accept loop를 돌며 들어오는 연결을 queue에 넣거나 즉시 503으로 거절한다. */
int server_run(Server *server, char *errbuf, size_t errbuf_size)
{
    if (server == NULL || server->listen_fd < 0) {
        set_error(errbuf, errbuf_size, "SERVER ERROR: invalid server state");
        return STATUS_EXEC_ERROR;
    }

    if (install_signal_handlers(errbuf, errbuf_size) != 0) {
        return STATUS_EXEC_ERROR;
    }

    g_stop_requested = 0;
    while (!g_stop_requested) {
        fd_set readfds;
        struct timeval timeout;
        int ready;

        FD_ZERO(&readfds);
        FD_SET(server->listen_fd, &readfds);
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        ready = select(server->listen_fd + 1, &readfds, NULL, NULL, &timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            set_error(errbuf, errbuf_size, "SERVER ERROR: select failed");
            return STATUS_EXEC_ERROR;
        }

        if (ready == 0 || !FD_ISSET(server->listen_fd, &readfds)) {
            continue;
        }

        for (;;) {
            int client_fd = accept(server->listen_fd, NULL, NULL);
            TaskQueueStatus queue_status;

            if (client_fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                set_error(errbuf, errbuf_size, "SERVER ERROR: accept failed");
                return STATUS_EXEC_ERROR;
            }

            configure_client_socket(client_fd);
            queue_status = task_queue_try_push(&server->queue, (ClientTask){client_fd});
            if (queue_status == TASK_QUEUE_STATUS_OK) {
                continue;
            }

            if (queue_status == TASK_QUEUE_STATUS_FULL) {
                send_error_response(client_fd, 503,
                                    db_api_status_to_error_code(DB_API_QUEUE_FULL),
                                    "request queue is full");
            }
            close(client_fd);
        }
    }

    return STATUS_OK;
}

/* 서버가 소유한 listen socket, pool, queue, db api를 정리한다. */
void server_destroy(Server *server)
{
    if (server == NULL) {
        return;
    }

    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
    }

    thread_pool_destroy(&server->thread_pool);
    task_queue_destroy(&server->queue);
    db_api_destroy(&server->db_api);
}
