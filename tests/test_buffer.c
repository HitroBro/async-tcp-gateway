#define _POSIX_C_SOURCE 200809L
#include "gateway.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

int main(void) {
    printf("[*] Running test_buffer: boundary wrap-around safety test...\n");
    IOBuffer buf;
    assert(buf_init(&buf, 1024) == 0);
    assert(buf.capacity == 1024);
    assert(buf_available_data(&buf) == 0);
    assert(buf_available_space(&buf) == 1023);

    // Initial contiguous write with head == 0 must reserve 1 guard byte
    size_t contig = buf_contiguous_write(&buf);
    assert(contig == 1023);

    // Fill the buffer to maximum
    memset(buf_write_ptr(&buf), 'A', contig);
    buf_advance_tail(&buf, contig);

    // Buffer is full (1023 bytes)
    assert(buf.tail == 1023);
    assert(buf.head == 0);
    assert(buf_available_data(&buf) == 1023);
    assert(buf_available_space(&buf) == 0);
    assert(buf_contiguous_write(&buf) == 0);

    // Invariant: full buffer must NEVER report empty (available_data != 0)
    assert(buf_available_data(&buf) != 0);

    buf_free(&buf);
    assert(buf.data == NULL);
    printf("[PASS] test_buffer boundary test passed successfully.\n");
    return 0;
}
