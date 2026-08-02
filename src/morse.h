#pragma once

#include <stdint.h>
#include <stddef.h>

#define MORSE_SAMPLE_RATE 8000

/* Render Morse code for `text` to newly malloc'd int16 samples at
 * MORSE_SAMPLE_RATE (PARIS timing). *out_n receives the sample count.
 * Returns NULL (and *out_n = 0) if text has no renderable characters.
 * Caller must free() the result. */
int16_t *morse_render(const char *text, int wpm, double pitch_hz, double level,
                      size_t *out_n);
