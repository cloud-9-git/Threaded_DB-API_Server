#ifndef JSON_PARSER_H
#define JSON_PARSER_H

#include <stddef.h>

typedef enum {
    JSON_SQL_OK = 0,
    JSON_SQL_INVALID = 1,
    JSON_SQL_MISSING = 2
} JsonSqlStatus;

JsonSqlStatus json_extract_sql(const char *body,
                               char **out_sql,
                               char *errbuf,
                               size_t errbuf_size);

#endif
