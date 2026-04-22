#ifndef JSON_WRITER_H
#define JSON_WRITER_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} JsonWriter;

int json_writer_init(JsonWriter *writer, char *errbuf, size_t errbuf_size);
void json_writer_destroy(JsonWriter *writer);
int json_writer_append_raw(JsonWriter *writer,
                           const char *text,
                           char *errbuf,
                           size_t errbuf_size);
int json_writer_append_raw_len(JsonWriter *writer,
                               const char *text,
                               size_t text_length,
                               char *errbuf,
                               size_t errbuf_size);
int json_writer_append_char(JsonWriter *writer,
                            char ch,
                            char *errbuf,
                            size_t errbuf_size);
int json_writer_append_json_string(JsonWriter *writer,
                                   const char *text,
                                   char *errbuf,
                                   size_t errbuf_size);
int json_writer_append_bool(JsonWriter *writer,
                            int value,
                            char *errbuf,
                            size_t errbuf_size);
int json_writer_append_uint64(JsonWriter *writer,
                              uint64_t value,
                              char *errbuf,
                              size_t errbuf_size);
char *json_writer_take_string(JsonWriter *writer);

#endif
