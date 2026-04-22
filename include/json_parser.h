#ifndef JSON_PARSER_H
#define JSON_PARSER_H

#include <stddef.h>

typedef enum {
    JSON_SQL_STATUS_OK = 0,
    JSON_SQL_STATUS_INVALID = 1,
    JSON_SQL_STATUS_MISSING = 2
} JsonSqlStatus;

JsonSqlStatus json_extract_sql_field(const char *json_text,
                                     char **out_sql,
                                     char *errbuf,
                                     size_t errbuf_size);

#endif
