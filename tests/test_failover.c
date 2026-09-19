#define _POSIX_C_SOURCE 200809L
#include "router.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

int main(void) {
    printf("[*] Running test_failover: verifying router failover mechanics...\n");
    Route route;
    memset(&route, 0, sizeof(Route));
    route.frontend_port = 8080;
    route.backend_count = 3;
    route.max_consecutive_failures = 1;
    route.current_backend_idx = 0;

    for (int i = 0; i < 3; i++) {
        snprintf(route.backends[i].ip, sizeof(route.backends[i].ip), "127.0.0.1");
        route.backends[i].port = 9001 + i;
        route.backends[i].is_alive = 1;
        route.backends[i].consecutive_failures = 0;
        route.backends[i].probe_fd = -1;
    }

    // First selection
    const BackendServer *b1 = router_select_backend(&route);
    assert(b1 != NULL);
    assert(b1->port == 9001);

    // Simulate connection failure on b1
    router_mark_backend_down(&route.backends[0], route.max_consecutive_failures);
    assert(route.backends[0].is_alive == 0);

    // Next selection should automatically failover to healthy backend (b2)
    const BackendServer *b2 = router_select_backend(&route);
    assert(b2 != NULL);
    assert(b2->port == 9002);

    // Simulate failure on b2
    router_mark_backend_down(&route.backends[1], route.max_consecutive_failures);
    assert(route.backends[1].is_alive == 0);

    // Next selection should failover to b3
    const BackendServer *b3 = router_select_backend(&route);
    assert(b3 != NULL);
    assert(b3->port == 9003);

    printf("[PASS] test_failover unit test passed.\n");
    return 0;
}
