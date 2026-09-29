#include "bufpool.h"
#include <stdlib.h>
#include <string.h>

int bufpool_init(BufferPool *pool, size_t chunk_size, size_t capacity) {
    if (!pool || chunk_size < sizeof(BufferChunk) || capacity == 0) return -1;

    pool->chunk_size = chunk_size;
    pool->capacity = capacity;
    pool->available = capacity;
    pool->storage = (uint8_t *)malloc(chunk_size * capacity);
    if (!pool->storage) return -1;

    pool->free_list = NULL;
    for (size_t i = 0; i < capacity; i++) {
        BufferChunk *chunk = (BufferChunk *)(pool->storage + i * chunk_size);
        chunk->next = pool->free_list;
        pool->free_list = chunk;
    }
    return 0;
}

void *bufpool_acquire(BufferPool *pool) {
    if (!pool || !pool->free_list) return NULL;
    BufferChunk *chunk = pool->free_list;
    // Pop head chunk from free-list
    pool->free_list = chunk->next;
    pool->available--;
    return (void *)chunk;
}

void bufpool_release(BufferPool *pool, void *ptr) {
    if (!pool || !ptr) return;
    BufferChunk *chunk = (BufferChunk *)ptr;
    chunk->next = pool->free_list;
    pool->free_list = chunk;
    pool->available++;
}

void bufpool_destroy(BufferPool *pool) {
    if (!pool) return;
    if (pool->storage) {
        free(pool->storage);
        pool->storage = NULL;
    }
    pool->free_list = NULL;
    pool->capacity = 0;
    pool->available = 0;
}
