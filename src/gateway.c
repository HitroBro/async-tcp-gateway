#include "gateway.h"
#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>

#define MAX_DEFERRED_CLEANUP 64
static ConnectionContext *deferred_cleanup_queue[MAX_DEFERRED_CLEANUP];
static int deferred_count = 0;

#define MAX_ACTIVE_CONNECTIONS 1024
static ConnectionContext *active_connections[MAX_ACTIVE_CONNECTIONS];
static int active_count = 0;

ConnectionContext *conn_context_create(int client_fd, const Route *route) {
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

    ctx->client_token.fd = client_fd;
    ctx->client_token.role = ROLE_CLIENT;
    ctx->client_token.parent = ctx;

    ctx->backend_token.fd = -1;
    ctx->backend_token.role = ROLE_BACKEND;
    ctx->backend_token.parent = ctx;

    if (active_count < MAX_ACTIVE_CONNECTIONS) {
        active_connections[active_count++] = ctx;
    }

    return ctx;
}

void conn_context_destroy(int epoll_fd, ConnectionContext *ctx) {
    if (!ctx || ctx->state == CONN_STATE_CLOSING) return;

    LOG_INFO("Tearing down asynchronous proxy bridge. Cleaning up resources.");
    ctx->state = CONN_STATE_CLOSING;

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