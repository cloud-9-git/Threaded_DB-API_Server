#include "server.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "errors.h"

enum {
    DEFAULT_PORT = 8080,
    DEFAULT_THREADS = 4,
    DEFAULT_QUEUE_SIZE = 64,
    DEFAULT_BODY_MAX_BYTES = 8192
};

/* 서버 CLI usage를 stdout에 출력한다. */
static void print_usage(const char *program_name)
{
    printf("Usage: %s -d <db_dir> [-p <port>] [-t <threads>] [-q <queue_size>]\n", program_name);
    printf("       %s --db <db_dir> [--port <port>] [--threads <threads>] [--queue-size <queue_size>]\n",
           program_name);
    printf("       %s -h | --help\n", program_name);
}

/* 양의 정수 문자열을 size_t로 변환하고 범위를 검사한다. */
static int parse_size_value(const char *text, size_t *out_value)
{
    char *endptr;
    unsigned long value;

    if (text == NULL || out_value == NULL) {
        return 0;
    }

    errno = 0;
    value = strtoul(text, &endptr, 10);
    if (errno != 0 || endptr == text || *endptr != '\0' || value == 0UL) {
        return 0;
    }

    *out_value = (size_t)value;
    return 1;
}

/* 포트 문자열을 int로 변환하고 1~65535 범위인지 확인한다. */
static int parse_port_value(const char *text, int *out_port)
{
    size_t parsed_port;

    if (!parse_size_value(text, &parsed_port) || parsed_port > 65535U) {
        return 0;
    }

    *out_port = (int)parsed_port;
    return 1;
}

/* argv를 파싱해 ServerConfig를 채우고 help 여부를 반환한다. */
static int parse_server_args(int argc,
                             char **argv,
                             ServerConfig *out_config,
                             int *out_help_requested)
{
    int i;

    if (out_config == NULL || out_help_requested == NULL) {
        return STATUS_INVALID_ARGS;
    }

    out_config->db_dir = NULL;
    out_config->port = DEFAULT_PORT;
    out_config->worker_count = DEFAULT_THREADS;
    out_config->queue_size = DEFAULT_QUEUE_SIZE;
    out_config->body_max_bytes = DEFAULT_BODY_MAX_BYTES;
    *out_help_requested = 0;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            *out_help_requested = 1;
            return STATUS_OK;
        }

        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--db") == 0) {
            if (i + 1 >= argc) {
                return STATUS_INVALID_ARGS;
            }
            out_config->db_dir = argv[++i];
            continue;
        }

        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
            if (i + 1 >= argc || !parse_port_value(argv[++i], &out_config->port)) {
                return STATUS_INVALID_ARGS;
            }
            continue;
        }

        if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--threads") == 0) {
            if (i + 1 >= argc || !parse_size_value(argv[++i], &out_config->worker_count)) {
                return STATUS_INVALID_ARGS;
            }
            continue;
        }

        if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--queue-size") == 0) {
            if (i + 1 >= argc || !parse_size_value(argv[++i], &out_config->queue_size)) {
                return STATUS_INVALID_ARGS;
            }
            continue;
        }

        return STATUS_INVALID_ARGS;
    }

    if (out_config->db_dir == NULL) {
        return STATUS_INVALID_ARGS;
    }

    return STATUS_OK;
}

/* mini_db_server 진입점으로, CLI 파싱 후 서버 lifecycle을 실행한다. */
int main(int argc, char **argv)
{
    ServerConfig config;
    Server server;
    int help_requested = 0;
    int status;
    char errbuf[256] = {0};

    memset(&server, 0, sizeof(server));
    server.listen_fd = -1;

    status = parse_server_args(argc, argv, &config, &help_requested);
    if (status != STATUS_OK) {
        print_usage(argv[0]);
        return status;
    }

    if (help_requested) {
        print_usage(argv[0]);
        return STATUS_OK;
    }

    status = server_init(&server, &config, errbuf, sizeof(errbuf));
    if (status != STATUS_OK) {
        if (errbuf[0] != '\0') {
            fprintf(stderr, "%s\n", errbuf);
        }
        server_destroy(&server);
        return status;
    }

    status = server_run(&server, errbuf, sizeof(errbuf));
    if (status != STATUS_OK && errbuf[0] != '\0') {
        fprintf(stderr, "%s\n", errbuf);
    }

    server_destroy(&server);
    return status;
}
