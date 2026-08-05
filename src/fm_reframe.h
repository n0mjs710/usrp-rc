#pragma once

/* Variable-length -> fixed 160-sample reframer for the MMDVM-Host FM
 * network receive path.
 *
 * MMDVM-Host does not send 20 ms frames. FMControl::writeModem() forwards
 * whatever the modem handed it that pass, rounded down to a whole 12-bit
 * sample pair and capped at 160 samples -- so an FMD payload is any even
 * count from 2 to 160 samples. Everything downstream in this program (the
 * STE delay line, the jitter buffer, port_mix_apply(), the Opus
 * accumulator) is built on exactly-160-sample frames, so the socket layer
 * has to put the stream back on that grid before handing it over.
 *
 * Note this reframer is NOT a jitter buffer and deliberately does no
 * timing: it holds at most one partial frame (<160 samples) and emits as
 * soon as a full frame exists, so it adds no latency of its own beyond the
 * unavoidable fill time. Delivery-timing analysis stays where it already
 * is, in main.c's pacing monitors. */

#include <stdint.h>
#include <string.h>

#define FM_REFRAME_SAMPLES 160u

/* Room for a full inbound burst plus a partial frame. MMDVM-Host caps a
 * packet at 160 samples and we drain to empty on every pass, so the live
 * occupancy is always < 160 + 160; the rest is slack against a pathological
 * backlog rather than an expected working set. */
#define FM_REFRAME_CAP     1024u

typedef struct {
    int16_t  buf[FM_REFRAME_CAP];
    unsigned len;      /* samples currently held */
} fm_reframe_t;

static inline void fm_reframe_reset(fm_reframe_t *r)
{
    r->len = 0;
}

/* Append n samples. Silently drops the overflow rather than corrupting the
 * frame grid -- if this ever trips, the receive loop has fallen far enough
 * behind that the audio is lost regardless. */
static inline void fm_reframe_push(fm_reframe_t *r, const int16_t *src, unsigned n)
{
    if (n > FM_REFRAME_CAP - r->len)
        n = FM_REFRAME_CAP - r->len;
    if (n == 0)
        return;
    memcpy(r->buf + r->len, src, (size_t)n * sizeof(int16_t));
    r->len += n;
}

/* Pop exactly FM_REFRAME_SAMPLES into out. Returns 1 if a full frame was
 * available, 0 otherwise (out untouched). */
static inline int fm_reframe_pop(fm_reframe_t *r, int16_t *out)
{
    if (r->len < FM_REFRAME_SAMPLES)
        return 0;

    memcpy(out, r->buf, FM_REFRAME_SAMPLES * sizeof(int16_t));
    r->len -= FM_REFRAME_SAMPLES;
    if (r->len)
        memmove(r->buf, r->buf + FM_REFRAME_SAMPLES, (size_t)r->len * sizeof(int16_t));

    return 1;
}

/* Zero-pad whatever partial frame remains up to a full frame and pop it.
 * Returns 1 if there was a remainder to flush, 0 if empty. Used at end of
 * transmission (FME) so the tail isn't left stranded in the buffer to be
 * prepended to the *next* transmission. */
static inline int fm_reframe_flush(fm_reframe_t *r, int16_t *out)
{
    if (r->len == 0)
        return 0;

    /* Callers pop to exhaustion first, so len is < a full frame here; the
     * clamp keeps a full frame from overrunning `out` if that ever stops
     * being true. */
    unsigned n = r->len < FM_REFRAME_SAMPLES ? r->len : FM_REFRAME_SAMPLES;

    memcpy(out, r->buf, (size_t)n * sizeof(int16_t));
    if (n < FM_REFRAME_SAMPLES)
        memset(out + n, 0, (size_t)(FM_REFRAME_SAMPLES - n) * sizeof(int16_t));
    r->len = 0;

    return 1;
}
