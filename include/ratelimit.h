#ifndef RATELIMIT_H
#define RATELIMIT_H

#include "config.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>
#include <stddef.h>

#define RATE_LIMIT_HASH_SIZE 1024
#define RATE_LIMIT_MAX_ENTRIES 8192
#define RATE_LIMIT_DEFAULT_TTL 60

typedef struct IPRateEntry {
    int af; // AF_INET or AF_INET6
    union {
        struct in_addr ipv4;
        struct in6_addr ipv6;
    } addr;
    TokenBucket bucket;
    time_t last_seen;
    struct IPRateEntry *next;
} IPRateEntry;

typedef struct {
    IPRateEntry *table[RATE_LIMIT_HASH_SIZE];
    int total_entries;
    int max_entries;
    time_t ttl_secs;
} IPRateLimiter;

void ip_ratelimit_init(IPRateLimiter *limiter, int max_entries, time_t ttl_secs);
int ip_ratelimit_check(IPRateLimiter *limiter, const struct sockaddr_storage *client_addr, int rate_per_sec);
void ip_ratelimit_sweep_idle(IPRateLimiter *limiter, time_t now);
void ip_ratelimit_cleanup(IPRateLimiter *limiter);

#endif // RATELIMIT_H
