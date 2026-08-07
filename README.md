# Async TCP Layer 4 Gateway

A high-concurrency, asynchronous Layer 4 TCP gateway/proxy written in C using `epoll`. Features round-robin load balancing, automatic failover, and asynchronous health checks with auto-recovery.

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
| **Load Balancing** | Atomic round-robin distribution across healthy backends |
| **Instant Failover** | Immediate retry on connection refusal or handshake failure |
| **Health Checks** | Background timer (5s) initiates async TCP probes to DOWN backends |
| **Auto-Recovery** | Successful probe automatically restores backend to rotation |
| **Flow Control** | Ring buffers with dynamic `EPOLLOUT` registration for backpressure |
| **Graceful Shutdown** | `signalfd` handles SIGINT/SIGTERM for clean teardown |
| **Zero-Copy Friendly** | `MSG_NOSIGNAL`, non-blocking I/O, deferred cleanup |

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
│   ├── config.c      # INI-style config parser with [route] sections
│   ├── event.c       # Core epoll event loop (438 lines)
│   ├── gateway.c     # Connection context lifecycle management
│   ├── logger.c      # Timestamped multi-level logging
│   ├── main.c        # Entry point
│   ├── net.c         # Socket operations
│   └── router.c      # Round-robin + async health probes
├── tests/
│   ├── mock_backend.py   # Test backend (echo/delay/crash modes)
│   └── test_client.py    # Automated test client
├── config/gateway.conf   # Production config (optional)
├── test.conf             # Sample config for testing
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
```

**Terminal 2 - Start gateway:**
```bash
./bin/gateway
# Loads config/test.conf by default (port 8080 → 9001, 9002)
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
[route]
frontend_port = 8080
backend = 10.0.0.1:9001
backend = 10.0.0.1:9002
backend = 10.0.0.2:9001

[route]
frontend_port = 8443
backend = 10.0.0.3:8080
backend = 10.0.0.4:8080
```

Run with custom config:
```bash
./bin/gateway  # Modify main.c CONFIG_PATH or extend to accept CLI arg
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
- Each token carries `role` (CLIENT, BACKEND, TIMER, HEALTH_PROBE, SIGNAL)
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

## Performance Notes

- **Ring buffer size**: 8KB (configurable via `IO_BUFFER_SIZE`)
- **Max events per `epoll_wait`**: 64 (`MAX_EVENTS`)
- **Max concurrent connections**: 1024 (`MAX_ACTIVE_CONNECTIONS`)
- **Max routes/backends**: 10 each (`MAX_ROUTES`, `MAX_BACKENDS`)
- **Failure threshold**: 1 consecutive failure → immediate `DOWN` (`MAX_CONSECUTIVE_FAILURES`)

## License

MIT License - Feel free to use, modify, and distribute.

## Contributing

1. Fork the repository
2. Create a feature branch
3. Ensure `make` builds cleanly (`-Wall -Wextra -pedantic`)
4. Test with the provided Python test suite
5. Submit a pull request