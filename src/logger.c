#include "logger.h"
#include <stdarg.h>
#include <time.h>

// Global log level filter (runtime configurable)
// Default: LOG_DEBUG (all messages)
LogLevel g_log_level = LOG_DEBUG;

void log_set_level(LogLevel level) {
    g_log_level = level;
}

void log_message(LogLevel level, const char *format, ...) {
    // Skip if message level is below current filter
    if (level > g_log_level) return;
    
    time_t now;
    time(&now);
    struct tm *local = localtime(&now);
    
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", local);

    const char *level_str = "";
    FILE *out_stream = stdout;

    switch (level) {
        case LOG_INFO:  level_str = "INFO";  break;
        case LOG_WARN:  level_str = "WARN";  break;
        case LOG_ERROR: level_str = "ERROR"; out_stream = stderr; break;
        case LOG_DEBUG: level_str = "DEBUG"; break;
    }

    fprintf(out_stream, "[%s] [%-5s] ", time_str, level_str);

    va_list args;
    va_start(args, format);
    vfprintf(out_stream, format, args);
    va_end(args);

    fprintf(out_stream, "\n");
    fflush(out_stream);
}
