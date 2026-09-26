#define _POSIX_C_SOURCE 200809L

#include "router.h"
#include "logger.h"
#include "net.h"
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <netdb.h>

// Smooth Weighted Round-Robin (Nginx-style)
const BackendServer *router_select_backend_wrr(const Route *route) {
    if (!route || route->backend_count == 0) return NULL;
    Route *mutable_route = (Route *)route;

    int total_weight = 0;
    BackendServer *best = NULL;

    for (int i = 0; i < route->backend_count; i++) {
        BackendServer *s = &mutable_route->backends[i];
        if (!s->is_alive) continue;

        s->current_weight += s->effective_weight;
        total_weight += s->effective_weight;

        if (best == NULL || s->current_weight > best->current_weight) {
            best = s;
        }
    }

    if (!best || total_weight <= 0) {
        LOG_ERROR("All backends for port %d are DOWN or have zero weight!", route->frontend_port);
        return NULL;
    }

    best->current_weight -= total_weight;
    LOG_DEBUG("WRR selected Backend -> %s:%d (current weight: %d, total: %d)",
              best->ip, best->port, best->current_weight, total_weight);
    return best;
}

const BackendServer *router_select_backend(const Route *route) {
    if (!route || route->backend_count == 0) {
        LOG_ERROR("Routing failure: Route contains zero configured backends.");
        return NULL;
    }
    if (route->strategy == STRATEGY_LEAST_CONN) {
        return router_select_backend_least_conn(route);
    }
    return router_select_backend_round_robin(route);
}

const BackendServer *router_select_backend_least_conn(const Route *route) {
    if (!route || route->backend_count == 0) {
        LOG_ERROR("Routing failure: Route contains zero configured backends.");
        return NULL;
    }

    int min_conns = -1;
    int candidates[MAX_BACKENDS];
    int candidate_count = 0;

    for (int i = 0; i < route->backend_count; i++) {
        const BackendServer *candidate = &route->backends[i];
        if (!candidate->is_alive) continue;

        if (min_conns < 0 || candidate->active_connections < min_conns) {
            min_conns = candidate->active_connections;
            candidates[0] = i;
            candidate_count = 1;
        } else if (candidate->active_connections == min_conns) {
            candidates[candidate_count++] = i;
        }
    }

    if (candidate_count == 0) {
        LOG_ERROR("All %d configured backends for port %d are currently DOWN!",
                  route->backend_count, route->frontend_port);
        return NULL;
    }

    // Fair tie-breaking: if multiple backends have identical minimum active connections,
    // round-robin between them using an atomic counter
    Route *mutable_route = (Route *)route;
    unsigned int raw_idx = (unsigned int)atomic_fetch_add(&mutable_route->current_backend_idx, 1);
    int selected_idx = candidates[raw_idx % (unsigned int)candidate_count];
    const BackendServer *selected = &route->backends[selected_idx];
    LOG_DEBUG("Least-Conn selected Backend #%d -> %s:%d (active conns: %d, candidates: %d)",
              selected_idx + 1, selected->ip, selected->port, selected->active_connections, candidate_count);
    return selected;
}

// Round-robin backend selection algorithm
const BackendServer *router_select_backend_round_robin(const Route *route) {
    if (!route || route->backend_count == 0) {
        LOG_ERROR("Routing failure: Route contains zero configured backends.");
        return NULL;
    }

    Route *mutable_route = (Route *)route;

    // Bounded O(N) search loop: We check at most 'backend_count' times to find a healthy server.
    // This prevents infinite loops if every single server in the cluster is marked DOWN!
    for (int i = 0; i < route->backend_count; i++) {
        // C1 FIX: Use atomic_fetch_add on _Atomic int field directly (no cast needed)
        unsigned int raw_idx = (unsigned int)atomic_fetch_add(&mutable_route->current_backend_idx, 1);
        unsigned int selected_idx = raw_idx % (unsigned int)route->backend_count;

        BackendServer *candidate = &mutable_route->backends[selected_idx];
        
        // Only return the candidate if our passive health tracker says it is ALIVE!
        if (candidate->is_alive) {
            LOG_DEBUG("Round-Robin selected Backend #%u -> %s:%d (Raw counter: %u)",
                      selected_idx + 1, candidate->ip, candidate->port, raw_idx);
            return candidate;
        }
        
        LOG_DEBUG("Skipping unhealthy Backend #%u (%s:%d) - Marked DOWN.",
                  selected_idx + 1, candidate->ip, candidate->port);
    }

    LOG_ERROR("All %d configured backends for port %d are currently DOWN!",
              route->backend_count, route->frontend_port);
    return NULL;
}

void router_mark_backend_down(BackendServer *backend, int max_consecutive_failures) {
    if (!backend || !backend->is_alive) return;

    backend->consecutive_failures++;
    LOG_WARN("Connection failure recorded for Backend %s:%d (Consecutive failures: %d)",
             backend->ip, backend->port, backend->consecutive_failures);
    metrics_increment_backend_failures();

    if (backend->consecutive_failures >= max_consecutive_failures) {
        backend->is_alive = 0; // Evict from active Round-Robin rotation
        LOG_ERROR("Backend %s:%d exceeded failure threshold (%d/%d)! Marked DOWN.",
                  backend->ip, backend->port, backend->consecutive_failures, max_consecutive_failures);
    }
}

void router_report_backend_success(BackendServer *backend) {
    if (!backend) return;

    // If it was previously struggling or marked down, celebrate the recovery!
    if (backend->consecutive_failures > 0 || !backend->is_alive) {
        LOG_INFO("Backend %s:%d responded successfully! Restoring to ALIVE status.",
                 backend->ip, backend->port);
    }
    
    backend->consecutive_failures = 0;
    backend->is_alive = 1;
}

void router_sweep_health_probes(int epoll_fd, GatewayConfig *config) {
    if (!config) return;

    for (int r = 0; r < config->route_count; r++) {
        Route *route = &config->routes[r];
        for (int b = 0; b < route->backend_count; b++) {
            BackendServer *backend = &route->backends[b];

            // Only probe servers that are marked DOWN (is_alive == 0)
            // and do NOT already have an active probe in flight (probe_fd == -1).
            if (backend->is_alive || backend->probe_fd != -1) {
                continue;
            }

            // M2: Use shared async connect helper
            int fd = -1;
            int ret = net_connect_async(backend->ip, backend->port, &fd);
            if (ret == -1) {
                LOG_DEBUG("Health probe immediate failure to %s:%d. Will retry next tick.",
                          backend->ip, backend->port);
                if (fd >= 0) close(fd);
                continue; // Leave probe_fd == -1 so next timer tick tries again
            } else if (ret == 0) {
                // Immediate connection success (common on loopback / localhost)!
                LOG_INFO("Health probe immediately verified! Restoring %s:%d to ALIVE status.",
                         backend->ip, backend->port);
                router_report_backend_success(backend);
                metrics_increment_health_probes();
                metrics_increment_health_probe_success();
                close(fd);
                continue;
            }

            // Asynchronous handshake in progress: Allocate ephemeral token and register in epoll
            metrics_increment_health_probes();
            EndpointToken *probe_token = malloc(sizeof(EndpointToken));
            if (!probe_token) {
                LOG_ERROR("Memory allocation failed for health probe token (%s:%d)", 
                          backend->ip, backend->port);
                close(fd);
                continue;
            }

            probe_token->fd = fd;
            probe_token->role = ROLE_HEALTH_PROBE;
            probe_token->backend = backend; // Links cleanly via our C11 anonymous union!

            struct epoll_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.events = EPOLLOUT | EPOLLIN; // Monitor for writable (handshake complete) or errors
            ev.data.ptr = probe_token;

            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
                LOG_ERROR("Failed to add probe FD %d to epoll: %s", fd, strerror(errno));
                free(probe_token);
                close(fd);
                continue;
            }

            // Lock the backend state so subsequent timer ticks don't launch overlapping probes
            backend->probe_fd = fd;
        }
    }
}

void router_handle_probe_event(int epoll_fd, EndpointToken *token, uint32_t events) {
    if (!token || !token->backend) return;

    BackendServer *backend = token->backend;
    int fd = token->fd;

    // C2 FIX: Nullify token FD immediately to prevent use-after-free if epoll returns stale event
    token->fd = -1;

    // 1. Instantly unregister the socket from epoll to prevent duplicate event notifications
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);

    int socket_error = 0;
    socklen_t len = sizeof(socket_error);

    // 2. Check for epoll error flags OR inspect SO_ERROR to verify the asynchronous 3-way handshake
    if ((events & (EPOLLERR | EPOLLHUP)) || 
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &len) < 0 || 
        socket_error != 0) {
        
        LOG_DEBUG("Background probe failed for %s:%d (Error: %s). Node remains DOWN.",
                  backend->ip, backend->port, socket_error ? strerror(socket_error) : "Hangup/Error");
    } else {
        // Handshake successfully completed! The server has recovered!
        LOG_INFO("Asynchronous health probe verified! Restoring %s:%d to active rotation.",
                 backend->ip, backend->port);
        router_report_backend_success(backend);
    }

    // 3. Clean up operational resources and unlock the probe state
    close(fd);
    backend->probe_fd = -1;
    free(token);
}