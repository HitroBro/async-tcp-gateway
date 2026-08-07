#include "gateway.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <time.h>
#include <inttypes.h>

#define MAX_DEFERRED_CLEANUP 64
static ConnectionContext *deferred_cleanup_queue[MAX_DEFERRED_CLEANUP];
static int deferred_count = 0;

// Maximum active connections - using compile-time max for array size,
// actual limit enforced via config->max_active_connections
#define MAX_ACTIVE_CONNECTIONS 1024
static ConnectionContext *active_connections[MAX_ACTIVE_CONNECTIONS];
static int active_count = 0;

// Global metrics
GatewayMetrics g_metrics = {0};

ConnectionContext *conn_context_create(int client_fd, const Route *route, const GatewayConfig *config) {
    ConnectionContext *ctx = (ConnectionContext *)malloc(sizeof(ConnectionContext));
    if (!ctx) {
        LOG_ERROR("Memory allocation failed for ConnectionContext.");
        return NULL;
    }

    memset(ctx, 0, sizeof(ConnectionContext));
    ctx->client_fd = client_fd;
    ctx->backend_fd = -1;
    ctx->state = CONN_STATE_CONNECTING;
    ctx->route = route;
    ctx->last_activity = time(NULL);  // Initialize last activity timestamp

    ctx->client_token.fd = client_fd;
    ctx->client_token.role = ROLE_CLIENT;
    ctx->client_token.parent = ctx;

    ctx->backend_token.fd = -1;
    ctx->backend_token.role = ROLE_BACKEND;
    ctx->backend_token.parent = ctx;

    // Track active connection for idle timeout sweep
    if (active_count < config->max_active_connections) {
        active_connections[active_count++] = ctx;
    }

    // Update metrics
    g_metrics.total_connections++;
    g_metrics.active_connections++;

    return ctx;
}

void conn_context_destroy(int epoll_fd, ConnectionContext *ctx) {
    if (!ctx || ctx->state == CONN_STATE_CLOSING) return;

    LOG_INFO("Tearing down asynchronous proxy bridge. Cleaning up resources.");
    ctx->state = CONN_STATE_CLOSING;

    // Remove from active_connections array
    for (int i = 0; i < active_count; i++) {
        if (active_connections[i] == ctx) {
            active_connections[i] = active_connections[--active_count];
            break;
        }
    }

    if (ctx->client_fd >= 0) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ctx->client_fd, NULL);
        close(ctx->client_fd);
        LOG_DEBUG("Closed Client FD %d", ctx->client_fd);
        ctx->client_fd = -1;
    }

    if (ctx->backend_fd >= 0) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ctx->backend_fd, NULL);
        close(ctx->backend_fd);
        LOG_DEBUG("Closed Backend FD %d", ctx->backend_fd);
        ctx->backend_fd = -1;
    }

    g_metrics.active_connections--;

    if (deferred_count < MAX_DEFERRED_CLEANUP) {
        deferred_cleanup_queue[deferred_count++] = ctx;
    } else {
        LOG_WARN("Deferred cleanup queue full; executing fallback immediate free.");
        free(ctx);
    }
}

void conn_context_destroy_all(int epoll_fd) {
    LOG_INFO("Initiating graceful teardown of all %d active connection bridges...", active_count);
    while (active_count > 0) {
        conn_context_destroy(epoll_fd, active_connections[active_count - 1]);
    }
    conn_context_sweep_cleanup();
}

void conn_context_sweep_cleanup(void) {
    for (int i = 0; i < deferred_count; i++) {
        free(deferred_cleanup_queue[i]);
    }
    deferred_count = 0;
}

void conn_context_sweep_idle(int epoll_fd) {
    time_t now = time(NULL);
    for (int i = 0; i < active_count; ) {
        ConnectionContext *ctx = active_connections[i];
        if (ctx->state == CONN_STATE_ESTABLISHED) {
            if (now - ctx->last_activity > CONNECTION_IDLE_TIMEOUT_SECS) {
                LOG_INFO("Closing idle connection: Client FD %d, Backend FD %d (idle %ld secs)",
                         ctx->client_fd, ctx->backend_fd, now - ctx->last_activity);
                conn_context_destroy(epoll_fd, ctx);
                // Don't increment i, conn_context_destroy already removed this element
                continue;
            }
        }
        i++;
    }
}

// Metrics functions
void metrics_increment_connections(void) {
    g_metrics.total_connections++;
    g_metrics.active_connections++;
}

void metrics_decrement_connections(void) {
    if (g_metrics.active_connections > 0) {
        g_metrics.active_connections--;
    }
}

void metrics_increment_failed(void) {
    g_metrics.failed_connections++;
}

void metrics_add_bytes_read(uint64_t bytes) {
    g_metrics.total_bytes_read += bytes;
}

void metrics_add_bytes_written(uint64_t bytes) {
    g_metrics.total_bytes_written += bytes;
}

void metrics_increment_backend_failures(void) {
    g_metrics.backend_failures++;
}

void metrics_increment_health_probes(void) {
    g_metrics.health_probes++;
}

void metrics_increment_health_probe_success(void) {
    g_metrics.health_probe_successes++;
}

void metrics_dump(const GatewayConfig *config) {
    LOG_INFO("=== GATEWAY METRICS ===");
    LOG_INFO("Total connections accepted: %" PRIu64, g_metrics.total_connections);
    LOG_INFO("Currently active connections: %" PRIu64, g_metrics.active_connections);
    LOG_INFO("Failed connections: %" PRIu64, g_metrics.failed_connections);
    LOG_INFO("Total bytes read: %" PRIu64, g_metrics.total_bytes_read);
    LOG_INFO("Total bytes written: %" PRIu64, g_metrics.total_bytes_written);
    LOG_INFO("Backend failures: %" PRIu64, g_metrics.backend_failures);
    LOG_INFO("Health probes initiated: %" PRIu64, g_metrics.health_probes);
    LOG_INFO("Health probe successes: %" PRIu64, g_metrics.health_probe_successes);
    
    if (config) {
        LOG_INFO("--- Route Status ---");
        for (int r = 0; r < config->route_count; r++) {
            const Route *route = &config->routes[r];
            LOG_INFO("Route #%d (port %d): %d backends", r + 1, route->frontend_port, route->backend_count);
            for (int b = 0; b < route->backend_count; b++) {
                const BackendServer *bs = &route->backends[b];
                LOG_INFO("  Backend %d: %s:%d [Status: %s, Active conns: %d, Failures: %d]",
                         b + 1, bs->ip, bs->port, 
                         bs->is_alive ? "ALIVE" : "DOWN",
                         bs->active_connections, bs->consecutive_failures);
            }
        }
    }
    LOG_INFO("========================");
}