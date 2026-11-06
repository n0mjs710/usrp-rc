#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Out of memory is fatal: there's no sensible way to carry on, and systemd restarts
 * the service. */
static inline void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (!q && n) {
        fprintf(stderr, "usrp-rc: out of memory\n");
        abort();
    }
    return q;
}

static inline uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Process CPU time (user+sys), unlike monotonic_ms() does NOT advance while
 * we're descheduled. Comparing a gap in both clocks attributes it: if CPU
 * time barely moved while wall time jumped, the OS took the CPU away from
 * us (scheduling contention, SD-card I/O, journald backpressure); if CPU
 * time moved by roughly the same amount, we spent that time actually
 * running. */
static inline uint64_t cpu_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
