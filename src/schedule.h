#pragma once

#include "config.h"

#include <stdbool.h>
#include <time.h>

/* Is this entry's window open at this time? An overnight window belongs to the day
 * it starts on: "fri 22:00-02:00" is still open at 01:00 on Saturday. */
bool schedule_entry_active(const config_schedule_t *entry, const struct tm *when);

/* Windows started or stopped by hand. The last event wins: a start or stop holds until
 * the window's schedule next changes, which clears it. Not kept over a restart. */
typedef struct {
    signed char manual[CFG_MAX_SCHEDULE];   /* 1 started, 0 stopped, -1 as scheduled */
    bool        scheduled[CFG_MAX_SCHEDULE];   /* as last seen, to notice a change */
} schedule_hand_t;

/* All windows as scheduled, none by hand. */
void schedule_hand_init(schedule_hand_t *h);

/* The settings in force at this time: base, with each open window's settings
 * applied in file order, so a later entry wins. field_on[i] says whether entry i's
 * producer field has current words (NULL: none do). active[i] says whether entry i is
 * open. hand may be NULL: as scheduled. Only the hand state is remembered between calls: a window's changes end when
 * it does, and a restart in the middle of one picks it straight back up. out shares
 * base's messages, so only base is freed. */
void schedule_evaluate(const config_t *base, const struct tm *when, const bool *field_on,
                       schedule_hand_t *hand, config_t *out, bool active[CFG_MAX_SCHEDULE]);
