#pragma once

#include <stdio.h>

/* Verbosity ceiling: each level shows everything at its own severity and
 * above (error is most severe, debug least). LOGE is unconditional --
 * errors are never suppressed regardless of configured level. */
typedef enum {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN  = 1,
    LOG_LEVEL_INFO  = 2,
    LOG_LEVEL_DEBUG = 3,
} log_level_t;

extern log_level_t g_log_level;

/* Parses "error"/"warn"/"info"/"debug" case-insensitively. Unknown values
 * log a warning and fall back to info. */
void log_set_level(const char *name);

/* Everything goes to stderr (the journal). Error, warn and info also go to the sink,
 * if one is set: the control terminal's view of the log. Debug stays out of it. */
void log_print(log_level_t level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* The sink gets the text of each call, which may be part of a line or several. */
void log_set_sink(void (*sink)(void *arg, const char *text), void *arg);

#define LOGE(...) log_print(LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOGW(...) do { if (g_log_level >= LOG_LEVEL_WARN)  log_print(LOG_LEVEL_WARN,  __VA_ARGS__); } while (0)
#define LOGI(...) do { if (g_log_level >= LOG_LEVEL_INFO)  log_print(LOG_LEVEL_INFO,  __VA_ARGS__); } while (0)
#define LOGD(...) do { if (g_log_level >= LOG_LEVEL_DEBUG) log_print(LOG_LEVEL_DEBUG, __VA_ARGS__); } while (0)
