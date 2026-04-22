#ifndef SERVER_H
#define SERVER_H

#include <pthread.h>
#include <stddef.h>

#include "db_api.h"
#include "task_queue.h"
#include "thread_pool.h"

typedef enum {
    SERVER_DISPATCH_THREAD_POOL = 0,
    SERVER_DISPATCH_SERIAL = 1,
    SERVER_DISPATCH_THREAD_PER_REQUEST = 2
} ServerDispatchMode;

typedef struct {
    const char *db_dir;
    const char *service_name;
    int port;
    size_t worker_count;
    size_t queue_size;
    size_t body_max_bytes;
    ServerDispatchMode dispatch_mode;
} ServerConfig;

typedef struct {
    ServerConfig config;
    int listen_fd;
    TaskQueue queue;
    ThreadPool thread_pool;
    DbApi db_api;
    pthread_mutex_t active_mutex;
    pthread_cond_t active_zero_cond;
    size_t active_request_count;
    int active_sync_initialized;
} Server;

int server_init(Server *server,
                const ServerConfig *config,
                char *errbuf,
                size_t errbuf_size);
int server_run(Server *server, char *errbuf, size_t errbuf_size);
void server_destroy(Server *server);

#endif
