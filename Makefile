CC := clang
CFLAGS := -std=c99 -Wall -Wextra -Werror -D_XOPEN_SOURCE=700 -Iinclude
PTHREAD_FLAGS := -pthread
BUILD_DIR := build

COMMON_SRCS := \
	src/utils.c \
	src/lexer.c \
	src/parser.c \
	src/schema.c \
	src/storage.c \
	src/runtime.c \
	src/executor.c \
	src/result.c \
	src/bptree.c \
	src/benchmark.c

CLI_SRCS := \
	src/main.c \
	src/cli.c

SERVER_SRCS := \
	src/server_main.c \
	src/server.c \
	src/http.c \
	src/thread_pool.c \
	src/task_queue.c \
	src/db_api.c \
	src/json_parser.c \
	src/json_writer.c

COMMON_OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(COMMON_SRCS))
CLI_OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(CLI_SRCS))
SERVER_OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SERVER_SRCS))
TEST_SOURCES := $(wildcard tests/test_*.c)
TEST_BINS := $(patsubst tests/%.c,$(BUILD_DIR)/%,$(TEST_SOURCES))
TEST_SHELLS := $(wildcard tests/test_*.sh)
TOOL_SOURCES := $(wildcard tools/*.c)
TOOL_BINS := $(patsubst tools/%.c,$(BUILD_DIR)/%,$(TOOL_SOURCES))

.PHONY: all test clean

all: sql_processor mini_db_server $(TEST_BINS) $(TOOL_BINS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/test_%: tests/test_%.c $(COMMON_OBJECTS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(COMMON_OBJECTS) $< -o $@

$(BUILD_DIR)/%: tools/%.c $(COMMON_OBJECTS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(COMMON_OBJECTS) $< -o $@

sql_processor: $(COMMON_OBJECTS) $(CLI_OBJECTS)
	$(CC) $(CFLAGS) $^ -o $@

mini_db_server: $(COMMON_OBJECTS) $(SERVER_OBJECTS)
	$(CC) $(CFLAGS) $(PTHREAD_FLAGS) $^ -o $@

test: $(TEST_BINS) sql_processor mini_db_server
	@for bin in $(TEST_BINS); do \
		$$bin; \
	done
	@for script in $(TEST_SHELLS); do \
		sh $$script; \
	done

clean:
	rm -rf $(BUILD_DIR) sql_processor mini_db_server
