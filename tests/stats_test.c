/* Usage statistics: rolling windows, persistence, and damaged files. */
#include "../src/stats.h"
#include "../src/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int s_fail;

static void check(const char *what, int ok)
{
    printf("  %s - %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        s_fail++;
}

#define HOUR 3600
#define T0   ((time_t)1800000000 / HOUR * HOUR)   /* the top of an hour */

int main(void)
{
    log_set_level("error");

    char path[] = "/tmp/usrp_rc_stats_test_XXXXXX";
    int fd = mkstemp(path);
    close(fd);
    unlink(path);

    stats_t *s;
    stats_create(&s, path, T0);
    check("a missing file is a fresh start", stats_keyups(s, T0, 168) == 0 && stats_minutes(s, T0, 168) == 0);

    stats_record(s, T0, 60.0);
    stats_record(s, T0 + 10, 30.0);
    stats_record(s, T0 + 5 * HOUR, 120.0);
    time_t now = T0 + 5 * HOUR;
    check("key-ups add up", stats_keyups(s, now, 24) == 3);
    check("minutes add up", stats_minutes(s, now, 24) == 4);
    check("minutes round to nearest", stats_minutes(s, T0, 24) == 2);

    check("a one-hour window sees only this hour", stats_keyups(s, now, 1) == 1);
    now = T0 + 23 * HOUR;
    check("still in the day at 23 hours", stats_keyups(s, now, 24) == 3);
    now = T0 + 24 * HOUR;
    check("the oldest hour leaves the day at 24 hours", stats_keyups(s, now, 24) == 1);
    check("... but is still in the week", stats_keyups(s, now, 168) == 3);
    now = T0 + 168 * HOUR;
    check("it leaves the week at 168 hours", stats_keyups(s, now, 168) == 1);
    now = T0 + 300 * HOUR;
    check("everything ages out", stats_keyups(s, now, 168) == 0);

    /* An old bucket gets reused rather than added to. */
    stats_record(s, T0 + 168 * HOUR, 60.0);
    now = T0 + 168 * HOUR;
    check("reusing a slot starts it fresh", stats_keyups(s, now, 1) == 1);

    check("save succeeds", stats_save(s) == 0);
    stats_destroy(s);

    stats_create(&s, path, now);
    check("figures survive a restart", stats_keyups(s, now, 168) == 2);
    stats_destroy(s);

    stats_create(&s, path, now + 200 * HOUR);
    check("old figures on disk stay out of the window", stats_keyups(s, now + 200 * HOUR, 168) == 0);
    stats_destroy(s);

    FILE *fp = fopen(path, "w");
    fprintf(fp, "garbage line\n%ld 5 300.0\n1 2\n%ld -4 10.0\n", (long)(now / HOUR), (long)(now / HOUR) - 1);
    fclose(fp);
    stats_create(&s, path, now);
    check("a damaged file keeps what it can", stats_keyups(s, now, 168) == 5 && stats_minutes(s, now, 168) == 5);
    stats_destroy(s);

    stats_create(&s, path, now);
    long before = stats_keyups(s, now, 168);
    check("nothing changed, nothing to save", stats_save(s) == 0 && before == 5);
    stats_destroy(s);
    unlink(path);

    stats_create(&s, "", now);
    stats_record(s, now, 60.0);
    check("no file: works in memory", stats_keyups(s, now, 1) == 1 && stats_save(s) == 0);
    stats_destroy(s);

    stats_create(&s, "/nonexistent-dir/stats", now);
    stats_record(s, now, 60.0);
    check("an unwritable file reports failure but keeps counting",
          stats_save(s) != 0 && stats_keyups(s, now, 1) == 1);
    stats_destroy(s);

    if (s_fail) {
        printf("\n==== %d check(s) FAILED ====\n", s_fail);
        return 1;
    }
    printf("\n==== all checks passed ====\n");
    return 0;
}
