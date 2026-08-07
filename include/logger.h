#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>

typedef enum {
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_DEBUG
} LogLevel;

void log_message(LogLevel level, const char *format, ...);

#define LOG_INFO(...)  log_message(LOG_INFO, __VA_ARGS__)
#define LOG_WARN(...)  log_message(LOG_WARN, __VA_ARGS__)
#define LOG_ERROR(...) log_message(LOG_ERROR, __VA_ARGS__)
#define LOG_DEBUG(...) log_message(LOG_DEBUG, __VA_ARGS__)

#endif // LOGGER_H
