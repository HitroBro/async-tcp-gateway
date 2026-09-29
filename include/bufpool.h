#ifndef BUFPOOL_H
#define BUFPOOL_H

#include <stddef.h>
#include <stdint.h>

typedef struct BufferChunk {
    struct BufferChunk *next;
} BufferChunk;

typedef struct {
    uint8_t *storage;
    size_t chunk_size;
    size_t capacity;
    size_t available;
    BufferChunk *free_list;
} BufferPool;

int bufpool_init(BufferPool *pool, size_t chunk_size, size_t capacity);
void *bufpool_acquire(BufferPool *pool);
void bufpool_release(BufferPool *pool, void *ptr);
void bufpool_destroy(BufferPool *pool);

#endif // BUFPOOL_H
