#include "message.h"
#include "tone.h"
#include "morse.h"
#include "sbuf.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *const F_NUMBERS[] = {
    "F_OH", "F_ONE", "F_TWO", "F_THREE", "F_FOUR", "F_FIVE", "F_SIX", "F_SEVEN", "F_EIGHT", "F_NINE",
    "F_TEN", "F_ELEVEN", "F_TWELVE", "F_THIRTEEN", "F_FOURTEEN", "F_FIFTEEN", "F_SIXTEEN",
    "F_SEVENTEEN", "F_EIGHTEEN", "F_NINETEEN",
};
static const char *const F_TENS[] = {"F_TWENTY", "F_THIRTY", "F_FORTY", "F_FIFTY"};

static const char *const NUMBERS[] = {
    "ZERO", "ONE", "TWO", "THREE", "FOUR", "FIVE", "SIX", "SEVEN", "EIGHT", "NINE",
    "TEN", "ELEVEN", "TWELVE", "THIRTEEN", "FOURTEEN", "FIFTEEN", "SIXTEEN",
    "SEVENTEEN", "EIGHTEEN", "NINETEEN",
};
static const char *const TENS[] = {
    "TWENTY", "THIRTY", "FORTY", "FIFTY", "SIXTY", "SEVENTY", "EIGHTY", "NINETY",
};
static const char *const DAYS[] = {
    "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY",
};
static const char *const MONTHS[] = {
    "JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE", "JULY", "AUGUST",
    "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER",
};

static message_stats_fn s_stats_fn;
static void            *s_stats_arg;

void message_set_stats_source(message_stats_fn fn, void *arg)
{
    s_stats_fn  = fn;
    s_stats_arg = arg;
}

/* 1-99, with no zero: the part of a number between its hundreds and thousands. */
static int small_number(int v, const char **out)
{
    int n = 0;
    if (v < 20) {
        out[n++] = NUMBERS[v];
    } else {
        out[n++] = TENS[v / 10 - 2];
        if (v % 10)
            out[n++] = NUMBERS[v % 10];
    }
    return n;
}

/* Clamped at 99,999, which a week of key-ups or minutes isn't going to reach. */
static int number_clips(long v, const char **out)
{
    if (v < 0)
        v = 0;
    if (v > 99999)
        v = 99999;
    if (v == 0) {
        out[0] = NUMBERS[0];
        return 1;
    }
    int n = 0;
    if (v >= 1000) {
        n += small_number((int)(v / 1000), out + n);
        out[n++] = "THOUSAND";
        v %= 1000;
    }
    if (v >= 100) {
        out[n++] = NUMBERS[v / 100];
        out[n++] = "HUNDRED";
        v %= 100;
    }
    if (v > 0)
        n += small_number((int)v, out + n);
    return n;
}

int message_var_clips(voice_var_t var, const struct tm *when, const message_stats_t *stats,
                      const char *out[MESSAGE_VAR_MAX_CLIPS])
{
    int hour = when->tm_hour, min = when->tm_min, n = 0;

    switch (var) {
    case VOICE_VAR_TIME:
        out[n++] = F_NUMBERS[hour % 12 ? hour % 12 : 12];
        if (min == 0) {
            out[n++] = "F_OCLOCK";
        } else if (min < 10) {
            out[n++] = "F_OH";
            out[n++] = F_NUMBERS[min];
        } else if (min < 20) {
            out[n++] = F_NUMBERS[min];
        } else {
            out[n++] = F_TENS[min / 10 - 2];
            if (min % 10)
                out[n++] = F_NUMBERS[min % 10];
        }
        break;
    case VOICE_VAR_AMPM:
        out[n++] = hour < 12 ? "F_A_M" : "F_P_M";
        break;
    case VOICE_VAR_GREETING:
        out[n++] = hour < 12 ? "F_MORNING" : hour < 18 ? "F_AFTERNOON" : "F_EVENING";
        break;
    case VOICE_VAR_DAY:
        out[n++] = DAYS[when->tm_wday];
        break;
    case VOICE_VAR_DATE:
        out[n++] = MONTHS[when->tm_mon];
        n += number_clips(when->tm_mday, out + n);
        break;
    case VOICE_VAR_YEAR:
        n = number_clips(when->tm_year + 1900L, out);
        break;
    case VOICE_VAR_KEYUPS_DAY:
        n = number_clips(stats->keyups_day, out);
        break;
    case VOICE_VAR_KEYUPS_WEEK:
        n = number_clips(stats->keyups_week, out);
        break;
    case VOICE_VAR_MINUTES_DAY:
        n = number_clips(stats->minutes_day, out);
        break;
    case VOICE_VAR_MINUTES_WEEK:
        n = number_clips(stats->minutes_week, out);
        break;
    case VOICE_VAR_NONE:
        break;
    }
    return n;
}

/* Every clip a variable could ask for has to exist, whatever it happens to say now. */
static int validate_var(const config_message_t *m, voice_var_t var, vocab_cache_t *vocab)
{
    /* Stepping everything together reaches every day, month, hour and minute, and every
     * word a number can use: past 1,099 they only repeat. */
    for (int i = 0; i < 1100; i++) {
        struct tm when = {.tm_hour = i % 24, .tm_min = i % 60, .tm_wday = i % 7,
                          .tm_mon = i % 12, .tm_mday = i % 31 + 1, .tm_year = i - 1900};
        message_stats_t stats = {i, i, i, i};
        const char *clips[MESSAGE_VAR_MAX_CLIPS];
        int n = message_var_clips(var, &when, &stats, clips);
        for (int c = 0; c < n; c++) {
            size_t len;
            if (!vocab_get(vocab, clips[c], &len)) {
                LOGE("config: messages.%s: a variable needs the voice clip '%s'\n", m->name, clips[c]);
                return 1;
            }
        }
    }
    return 0;
}

int message_has_voice(const config_t *cfg, const char *name)
{
    const config_message_t *m = config_find_message(cfg, name);
    if (!m)
        return 0;
    for (int i = 0; i < m->nelements; i++)
        if (m->elements[i].type == ELEM_VOICE)
            return 1;
    return 0;
}

int message_needs_padding(const config_t *cfg, const char *name)
{
    const config_message_t *m = config_find_message(cfg, name);
    if (!m)
        return 0;
    for (int i = 0; i < m->nelements; i++)
        if (m->elements[i].type == ELEM_CW || m->elements[i].type == ELEM_VOICE)
            return 1;
    return 0;
}

static int check_length(const config_t *cfg, vocab_cache_t *vocab, const config_message_t *m,
                        const char *where)
{
    rendered_audio_t r = message_render(cfg, vocab, m->name, -1.0);
    int bad = r.n > (size_t)MESSAGE_MAX_SECONDS * 8000;
    if (bad)
        LOGE("config: line %d: messages.%s: %zu s long%s%s, the limit is %d s\n",
             m->line, m->name, (r.n + 7999) / 8000, where ? " during " : "", where ? where : "",
             MESSAGE_MAX_SECONDS);
    free(r.samples);
    return bad;
}

int message_validate(const config_t *cfg, vocab_cache_t *vocab)
{
    int errors = 0;
    for (int i = 0; i < cfg->nmessages; i++) {
        const config_message_t *m = &cfg->messages[i];
        int before = errors;
        for (int j = 0; j < m->nelements; j++) {
            const config_elem_t *e = &m->elements[j];
            if (e->type == ELEM_CW) {
                for (const char *p = e->cw_text; *p; p++) {
                    if (!morse_has_code(*p)) {
                        LOGE("config: line %d: messages.%s: no Morse code for '%c'\n", m->line, m->name, *p);
                        errors++;
                    }
                }
            } else if (e->type == ELEM_VOICE) {
                for (int w = 0; w < e->n_voice_words; w++) {
                    size_t n;
                    if (e->voice_words[w].var != VOICE_VAR_NONE) {
                        errors += validate_var(m, e->voice_words[w].var, vocab);
                    } else if (!e->voice_words[w].is_gap && !vocab_get(vocab, e->voice_words[w].clip, &n)) {
                        LOGE("config: line %d: messages.%s: no voice clip named '%s'\n",
                             m->line, m->name, e->voice_words[w].clip);
                        errors++;
                    }
                }
            }
        }
        if (errors == before)
            errors += check_length(cfg, vocab, m, NULL);
    }
    return errors;
}

int message_check_schedule(const config_t *cfg, vocab_cache_t *vocab)
{
    int errors = 0;
    config_t *open = malloc(sizeof(*open));
    if (!open)
        return 0;
    for (int i = 0; i < cfg->nschedule; i++) {
        const config_schedule_t *e = &cfg->schedule[i];
        bool audio = false;
        for (int j = 0; j < e->nsets; j++)
            audio |= e->sets[j].offset >= offsetof(config_t, audio)
                  && e->sets[j].offset < offsetof(config_t, audio) + sizeof(cfg->audio);
        if (!audio)
            continue;
        *open = *cfg;
        for (int j = 0; j < e->nsets; j++)
            config_apply_override(open, &e->sets[j]);
        char where[64];
        snprintf(where, sizeof(where), "schedule.%s", e->name);
        for (int m = 0; m < open->nmessages; m++)
            errors += check_length(open, vocab, &open->messages[m], where);
    }
    free(open);
    return errors;
}

static void append_clip(const config_t *cfg, vocab_cache_t *vocab, const char *clip, sbuf_t *buf)
{
    size_t n;
    const int16_t *cached = vocab_get(vocab, clip, &n);
    if (!cached) {
        LOGE("message: voice clip '%s' not found\n", clip);
        return;
    }
    sbuf_append_scaled(buf, cached, n, cfg->audio.voice_level);
}

rendered_audio_t message_render_clips(const config_t *cfg, vocab_cache_t *vocab,
                                      const char *const *clips, int nclips)
{
    sbuf_t buf = {0};
    for (int i = 0; i < nclips; i++)
        append_clip(cfg, vocab, clips[i], &buf);
    sbuf_scale(&buf, cfg->audio.master_gain);

    rendered_audio_t result = {.samples = buf.data, .n = buf.len};
    return result;
}

rendered_audio_t message_render(const config_t *cfg, vocab_cache_t *vocab,
                                const char *name, double morse_level_override)
{
    rendered_audio_t result = {0};
    const config_message_t *m = config_find_message(cfg, name);
    if (!m) {
        LOGE("message: '%s' not found\n", name ? name : "(null)");
        return result;
    }
    if (m->nelements == 0) {
        LOGE("message: '%s' has no elements\n", name);
        return result;
    }

    double morse_level = (morse_level_override >= 0.0)
                        ? morse_level_override : cfg->audio.morse_level;

    sbuf_t buf = {0};

    for (int i = 0; i < m->nelements; i++) {
        const config_elem_t *e = &m->elements[i];

        switch (e->type) {
        case ELEM_CW: {
            if (!e->cw_text[0]) {
                LOGE("message: '%s' CW element has no text\n", name);
                break;
            }
            size_t n;
            int16_t *samples = morse_render(e->cw_text, cfg->audio.morse_wpm,
                                            (double)cfg->audio.morse_pitch,
                                            morse_level, &n);
            if (samples) {
                sbuf_append(&buf, samples, n);
                free(samples);
            }
            break;
        }
        case ELEM_VOICE: {
            if (e->n_voice_words == 0) {
                LOGE("message: '%s' VOICE element has no words\n", name);
                break;
            }
            for (int w = 0; w < e->n_voice_words; w++) {
                const config_voice_word_t *word = &e->voice_words[w];
                if (word->is_gap) {
                    int ms = word->gap_ms >= 0 ? word->gap_ms : cfg->audio.voice_gap_ms;
                    sbuf_append_silence(&buf, (size_t)((double)ms / 1000.0 * TONE_SAMPLE_RATE));
                    continue;
                }
                if (word->var != VOICE_VAR_NONE) {
                    time_t t = time(NULL);
                    struct tm when;
                    localtime_r(&t, &when);
                    message_stats_t stats = {0};
                    if (s_stats_fn)
                        s_stats_fn(s_stats_arg, &stats);
                    const char *clips[MESSAGE_VAR_MAX_CLIPS];
                    int n = message_var_clips(word->var, &when, &stats, clips);
                    for (int c = 0; c < n; c++)
                        append_clip(cfg, vocab, clips[c], &buf);
                    continue;
                }
                append_clip(cfg, vocab, word->clip, &buf);
            }
            break;
        }
        case ELEM_TONE: {
            size_t n;
            double amp = e->amp >= 0.0 ? e->amp : cfg->audio.tone_level;
            int16_t *samples = tone_render(e->freq1, e->freq2, e->ms, amp, &n);
            if (samples) {
                sbuf_append(&buf, samples, n);
                free(samples);
            }
            break;
        }
        }
    }

    sbuf_scale(&buf, cfg->audio.master_gain);

    result.samples = buf.data;
    result.n       = buf.len;
    return result;
}
