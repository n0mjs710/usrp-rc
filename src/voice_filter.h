#pragma once

#include <stdint.h>
#include <stddef.h>

/* Band-limits voice clip samples in place (8 kHz mono s16) to match the
 * bandwidth MMDVMHost's own RX audio chain already imposes on real
 * repeated voice (FMControl.cpp: 3rd-order Chebyshev Type 1, 0.2 dB
 * ripple, 300-2700 Hz, fs=8000) -- see voice_filter.c for the exact
 * coefficients and why they're a literal replica rather than an
 * independently-designed filter. Without this, our synthesized clips
 * carry high-frequency content (sibilants especially) that live RX audio
 * never has, which MMDVMHost's TX pre-emphasis then boosts disproportionately. */
void voice_filter_apply(int16_t *samples, size_t n);
