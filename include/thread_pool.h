#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <pthread.h>
#include <stddef.h>

#include "task_queue.h"

typedef void (*ThreadPoolTaskHandler)(ClientTask task, void *user_data);

typedef struct {
    pthread_t *threads;
    size_t thread_count;
    TaskQueue *queue;
    ThreadPoolTaskHandler handler;
    void *user_data;
} ThreadPool;

int thread_pool_init(ThreadPool *pool,
                     size_t thread_count,
                     TaskQueue *queue,
                     ThreadPoolTaskHandler handler,
                     void *user_data,
                     char *errbuf,
                     size_t errbuf_size);
void thread_pool_destroy(ThreadPool *pool);

#endif
