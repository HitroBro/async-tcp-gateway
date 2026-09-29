# Compiler and Flags
CC = gcc
CFLAGS = -Wall -Wextra -pedantic -std=c11 -g -I./include

# Directories
SRC_DIR = src
BIN_DIR = bin

# Source and Object Files (includes slab pool bufpool.c)
SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(SRCS:.c=.o)

# Target Executable
TARGET = $(BIN_DIR)/gateway

# Default rule
all: $(TARGET)

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

# Compile C files into Object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean build artifacts
# Unit Tests
test_buffer: tests/test_buffer.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_buffer tests/test_buffer.c
	./$(BIN_DIR)/test_buffer

test_config: src/config.o src/logger.o tests/test_config.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_config tests/test_config.c src/config.o src/logger.o
	./$(BIN_DIR)/test_config

test_failover: src/router.o src/net.o src/gateway.o src/logger.o tests/test_failover.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_failover tests/test_failover.c src/router.o src/net.o src/gateway.o src/logger.o
	./$(BIN_DIR)/test_failover

test_ratelimit: src/ratelimit.o src/logger.o tests/test_ratelimit.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_ratelimit tests/test_ratelimit.c src/ratelimit.o src/logger.o
	./$(BIN_DIR)/test_ratelimit

test_gateway: src/gateway.o src/logger.o tests/test_gateway.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_gateway tests/test_gateway.c src/gateway.o src/logger.o
	./$(BIN_DIR)/test_gateway

test_router: src/router.o src/logger.o src/net.o src/gateway.o tests/test_router.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_router tests/test_router.c src/router.o src/logger.o src/net.o src/gateway.o
	./$(BIN_DIR)/test_router

test_net: src/net.o src/logger.o tests/test_net.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_net tests/test_net.c src/net.o src/logger.o
	./$(BIN_DIR)/test_net

test_bufpool: src/bufpool.o tests/test_bufpool.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $(BIN_DIR)/test_bufpool tests/test_bufpool.c src/bufpool.o
	./$(BIN_DIR)/test_bufpool

# Unified Test Target
test: test_buffer test_config test_failover test_ratelimit test_gateway test_router test_net test_bufpool
	@echo "All unit tests passed successfully."

# Sanitizer Builds
asan: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
asan: clean all test

ubsan: CFLAGS += -fsanitize=undefined
ubsan: clean all test

clean:
	rm -f $(SRC_DIR)/*.o $(TARGET)

.PHONY: all clean test test_buffer test_config test_failover test_ratelimit test_gateway test_router asan ubsan
