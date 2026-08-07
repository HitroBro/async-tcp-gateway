# Compiler and Flags
CC = gcc
CFLAGS = -Wall -Wextra -pedantic -std=c11 -g -I./include

# Directories
SRC_DIR = src
BIN_DIR = bin

# Source and Object Files
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
clean:
	rm -f $(SRC_DIR)/*.o $(TARGET)

.PHONY: all clean
