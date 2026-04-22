#include "db_api.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "errors.h"
#include "executor.h"
#include "json_writer.h"
#include "lexer.h"
#include "parser.h"
#include "utils.h"

/* db_api 계층 오류 메시지를 errbuf에 기록한다. */
static void set_error(char *errbuf, size_t errbuf_size, const char *message)
{
    if (errbuf != NULL && errbuf_size > 0U) {
        snprintf(errbuf, errbuf_size, "%s", message);
    }
}

/* 공통 엔진 prefix를 제거해 API 응답 메시지를 간결하게 만든다. */
static const char *strip_error_prefix(const char *message)
{
    static const char *prefixes[] = {
        "PARSE ERROR: ",
        "LEX ERROR: ",
        "EXEC ERROR: ",
        "SCHEMA ERROR: ",
        "STORAGE ERROR: ",
        "INDEX ERROR: ",
        "INVALID_JSON: ",
        "MISSING_SQL_FIELD: "
    };
    size_t i;

    if (message == NULL) {
        return "";
    }

    for (i = 0U; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        size_t prefix_length = strlen(prefixes[i]);

        if (strncmp(message, prefixes[i], prefix_length) == 0) {
            return message + prefix_length;
        }
    }

    return message;
}

/* 성공 응답 SELECT columns 배열을 JSON으로 직렬화한다. */
static int append_columns_json(JsonWriter *writer,
                               const QueryResult *query_result,
                               char *errbuf,
                               size_t errbuf_size)
{
    size_t i;

    if (json_writer_append_char(writer, '[', errbuf, errbuf_size) != 0) {
        return -1;
    }

    for (i = 0U; i < query_result->column_count; ++i) {
        if (i > 0U &&
            json_writer_append_char(writer, ',', errbuf, errbuf_size) != 0) {
            return -1;
        }
        if (json_writer_append_json_string(writer, query_result->columns[i], errbuf, errbuf_size) != 0) {
            return -1;
        }
    }

    return json_writer_append_char(writer, ']', errbuf, errbuf_size);
}

/* 성공 응답 SELECT rows 2차원 배열을 JSON으로 직렬화한다. */
static int append_rows_json(JsonWriter *writer,
                            const QueryResult *query_result,
                            char *errbuf,
                            size_t errbuf_size)
{
    size_t row_index;
    size_t column_index;

    if (json_writer_append_char(writer, '[', errbuf, errbuf_size) != 0) {
        return -1;
    }

    for (row_index = 0U; row_index < query_result->row_count; ++row_index) {
        if (row_index > 0U &&
            json_writer_append_char(writer, ',', errbuf, errbuf_size) != 0) {
            return -1;
        }

        if (json_writer_append_char(writer, '[', errbuf, errbuf_size) != 0) {
            return -1;
        }

        for (column_index = 0U; column_index < query_result->rows[row_index].value_count; ++column_index) {
            if (column_index > 0U &&
                json_writer_append_char(writer, ',', errbuf, errbuf_size) != 0) {
                return -1;
            }
            if (json_writer_append_json_string(writer,
                                               query_result->rows[row_index].values[column_index],
                                               errbuf, errbuf_size) != 0) {
                return -1;
            }
        }

        if (json_writer_append_char(writer, ']', errbuf, errbuf_size) != 0) {
            return -1;
        }
    }

    return json_writer_append_char(writer, ']', errbuf, errbuf_size);
}

/* ExecResult를 명세된 최종 응답 JSON 형태로 직렬화한다. */
static int serialize_exec_result(const ExecResult *result,
                                 char **out_json_body,
                                 char *errbuf,
                                 size_t errbuf_size)
{
    JsonWriter writer;

    if (result == NULL || out_json_body == NULL) {
        set_error(errbuf, errbuf_size, "invalid result serialization arguments");
        return -1;
    }

    if (json_writer_init(&writer, errbuf, errbuf_size) != 0) {
        return -1;
    }

    if (result->type == RESULT_SELECT) {
        if (json_writer_append_raw(&writer, "{\"success\":", errbuf, errbuf_size) != 0 ||
            json_writer_append_bool(&writer, 1, errbuf, errbuf_size) != 0 ||
            json_writer_append_raw(&writer, ",\"type\":\"select\",\"used_index\":", errbuf, errbuf_size) != 0 ||
            json_writer_append_bool(&writer, result->used_index != 0, errbuf, errbuf_size) != 0 ||
            json_writer_append_raw(&writer, ",\"row_count\":", errbuf, errbuf_size) != 0 ||
            json_writer_append_uint64(&writer, (uint64_t)result->query_result.row_count, errbuf, errbuf_size) != 0 ||
            json_writer_append_raw(&writer, ",\"columns\":", errbuf, errbuf_size) != 0 ||
            append_columns_json(&writer, &result->query_result, errbuf, errbuf_size) != 0 ||
            json_writer_append_raw(&writer, ",\"rows\":", errbuf, errbuf_size) != 0 ||
            append_rows_json(&writer, &result->query_result, errbuf, errbuf_size) != 0 ||
            json_writer_append_char(&writer, '}', errbuf, errbuf_size) != 0) {
            json_writer_destroy(&writer);
            return -1;
        }
    } else {
        if (json_writer_append_raw(&writer, "{\"success\":", errbuf, errbuf_size) != 0 ||
            json_writer_append_bool(&writer, 1, errbuf, errbuf_size) != 0 ||
            json_writer_append_raw(&writer, ",\"type\":\"insert\",\"affected_rows\":", errbuf, errbuf_size) != 0 ||
            json_writer_append_uint64(&writer, (uint64_t)result->affected_rows, errbuf, errbuf_size) != 0 ||
            json_writer_append_raw(&writer, ",\"generated_id\":", errbuf, errbuf_size) != 0) {
            json_writer_destroy(&writer);
            return -1;
        }

        if (result->has_generated_id) {
            if (json_writer_append_uint64(&writer, result->generated_id, errbuf, errbuf_size) != 0) {
                json_writer_destroy(&writer);
                return -1;
            }
        } else if (json_writer_append_raw(&writer, "null", errbuf, errbuf_size) != 0) {
            json_writer_destroy(&writer);
            return -1;
        }

        if (json_writer_append_char(&writer, '}', errbuf, errbuf_size) != 0) {
            json_writer_destroy(&writer);
            return -1;
        }
    }

    *out_json_body = json_writer_take_string(&writer);
    json_writer_destroy(&writer);
    return *out_json_body == NULL ? -1 : 0;
}

/* 공통 엔진 상태 코드를 API error_code로 변환한다. */
static DbApiStatus map_engine_status(int status)
{
    switch (status) {
        case STATUS_SCHEMA_ERROR:
            return DB_API_SCHEMA_ERROR;
        case STATUS_STORAGE_ERROR:
            return DB_API_STORAGE_ERROR;
        case STATUS_INDEX_ERROR:
            return DB_API_INDEX_ERROR;
        case STATUS_EXEC_ERROR:
            return DB_API_EXECUTION_ERROR;
        case STATUS_PARSE_ERROR:
        case STATUS_LEX_ERROR:
            return DB_API_SQL_PARSE_ERROR;
        default:
            return DB_API_INTERNAL_ERROR;
    }
}

/* SQL 토큰 스트림에서 실제 첫 문장 시작 위치를 반환한다. */
static size_t skip_leading_semicolons(const TokenArray *tokens, size_t cursor)
{
    while (cursor < tokens->count && tokens->items[cursor].type == TOKEN_SEMICOLON) {
        cursor += 1U;
    }

    return cursor;
}

/* 명세된 SELECT preload + read lock 실행 순서를 지킨다. */
static int execute_select_with_locks(DbApi *api,
                                     const Statement *stmt,
                                     ExecResult *result,
                                     char *errbuf,
                                     size_t errbuf_size)
{
    int status;

    if (pthread_rwlock_wrlock(&api->lock) != 0) {
        set_error(errbuf, errbuf_size, "failed to acquire write lock");
        return -1;
    }

    status = runtime_preload_table(&api->ctx,
                                   stmt->select_stmt.table_name,
                                   errbuf,
                                   errbuf_size);
    pthread_rwlock_unlock(&api->lock);
    if (status != STATUS_OK) {
        return status;
    }

    if (pthread_rwlock_rdlock(&api->lock) != 0) {
        set_error(errbuf, errbuf_size, "failed to acquire read lock");
        return -1;
    }

    status = execute_statement(&api->ctx, stmt, result, errbuf, errbuf_size);
    pthread_rwlock_unlock(&api->lock);
    return status;
}

/* 명세된 INSERT write lock 실행 순서를 지킨다. */
static int execute_insert_with_lock(DbApi *api,
                                    const Statement *stmt,
                                    ExecResult *result,
                                    char *errbuf,
                                    size_t errbuf_size)
{
    int status;

    if (pthread_rwlock_wrlock(&api->lock) != 0) {
        set_error(errbuf, errbuf_size, "failed to acquire write lock");
        return -1;
    }

    status = execute_statement(&api->ctx, stmt, result, errbuf, errbuf_size);
    pthread_rwlock_unlock(&api->lock);
    return status;
}

/* API가 사용하는 실행 컨텍스트와 rwlock을 준비한다. */
int db_api_init(DbApi *api, const char *db_dir, char *errbuf, size_t errbuf_size)
{
    int status;

    if (api == NULL || db_dir == NULL) {
        set_error(errbuf, errbuf_size, "invalid db api arguments");
        return -1;
    }

    memset(api, 0, sizeof(*api));
    status = init_execution_context(db_dir, &api->ctx, errbuf, errbuf_size);
    if (status != STATUS_OK) {
        return status;
    }

    if (pthread_rwlock_init(&api->lock, NULL) != 0) {
        free_execution_context(&api->ctx);
        memset(api, 0, sizeof(*api));
        set_error(errbuf, errbuf_size, "failed to initialize rwlock");
        return -1;
    }

    return STATUS_OK;
}

/* DB API가 보유한 실행 컨텍스트와 rwlock 리소스를 정리한다. */
void db_api_destroy(DbApi *api)
{
    if (api == NULL) {
        return;
    }

    free_execution_context(&api->ctx);
    pthread_rwlock_destroy(&api->lock);
    memset(api, 0, sizeof(*api));
}

/* 명세의 error_code 이름을 각 API 상태 값에 맞게 반환한다. */
const char *db_api_status_to_error_code(DbApiStatus status)
{
    switch (status) {
        case DB_API_INVALID_JSON:
            return "INVALID_JSON";
        case DB_API_MISSING_SQL_FIELD:
            return "MISSING_SQL_FIELD";
        case DB_API_EMPTY_QUERY:
            return "EMPTY_QUERY";
        case DB_API_MULTI_STATEMENT_NOT_ALLOWED:
            return "MULTI_STATEMENT_NOT_ALLOWED";
        case DB_API_UNSUPPORTED_QUERY:
            return "UNSUPPORTED_QUERY";
        case DB_API_SQL_PARSE_ERROR:
            return "SQL_PARSE_ERROR";
        case DB_API_SCHEMA_ERROR:
            return "SCHEMA_ERROR";
        case DB_API_STORAGE_ERROR:
            return "STORAGE_ERROR";
        case DB_API_INDEX_ERROR:
            return "INDEX_ERROR";
        case DB_API_EXECUTION_ERROR:
            return "EXECUTION_ERROR";
        case DB_API_QUEUE_FULL:
            return "QUEUE_FULL";
        case DB_API_INTERNAL_ERROR:
        case DB_API_OK:
        default:
            return "INTERNAL_ERROR";
    }
}

/* 명세의 상태 코드 규칙에 따라 HTTP status를 정한다. */
int db_api_status_to_http_status(DbApiStatus status)
{
    switch (status) {
        case DB_API_OK:
            return 200;
        case DB_API_INVALID_JSON:
        case DB_API_MISSING_SQL_FIELD:
        case DB_API_EMPTY_QUERY:
        case DB_API_MULTI_STATEMENT_NOT_ALLOWED:
        case DB_API_UNSUPPORTED_QUERY:
        case DB_API_SQL_PARSE_ERROR:
        case DB_API_SCHEMA_ERROR:
        case DB_API_EXECUTION_ERROR:
            return 400;
        case DB_API_QUEUE_FULL:
            return 503;
        case DB_API_STORAGE_ERROR:
        case DB_API_INDEX_ERROR:
        case DB_API_INTERNAL_ERROR:
        default:
            return 500;
    }
}

/* 공통 에러 응답 JSON을 만든다. */
char *db_api_build_error_json(DbApiStatus status,
                              const char *message,
                              char *errbuf,
                              size_t errbuf_size)
{
    JsonWriter writer;
    const char *error_code = db_api_status_to_error_code(status);
    const char *clean_message = strip_error_prefix(message);

    if (json_writer_init(&writer, errbuf, errbuf_size) != 0) {
        return NULL;
    }

    if (json_writer_append_raw(&writer, "{\"success\":false,\"error_code\":", errbuf, errbuf_size) != 0 ||
        json_writer_append_json_string(&writer, error_code, errbuf, errbuf_size) != 0 ||
        json_writer_append_raw(&writer, ",\"message\":", errbuf, errbuf_size) != 0 ||
        json_writer_append_json_string(&writer, clean_message, errbuf, errbuf_size) != 0 ||
        json_writer_append_char(&writer, '}', errbuf, errbuf_size) != 0) {
        json_writer_destroy(&writer);
        return NULL;
    }

    return json_writer_take_string(&writer);
}

/* 단일 SQL 문자열을 실행해 성공/실패 JSON과 HTTP status를 반환한다. */
DbApiStatus db_api_execute_sql(DbApi *api,
                               const char *sql,
                               char **out_json_body,
                               int *out_http_status,
                               char *errbuf,
                               size_t errbuf_size)
{
    char *sql_copy = NULL;
    char *trimmed_sql = NULL;
    TokenArray tokens = {0};
    Statement stmt = {0};
    ExecResult result = {0};
    DbApiStatus api_status = DB_API_INTERNAL_ERROR;
    int engine_status;
    size_t cursor = 0U;

    if (out_json_body != NULL) {
        *out_json_body = NULL;
    }
    if (out_http_status != NULL) {
        *out_http_status = 500;
    }

    if (api == NULL || sql == NULL || out_json_body == NULL || out_http_status == NULL) {
        set_error(errbuf, errbuf_size, "invalid db api execute arguments");
        return DB_API_INTERNAL_ERROR;
    }

    sql_copy = strdup_safe(sql);
    trimmed_sql = trim_whitespace(sql_copy);
    if (trimmed_sql == NULL || trimmed_sql[0] == '\0') {
        api_status = DB_API_EMPTY_QUERY;
        set_error(errbuf, errbuf_size, "sql query is empty");
        goto build_error;
    }

    engine_status = tokenize_sql(trimmed_sql, &tokens, errbuf, errbuf_size);
    if (engine_status != STATUS_OK) {
        api_status = DB_API_SQL_PARSE_ERROR;
        goto build_error;
    }

    cursor = skip_leading_semicolons(&tokens, 0U);
    if (cursor >= tokens.count || tokens.items[cursor].type == TOKEN_EOF) {
        api_status = DB_API_EMPTY_QUERY;
        set_error(errbuf, errbuf_size, "sql query is empty");
        goto build_error;
    }

    if (tokens.items[cursor].type != TOKEN_INSERT &&
        tokens.items[cursor].type != TOKEN_SELECT) {
        api_status = DB_API_UNSUPPORTED_QUERY;
        set_error(errbuf, errbuf_size, "only INSERT and SELECT are supported");
        goto build_error;
    }

    engine_status = parse_next_statement(&tokens, &cursor, &stmt, errbuf, errbuf_size);
    if (engine_status != STATUS_OK) {
        api_status = DB_API_SQL_PARSE_ERROR;
        goto build_error;
    }

    cursor = skip_leading_semicolons(&tokens, cursor);
    if (cursor >= tokens.count || tokens.items[cursor].type != TOKEN_EOF) {
        api_status = DB_API_MULTI_STATEMENT_NOT_ALLOWED;
        set_error(errbuf, errbuf_size, "multiple SQL statements are not allowed");
        goto build_error;
    }

    if (stmt.type == STMT_SELECT) {
        engine_status = execute_select_with_locks(api, &stmt, &result, errbuf, errbuf_size);
    } else {
        engine_status = execute_insert_with_lock(api, &stmt, &result, errbuf, errbuf_size);
    }

    if (engine_status != STATUS_OK) {
        api_status = map_engine_status(engine_status);
        if (engine_status == -1) {
            api_status = DB_API_INTERNAL_ERROR;
        }
        goto build_error;
    }

    if (serialize_exec_result(&result, out_json_body, errbuf, errbuf_size) != 0) {
        api_status = DB_API_INTERNAL_ERROR;
        set_error(errbuf, errbuf_size, "failed to serialize execution result");
        goto build_error;
    }

    *out_http_status = 200;
    api_status = DB_API_OK;
    goto cleanup;

build_error:
    *out_http_status = db_api_status_to_http_status(api_status);
    *out_json_body = db_api_build_error_json(api_status, errbuf, errbuf, errbuf_size);
    if (*out_json_body == NULL) {
        *out_json_body = strdup_safe("{\"success\":false,\"error_code\":\"INTERNAL_ERROR\",\"message\":\"internal error\"}");
        *out_http_status = 500;
        api_status = DB_API_INTERNAL_ERROR;
    }

cleanup:
    free_exec_result(&result);
    free_statement(&stmt);
    free_token_array(&tokens);
    free(sql_copy);
    return api_status;
}
