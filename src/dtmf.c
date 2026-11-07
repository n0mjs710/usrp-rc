#include "dtmf.h"
#include "log.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_RATE 8000.0

/* 205 samples (25.6 ms) is the classic DTMF block: 39 Hz bins, with the closest
 * tones (697/770 Hz) landing near a null of each other's response. Sliding it
 * every 10 ms gives min_tone_ms useful resolution without shortening the window
 * and losing the frequency selectivity. */
#define WINDOW_SAMPLES 205
#define HOP_SAMPLES    80
#define HOP_MS         10
#define WINDOW_MS      26

/* Signal-quality gates. Deliberately permissive: the minimum tone time is the
 * main defense against voice, and these get tuned on the air if they need to. */
#define MIN_TONE_POWER  1.0e-4   /* each tone at least -40 dBFS (amplitude^2) */
#define MAX_TWIST_RATIO 6.3      /* the two tones within 8 dB of each other */
#define MIN_DOMINANCE   4.0      /* 6 dB above the runner-up in the same group */
#define MIN_ENERGY_FRAC 0.40     /* the pair is this share of the window's energy */

/* The two halves of the window must hold similar energy. Windows overlap, so
 * without this a burst counts in every window it merely touches, and min_tone_ms
 * would pass tones much shorter than it says. A ratio of 0.5 means the tone fills
 * at least 3/4 of the window. */
#define MIN_HALF_BALANCE 0.5

/* A valid tone ends after this many consecutive analyses without it, so one bad
 * window on a noisy signal doesn't split a digit in two. */
#define RELEASE_MISSES  2

static const double LOW_HZ[4]  = {697.0, 770.0, 852.0, 941.0};
static const double HIGH_HZ[4] = {1209.0, 1336.0, 1477.0, 1633.0};
static const char   KEYS[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'},
};

/* Instrumentation: what each analysis measured, and the worst of a run of them. */
typedef struct {
    double level;           /* weaker tone, dBFS */
    double frac;            /* the pair's share of the window energy */
    double dom_low, dom_high;   /* dB over the runner-up in the group */
    double twist;           /* low over high, dB */
    double h2_low, h2_high; /* 2nd harmonic over its fundamental, dB */
    double balance;         /* weaker half over stronger half of the window */
} metrics_t;

typedef struct {
    int    n;
    double level_min, level_max, frac_min, dom_low_min, dom_high_min;
    double twist_min, twist_max, h2_low_max, h2_high_max, balance_min;
} run_t;

static double db(double ratio)
{
    return ratio > 0.0 ? 10.0 * log10(ratio) : -99.0;
}

static void run_start(run_t *r, const metrics_t *m)
{
    r->n = 1;
    r->level_min = r->level_max = m->level;
    r->frac_min = m->frac;
    r->dom_low_min = m->dom_low;
    r->dom_high_min = m->dom_high;
    r->twist_min = r->twist_max = m->twist;
    r->h2_low_max = m->h2_low;
    r->h2_high_max = m->h2_high;
    r->balance_min = m->balance;
}

static void run_add(run_t *r, const metrics_t *m)
{
    r->n++;
    r->level_min = fmin(r->level_min, m->level);
    r->level_max = fmax(r->level_max, m->level);
    r->frac_min = fmin(r->frac_min, m->frac);
    r->dom_low_min = fmin(r->dom_low_min, m->dom_low);
    r->dom_high_min = fmin(r->dom_high_min, m->dom_high);
    r->twist_min = fmin(r->twist_min, m->twist);
    r->twist_max = fmax(r->twist_max, m->twist);
    r->h2_low_max = fmax(r->h2_low_max, m->h2_low);
    r->h2_high_max = fmax(r->h2_high_max, m->h2_high);
    r->balance_min = fmin(r->balance_min, m->balance);
}

static void run_log(const run_t *r, char key, const char *what, int ms)
{
    LOGI("dtmf-metrics: '%c' %s %d ms, %d windows: level %.1f..%.1f dBFS, frac min %.2f, "
         "dom min L %.1f H %.1f dB, twist %+.1f..%+.1f dB, h2 max L %.1f H %.1f dB, "
         "balance min %.2f\n",
         key, what, ms, r->n, r->level_min, r->level_max, r->frac_min,
         r->dom_low_min, r->dom_high_min, r->twist_min, r->twist_max,
         r->h2_low_max, r->h2_high_max, r->balance_min);
}

struct dtmf_decoder {
    float samples[WINDOW_SAMPLES];   /* ring buffer */
    int   head;
    int   filled;
    int   hop_left;

    float low_coef[4];
    float high_coef[4];
    float low_h2_coef[4];
    float high_h2_coef[4];
    run_t run;              /* the current candidate or active tone */

    int   hits_required;

    char  candidate;
    int   candidate_hits;

    char  active;
    int   active_hits;
    int   misses;
};

static double goertzel_power(const float *x, float coef)
{
    float s1 = 0.0f, s2 = 0.0f;
    for (int i = 0; i < WINDOW_SAMPLES; i++) {
        float s0 = x[i] + coef * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    double p = (double)s1 * s1 + (double)s2 * s2 - (double)coef * s1 * s2;
    /* Normalize so a full-window sine of amplitude A reads as A^2. */
    double norm = 2.0 / WINDOW_SAMPLES;
    return p * norm * norm;
}

/* Index of the strongest of four powers; *ratio is how far it clears the rest. */
static int strongest(const double p[4], double *ratio)
{
    int best = 0;
    for (int i = 1; i < 4; i++)
        if (p[i] > p[best])
            best = i;
    double second = 0.0;
    for (int i = 0; i < 4; i++)
        if (i != best && p[i] > second)
            second = p[i];
    *ratio = second > 0.0 ? p[best] / second : 1.0e9;
    return best;
}

/* The DTMF key in the current window, or 0. */
static char analyze(const dtmf_decoder_t *d, metrics_t *m)
{
    float x[WINDOW_SAMPLES];
    double first = 0.0, second = 0.0;
    for (int i = 0; i < WINDOW_SAMPLES; i++) {
        x[i] = d->samples[(d->head + i) % WINDOW_SAMPLES];
        if (i < WINDOW_SAMPLES / 2)
            first += (double)x[i] * x[i];
        else
            second += (double)x[i] * x[i];
    }
    double energy = (first + second) / WINDOW_SAMPLES;
    if (energy <= 0.0)
        return 0;
    double lesser = first < second ? first : second;
    double greater = first < second ? second : first;
    if (lesser < MIN_HALF_BALANCE * greater)
        return 0;

    double low[4], high[4];
    for (int i = 0; i < 4; i++) {
        low[i]  = goertzel_power(x, d->low_coef[i]);
        high[i] = goertzel_power(x, d->high_coef[i]);
    }

    double low_ratio, high_ratio;
    int r = strongest(low, &low_ratio);
    int c = strongest(high, &high_ratio);
    double pl = low[r], ph = high[c];

    if (pl < MIN_TONE_POWER || ph < MIN_TONE_POWER)
        return 0;
    if (low_ratio < MIN_DOMINANCE || high_ratio < MIN_DOMINANCE)
        return 0;
    if (pl / ph > MAX_TWIST_RATIO || ph / pl > MAX_TWIST_RATIO)
        return 0;
    /* A sine of amplitude A carries A^2/2 of energy. */
    if ((pl + ph) / 2.0 < MIN_ENERGY_FRAC * energy)
        return 0;

    m->level    = db(pl < ph ? pl : ph);
    m->frac     = (pl + ph) / 2.0 / energy;
    m->dom_low  = db(low_ratio);
    m->dom_high = db(high_ratio);
    m->twist    = db(pl / ph);
    m->h2_low   = db(goertzel_power(x, d->low_h2_coef[r]) / pl);
    m->h2_high  = db(goertzel_power(x, d->high_h2_coef[c]) / ph);
    m->balance  = lesser / greater;

    return KEYS[r][c];
}

static int active_ms(const dtmf_decoder_t *d)
{
    return WINDOW_MS + (d->active_hits - 1) * HOP_MS;
}

/* Runs one analysis of the window through the debounce. Returns events written. */
static int step(dtmf_decoder_t *d, char key, const metrics_t *m, dtmf_event_t *out)
{
    int n = 0;

    if (d->active) {
        if (key == d->active) {
            d->active_hits++;
            d->misses = 0;
            run_add(&d->run, m);
            return 0;
        }
        /* A different key can't be the same press; anything else may be noise. */
        if (++d->misses < RELEASE_MISSES && !key)
            return 0;

        out[n].type  = DTMF_TONE_OFF;
        out[n].digit = d->active;
        out[n].ms    = active_ms(d);
        run_log(&d->run, d->active, "valid", out[n].ms);
        n++;
        d->active = 0;
        d->misses = 0;
        d->candidate = 0;
        d->candidate_hits = 0;
    }

    if (!key) {
        if (d->candidate && d->candidate_hits >= 2)
            run_log(&d->run, d->candidate, "short", WINDOW_MS + (d->candidate_hits - 1) * HOP_MS);
        if (d->candidate)
            LOGD("dtmf: ignored '%c' after %d window(s), under the minimum tone time\n",
                 d->candidate, d->candidate_hits);
        d->candidate = 0;
        d->candidate_hits = 0;
        return n;
    }

    if (key == d->candidate) {
        d->candidate_hits++;
        run_add(&d->run, m);
    } else {
        d->candidate = key;
        d->candidate_hits = 1;
        run_start(&d->run, m);
    }

    if (d->candidate_hits >= d->hits_required) {
        d->active      = key;
        d->active_hits = d->candidate_hits;
        d->misses      = 0;
        d->candidate   = 0;
        d->candidate_hits = 0;
        out[n].type  = DTMF_TONE_ON;
        out[n].digit = key;
        out[n].ms    = active_ms(d);
        n++;
    }
    return n;
}

int dtmf_decoder_push(dtmf_decoder_t *d, const int16_t frame[160],
                      dtmf_event_t out[DTMF_MAX_EVENTS])
{
    int n = 0;
    for (int i = 0; i < 160; i++) {
        d->samples[d->head] = (float)frame[i] / 32768.0f;
        d->head = (d->head + 1) % WINDOW_SAMPLES;
        if (d->filled < WINDOW_SAMPLES)
            d->filled++;
        if (--d->hop_left > 0)
            continue;
        d->hop_left = HOP_SAMPLES;
        if (d->filled == WINDOW_SAMPLES && n <= DTMF_MAX_EVENTS - 2) {
            metrics_t m;
            char key = analyze(d, &m);
            n += step(d, key, &m, &out[n]);
        }
    }
    return n;
}

int dtmf_decoder_active_ms(const dtmf_decoder_t *d)
{
    return d->active ? active_ms(d) : 0;
}

void dtmf_decoder_reset(dtmf_decoder_t *d)
{
    if (d->active)
        run_log(&d->run, d->active, "cut by unkey", active_ms(d));
    memset(d->samples, 0, sizeof(d->samples));
    d->head = 0;
    d->filled = 0;
    d->hop_left = HOP_SAMPLES;
    d->candidate = 0;
    d->candidate_hits = 0;
    d->active = 0;
    d->active_hits = 0;
    d->misses = 0;
}

int dtmf_decoder_create(dtmf_decoder_t **out, int min_tone_ms)
{
    dtmf_decoder_t *d = calloc(1, sizeof(*d));
    if (!d)
        return -1;

    for (int i = 0; i < 4; i++) {
        d->low_coef[i]  = (float)(2.0 * cos(2.0 * M_PI * LOW_HZ[i] / SAMPLE_RATE));
        d->high_coef[i] = (float)(2.0 * cos(2.0 * M_PI * HIGH_HZ[i] / SAMPLE_RATE));
        d->low_h2_coef[i]  = (float)(2.0 * cos(2.0 * M_PI * 2.0 * LOW_HZ[i] / SAMPLE_RATE));
        d->high_h2_coef[i] = (float)(2.0 * cos(2.0 * M_PI * 2.0 * HIGH_HZ[i] / SAMPLE_RATE));
    }

    /* The first hit proves one window's worth of tone; each later one adds a hop. */
    int extra = min_tone_ms - WINDOW_MS;
    d->hits_required = extra > 0 ? 1 + (extra + HOP_MS - 1) / HOP_MS : 1;

    dtmf_decoder_reset(d);
    *out = d;
    return 0;
}

void dtmf_decoder_destroy(dtmf_decoder_t *d)
{
    free(d);
}
