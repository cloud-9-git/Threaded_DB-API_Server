#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <pthread.h>
#include <stddef.h>

#include "task_queue.h"

typedef void (*TaskHandler)(Task task, void *user_data);

typedef struct {
    pthread_t *threads;
    size_t thread_count;
    TaskQueue queue;
    TaskHandler handler;
    void *user_data;
    int started;
} ThreadPool;

int thread_pool_init(ThreadPool *pool,
                     size_t thread_count,
                     size_t queue_capacity,
                     TaskHandler handler,
                     void *user_data);
int thread_pool_submit(ThreadPool *pool, Task task);
void thread_pool_shutdown(ThreadPool *pool);
void thread_pool_destroy(ThreadPool *pool);

#endif
