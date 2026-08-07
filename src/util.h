#pragma once

#include <stdint.h>
#include <time.h>

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
