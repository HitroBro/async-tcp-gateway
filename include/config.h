#ifndef CONFIG_H
#define CONFIG_H

#include <arpa/inet.h>
#include <stdatomic.h>

// Default limits (can be overridden in config file)
#define MAX_ROUTES 10
#define MAX_BACKENDS 10
#define MAX_ACTIVE_CONNECTIONS 1024
#define IO_BUFFER_SIZE 8192
#define MAX_LINE_LEN 256
#define MAX_CONSECUTIVE_FAILURES 1  // Hard connection refusals trigger immediate failover
#define CONNECTION_IDLE_TIMEOUT_SECS 30  // Close connections idle for more than 30 seconds

// M3: Rate limiting defaults
#define DEFAULT_MAX_CONNECTIONS_PER_SEC 1000  // Global connection rate limit
#define DEFAULT_MAX_CONNECTIONS_PER_IP_PER_SEC 50  // Per-IP connection rate limit

// Absolute hard limits (compile-time) - config values cannot exceed these
#define HARD_MAX_ROUTES MAX_ROUTES
#define HARD_MAX_BACKENDS MAX_BACKENDS
#define HARD_MAX_ACTIVE_CONNECTIONS 65536  // Practical limit for dynamic allocation

// Represents a single destination backend server
typedef struct {
    char ip[INET_ADDRSTRLEN];
    int port;
    int is_alive; 
    int active_connections;
    int consecutive_failures;
    int probe_fd;
} BackendServer;

// Represents a routing rule from a frontend port to one or more backends
typedef struct {
    int frontend_port;
    BackendServer backends[MAX_BACKENDS];
    int backend_count;
    _Atomic int current_backend_idx;  // Atomic for thread-safe round-robin (C11)
    int max_consecutive_failures;  // Per-route failure threshold (falls back to global)
} Route;

// M3: Simple token bucket for rate limiting
typedef struct {
    _Atomic int tokens;
    int max_tokens;
    int refill_rate_per_sec;
    time_t last_refill;
} TokenBucket;

// The complete gateway configuration state
typedef struct {
    Route routes[MAX_ROUTES];
    int route_count;
    
    // Configurable limits (parsed from config file)
    int max_routes;
    int max_backends;
    int max_active_connections;
    int io_buffer_size;
    int max_consecutive_failures;
    int connection_idle_timeout_secs;
    
    // M3: Rate limiting configuration
    int max_connections_per_sec;
    int max_connections_per_ip_per_sec;
} GatewayConfig;

int config_load(const char *filepath, GatewayConfig *config);
void config_print(const GatewayConfig *config);

#endif // CONFIG_H