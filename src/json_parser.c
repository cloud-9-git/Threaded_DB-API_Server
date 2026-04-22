#include "json_parser.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* JSON 본문을 순차적으로 읽기 위한 내부 파서 상태다. */
typedef struct {
    const char *text;
    size_t length;
    size_t pos;
} JsonParser;

/* json parser 내부 오류 메시지를 errbuf에 기록한다. */
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

/* 현재 위치가 가리키는 문자를 반환하고 끝이면 NUL을 돌려준다. */
static char parser_peek(const JsonParser *parser)
{
    if (parser->pos >= parser->length) {
        return '\0';
    }

    return parser->text[parser->pos];
}

/* JSON 공백을 모두 건너뛴다. */
static void parser_skip_whitespace(JsonParser *parser)
{
    while (parser->pos < parser->length &&
           isspace((unsigned char)parser->text[parser->pos])) {
        parser->pos += 1U;
    }
}

/* out_text 동적 버퍼 뒤에 byte 하나를 append한다. */
static int append_byte(char **out_text, size_t *length, size_t *capacity, unsigned char byte)
{
    char *grown;
    size_t new_capacity;

    if (*length + 2U > *capacity) {
        new_capacity = *capacity == 0U ? 32U : (*capacity * 2U);
        grown = (char *)realloc(*out_text, new_capacity);
        if (grown == NULL) {
            return 0;
        }
        *out_text = grown;
        *capacity = new_capacity;
    }

    (*out_text)[*length] = (char)byte;
    *length += 1U;
    (*out_text)[*length] = '\0';
    return 1;
}

/* codepoint를 UTF-8 바이트열로 변환해 out_text에 이어 붙인다. */
static int append_codepoint_utf8(char **out_text,
                                 size_t *length,
                                 size_t *capacity,
                                 uint32_t codepoint)
{
    if (codepoint <= 0x7FU) {
        return append_byte(out_text, length, capacity, (unsigned char)codepoint);
    }
    if (codepoint <= 0x7FFU) {
        return append_byte(out_text, length, capacity, (unsigned char)(0xC0U | (codepoint >> 6U))) &&
               append_byte(out_text, length, capacity, (unsigned char)(0x80U | (codepoint & 0x3FU)));
    }

    return append_byte(out_text, length, capacity, (unsigned char)(0xE0U | (codepoint >> 12U))) &&
           append_byte(out_text, length, capacity, (unsigned char)(0x80U | ((codepoint >> 6U) & 0x3FU))) &&
           append_byte(out_text, length, capacity, (unsigned char)(0x80U | (codepoint & 0x3FU)));
}

/* 16진수 문자 하나를 0~15 값으로 바꾼다. */
static int parse_hex_digit(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }

    return -1;
}

/* JSON 문자열 하나를 디코드해 heap 문자열로 반환한다. */
static int parse_json_string(JsonParser *parser,
                             char **out_text,
                             char *errbuf,
                             size_t errbuf_size)
{
    char *decoded = NULL;
    size_t length = 0U;
    size_t capacity = 0U;

    if (parser_peek(parser) != '"') {
        set_error(errbuf, errbuf_size, "INVALID_JSON: expected string");
        return 0;
    }

    parser->pos += 1U;
    while (parser->pos < parser->length) {
        unsigned char ch = (unsigned char)parser->text[parser->pos++];

        if (ch == '"') {
            *out_text = decoded;
            return 1;
        }

        if (ch == '\\') {
            uint32_t codepoint = 0U;
            int digit;
            char escape;

            if (parser->pos >= parser->length) {
                free(decoded);
                set_error(errbuf, errbuf_size, "INVALID_JSON: unterminated escape sequence");
                return 0;
            }

            escape = parser->text[parser->pos++];
            switch (escape) {
                case '"':
                case '\\':
                case '/':
                    if (!append_byte(&decoded, &length, &capacity, (unsigned char)escape)) {
                        free(decoded);
                        set_error(errbuf, errbuf_size, "INVALID_JSON: out of memory");
                        return 0;
                    }
                    break;
                case 'b':
                    if (!append_byte(&decoded, &length, &capacity, '\b')) {
                        free(decoded);
                        set_error(errbuf, errbuf_size, "INVALID_JSON: out of memory");
                        return 0;
                    }
                    break;
                case 'f':
                    if (!append_byte(&decoded, &length, &capacity, '\f')) {
                        free(decoded);
                        set_error(errbuf, errbuf_size, "INVALID_JSON: out of memory");
                        return 0;
                    }
                    break;
                case 'n':
                    if (!append_byte(&decoded, &length, &capacity, '\n')) {
                        free(decoded);
                        set_error(errbuf, errbuf_size, "INVALID_JSON: out of memory");
                        return 0;
                    }
                    break;
                case 'r':
                    if (!append_byte(&decoded, &length, &capacity, '\r')) {
                        free(decoded);
                        set_error(errbuf, errbuf_size, "INVALID_JSON: out of memory");
                        return 0;
                    }
                    break;
                case 't':
                    if (!append_byte(&decoded, &length, &capacity, '\t')) {
                        free(decoded);
                        set_error(errbuf, errbuf_size, "INVALID_JSON: out of memory");
                        return 0;
                    }
                    break;
                case 'u':
                    if (parser->pos + 4U > parser->length) {
                        free(decoded);
                        set_error(errbuf, errbuf_size, "INVALID_JSON: incomplete unicode escape");
                        return 0;
                    }
                    codepoint = 0U;
                    digit = parse_hex_digit(parser->text[parser->pos]);
                    if (digit < 0) {
                        free(decoded);
                        set_error(errbuf, errbuf_size, "INVALID_JSON: invalid unicode escape");
                        return 0;
                    }
                    codepoint = (uint32_t)digit;
                    digit = parse_hex_digit(parser->text[parser->pos + 1U]);
                    if (digit < 0) {
                        free(decoded);
                        set_error(errbuf, errbuf_size, "INVALID_JSON: invalid unicode escape");
                        return 0;
                    }
                    codepoint = (codepoint << 4U) | (uint32_t)digit;
                    digit = parse_hex_digit(parser->text[parser->pos + 2U]);
                    if (digit < 0) {
                        free(decoded);
                        set_error(errbuf, errbuf_size, "INVALID_JSON: invalid unicode escape");
                        return 0;
                    }
                    codepoint = (codepoint << 4U) | (uint32_t)digit;
                    digit = parse_hex_digit(parser->text[parser->pos + 3U]);
                    if (digit < 0) {
                        free(decoded);
                        set_error(errbuf, errbuf_size, "INVALID_JSON: invalid unicode escape");
                        return 0;
                    }
                    codepoint = (codepoint << 4U) | (uint32_t)digit;
                    parser->pos += 4U;
                    if (codepoint >= 0xD800U && codepoint <= 0xDFFFU) {
                        free(decoded);
                        set_error(errbuf, errbuf_size, "INVALID_JSON: unsupported surrogate escape");
                        return 0;
                    }
                    if (!append_codepoint_utf8(&decoded, &length, &capacity, codepoint)) {
                        free(decoded);
                        set_error(errbuf, errbuf_size, "INVALID_JSON: out of memory");
                        return 0;
                    }
                    break;
                default:
                    free(decoded);
                    set_error(errbuf, errbuf_size, "INVALID_JSON: unsupported escape sequence");
                    return 0;
            }
            continue;
        }

        if (ch < 0x20U) {
            free(decoded);
            set_error(errbuf, errbuf_size, "INVALID_JSON: control character in string");
            return 0;
        }

        if (!append_byte(&decoded, &length, &capacity, ch)) {
            free(decoded);
            set_error(errbuf, errbuf_size, "INVALID_JSON: out of memory");
            return 0;
        }
    }

    free(decoded);
    set_error(errbuf, errbuf_size, "INVALID_JSON: unterminated string");
    return 0;
}

/* JSON number 문법 하나를 건너뛴다. */
static int skip_json_number(JsonParser *parser, char *errbuf, size_t errbuf_size)
{
    size_t start = parser->pos;

    if (parser_peek(parser) == '-') {
        parser->pos += 1U;
    }

    if (parser_peek(parser) == '0') {
        parser->pos += 1U;
    } else if (isdigit((unsigned char)parser_peek(parser))) {
        while (isdigit((unsigned char)parser_peek(parser))) {
            parser->pos += 1U;
        }
    } else {
        set_error(errbuf, errbuf_size, "INVALID_JSON: invalid number");
        return 0;
    }

    if (parser_peek(parser) == '.') {
        parser->pos += 1U;
        if (!isdigit((unsigned char)parser_peek(parser))) {
            set_error(errbuf, errbuf_size, "INVALID_JSON: invalid fractional number");
            return 0;
        }
        while (isdigit((unsigned char)parser_peek(parser))) {
            parser->pos += 1U;
        }
    }

    if (parser_peek(parser) == 'e' || parser_peek(parser) == 'E') {
        parser->pos += 1U;
        if (parser_peek(parser) == '+' || parser_peek(parser) == '-') {
            parser->pos += 1U;
        }
        if (!isdigit((unsigned char)parser_peek(parser))) {
            set_error(errbuf, errbuf_size, "INVALID_JSON: invalid exponent");
            return 0;
        }
        while (isdigit((unsigned char)parser_peek(parser))) {
            parser->pos += 1U;
        }
    }

    if (parser->pos == start) {
        set_error(errbuf, errbuf_size, "INVALID_JSON: invalid number");
        return 0;
    }

    return 1;
}

/* literal 텍스트와 현재 위치가 정확히 일치하는지 검사하고 소비한다. */
static int consume_literal(JsonParser *parser, const char *literal)
{
    size_t literal_length = strlen(literal);

    if (parser->pos + literal_length > parser->length) {
        return 0;
    }

    if (memcmp(parser->text + parser->pos, literal, literal_length) != 0) {
        return 0;
    }

    parser->pos += literal_length;
    return 1;
}

/* JSON value 하나를 재귀적으로 건너뛴다. */
static int skip_json_value(JsonParser *parser, char *errbuf, size_t errbuf_size)
{
    char *scratch = NULL;

    parser_skip_whitespace(parser);
    switch (parser_peek(parser)) {
        case '"':
            if (!parse_json_string(parser, &scratch, errbuf, errbuf_size)) {
                return 0;
            }
            free(scratch);
            return 1;
        case '{':
            parser->pos += 1U;
            parser_skip_whitespace(parser);
            if (parser_peek(parser) == '}') {
                parser->pos += 1U;
                return 1;
            }
            for (;;) {
                if (!parse_json_string(parser, &scratch, errbuf, errbuf_size)) {
                    return 0;
                }
                free(scratch);
                scratch = NULL;
                parser_skip_whitespace(parser);
                if (parser_peek(parser) != ':') {
                    set_error(errbuf, errbuf_size, "INVALID_JSON: expected ':' in object");
                    return 0;
                }
                parser->pos += 1U;
                if (!skip_json_value(parser, errbuf, errbuf_size)) {
                    return 0;
                }
                parser_skip_whitespace(parser);
                if (parser_peek(parser) == ',') {
                    parser->pos += 1U;
                    parser_skip_whitespace(parser);
                    continue;
                }
                if (parser_peek(parser) == '}') {
                    parser->pos += 1U;
                    return 1;
                }
                set_error(errbuf, errbuf_size, "INVALID_JSON: expected ',' or '}' in object");
                return 0;
            }
        case '[':
            parser->pos += 1U;
            parser_skip_whitespace(parser);
            if (parser_peek(parser) == ']') {
                parser->pos += 1U;
                return 1;
            }
            for (;;) {
                if (!skip_json_value(parser, errbuf, errbuf_size)) {
                    return 0;
                }
                parser_skip_whitespace(parser);
                if (parser_peek(parser) == ',') {
                    parser->pos += 1U;
                    parser_skip_whitespace(parser);
                    continue;
                }
                if (parser_peek(parser) == ']') {
                    parser->pos += 1U;
                    return 1;
                }
                set_error(errbuf, errbuf_size, "INVALID_JSON: expected ',' or ']' in array");
                return 0;
            }
        case 't':
            if (consume_literal(parser, "true")) {
                return 1;
            }
            break;
        case 'f':
            if (consume_literal(parser, "false")) {
                return 1;
            }
            break;
        case 'n':
            if (consume_literal(parser, "null")) {
                return 1;
            }
            break;
        default:
            if (parser_peek(parser) == '-' || isdigit((unsigned char)parser_peek(parser))) {
                return skip_json_number(parser, errbuf, errbuf_size);
            }
            break;
    }

    set_error(errbuf, errbuf_size, "INVALID_JSON: invalid value");
    return 0;
}

/* top-level object에서 sql 문자열 필드만 추출한다. */
JsonSqlStatus json_extract_sql_field(const char *json_text,
                                     char **out_sql,
                                     char *errbuf,
                                     size_t errbuf_size)
{
    JsonParser parser;
    int found_sql = 0;

    if (out_sql != NULL) {
        *out_sql = NULL;
    }

    if (json_text == NULL || out_sql == NULL) {
        set_error(errbuf, errbuf_size, "INVALID_JSON: invalid parser arguments");
        return JSON_SQL_STATUS_INVALID;
    }

    parser.text = json_text;
    parser.length = strlen(json_text);
    parser.pos = 0U;

    parser_skip_whitespace(&parser);
    if (parser_peek(&parser) != '{') {
        set_error(errbuf, errbuf_size, "INVALID_JSON: expected top-level object");
        return JSON_SQL_STATUS_INVALID;
    }
    parser.pos += 1U;

    parser_skip_whitespace(&parser);
    if (parser_peek(&parser) == '}') {
        set_error(errbuf, errbuf_size, "MISSING_SQL_FIELD: missing top-level sql field");
        return JSON_SQL_STATUS_MISSING;
    }

    for (;;) {
        char *key = NULL;

        if (!parse_json_string(&parser, &key, errbuf, errbuf_size)) {
            return JSON_SQL_STATUS_INVALID;
        }

        parser_skip_whitespace(&parser);
        if (parser_peek(&parser) != ':') {
            free(key);
            set_error(errbuf, errbuf_size, "INVALID_JSON: expected ':' after object key");
            return JSON_SQL_STATUS_INVALID;
        }
        parser.pos += 1U;
        parser_skip_whitespace(&parser);

        if (strcmp(key, "sql") == 0) {
            char *sql_value = NULL;

            if (parser_peek(&parser) != '"') {
                free(key);
                set_error(errbuf, errbuf_size, "MISSING_SQL_FIELD: sql field must be a string");
                return JSON_SQL_STATUS_MISSING;
            }

            if (!parse_json_string(&parser, &sql_value, errbuf, errbuf_size)) {
                free(key);
                return JSON_SQL_STATUS_INVALID;
            }

            free(*out_sql);
            *out_sql = sql_value;
            found_sql = 1;
        } else if (!skip_json_value(&parser, errbuf, errbuf_size)) {
            free(key);
            return JSON_SQL_STATUS_INVALID;
        }

        free(key);
        parser_skip_whitespace(&parser);
        if (parser_peek(&parser) == ',') {
            parser.pos += 1U;
            parser_skip_whitespace(&parser);
            continue;
        }
        if (parser_peek(&parser) == '}') {
            parser.pos += 1U;
            break;
        }

        set_error(errbuf, errbuf_size, "INVALID_JSON: expected ',' or '}' in object");
        free(*out_sql);
        *out_sql = NULL;
        return JSON_SQL_STATUS_INVALID;
    }

    parser_skip_whitespace(&parser);
    if (parser.pos != parser.length) {
        free(*out_sql);
        *out_sql = NULL;
        set_error(errbuf, errbuf_size, "INVALID_JSON: unexpected trailing characters");
        return JSON_SQL_STATUS_INVALID;
    }

    if (!found_sql || *out_sql == NULL) {
        set_error(errbuf, errbuf_size, "MISSING_SQL_FIELD: missing top-level sql field");
        return JSON_SQL_STATUS_MISSING;
    }

    return JSON_SQL_STATUS_OK;
}
