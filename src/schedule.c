#include "schedule.h"

#include <stdio.h>
#include <string.h>

bool schedule_entry_active(const config_schedule_t *entry, const struct tm *when)
{
    int now = when->tm_hour * 60 + when->tm_min;
    bool today = entry->days & (1u << when->tm_wday);

    if (entry->start_min < entry->end_min)
        return today && now >= entry->start_min && now < entry->end_min;

    bool yesterday = entry->days & (1u << ((when->tm_wday + 6) % 7));
    return (today && now >= entry->start_min) || (yesterday && now < entry->end_min);
}

static void apply(config_events_t *ev, const config_override_t *o)
{
    char *dst = (char *)ev + o->offset;

    if (!o->count_offset) {
        snprintf(dst, o->size, "%s", o->value);
        return;
    }

    char (*names)[CFG_NAME_STR] = (char (*)[CFG_NAME_STR])dst;
    int max = (int)(o->size / CFG_NAME_STR), count = 0;
    char copy[sizeof(o->value)];
    strcpy(copy, o->value);
    char *save = NULL;
    for (char *n = strtok_r(copy, ",", &save); n && count < max; n = strtok_r(NULL, ",", &save))
        snprintf(names[count++], CFG_NAME_STR, "%s", n);
    *(int *)((char *)ev + o->count_offset) = count;
}

void schedule_evaluate(const config_t *cfg, const struct tm *when,
                       config_events_t *out, bool active[CFG_MAX_SCHEDULE])
{
    *out = cfg->events;
    for (int i = 0; i < cfg->nschedule; i++) {
        active[i] = schedule_entry_active(&cfg->schedule[i], when);
        if (!active[i])
            continue;
        for (int j = 0; j < cfg->schedule[i].nsets; j++)
            apply(out, &cfg->schedule[i].sets[j]);
    }
}
