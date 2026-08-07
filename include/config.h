#ifndef CONFIG_H
#define CONFIG_H

#include <arpa/inet.h>

// Default limits (can be overridden in config file)
#define MAX_ROUTES 10
#define MAX_BACKENDS 10
#define MAX_ACTIVE_CONNECTIONS 1024
#define IO_BUFFER_SIZE 8192
#define MAX_LINE_LEN 256
#define MAX_CONSECUTIVE_FAILURES 1  // Hard connection refusals trigger immediate failover
#define CONNECTION_IDLE_TIMEOUT_SECS 30  // Close connections idle for more than 30 seconds

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
    int current_backend_idx;  
    int max_consecutive_failures;  // Per-route failure threshold (falls back to global)
} Route;

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
} GatewayConfig;

int config_load(const char *filepath, GatewayConfig *config);
void config_print(const GatewayConfig *config);

#endif // CONFIG_H