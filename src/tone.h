#pragma once

#include <stdint.h>
#include <stddef.h>

#define TONE_SAMPLE_RATE 8000

/* Render a single/dual-frequency tone burst (or silence if both freqs <= 0)
 * to newly malloc'd int16 samples at TONE_SAMPLE_RATE. *out_n receives the
 * sample count. Returns NULL on allocation failure or zero-length request.
 * Caller must free() the result. */
int16_t *tone_render(double freq1, double freq2, int ms, double amp, size_t *out_n);
