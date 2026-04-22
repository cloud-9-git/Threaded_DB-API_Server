#include "queue.h"

#include <string.h>

void q_init(Queue *q)
{
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->cond, NULL);
}

int q_push(Queue *q, Job *j)
{
    int ok = 0;

    pthread_mutex_lock(&q->lock);
    if (!q->closed && q->len < QUEUE_MAX) {
        q->jobs[q->tail] = *j;
        q->tail = (q->tail + 1) % QUEUE_MAX;
        q->len += 1;
        ok = 1;
        pthread_cond_signal(&q->cond);
    }
    pthread_mutex_unlock(&q->lock);
    return ok;
}

int q_pop(Queue *q, Job *j)
{
    pthread_mutex_lock(&q->lock);
    while (!q->closed && q->len == 0) {
        // 큐가 비면 worker는 여기서 기다린다.
        pthread_cond_wait(&q->cond, &q->lock);
    }
    if (q->len == 0 && q->closed) {
        pthread_mutex_unlock(&q->lock);
        return 0;
    }

    *j = q->jobs[q->head];
    q->head = (q->head + 1) % QUEUE_MAX;
    q->len -= 1;
    pthread_mutex_unlock(&q->lock);
    return 1;
}

int q_len(Queue *q)
{
    int len;

    pthread_mutex_lock(&q->lock);
    len = q->len;
    pthread_mutex_unlock(&q->lock);
    return len;
}

void q_close(Queue *q)
{
    pthread_mutex_lock(&q->lock);
    q->closed = 1;
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->lock);
}
