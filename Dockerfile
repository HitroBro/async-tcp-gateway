# Multi-stage build for async-tcp-gateway
# Stage 1: Build
FROM gcc:13-bookworm AS builder

WORKDIR /app

# Install build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    make \
    && rm -rf /var/lib/apt/lists/*

# Copy source files
COPY include/ ./include/
COPY src/ ./src/
COPY Makefile ./
COPY config/ ./config/

# Build with strict flags
RUN make clean && make

# Stage 2: Runtime (distroless)
FROM gcr.io/distroless/cc-debian12:nonroot

WORKDIR /app

# Copy binary and config
COPY --from=builder /app/bin/gateway ./gateway
COPY --from=builder /app/config/gateway.conf ./config/gateway.conf

# Expose default ports
EXPOSE 8080 8888

# Run as non-root user (distroless nonroot)
USER nonroot:nonroot

ENTRYPOINT ["./gateway", "-c", "config/gateway.conf"]