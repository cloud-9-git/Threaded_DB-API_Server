#ifndef QUEUE_H
#define QUEUE_H

#include <pthread.h>

#define QUEUE_MAX 1024
#define JOB_SQL_MAX 2048

enum {
    JOB_SQL = 1,
    JOB_BENCH = 2
};

typedef struct {
    int type;
    int fd;
    char sql[JOB_SQL_MAX];
    char mode[16];
    long count;
    double in_ms;
} Job;

typedef struct {
    Job jobs[QUEUE_MAX];
    int head;
    int tail;
    int len;
    int closed;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} Queue;

void q_init(Queue *q);
int q_push(Queue *q, Job *j);
int q_pop(Queue *q, Job *j);
int q_len(Queue *q);
void q_close(Queue *q);

#endif
