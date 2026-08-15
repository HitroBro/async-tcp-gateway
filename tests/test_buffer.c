#define _POSIX_C_SOURCE 200809L
#include "gateway.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static void test_boundary_wrap(void) {
    IOBuffer buf;
    assert(buf_init(&buf, 1024) == 0);
    assert(buf.capacity == 1024);
    assert(buf_available_data(&buf) == 0);
    assert(buf_available_space(&buf) == 1023);

    size_t contig = buf_contiguous_write(&buf);
    assert(contig == 1023);

    memset(buf_write_ptr(&buf), 'A', contig);
    buf_advance_tail(&buf, contig);

    assert(buf.tail == 1023);
    assert(buf.head == 0);
    assert(buf_available_data(&buf) == 1023);
    assert(buf_available_space(&buf) == 0);
    assert(buf_contiguous_write(&buf) == 0);
    assert(buf_available_data(&buf) != 0);

    buf_free(&buf);
}

static void test_circular_wrap_and_drain(void) {
    IOBuffer buf;
    assert(buf_init(&buf, 1024) == 0);

    // Step 1: Write 500 bytes
    memset(buf_write_ptr(&buf), 'X', 500);
    buf_advance_tail(&buf, 500);
    assert(buf_available_data(&buf) == 500);

    // Step 2: Read/drain 300 bytes
    buf_advance_head(&buf, 300);
    assert(buf.head == 300);
    assert(buf_available_data(&buf) == 200);

    // Step 3: Write contiguous block to buffer end (1024 - 500 = 524 bytes)
    size_t w1 = buf_contiguous_write(&buf);
    assert(w1 == 524);
    memset(buf_write_ptr(&buf), 'Y', w1);
    buf_advance_tail(&buf, w1);
    assert(buf.tail == 0); // Wrapped around to 0!

    // Step 4: Write remaining space up to head - 1 (300 - 0 - 1 = 299 bytes)
    size_t w2 = buf_contiguous_write(&buf);
    assert(w2 == 299);
    memset(buf_write_ptr(&buf), 'Z', w2);
    buf_advance_tail(&buf, w2);
    assert(buf.tail == 299);
    assert(buf_contiguous_write(&buf) == 0); // Completely full
    assert(buf_available_data(&buf) == 1023);

    // Step 5: Read contiguous chunk to end (1024 - 300 = 724 bytes)
    size_t r1 = buf_contiguous_read(&buf);
    assert(r1 == 724);
    buf_advance_head(&buf, r1);
    assert(buf.head == 0);

    // Step 6: Read wrapped chunk (299 bytes)
    size_t r2 = buf_contiguous_read(&buf);
    assert(r2 == 299);
    buf_advance_head(&buf, r2);
    assert(buf.head == 299);
    assert(buf_available_data(&buf) == 0);
    assert(buf_available_space(&buf) == 1023);

    buf_free(&buf);
}

int main(void) {
    printf("[*] Running test_buffer: boundary and circular wrap tests...\n");
    test_boundary_wrap();
    test_circular_wrap_and_drain();
    printf("[PASS] All ring buffer boundary and circular tests passed.\n");
    return 0;
}
