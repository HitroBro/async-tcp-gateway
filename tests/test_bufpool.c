#define _POSIX_C_SOURCE 200809L
#include "bufpool.h"
#include <stdio.h>
#include <assert.h>

int main(void) {
    printf("[*] Running test_bufpool: verifying chunk slab pool...\n");
    BufferPool pool;
    assert(bufpool_init(&pool, 4096, 4) == 0);
    assert(pool.available == 4);

    void *c1 = bufpool_acquire(&pool);
    void *c2 = bufpool_acquire(&pool);
    void *c3 = bufpool_acquire(&pool);
    void *c4 = bufpool_acquire(&pool);
    assert(c1 && c2 && c3 && c4);
    assert(pool.available == 0);
    assert(bufpool_acquire(&pool) == NULL);

    bufpool_release(&pool, c2);
    assert(pool.available == 1);
    void *c_reused = bufpool_acquire(&pool);
    assert(c_reused == c2);

    bufpool_release(&pool, c1);
    bufpool_release(&pool, c3);
    bufpool_release(&pool, c4);
    bufpool_release(&pool, c_reused);
    assert(pool.available == 4);

    bufpool_destroy(&pool);
    printf("[PASS] test_bufpool passed all tests successfully.\n");
    return 0;
}
