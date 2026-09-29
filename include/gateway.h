#ifndef GATEWAY_H
#define GATEWAY_H

#include "config.h"
#include "bufpool.h"
#include <sys/types.h>
#include <stdlib.h>

// Forward declare constants from config.h to avoid duplication
// #define IO_BUFFER_SIZE 8192
// #define CONNECTION_IDLE_TIMEOUT_SECS 30

typedef enum {
    CONN_STATE_CONNECTING,  // Backend connection initiated, waiting for handshake
    CONN_STATE_ESTABLISHED, // Bidirectional pipeline fully operational
    CONN_STATE_CLOSING      // Marked for cleanup
} ConnState;

// Simple ring buffer structure to manage partial network reads/writes
typedef struct {
    char *data;
    size_t capacity;
    size_t head; // Read offset pointer
    size_t tail; // Write offset pointer
} IOBuffer;

static inline size_t buf_available_data(const IOBuffer *buf) {
    if (!buf || !buf->data || buf->capacity == 0) return 0;
    return (buf->tail >= buf->head) ? (buf->tail - buf->head) : (buf->capacity - buf->head + buf->tail);
}

static inline size_t buf_available_space(const IOBuffer *buf) {
    if (!buf || !buf->data || buf->capacity <= 1) return 0;
    return (buf->capacity - 1) - buf_available_data(buf);
}

static inline void buf_advance_head(IOBuffer *buf, size_t len) {
    if (!buf || buf->capacity == 0) return;
    buf->head = (buf->head + len) % buf->capacity;
}

static inline void buf_advance_tail(IOBuffer *buf, size_t len) {
    if (!buf || buf->capacity == 0) return;
    buf->tail = (buf->tail + len) % buf->capacity;
}

static inline const char *buf_read_ptr(const IOBuffer *buf) {
    return buf->data + buf->head;
}

static inline char *buf_write_ptr(IOBuffer *buf) {
    return buf->data + buf->tail;
}

static inline size_t buf_contiguous_read(const IOBuffer *buf) {
    if (!buf || !buf->data || buf->capacity == 0) return 0;
    if (buf->tail >= buf->head) {
        return buf->tail - buf->head;
    }
    return buf->capacity - buf->head;
}

static inline size_t buf_contiguous_write(const IOBuffer *buf) {
    if (!buf || !buf->data || buf->capacity == 0) return 0;
    if (buf->tail >= buf->head) {
        size_t avail = buf->capacity - buf->tail;
        if (buf->head == 0) {
            return (avail > 0) ? (avail - 1) : 0;
        }
        return avail;
    }
    return (buf->head > buf->tail) ? (buf->head - buf->tail - 1) : 0;
}

static inline int buf_init(IOBuffer *buf, size_t capacity) {
    if (!buf) return -1;
    buf->data = (char *)malloc(capacity);
    if (!buf->data) {
        buf->capacity = 0;
        buf->head = 0;
        buf->tail = 0;
        return -1;
    }
    buf->capacity = capacity;
    buf->head = 0;
    buf->tail = 0;
    return 0;
}

static inline void buf_free(IOBuffer *buf) {
    if (buf) {
        if (buf->data) {
            free(buf->data);
            buf->data = NULL;
        }
        buf->capacity = 0;
        buf->head = 0;
        buf->tail = 0;
    }
}

static inline void buf_reset(IOBuffer *buf) {
    buf->head = 0;
    buf->tail = 0;
}

typedef struct ConnectionContext ConnectionContext; // Forward declaration

// Global metrics counters
typedef struct {
    _Atomic uint64_t total_connections;
    _Atomic uint64_t active_connections;
    _Atomic uint64_t failed_connections;
    _Atomic uint64_t total_bytes_read;
    _Atomic uint64_t total_bytes_written;
    _Atomic uint64_t backend_failures;
    _Atomic uint64_t health_probes;
    _Atomic uint64_t health_probe_successes;
} GatewayMetrics;

extern GatewayMetrics g_metrics;

typedef enum {
    ROLE_CLIENT,
    ROLE_BACKEND,
    ROLE_TIMER,
    ROLE_HEALTH_PROBE,
    ROLE_SIGNAL,
    ROLE_LISTENER
} EndpointRole;

typedef struct {
    int fd;
    EndpointRole role;
    union {
        ConnectionContext *parent;
        BackendServer *backend;
    };
} EndpointToken;

// The complete state machine representing a single active client-backend bridge
struct ConnectionContext {
    int client_fd;
    int backend_fd;
    ConnState state;

    IOBuffer client_to_backend;
    IOBuffer backend_to_client;

    const Route *route;
    BackendServer *target_backend;

    EndpointToken client_token;
    EndpointToken backend_token;

    time_t last_activity;  // Timestamp of last read/write activity for idle timeout (monotonic clock)

    // TCP Half-Close (FIN) tracking flags
    int client_read_closed;   // Client sent FIN (EOF on recv)
    int backend_read_closed;  // Backend sent FIN (EOF on recv)
    int client_write_closed;  // shutdown(client_fd, SHUT_WR) sent
    int backend_write_closed; // shutdown(backend_fd, SHUT_WR) sent

    int active_index;         // O(1) position index inside active_connections array
};

static inline int conn_is_half_closed(const ConnectionContext *ctx) {
    if (!ctx) return 0;
    return (ctx->client_read_closed || ctx->backend_read_closed ||
            ctx->client_write_closed || ctx->backend_write_closed);
}

static inline int conn_is_fully_closed(const ConnectionContext *ctx) {
    if (!ctx) return 1;
    // Both endpoints have either received FIN or finished transmitting SHUT_WR
    return (ctx->client_read_closed && ctx->backend_read_closed) ||
           (ctx->client_write_closed && ctx->backend_write_closed) ||
           (ctx->client_read_closed && ctx->backend_write_closed &&
            ctx->backend_read_closed && ctx->client_write_closed);
}

// Allocates and initializes a brand new connection context
ConnectionContext *conn_context_create(int client_fd, const Route *route, const GatewayConfig *config);
void conn_context_destroy(int epoll_fd, ConnectionContext *ctx);
void conn_context_destroy_all(int epoll_fd);
void conn_context_sweep_cleanup(void);
void conn_context_sweep_idle(int epoll_fd, const GatewayConfig *config);  // Close connections idle beyond timeout

// Metrics functions
void metrics_dump(const GatewayConfig *config);
void metrics_increment_connections(void);
void metrics_decrement_connections(void);
void metrics_increment_failed(void);
void metrics_add_bytes_read(uint64_t bytes);
void metrics_add_bytes_written(uint64_t bytes);
void metrics_increment_backend_failures(void);
void metrics_increment_health_probes(void);
void metrics_increment_health_probe_success(void);

#endif // GATEWAY_H