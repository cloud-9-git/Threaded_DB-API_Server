#include "task_queue.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* task queue 내부 오류 메시지를 errbuf에 기록한다. */
static void set_error(char *errbuf, size_t errbuf_size, const char *fmt, ...)
{
    va_list args;

    if (errbuf == NULL || errbuf_size == 0U) {
        return;
    }

    va_start(args, fmt);
    vsnprintf(errbuf, errbuf_size, fmt, args);
    va_end(args);
}

/* bounded queue를 초기화하고 mutex/cond와 순환 버퍼를 준비한다. */
int task_queue_init(TaskQueue *queue, size_t capacity, char *errbuf, size_t errbuf_size)
{
    ClientTask *items;

    if (queue == NULL || capacity == 0U) {
        set_error(errbuf, errbuf_size, "TASK QUEUE ERROR: invalid queue arguments");
        return -1;
    }

    memset(queue, 0, sizeof(*queue));

    items = (ClientTask *)calloc(capacity, sizeof(ClientTask));
    if (items == NULL) {
        set_error(errbuf, errbuf_size, "TASK QUEUE ERROR: out of memory");
        return -1;
    }

    if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
        free(items);
        set_error(errbuf, errbuf_size, "TASK QUEUE ERROR: failed to initialize mutex");
        return -1;
    }

    if (pthread_cond_init(&queue->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&queue->mutex);
        free(items);
        set_error(errbuf, errbuf_size, "TASK QUEUE ERROR: failed to initialize condition variable");
        return -1;
    }

    queue->items = items;
    queue->capacity = capacity;
    return 0;
}

/* queue가 가득 차지 않았을 때만 client task를 즉시 push한다. */
TaskQueueStatus task_queue_try_push(TaskQueue *queue, ClientTask task)
{
    TaskQueueStatus status = TASK_QUEUE_STATUS_OK;

    if (queue == NULL) {
        return TASK_QUEUE_STATUS_ERROR;
    }

    if (pthread_mutex_lock(&queue->mutex) != 0) {
        return TASK_QUEUE_STATUS_ERROR;
    }

    if (queue->shutdown) {
        status = TASK_QUEUE_STATUS_SHUTDOWN;
    } else if (queue->count >= queue->capacity) {
        status = TASK_QUEUE_STATUS_FULL;
    } else {
        queue->items[queue->tail] = task;
        queue->tail = (queue->tail + 1U) % queue->capacity;
        queue->count += 1U;
        pthread_cond_signal(&queue->not_empty);
    }

    pthread_mutex_unlock(&queue->mutex);
    return status;
}

/* worker가 queue에서 task 하나를 꺼내며, shutdown이면 종료 신호를 돌려준다. */
TaskQueueStatus task_queue_pop(TaskQueue *queue, ClientTask *out_task)
{
    TaskQueueStatus status = TASK_QUEUE_STATUS_OK;

    if (queue == NULL || out_task == NULL) {
        return TASK_QUEUE_STATUS_ERROR;
    }

    if (pthread_mutex_lock(&queue->mutex) != 0) {
        return TASK_QUEUE_STATUS_ERROR;
    }

    while (queue->count == 0U && !queue->shutdown) {
        if (pthread_cond_wait(&queue->not_empty, &queue->mutex) != 0) {
            pthread_mutex_unlock(&queue->mutex);
            return TASK_QUEUE_STATUS_ERROR;
        }
    }

    if (queue->count == 0U && queue->shutdown) {
        status = TASK_QUEUE_STATUS_SHUTDOWN;
    } else {
        *out_task = queue->items[queue->head];
        queue->head = (queue->head + 1U) % queue->capacity;
        queue->count -= 1U;
    }

    pthread_mutex_unlock(&queue->mutex);
    return status;
}

/* queue shutdown 플래그를 세우고 대기 중인 worker를 모두 깨운다. */
void task_queue_shutdown(TaskQueue *queue)
{
    if (queue == NULL || queue->items == NULL) {
        return;
    }

    if (pthread_mutex_lock(&queue->mutex) != 0) {
        return;
    }

    queue->shutdown = 1;
    pthread_cond_broadcast(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);
}

/* queue 내부 버퍼와 동기화 객체를 해제한다. */
void task_queue_destroy(TaskQueue *queue)
{
    if (queue == NULL || queue->items == NULL) {
        return;
    }

    task_queue_shutdown(queue);
    pthread_cond_destroy(&queue->not_empty);
    pthread_mutex_destroy(&queue->mutex);
    free(queue->items);
    memset(queue, 0, sizeof(*queue));
}
