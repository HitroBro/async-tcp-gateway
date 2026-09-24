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

static int token_bucket_consume_entry(TokenBucket *tb, time_t now) {
    if (now > tb->last_refill) {
        int elapsed = (int)(now - tb->last_refill);
        int refill = elapsed * tb->refill_rate_per_sec;
        int new_tokens = tb->tokens + refill;
        if (new_tokens > tb->max_tokens) new_tokens = tb->max_tokens;
        atomic_store(&tb->tokens, new_tokens);
        tb->last_refill = now;
    }
    
    int current = atomic_load(&tb->tokens);
    if (current > 0) {
        atomic_fetch_sub(&tb->tokens, 1);
        return 1; // Token consumed
    }
    return 0; // No tokens available
}

static int compare_addr(int af, const struct sockaddr_storage *ss, const IPRateEntry *entry) {
    if (af != entry->af) return 0;
    if (af == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)ss;
        return memcmp(&sin->sin_addr, &entry->addr.ipv4, sizeof(struct in_addr)) == 0;
    } else if (af == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)ss;
        return memcmp(&sin6->sin6_addr, &entry->addr.ipv6, sizeof(struct in6_addr)) == 0;
    }
    return 0;
}

int ip_ratelimit_check(IPRateLimiter *limiter, const struct sockaddr_storage *client_addr, int rate_per_sec) {
    if (!limiter || !client_addr) return 1;
    if (client_addr->ss_family != AF_INET && client_addr->ss_family != AF_INET6) {
        return 1;
    }

    time_t now = time(NULL);
    uint32_t hash = hash_sockaddr(client_addr);
    // Optimized fast power-of-two bitmask indexing
    size_t idx = (size_t)(hash & (RATE_LIMIT_HASH_SIZE - 1));

    // Search existing bucket
    IPRateEntry *curr = limiter->table[idx];
    while (curr) {
        if (compare_addr(client_addr->ss_family, client_addr, curr)) {
            curr->last_seen = now;
            return token_bucket_consume_entry(&curr->bucket, now);
        }
        curr = curr->next;
    }

    // Check capacity and sweep or evict if full
    if (limiter->total_entries >= limiter->max_entries) {
        ip_ratelimit_sweep_idle(limiter, now);
        if (limiter->total_entries >= limiter->max_entries) {
            // Evict oldest node at head of current bucket
            if (limiter->table[idx]) {
                IPRateEntry *evicted = limiter->table[idx];
                limiter->table[idx] = evicted->next;
                free(evicted);
                limiter->total_entries--;
            }
        }
    }

    // New IP: allocate and insert
    IPRateEntry *entry = (IPRateEntry *)malloc(sizeof(IPRateEntry));
    if (!entry) {
        LOG_WARN("Memory allocation failed for rate limiter entry, allowing connection");
        return 1;
    }

    entry->af = client_addr->ss_family;
    if (entry->af == AF_INET) {
        entry->addr.ipv4 = ((const struct sockaddr_in *)client_addr)->sin_addr;
    } else {
        entry->addr.ipv6 = ((const struct sockaddr_in6 *)client_addr)->sin6_addr;
    }

    entry->bucket.tokens = rate_per_sec;
    entry->bucket.max_tokens = rate_per_sec;
    entry->bucket.refill_rate_per_sec = rate_per_sec;
    entry->bucket.last_refill = now;
    entry->last_seen = now;

    entry->next = limiter->table[idx];
    limiter->table[idx] = entry;
    limiter->total_entries++;

    return token_bucket_consume_entry(&entry->bucket, now);
}

void ip_ratelimit_sweep_idle(IPRateLimiter *limiter, time_t now) {
    if (!limiter) return;
    int reclaimed = 0;

    for (size_t i = 0; i < RATE_LIMIT_HASH_SIZE; i++) {
        IPRateEntry **curr_ptr = &limiter->table[i];
        while (*curr_ptr) {
            IPRateEntry *entry = *curr_ptr;
            if (now - entry->last_seen > limiter->ttl_secs) {
                *curr_ptr = entry->next;
                free(entry);
                limiter->total_entries--;
                reclaimed++;
            } else {
                curr_ptr = &entry->next;
            }
        }
    }
    if (reclaimed > 0) {
        LOG_DEBUG("Rate limiter swept %d idle entries (active: %d)", reclaimed, limiter->total_entries);
    }
}

void ip_ratelimit_cleanup(IPRateLimiter *limiter) {
    if (!limiter) return;
    for (size_t i = 0; i < RATE_LIMIT_HASH_SIZE; i++) {
        IPRateEntry *curr = limiter->table[i];
        while (curr) {
            IPRateEntry *next = curr->next;
            free(curr);
            curr = next;
        }
        limiter->table[i] = NULL;
    }
    limiter->total_entries = 0;
}
