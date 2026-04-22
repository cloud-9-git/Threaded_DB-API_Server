#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>

#include "db_api.h"
#include "thread_pool.h"

typedef struct {
    const char *db_dir;
    int port;
    size_t worker_count;
    size_t queue_capacity;
    size_t max_body_size;
} ServerConfig;

typedef struct {
    ServerConfig config;
    int listen_fd;
    DbApi db_api;
    ThreadPool pool;
    int initialized;
} Server;

void server_config_init(ServerConfig *config);
int server_init(Server *server, const ServerConfig *config, char *errbuf, size_t errbuf_size);
int server_run(Server *server);
void server_destroy(Server *server);

#endif
