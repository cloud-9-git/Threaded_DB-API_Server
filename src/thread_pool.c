#include "thread_pool.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* thread pool 초기화/생성 실패 메시지를 errbuf에 기록한다. */
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

/* worker는 queue에서 task를 꺼내 등록된 handler로 위임한다. */
static void *thread_pool_worker_main(void *arg)
{
    ThreadPool *pool = (ThreadPool *)arg;

    for (;;) {
        ClientTask task;
        TaskQueueStatus status;

        status = task_queue_pop(pool->queue, &task);
        if (status == TASK_QUEUE_STATUS_SHUTDOWN) {
            break;
        }
        if (status != TASK_QUEUE_STATUS_OK) {
            continue;
        }

        pool->handler(task, pool->user_data);
    }

    return NULL;
}

/* 지정된 수만큼 worker thread를 만들고 queue/handler를 묶는다. */
int thread_pool_init(ThreadPool *pool,
                     size_t thread_count,
                     TaskQueue *queue,
                     ThreadPoolTaskHandler handler,
                     void *user_data,
                     char *errbuf,
                     size_t errbuf_size)
{
    size_t i;

    if (pool == NULL || queue == NULL || handler == NULL || thread_count == 0U) {
        set_error(errbuf, errbuf_size, "THREAD POOL ERROR: invalid pool arguments");
        return -1;
    }

    memset(pool, 0, sizeof(*pool));
    pool->threads = (pthread_t *)calloc(thread_count, sizeof(pthread_t));
    if (pool->threads == NULL) {
        set_error(errbuf, errbuf_size, "THREAD POOL ERROR: out of memory");
        return -1;
    }

    pool->thread_count = thread_count;
    pool->queue = queue;
    pool->handler = handler;
    pool->user_data = user_data;

    for (i = 0U; i < thread_count; ++i) {
        if (pthread_create(&pool->threads[i], NULL, thread_pool_worker_main, pool) != 0) {
            size_t joined;

            task_queue_shutdown(queue);
            for (joined = 0U; joined < i; ++joined) {
                pthread_join(pool->threads[joined], NULL);
            }
            free(pool->threads);
            memset(pool, 0, sizeof(*pool));
            set_error(errbuf, errbuf_size, "THREAD POOL ERROR: failed to create worker thread");
            return -1;
        }
    }

    return 0;
}

/* queue를 닫고 모든 worker thread 종료를 기다린 뒤 리소스를 회수한다. */
void thread_pool_destroy(ThreadPool *pool)
{
    size_t i;

    if (pool == NULL || pool->threads == NULL) {
        return;
    }

    if (pool->queue != NULL) {
        task_queue_shutdown(pool->queue);
    }

    for (i = 0U; i < pool->thread_count; ++i) {
        pthread_join(pool->threads[i], NULL);
    }

    free(pool->threads);
    memset(pool, 0, sizeof(*pool));
}
