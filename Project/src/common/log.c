#include "common/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>

/* -----------------------------------------------------------------------
 * Thread-safe structured logger with colorized output.
 * Format: [2026-06-10 17:03:16.234] [INFO ] [raft] Node 1 elected leader
 * ----------------------------------------------------------------------- */

static log_level_t g_min_level = LOG_LEVEL_INFO;
static int         g_use_color = -1; /* -1 = auto-detect */
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ANSI color codes */
static const char *level_colors[] = {
    "\033[90m",   /* TRACE - gray */
    "\033[36m",   /* DEBUG - cyan */
    "\033[32m",   /* INFO  - green */
    "\033[33m",   /* WARN  - yellow */
    "\033[31m",   /* ERROR - red */
    "\033[35;1m", /* FATAL - bold magenta */
};

static const char *level_names[] = {
    "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL",
};

void log_set_level(log_level_t level) {
    g_min_level = level;
}

log_level_t log_get_level(void) {
    return g_min_level;
}

void log_set_color(int enabled) {
    g_use_color = enabled;
}

static int should_use_color(void) {
    if (g_use_color >= 0)
        return g_use_color;
    return isatty(STDERR_FILENO);
}

void log_write(log_level_t level, const char *module, const char *file,
               int line, const char *fmt, ...) {
    if (level < g_min_level)
        return;

    /* Get timestamp with millisecond precision */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);

    char timebuf[32];
    int tlen = (int)strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm);
    snprintf(timebuf + tlen, sizeof(timebuf) - (size_t)tlen, ".%03d", (int)(tv.tv_usec / 1000));

    /* Extract just the filename from the path */
    const char *basename = strrchr(file, '/');
    if (!basename) basename = strrchr(file, '\\');
    basename = basename ? basename + 1 : file;

    /* Format the user message */
    char msgbuf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msgbuf, sizeof(msgbuf), fmt, ap);
    va_end(ap);

    /* Write atomically under lock */
    pthread_mutex_lock(&g_log_mutex);

    int color = should_use_color();
    if (color) {
        fprintf(stderr, "\033[90m[%s]\033[0m %s[%s]\033[0m \033[90m[%s]\033[0m %s",
                timebuf,
                level_colors[level], level_names[level],
                module,
                msgbuf);
        if (level >= LOG_LEVEL_WARN) {
            fprintf(stderr, " \033[90m(%s:%d)\033[0m", basename, line);
        }
        fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "[%s] [%s] [%s] %s",
                timebuf, level_names[level], module, msgbuf);
        if (level >= LOG_LEVEL_WARN) {
            fprintf(stderr, " (%s:%d)", basename, line);
        }
        fprintf(stderr, "\n");
    }

    fflush(stderr);
    pthread_mutex_unlock(&g_log_mutex);

    /* Fatal is fatal */
    if (level == LOG_LEVEL_FATAL) {
        abort();
    }
}
