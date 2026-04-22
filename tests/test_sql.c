#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db_api.h"

static void fail(const char *msg, const char *body)
{
    fprintf(stderr, "test_sql failed: %s\nbody: %s\n", msg, body == NULL ? "" : body);
    exit(1);
}

static void assert_has(const char *body, const char *needle, const char *msg)
{
    if (strstr(body, needle) == NULL) {
        fail(msg, body);
    }
}

static void exec_sql(const char *sql, char *out, size_t max)
{
    db_exec(sql, out, (int)max);
}

static void test_books_flow(void)
{
    char out[DB_OUT_MAX];

    db_init();
    exec_sql("CREATE TABLE books;", out, sizeof(out));
    assert_has(out, "\"ok\":true", "create should work");

    exec_sql("INSERT INTO books VALUES (1, 'C Book', 'Kim', 2024);", out, sizeof(out));
    assert_has(out, "\"affected\":1", "insert should affect one row");

    exec_sql("SELECT * FROM books WHERE id = 1;", out, sizeof(out));
    assert_has(out, "\"id\":1", "select by id should find row");
    assert_has(out, "\"title\":\"C Book\"", "title should match");

    exec_sql("SELECT * FROM books;", out, sizeof(out));
    assert_has(out, "\"author\":\"Kim\"", "select all should show row");

    exec_sql("DELETE FROM books WHERE id = 1;", out, sizeof(out));
    assert_has(out, "\"affected\":1", "delete should affect one row");

    exec_sql("SELECT * FROM books WHERE id = 1;", out, sizeof(out));
    assert_has(out, "\"rows\":[]", "deleted row should be gone");
}

static void test_edges(void)
{
    char out[DB_OUT_MAX];
    char long_sql[DB_SQL_MAX + 16];

    db_init();
    exec_sql("", out, sizeof(out));
    assert_has(out, "\"ok\":false", "empty sql should fail");

    exec_sql("BAD SQL;", out, sizeof(out));
    assert_has(out, "\"err\":\"bad sql\"", "bad sql should fail");

    exec_sql("SELECT * FROM books WHERE id = 999;", out, sizeof(out));
    assert_has(out, "\"rows\":[]", "missing id should return empty rows");

    exec_sql("INSERT INTO books VALUES (7, 'space title here', 'author name here', 2025);", out, sizeof(out));
    assert_has(out, "\"ok\":true", "insert with spaces should work");

    exec_sql("INSERT INTO books VALUES (7, 'dup', 'dup', 2025);", out, sizeof(out));
    assert_has(out, "\"err\":\"duplicate id\"", "duplicate insert should fail");

    exec_sql("DELETE FROM books WHERE id = 999;", out, sizeof(out));
    assert_has(out, "\"affected\":0", "delete missing id should affect zero");

    memset(long_sql, 'A', sizeof(long_sql));
    long_sql[sizeof(long_sql) - 1U] = '\0';
    exec_sql(long_sql, out, sizeof(out));
    assert_has(out, "\"ok\":false", "long sql should fail");
}

int main(void)
{
    test_books_flow();
    test_edges();
    db_free();
    puts("test_sql: OK");
    return 0;
}
