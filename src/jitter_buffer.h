#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Plain arrival-order FIFO for link-RX audio, not a sequence-number-windowed
 * de-jitter buffer. Earlier versions policed incoming USRP sequence numbers
 * against a fixed playout window; the moment a source's numbering did
 * anything unexpected (observed in practice with ASL), that window could
 * not resync itself and the rest of a transmission went to silence while
 * real audio was discarded as "late" -- see the 2026-08-05 incident.
 *
 * fmgateway's USRPNetwork.cpp doesn't look at incoming sequence numbers at
 * all; it just appends payload bytes to a FIFO in UDP arrival order and
 * drains them. This buffer follows that model, with one addition: a small
 * prefill ("shock absorber") before playout starts, so ordinary arrival
 * jitter doesn't immediately show up as audible gaps. The heavier jitter
 * absorption happens one layer downstream already, in the MMDVM modem
 * firmware's CFMUpSampler (75ms ext-audio prefill) -- this buffer's own
 * prefill is deliberately small, a little on top of that, not a
 * replacement for it. */

#define JITTER_PREFILL_MIN_MS   20u
#define JITTER_PREFILL_MAX_MS   250u
#define JITTER_PREFILL_DEFAULT  40u   /* 2 frames -- "a little" on top of the modem's 75ms */

typedef struct jitter_buffer jitter_buffer_t;

int  jitter_buffer_create(jitter_buffer_t **jb, unsigned int prefill_ms);

/* Push a 160-sample voice frame in arrival order. No sequence number is
 * consulted -- this is a FIFO, not a reorder window. If the buffer is at
 * capacity (a burst arrived faster than it's being drained), the oldest
 * buffered frame is dropped to make room, favoring fresh audio over stale
 * backlog; jitter_buffer_overflow_count() reflects this. */
void jitter_buffer_push(jitter_buffer_t *jb, const int16_t *samples);

/* Pull the next 160-sample frame for playout. Returns true if a real frame
 * was available; false if silence was emitted, either because the buffer
 * is still priming (accumulating its prefill target after a flush) or
 * because it genuinely ran dry. Priming silence is not counted; a dry-run
 * underrun is. */
bool jitter_buffer_pull(jitter_buffer_t *jb, int16_t *samples_out);

/* Drain all pending frames and re-arm the prefill ramp (call on UNKEY). */
void jitter_buffer_flush(jitter_buffer_t *jb);

/* Frames dropped because the buffer was full, since the last call (resets). Expected
 * to be rare: only a sustained burst fills the buffer. */
uint64_t jitter_buffer_overflow_count(jitter_buffer_t *jb);

/* Pulls that found it dry after priming, since the last call (resets). */
uint64_t jitter_buffer_silence_count(jitter_buffer_t *jb);

void jitter_buffer_destroy(jitter_buffer_t *jb);
