#pragma once

#include <stdbool.h>
#include <time.h>

/* Repeater usage over a rolling week, in hourly buckets: key-ups and time spent
 * repeating traffic. Survives restarts through a small text file, written now and
 * then rather than on every key-up to go easy on the SD card, so a power failure
 * loses at most the time since the last save.
 *
 * Every call takes the time, which keeps this deterministic under test. */

typedef struct stats stats_t;

/* path NULL or empty = keep usage in memory only. A missing file is a fresh start. */
int  stats_create(stats_t **out, const char *path, time_t now);
void stats_destroy(stats_t *s);

/* One transmission finished, counted against the hour it ended in. */
void stats_record(stats_t *s, time_t now, double seconds);

/* Totals over the last `hours` hours, up to 168 (including the current hour). */
long stats_keyups(const stats_t *s, time_t now, int hours);
long stats_minutes(const stats_t *s, time_t now, int hours);

/* Write the file if anything changed. Returns 0 on success or nothing to do. */
int stats_save(stats_t *s);
