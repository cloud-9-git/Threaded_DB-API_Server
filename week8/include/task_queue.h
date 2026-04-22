#ifndef TASK_QUEUE_H
#define TASK_QUEUE_H

#include <pthread.h>
#include <stddef.h>

typedef struct {
    int client_fd;
} Task;

typedef struct {
    Task *items;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    int shutdown;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
} TaskQueue;

int task_queue_init(TaskQueue *queue, size_t capacity);
void task_queue_destroy(TaskQueue *queue);
int task_queue_push(TaskQueue *queue, Task task);
int task_queue_pop(TaskQueue *queue, Task *out_task);
void task_queue_shutdown(TaskQueue *queue);

#endif
