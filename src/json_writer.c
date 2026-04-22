#include "json_writer.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* json writer 내부 오류 메시지를 errbuf에 기록한다. */
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

/* writer 버퍼가 extra 바이트를 더 담을 수 있도록 용량을 늘린다. */
static int ensure_capacity(JsonWriter *writer,
                           size_t extra,
                           char *errbuf,
                           size_t errbuf_size)
{
    size_t required;
    size_t new_capacity;
    char *grown;

    if (writer == NULL) {
        set_error(errbuf, errbuf_size, "JSON ERROR: invalid writer");
        return -1;
    }

    required = writer->length + extra + 1U;
    if (required <= writer->capacity) {
        return 0;
    }

    new_capacity = writer->capacity == 0U ? 256U : writer->capacity;
    while (new_capacity < required) {
        new_capacity *= 2U;
    }

    grown = (char *)realloc(writer->data, new_capacity);
    if (grown == NULL) {
        set_error(errbuf, errbuf_size, "JSON ERROR: out of memory");
        return -1;
    }

    writer->data = grown;
    writer->capacity = new_capacity;
    return 0;
}

/* writer를 비어 있는 문자열 빌더 상태로 초기화한다. */
int json_writer_init(JsonWriter *writer, char *errbuf, size_t errbuf_size)
{
    if (writer == NULL) {
        set_error(errbuf, errbuf_size, "JSON ERROR: invalid writer");
        return -1;
    }

    memset(writer, 0, sizeof(*writer));
    if (ensure_capacity(writer, 0U, errbuf, errbuf_size) != 0) {
        return -1;
    }

    writer->data[0] = '\0';
    return 0;
}

/* writer가 소유한 동적 버퍼를 해제한다. */
void json_writer_destroy(JsonWriter *writer)
{
    if (writer == NULL) {
        return;
    }

    free(writer->data);
    memset(writer, 0, sizeof(*writer));
}

/* text_length 바이트를 그대로 writer 뒤에 이어 붙인다. */
int json_writer_append_raw_len(JsonWriter *writer,
                               const char *text,
                               size_t text_length,
                               char *errbuf,
                               size_t errbuf_size)
{
    if (writer == NULL || (text == NULL && text_length != 0U)) {
        set_error(errbuf, errbuf_size, "JSON ERROR: invalid append arguments");
        return -1;
    }

    if (ensure_capacity(writer, text_length, errbuf, errbuf_size) != 0) {
        return -1;
    }

    if (text_length > 0U) {
        memcpy(writer->data + writer->length, text, text_length);
        writer->length += text_length;
    }
    writer->data[writer->length] = '\0';
    return 0;
}

/* NUL 종료 문자열 전체를 writer 뒤에 이어 붙인다. */
int json_writer_append_raw(JsonWriter *writer,
                           const char *text,
                           char *errbuf,
                           size_t errbuf_size)
{
    size_t text_length = 0U;

    if (text != NULL) {
        text_length = strlen(text);
    }

    return json_writer_append_raw_len(writer, text, text_length, errbuf, errbuf_size);
}

/* 단일 문자 하나를 writer 뒤에 추가한다. */
int json_writer_append_char(JsonWriter *writer,
                            char ch,
                            char *errbuf,
                            size_t errbuf_size)
{
    return json_writer_append_raw_len(writer, &ch, 1U, errbuf, errbuf_size);
}

/* bool 값을 JSON true/false 토큰으로 기록한다. */
int json_writer_append_bool(JsonWriter *writer,
                            int value,
                            char *errbuf,
                            size_t errbuf_size)
{
    return json_writer_append_raw(writer, value ? "true" : "false", errbuf, errbuf_size);
}

/* uint64 정수를 10진수 JSON 숫자로 기록한다. */
int json_writer_append_uint64(JsonWriter *writer,
                              uint64_t value,
                              char *errbuf,
                              size_t errbuf_size)
{
    char buffer[32];
    int written;

    written = snprintf(buffer, sizeof(buffer), "%" PRIu64, value);
    if (written < 0) {
        set_error(errbuf, errbuf_size, "JSON ERROR: failed to format integer");
        return -1;
    }

    return json_writer_append_raw_len(writer, buffer, (size_t)written, errbuf, errbuf_size);
}

/* 일반 텍스트를 JSON 문자열 규칙에 맞게 escape해서 기록한다. */
int json_writer_append_json_string(JsonWriter *writer,
                                   const char *text,
                                   char *errbuf,
                                   size_t errbuf_size)
{
    size_t i;

    if (text == NULL) {
        text = "";
    }

    if (json_writer_append_char(writer, '"', errbuf, errbuf_size) != 0) {
        return -1;
    }

    for (i = 0U; text[i] != '\0'; ++i) {
        unsigned char ch = (unsigned char)text[i];
        char unicode_escape[7];
        int written;

        switch (ch) {
            case '\\':
                if (json_writer_append_raw(writer, "\\\\", errbuf, errbuf_size) != 0) {
                    return -1;
                }
                break;
            case '"':
                if (json_writer_append_raw(writer, "\\\"", errbuf, errbuf_size) != 0) {
                    return -1;
                }
                break;
            case '\b':
                if (json_writer_append_raw(writer, "\\b", errbuf, errbuf_size) != 0) {
                    return -1;
                }
                break;
            case '\f':
                if (json_writer_append_raw(writer, "\\f", errbuf, errbuf_size) != 0) {
                    return -1;
                }
                break;
            case '\n':
                if (json_writer_append_raw(writer, "\\n", errbuf, errbuf_size) != 0) {
                    return -1;
                }
                break;
            case '\r':
                if (json_writer_append_raw(writer, "\\r", errbuf, errbuf_size) != 0) {
                    return -1;
                }
                break;
            case '\t':
                if (json_writer_append_raw(writer, "\\t", errbuf, errbuf_size) != 0) {
                    return -1;
                }
                break;
            default:
                if (ch < 0x20U) {
                    written = snprintf(unicode_escape, sizeof(unicode_escape), "\\u%04x", ch);
                    if (written < 0 ||
                        json_writer_append_raw_len(writer, unicode_escape, (size_t)written,
                                                   errbuf, errbuf_size) != 0) {
                        return -1;
                    }
                } else if (json_writer_append_char(writer, (char)ch, errbuf, errbuf_size) != 0) {
                    return -1;
                }
                break;
        }
    }

    return json_writer_append_char(writer, '"', errbuf, errbuf_size);
}

/* 완성된 JSON 문자열 버퍼 소유권을 caller에게 넘긴다. */
char *json_writer_take_string(JsonWriter *writer)
{
    char *result;

    if (writer == NULL) {
        return NULL;
    }

    result = writer->data;
    writer->data = NULL;
    writer->length = 0U;
    writer->capacity = 0U;
    return result;
}
