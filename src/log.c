#include "log.h"

#include <strings.h>

log_level_t g_log_level = LOG_LEVEL_INFO;

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
