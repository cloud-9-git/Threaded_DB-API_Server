#include "json_parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error(char *errbuf, size_t errbuf_size, const char *message)
{
    if (errbuf != NULL && errbuf_size > 0U) {
        snprintf(errbuf, errbuf_size, "%s", message);
    }
}

static void skip_ws(const char **cursor)
{
    while (**cursor != '\0' && isspace((unsigned char)**cursor)) {
        ++(*cursor);
    }
}

static int is_hex4(const char *text)
{
    size_t i;

    for (i = 0U; i < 4U; ++i) {
        if (!isxdigit((unsigned char)text[i])) {
            return 0;
        }
    }
    return 1;
}

static int append_char(char **buffer, size_t *length, size_t *capacity, char ch)
{
    char *grown;
    size_t new_capacity;

    if (*length + 1U >= *capacity) {
        new_capacity = *capacity == 0U ? 32U : *capacity * 2U;
        grown = (char *)realloc(*buffer, new_capacity);
        if (grown == NULL) {
            return 0;
        }
        *buffer = grown;
        *capacity = new_capacity;
    }

    (*buffer)[*length] = ch;
    *length += 1U;
    (*buffer)[*length] = '\0';
    return 1;
}

static int parse_string(const char **cursor, char **out_text)
{
    char *buffer = NULL;
    size_t length = 0U;
    size_t capacity = 0U;

    if (**cursor != '"') {
        return 0;
    }
    ++(*cursor);

    while (**cursor != '\0') {
        unsigned char ch = (unsigned char)**cursor;

        if (ch == '"') {
            ++(*cursor);
            if (buffer == NULL) {
                buffer = (char *)malloc(1U);
                if (buffer == NULL) {
                    return 0;
                }
                buffer[0] = '\0';
            }
            *out_text = buffer;
            return 1;
        }

        if (ch < 0x20U) {
            free(buffer);
            return 0;
        }

        if (ch == '\\') {
            ++(*cursor);
            switch (**cursor) {
                case '"':
                case '\\':
                case '/':
                    ch = (unsigned char)**cursor;
                    break;
                case 'b':
                    ch = '\b';
                    break;
                case 'f':
                    ch = '\f';
                    break;
                case 'n':
                    ch = '\n';
                    break;
                case 'r':
                    ch = '\r';
                    break;
                case 't':
                    ch = '\t';
                    break;
                case 'u':
                    if (!is_hex4(*cursor + 1)) {
                        free(buffer);
                        return 0;
                    }
                    *cursor += 4;
                    ch = '?';
                    break;
                default:
                    free(buffer);
                    return 0;
            }
        }

        if (!append_char(&buffer, &length, &capacity, (char)ch)) {
            free(buffer);
            return 0;
        }
        ++(*cursor);
    }

    free(buffer);
    return 0;
}

static int skip_value(const char **cursor);

static int skip_object(const char **cursor)
{
    char *key = NULL;

    if (**cursor != '{') {
        return 0;
    }
    ++(*cursor);
    skip_ws(cursor);

    if (**cursor == '}') {
        ++(*cursor);
        return 1;
    }

    while (**cursor != '\0') {
        if (!parse_string(cursor, &key)) {
            return 0;
        }
        free(key);
        key = NULL;
        skip_ws(cursor);
        if (**cursor != ':') {
            return 0;
        }
        ++(*cursor);
        skip_ws(cursor);
        if (!skip_value(cursor)) {
            return 0;
        }
        skip_ws(cursor);
        if (**cursor == '}') {
            ++(*cursor);
            return 1;
        }
        if (**cursor != ',') {
            return 0;
        }
        ++(*cursor);
        skip_ws(cursor);
    }

    return 0;
}

static int skip_array(const char **cursor)
{
    if (**cursor != '[') {
        return 0;
    }
    ++(*cursor);
    skip_ws(cursor);

    if (**cursor == ']') {
        ++(*cursor);
        return 1;
    }

    while (**cursor != '\0') {
        if (!skip_value(cursor)) {
            return 0;
        }
        skip_ws(cursor);
        if (**cursor == ']') {
            ++(*cursor);
            return 1;
        }
        if (**cursor != ',') {
            return 0;
        }
        ++(*cursor);
        skip_ws(cursor);
    }

    return 0;
}

static int skip_literal(const char **cursor, const char *literal)
{
    size_t length = strlen(literal);

    if (strncmp(*cursor, literal, length) != 0) {
        return 0;
    }
    *cursor += length;
    return 1;
}

static int skip_number(const char **cursor)
{
    const char *start = *cursor;

    if (**cursor == '-') {
        ++(*cursor);
    }
    if (**cursor == '0') {
        ++(*cursor);
    } else if (isdigit((unsigned char)**cursor)) {
        while (isdigit((unsigned char)**cursor)) {
            ++(*cursor);
        }
    } else {
        return 0;
    }

    if (**cursor == '.') {
        ++(*cursor);
        if (!isdigit((unsigned char)**cursor)) {
            return 0;
        }
        while (isdigit((unsigned char)**cursor)) {
            ++(*cursor);
        }
    }

    if (**cursor == 'e' || **cursor == 'E') {
        ++(*cursor);
        if (**cursor == '+' || **cursor == '-') {
            ++(*cursor);
        }
        if (!isdigit((unsigned char)**cursor)) {
            return 0;
        }
        while (isdigit((unsigned char)**cursor)) {
            ++(*cursor);
        }
    }

    return *cursor > start;
}

static int skip_value(const char **cursor)
{
    char *text = NULL;
    int ok;

    skip_ws(cursor);
    switch (**cursor) {
        case '"':
            ok = parse_string(cursor, &text);
            free(text);
            return ok;
        case '{':
            return skip_object(cursor);
        case '[':
            return skip_array(cursor);
        case 't':
            return skip_literal(cursor, "true");
        case 'f':
            return skip_literal(cursor, "false");
        case 'n':
            return skip_literal(cursor, "null");
        default:
            return skip_number(cursor);
    }
}

JsonSqlStatus json_extract_sql(const char *body,
                               char **out_sql,
                               char *errbuf,
                               size_t errbuf_size)
{
    const char *cursor = body;
    char *key = NULL;

    if (out_sql == NULL || body == NULL) {
        set_error(errbuf, errbuf_size, "invalid JSON input");
        return JSON_SQL_INVALID;
    }
    *out_sql = NULL;

    skip_ws(&cursor);
    if (*cursor != '{') {
        set_error(errbuf, errbuf_size, "request body must be a JSON object");
        return JSON_SQL_INVALID;
    }
    ++cursor;
    skip_ws(&cursor);

    if (*cursor == '}') {
        set_error(errbuf, errbuf_size, "missing top-level sql field");
        return JSON_SQL_MISSING;
    }

    while (*cursor != '\0') {
        if (!parse_string(&cursor, &key)) {
            set_error(errbuf, errbuf_size, "invalid JSON object key");
            return JSON_SQL_INVALID;
        }
        skip_ws(&cursor);
        if (*cursor != ':') {
            free(key);
            set_error(errbuf, errbuf_size, "expected ':' after JSON key");
            return JSON_SQL_INVALID;
        }
        ++cursor;
        skip_ws(&cursor);

        if (strcmp(key, "sql") == 0) {
            free(key);
            key = NULL;
            if (!parse_string(&cursor, out_sql)) {
                set_error(errbuf, errbuf_size, "sql field must be a string");
                return JSON_SQL_INVALID;
            }
        } else {
            free(key);
            key = NULL;
            if (!skip_value(&cursor)) {
                set_error(errbuf, errbuf_size, "invalid JSON value");
                return JSON_SQL_INVALID;
            }
        }

        skip_ws(&cursor);
        if (*cursor == '}') {
            ++cursor;
            skip_ws(&cursor);
            if (*cursor != '\0') {
                free(*out_sql);
                *out_sql = NULL;
                set_error(errbuf, errbuf_size, "trailing data after JSON object");
                return JSON_SQL_INVALID;
            }
            if (*out_sql == NULL) {
                set_error(errbuf, errbuf_size, "missing top-level sql field");
                return JSON_SQL_MISSING;
            }
            return JSON_SQL_OK;
        }
        if (*cursor != ',') {
            free(*out_sql);
            *out_sql = NULL;
            set_error(errbuf, errbuf_size, "expected ',' or '}' in JSON object");
            return JSON_SQL_INVALID;
        }
        ++cursor;
        skip_ws(&cursor);
    }

    free(*out_sql);
    *out_sql = NULL;
    set_error(errbuf, errbuf_size, "unterminated JSON object");
    return JSON_SQL_INVALID;
}
