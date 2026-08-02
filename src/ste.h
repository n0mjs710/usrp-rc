#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Squelch tail elimination: a FIFO delay of ste_delay_ms on the mmdvm-RX ->
 * link-TX path. While the gate is held open, 20 ms frames are pushed in and
 * the oldest frame (once the queue depth is reached) is popped out. On gate
 * close the queue is dropped entirely — the squelch-crash tail never exits
 * it. ste_reset() clears the queue on gate close so stale audio from the
 * previous transmission never replays on the next open edge. */

typedef struct ste ste_t;

/* delay_ms == 0 disables STE: push/pop become a pass-through. */
int  ste_create(ste_t **out, int delay_ms);

/* Push one 160-sample (20 ms) frame while the gate is open. If the queue
 * has reached its configured depth, pops the oldest frame into *out and
 * returns true (that frame should be forwarded). Otherwise returns false
 * (still filling the delay window — forward nothing yet). */
bool ste_push(ste_t *s, const int16_t *in, int16_t *out);

/* Call on gate close: discard all buffered frames. */
void ste_reset(ste_t *s);

void ste_destroy(ste_t *s);
