#include <stdio.h>
#include <stdlib.h>

#include "queue.h"

static void fail(const char *msg)
{
    fprintf(stderr, "test_queue failed: %s\n", msg);
    exit(1);
}

static void assert_true(int ok, const char *msg)
{
    if (!ok) {
        fail(msg);
    }
}

static void test_push_pop_order(void)
{
    Queue q;
    Job a;
    Job b;
    Job out;

    q_init(&q);
    a.type = JOB_SQL;
    a.fd = 1;
    a.in_ms = 0.0;
    snprintf(a.sql, sizeof(a.sql), "SELECT * FROM books;");
    b = a;
    b.fd = 2;
    snprintf(b.sql, sizeof(b.sql), "SELECT * FROM books WHERE id = 1;");

    assert_true(q_push(&q, &a) == 1, "first push should work");
    assert_true(q_push(&q, &b) == 1, "second push should work");
    assert_true(q_len(&q) == 2, "queue len should be 2");
    assert_true(q_pop(&q, &out) == 1, "first pop should work");
    assert_true(out.fd == 1, "first pop should keep order");
    assert_true(q_pop(&q, &out) == 1, "second pop should work");
    assert_true(out.fd == 2, "second pop should keep order");
    assert_true(q_len(&q) == 0, "queue should be empty");
    q_close(&q);
    assert_true(q_pop(&q, &out) == 0, "closed empty queue should stop");
}

static void test_full_queue(void)
{
    Queue q;
    Job j;
    int i;

    q_init(&q);
    j.type = JOB_SQL;
    j.fd = 1;
    j.in_ms = 0.0;
    snprintf(j.sql, sizeof(j.sql), "SELECT * FROM books;");
    for (i = 0; i < QUEUE_MAX; ++i) {
        assert_true(q_push(&q, &j) == 1, "push until full should work");
    }
    assert_true(q_push(&q, &j) == 0, "full queue should reject push");
    assert_true(q_len(&q) == QUEUE_MAX, "full queue len should match max");
    q_close(&q);
}

int main(void)
{
    test_push_pop_order();
    test_full_queue();
    puts("test_queue: OK");
    return 0;
}
