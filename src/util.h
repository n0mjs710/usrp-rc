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

