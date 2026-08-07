#ifndef GATEWAY_H
#define GATEWAY_H

#include "config.h"
#include <sys/types.h>

#define IO_BUFFER_SIZE 8192
#define CONNECTION_IDLE_TIMEOUT_SECS 30  // Close connections idle for more than 30 seconds

typedef enum {
    CONN_STATE_CONNECTING,  // Backend connection initiated, waiting for handshake
    CONN_STATE_ESTABLISHED, // Bidirectional pipeline fully operational
    CONN_STATE_CLOSING      // Marked for cleanup
} ConnState;

// Simple ring buffer structure to manage partial network reads/writes
typedef struct {
    char data[IO_BUFFER_SIZE];
    size_t head; // Read offset pointer
    size_t tail; // Write offset pointer
} IOBuffer;

static inline size_t buf_available_data(const IOBuffer *buf) {
    return (buf->tail >= buf->head) ? (buf->tail - buf->head) : (IO_BUFFER_SIZE - buf->head + buf->tail);
}

static inline size_t buf_available_space(const IOBuffer *buf) {
    if (buf->tail >= buf->head) {
        return IO_BUFFER_SIZE - buf->tail + buf->head - (buf->head == 0 ? 1 : 0);
    } else {
        return buf->head - buf->tail - 1;
    }
}

static inline void buf_advance_head(IOBuffer *buf, size_t len) {
    buf->head = (buf->head + len) % IO_BUFFER_SIZE;
}

static inline void buf_advance_tail(IOBuffer *buf, size_t len) {
    buf->tail = (buf->tail + len) % IO_BUFFER_SIZE;
}

static inline const char *buf_read_ptr(const IOBuffer *buf) {
    return buf->data + buf->head;
}

static inline char *buf_write_ptr(IOBuffer *buf) {
    return buf->data + buf->tail;
}

static inline size_t buf_contiguous_read(const IOBuffer *buf) {
    if (buf->tail >= buf->head) {
        return buf->tail - buf->head;
    }
    return IO_BUFFER_SIZE - buf->head;
}

static inline size_t buf_contiguous_write(IOBuffer *buf) {
    if (buf->tail >= buf->head) {
        return IO_BUFFER_SIZE - buf->tail;
    }
    return buf->head - buf->tail - 1;
}

static inline void buf_reset(IOBuffer *buf) {
    buf->head = 0;
    buf->tail = 0;
}

typedef struct ConnectionContext ConnectionContext; // Forward declaration

// Global metrics counters
typedef struct {
    uint64_t total_connections;
    uint64_t active_connections;
    uint64_t failed_connections;
    uint64_t total_bytes_read;
    uint64_t total_bytes_written;
    uint64_t backend_failures;
    uint64_t health_probes;
    uint64_t health_probe_successes;
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

    time_t last_activity;  // Timestamp of last read/write activity for idle timeout
};

// Allocates and initializes a brand new connection context
ConnectionContext *conn_context_create(int client_fd, const Route *route, const GatewayConfig *config);
void conn_context_destroy(int epoll_fd, ConnectionContext *ctx);
void conn_context_destroy_all(int epoll_fd);
void conn_context_sweep_cleanup(void);
void conn_context_sweep_idle(int epoll_fd);  // Close connections idle beyond timeout

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