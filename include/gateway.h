#ifndef GATEWAY_H
#define GATEWAY_H

#include "config.h"
#include <sys/types.h>

#define IO_BUFFER_SIZE 8192

typedef enum {
    CONN_STATE_CONNECTING,  // Backend connection initiated, waiting for handshake[cite: 1]
    CONN_STATE_ESTABLISHED, // Bidirectional pipeline fully operational[cite: 1]
    CONN_STATE_CLOSING      // Marked for cleanup
} ConnState;

// Simple ring-like buffer structure to manage partial network reads/writes
typedef struct {
    char data[IO_BUFFER_SIZE];
    size_t head; // Read offset pointer
    size_t tail; // Write offset pointer
} IOBuffer;

typedef struct ConnectionContext ConnectionContext; // Forward declaration

typedef enum {
    ROLE_CLIENT,
    ROLE_BACKEND,
    ROLE_TIMER,
    ROLE_HEALTH_PROBE,
    ROLE_SIGNAL
} EndpointRole;

typedef struct {
    int fd;
    EndpointRole role;
    union {
        ConnectionContext *parent;
        BackendServer *backend;
    };
} EndpointToken;

// The complete state machine representing a single active client-backend bridge[cite: 1]
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
};

// Allocates and initializes a brand new connection context
ConnectionContext *conn_context_create(int client_fd, const Route *route);
void conn_context_destroy(int epoll_fd, ConnectionContext *ctx);
void conn_context_destroy_all(int epoll_fd);
void conn_context_sweep_cleanup(void);

static inline size_t buf_available_data(const IOBuffer *buf) {
    return buf->tail - buf->head;
}

// Helper function to query how much empty space remains in a buffer
static inline size_t buf_available_space(const IOBuffer *buf) {
    return IO_BUFFER_SIZE - buf->tail;
}

#endif // GATEWAY_H