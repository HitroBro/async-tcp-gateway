# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- GitHub Actions CI workflow for automated build and test
- Static analysis with cppcheck and clang-tidy
- Comprehensive README with CI badge

## [0.1.0] - 2025-08-10

### Added
- High-concurrency async TCP Layer 4 gateway using epoll
- Multi-route support with independent backend pools
- Round-robin load balancing with atomic operations
- Async backend connection with EPOLLOUT handshake verification
- Instant failover on connection refusal or handshake failure
- Background health checks using timerfd (5s interval)
- Auto-recovery of DOWN backends via successful probe
- Ring buffer I/O with dynamic EPOLLOUT registration
- Configurable idle timeout (default 30s) using CLOCK_MONOTONIC
- IPv6 dual-stack support (AF_UNSPEC, IPV6_V6ONLY=0)
- Graceful shutdown via signalfd (SIGINT/SIGTERM)
- Metrics observability via SIGUSR1
- Token bucket rate limiting (global + per-IP)
- TCP optimizations (TCP_NODELAY, SO_KEEPALIVE)
- C11 _Atomic lock-free counters
- INI-style configuration parser with [route] and [global] sections
- Comprehensive Python test suite (mock backends + test client)
- Makefile with clean build (-Wall -Wextra -pedantic -std=c11)

### Security
- Path traversal protection in configuration parser
- Connection rate limiting (token bucket per-IP and global)
- Message size limits and buffer overflow protection

### Performance
- O(1) epoll dispatch via epoll_data.ptr token architecture
- Ring buffers with dynamic EPOLLOUT for backpressure
- Lock-free atomic metrics
- Configurable connection limits (default 1024, max 65536)
- Configurable epoll event batch size (256)