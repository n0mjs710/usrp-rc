#pragma once

#include "config.h"

#include <stdbool.h>
#include <time.h>

/* Is this entry's window open at this time? An overnight window belongs to the day
 * it starts on: "fri 22:00-02:00" is still open at 01:00 on Saturday. */
bool schedule_entry_active(const config_schedule_t *entry, const struct tm *when);

/* The events in force at this time: the config's, with each open window's settings
 * applied in file order, so a later entry wins. active[i] says whether entry i is
 * open. Nothing is remembered between calls: a window's changes end when it does,
 * and a restart in the middle of one picks it straight back up. */
void schedule_evaluate(const config_t *cfg, const struct tm *when,
                       config_events_t *out, bool active[CFG_MAX_SCHEDULE]);
