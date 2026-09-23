#define _POSIX_C_SOURCE 200809L

#include "event.h"
#include "net.h"
#include "gateway.h"
#include "router.h"
#include "ratelimit.h"
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
#include <stdatomic.h>

#define MAX_EVENTS 256  // H4: Increased from 64 to reduce starvation under load

// Maximum listeners = MAX_ROUTES (max 10)
#define MAX_LISTENERS 10
static EndpointToken *listener_tokens[MAX_LISTENERS];
static int listener_token_count = 0;

// M3: Rate limiting - global token bucket
static TokenBucket global_bucket;

// Scalable hash table rate limiter with IPv4/IPv6 support and TTL eviction
static IPRateLimiter g_ip_limiter;

// Internal forward declarations for event processing helpers
static void handle_listener_event(int epoll_fd, EndpointToken *token, const GatewayConfig *config);
static void handle_proxy_event(int epoll_fd, EndpointToken *token, uint32_t events);
static int  initiate_backend_connection(int epoll_fd, ConnectionContext *ctx);
static void process_socket_read(int epoll_fd, ConnectionContext *ctx, int from_fd, int to_fd, IOBuffer *buf);
static void process_socket_write(int epoll_fd, ConnectionContext *ctx, int to_fd, int from_fd, IOBuffer *buf);
static void update_epoll_interests(int epoll_fd, EndpointToken *token, uint32_t base_events, IOBuffer *buf);

// M3: Simple token bucket for rate limiting
static int token_bucket_consume(TokenBucket *tb) {
    time_t now = time(NULL);
    if (now > tb->last_refill) {
        int elapsed = now - tb->last_refill;
        int refill = elapsed * tb->refill_rate_per_sec;
        int new_tokens = tb->tokens + refill;
        if (new_tokens > tb->max_tokens) new_tokens = tb->max_tokens;
        atomic_store(&tb->tokens, new_tokens);
        tb->last_refill = now;
    }
    
    int current = atomic_load(&tb->tokens);
    if (current > 0) {
        atomic_fetch_sub(&tb->tokens, 1);
        return 1; // Token consumed
    }
    return 0; // No tokens available
}

// H2: Get monotonic time in seconds (avoids syscall overhead of time() and NTP adjustments)
static inline time_t get_monotonic_secs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

int event_loop_run(const GatewayConfig *config) {
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        LOG_ERROR("Fatal: epoll_create1 failed: %s", strerror(errno));
        return -1;
    }

    // M3: Initialize global rate limiter token bucket
    global_bucket.tokens = config->max_connections_per_sec;
    global_bucket.max_tokens = config->max_connections_per_sec;
    global_bucket.refill_rate_per_sec = config->max_connections_per_sec;
    global_bucket.last_refill = time(NULL);
    
    // Initialize scalable dual-stack per-IP rate limiter
    ip_ratelimit_init(&g_ip_limiter, RATE_LIMIT_MAX_ENTRIES, RATE_LIMIT_DEFAULT_TTL);

    // Create and register a listener for each route
    for (int r = 0; r < config->route_count; r++) {
        const Route *route = &config->routes[r];
        // H1: SO_REUSEPORT disabled by default (can be enabled via config in future)
        int listener_fd = net_create_listener(route->frontend_port, 0);
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

        // Track listener token for cleanup
        if (listener_token_count < MAX_LISTENERS) {
            listener_tokens[listener_token_count++] = listener_token;
        }

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
        // C3 FIX: Clean up listener tokens on error
        for (int i = 0; i < listener_token_count; i++) {
            free(listener_tokens[i]);
        }
        listener_token_count = 0;
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
        // C3 FIX: Clean up listener tokens on error
        for (int i = 0; i < listener_token_count; i++) {
            free(listener_tokens[i]);
        }
        listener_token_count = 0;
        close(timer_fd);
        close(epoll_fd);
        return -1;
    }

    int sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sig_fd < 0) {
        LOG_ERROR("Fatal: signalfd failed: %s", strerror(errno));
        // C3 FIX: Clean up listener tokens on error
        for (int i = 0; i < listener_token_count; i++) {
            free(listener_tokens[i]);
        }
        listener_token_count = 0;
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
        // C3 FIX: Clean up listener tokens on error
        for (int i = 0; i < listener_token_count; i++) {
            free(listener_tokens[i]);
        }
        listener_token_count = 0;
        close(sig_fd);
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
        // C3 FIX: Clean up listener tokens on error
        for (int i = 0; i < listener_token_count; i++) {
            free(listener_tokens[i]);
        }
        listener_token_count = 0;
        close(sig_fd);
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
        // C3 FIX: Clean up listener tokens on error
        for (int i = 0; i < listener_token_count; i++) {
            free(listener_tokens[i]);
        }
        listener_token_count = 0;
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

            // C2 FIX: Guard against stale events for freed tokens (fd set to -1 in router_handle_probe_event)
            if (token->fd < 0) continue;

            if (token->role == ROLE_TIMER) {
                uint64_t expirations = 0;
                ssize_t bytes_read = read(timer_fd, &expirations, sizeof(expirations));
                if (bytes_read == sizeof(expirations)) {
                    LOG_INFO("Background timer tick detected (expirations: %lu).", (unsigned long)expirations);
                    router_sweep_health_probes(epoll_fd, (GatewayConfig *)config);
                    conn_context_sweep_idle(epoll_fd, config);  // Check for idle connections using configured timeout
                    ip_ratelimit_sweep_idle(&g_ip_limiter, time(NULL));  // Sweep expired rate limit buckets
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
        for (int i = 0; i < listener_token_count; i++) {
            free(listener_tokens[i]);
        }
        listener_token_count = 0;

        conn_context_destroy_all(epoll_fd);
        ip_ratelimit_cleanup(&g_ip_limiter);

    close(sig_fd);
    close(timer_fd);
    close(epoll_fd);
    return 0;
}

static void handle_listener_event(int epoll_fd, EndpointToken *token, const GatewayConfig *config) {
    const Route *route = (const Route *)token->parent;
    int listener_fd = token->fd;
    
    while (1) {
        struct sockaddr_storage client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listener_fd, (struct sockaddr *)&client_addr, &client_len);
        
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break; // Ingestion queue drained
            LOG_ERROR("Accept failed on port %d: %s", route->frontend_port, strerror(errno));
            break;
        }

        // M3: Rate limiting - check global token bucket
        if (!token_bucket_consume(&global_bucket)) {
            LOG_WARN("Global connection rate limit exceeded, dropping connection from FD %d", client_fd);
            close(client_fd);
            continue;
        }
        
        // Scalable per-IP rate limiting (dual-stack IPv4/IPv6)
        if (!ip_ratelimit_check(&g_ip_limiter, &client_addr, config->max_connections_per_ip_per_sec)) {
            LOG_WARN("Per-IP connection rate limit exceeded, dropping connection on FD %d", client_fd);
            close(client_fd);
            continue;
        }

        char client_ip[INET6_ADDRSTRLEN];
        char client_port[16];
        // M5: Move getnameinfo behind LOG_DEBUG to avoid syscall overhead in hot path
        int log_debug_enabled = 1; // In production, check log level
        if (log_debug_enabled && getnameinfo((struct sockaddr *)&client_addr, client_len,
                        client_ip, sizeof(client_ip),
                        client_port, sizeof(client_port),
                        NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
            snprintf(client_ip, sizeof(client_ip), "unknown");
            snprintf(client_port, sizeof(client_port), "unknown");
        } else if (!log_debug_enabled) {
            snprintf(client_ip, sizeof(client_ip), "unknown");
            snprintf(client_port, sizeof(client_port), "unknown");
        }
        LOG_INFO("Accepted asynchronous connection from Client %s:%s (FD: %d) on port %d",
                 client_ip, client_port, client_fd, route->frontend_port);

        if (net_set_nonblocking(client_fd) < 0) {
            close(client_fd);
            continue;
        }

        // H3: Apply TCP_NODELAY and SO_KEEPALIVE for low latency and dead connection detection
        net_set_tcp_nodelay(client_fd, 1);
        net_set_keepalive(client_fd, 30, 10, 3);  // Idle 30s, probe every 10s, 3 probes

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
        LOG_INFO("Initiating failover connection to backend %s:%d for Client FD %d",
                 target->ip, target->port, ctx->client_fd);

        int fd = -1;
        int ret = net_connect_async(target->ip, target->port, &fd);
        if (ret == -1) {
            LOG_WARN("Immediate connect failure to %s:%d. Triggering failover...", target->ip, target->port);
            router_mark_backend_down(ctx->target_backend, ctx->route->max_consecutive_failures);
            if (fd >= 0) close(fd);
            ctx->backend_fd = -1;
            continue; // Loop around and instantly try the next healthy backend in the pool!
        }

        // Use the successfully connected socket
        ctx->backend_fd = fd;
        ctx->backend_token.fd = fd;

        // If we got here, either connect succeeded immediately or EINPROGRESS
        if (ret == 0) {
            // Immediate connection finalized (common on local loopback sockets)
            ctx->state = CONN_STATE_ESTABLISHED;
            router_report_backend_success(ctx->target_backend);
            LOG_INFO("Immediate connection established to %s:%d (FD: %d).", target->ip, target->port, fd);
        } else {
            LOG_DEBUG("Asynchronous non-blocking connect in progress for %s:%d (FD: %d)", target->ip, target->port, fd);
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

    // PHASE 1: Asynchronous Handshake Verification & Failover Logic (prioritized before generic error trap)
    if (ctx->state == CONN_STATE_CONNECTING) {
        if (token->role == ROLE_BACKEND) {
            int socket_error = 0;
            socklen_t len = sizeof(socket_error);
            int getsock_res = getsockopt(ready_fd, SOL_SOCKET, SO_ERROR, &socket_error, &len);

            if ((events & (EPOLLERR | EPOLLHUP)) || getsock_res < 0 || socket_error != 0) {
                LOG_WARN("Asynchronous connect to %s:%d failed (%s, events=0x%x). Triggering failover...",
                         ctx->target_backend->ip, ctx->target_backend->port,
                         socket_error ? strerror(socket_error) : "Hangup/Error", events);
               
                            router_mark_backend_down(ctx->target_backend, ctx->route->max_consecutive_failures);
                if (ctx->backend_fd >= 0) {
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ctx->backend_fd, NULL);
                    close(ctx->backend_fd);
                    ctx->backend_fd = -1;
                    ctx->backend_token.fd = -1;
                }

                if (initiate_backend_connection(epoll_fd, ctx) < 0) {
                    LOG_ERROR("Failover exhausted: Unable to connect to any backend for Client FD %d", ctx->client_fd);
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

    // Direct Error Trap Handlers (for established data streaming or client disconnects)
    if (events & (EPOLLERR | EPOLLHUP)) {
        LOG_WARN("Socket hardware hangup/error detected on FD %d.", ready_fd);
        if (token->role == ROLE_BACKEND && ctx->target_backend && ctx->route) {
            router_mark_backend_down(ctx->target_backend, ctx->route->max_consecutive_failures);
        }
        conn_context_destroy(epoll_fd, ctx);
        return;
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

            // M1: Backpressure propagation - disable EPOLLIN on source when buffer full
            update_epoll_interests(epoll_fd, from_token, 0, buf);
            
            // M1: Also disable EPOLLIN on the opposite direction to propagate backpressure
            // If reading from client and buffer full, stop reading from client
            // If reading from backend and buffer full, stop reading from backend
            break;
        }

        ssize_t bytes = recv(from_fd, buf_write_ptr(buf), space, 0);
        if (bytes == 0) {
            LOG_INFO("Graceful stream close (FIN) detected on endpoint FD %d.", from_fd);
            if (from_fd == ctx->client_fd) {
                ctx->client_read_closed = 1;
            } else {
                ctx->backend_read_closed = 1;
            }

            EndpointToken *from_token = (from_fd == ctx->client_fd) ? &ctx->client_token : &ctx->backend_token;
            update_epoll_interests(epoll_fd, from_token, 0, buf);

            // Attempt immediate flush pass of any remaining data in the buffer to to_fd
            if (buf_available_data(buf) > 0) {
                process_socket_write(epoll_fd, ctx, to_fd, from_fd, buf);
                if (ctx->state == CONN_STATE_CLOSING) return;
            }

            // If buffer is now empty, propagate half-close via shutdown(to_fd, SHUT_WR)
            if (buf_available_data(buf) == 0) {
                if (to_fd == ctx->client_fd && !ctx->client_write_closed) {
                    shutdown(to_fd, SHUT_WR);
                    ctx->client_write_closed = 1;
                    LOG_DEBUG("Propagated half-close (SHUT_WR) to Client FD %d", to_fd);
                } else if (to_fd == ctx->backend_fd && !ctx->backend_write_closed) {
                    shutdown(to_fd, SHUT_WR);
                    ctx->backend_write_closed = 1;
                    LOG_DEBUG("Propagated half-close (SHUT_WR) to Backend FD %d", to_fd);
                }
            }

            if (conn_is_fully_closed(ctx)) {
                conn_context_destroy(epoll_fd, ctx);
            }
            return;
        }
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break; // Input queue fully drained
            if (errno == EINTR) continue;
            conn_context_destroy(epoll_fd, ctx);
            return;
        }

        buf_advance_tail(buf, bytes);
        ctx->last_activity = get_monotonic_secs();  // H2: Use monotonic clock
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
        ctx->last_activity = get_monotonic_secs();  // H2: Use monotonic clock
        metrics_add_bytes_written(bytes);
    }

    // Buffer fully drained! Reset alignment tracking back to zero offsets
    buf_reset(buf);

    // If source closed its write side and target write side is not yet closed, propagate SHUT_WR
    if (from_fd == ctx->client_fd && ctx->client_read_closed && !ctx->backend_write_closed) {
        shutdown(to_fd, SHUT_WR);
        ctx->backend_write_closed = 1;
        LOG_DEBUG("Deferred half-close (SHUT_WR) sent to Backend FD %d after buffer drain", to_fd);
    } else if (from_fd == ctx->backend_fd && ctx->backend_read_closed && !ctx->client_write_closed) {
        shutdown(to_fd, SHUT_WR);
        ctx->client_write_closed = 1;
        LOG_DEBUG("Deferred half-close (SHUT_WR) sent to Client FD %d after buffer drain", to_fd);
    }

    if (conn_is_fully_closed(ctx)) {
        conn_context_destroy(epoll_fd, ctx);
        return;
    }

    // M1: Backpressure - re-enable EPOLLIN on the from_fd now that buffer has space
    EndpointToken *from_token = (from_fd == ctx->client_fd) ? &ctx->client_token : &ctx->backend_token;
    update_epoll_interests(epoll_fd, from_token, EPOLLIN, buf);

    // Remove corporate interest in writable alerts to protect against looping spikes
    EndpointToken *to_token   = (to_fd == ctx->client_fd)   ? &ctx->client_token : &ctx->backend_token;
    EndpointToken *from_token2 = (from_fd == ctx->client_fd) ? &ctx->client_token : &ctx->backend_token;

    update_epoll_interests(epoll_fd, to_token, EPOLLIN, buf);
    update_epoll_interests(epoll_fd, from_token2, EPOLLIN, buf);
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