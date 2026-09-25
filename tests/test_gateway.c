#define _POSIX_C_SOURCE 200809L

#include "gateway.h"
#include "config.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>

int main(void) {
    printf("[*] Running test_gateway: verifying O(1) connection indexing and swap-and-pop...\n");

    GatewayConfig config;
    config.max_active_connections = 100;
    config.io_buffer_size = 4096;
    config.connection_idle_timeout_secs = 30;

    Route route;
    route.frontend_port = 8080;
    route.backend_count = 1;
    route.backends[0].is_alive = 1;
    route.backends[0].port = 9001;

    ConnectionContext *c1 = conn_context_create(10, &route, &config);
    ConnectionContext *c2 = conn_context_create(11, &route, &config);
    ConnectionContext *c3 = conn_context_create(12, &route, &config);

    assert(c1 != NULL && c2 != NULL && c3 != NULL);
    assert(c1->active_index == 0);
    assert(c2->active_index == 1);
    assert(c3->active_index == 2);

    // Destroy middle element (c2 at index 1)
    // c3 (tail) should be swapped into index 1
    conn_context_destroy(-1, c2);
    assert(c2->active_index == -1);
    assert(c3->active_index == 1);
    assert(c1->active_index == 0);

    // Destroy head element (c1 at index 0)
    // c3 should be swapped into index 0
    conn_context_destroy(-1, c1);
    assert(c1->active_index == -1);
    assert(c3->active_index == 0);

    // Destroy remaining element (c3 at index 0)
    conn_context_destroy(-1, c3);
    assert(c3->active_index == -1);

    conn_context_sweep_cleanup();
    printf("[PASS] test_gateway O(1) swap-and-pop tests passed successfully.\n");
    return 0;
}
