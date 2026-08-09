# Contributing to async-tcp-gateway

Thank you for your interest in contributing! This document outlines the process for contributing to this high-performance C networking project.

## Code of Conduct

By participating, you agree to maintain a respectful and inclusive environment. Harassment, discrimination, or disruptive behavior will not be tolerated.

## How to Contribute

### 1. Fork & Clone

```bash
git clone https://github.com/your-username/async-tcp-gateway.git
cd async-tcp-gateway
git remote add upstream https://github.com/HitroBro/async-tcp-gateway.git
```

### 2. Create a Branch

```bash
git checkout -b feature/your-feature-name
# or
git checkout -b fix/your-bug-fix
```

### 3. Make Changes

- Follow the existing code style (see **Code Style** below)
- Add tests for new functionality
- Update documentation as needed
- Ensure `make` builds cleanly with `-Wall -Wextra -pedantic -std=c11`

### 4. Test Thoroughly

```bash
# Build
make clean && make

# Run tests (requires 3 terminals)
# Terminal 1: Start backends
python3 tests/mock_backend.py -p 9001 &
python3 tests/mock_backend.py -p 9002 &
python3 tests/mock_backend.py -p 9003 &
python3 tests/mock_backend.py -p 9004 &
python3 tests/mock_backend.py -p 9005 &

# Terminal 2: Start gateway
./bin/gateway

# Terminal 3: Run test client
python3 tests/test_client.py -p 8080 -m "Test message"
```

### 5. Submit Pull Request

- Push to your fork
- Open PR against `main` branch
- Fill out the PR template
- Ensure CI passes

## Code Style

### C Code

- **Standard**: C11 (`-std=c11`)
- **Warnings**: `-Wall -Wextra -pedantic` (must compile clean)
- **Indentation**: 4 spaces, no tabs
- **Braces**: K&R style (opening brace on same line)
- **Line length**: 100 characters max
- **Naming**:
  - Functions: `snake_case` (e.g., `router_mark_backend_down`)
  - Structs: `PascalCase` (e.g., `ConnectionContext`)
  - Macros: `UPPER_SNAKE_CASE` (e.g., `MAX_EVENTS`)
  - Enums: `UPPER_SNAKE_CASE` (e.g., `TOKEN_CLIENT`)

### Example

```c
// Good
static int router_select_backend(Router *router, int route_idx) {
    BackendServer *backend = NULL;
    int attempts = 0;

    while (attempts < router->routes[route_idx].backend_count) {
        backend = &router->routes[route_idx].backends[
            atomic_fetch_add(&router->routes[route_idx].rr_counter, 1)
            % router->routes[route_idx].backend_count
        ];

        if (atomic_load(&backend->status) == BACKEND_ALIVE) {
            return backend->fd;
        }
        attempts++;
    }
    return -1;
}
```

### Commit Messages

Follow [Conventional Commits](https://www.conventionalcommits.org/):

```
type(scope): brief description

Longer explanation if needed.

Fixes #123
```

Types: `feat`, `fix`, `docs`, `style`, `refactor`, `perf`, `test`, `chore`, `ci`

### Example

```
feat(router): add weighted round-robin support

Implements weighted round-robin load balancing by adding
a weight field to BackendServer struct and modifying the
selection algorithm to respect weights.

Fixes #45
```

## Reporting Issues

Use the issue templates:

- **Bug Report** — For bugs, crashes, unexpected behavior
- **Feature Request** — For new features, enhancements
- **Security Issue** — For security vulnerabilities (see Security Policy)

## Security Policy

See [SECURITY.md](SECURITY.md) for reporting vulnerabilities.

## Development Setup

```bash
# Install dependencies
sudo apt-get install gcc make python3 cppcheck clang-tidy

# Build
make clean && make

# Run static analysis
cppcheck --enable=all --std=c11 --suppress=missingIncludeSystem src/ include/
clang-tidy src/*.c -- -Iinclude -std=c11
```

## Questions?

Open a Discussion or email the maintainer.

Thank you for contributing!