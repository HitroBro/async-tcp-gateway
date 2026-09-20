#define _POSIX_C_SOURCE 200809L
#include "ratelimit.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>

// 32-bit FNV-1a hash function across address bytes
static uint32_t hash_sockaddr(const struct sockaddr_storage *ss) {
    uint32_t hash = 2166136261u;
    const unsigned char *bytes = NULL;
    size_t len = 0;

    if (ss->ss_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)ss;
        bytes = (const unsigned char *)&sin->sin_addr.s_addr;
        len = sizeof(sin->sin_addr.s_addr);
    } else if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)ss;
        bytes = (const unsigned char *)&sin6->sin6_addr.s6_addr;
        len = sizeof(sin6->sin6_addr.s6_addr);
    } else {
        return 0;
    }

    for (size_t i = 0; i < len; i++) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

void ip_ratelimit_init(IPRateLimiter *limiter, int max_entries, time_t ttl_secs) {
    if (!limiter) return;
    memset(limiter->table, 0, sizeof(limiter->table));
    limiter->total_entries = 0;
    limiter->max_entries = (max_entries > 0) ? max_entries : RATE_LIMIT_MAX_ENTRIES;
    limiter->ttl_secs = (ttl_secs > 0) ? ttl_secs : RATE_LIMIT_DEFAULT_TTL;
}
