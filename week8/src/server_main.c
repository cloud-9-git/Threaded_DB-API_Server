#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"

static void print_usage(const char *program_name)
{
    printf("Usage: %s -d <db_dir> [-p <port>] [-t <threads>] [-q <queue-size>]\n", program_name);
    printf("       %s --db <db_dir> [--port <port>] [--threads <threads>] [--queue-size <size>]\n", program_name);
    printf("       %s -h | --help\n", program_name);
}

static int parse_positive_int(const char *text, int *out_value)
{
    char *end = NULL;
    long value;

    if (text == NULL || out_value == NULL || text[0] == '\0') {
        return 0;
    }

    value = strtol(text, &end, 10);
    if (end == NULL || *end != '\0' || value <= 0L || value > 65535L) {
        return 0;
    }

    *out_value = (int)value;
    return 1;
}

static int parse_size_arg(const char *text, size_t *out_value)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || out_value == NULL || text[0] == '\0') {
        return 0;
    }

    value = strtoul(text, &end, 10);
    if (end == NULL || *end != '\0' || value == 0UL) {
        return 0;
    }

    *out_value = (size_t)value;
    return 1;
}

static int parse_args(int argc, char **argv, ServerConfig *config, int *help_requested)
{
    int i;

    server_config_init(config);
    *help_requested = 0;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            *help_requested = 1;
            return 1;
        }
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--db") == 0) {
            if (i + 1 >= argc) {
                return 0;
            }
            config->db_dir = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
            if (i + 1 >= argc || !parse_positive_int(argv[++i], &config->port)) {
                return 0;
            }
            continue;
        }
        if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--threads") == 0) {
            if (i + 1 >= argc || !parse_size_arg(argv[++i], &config->worker_count)) {
                return 0;
            }
            continue;
        }
        if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--queue-size") == 0) {
            if (i + 1 >= argc || !parse_size_arg(argv[++i], &config->queue_capacity)) {
                return 0;
            }
            continue;
        }
        return 0;
    }

    return config->db_dir != NULL;
}

int main(int argc, char **argv)
{
    ServerConfig config;
    Server server = {0};
    char errbuf[256] = {0};
    int help_requested = 0;

    signal(SIGPIPE, SIG_IGN);

    if (!parse_args(argc, argv, &config, &help_requested)) {
        print_usage(argv[0]);
        return 1;
    }

    if (help_requested) {
        print_usage(argv[0]);
        return 0;
    }

    if (!server_init(&server, &config, errbuf, sizeof(errbuf))) {
        fprintf(stderr, "%s\n", errbuf[0] == '\0' ? "failed to initialize server" : errbuf);
        return 1;
    }

    return server_run(&server);
}
