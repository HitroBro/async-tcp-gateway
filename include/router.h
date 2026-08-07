#ifndef ROUTER_H
#define ROUTER_H

#include "config.h"
#include "gateway.h"

// Selects the next healthy target backend using atomic Round-Robin.
// Skips servers where is_alive == 0. Returns NULL if all servers are DOWN.
const BackendServer *router_select_backend(const Route *route);

// Records a connection failure against a backend. If consecutive failures
// reach the threshold, marks the server as DOWN (is_alive = 0).
void router_mark_backend_down(BackendServer *backend);

// Records a successful handshake, resetting failure counters and marking ALIVE.
void router_report_backend_success(BackendServer *backend);

// Sweeps through all routes and initiates asynchronous TCP health checks
// against any idle backend servers marked as DOWN (is_alive == 0).
void router_sweep_health_probes(int epoll_fd, GatewayConfig *config);

// Processes epoll events (EPOLLOUT, EPOLLERR, EPOLLHUP) on active health probe
// sockets to verify connection recovery and restore healthy nodes.
void router_handle_probe_event(int epoll_fd, EndpointToken *token, uint32_t events);

#endif // ROUTER_H