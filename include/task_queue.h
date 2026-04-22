#ifndef TASK_QUEUE_H
#define TASK_QUEUE_H

#include <pthread.h>
#include <stddef.h>

typedef struct {
    int client_fd;
} ClientTask;

typedef enum {
    TASK_QUEUE_STATUS_OK = 0,
    TASK_QUEUE_STATUS_FULL = 1,
    TASK_QUEUE_STATUS_SHUTDOWN = 2,
    TASK_QUEUE_STATUS_ERROR = 3
} TaskQueueStatus;

typedef struct {
    ClientTask *items;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    int shutdown;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
} TaskQueue;

int task_queue_init(TaskQueue *queue, size_t capacity, char *errbuf, size_t errbuf_size);
TaskQueueStatus task_queue_try_push(TaskQueue *queue, ClientTask task);
TaskQueueStatus task_queue_pop(TaskQueue *queue, ClientTask *out_task);
void task_queue_shutdown(TaskQueue *queue);
void task_queue_destroy(TaskQueue *queue);

#endif
