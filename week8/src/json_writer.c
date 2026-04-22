#include "json_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int json_builder_reserve(JsonBuilder *builder, size_t extra)
{
    char *grown;
    size_t needed;
    size_t new_capacity;

    if (builder == NULL) {
        return 0;
    }

    needed = builder->length + extra + 1U;
    if (needed <= builder->capacity) {
        return 1;
    }

    new_capacity = builder->capacity == 0U ? 128U : builder->capacity;
    while (new_capacity < needed) {
        new_capacity *= 2U;
    }

    grown = (char *)realloc(builder->data, new_capacity);
    if (grown == NULL) {
        return 0;
    }

    builder->data = grown;
    builder->capacity = new_capacity;
    return 1;
}

int json_builder_init(JsonBuilder *builder)
{
    if (builder == NULL) {
        return 0;
    }

    builder->data = NULL;
    builder->length = 0U;
    builder->capacity = 0U;
    if (!json_builder_reserve(builder, 0U)) {
        return 0;
    }
    builder->data[0] = '\0';
    return 1;
}

int json_builder_append(JsonBuilder *builder, const char *text)
{
    size_t length;

    if (text == NULL) {
        text = "";
    }

    length = strlen(text);
    if (!json_builder_reserve(builder, length)) {
        return 0;
    }

    memcpy(builder->data + builder->length, text, length + 1U);
    builder->length += length;
    return 1;
}

int json_builder_append_char(JsonBuilder *builder, char ch)
{
    if (!json_builder_reserve(builder, 1U)) {
        return 0;
    }

    builder->data[builder->length] = ch;
    builder->length += 1U;
    builder->data[builder->length] = '\0';
    return 1;
}

int json_builder_append_size(JsonBuilder *builder, size_t value)
{
    char buffer[32];

    snprintf(buffer, sizeof(buffer), "%zu", value);
    return json_builder_append(builder, buffer);
}

int json_builder_append_uint64(JsonBuilder *builder, unsigned long long value)
{
    char buffer[32];

    snprintf(buffer, sizeof(buffer), "%llu", value);
    return json_builder_append(builder, buffer);
}

int json_builder_append_bool(JsonBuilder *builder, int value)
{
    return json_builder_append(builder, value ? "true" : "false");
}

int json_builder_append_escaped_string(JsonBuilder *builder, const char *text)
{
    size_t i;

    if (text == NULL) {
        text = "";
    }

    if (!json_builder_append_char(builder, '"')) {
        return 0;
    }

    for (i = 0U; text[i] != '\0'; ++i) {
        unsigned char ch = (unsigned char)text[i];
        char buffer[8];

        switch (ch) {
            case '"':
                if (!json_builder_append(builder, "\\\"")) {
                    return 0;
                }
                break;
            case '\\':
                if (!json_builder_append(builder, "\\\\")) {
                    return 0;
                }
                break;
            case '\b':
                if (!json_builder_append(builder, "\\b")) {
                    return 0;
                }
                break;
            case '\f':
                if (!json_builder_append(builder, "\\f")) {
                    return 0;
                }
                break;
            case '\n':
                if (!json_builder_append(builder, "\\n")) {
                    return 0;
                }
                break;
            case '\r':
                if (!json_builder_append(builder, "\\r")) {
                    return 0;
                }
                break;
            case '\t':
                if (!json_builder_append(builder, "\\t")) {
                    return 0;
                }
                break;
            default:
                if (ch < 0x20U) {
                    snprintf(buffer, sizeof(buffer), "\\u%04x", (unsigned int)ch);
                    if (!json_builder_append(builder, buffer)) {
                        return 0;
                    }
                } else if (!json_builder_append_char(builder, (char)ch)) {
                    return 0;
                }
                break;
        }
    }

    return json_builder_append_char(builder, '"');
}

char *json_builder_take(JsonBuilder *builder)
{
    char *data;

    if (builder == NULL) {
        return NULL;
    }

    data = builder->data;
    builder->data = NULL;
    builder->length = 0U;
    builder->capacity = 0U;
    return data;
}

void json_builder_free(JsonBuilder *builder)
{
    if (builder == NULL) {
        return;
    }

    free(builder->data);
    builder->data = NULL;
    builder->length = 0U;
    builder->capacity = 0U;
}

char *json_make_error(const char *error_code, const char *message)
{
    JsonBuilder builder;

    if (!json_builder_init(&builder)) {
        return NULL;
    }

    if (!json_builder_append(&builder, "{\"success\":false,\"error_code\":") ||
        !json_builder_append_escaped_string(&builder, error_code) ||
        !json_builder_append(&builder, ",\"message\":") ||
        !json_builder_append_escaped_string(&builder, message) ||
        !json_builder_append_char(&builder, '}')) {
        json_builder_free(&builder);
        return NULL;
    }

    return json_builder_take(&builder);
}
