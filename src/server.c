#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
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

typedef struct {
    Server *server;
    int client_fd;
} RequestThreadArgs;

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
static char *build_health_json(const char *service_name, char *errbuf, size_t errbuf_size)
{
    JsonWriter writer;
    const char *resolved_service_name = service_name == NULL ? "mini_db_server" : service_name;

    if (json_writer_init(&writer, errbuf, errbuf_size) != 0) {
        return NULL;
    }

    if (json_writer_append_raw(&writer, "{\"success\":true,\"service\":",
                               errbuf, errbuf_size) != 0 ||
        json_writer_append_json_string(&writer, resolved_service_name, errbuf, errbuf_size) != 0 ||
        json_writer_append_char(&writer, '}', errbuf, errbuf_size) != 0) {
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
static void server_handle_client_connection(Server *server, int client_fd)
{
    HttpRequest request = {0};
    char errbuf[256] = {0};
    char *json_body = NULL;
    char *sql_text = NULL;
    int http_status = 500;
    HttpReadStatus read_status;

    read_status = http_read_request(client_fd,
                                    server->config.body_max_bytes,
                                    &request,
                                    errbuf,
                                    sizeof(errbuf));
    if (read_status == HTTP_READ_STATUS_TOO_LARGE) {
        send_error_response(client_fd, 413, "INTERNAL_ERROR",
                            errbuf[0] != '\0' ? errbuf : "request body exceeds limit");
        goto cleanup;
    }
    if (read_status == HTTP_READ_STATUS_BAD_REQUEST) {
        send_error_response(client_fd, 400, "INVALID_JSON",
                            errbuf[0] != '\0' ? errbuf : "invalid HTTP request");
        goto cleanup;
    }
    if (read_status != HTTP_READ_STATUS_OK) {
        send_error_response(client_fd, 500, "INTERNAL_ERROR", "internal error");
        goto cleanup;
    }

    if (strcmp(request.path, "/health") == 0) {
        if (strcmp(request.method, "GET") != 0) {
            send_error_response(client_fd, 405, "INTERNAL_ERROR", "method not allowed");
            goto cleanup;
        }

        json_body = build_health_json(server->config.service_name, errbuf, sizeof(errbuf));
        if (json_body == NULL) {
            send_error_response(client_fd, 500, "INTERNAL_ERROR", "internal error");
            goto cleanup;
        }

        http_send_json_response(client_fd, 200, json_body, errbuf, sizeof(errbuf));
        goto cleanup;
    }

    if (strcmp(request.path, "/query") == 0) {
        JsonSqlStatus json_status;

        if (strcmp(request.method, "POST") != 0) {
            send_error_response(client_fd, 405, "INTERNAL_ERROR", "method not allowed");
            goto cleanup;
        }

        json_status = json_extract_sql_field(request.body, &sql_text, errbuf, sizeof(errbuf));
        if (json_status == JSON_SQL_STATUS_INVALID) {
            json_body = db_api_build_error_json(DB_API_INVALID_JSON, errbuf, errbuf, sizeof(errbuf));
            if (json_body == NULL) {
                send_error_response(client_fd, 500, "INTERNAL_ERROR", "internal error");
                goto cleanup;
            }
            http_send_json_response(client_fd, 400, json_body, errbuf, sizeof(errbuf));
            goto cleanup;
        }
        if (json_status == JSON_SQL_STATUS_MISSING) {
            json_body = db_api_build_error_json(DB_API_MISSING_SQL_FIELD, errbuf, errbuf, sizeof(errbuf));
            if (json_body == NULL) {
                send_error_response(client_fd, 500, "INTERNAL_ERROR", "internal error");
                goto cleanup;
            }
            http_send_json_response(client_fd, 400, json_body, errbuf, sizeof(errbuf));
            goto cleanup;
        }

        db_api_execute_sql(&server->db_api,
                           sql_text,
                           &json_body,
                           &http_status,
                           errbuf,
                           sizeof(errbuf));
        if (json_body == NULL) {
            send_error_response(client_fd, 500, "INTERNAL_ERROR", "internal error");
            goto cleanup;
        }

        http_send_json_response(client_fd, http_status, json_body, errbuf, sizeof(errbuf));
        goto cleanup;
    }

    send_error_response(client_fd, 404, "INTERNAL_ERROR", "path not found");

cleanup:
    free(json_body);
    free(sql_text);
    http_free_request(&request);
    close(client_fd);
}

/* thread pool worker adapter다. */
static void server_handle_client_task(ClientTask task, void *user_data)
{
    Server *server = (Server *)user_data;

    server_handle_client_connection(server, task.client_fd);
}

/* thread-per-request에서 동시에 허용할 최대 요청 수를 계산한다. */
static size_t server_thread_per_request_capacity(const Server *server)
{
    size_t capacity = server->config.worker_count;

    if (SIZE_MAX - capacity < server->config.queue_size) {
        return SIZE_MAX;
    }

    return capacity + server->config.queue_size;
}

/* thread-per-request 동시 처리 카운터를 증가시키고 상한 초과 시 실패를 돌려준다. */
static int server_begin_active_request(Server *server)
{
    int status = 0;

    if (!server->active_sync_initialized) {
        return -1;
    }

    if (pthread_mutex_lock(&server->active_mutex) != 0) {
        return -1;
    }

    if (server->active_request_count >= server_thread_per_request_capacity(server)) {
        status = -1;
    } else {
        server->active_request_count += 1U;
    }

    pthread_mutex_unlock(&server->active_mutex);
    return status;
}

/* thread-per-request 동시 처리 카운터를 감소시키고 0이면 대기자를 깨운다. */
static void server_finish_active_request(Server *server)
{
    if (!server->active_sync_initialized) {
        return;
    }

    if (pthread_mutex_lock(&server->active_mutex) != 0) {
        return;
    }

    if (server->active_request_count > 0U) {
        server->active_request_count -= 1U;
    }

    if (server->active_request_count == 0U) {
        pthread_cond_broadcast(&server->active_zero_cond);
    }

    pthread_mutex_unlock(&server->active_mutex);
}

/* 남아 있는 thread-per-request 요청 스레드가 모두 끝날 때까지 기다린다. */
static void server_wait_for_active_requests(Server *server)
{
    if (!server->active_sync_initialized) {
        return;
    }

    if (pthread_mutex_lock(&server->active_mutex) != 0) {
        return;
    }

    while (server->active_request_count > 0U) {
        if (pthread_cond_wait(&server->active_zero_cond, &server->active_mutex) != 0) {
            break;
        }
    }

    pthread_mutex_unlock(&server->active_mutex);
}

/* accept 대상 listen socket이 준비되거나 종료 시그널이 올 때까지 기다린다. */
static int server_wait_until_ready(Server *server, char *errbuf, size_t errbuf_size)
{
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
            return -1;
        }

        if (ready == 0 || !FD_ISSET(server->listen_fd, &readfds)) {
            continue;
        }

        return 1;
    }

    return 0;
}

/* non-blocking listen socket에서 pending client fd를 하나 받아온다. */
static int server_accept_client(Server *server, int *out_client_fd, char *errbuf, size_t errbuf_size)
{
    int client_fd;

    if (out_client_fd == NULL) {
        set_error(errbuf, errbuf_size, "SERVER ERROR: invalid accept target");
        return -1;
    }

    client_fd = accept(server->listen_fd, NULL, NULL);
    if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        if (errno == EINTR) {
            return 1;
        }
        set_error(errbuf, errbuf_size, "SERVER ERROR: accept failed");
        return -1;
    }

    configure_client_socket(client_fd);
    *out_client_fd = client_fd;
    return 2;
}

/* thread-per-request용 요청 스레드 본문이다. */
static void *server_thread_per_request_main(void *arg)
{
    RequestThreadArgs *request = (RequestThreadArgs *)arg;

    server_handle_client_connection(request->server, request->client_fd);
    server_finish_active_request(request->server);
    free(request);
    return NULL;
}

/* accept한 요청을 thread pool queue에 넣거나 즉시 503으로 거절한다. */
static int server_run_thread_pool(Server *server, char *errbuf, size_t errbuf_size)
{
    for (;;) {
        int wait_status = server_wait_until_ready(server, errbuf, errbuf_size);

        if (wait_status < 0) {
            return STATUS_EXEC_ERROR;
        }
        if (wait_status == 0) {
            return STATUS_OK;
        }

        for (;;) {
            int client_fd = -1;
            int accept_status = server_accept_client(server, &client_fd, errbuf, errbuf_size);
            TaskQueueStatus queue_status;

            if (accept_status < 0) {
                return STATUS_EXEC_ERROR;
            }
            if (accept_status == 0) {
                break;
            }
            if (accept_status == 1) {
                continue;
            }

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
}

/* 메인 스레드가 요청을 하나씩 순서대로 처리하는 직렬 모드다. */
static int server_run_serial(Server *server, char *errbuf, size_t errbuf_size)
{
    for (;;) {
        int wait_status = server_wait_until_ready(server, errbuf, errbuf_size);

        if (wait_status < 0) {
            return STATUS_EXEC_ERROR;
        }
        if (wait_status == 0) {
            return STATUS_OK;
        }

        for (;;) {
            int client_fd = -1;
            int accept_status = server_accept_client(server, &client_fd, errbuf, errbuf_size);

            if (accept_status < 0) {
                return STATUS_EXEC_ERROR;
            }
            if (accept_status == 0) {
                break;
            }
            if (accept_status == 1) {
                continue;
            }

            server_handle_client_connection(server, client_fd);
        }
    }
}

/* 요청마다 새 스레드를 만들어 처리하는 thread-per-request 모드다. */
static int server_run_thread_per_request(Server *server, char *errbuf, size_t errbuf_size)
{
    for (;;) {
        int wait_status = server_wait_until_ready(server, errbuf, errbuf_size);

        if (wait_status < 0) {
            return STATUS_EXEC_ERROR;
        }
        if (wait_status == 0) {
            server_wait_for_active_requests(server);
            return STATUS_OK;
        }

        for (;;) {
            int client_fd = -1;
            int accept_status = server_accept_client(server, &client_fd, errbuf, errbuf_size);

            if (accept_status < 0) {
                server_wait_for_active_requests(server);
                return STATUS_EXEC_ERROR;
            }
            if (accept_status == 0) {
                break;
            }
            if (accept_status == 1) {
                continue;
            }

            if (server_begin_active_request(server) != 0) {
                send_error_response(client_fd, 503,
                                    db_api_status_to_error_code(DB_API_QUEUE_FULL),
                                    "request queue is full");
                close(client_fd);
                continue;
            }

            RequestThreadArgs *request = (RequestThreadArgs *)malloc(sizeof(*request));
            pthread_t thread;

            if (request == NULL) {
                server_finish_active_request(server);
                send_error_response(client_fd, 500, "INTERNAL_ERROR", "internal error");
                close(client_fd);
                continue;
            }

            request->server = server;
            request->client_fd = client_fd;

            if (pthread_create(&thread, NULL, server_thread_per_request_main, request) != 0) {
                free(request);
                server_finish_active_request(server);
                send_error_response(client_fd, 500, "INTERNAL_ERROR", "internal error");
                close(client_fd);
                continue;
            }

            pthread_detach(thread);
        }
    }
}

/* thread-per-request용 동기화 객체를 준비한다. */
static int server_init_active_sync(Server *server, char *errbuf, size_t errbuf_size)
{
    if (pthread_mutex_init(&server->active_mutex, NULL) != 0) {
        set_error(errbuf, errbuf_size, "SERVER ERROR: failed to initialize active request mutex");
        return -1;
    }

    if (pthread_cond_init(&server->active_zero_cond, NULL) != 0) {
        pthread_mutex_destroy(&server->active_mutex);
        memset(&server->active_mutex, 0, sizeof(server->active_mutex));
        set_error(errbuf, errbuf_size, "SERVER ERROR: failed to initialize active request condition");
        return -1;
    }

    server->active_sync_initialized = 1;
    return 0;
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
    const char *service_name;

    if (server == NULL || config == NULL || config->db_dir == NULL ||
        config->worker_count == 0U || config->queue_size == 0U) {
        set_error(errbuf, errbuf_size, "SERVER ERROR: invalid server configuration");
        return STATUS_INVALID_ARGS;
    }

    memset(server, 0, sizeof(*server));
    server->listen_fd = -1;
    server->config = *config;
    service_name = config->service_name == NULL ? "mini_db_server" : config->service_name;
    server->config.service_name = service_name;

    if (config->dispatch_mode != SERVER_DISPATCH_THREAD_POOL &&
        config->dispatch_mode != SERVER_DISPATCH_SERIAL &&
        config->dispatch_mode != SERVER_DISPATCH_THREAD_PER_REQUEST) {
        set_error(errbuf, errbuf_size, "SERVER ERROR: invalid dispatch mode");
        return STATUS_INVALID_ARGS;
    }

    if (server_init_active_sync(server, errbuf, errbuf_size) != 0) {
        server_destroy(server);
        return STATUS_EXEC_ERROR;
    }

    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd < 0) {
        set_error(errbuf, errbuf_size, "SERVER ERROR: failed to create listen socket");
        server_destroy(server);
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

    if (config->dispatch_mode == SERVER_DISPATCH_THREAD_POOL) {
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
    if (server->config.dispatch_mode == SERVER_DISPATCH_THREAD_POOL) {
        return server_run_thread_pool(server, errbuf, errbuf_size);
    }

    if (server->config.dispatch_mode == SERVER_DISPATCH_SERIAL) {
        return server_run_serial(server, errbuf, errbuf_size);
    }

    return server_run_thread_per_request(server, errbuf, errbuf_size);
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

    if (server->config.dispatch_mode == SERVER_DISPATCH_THREAD_PER_REQUEST) {
        server_wait_for_active_requests(server);
    }

    thread_pool_destroy(&server->thread_pool);
    task_queue_destroy(&server->queue);
    db_api_destroy(&server->db_api);

    if (server->active_sync_initialized) {
        pthread_cond_destroy(&server->active_zero_cond);
        pthread_mutex_destroy(&server->active_mutex);
        server->active_sync_initialized = 0;
    }
}
