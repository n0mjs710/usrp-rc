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

#define LOGE(...) fprintf(stderr, __VA_ARGS__)
#define LOGW(...) do { if (g_log_level >= LOG_LEVEL_WARN)  fprintf(stderr, __VA_ARGS__); } while (0)
#define LOGI(...) do { if (g_log_level >= LOG_LEVEL_INFO)  fprintf(stderr, __VA_ARGS__); } while (0)
#define LOGD(...) do { if (g_log_level >= LOG_LEVEL_DEBUG) fprintf(stderr, __VA_ARGS__); } while (0)
