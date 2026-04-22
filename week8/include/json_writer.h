#ifndef JSON_WRITER_H
#define JSON_WRITER_H

#include <stddef.h>

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} JsonBuilder;

int json_builder_init(JsonBuilder *builder);
int json_builder_append(JsonBuilder *builder, const char *text);
int json_builder_append_char(JsonBuilder *builder, char ch);
int json_builder_append_size(JsonBuilder *builder, size_t value);
int json_builder_append_uint64(JsonBuilder *builder, unsigned long long value);
int json_builder_append_bool(JsonBuilder *builder, int value);
int json_builder_append_escaped_string(JsonBuilder *builder, const char *text);
char *json_builder_take(JsonBuilder *builder);
void json_builder_free(JsonBuilder *builder);

char *json_make_error(const char *error_code, const char *message);

#endif
