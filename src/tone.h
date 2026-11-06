#pragma once

#include <stdint.h>
#include <stddef.h>

#define TONE_SAMPLE_RATE 8000

/* Render a single/dual-frequency tone burst (or silence if both freqs <= 0)
 * to newly malloc'd int16 samples at TONE_SAMPLE_RATE. *out_n receives the
 * sample count. Returns NULL on allocation failure or zero-length request.
 * Caller must free() the result. */
int16_t *tone_render(double freq1, double freq2, int ms, double amp, size_t *out_n);

/* DTMF keys (0-9, A-D, *, #) as tone_ms tones gap_ms apart, between lead_ms and
 * trail_ms of silence. Both tones at the same level, peaking at amp together. */
int16_t *tone_render_dtmf(const char *keys, int lead_ms, int tone_ms, int gap_ms,
                          int trail_ms, double amp, size_t *out_n);
