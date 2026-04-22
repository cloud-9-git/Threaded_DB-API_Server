#include "task_queue.h"

#include <stdlib.h>
#include <string.h>

int task_queue_init(TaskQueue *queue, size_t capacity)
{
    if (queue == NULL || capacity == 0U) {
        return 0;
    }

    memset(queue, 0, sizeof(*queue));
    queue->items = (Task *)calloc(capacity, sizeof(Task));
    if (queue->items == NULL) {
        return 0;
    }
    queue->capacity = capacity;

    if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
        free(queue->items);
        memset(queue, 0, sizeof(*queue));
        return 0;
    }

    if (pthread_cond_init(&queue->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&queue->mutex);
        free(queue->items);
        memset(queue, 0, sizeof(*queue));
        return 0;
    }

    return 1;
}

void task_queue_destroy(TaskQueue *queue)
{
    if (queue == NULL) {
        return;
    }

    pthread_cond_destroy(&queue->not_empty);
    pthread_mutex_destroy(&queue->mutex);
    free(queue->items);
    memset(queue, 0, sizeof(*queue));
}

int task_queue_push(TaskQueue *queue, Task task)
{
    int result = 1;

    if (queue == NULL) {
        return -1;
    }

    pthread_mutex_lock(&queue->mutex);
    if (queue->shutdown) {
        result = -1;
    } else if (queue->count == queue->capacity) {
        result = 0;
    } else {
        queue->items[queue->tail] = task;
        queue->tail = (queue->tail + 1U) % queue->capacity;
        queue->count += 1U;
        pthread_cond_signal(&queue->not_empty);
    }
    pthread_mutex_unlock(&queue->mutex);
    return result;
}

int task_queue_pop(TaskQueue *queue, Task *out_task)
{
    if (queue == NULL || out_task == NULL) {
        return 0;
    }

    pthread_mutex_lock(&queue->mutex);
    while (!queue->shutdown && queue->count == 0U) {
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }

    if (queue->count == 0U && queue->shutdown) {
        pthread_mutex_unlock(&queue->mutex);
        return 0;
    }

    *out_task = queue->items[queue->head];
    queue->head = (queue->head + 1U) % queue->capacity;
    queue->count -= 1U;
    pthread_mutex_unlock(&queue->mutex);
    return 1;
}

void task_queue_shutdown(TaskQueue *queue)
{
    if (queue == NULL) {
        return;
    }

    pthread_mutex_lock(&queue->mutex);
    queue->shutdown = 1;
    pthread_cond_broadcast(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);
}
