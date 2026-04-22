#ifndef DB_API_H
#define DB_API_H

#include <pthread.h>
#include <stddef.h>

#include "runtime.h"

typedef enum {
    DB_API_OK = 0,
    DB_API_INVALID_JSON,
    DB_API_MISSING_SQL_FIELD,
    DB_API_EMPTY_QUERY,
    DB_API_MULTI_STATEMENT_NOT_ALLOWED,
    DB_API_UNSUPPORTED_QUERY,
    DB_API_SQL_PARSE_ERROR,
    DB_API_SCHEMA_ERROR,
    DB_API_STORAGE_ERROR,
    DB_API_INDEX_ERROR,
    DB_API_EXECUTION_ERROR,
    DB_API_QUEUE_FULL,
    DB_API_INTERNAL_ERROR
} DbApiStatus;

typedef struct {
    ExecutionContext ctx;
    pthread_rwlock_t lock;
} DbApi;

int db_api_init(DbApi *api, const char *db_dir, char *errbuf, size_t errbuf_size);
void db_api_destroy(DbApi *api);
DbApiStatus db_api_execute_sql(DbApi *api,
                               const char *sql,
                               char **out_json_body,
                               int *out_http_status,
                               char *errbuf,
                               size_t errbuf_size);
int db_api_status_to_http_status(DbApiStatus status);
const char *db_api_status_to_error_code(DbApiStatus status);
char *db_api_build_error_json(DbApiStatus status,
                              const char *message,
                              char *errbuf,
                              size_t errbuf_size);

#endif
