#include <stdio.h>
#include <stdlib.h>

#include "bptree.h"
#include "errors.h"

static void fail(const char *msg)
{
    fprintf(stderr, "test_btree failed: %s\n", msg);
    exit(1);
}

static void assert_true(int ok, const char *msg)
{
    if (!ok) {
        fail(msg);
    }
}

int main(void)
{
    BPTree tree;
    char err[128] = {0};
    long off = 0L;
    int found = 0;

    assert_true(bptree_init(&tree, err, sizeof(err)) == STATUS_OK, "init should work");
    assert_true(bptree_insert(&tree, 10U, 100L, err, sizeof(err)) == STATUS_OK, "insert should work");
    assert_true(bptree_search(&tree, 10U, &off, &found, err, sizeof(err)) == STATUS_OK, "search should work");
    assert_true(found == 1 && off == 100L, "inserted key should map to offset");
    assert_true(bptree_search(&tree, 99U, &off, &found, err, sizeof(err)) == STATUS_OK, "missing search should work");
    assert_true(found == 0, "missing key should not be found");
    assert_true(bptree_insert(&tree, 10U, 200L, err, sizeof(err)) == STATUS_INDEX_ERROR,
                "duplicate key should fail");
    bptree_destroy(&tree);
    puts("test_btree: OK");
    return 0;
}
