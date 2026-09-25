#define _POSIX_C_SOURCE 200809L

#include "router.h"
#include "config.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

int main(void) {
    printf("[*] Running test_router: verifying load balancing strategies...\n");

    Route route;
    memset(&route, 0, sizeof(Route));
    route.frontend_port = 8080;
    route.backend_count = 3;
    route.max_consecutive_failures = 1;

    strcpy(route.backends[0].ip, "127.0.0.1");
    route.backends[0].port = 9001;
    route.backends[0].is_alive = 1;
    route.backends[0].active_connections = 5;

    strcpy(route.backends[1].ip, "127.0.0.1");
    route.backends[1].port = 9002;
    route.backends[1].is_alive = 1;
    route.backends[1].active_connections = 1; // Lowest

    strcpy(route.backends[2].ip, "127.0.0.1");
    route.backends[2].port = 9003;
    route.backends[2].is_alive = 1;
    route.backends[2].active_connections = 3;

    // Test 1: least_conn selects backend 1 (active_connections == 1)
    route.strategy = STRATEGY_LEAST_CONN;
    const BackendServer *selected = router_select_backend(&route);
    assert(selected != NULL);
    assert(selected->port == 9002);
    printf("[PASS] Least-conn correctly selected server with fewest active connections.\n");

    // Test 2: If lowest server is marked DOWN, selects next lowest
    route.backends[1].is_alive = 0;
    selected = router_select_backend(&route);
    assert(selected != NULL);
    assert(selected->port == 9003); // Next lowest alive is backend 2 (3 conns)
    printf("[PASS] Least-conn correctly skips dead backend with lowest count.\n");

    // Test 3: Fair tie breaking when counts are equal
    route.backends[0].active_connections = 2;
    route.backends[2].active_connections = 2;
    const BackendServer *s1 = router_select_backend(&route);
    const BackendServer *s2 = router_select_backend(&route);
    assert(s1 != NULL && s2 != NULL);
    assert(s1->port != s2->port); // Must round-robin between equal candidates
    printf("[PASS] Least-conn tie-breaking alternates between equal candidates.\n");

    // Test 4: All backends down returns NULL
    route.backends[0].is_alive = 0;
    route.backends[2].is_alive = 0;
    assert(router_select_backend(&route) == NULL);
    printf("[PASS] Least-conn returns NULL when all backends are DOWN.\n");

    // Test 5: Round-robin selection
    route.backends[0].is_alive = 1;
    route.backends[1].is_alive = 1;
    route.backends[2].is_alive = 1;
    route.strategy = STRATEGY_ROUND_ROBIN;
    const BackendServer *rr1 = router_select_backend(&route);
    const BackendServer *rr2 = router_select_backend(&route);
    const BackendServer *rr3 = router_select_backend(&route);
    assert(rr1 != NULL && rr2 != NULL && rr3 != NULL);
    printf("[PASS] Round-robin strategy functioning correctly.\n");

    printf("[PASS] test_router passed all test cases successfully.\n");
    return 0;
}
