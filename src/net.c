#define _POSIX_C_SOURCE 200809L

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
#include <netdb.h>
#include <netinet/ip6.h>  // For IPV6_V6ONLY

// Creates a dual-stack (IPv4/IPv6) listener socket
int net_create_listener(int port) {
    int server_fd;
    struct addrinfo hints, *res, *rp;
    char port_str[16];

    snprintf(port_str, sizeof(port_str), "%d", port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;     // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM; // TCP
    hints.ai_flags = AI_PASSIVE;     // For bind on any address
    hints.ai_protocol = 0;

    int ret = getaddrinfo(NULL, port_str, &hints, &res);
    if (ret != 0) {
        LOG_ERROR("getaddrinfo failed for port %d: %s", port, gai_strerror(ret));
        return -1;
    }

    int opt = 1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        server_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (server_fd < 0) {
            continue;
        }

        // Allow socket reuse
        if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            LOG_ERROR("Failed to set SO_REUSEADDR option: %s", strerror(errno));
            close(server_fd);
            freeaddrinfo(res);
            return -1;
        }

        // Enable dual-stack: allow IPv6 socket to accept IPv4 connections too
        if (rp->ai_family == AF_INET6) {
            int ipv6only = 0;
            if (setsockopt(server_fd, IPPROTO_IPV6, IPV6_V6ONLY, &ipv6only, sizeof(ipv6only)) < 0) {
                LOG_WARN("Failed to set IPV6_V6ONLY=0 (dual-stack may not work): %s", strerror(errno));
            }
        }

        if (bind(server_fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break; // Success
        }

        close(server_fd);
    }

    freeaddrinfo(res);

    if (rp == NULL) {
        LOG_ERROR("Failed to bind socket to port %d: %s", port, strerror(errno));
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

// Connects to backend using getaddrinfo (supports IPv4 and IPv6)
int net_connect_to_backend(const char *ip, int port) {
    int backend_fd;
    struct addrinfo hints, *res, *rp;
    char port_str[16];

    snprintf(port_str, sizeof(port_str), "%d", port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;     // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM; // TCP
    hints.ai_flags = 0;
    hints.ai_protocol = 0;

    int ret = getaddrinfo(ip, port_str, &hints, &res);
    if (ret != 0) {
        LOG_ERROR("getaddrinfo failed for %s:%d: %s", ip, port, gai_strerror(ret));
        return -1;
    }

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        backend_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (backend_fd < 0) {
            continue;
        }

        if (connect(backend_fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break; // Success
        }

        close(backend_fd);
    }

    freeaddrinfo(res);

    if (rp == NULL) {
        LOG_ERROR("Failed to connect to backend %s:%d: %s", ip, port, strerror(errno));
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