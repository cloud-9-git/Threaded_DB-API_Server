#ifndef POOL_H
#define POOL_H

#include <pthread.h>

#include "queue.h"

typedef struct WorkerArg WorkerArg;

typedef struct {
    Queue q;
    pthread_t *tids;
    WorkerArg *args;
    int wcnt;
    pthread_mutex_t stat_lock;
    long done;
} Pool;

int pool_start(Pool *p, int wcnt);
void pool_stop(Pool *p);
int pool_submit_sql(Pool *p, int fd, const char *sql, double in_ms);
int pool_submit_bench(Pool *p, int fd, const char *mode, long count, double in_ms);
void pool_stats_json(Pool *p, char *out, int max);

#endif
