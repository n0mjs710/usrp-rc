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
    int       lead_samples;   /* leading-edge mute, per transmission */
    int       lead_left;      /* still to mute in this transmission */
};

int ste_create(ste_t **out, int delay_ms, int lead_trim_ms)
{
    ste_t *s = calloc(1, sizeof(*s));
    if (!s)
        return -1;

    if (lead_trim_ms > 0) {
        s->lead_samples = lead_trim_ms * 8;   /* 8 samples/ms @ 8 kHz */
        s->lead_left    = s->lead_samples;
    }

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
    int16_t frame[STE_FRAME_SAMPLES];
    memcpy(frame, in, sizeof(frame));

    if (s->lead_left > 0) {
        int n = s->lead_left < STE_FRAME_SAMPLES ? s->lead_left : STE_FRAME_SAMPLES;
        memset(frame, 0, (size_t)n * sizeof(int16_t));
        s->lead_left -= n;
    }

    if (!s->enabled) {
        memcpy(out, frame, sizeof(frame));
        return true;
    }

    memcpy(s->frames[s->tail], frame, sizeof(frame));
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
    s->lead_left = s->lead_samples;
}

void ste_destroy(ste_t *s)
{
    if (!s)
        return;
    free(s->frames);
    free(s);
}
