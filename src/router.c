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
#define MAX_CONSECUTIVE_FAILURES 1 // Hard connection refusals trigger immediate failover

const BackendServer *router_select_backend(const Route *route) {
    if (!route || route->backend_count == 0) {
        LOG_ERROR("Routing failure: Route contains zero configured backends.");
        return NULL;
    }

    Route *mutable_route = (Route *)route;

    // Bounded O(N) search loop: We check at most 'backend_count' times to find a healthy server.
    // This prevents infinite loops if every single server in the cluster is marked DOWN!
    for (int i = 0; i < route->backend_count; i++) {
        unsigned int raw_idx = (unsigned int)atomic_fetch_add((_Atomic int *)&mutable_route->current_backend_idx, 1);
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

void router_mark_backend_down(BackendServer *backend) {
    if (!backend || !backend->is_alive) return;

    backend->consecutive_failures++;
    LOG_WARN("Connection failure recorded for Backend %s:%d (Consecutive failures: %d)", 
             backend->ip, backend->port, backend->consecutive_failures);

    if (backend->consecutive_failures >= MAX_CONSECUTIVE_FAILURES) {
        backend->is_alive = 0; // Evict from active Round-Robin rotation
        LOG_ERROR("Backend %s:%d exceeded failure threshold! Marked DOWN.", 
                  backend->ip, backend->port);
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

            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) {
                LOG_ERROR("Health probe socket creation failed for %s:%d: %s", 
                          backend->ip, backend->port, strerror(errno));
                continue;
            }

            if (net_set_nonblocking(fd) < 0) {
                close(fd);
                continue;
            }

            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(backend->port);
            if (inet_pton(AF_INET, backend->ip, &addr.sin_addr) <= 0) {
                LOG_ERROR("Invalid IP address for probe: %s", backend->ip);
                close(fd);
                continue;
            }

            LOG_INFO("Initiating background health probe to DOWN server %s:%d (Probe FD: %d)...", 
                     backend->ip, backend->port, fd);

            if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                if (errno != EINPROGRESS) {
                    // Immediate hard refusal (e.g., Host Unreachable / Connection Refused)
                    LOG_DEBUG("Health probe immediate failure to %s:%d (%s). Will retry next tick.", 
                              backend->ip, backend->port, strerror(errno));
                    close(fd);
                    continue; // Leave probe_fd == -1 so next timer tick tries again
                }
            } else {
                // Immediate connection success (common on loopback / localhost)!
                LOG_INFO("Health probe immediately verified! Restoring %s:%d to ALIVE status.", 
                         backend->ip, backend->port);
                router_report_backend_success(backend);
                close(fd);
                continue;
            }

            // Asynchronous handshake in progress: Allocate ephemeral token and register in epoll
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