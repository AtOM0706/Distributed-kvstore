#ifndef KVSTORE_LOG_H
#define KVSTORE_LOG_H

#include <stdio.h>
#include <stdarg.h>

/* -----------------------------------------------------------------------
 * Structured logging with levels, timestamps, colors, and thread safety.
 * Usage:
 *   LOG_INFO("raft", "Node %d elected leader for term %lu", id, term);
 *   LOG_ERROR("wal", "Failed to fsync: %s", strerror(errno));
 * ----------------------------------------------------------------------- */

typedef enum {
    LOG_LEVEL_TRACE = 0,
    LOG_LEVEL_DEBUG = 1,
    LOG_LEVEL_INFO  = 2,
    LOG_LEVEL_WARN  = 3,
    LOG_LEVEL_ERROR = 4,
    LOG_LEVEL_FATAL = 5,
} log_level_t;

/* Set the minimum log level (messages below this are suppressed) */
void log_set_level(log_level_t level);

/* Get the current log level */
log_level_t log_get_level(void);

/* Enable or disable color output (enabled by default if stdout is a tty) */
void log_set_color(int enabled);

/* Core logging function — use the macros below instead */
void log_write(log_level_t level, const char *module, const char *file,
               int line, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

/* Convenience macros */
#define LOG_TRACE(module, ...) \
    log_write(LOG_LEVEL_TRACE, module, __FILE__, __LINE__, __VA_ARGS__)

#define LOG_DEBUG(module, ...) \
    log_write(LOG_LEVEL_DEBUG, module, __FILE__, __LINE__, __VA_ARGS__)

#define LOG_INFO(module, ...) \
    log_write(LOG_LEVEL_INFO, module, __FILE__, __LINE__, __VA_ARGS__)

#define LOG_WARN(module, ...) \
    log_write(LOG_LEVEL_WARN, module, __FILE__, __LINE__, __VA_ARGS__)

#define LOG_ERROR(module, ...) \
    log_write(LOG_LEVEL_ERROR, module, __FILE__, __LINE__, __VA_ARGS__)

#define LOG_FATAL(module, ...) \
    log_write(LOG_LEVEL_FATAL, module, __FILE__, __LINE__, __VA_ARGS__)

#endif /* KVSTORE_LOG_H */
