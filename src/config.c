#include <strings.h>
#include "config.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <arpa/inet.h>

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

// Internal helper: Parses routing strategy string into RoutingStrategy enum
static __attribute__((unused)) int parse_routing_strategy(const char *str, RoutingStrategy *out_strategy) {
    if (!str || !out_strategy) return -1;
    if (strcasecmp(str, "round_robin") == 0 || strcasecmp(str, "round-robin") == 0) {
        *out_strategy = STRATEGY_ROUND_ROBIN;
        return 0;
    }
    if (strcasecmp(str, "least_conn") == 0 || strcasecmp(str, "least-conn") == 0 ||
        strcasecmp(str, "least_connections") == 0) {
        *out_strategy = STRATEGY_LEAST_CONN;
        return 0;
    }
    return -1;
}

// Internal helper: Safe integer parsing with strtol
// Returns 0 on success, -1 on error. Sets *out_val on success.
static int parse_int(const char *str, int *out_val, int min_val, int max_val) {
    char *endptr;
    errno = 0;
    long val = strtol(str, &endptr, 10);
    
    if (errno != 0 || endptr == str || *endptr != '\0') {
        return -1; // Not a valid integer
    }
    if (val < min_val || val > max_val) {
        return -1; // Out of range
    }
    *out_val = (int)val;
    return 0;
}

// Parses backend address strings supporting [ipv6]:port, ipv4:port, or host:port
static int parse_backend_endpoint(const char *str, char *out_ip, size_t ip_len, int *out_port) {
    if (!str || !out_ip || !out_port || ip_len == 0) return -1;

    if (str[0] == '[') {
        // Bracketed IPv6 notation: [2001:db8::1]:8080
        const char *close_bracket = strchr(str, ']');
        if (!close_bracket || close_bracket[1] != ':') {
            return -1; // Malformed bracketed address
        }
        size_t addr_len = (size_t)(close_bracket - str - 1);
        if (addr_len == 0 || addr_len >= ip_len) {
            return -1; // Address too long or empty
        }
        memcpy(out_ip, str + 1, addr_len);
        out_ip[addr_len] = '\0';

        // Validate IPv6 syntax using inet_pton
        struct in6_addr a6;
        if (inet_pton(AF_INET6, out_ip, &a6) != 1) {
            return -1; // Invalid IPv6 format
        }
        return parse_int(close_bracket + 2, out_port, 1, 65535);
    }

    // Standard host:port or ipv4:port
    const char *colon = strrchr(str, ':');
    if (!colon || colon == str) {
        return -1;
    }
    size_t addr_len = (size_t)(colon - str);
    if (addr_len == 0 || addr_len >= ip_len) {
        return -1;
    }
    memcpy(out_ip, str, addr_len);
    out_ip[addr_len] = '\0';

    return parse_int(colon + 1, out_port, 1, 65535);
}

// Parses backend address with optional weight=N suffix (e.g., 127.0.0.1:9001 weight=3)
static __attribute__((unused)) int parse_backend_endpoint_weighted(const char *str, char *out_ip, size_t ip_len, int *out_port, int *out_weight) {
    if (out_weight) *out_weight = 1; // Default weight
    char buf[MAX_LINE_LEN];
    strncpy(buf, str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *weight_token = strstr(buf, "weight=");
    if (weight_token) {
        int w = 1;
        if (parse_int(weight_token + 7, &w, 1, 1000) == 0 && out_weight) {
            *out_weight = w;
        }
        *weight_token = '\0';
        while (weight_token > buf && isspace((unsigned char)*(weight_token - 1))) {
            *(--weight_token) = '\0';
        }
    }
    return parse_backend_endpoint(buf, out_ip, ip_len, out_port);
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
    config->default_strategy = STRATEGY_ROUND_ROBIN;
    // M3: Rate limiting defaults
    config->max_connections_per_sec = DEFAULT_MAX_CONNECTIONS_PER_SEC;
    config->max_connections_per_ip_per_sec = DEFAULT_MAX_CONNECTIONS_PER_IP_PER_SEC;

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
                current_route->strategy = config->default_strategy;
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
                        if (parse_int(val, &config->max_routes, 1, MAX_ROUTES) != 0) {
                            LOG_WARN("Invalid max_routes value '%s' on line %d. Using default %d.", val, line_num, MAX_ROUTES);
                        } else if (config->max_routes > MAX_ROUTES) {
                            LOG_WARN("max_routes (%d) exceeds compile-time maximum (%d), capping.", config->max_routes, MAX_ROUTES);
                            config->max_routes = MAX_ROUTES;
                        }
                    } else if (strcmp(key, "max_backends") == 0) {
                        if (parse_int(val, &config->max_backends, 1, MAX_BACKENDS) != 0) {
                            LOG_WARN("Invalid max_backends value '%s' on line %d. Using default %d.", val, line_num, MAX_BACKENDS);
                        } else if (config->max_backends > MAX_BACKENDS) {
                            LOG_WARN("max_backends (%d) exceeds compile-time maximum (%d), capping.", config->max_backends, MAX_BACKENDS);
                            config->max_backends = MAX_BACKENDS;
                        }
                    } else if (strcmp(key, "max_active_connections") == 0) {
                        if (parse_int(val, &config->max_active_connections, 1, HARD_MAX_ACTIVE_CONNECTIONS) != 0) {
                            LOG_WARN("Invalid max_active_connections value '%s' on line %d. Using default %d.", val, line_num, MAX_ACTIVE_CONNECTIONS);
                        }
                    } else if (strcmp(key, "io_buffer_size") == 0) {
                        if (parse_int(val, &config->io_buffer_size, 1024, 1048576) != 0) {
                            LOG_WARN("Invalid io_buffer_size value '%s' on line %d. Using default %d (allowed range: 1024 to 1048576).", val, line_num, IO_BUFFER_SIZE);
                        } else {
                            LOG_INFO("Configured dynamic connection buffer size: %d bytes", config->io_buffer_size);
                        }
                    } else if (strcmp(key, "max_consecutive_failures") == 0) {
                        if (parse_int(val, &config->max_consecutive_failures, 1, 100) != 0) {
                            LOG_WARN("Invalid max_consecutive_failures value '%s' on line %d. Using default %d.", val, line_num, MAX_CONSECUTIVE_FAILURES);
                        }
                    } else if (strcmp(key, "connection_idle_timeout_secs") == 0) {
                        if (parse_int(val, &config->connection_idle_timeout_secs, 1, 86400) != 0) {
                            LOG_WARN("Invalid connection_idle_timeout_secs value '%s' on line %d. Using default %d.", val, line_num, CONNECTION_IDLE_TIMEOUT_SECS);
                        }
                    } else if (strcmp(key, "max_connections_per_sec") == 0) {
                        if (parse_int(val, &config->max_connections_per_sec, 1, 100000) != 0) {
                            LOG_WARN("Invalid max_connections_per_sec value '%s' on line %d. Using default %d.", val, line_num, DEFAULT_MAX_CONNECTIONS_PER_SEC);
                        }
                    } else if (strcmp(key, "max_connections_per_ip_per_sec") == 0) {
                        if (parse_int(val, &config->max_connections_per_ip_per_sec, 1, 10000) != 0) {
                            LOG_WARN("Invalid max_connections_per_ip_per_sec value '%s' on line %d. Using default %d.", val, line_num, DEFAULT_MAX_CONNECTIONS_PER_IP_PER_SEC);
                        }
                    } else if (strcmp(key, "strategy") == 0) {
                        if (parse_routing_strategy(val, &config->default_strategy) != 0) {
                            LOG_WARN("Invalid global strategy '%s' on line %d. Defaulting to round_robin.", val, line_num);
                        } else {
                            LOG_INFO("Configured global default routing strategy: %s", val);
                        }
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
                int port;
                if (parse_int(val, &port, 1, 65535) != 0) {
                    LOG_ERROR("Invalid frontend_port '%s' on line %d (must be 1-65535)", val, line_num);
                } else {
                    current_route->frontend_port = port;
                }
            } else if (strcmp(key, "backend") == 0) {
                if (current_route->backend_count >= config->max_backends) {
                    LOG_WARN("Max backends (%d) reached for port %d.", config->max_backends, current_route->frontend_port);
                    continue;
                }

                BackendServer *backend = &current_route->backends[current_route->backend_count];
                
                int weight = 1;
                int parsed = parse_backend_endpoint_weighted(val, backend->ip, sizeof(backend->ip), &backend->port, &weight);
                if (parsed == 0) {
                    LOG_DEBUG("Parsed backend target: %s:%d (weight: %d)", backend->ip, backend->port, weight);
                    backend->is_alive = 1;
                    backend->weight = weight;
                    backend->current_weight = 0;
                    backend->effective_weight = weight;
                    backend->active_connections = 0;
                    backend->consecutive_failures = 0;
                    backend->probe_fd = -1;
                    current_route->backend_count++;
                } else {
                    LOG_ERROR("Malformed backend target on line %d: %s (Expected format [IPv6]:PORT or IP:PORT)", line_num, val);
                }
            } else if (strcmp(key, "max_consecutive_failures") == 0) {
                if (parse_int(val, &current_route->max_consecutive_failures, 1, 100) != 0) {
                    LOG_WARN("Invalid max_consecutive_failures value '%s' on line %d. Using default %d.", val, line_num, MAX_CONSECUTIVE_FAILURES);
                }
            } else if (strcmp(key, "strategy") == 0) {
                if (parse_routing_strategy(val, &current_route->strategy) != 0) {
                    LOG_WARN("Invalid route strategy '%s' on line %d. Using default.", val, line_num);
                } else {
                    LOG_INFO("Configured route on port %d strategy: %s", current_route->frontend_port, val);
                }
            } else {
                LOG_WARN("Unknown configuration key '%s' on line %d.", key, line_num);
            }
        }
    }

    fclose(file);

    // C4 FIX: Validate config limits against hard maximums and sensible minimums
    if (config->max_active_connections <= 0) {
        LOG_WARN("max_active_connections (%d) invalid, using default %d", config->max_active_connections, MAX_ACTIVE_CONNECTIONS);
        config->max_active_connections = MAX_ACTIVE_CONNECTIONS;
    } else if (config->max_active_connections > HARD_MAX_ACTIVE_CONNECTIONS) {
        LOG_WARN("max_active_connections (%d) exceeds hard limit (%d), capping.", config->max_active_connections, HARD_MAX_ACTIVE_CONNECTIONS);
        config->max_active_connections = HARD_MAX_ACTIVE_CONNECTIONS;
    }

    if (config->max_consecutive_failures <= 0) {
        LOG_WARN("max_consecutive_failures (%d) invalid, using default %d", config->max_consecutive_failures, MAX_CONSECUTIVE_FAILURES);
        config->max_consecutive_failures = MAX_CONSECUTIVE_FAILURES;
    }

    if (config->connection_idle_timeout_secs <= 0) {
        LOG_WARN("connection_idle_timeout_secs (%d) invalid, using default %d", config->connection_idle_timeout_secs, CONNECTION_IDLE_TIMEOUT_SECS);
        config->connection_idle_timeout_secs = CONNECTION_IDLE_TIMEOUT_SECS;
    }

    if (config->io_buffer_size <= 0) {
        LOG_WARN("io_buffer_size (%d) invalid, using default %d", config->io_buffer_size, IO_BUFFER_SIZE);
        config->io_buffer_size = IO_BUFFER_SIZE;
    }

    // M3: Validate rate limiting config
    if (config->max_connections_per_sec <= 0) {
        LOG_WARN("max_connections_per_sec (%d) invalid, using default %d", config->max_connections_per_sec, DEFAULT_MAX_CONNECTIONS_PER_SEC);
        config->max_connections_per_sec = DEFAULT_MAX_CONNECTIONS_PER_SEC;
    }

    if (config->max_connections_per_ip_per_sec <= 0) {
        LOG_WARN("max_connections_per_ip_per_sec (%d) invalid, using default %d", config->max_connections_per_ip_per_sec, DEFAULT_MAX_CONNECTIONS_PER_IP_PER_SEC);
        config->max_connections_per_ip_per_sec = DEFAULT_MAX_CONNECTIONS_PER_IP_PER_SEC;
    }

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
    LOG_INFO("Rate limiting: %d conn/sec global, %d conn/sec per IP",
             config->max_connections_per_sec, config->max_connections_per_ip_per_sec);
    LOG_INFO("====================================");
}