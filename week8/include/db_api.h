#ifndef DB_API_H
#define DB_API_H

#include <pthread.h>
#include <stddef.h>

#include "runtime.h"

typedef struct {
    int http_status;
    char *json_body;
} DbApiResult;

typedef struct {
    ExecutionContext ctx;
    pthread_rwlock_t rwlock;
    int initialized;
} DbApi;

int db_api_init(DbApi *api, const char *db_dir, char *errbuf, size_t errbuf_size);
void db_api_destroy(DbApi *api);
int db_api_execute_sql(DbApi *api, const char *sql, DbApiResult *out_result);
void db_api_result_free(DbApiResult *result);

#endif
