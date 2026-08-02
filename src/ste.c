#include "ste.h"

#include <stdlib.h>
#include <string.h>

#define STE_FRAME_SAMPLES 160   /* 20 ms @ 8 kHz */

struct ste {
    bool      enabled;
    int       depth;      /* frames of delay */
    int       cap;        /* depth + 1 */
    int16_t (*frames)[STE_FRAME_SAMPLES];
    int       count;
    int       head;
    int       tail;
};

int ste_create(ste_t **out, int delay_ms)
{
    ste_t *s = calloc(1, sizeof(*s));
    if (!s)
        return -1;

    if (delay_ms > 0) {
        s->enabled = true;
        s->depth   = (delay_ms + 19) / 20;
        if (s->depth < 1)
            s->depth = 1;
        s->cap    = s->depth + 1;
        s->frames = calloc((size_t)s->cap, sizeof(*s->frames));
        if (!s->frames) {
            free(s);
            return -1;
        }
    }

    *out = s;
    return 0;
}

bool ste_push(ste_t *s, const int16_t *in, int16_t *out)
{
    if (!s->enabled) {
        memcpy(out, in, STE_FRAME_SAMPLES * sizeof(int16_t));
        return true;
    }

    memcpy(s->frames[s->tail], in, STE_FRAME_SAMPLES * sizeof(int16_t));
    s->tail = (s->tail + 1) % s->cap;
    s->count++;

    if (s->count > s->depth) {
        memcpy(out, s->frames[s->head], STE_FRAME_SAMPLES * sizeof(int16_t));
        s->head = (s->head + 1) % s->cap;
        s->count--;
        return true;
    }
    return false;
}

void ste_reset(ste_t *s)
{
    s->count = 0;
    s->head  = 0;
    s->tail  = 0;
}

void ste_destroy(ste_t *s)
{
    if (!s)
        return;
    free(s->frames);
    free(s);
}
