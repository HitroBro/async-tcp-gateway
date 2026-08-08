# Async TCP Layer 4 Gateway

A high-concurrency, asynchronous Layer 4 TCP gateway/proxy written in C using `epoll`. Features round-robin load balancing, automatic failover, asynchronous health checks with auto-recovery, configurable limits, IPv6 dual-stack support, and metrics/observability.

## Architecture Overview

```
Client → [Gateway:8080] → Backend Pool (127.0.0.1:9001, 127.0.0.1:9002, ...)
                    ↓
            epoll Event Loop
            ├── Listener (accept)
            ├── Client sockets (EPOLLIN/EPOLLOUT)
            ├── Backend sockets (async connect + EPOLLOUT handshake)
            ├── Health probes (timerfd → async connect)
            └── Signal handling (signalfd)
```

## Features

| Feature | Description |
|---------|-------------|
| **High Concurrency** | Single-threaded `epoll` handles thousands of concurrent connections |
| **Multi-Route Support** | Multiple `[route]` sections, each with own listener port and backend pool |
| **Load Balancing** | Atomic round-robin distribution across healthy backends |
| **Instant Failover** | Immediate retry on connection refusal or handshake failure |
| **Health Checks** | Background timer (5s) initiates async TCP probes to DOWN backends |
| **Auto-Recovery** | Successful probe automatically restores backend to rotation |
| **Flow Control** | Ring buffers with dynamic `EPOLLOUT` registration for backpressure |
| **Idle Timeout** | Connections idle >30s (configurable) are automatically closed |
| **IPv6 Dual-Stack** | `AF_UNSPEC` + `getaddrinfo` with `IPV6_V6ONLY=0` |
| **Graceful Shutdown** | `signalfd` handles SIGINT/SIGTERM for clean teardown |
| **Metrics/Observability** | SIGUSR1 dumps connection stats, byte counters, route/backend status |
| **CLI Config** | `-c /path/to.conf` custom config, `-h` help |
| **Zero-Copy Friendly** | `MSG_NOSIGNAL`, non-blocking I/O, deferred cleanup |
| **Rate Limiting** | Token bucket per-IP and global connection rate limiting |
| **TCP Optimizations** | `TCP_NODELAY` (disable Nagle), `SO_KEEPALIVE` with custom timing |
| **Monotonic Clock** | `CLOCK_MONOTONIC` for idle timeout (NTP-safe) |
| **Atomic Metrics** | Lock-free counters using C11 `_Atomic` for thread safety |

## Project Structure

```
.
├── include/
│   ├── config.h      # Config structures (Route, BackendServer, GatewayConfig)
│   ├── event.h       # Event loop interface
│   ├── gateway.h     # Connection state machine, I/O buffers, epoll tokens
│   ├── logger.h      # Logging macros (INFO/WARN/ERROR/DEBUG)
│   ├── net.h         # Network utilities (listener, connect, send_all, nonblock)
│   └── router.h      # Load balancing & health checking API
├── src/
│   ├── config.c      # INI-style config parser with [route] and [global] sections
│   ├── event.c       # Core epoll event loop (500+ lines)
│   ├── gateway.c     # Connection context lifecycle management
│   ├── logger.c      # Timestamped multi-level logging
│   ├── main.c        # Entry point with CLI argument parsing
│   ├── net.c         # Socket operations (dual-stack IPv4/IPv6)
│   └── router.c      # Round-robin + async health probes
├── tests/
│   ├── mock_backend.py   # Test backend (echo/delay/crash modes)
│   └── test_client.py    # Automated test client
├── config/gateway.conf   # Production config (optional)
├── Makefile
└── README.md
```

## Quick Start

### Prerequisites
- Linux with `epoll`, `timerfd`, `signalfd` (kernel 2.6.27+)
- GCC with C11 support (`-std=c11`)
- Python 3 (for test tools)

### Build

```bash
make
# Output: bin/gateway
```

### Run Tests

**Terminal 1 - Start mock backends:**
```bash
python3 tests/mock_backend.py -p 9001 &
python3 tests/mock_backend.py -p 9002 &
python3 tests/mock_backend.py -p 9003 &
python3 tests/mock_backend.py -p 9004 &
python3 tests/mock_backend.py -p 9005 &
```

**Terminal 2 - Start gateway:**
```bash
./bin/gateway
# Loads config/gateway.conf by default (port 8080 → 9001,9002; port 8888 → 9003,9004,9005)
```

**Terminal 3 - Run test client:**
```bash
python3 tests/test_client.py -p 8080 -m "Hello Gateway!"
# Expected: [TEST PASSED] Response matches expected echo format!
```

### Test Failure Scenarios

```bash
# Test failover: kill one backend, gateway should route to the other
python3 tests/mock_backend.py -p 9001 -m crash &

# Test delay handling
python3 tests/mock_backend.py -p 9002 -m delay -d 3 &

# Test health check recovery: start backend DOWN, bring it up
python3 tests/mock_backend.py -p 9003 &  # Start after gateway
# Gateway will detect it within 5s via health probe
```

## Configuration

Create a config file (e.g., `config/gateway.conf`):

```ini
### Global settings (optional - all have sensible defaults)
[global]
max_routes = 10              # Maximum number of routes (compile-time max: 10)
max_backends = 10            # Maximum backends per route (compile-time max: 10)
max_active_connections = 1024 # Maximum concurrent connections
io_buffer_size = 8192        # I/O ring buffer size in bytes
max_consecutive_failures = 1 # Failures before marking backend DOWN
connection_idle_timeout_secs = 30 # Idle timeout in seconds
max_connections_per_sec = 1000 # Global connection rate limit (tokens/sec)
max_connections_per_ip_per_sec = 50 # Per-IP connection rate limit (tokens/sec)

# Route 1: Listen on port 8080, balance across 2 backends
[route]
frontend_port = 8080
backend = 127.0.0.1:9001
backend = 127.0.0.1:9002

# Route 2: Listen on port 8888, balance across 3 backends
[route]
frontend_port = 8888
backend = 127.0.0.1:9003
backend = 127.0.0.1:9004
backend = 127.0.0.1:9005
```

Run with custom config:
```bash
./bin/gateway -c /path/to/custom.conf
```

### CLI Options
```bash
./bin/gateway -h
# Usage: ./bin/gateway [-c config_file]
#   -c config_file   Path to configuration file (default: config/gateway.conf)
#   -h               Show this help message
```

## Design Highlights

### Connection State Machine
```
CONN_STATE_CONNECTING  →  CONN_STATE_ESTABLISHED  →  CONN_STATE_CLOSING
       ↑                      ↓                         ↓
   async connect         bidirectional           cleanup +
   (EPOLLOUT)              proxy                  deferred free
```

### Epoll Token Architecture
- Uses `epoll_data.ptr` with `EndpointToken` structs
- Each token carries `role` (CLIENT, BACKEND, TIMER, HEALTH_PROBE, SIGNAL, LISTENER)
- Union payload: `ConnectionContext*` or `BackendServer*`
- Enables O(1) dispatch without FD-to-context lookups

### Async Backend Connection
1. `connect()` returns `EINPROGRESS` → register `EPOLLOUT`
2. `EPOLLOUT` fires → `getsockopt(SO_ERROR)` verifies handshake
3. Success: transition to `ESTABLISHED`, enable `EPOLLIN` both directions
4. Failure: `router_mark_backend_down()`, immediate retry next backend

### Health Check Recovery
- `timerfd` fires every 5s → `router_sweep_health_probes()`
- For each DOWN backend without active probe: async `connect()`
- Probe completion → `router_handle_probe_event()` verifies `SO_ERROR`
- Success: `router_report_backend_success()` → back in rotation

### Metrics/Observability
Send `SIGUSR1` to dump metrics to logs:
```bash
kill -USR1 <gateway_pid>
```
Output includes:
- Total connections, active connections, failed connections
- Total bytes read/written
- Backend failures, health probes initiated/succeeded
- Per-route backend status (ALIVE/DOWN, active connections, failure count)

## Performance Notes

- **Ring buffer size**: 8KB (configurable via `io_buffer_size`)
- **Max events per `epoll_wait`**: 256 (`MAX_EVENTS`, increased from 64)
- **Max concurrent connections**: 1024 (configurable via `max_active_connections`, dynamic allocation up to 65536)
- **Max routes/backends**: 10 each (configurable via `max_routes`, `max_backends`)
- **Failure threshold**: 1 consecutive failure → immediate `DOWN` (configurable)
- **Idle timeout resolution**: 1 second (using `CLOCK_MONOTONIC`)
- **Rate limiting**: Token bucket algorithm (global + per-IP)

## Limitations

- **Layer 4 only**: This is a TCP proxy (Layer 4). It does not terminate TLS/SSL. For HTTPS, deploy a TLS terminator (e.g., nginx, HAProxy) in front, or use a Layer 7 proxy.
- **Single-threaded**: The event loop runs on a single CPU core. For multi-core scaling, run multiple instances with `SO_REUSEPORT` (configurable).
- **No HTTP/2 or WebSocket awareness**: Pure TCP stream proxy.

## License

MIT License - Feel free to use, modify, and distribute.

## Contributing

1. Fork the repository
2. Create a feature branch
3. Ensure `make` builds cleanly (`-Wall -Wextra -pedantic`)
4. Test with the provided Python test suite
5. Submit a pull request