#include "net.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

int net_create_listener(int port) {
    int server_fd;
    struct sockaddr_in server_addr;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        LOG_ERROR("Failed to create server socket: %s", strerror(errno));
        return -1;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        LOG_ERROR("Failed to set SO_REUSEADDR option: %s", strerror(errno));
        close(server_fd);
        return -1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        LOG_ERROR("Failed to bind socket to port %d: %s", port, strerror(errno));
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, SOMAXCONN) < 0) {
        LOG_ERROR("Failed to listen on socket: %s", strerror(errno));
        close(server_fd);
        return -1;
    }

    LOG_INFO("Gateway listener created successfully on port %d (FD: %d)", port, server_fd);
    return server_fd;
}

int net_connect_to_backend(const char *ip, int port) {
    int backend_fd;
    struct sockaddr_in server_addr;

    // 1. Create an active client socket
    backend_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (backend_fd < 0) {
        LOG_ERROR("Failed to create backend socket: %s", strerror(errno));
        return -1;
    }

    // 2. Configure target backend address structure
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    // 3. Convert IPv4 string (e.g., "127.0.0.1") to binary network format
    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        LOG_ERROR("Invalid backend IP address format: %s", ip);
        close(backend_fd);
        return -1;
    }

    // 4. Initiate TCP Three-Way Handshake (Blocking call)
    if (connect(backend_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        LOG_ERROR("Failed to connect to backend %s:%d: %s", ip, port, strerror(errno));
        close(backend_fd);
        return -1;
    }

    LOG_DEBUG("Successfully connected to backend target %s:%d (FD: %d)", ip, port, backend_fd);
    return backend_fd;
}

int net_send_all(int sockfd, const void *buffer, size_t len) {
    size_t total_sent = 0;
    const char *buf_ptr = (const char *)buffer;

    // Loop until every single byte in the buffer has been pushed to the OS kernel
    while (total_sent < len) {
        // MSG_NOSIGNAL prevents fatal SIGPIPE crashes if the remote peer disconnected abruptly!
        ssize_t bytes_sent = send(sockfd, buf_ptr + total_sent, len - total_sent, MSG_NOSIGNAL);
        
        if (bytes_sent < 0) {
            // If interrupted by a system signal, retry immediately
            if (errno == EINTR) {
                continue;
            }
            LOG_ERROR("Socket transmission error on FD %d: %s", sockfd, strerror(errno));
            return -1;
        }

        if (bytes_sent == 0) {
            // 0 bytes sent typically implies the underlying connection is closed
            LOG_WARN("Socket FD %d closed unexpectedly during transmission.", sockfd);
            return -1;
        }

        total_sent += bytes_sent;
    }

    return 0;
}

int net_set_nonblocking(int sockfd) {
    // 1. Get current file status flags
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1) {
        LOG_ERROR("fcntl(F_GETFL) failed on FD %d: %s", sockfd, strerror(errno));
        return -1;
    }

    // 2. Set the O_NONBLOCK flag while preserving existing flags
    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
        LOG_ERROR("fcntl(F_SETFL, O_NONBLOCK) failed on FD %d: %s", sockfd, strerror(errno));
        return -1;
    }

    return 0;
}