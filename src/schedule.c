#include "schedule.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    int year, mon, mday, wday;   /* mon is 0-11, wday 0 = Sunday */
} day_t;

static int days_in_month(int year, int mon)
{
    static const int DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return mon == 1 && leap ? 29 : DAYS[mon];
}

static day_t day_before(day_t d)
{
    d.wday = (d.wday + 6) % 7;
    if (d.mday > 1) {
        d.mday--;
        return d;
    }
    if (d.mon == 0) {
        d.mon = 11;
        d.year--;
    } else {
        d.mon--;
    }
    d.mday = days_in_month(d.year, d.mon);
    return d;
}

static bool day_matches(const config_schedule_t *entry, day_t d)
{
    if (entry->days & (1u << d.wday))
        return true;
    for (int i = 0; i < entry->nrules; i++) {
        const config_day_rule_t *r = &entry->rules[i];
        switch (r->type) {
        case DAY_RULE_NTH_WEEKDAY:
            if (d.wday == r->b && (d.mday - 1) / 7 + 1 == r->a)
                return true;
            break;
        case DAY_RULE_MONTH_DAY:
            if (d.mday == r->a)
                return true;
            break;
        case DAY_RULE_LAST_DAY:
            if (d.mday == days_in_month(d.year, d.mon))
                return true;
            break;
        case DAY_RULE_DATE:
            if (d.mon == r->a && d.mday == r->b)
                return true;
            break;
        }
    }
    return false;
}

bool schedule_entry_active(const config_schedule_t *entry, const struct tm *when)
{
    int now = when->tm_hour * 60 + when->tm_min;
    day_t today = {when->tm_year + 1900, when->tm_mon, when->tm_mday, when->tm_wday};

    if (entry->start_min < entry->end_min)
        return now >= entry->start_min && now < entry->end_min && day_matches(entry, today);

    return (now >= entry->start_min && day_matches(entry, today))
        || (now < entry->end_min && day_matches(entry, day_before(today)));
}

void schedule_evaluate(const config_t *base, const struct tm *when,
                       config_t *out, bool active[CFG_MAX_SCHEDULE])
{
    *out = *base;
    for (int i = 0; i < base->nschedule; i++) {
        active[i] = schedule_entry_active(&base->schedule[i], when);
        if (!active[i])
            continue;
        for (int j = 0; j < base->schedule[i].nsets; j++)
            config_apply_override(out, &base->schedule[i].sets[j]);
    }
}
