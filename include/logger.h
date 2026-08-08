#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>

typedef enum {
    LOG_ERROR = 0,
    LOG_WARN  = 1,
    LOG_INFO  = 2,
    LOG_DEBUG = 3
} LogLevel;

// Global log level filter (runtime configurable)
// Only messages at or above this level will be printed
// Default: LOG_DEBUG (all messages)
extern LogLevel g_log_level;

void log_message(LogLevel level, const char *format, ...);
void log_set_level(LogLevel level);

#define LOG_INFO(...)  do { if (LOG_INFO <= g_log_level)  log_message(LOG_INFO, __VA_ARGS__); } while(0)
#define LOG_WARN(...)  do { if (LOG_WARN <= g_log_level)  log_message(LOG_WARN, __VA_ARGS__); } while(0)
#define LOG_ERROR(...) do { if (LOG_ERROR <= g_log_level) log_message(LOG_ERROR, __VA_ARGS__); } while(0)
#define LOG_DEBUG(...) do { if (LOG_DEBUG <= g_log_level) log_message(LOG_DEBUG, __VA_ARGS__); } while(0)

#endif // LOGGER_H
