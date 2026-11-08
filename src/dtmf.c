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

/* Signal-quality gates. Energy share and dominance reject voice: on the air, real keys
 * read >= 0.84 and >= 14.5 dB, and voice, clipped voice included, rarely passed 0.60 and
 * 12 dB. */
#define MIN_TONE_POWER  1.0e-4   /* each tone at least -40 dBFS (amplitude^2) */
#define MAX_TWIST_RATIO 6.3      /* the two tones within 8 dB of each other */
#define MIN_DOMINANCE   10.0     /* 10 dB above the runner-up in the same group */
#define MIN_ENERGY_FRAC 0.65     /* the pair is this share of the window's energy */

/* The two halves of the window must hold similar energy. Windows overlap, so
 * without this a burst counts in every window it merely touches, and min_tone_ms
 * would pass tones much shorter than it says. A ratio of 0.5 means the tone fills
 * at least 3/4 of the window. */
#define MIN_HALF_BALANCE 0.5

/* The bins are only about +-20 Hz wide, so each tone is also measured 1% either side
 * of nominal and the strongest of the three counts. That keeps the standard's +-1.5%
 * accepted at the energy share above, and +-3.5% is still rejected. */
#define SIDE_BIN 0.01

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

/* Instrumentation (dtmf-metrics branch only). Every window that passes the old, loose
 * gates is measured, and each run of one key is logged when it ends, with how many of
 * its windows passed the real gates. That shows how close voice gets to them. */
#define LOOSE_DOMINANCE  4.0     /* 6 dB */
#define LOOSE_ENERGY_FRAC 0.40

typedef struct {
    double level;               /* weaker tone, dBFS */
    double frac;                /* the pair's share of the window energy */
    double dom_low, dom_high;   /* dB over the runner-up in the group */
    double twist;               /* low over high, dB */
    double h2_low, h2_high;     /* 2nd harmonic over its fundamental, dB */
    double balance;             /* weaker half over stronger half of the window */
} metrics_t;

typedef struct {
    char   key;
    int    n, strict;
    double level_min, level_max, frac_min, frac_max, dom_low_min, dom_high_min;
    double twist_min, twist_max, h2_low_max, h2_high_max, balance_min;
} run_t;

static double db(double ratio)
{
    return ratio > 0.0 ? 10.0 * log10(ratio) : -99.0;
}

static void run_log(const run_t *r)
{
    if (r->n < 2)
        return;
    LOGI("dtmf-metrics: '%c' %d ms, %d windows, %d strict: level %.1f..%.1f dBFS, "
         "frac %.2f..%.2f, dom min L %.1f H %.1f dB, twist %+.1f..%+.1f dB, "
         "h2 max L %.1f H %.1f dB, balance min %.2f\n",
         r->key, WINDOW_MS + (r->n - 1) * HOP_MS, r->n, r->strict,
         r->level_min, r->level_max, r->frac_min, r->frac_max,
         r->dom_low_min, r->dom_high_min, r->twist_min, r->twist_max,
         r->h2_low_max, r->h2_high_max, r->balance_min);
}

/* One window's loose key (0 = none) and whether it also passed the real gates. */
static void run_feed(run_t *r, char loose, bool strict, const metrics_t *m)
{
    if (loose != r->key) {
        run_log(r);
        memset(r, 0, sizeof(*r));
        r->key = loose;
        if (!loose)
            return;
        r->level_min = r->level_max = m->level;
        r->frac_min = r->frac_max = m->frac;
        r->dom_low_min = m->dom_low;
        r->dom_high_min = m->dom_high;
        r->twist_min = r->twist_max = m->twist;
        r->h2_low_max = m->h2_low;
        r->h2_high_max = m->h2_high;
        r->balance_min = m->balance;
    } else if (!loose) {
        return;
    }
    r->n++;
    r->strict += strict;
    r->level_min = fmin(r->level_min, m->level);
    r->level_max = fmax(r->level_max, m->level);
    r->frac_min = fmin(r->frac_min, m->frac);
    r->frac_max = fmax(r->frac_max, m->frac);
    r->dom_low_min = fmin(r->dom_low_min, m->dom_low);
    r->dom_high_min = fmin(r->dom_high_min, m->dom_high);
    r->twist_min = fmin(r->twist_min, m->twist);
    r->twist_max = fmax(r->twist_max, m->twist);
    r->h2_low_max = fmax(r->h2_low_max, m->h2_low);
    r->h2_high_max = fmax(r->h2_high_max, m->h2_high);
    r->balance_min = fmin(r->balance_min, m->balance);
}

struct dtmf_decoder {
    float samples[WINDOW_SAMPLES];   /* ring buffer */
    int   head;
    int   filled;
    int   hop_left;

    float low_coef[4][3];    /* nominal, then 1% below and above */
    float high_coef[4][3];
    float low_h2_coef[4];
    float high_h2_coef[4];
    run_t run;

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

static double tone_power(const float *x, const float coef[3])
{
    double p = goertzel_power(x, coef[0]);
    for (int i = 1; i < 3; i++)
        p = fmax(p, goertzel_power(x, coef[i]));
    return p;
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
static char analyze(const dtmf_decoder_t *d, char *loose, metrics_t *m)
{
    *loose = 0;
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
        low[i]  = tone_power(x, d->low_coef[i]);
        high[i] = tone_power(x, d->high_coef[i]);
    }

    double low_ratio, high_ratio;
    int r = strongest(low, &low_ratio);
    int c = strongest(high, &high_ratio);
    double pl = low[r], ph = high[c];

    if (pl < MIN_TONE_POWER || ph < MIN_TONE_POWER)
        return 0;
    if (low_ratio < LOOSE_DOMINANCE || high_ratio < LOOSE_DOMINANCE)
        return 0;
    if (pl / ph > MAX_TWIST_RATIO || ph / pl > MAX_TWIST_RATIO)
        return 0;
    /* A sine of amplitude A carries A^2/2 of energy. */
    double frac = (pl + ph) / 2.0 / energy;
    if (frac < LOOSE_ENERGY_FRAC)
        return 0;

    *loose = KEYS[r][c];
    m->level    = db(pl < ph ? pl : ph);
    m->frac     = frac;
    m->dom_low  = db(low_ratio);
    m->dom_high = db(high_ratio);
    m->twist    = db(pl / ph);
    m->h2_low   = db(goertzel_power(x, d->low_h2_coef[r]) / pl);
    m->h2_high  = db(goertzel_power(x, d->high_h2_coef[c]) / ph);
    m->balance  = lesser / greater;

    if (low_ratio < MIN_DOMINANCE || high_ratio < MIN_DOMINANCE)
        return 0;
    if (frac < MIN_ENERGY_FRAC)
        return 0;
    return KEYS[r][c];
}

static int active_ms(const dtmf_decoder_t *d)
{
    return WINDOW_MS + (d->active_hits - 1) * HOP_MS;
}

/* Runs one analysis of the window through the debounce. Returns events written. */
static int step(dtmf_decoder_t *d, char key, dtmf_event_t *out)
{
    int n = 0;

    if (d->active) {
        if (key == d->active) {
            d->active_hits++;
            d->misses = 0;
            return 0;
        }
        /* A different key can't be the same press; anything else may be noise. */
        if (++d->misses < RELEASE_MISSES && !key)
            return 0;

        out[n].type  = DTMF_TONE_OFF;
        out[n].digit = d->active;
        out[n].ms    = active_ms(d);
        n++;
        d->active = 0;
        d->misses = 0;
        d->candidate = 0;
        d->candidate_hits = 0;
    }

    if (!key) {
        if (d->candidate)
            LOGD("dtmf: ignored '%c' after %d window(s), under the minimum tone time\n",
                 d->candidate, d->candidate_hits);
        d->candidate = 0;
        d->candidate_hits = 0;
        return n;
    }

    if (key == d->candidate) {
        d->candidate_hits++;
    } else {
        d->candidate = key;
        d->candidate_hits = 1;
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
            char loose;
            metrics_t m;
            char key = analyze(d, &loose, &m);
            run_feed(&d->run, loose, key != 0, &m);
            n += step(d, key, &out[n]);
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
    run_log(&d->run);
    memset(&d->run, 0, sizeof(d->run));
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

    static const double SCALE[3] = {1.0, 1.0 - SIDE_BIN, 1.0 + SIDE_BIN};
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 3; j++) {
            d->low_coef[i][j]  = (float)(2.0 * cos(2.0 * M_PI * LOW_HZ[i] * SCALE[j] / SAMPLE_RATE));
            d->high_coef[i][j] = (float)(2.0 * cos(2.0 * M_PI * HIGH_HZ[i] * SCALE[j] / SAMPLE_RATE));
        }
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
