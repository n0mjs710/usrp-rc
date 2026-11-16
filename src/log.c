#include "log.h"

#include <stdarg.h>
#include <strings.h>

log_level_t g_log_level = LOG_LEVEL_INFO;

static void (*s_sink)(void *arg, const char *text);
static void *s_sink_arg;

void log_set_sink(void (*sink)(void *arg, const char *text), void *arg)
{
    s_sink = sink;
    s_sink_arg = arg;
}

void log_print(log_level_t level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (s_sink && level <= LOG_LEVEL_INFO) {
        char text[512];
        va_list copy;
        va_copy(copy, ap);
        int n = vsnprintf(text, sizeof(text), fmt, copy);
        va_end(copy);
        if (n >= (int)sizeof(text))
            text[sizeof(text) - 2] = '\n';   /* cut short, still one line */
        s_sink(s_sink_arg, text);
    }
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void log_set_level(const char *name)
{
    if (!name || !name[0])
        return;

    if (strcasecmp(name, "error") == 0)
        g_log_level = LOG_LEVEL_ERROR;
    else if (strcasecmp(name, "warn") == 0)
        g_log_level = LOG_LEVEL_WARN;
    else if (strcasecmp(name, "info") == 0)
        g_log_level = LOG_LEVEL_INFO;
    else if (strcasecmp(name, "debug") == 0)
        g_log_level = LOG_LEVEL_DEBUG;
    else {
        g_log_level = LOG_LEVEL_INFO;
        LOGE("config: unknown log.level '%s'; using info\n", name);
    }
}
