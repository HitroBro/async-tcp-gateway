#define _POSIX_C_SOURCE 200809L
#include "ratelimit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <arpa/inet.h>

int main(void) {
    printf("[*] Running test_ratelimit: verifying dual-stack rate limiting...\n");
    IPRateLimiter limiter;
    ip_ratelimit_init(&limiter, 100, 10);

    // Test IPv4
    struct sockaddr_storage ss4;
    memset(&ss4, 0, sizeof(ss4));
    struct sockaddr_in *sin4 = (struct sockaddr_in *)&ss4;
    sin4->sin_family = AF_INET;
    inet_pton(AF_INET, "192.168.1.50", &sin4->sin_addr);

    // Limit = 5 conn/sec
    for (int i = 0; i < 5; i++) {
        assert(ip_ratelimit_check(&limiter, &ss4, 5) == 1);
    }
    // 6th should be rejected
    assert(ip_ratelimit_check(&limiter, &ss4, 5) == 0);

    // Test IPv6 independence
    struct sockaddr_storage ss6;
    memset(&ss6, 0, sizeof(ss6));
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&ss6;
    sin6->sin6_family = AF_INET6;
    inet_pton(AF_INET6, "2001:db8::1", &sin6->sin6_addr);

    // IPv6 should have its own separate tokens
    for (int i = 0; i < 5; i++) {
        assert(ip_ratelimit_check(&limiter, &ss6, 5) == 1);
    }
    assert(ip_ratelimit_check(&limiter, &ss6, 5) == 0);

    assert(limiter.total_entries == 2);
    ip_ratelimit_cleanup(&limiter);
    assert(limiter.total_entries == 0);
    // Test TTL sweeping
    IPRateLimiter ttl_limiter;
    ip_ratelimit_init(&ttl_limiter, 100, 5); // 5-second TTL
    assert(ip_ratelimit_check(&ttl_limiter, &ss4, 10) == 1);
    assert(ttl_limiter.total_entries == 1);

    // Immediate sweep should retain recent entry
    ip_ratelimit_sweep_idle(&ttl_limiter, time(NULL));
    assert(ttl_limiter.total_entries == 1);

    // Simulated sweep after TTL expiry (now + 10s)
    ip_ratelimit_sweep_idle(&ttl_limiter, time(NULL) + 10);
    assert(ttl_limiter.total_entries == 0);
    ip_ratelimit_cleanup(&ttl_limiter);


    printf("[PASS] test_ratelimit unit tests passed.\n");
    return 0;
}
