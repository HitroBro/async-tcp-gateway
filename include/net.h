#ifndef NET_H
#define NET_H

#include <sys/types.h>

// Creates, binds, and sets a TCP socket to listening mode.
// Supports both IPv4 and IPv6 (dual-stack).
// Returns the server socket file descriptor, or -1 on failure.
int net_create_listener(int port);

// Initiates a TCP Three-Way Handshake with an active backend server.
// Supports both IPv4 and IPv6 addresses.
// Returns the connected socket file descriptor, or -1 on failure.
int net_connect_to_backend(const char *ip, int port);

// Guaranteed transmission loop. Ensures all 'len' bytes are sent,
// protecting against partial writes and SIGPIPE crashes.
// Returns 0 on complete success, or -1 if the connection dropped/failed.
int net_send_all(int sockfd, const void *buffer, size_t len);

// Sets a file descriptor (socket) to non-blocking mode using fcntl.
// Returns 0 on success, or -1 on failure.
int net_set_nonblocking(int sockfd);

#endif // NET_H