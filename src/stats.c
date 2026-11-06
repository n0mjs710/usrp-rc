#include "stats.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NBUCKETS 168   /* one week of hours */

typedef struct {
    long   hour;       /* hours since the epoch; timezone-free, so DST can't confuse it */
    long   keyups;
    double seconds;
} bucket_t;

struct stats {
    bucket_t buckets[NBUCKETS];
    char    *path;
    bool     dirty;
};

static bucket_t *bucket_for(stats_t *s, long hour)
{
    bucket_t *b = &s->buckets[hour % NBUCKETS];
    if (b->hour != hour) {
        b->hour = hour;
        b->keyups = 0;
        b->seconds = 0.0;
    }
    return b;
}

static void load(stats_t *s, time_t now)
{
    FILE *fp = fopen(s->path, "r");
    if (!fp) {
        if (errno != ENOENT)
            LOGW("stats: can't read %s: %s\n", s->path, strerror(errno));
        return;
    }
    long newest = (long)(now / 3600);
    char line[128];
    while (fgets(line, sizeof(line), fp)) {
        long hour, keyups;
        double seconds;
        if (line[0] == '#' || sscanf(line, "%ld %ld %lf", &hour, &keyups, &seconds) != 3)
            continue;
        /* Hours ahead of the clock are kept: a board with no RTC can boot before NTP
         * sets it. The window in sum() leaves them out until the clock catches up. */
        if (hour <= newest - NBUCKETS || keyups < 0 || seconds < 0.0)
            continue;
        bucket_t *b = bucket_for(s, hour);
        b->keyups = keyups;
        b->seconds = seconds;
    }
    fclose(fp);
}

int stats_create(stats_t **out, const char *path, time_t now)
{
    stats_t *s = calloc(1, sizeof(*s));
    if (!s)
        return -1;
    for (int i = 0; i < NBUCKETS; i++)
        s->buckets[i].hour = -1;
    if (path && path[0]) {
        s->path = strdup(path);
        load(s, now);
    }
    *out = s;
    return 0;
}

void stats_destroy(stats_t *s)
{
    if (!s)
        return;
    free(s->path);
    free(s);
}

void stats_record(stats_t *s, time_t now, double seconds)
{
    bucket_t *b = bucket_for(s, (long)(now / 3600));
    b->keyups++;
    b->seconds += seconds;
    s->dirty = true;
}

static double sum(const stats_t *s, time_t now, int hours, bool want_keyups)
{
    long newest = (long)(now / 3600);
    double total = 0.0;
    for (int i = 0; i < NBUCKETS; i++) {
        const bucket_t *b = &s->buckets[i];
        if (b->hour > newest - hours && b->hour <= newest)
            total += want_keyups ? (double)b->keyups : b->seconds;
    }
    return total;
}

long stats_keyups(const stats_t *s, time_t now, int hours)
{
    return (long)sum(s, now, hours, true);
}

long stats_minutes(const stats_t *s, time_t now, int hours)
{
    return (long)(sum(s, now, hours, false) / 60.0 + 0.5);
}

int stats_save(stats_t *s)
{
    if (!s->path || !s->dirty)
        return 0;

    /* Temp file and rename, so a power failure leaves the old file or the new one. */
    char tmp[4096];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", s->path) >= (int)sizeof(tmp))
        return -1;
    FILE *fp = fopen(tmp, "w");
    if (!fp) {
        LOGW("stats: can't write %s: %s\n", tmp, strerror(errno));
        return -1;
    }
    fprintf(fp, "# usrp-rc usage: hour-since-epoch keyups seconds\n");
    for (int i = 0; i < NBUCKETS; i++) {
        const bucket_t *b = &s->buckets[i];
        if (b->hour >= 0 && (b->keyups || b->seconds > 0.0))
            fprintf(fp, "%ld %ld %.1f\n", b->hour, b->keyups, b->seconds);
    }
    bool ok = fflush(fp) == 0 && fsync(fileno(fp)) == 0;
    ok = fclose(fp) == 0 && ok;
    if (!ok || rename(tmp, s->path) != 0) {
        LOGW("stats: can't save %s: %s\n", s->path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    s->dirty = false;
    return 0;
}
