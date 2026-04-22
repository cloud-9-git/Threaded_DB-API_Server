#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "db_api.h"
#include "http.h"
#include "pool.h"

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s --port 8080 --workers 8\n", prog);
}

static int cpu_default_workers(void)
{
#ifdef _SC_NPROCESSORS_ONLN
    long n = sysconf(_SC_NPROCESSORS_ONLN);

    if (n < 1L) {
        n = 1L;
    }
    if (n > 64L) {
        n = 64L;
    }
    return (int)n * 2;
#else
    return 2;
#endif
}

int main(int argc, char **argv)
{
    int port = 8080;
    int workers = cpu_default_workers();
    int i;
    Pool pool;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            workers = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (port < 1 || port > 65535 || workers < 1) {
        usage(argv[0]);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    if (!db_init()) {
        fprintf(stderr, "failed to init db\n");
        return 1;
    }
    if (!pool_start(&pool, workers)) {
        fprintf(stderr, "failed to start workers\n");
        db_free();
        return 1;
    }

    return http_run(port, &pool);
}
