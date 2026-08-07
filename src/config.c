#include "config.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// Internal helper: Strips leading and trailing whitespace/newlines from a string
static char *trim_whitespace(char *str) {
    char *end;
    // Trim leading space
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str; // All spaces

    // Trim trailing space
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    
    // Write new null terminator
    end[1] = '\0';
    return str;
}

int config_load(const char *filepath, GatewayConfig *config) {
    FILE *file = fopen(filepath, "r");
    if (!file) {
        LOG_ERROR("Could not open configuration file: %s", filepath);
        return -1;
    }

    // Zero out the entire structure to ensure clean defaults
    memset(config, 0, sizeof(GatewayConfig));

    // Set default values for configurable limits
    config->max_routes = MAX_ROUTES;
    config->max_backends = MAX_BACKENDS;
    config->max_active_connections = MAX_ACTIVE_CONNECTIONS;
    config->io_buffer_size = IO_BUFFER_SIZE;
    config->max_consecutive_failures = MAX_CONSECUTIVE_FAILURES;
    config->connection_idle_timeout_secs = CONNECTION_IDLE_TIMEOUT_SECS;

    char line[MAX_LINE_LEN];
    Route *current_route = NULL;
    int line_num = 0;
    int in_global_section = 1;  // Start in global section before first [route]

    while (fgets(line, sizeof(line), file)) {
        line_num++;
        char *trimmed = trim_whitespace(line);

        // Ignore empty lines and comments
        if (trimmed[0] == '\0' || trimmed[0] == '#') {
            continue;
        }

        // Check for a new section header: [route] or [global]
        if (trimmed[0] == '[') {
            if (strcmp(trimmed, "[route]") == 0) {
                in_global_section = 0;
                if (config->route_count >= config->max_routes) {
                    LOG_WARN("Max routes (%d) reached at line %d. Ignoring further routes.", config->max_routes, line_num);
                    current_route = NULL;
                    continue;
                }
                current_route = &config->routes[config->route_count];
                config->route_count++;
                // Inherit global max_consecutive_failures by default
                current_route->max_consecutive_failures = config->max_consecutive_failures;
                continue;
            } else if (strcmp(trimmed, "[global]") == 0) {
                in_global_section = 1;
                current_route = NULL;
                continue;
            } else {
                LOG_WARN("Unknown section '%s' on line %d. Ignoring.", trimmed, line_num);
                in_global_section = 0;
                current_route = NULL;
                continue;
            }
        }

        // Parse key = value
        char *key = strtok(trimmed, "=");
        char *val = strtok(NULL, "=");

        if (!key || !val) {
            LOG_WARN("Syntax error on line %d: Malformed key-value pair.", line_num);
            continue;
        }

        key = trim_whitespace(key);
        val = trim_whitespace(val);

        if (in_global_section) {
            // Global configuration options
            if (strcmp(key, "max_routes") == 0) {
                config->max_routes = atoi(val);
                if (config->max_routes > MAX_ROUTES) {
                    LOG_WARN("max_routes (%d) exceeds compile-time maximum (%d), capping.", config->max_routes, MAX_ROUTES);
                    config->max_routes = MAX_ROUTES;
                }
            } else if (strcmp(key, "max_backends") == 0) {
                config->max_backends = atoi(val);
                if (config->max_backends > MAX_BACKENDS) {
                    LOG_WARN("max_backends (%d) exceeds compile-time maximum (%d), capping.", config->max_backends, MAX_BACKENDS);
                    config->max_backends = MAX_BACKENDS;
                }
            } else if (strcmp(key, "max_active_connections") == 0) {
                config->max_active_connections = atoi(val);
            } else if (strcmp(key, "io_buffer_size") == 0) {
                config->io_buffer_size = atoi(val);
            } else if (strcmp(key, "max_consecutive_failures") == 0) {
                config->max_consecutive_failures = atoi(val);
            } else if (strcmp(key, "connection_idle_timeout_secs") == 0) {
                config->connection_idle_timeout_secs = atoi(val);
            } else {
                LOG_WARN("Unknown global configuration key '%s' on line %d.", key, line_num);
            }
        } else {
            // Route-specific configuration
            if (!current_route) {
                LOG_WARN("Syntax error on line %d: Property defined outside of a [route] block.", line_num);
                continue;
            }

            if (strcmp(key, "frontend_port") == 0) {
                current_route->frontend_port = atoi(val);
            } else if (strcmp(key, "backend") == 0) {
                if (current_route->backend_count >= config->max_backends) {
                    LOG_WARN("Max backends (%d) reached for port %d.", config->max_backends, current_route->frontend_port);
                    continue;
                }

                BackendServer *backend = &current_route->backends[current_route->backend_count];
                
                // Parse IP:PORT safely using width specifier %15[^:] to prevent buffer overflows!
                int parsed = sscanf(val, "%15[^:]:%d", backend->ip, &backend->port);
                if (parsed == 2) {
                    backend->is_alive = 1;
                    backend->active_connections = 0;
                    backend->consecutive_failures = 0;
                    backend->probe_fd = -1; // -1 indicates NO active health check socket
                    current_route->backend_count++;
                } else {
                    LOG_ERROR("Malformed backend target on line %d: %s (Expected format IP:PORT)", line_num, val);
                }
            } else if (strcmp(key, "max_consecutive_failures") == 0) {
                current_route->max_consecutive_failures = atoi(val);
            } else {
                LOG_WARN("Unknown configuration key '%s' on line %d.", key, line_num);
            }
        }
    }

    fclose(file);

    if (config->route_count == 0) {
        LOG_ERROR("No valid routing rules found in %s.", filepath);
        return -1;
    }

    LOG_INFO("Successfully loaded %d routing rule(s) from %s.", config->route_count, filepath);
    return 0;
}

void config_print(const GatewayConfig *config) {
    LOG_INFO("=== Active Gateway Routing Table ===");
    for (int i = 0; i < config->route_count; i++) {
        const Route *route = &config->routes[i];
        LOG_INFO("Route #%d: Listen on Port [%d] -> %d Backend(s) configured:", 
                 i + 1, route->frontend_port, route->backend_count);
        for (int j = 0; j < route->backend_count; j++) {
            const BackendServer *bs = &route->backends[j];
            LOG_INFO("   ├── Backend %d: %s:%d [Status: %s]", 
                     j + 1, bs->ip, bs->port, bs->is_alive ? "ALIVE" : "DOWN");
        }
    }
    LOG_INFO("====================================");
}