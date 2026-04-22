#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>

#include "db_api.h"
#include "task_queue.h"
#include "thread_pool.h"

typedef struct {
    const char *db_dir;
    int port;
    size_t worker_count;
    size_t queue_size;
    size_t body_max_bytes;
} ServerConfig;

typedef struct {
    ServerConfig config;
    int listen_fd;
    TaskQueue queue;
    ThreadPool thread_pool;
    DbApi db_api;
} Server;

int server_init(Server *server,
                const ServerConfig *config,
                char *errbuf,
                size_t errbuf_size);
int server_run(Server *server, char *errbuf, size_t errbuf_size);
void server_destroy(Server *server);

#endif
