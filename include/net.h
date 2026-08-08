#ifndef NET_H
#define NET_H

#include <sys/types.h>

// Creates a dual-stack (IPv4/IPv6) listener socket with optional SO_REUSEPORT
// Supports both IPv4 and IPv6 (dual-stack).
// Returns the server socket file descriptor, or -1 on failure.
int net_create_listener(int port, int reuse_port);

// Initiates a TCP Three-Way Handshake with an active backend server (blocking).
// Supports both IPv4 and IPv6 addresses.
// Returns the connected socket file descriptor, or -1 on failure.
// NOTE: Currently unused in async gateway (uses net_connect_async instead).
// Kept for potential blocking I/O use cases or external consumers.
int net_connect_to_backend(const char *ip, int port);

// M2: Initiate async (non-blocking) TCP connection to backend.
// Returns socket FD on success (connection in progress or immediate), -1 on hard failure,
// or -2 if connection is in progress (EINPROGRESS). On success/EINPROGRESS, the socket
// is non-blocking and has TCP_NODELAY/SO_KEEPALIVE configured.
// Caller must monitor EPOLLOUT and verify with getsockopt(SO_ERROR).
int net_connect_async(const char *ip, int port, int *out_fd);

// Guaranteed transmission loop. Ensures all 'len' bytes are sent,
// protecting against partial writes and SIGPIPE crashes.
// Returns 0 on complete success, or -1 if the connection dropped/failed.
// NOTE: Currently unused in async gateway (uses ring buffer + send() instead).
// Kept for blocking I/O use cases or external consumers.
int net_send_all(int sockfd, const void *buffer, size_t len);

// Sets a file descriptor (socket) to non-blocking mode using fcntl.
// Returns 0 on success, or -1 on failure.
int net_set_nonblocking(int sockfd);

// Configure TCP_NODELAY (disable Nagle's algorithm) for low latency.
// Returns 0 on success, -1 on failure.
int net_set_tcp_nodelay(int sockfd, int enable);

// Configure SO_KEEPALIVE with custom timing for dead connection detection.
// idle_secs: time before first probe (0 = system default)
// interval_secs: interval between probes (0 = system default)
// max_probes: number of unacknowledged probes before dropping (0 = system default)
// Returns 0 on success, -1 on failure.
int net_set_keepalive(int sockfd, int idle_secs, int interval_secs, int max_probes);

#endif // NET_H