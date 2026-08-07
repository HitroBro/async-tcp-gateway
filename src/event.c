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
#include <netdb.h>

#define MAX_EVENTS 64

// Listener context to map listener FD to its route
typedef struct {
    int fd;
    const Route *route;
} ListenerContext;

// Internal forward declarations for event processing helpers
static void handle_listener_event(int epoll_fd, EndpointToken *token, const GatewayConfig *config);
static void handle_proxy_event(int epoll_fd, EndpointToken *token, uint32_t events);
static int  initiate_backend_connection(int epoll_fd, ConnectionContext *ctx);
static void process_socket_read(int epoll_fd, ConnectionContext *ctx, int from_fd, int to_fd, IOBuffer *buf);
static void process_socket_write(int epoll_fd, ConnectionContext *ctx, int to_fd, int from_fd, IOBuffer *buf);
static void update_epoll_interests(int epoll_fd, EndpointToken *token, uint32_t base_events, IOBuffer *buf);

int event_loop_run(const GatewayConfig *config) {
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        LOG_ERROR("Fatal: epoll_create1 failed: %s", strerror(errno));
        return -1;
    }

    // Create and register a listener for each route
    for (int r = 0; r < config->route_count; r++) {
        const Route *route = &config->routes[r];
        int listener_fd = net_create_listener(route->frontend_port);
        if (listener_fd < 0) {
            LOG_ERROR("Fatal: Failed to start listener on port %d.", route->frontend_port);
            close(epoll_fd);
            return -1;
        }
        if (net_set_nonblocking(listener_fd) < 0) {
            close(listener_fd);
            close(epoll_fd);
            return -1;
        }

        // Store route info in listener token
        EndpointToken *listener_token = malloc(sizeof(EndpointToken));
        if (!listener_token) {
            LOG_ERROR("Fatal: Failed to allocate listener token for port %d.", route->frontend_port);
            close(listener_fd);
            close(epoll_fd);
            return -1;
        }
        listener_token->fd = listener_fd;
        listener_token->role = ROLE_LISTENER;
        listener_token->parent = (ConnectionContext *)route; // Store route pointer in parent field

        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.ptr = listener_token;

        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listener_fd, &ev) < 0) {
            LOG_ERROR("Fatal: Failed to add listener to epoll for port %d.", route->frontend_port);
            free(listener_token);
            close(listener_fd);
            close(epoll_fd);
            return -1;
        }
        LOG_INFO("Gateway listener bound to port %d.", route->frontend_port);
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
    sigaddset(&mask, SIGUSR1);  // For metrics dump
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
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = &timer_token;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, timer_fd, &ev) < 0) {
        LOG_ERROR("Fatal: Failed to add timerfd to epoll: %s", strerror(errno));
        close(timer_fd);
        close(epoll_fd);
        return -1;
    }

    EndpointToken sig_token = { .fd = sig_fd, .role = ROLE_SIGNAL, .parent = NULL };
    memset(&ev, 0, sizeof(ev));
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
            EndpointToken *token = (EndpointToken *)events[i].data.ptr;

            if (token->role == ROLE_TIMER) {
                uint64_t expirations = 0;
                ssize_t bytes_read = read(timer_fd, &expirations, sizeof(expirations));
                if (bytes_read == sizeof(expirations)) {
                    LOG_INFO("Background timer tick detected (expirations: %lu).", (unsigned long)expirations);
                    router_sweep_health_probes(epoll_fd, (GatewayConfig *)config);
                    conn_context_sweep_idle(epoll_fd);  // Check for idle connections
                }
            } else if (token->role == ROLE_SIGNAL) {
                struct signalfd_siginfo fdsi;
                ssize_t s = read(sig_fd, &fdsi, sizeof(fdsi));
                if (s == sizeof(fdsi)) {
                    if (fdsi.ssi_signo == SIGUSR1) {
                        LOG_INFO("Metrics dump signal (SIGUSR1) received.");
                        metrics_dump(config);
                    } else {
                        LOG_INFO("Shutdown signal (%d) received. Initiating graceful teardown...", fdsi.ssi_signo);
                        running = 0;
                        break;
                    }
                }
            } else if (token->role == ROLE_HEALTH_PROBE) {
                router_handle_probe_event(epoll_fd, token, active_events);
            } else if (token->role == ROLE_LISTENER) {
                // Listener socket (parent stores Route*)
                handle_listener_event(epoll_fd, token, config);
            } else {
                // Client or backend socket
                handle_proxy_event(epoll_fd, token, active_events);
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
            }
        }
    }

    // Clean up listener tokens
    // Note: We can't easily iterate them, but they'll be closed when we close epoll_fd
    // For proper cleanup, we'd need to track them. For now, just close epoll.

    conn_context_destroy_all(epoll_fd);

    close(sig_fd);
    close(timer_fd);
    close(epoll_fd);
    return 0;
}

static void handle_listener_event(int epoll_fd, EndpointToken *token, const GatewayConfig *config) {
    const Route *route = (const Route *)token->parent;
    int listener_fd = token->fd;
    
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listener_fd, (struct sockaddr *)&client_addr, &client_len);
        
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break; // Ingestion queue drained
            LOG_ERROR("Accept failed on port %d: %s", route->frontend_port, strerror(errno));
            break;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        LOG_INFO("Accepted asynchronous connection from Client %s:%d (FD: %d) on port %d",
                 client_ip, ntohs(client_addr.sin_port), client_fd, route->frontend_port);

        if (net_set_nonblocking(client_fd) < 0) {
            close(client_fd);
            continue;
        }

        // Use the route this listener is bound to
        ConnectionContext *ctx = conn_context_create(client_fd, route, config);
        if (!ctx) {
            close(client_fd);
            continue;
        }

        // Register client socket inside epoll using custom user-space pointer architecture
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN; // Listen for data from client
        ev.data.ptr = &ctx->client_token;

        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
            LOG_ERROR("Failed to add client to epoll.");
            conn_context_destroy(epoll_fd, ctx);
            continue;
        }

        // Immediately trigger the background connection sequence to the backend server
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

        struct addrinfo hints, *res, *rp;
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", target->port);

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = 0;

        int ret = getaddrinfo(target->ip, port_str, &hints, &res);
        if (ret != 0) {
            LOG_ERROR("getaddrinfo failed for %s:%d: %s", target->ip, target->port, gai_strerror(ret));
            close(fd);
            ctx->backend_fd = -1;
            continue;
        }

        int connected = 0;
        for (rp = res; rp != NULL; rp = rp->ai_next) {
            // We already created the socket, so we need to connect with the right family
            if (connect(fd, rp->ai_addr, rp->ai_addrlen) < 0) {
                if (errno != EINPROGRESS) {
                    LOG_DEBUG("Connection attempt to %s:%d failed (%s), trying next address",
                              target->ip, target->port, strerror(errno));
                    continue;
                }
            }
            connected = 1;
            break;
        }

        freeaddrinfo(res);

        if (!connected) {
            LOG_WARN("Immediate connect failure to %s:%d. Triggering failover...",
                     target->ip, target->port);
            
            router_mark_backend_down(ctx->target_backend, ctx->route->max_consecutive_failures);
            close(fd);
            ctx->backend_fd = -1;
            
            continue; // Loop around and instantly try the next healthy backend in the pool!
        }

        // If we got here, either connect succeeded immediately or EINPROGRESS
        if (errno == EINPROGRESS || errno == 0) {
            // Check if it succeeded immediately
            if (errno == 0) {
                // Immediate connection finalized (common on local loopback sockets)
                ctx->state = CONN_STATE_ESTABLISHED;
                router_report_backend_success(ctx->target_backend);
                LOG_INFO("Immediate connection established to %s:%d.", target->ip, target->port);
            }
        }

        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN | EPOLLOUT; // Monitor for readability (errors) and writability (handshake done)
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
        if (token->role == ROLE_BACKEND && ctx->target_backend && ctx->route) {
            router_mark_backend_down(ctx->target_backend, ctx->route->max_consecutive_failures);
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
               
                            router_mark_backend_down(ctx->target_backend, ctx->route->max_consecutive_failures);
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
        size_t space = buf_contiguous_write(buf);
        if (space == 0) {
            EndpointToken *from_token = (from_fd == ctx->client_fd) ? &ctx->client_token : &ctx->backend_token;

            update_epoll_interests(epoll_fd, from_token, 0, buf);
            break;
        }

        ssize_t bytes = recv(from_fd, buf_write_ptr(buf), space, 0);
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

        buf_advance_tail(buf, bytes);
        ctx->last_activity = time(NULL);  // Update last activity timestamp on read
        metrics_add_bytes_read(bytes);
        
        // Optimize: Attempt immediate transmission pass to maximize network throughput
        process_socket_write(epoll_fd, ctx, to_fd, from_fd, buf);
        if (ctx->state == CONN_STATE_CLOSING) return;
    }
}

static void process_socket_write(int epoll_fd, ConnectionContext *ctx, int to_fd, int from_fd, IOBuffer *buf) {
    while (buf_available_data(buf) > 0) {
        size_t contig_read = buf_contiguous_read(buf);
        ssize_t bytes = send(to_fd, buf_read_ptr(buf), contig_read, MSG_NOSIGNAL);
        
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

        buf_advance_head(buf, bytes);
        ctx->last_activity = time(NULL);  // Update last activity timestamp on write
        metrics_add_bytes_written(bytes);
    }

    // Buffer fully drained! Reset alignment tracking back to zero offsets
    buf_reset(buf);

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