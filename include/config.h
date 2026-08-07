#ifndef CONFIG_H
#define CONFIG_H

#include <arpa/inet.h>

#define MAX_ROUTES 10
#define MAX_BACKENDS 10
#define MAX_LINE_LEN 256

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
} Route;

// The complete gateway configuration state
typedef struct {
    Route routes[MAX_ROUTES];
    int route_count;
} GatewayConfig;

int config_load(const char *filepath, GatewayConfig *config);
void config_print(const GatewayConfig *config);

#endif // CONFIG_H