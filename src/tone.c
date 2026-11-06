#include "tone.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define ATTACK_S 0.005   /* 5 ms raised-cosine attack/decay, click-free keying */

int16_t *tone_render(double freq1, double freq2, int ms, double amp, size_t *out_n)
{
    if (ms <= 0) {
        *out_n = 0;
        return NULL;
    }

    size_t n = (size_t)((double)ms / 1000.0 * TONE_SAMPLE_RATE);
    if (n == 0) {
        *out_n = 0;
        return NULL;
    }

    double *wave = calloc(n, sizeof(double));
    if (!wave) {
        *out_n = 0;
        return NULL;
    }

    int active = 0;
    if (freq1 > 0) active++;
    if (freq2 > 0) active++;

    if (active > 0) {
        double dt = 1.0 / TONE_SAMPLE_RATE;
        for (size_t i = 0; i < n; i++) {
            double t = (double)i * dt;
            double s = 0.0;
            if (freq1 > 0) s += sin(2.0 * M_PI * freq1 * t);
            if (freq2 > 0) s += sin(2.0 * M_PI * freq2 * t);
            wave[i] = s / (double)active;
        }

        /* Raised-cosine (Hann) attack/decay ramp. */
        size_t ramp_n = (size_t)(ATTACK_S * TONE_SAMPLE_RATE);
        if (ramp_n > n / 2)
            ramp_n = n / 2;
        for (size_t i = 0; i < ramp_n; i++) {
            double w = 0.5 * (1.0 - cos(M_PI * (double)i / (double)ramp_n));
            wave[i]         *= w;
            wave[n - 1 - i] *= w;
        }
    }

    int16_t *out = malloc(n * sizeof(int16_t));
    if (!out) {
        free(wave);
        *out_n = 0;
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        double s = wave[i] * amp;
        if (s > 1.0)  s = 1.0;
        if (s < -1.0) s = -1.0;
        out[i] = (int16_t)(s * 32767.0);
    }

    free(wave);
    *out_n = n;
    return out;
}

static bool dtmf_freqs(char key, double *row, double *col)
{
    static const char KEYS[] = "123A456B789C*0#D";
    static const double ROWS[] = {697, 770, 852, 941};
    static const double COLS[] = {1209, 1336, 1477, 1633};
    const char *k = key ? strchr(KEYS, key) : NULL;
    if (!k)
        return false;
    *row = ROWS[(k - KEYS) / 4];
    *col = COLS[(k - KEYS) % 4];
    return true;
}

int16_t *tone_render_dtmf(const char *keys, int lead_ms, int tone_ms, int gap_ms,
                          int trail_ms, double amp, size_t *out_n)
{
    size_t per_ms = TONE_SAMPLE_RATE / 1000;
    size_t nkeys = strlen(keys);
    size_t cap = per_ms * ((size_t)lead_ms + nkeys * (size_t)(tone_ms + gap_ms) + (size_t)trail_ms);
    int16_t *out = cap ? calloc(cap, sizeof(int16_t)) : NULL;
    *out_n = 0;
    if (!out)
        return NULL;

    size_t n = per_ms * (size_t)lead_ms;
    for (size_t i = 0; i < nkeys; i++) {
        double row, col;
        if (!dtmf_freqs(keys[i], &row, &col))
            continue;
        size_t len;
        int16_t *t = tone_render(row, col, tone_ms, amp, &len);
        if (t) {
            memcpy(out + n, t, len * sizeof(int16_t));
            free(t);
            n += len;
        }
        if (i + 1 < nkeys)
            n += per_ms * (size_t)gap_ms;
    }
    *out_n = n + per_ms * (size_t)trail_ms;
    return out;
}
