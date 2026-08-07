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

    char line[MAX_LINE_LEN];
    Route *current_route = NULL;
    int line_num = 0;

    while (fgets(line, sizeof(line), file)) {
        line_num++;
        char *trimmed = trim_whitespace(line);

        // Ignore empty lines and comments
        if (trimmed[0] == '\0' || trimmed[0] == '#') {
            continue;
        }

        // Check for a new section header: [route]
        if (strcmp(trimmed, "[route]") == 0) {
            if (config->route_count >= MAX_ROUTES) {
                LOG_WARN("Max routes (%d) reached at line %d. Ignoring further routes.", MAX_ROUTES, line_num);
                current_route = NULL;
                continue;
            }
            current_route = &config->routes[config->route_count];
            config->route_count++;
            continue;
        }

        // If we haven't encountered a [route] tag yet, key-value pairs are invalid
        if (!current_route) {
            LOG_WARN("Syntax error on line %d: Property defined outside of a [route] block.", line_num);
            continue;
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

        if (strcmp(key, "frontend_port") == 0) {
            current_route->frontend_port = atoi(val);
        } 
        else if (strcmp(key, "backend") == 0) {
            if (current_route->backend_count >= MAX_BACKENDS) {
                LOG_WARN("Max backends (%d) reached for port %d.", MAX_BACKENDS, current_route->frontend_port);
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
        } 
        else {
            LOG_WARN("Unknown configuration key '%s' on line %d.", key, line_num);
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