#include "jitter_buffer.h"
#include "usrp_protocol.h"

#include <stdlib.h>
#include <string.h>

/* Hard overflow ceiling, independent of the (much smaller) prefill target --
 * bounds worst-case latency growth during a burst. Not a reorder window. */
#define MAX_SLOTS 16u   /* 320ms at 20ms/frame */

typedef struct {
    int16_t samples[USRP_AUDIO_FRAMES];
} jb_slot_t;

struct jitter_buffer {
    jb_slot_t        slots[MAX_SLOTS];
    unsigned int     read_idx;       /* next slot to drain */
    unsigned int     count;          /* frames currently buffered */
    unsigned int     prefill_frames; /* target depth before playout starts */
    bool             priming;        /* true until count reaches prefill_frames */
    uint64_t         silence_count;
    uint64_t         overflow_count;
};

int jitter_buffer_create(jitter_buffer_t **jb, unsigned int prefill_ms)
{
    if (prefill_ms < JITTER_PREFILL_MIN_MS) prefill_ms = JITTER_PREFILL_MIN_MS;
    if (prefill_ms > JITTER_PREFILL_MAX_MS) prefill_ms = JITTER_PREFILL_MAX_MS;

    jitter_buffer_t *self = calloc(1, sizeof(*self));
    if (!self) return -1;

    self->prefill_frames = (prefill_ms + 19u) / 20u;
    if (self->prefill_frames > MAX_SLOTS) self->prefill_frames = MAX_SLOTS;
    self->priming = true;
    *jb = self;
    return 0;
}

void jitter_buffer_push(jitter_buffer_t *jb, const int16_t *samples)
{
    if (jb->count >= MAX_SLOTS) {
        /* At capacity: drop the oldest frame to make room for this one --
         * prefer fresh audio over stale backlog. */
        jb->read_idx = (jb->read_idx + 1u) % MAX_SLOTS;
        jb->count--;
        jb->overflow_count++;
    }

    unsigned int write_idx = (jb->read_idx + jb->count) % MAX_SLOTS;
    memcpy(jb->slots[write_idx].samples, samples, USRP_AUDIO_BYTES);
    jb->count++;

}

bool jitter_buffer_pull(jitter_buffer_t *jb, int16_t *samples_out)
{

    if (jb->priming) {
        if (jb->count < jb->prefill_frames) {
            memset(samples_out, 0, USRP_AUDIO_BYTES);
            return false;
        }
        jb->priming = false;
    }

    bool real;
    if (jb->count > 0) {
        memcpy(samples_out, jb->slots[jb->read_idx].samples, USRP_AUDIO_BYTES);
        jb->read_idx = (jb->read_idx + 1u) % MAX_SLOTS;
        jb->count--;
        real = true;
    } else {
        memset(samples_out, 0, USRP_AUDIO_BYTES);
        jb->silence_count++;
        real = false;
    }

    return real;
}

void jitter_buffer_flush(jitter_buffer_t *jb)
{
    jb->read_idx = 0;
    jb->count    = 0;
    jb->priming  = true;
}

static uint64_t take(uint64_t *v)
{
    uint64_t was = *v;
    *v = 0;
    return was;
}

uint64_t jitter_buffer_overflow_count(jitter_buffer_t *jb)
{
    return take(&jb->overflow_count);
}

uint64_t jitter_buffer_silence_count(jitter_buffer_t *jb)
{
    return take(&jb->silence_count);
}

void jitter_buffer_destroy(jitter_buffer_t *jb)
{
    free(jb);
}
