/*
 * Replica of MMDVM-Host's FMControl.cpp RX audio filter: a 3-stage cascaded
 * IIR (Chebyshev Type 1, 0.2 dB ripple, 3rd order, 300-2700 Hz bandpass at
 * fs=8000). Coefficients and structure copied directly from FMControl.cpp /
 * IIRDirectForm1Filter.cpp (Copyright (C) 2015-2020,2023 Jonathan Naylor
 * G4KLX; Copyright (C) 2020 Geoffrey Merck F4FXL KC3FRA, GPLv2+), not
 * independently designed. We apply this to our own generated voice clips so
 * their bandwidth exactly matches what MMDVM-Host's RX chain already imposes
 * on real repeated voice, rather than approximate it with a freshly-designed
 * filter of unverified equivalence.
 *
 * One deliberate deviation: MMDVM-Host's IIRDirectForm1Filter applies a
 * further +2 dB/stage (FILTER_GAIN_DB, +6 dB compounded across 3 stages) on
 * top of these coefficients. Measured against the raw coefficients alone,
 * the passband is already unity gain (0 dB, same 0.2 dB ripple, same
 * 300/2700 Hz corners) -- that extra gain isn't shaping the response, it's
 * MMDVM-Host restoring level for its own downstream chain, which works on
 * raw discriminator audio with headroom to spare. Our voice clips are
 * pre-normalized close to full scale (~0.9), so replicating that gain would
 * just hard-clip them for no benefit to the filter shape. We omit it.
 *
 * Direct-form-1 difference equation per stage (matches IIRDirectForm1Filter
 * exactly):
 *   y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
 */

#include "voice_filter.h"

typedef struct {
    double b0, b1, b2, a1, a2;
} stage_coef_t;

typedef struct {
    double x1, x2, y1, y2;
} stage_state_t;

static const stage_coef_t STAGES[3] = {
    { 0.29495028,  0.0,  -0.29495028, -0.61384624,  -0.057158668 },
    { 1.0,         2.0,   1.0,         0.9946123,    0.6050482   },
    { 1.0,        -2.0,   1.0,        -1.8414584,    0.8804949   },
};

static double stage_apply(const stage_coef_t *c, stage_state_t *s, double x0)
{
    double y0 = c->b0 * x0 + c->b1 * s->x1 + c->b2 * s->x2
              - c->a1 * s->y1 - c->a2 * s->y2;
    s->x2 = s->x1;
    s->y2 = s->y1;
    s->x1 = x0;
    s->y1 = y0;
    return y0;
}

void voice_filter_apply(int16_t *samples, size_t n)
{
    stage_state_t st[3] = {0};

    for (size_t i = 0; i < n; i++) {
        double x = (double)samples[i] / 32768.0;
        for (int s = 0; s < 3; s++)
            x = stage_apply(&STAGES[s], &st[s], x);
        if (x > 1.0)  x = 1.0;
        if (x < -1.0) x = -1.0;
        samples[i] = (int16_t)(x * 32767.0);
    }
}
