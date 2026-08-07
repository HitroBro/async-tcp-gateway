#define _POSIX_C_SOURCE 200809L

#include "event.h"
#include "net.h"
#include "gateway.h"
#include "router.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/signalfd.h>
#include <signal.h>

#define MAX_EVENTS 64

// Internal forward declarations for event processing helpers
static void handle_listener_event(int epoll_fd, int listener_fd, const GatewayConfig *config);
static void handle_proxy_event(int epoll_fd, EndpointToken *token, uint32_t events);
static int  initiate_backend_connection(int epoll_fd, ConnectionContext *ctx);
static void process_socket_read(int epoll_fd, ConnectionContext *ctx, int from_fd, int to_fd, IOBuffer *buf);
static void process_socket_write(int epoll_fd, ConnectionContext *ctx, int to_fd, int from_fd, IOBuffer *buf);
static void update_epoll_interests(int epoll_fd, EndpointToken *token, uint32_t base_events, IOBuffer *buf);

int event_loop_run(int listener_fd, const GatewayConfig *config) {
    if (net_set_nonblocking(listener_fd) < 0) return -1;

    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        LOG_ERROR("Fatal: epoll_create1 failed: %s", strerror(errno));
        return -1;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = listener_fd; // For the listener, we safely use legacy .fd matching

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listener_fd, &ev) < 0) {
        LOG_ERROR("Fatal: Failed to add listener to epoll.");
        close(epoll_fd);
        return -1;
    }

    int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (timer_fd < 0) {
        LOG_ERROR("Fatal: timerfd_create failed: %s", strerror(errno));
        close(epoll_fd);
        return -1;
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
        LOG_ERROR("Fatal: sigprocmask failed: %s", strerror(errno));
        close(timer_fd);
        close(epoll_fd);
        return -1;
    }

    int sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sig_fd < 0) {
        LOG_ERROR("Fatal: signalfd failed: %s", strerror(errno));
        close(timer_fd);
        close(epoll_fd);
        return -1;
    }

    struct itimerspec ts;
    ts.it_interval.tv_sec = 5;
    ts.it_interval.tv_nsec = 0;
    ts.it_value.tv_sec = 5;
    ts.it_value.tv_nsec = 0;
    if (timerfd_settime(timer_fd, 0, &ts, NULL) < 0) {
        LOG_ERROR("Fatal: timerfd_settime failed: %s", strerror(errno));
        close(timer_fd);
        close(epoll_fd);
        return -1;
    }

EndpointToken timer_token = { .fd = timer_fd, .role = ROLE_TIMER, .parent = NULL };
 ev.events = EPOLLIN;
 ev.data.ptr = &timer_token;
 if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, timer_fd, &ev) < 0) {
     LOG_ERROR("Fatal: Failed to add timerfd to epoll: %s", strerror(errno));
     close(timer_fd);
     close(epoll_fd);
     return -1;
 }

 EndpointToken sig_token = { .fd = sig_fd, .role = ROLE_SIGNAL, .parent = NULL };
 ev.events = EPOLLIN;
 ev.data.ptr = &sig_token;
 if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sig_fd, &ev) < 0) {
     LOG_ERROR("Fatal: Failed to add signalfd to epoll: %s", strerror(errno));
     close(sig_fd);
     close(timer_fd);
     close(epoll_fd);
     return -1;
 }

 LOG_INFO("Asynchronous multi-client event loop fully initialized.");
 struct epoll_event events[MAX_EVENTS];
 int running = 1;

while (running) {
        int n_ready = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (n_ready < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n_ready && running; i++) {
            uint32_t active_events = events[i].events;

            // Scenario 1: Activity on our primary structural listening socket
            if (events[i].data.fd == listener_fd) {
                handle_listener_event(epoll_fd, listener_fd, config);
            } 
            // Scenario 2: Activity on a mapped client, backend, timer, or signal endpoint
            else {
                EndpointToken *token = (EndpointToken *)events[i].data.ptr;

                if (token->role == ROLE_TIMER) {
                  uint64_t expirations = 0;
                  ssize_t bytes_read = read(timer_fd, &expirations, sizeof(expirations));
                  if (bytes_read == sizeof(expirations)) {
                      LOG_INFO("Background timer tick detected (expirations: %lu).", (unsigned long)expirations);
                      router_sweep_health_probes(epoll_fd, (GatewayConfig *)config);
                  }
              } else if (token->role == ROLE_SIGNAL) {
                  struct signalfd_siginfo fdsi;
                  ssize_t s = read(sig_fd, &fdsi, sizeof(fdsi));
                  if (s == sizeof(fdsi)) {
                      LOG_INFO("Shutdown signal (%d) received. Initiating graceful teardown...", fdsi.ssi_signo);
                  }
                  running = 0;
                  break;
              } else if (token->role == ROLE_HEALTH_PROBE) {
                  router_handle_probe_event(epoll_fd, token, active_events);
              } else {
                  handle_proxy_event(epoll_fd, token, active_events);
              }
            }
        }
        conn_context_sweep_cleanup();
    }

    LOG_INFO("Event loop terminated. Reclaiming active health probe descriptors...");
    for (int r = 0; r < config->route_count; r++) {
        for (int b = 0; b < config->routes[r].backend_count; b++) {
            if (config->routes[r].backends[b].probe_fd >= 0) {
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, config->routes[r].backends[b].probe_fd, NULL);
                close(config->routes[r].backends[b].probe_fd);
                // config->routes[r].backends[b].probe_fd = -1;
            }
        }
    }

    conn_context_destroy_all(epoll_fd);

    close(sig_fd);
    close(timer_fd);
    close(epoll_fd);
    return 0;
}

static void handle_listener_event(int epoll_fd, int listener_fd, const GatewayConfig *config) {
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listener_fd, (struct sockaddr *)&client_addr, &client_len);
        
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break; // Ingestion queue drained
            LOG_ERROR("Accept failed: %s", strerror(errno));
            break;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        LOG_INFO("Accepted asynchronous connection from Client %s:%d (FD: %d)", 
                 client_ip, ntohs(client_addr.sin_port), client_fd);

        if (net_set_nonblocking(client_fd) < 0) {
            close(client_fd);
            continue;
        }

        // Target route selection (Milestone 4 uses primary configuration route index 0)[cite: 1]
        ConnectionContext *ctx = conn_context_create(client_fd, &config->routes[0]);
        if (!ctx) {
            close(client_fd);
            continue;
        }

        // Register client socket inside epoll using custom user-space pointer architecture
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN; // Listen for data from client[cite: 1]
        // ev.data.ptr = ctx;   // Attach the memory context block!
        ev.data.ptr = &ctx->client_token;

        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
            LOG_ERROR("Failed to add client to epoll.");
            conn_context_destroy(epoll_fd, ctx);
            continue;
        }

        // Immediately trigger the background connection sequence to the backend server[cite: 1]
        if (initiate_backend_connection(epoll_fd, ctx) < 0) {
            conn_context_destroy(epoll_fd, ctx);
        }
    }
}

static int initiate_backend_connection(int epoll_fd, ConnectionContext *ctx) {
    // We wrap our connection initiator in a retry loop. If our first Round-Robin target
    // fails immediately (e.g., ECONNREFUSED), we mark it DOWN and instantly try the next healthy one!
    while (1) {
        const BackendServer *target = router_select_backend(ctx->route);
        if (!target) {
            LOG_ERROR("Failover exhausted: No healthy backends remain for Client FD %d.", ctx->client_fd);
            return -1;
        }

        // Store the target pointer in our context so we know who we are talking to if async errors occur!
        ctx->target_backend = (BackendServer *)target;

        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            LOG_ERROR("Socket creation failed during backend initiation: %s", strerror(errno));
            return -1;
        }

        if (net_set_nonblocking(fd) < 0) {
            close(fd);
            return -1;
        }

        ctx->backend_fd = fd;
        ctx->backend_token.fd = fd;

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(target->port);
        inet_pton(AF_INET, target->ip, &addr.sin_addr);

        LOG_DEBUG("Initiating connection to target %s:%d (Backend FD: %d)", target->ip, target->port, fd);

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            if (errno != EINPROGRESS) {
                // IMMEDIATE HARD FAILURE (e.g., Connection Refused / Host Unreachable)
                LOG_WARN("Immediate connect failure to %s:%d (%s). Triggering failover...", 
                         target->ip, target->port, strerror(errno));
                
                router_mark_backend_down(ctx->target_backend); // Flag passive failure[cite: 1]
                close(fd);
                ctx->backend_fd = -1;
                
                continue; // Loop around and instantly try the next healthy backend in the pool![cite: 1]
            }
            // If errno == EINPROGRESS, the TCP handshake is proceeding asynchronously in the kernel.
        } else {
            // Immediate connection finalized (common on local loopback sockets)
            ctx->state = CONN_STATE_ESTABLISHED;
            router_report_backend_success(ctx->target_backend);
            LOG_INFO("Immediate connection established to %s:%d.", target->ip, target->port);
        }

        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN | EPOLLOUT; // Monitor for readability (errors) and writability (handshake done)
        // ev.data.ptr = ctx;
        ev.data.ptr = &ctx->backend_token;

        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            LOG_ERROR("Failed to add Backend FD %d to epoll: %s", fd, strerror(errno));
            close(fd);
            ctx->backend_fd = -1;
            return -1;
        }

        return 0; // Successfully initiated connection monitoring!
    }
}

static void handle_proxy_event(int epoll_fd, EndpointToken *token, uint32_t events) {
    ConnectionContext *ctx = token->parent;
    int ready_fd = token->fd;

    if (ctx->state == CONN_STATE_CLOSING) return;

    // Direct Error Trap Handlers
    if (events & (EPOLLERR | EPOLLHUP)) {
        LOG_WARN("Socket hardware hangup/error detected on FD %d.", ready_fd);
        if (token->role == ROLE_BACKEND && ctx->target_backend) {
            router_mark_backend_down(ctx->target_backend);
        }
        conn_context_destroy(epoll_fd, ctx);
        return;
    }

    // PHASE 1: Asynchronous Handshake Verification & Failover Logic
    if (ctx->state == CONN_STATE_CONNECTING) {
        if (token->role == ROLE_BACKEND && (events & EPOLLOUT)) {
            int socket_error = 0;
            socklen_t len = sizeof(socket_error);

            if (getsockopt(ready_fd, SOL_SOCKET, SO_ERROR, &socket_error, &len) < 0 || socket_error != 0) {
                LOG_WARN("Asynchronous handshake rejected by %s:%d. Triggering failover...",
                         ctx->target_backend->ip, ctx->target_backend->port);
                
                router_mark_backend_down(ctx->target_backend);
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ctx->backend_fd, NULL);
                close(ctx->backend_fd);
                ctx->backend_fd = -1;
                ctx->backend_token.fd = -1;

                if (initiate_backend_connection(epoll_fd, ctx) < 0) {
                    conn_context_destroy(epoll_fd, ctx);
                }
                return;
            }

            router_report_backend_success(ctx->target_backend);
            ctx->state = CONN_STATE_ESTABLISHED;
            LOG_INFO("Asynchronous bridge verified. Session active: Client FD %d <===> Backend FD %d",
                     ctx->client_fd, ctx->backend_fd);

            // Pass our tokens to our newly repaired update helper!
            update_epoll_interests(epoll_fd, &ctx->backend_token, EPOLLIN, &ctx->backend_to_client);
            update_epoll_interests(epoll_fd, &ctx->client_token, EPOLLIN, &ctx->client_to_backend);
            return;
        }
    }

    // PHASE 2: Standard Bidirectional Stream Data Processing Loop
    if (token->role == ROLE_CLIENT) {
        if (events & EPOLLIN)  process_socket_read(epoll_fd, ctx, ctx->client_fd, ctx->backend_fd, &ctx->client_to_backend);
        if (events & EPOLLOUT) process_socket_write(epoll_fd, ctx, ctx->client_fd, ctx->backend_fd, &ctx->backend_to_client);
    } 
    else if (token->role == ROLE_BACKEND) {
        if (events & EPOLLIN)  process_socket_read(epoll_fd, ctx, ctx->backend_fd, ctx->client_fd, &ctx->backend_to_client);
        if (events & EPOLLOUT) process_socket_write(epoll_fd, ctx, ctx->backend_fd, ctx->client_fd, &ctx->client_to_backend);
    }
}

static void process_socket_read(int epoll_fd, ConnectionContext *ctx, int from_fd, int to_fd, IOBuffer *buf) {
    while (1) {
        size_t space = buf_available_space(buf);
        if (space == 0) {
            EndpointToken *from_token = (from_fd == ctx->client_fd) ? &ctx->client_token : &ctx->backend_token;

            update_epoll_interests(epoll_fd, from_token, 0, buf);
            break;
}

        ssize_t bytes = recv(from_fd, buf->data + buf->tail, space, 0);
        if (bytes == 0) {
            LOG_INFO("Graceful stream close detected via connection endpoint FD %d.", from_fd);
            conn_context_destroy(epoll_fd, ctx);
            return;
        }
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break; // Input queue fully drained
            if (errno == EINTR) continue;
            conn_context_destroy(epoll_fd, ctx);
            return;
        }

        buf->tail += bytes;
        
        // Optimize: Attempt immediate transmission pass to maximize network throughput
        process_socket_write(epoll_fd, ctx, to_fd, from_fd, buf);
        if (ctx->state == CONN_STATE_CLOSING) return;
    }
}

static void process_socket_write(int epoll_fd, ConnectionContext *ctx, int to_fd, int from_fd, IOBuffer *buf) {
    while (buf_available_data(buf) > 0) {
        ssize_t bytes = send(to_fd, buf->data + buf->head, buf_available_data(buf), MSG_NOSIGNAL);
        
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Kernel transmit buffer congested! Register interest in writable notifications
                EndpointToken *to_token = (to_fd == ctx->client_fd) ? &ctx->client_token : &ctx->backend_token;

                update_epoll_interests(epoll_fd, to_token, EPOLLIN | EPOLLOUT, buf);

                return;
            }
            if (errno == EINTR) continue;
            conn_context_destroy(epoll_fd, ctx);
            return;
        }

        buf->head += bytes;
    }

    // Buffer fully drained! Reset alignment tracking back to zero offsets
    buf->head = 0;
    buf->tail = 0;

    // Remove corporate interest in writable alerts to protect against looping spikes
    EndpointToken *to_token   = (to_fd == ctx->client_fd)   ? &ctx->client_token : &ctx->backend_token;
    EndpointToken *from_token = (from_fd == ctx->client_fd) ? &ctx->client_token : &ctx->backend_token;

    update_epoll_interests(epoll_fd, to_token, EPOLLIN, buf);
    update_epoll_interests(epoll_fd, from_token, EPOLLIN, buf);
}

static void update_epoll_interests(int epoll_fd, EndpointToken *token, uint32_t base_events, IOBuffer *buf) {
    if (!token || token->fd < 0) return;

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = base_events;
    
    // If our outgoing memory buffer contains trapped bytes, actively request writable notifications!
    if (buf && buf_available_data(buf) > 0) {
        ev.events |= EPOLLOUT;
    }

    ev.data.ptr = token; // Maintain our token pointer linkage in the kernel union!

    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, token->fd, &ev) < 0) {
        LOG_ERROR("Failed to update epoll interests on FD %d: %s", token->fd, strerror(errno));
    }
}