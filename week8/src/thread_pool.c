#include "thread_pool.h"

#include <stdlib.h>
#include <string.h>

static void *worker_main(void *arg)
{
    ThreadPool *pool = (ThreadPool *)arg;
    Task task;

    while (task_queue_pop(&pool->queue, &task)) {
        pool->handler(task, pool->user_data);
    }

    return NULL;
}

int thread_pool_init(ThreadPool *pool,
                     size_t thread_count,
                     size_t queue_capacity,
                     TaskHandler handler,
                     void *user_data)
{
    size_t i;

    if (pool == NULL || thread_count == 0U || queue_capacity == 0U || handler == NULL) {
        return 0;
    }

    memset(pool, 0, sizeof(*pool));
    pool->threads = (pthread_t *)calloc(thread_count, sizeof(pthread_t));
    if (pool->threads == NULL) {
        return 0;
    }

    if (!task_queue_init(&pool->queue, queue_capacity)) {
        free(pool->threads);
        memset(pool, 0, sizeof(*pool));
        return 0;
    }

    pool->thread_count = thread_count;
    pool->handler = handler;
    pool->user_data = user_data;

    for (i = 0U; i < thread_count; ++i) {
        if (pthread_create(&pool->threads[i], NULL, worker_main, pool) != 0) {
            pool->thread_count = i;
            thread_pool_destroy(pool);
            return 0;
        }
    }

    pool->started = 1;
    return 1;
}

int thread_pool_submit(ThreadPool *pool, Task task)
{
    if (pool == NULL || !pool->started) {
        return -1;
    }

    return task_queue_push(&pool->queue, task);
}

void thread_pool_shutdown(ThreadPool *pool)
{
    if (pool == NULL || !pool->started) {
        return;
    }

    task_queue_shutdown(&pool->queue);
}

void thread_pool_destroy(ThreadPool *pool)
{
    size_t i;

    if (pool == NULL) {
        return;
    }

    if (pool->started) {
        task_queue_shutdown(&pool->queue);
    }

    for (i = 0U; i < pool->thread_count; ++i) {
        pthread_join(pool->threads[i], NULL);
    }

    task_queue_destroy(&pool->queue);
    free(pool->threads);
    memset(pool, 0, sizeof(*pool));
}
