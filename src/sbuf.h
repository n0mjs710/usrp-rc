#pragma once

/* Tiny growable int16 sample buffer, shared by morse.c and message.c for
 * concatenating rendered audio segments. */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int16_t *data;
    size_t   len;
    size_t   cap;
} sbuf_t;

static inline void sbuf_reserve(sbuf_t *b, size_t extra)
{
    if (b->len + extra <= b->cap)
        return;
    size_t new_cap = b->cap ? b->cap * 2 : 256;
    while (new_cap < b->len + extra)
        new_cap *= 2;
    b->data = realloc(b->data, new_cap * sizeof(int16_t));
    b->cap  = new_cap;
}

static inline void sbuf_append(sbuf_t *b, const int16_t *src, size_t n)
{
    if (n == 0)
        return;
    sbuf_reserve(b, n);
    memcpy(b->data + b->len, src, n * sizeof(int16_t));
    b->len += n;
}

static inline void sbuf_append_silence(sbuf_t *b, size_t n)
{
    if (n == 0)
        return;
    sbuf_reserve(b, n);
    memset(b->data + b->len, 0, n * sizeof(int16_t));
    b->len += n;
}

/* Append src scaled by `level` (0.0-1.0+, clamped to int16 range). */
static inline void sbuf_append_scaled(sbuf_t *b, const int16_t *src, size_t n, double level)
{
    if (n == 0)
        return;
    sbuf_reserve(b, n);
    for (size_t i = 0; i < n; i++) {
        double s = (double)src[i] * level;
        if (s > 32767.0)  s = 32767.0;
        if (s < -32768.0) s = -32768.0;
        b->data[b->len + i] = (int16_t)s;
    }
    b->len += n;
}
