CC := clang
CFLAGS := -std=c99 -D_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Iinclude -pthread
LDFLAGS := -pthread
BUILD_DIR := build

PORT ?= 8080
WORKERS ?= 8
COUNT ?= 1000000
CONC ?= 128
BENCH_WORKERS ?= 1,2,4,8,16,32

SRC_FILES := $(wildcard src/*.c)
LIB_SRC_FILES := $(filter-out src/main.c src/server_main.c,$(SRC_FILES))
LIB_OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(LIB_SRC_FILES))
SQL_OBJECTS := $(LIB_OBJECTS) $(BUILD_DIR)/main.o
SERVER_OBJECTS := $(LIB_OBJECTS) $(BUILD_DIR)/server_main.o
TEST_SOURCES := $(wildcard tests/test_*.c)
TEST_BINS := $(patsubst tests/%.c,$(BUILD_DIR)/%,$(TEST_SOURCES))
TEST_SHELLS := $(wildcard tests/test_*.sh)
TOOL_SOURCES := $(wildcard tools/*.c)
TOOL_BINS := $(patsubst tools/%.c,$(BUILD_DIR)/%,$(TOOL_SOURCES))

.PHONY: all run test api-test bench clean

all: server sql_processor $(TEST_BINS) $(TOOL_BINS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/test_%: tests/test_%.c $(LIB_OBJECTS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $< -o $@ $(LDFLAGS)

$(BUILD_DIR)/%: tools/%.c $(LIB_OBJECTS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LIB_OBJECTS) $< -o $@ $(LDFLAGS)

sql_processor: $(SQL_OBJECTS)
	$(CC) $(CFLAGS) $(SQL_OBJECTS) -o $@ $(LDFLAGS)

server: $(SERVER_OBJECTS)
	$(CC) $(CFLAGS) $(SERVER_OBJECTS) -o $@ $(LDFLAGS)

run: server
	./server --port $(PORT) --workers $(WORKERS)

test: $(TEST_BINS) sql_processor
	@for bin in $(TEST_BINS); do \
		$$bin; \
	done
	@for script in $(TEST_SHELLS); do \
		sh $$script; \
	done

api-test: server
	sh tests/api_test.sh

bench: server
	node bench/bench.js --workers $(BENCH_WORKERS) --count $(COUNT) --conc $(CONC)

clean:
	rm -rf $(BUILD_DIR) sql_processor server
