#include "pool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "db_api.h"
#include "http.h"
#include "time_ms.h"

struct WorkerArg {
    Pool *pool;
    int id;
};

static void stat_done(Pool *p)
{
    pthread_mutex_lock(&p->stat_lock);
    p->done += 1L;
    pthread_mutex_unlock(&p->stat_lock);
}

static void reply_job(Pool *p, WorkerArg *arg, Job *j)
{
    char frag[DB_OUT_MAX];
    char body[DB_OUT_MAX + 512];
    double start = now_ms();
    double wait = start - j->in_ms;
    double work;
    double total;

    if (j->type == JOB_SQL) {
        db_exec(j->sql, frag, sizeof(frag));
    } else {
        db_run_bench(j->mode, j->count, p->wcnt, frag, sizeof(frag));
    }

    work = now_ms() - start;
    total = now_ms() - j->in_ms;
    snprintf(body, sizeof(body),
             "{%s,\"wait_ms\":%.3f,\"work_ms\":%.3f,\"total_ms\":%.3f,\"worker\":%d}",
             frag, wait, work, total, arg->id);
    http_send_json(j->fd, strstr(frag, "\"ok\":true") != NULL ? 200 : 400, body);
    close(j->fd);
    stat_done(p);
}

static void *worker_main(void *data)
{
    WorkerArg *arg = (WorkerArg *)data;
    Job j;

    while (q_pop(&arg->pool->q, &j)) {
        reply_job(arg->pool, arg, &j);
    }
    return NULL;
}

int pool_start(Pool *p, int wcnt)
{
    int i;

    memset(p, 0, sizeof(*p));
    if (wcnt < 1) {
        wcnt = 1;
    }
    p->wcnt = wcnt;
    q_init(&p->q);
    pthread_mutex_init(&p->stat_lock, NULL);
    p->tids = (pthread_t *)calloc((size_t)wcnt, sizeof(pthread_t));
    p->args = (WorkerArg *)calloc((size_t)wcnt, sizeof(WorkerArg));
    if (p->tids == NULL || p->args == NULL) {
        free(p->tids);
        free(p->args);
        return 0;
    }

    for (i = 0; i < wcnt; ++i) {
        p->args[i].pool = p;
        p->args[i].id = i + 1;
        if (pthread_create(&p->tids[i], NULL, worker_main, &p->args[i]) != 0) {
            q_close(&p->q);
            while (i > 0) {
                i -= 1;
                pthread_join(p->tids[i], NULL);
            }
            free(p->tids);
            free(p->args);
            return 0;
        }
    }
    return 1;
}

void pool_stop(Pool *p)
{
    int i;

    if (p == NULL) {
        return;
    }
    q_close(&p->q);
    for (i = 0; i < p->wcnt; ++i) {
        pthread_join(p->tids[i], NULL);
    }
    free(p->tids);
    free(p->args);
    p->tids = NULL;
    p->args = NULL;
}

int pool_submit_sql(Pool *p, int fd, const char *sql, double in_ms)
{
    Job j;

    memset(&j, 0, sizeof(j));
    j.type = JOB_SQL;
    j.fd = fd;
    j.in_ms = in_ms;
    snprintf(j.sql, sizeof(j.sql), "%s", sql == NULL ? "" : sql);
    return q_push(&p->q, &j);
}

int pool_submit_bench(Pool *p, int fd, const char *mode, long count, double in_ms)
{
    Job j;

    memset(&j, 0, sizeof(j));
    j.type = JOB_BENCH;
    j.fd = fd;
    j.count = count;
    j.in_ms = in_ms;
    snprintf(j.mode, sizeof(j.mode), "%s", mode == NULL ? "" : mode);
    return q_push(&p->q, &j);
}

void pool_stats_json(Pool *p, char *out, int max)
{
    long done;

    pthread_mutex_lock(&p->stat_lock);
    done = p->done;
    pthread_mutex_unlock(&p->stat_lock);
    snprintf(out, (size_t)max,
             "{\"workers\":%d,\"queue_now\":%d,\"queue_max\":%d,\"done\":%ld}",
             p->wcnt, q_len(&p->q), QUEUE_MAX, done);
}
