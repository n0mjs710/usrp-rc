#include "message.h"
#include "tone.h"
#include "morse.h"
#include "sbuf.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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
    case VOICE_VAR_EXTERNAL:   /* read from its file when it plays: see ext_read() */
    case VOICE_VAR_MESSAGE:    /* the message it names: see render_into() */
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

/* PLAY is any message on the air, CHECK the length check at load, which leaves
 * producer variables out: their files are data, not configuration. */
typedef enum { RENDER_PLAY, RENDER_CHECK } render_mode_t;

static rendered_audio_t render(const config_t *cfg, vocab_cache_t *vocab, const char *name,
                               double morse_level_override, render_mode_t mode);

static int check_length(const config_t *cfg, vocab_cache_t *vocab, const config_message_t *m,
                        const char *where)
{
    rendered_audio_t r = render(cfg, vocab, m->name, -1.0, RENDER_CHECK);
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
                    if (e->voice_words[w].var == VOICE_VAR_EXTERNAL
                        || e->voice_words[w].var == VOICE_VAR_MESSAGE) {
                        continue;   /* a producer checks its own words; config checks references */
                    } else if (e->voice_words[w].var != VOICE_VAR_NONE) {
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

/* ── external producers ────────────────────────────────────────────────
 * {producer.field} is the file <external.dir>/<producer>/<field>: clip names, numbers
 * and _NNN gaps. Fresh means its mtime is within the producer's lifetime; a fresh empty
 * file has nothing to report. A word that isn't a clip or a number drops the whole
 * variable: skipping just that word could change what it means. A stale, missing or
 * dropped variable plays the producer's fallback, if it has one, every time; usrp-rc
 * sets no policy beyond that. */

typedef enum { EXT_FRESH, EXT_STALE, EXT_MISSING, EXT_DROPPED } ext_state_t;

#define EXT_MAX_BYTES 1024

typedef struct {
    ext_state_t state;
    int         n;
    struct {
        int  gap_ms;                /* < -1 = a clip; -1 = the default gap; else ms */
        char clip[CFG_NAME_STR];
    } w[CFG_MAX_VOICE_WORDS];
    char why[96];
} ext_words_t;

static void ext_path(const config_t *cfg, const char *var, char *buf, size_t bufsz)
{
    const char *dot = strchr(var, '.');
    snprintf(buf, bufsz, "%s/%.*s/%s", cfg->external.dir, (int)(dot - var), var, dot + 1);
}

/* Each variable's last state, so a change is logged once, not every time it plays. */
static struct {
    char        var[CFG_NAME_STR];
    ext_state_t state;
    char        why[96];
} s_ext_seen[64];
static int s_ext_nseen;

static void ext_log(const char *var, const ext_words_t *x)
{
    int i = 0;
    while (i < s_ext_nseen && strcmp(s_ext_seen[i].var, var) != 0)
        i++;
    if (i < s_ext_nseen && s_ext_seen[i].state == x->state
        && (x->state != EXT_DROPPED || strcmp(s_ext_seen[i].why, x->why) == 0))
        return;
    if (i == s_ext_nseen && s_ext_nseen < (int)(sizeof(s_ext_seen) / sizeof(s_ext_seen[0])))
        s_ext_nseen++;
    if (i < s_ext_nseen) {
        snprintf(s_ext_seen[i].var, sizeof(s_ext_seen[i].var), "%s", var);
        s_ext_seen[i].state = x->state;
        snprintf(s_ext_seen[i].why, sizeof(s_ext_seen[i].why), "%s", x->why);
    }
    switch (x->state) {
    case EXT_FRESH:   LOGI("external: {%s} FRESH\n", var); break;
    case EXT_STALE:   LOGW("external: {%s} STALE, %s\n", var, x->why); break;
    case EXT_MISSING: LOGW("external: {%s} MISSING, %s\n", var, x->why); break;
    case EXT_DROPPED: LOGW("external: {%s} DROPPED, %s\n", var, x->why); break;
    }
}

/* A number in a producer file: an optional sign, digits, and optionally a point and
 * more digits; "-3.5" is MINUS THREE POINT FIVE. The whole part goes up to 99,999.
 * Returns the clips put in out, 0 if tok is a number too big or too long to say, or
 * -1 if tok isn't a number. */
static int ext_number(const char *tok, const char **out, int room)
{
    const char *p = tok + (tok[0] == '-' || tok[0] == '+');
    size_t whole = strspn(p, "0123456789");
    const char *frac = p + whole;
    if (whole == 0 || (*frac && (frac[0] != '.' || !frac[1] || frac[1 + strspn(frac + 1, "0123456789")])))
        return -1;
    long v = 0;
    for (size_t i = 0; i < whole; i++)
        if ((v = v * 10 + (p[i] - '0')) > 99999)
            return 0;
    const char *num[MESSAGE_VAR_MAX_CLIPS];
    int nn = number_clips(v, num);
    if ((tok[0] == '-') + nn + (int)strlen(frac) > room)   /* frac is POINT and its digits */
        return 0;
    int k = 0;
    if (tok[0] == '-')
        out[k++] = "MINUS";
    for (int i = 0; i < nn; i++)
        out[k++] = num[i];
    if (*frac) {
        out[k++] = "POINT";
        for (const char *d = frac + 1; *d; d++)
            out[k++] = NUMBERS[*d - '0'];
    }
    return k;
}

static ext_state_t ext_read(const config_t *cfg, vocab_cache_t *vocab, const char *var,
                            const config_producer_t *p, ext_words_t *x)
{
    char path[512];
    ext_path(cfg, var, path, sizeof(path));
    x->n = 0;
    x->why[0] = '\0';

    struct stat st;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &st) != 0) {
        x->state = EXT_MISSING;
        snprintf(x->why, sizeof(x->why), "%s", strerror(errno));
    } else if (time(NULL) - st.st_mtime >= p->lifetime_s) {
        x->state = EXT_STALE;
        snprintf(x->why, sizeof(x->why), "%ld s old, lifetime %d s", (long)(time(NULL) - st.st_mtime),
                 p->lifetime_s);
    } else {
        char text[EXT_MAX_BYTES + 1];
        ssize_t got = read(fd, text, sizeof(text));
        x->state = EXT_FRESH;
        if (got < 0) {
            x->state = EXT_DROPPED;
            snprintf(x->why, sizeof(x->why), "can't read: %s", strerror(errno));
        } else if (got > EXT_MAX_BYTES) {
            x->state = EXT_DROPPED;
            snprintf(x->why, sizeof(x->why), "over %d bytes", EXT_MAX_BYTES);
        } else {
            text[got] = '\0';
            char *save = NULL;
            for (char *tok = strtok_r(text, " \t\r\n", &save); tok && x->state == EXT_FRESH;
                 tok = strtok_r(NULL, " \t\r\n", &save)) {
                size_t len;
                const char *num[CFG_MAX_VOICE_WORDS];
                int k;
                if (x->n == CFG_MAX_VOICE_WORDS) {
                    x->state = EXT_DROPPED;
                    snprintf(x->why, sizeof(x->why), "more than %d words", CFG_MAX_VOICE_WORDS);
                } else if (tok[0] == '_' && strspn(tok + 1, "0123456789") == strlen(tok + 1)
                           && strlen(tok) <= 6) {
                    x->w[x->n++].gap_ms = tok[1] ? atoi(tok + 1) : -1;
                } else if ((k = ext_number(tok, num, CFG_MAX_VOICE_WORDS - x->n)) >= 0) {
                    if (k == 0) {
                        x->state = EXT_DROPPED;
                        snprintf(x->why, sizeof(x->why), "can't say '%.40s': over 99,999, or too many words", tok);
                    }
                    for (int i = 0; i < k; i++) {
                        x->w[x->n].gap_ms = -2;
                        strcpy(x->w[x->n++].clip, num[i]);
                    }
                } else if (strlen(tok) >= CFG_NAME_STR || !vocab_get(vocab, tok, &len)) {
                    x->state = EXT_DROPPED;
                    snprintf(x->why, sizeof(x->why), "no clip '%.40s'", tok);
                } else {
                    x->w[x->n].gap_ms = -2;
                    strcpy(x->w[x->n++].clip, tok);
                }
            }
        }
    }
    if (fd >= 0)
        close(fd);
    ext_log(var, x);
    return x->state;
}

bool message_field_current(const config_t *cfg, vocab_cache_t *vocab, const char *var)
{
    const config_producer_t *p = config_find_producer(cfg, var);
    ext_words_t x;
    return p && ext_read(cfg, vocab, var, p, &x) == EXT_FRESH && x.n > 0;
}

static void render_into(const config_t *cfg, vocab_cache_t *vocab, const config_message_t *m,
                        double morse_level, render_mode_t mode, sbuf_t *buf);

static void render_external(const config_t *cfg, vocab_cache_t *vocab, const char *var,
                            double morse_level, sbuf_t *buf)
{
    const config_producer_t *p = config_find_producer(cfg, var);
    if (!p)
        return;
    ext_words_t x;
    if (ext_read(cfg, vocab, var, p, &x) == EXT_FRESH) {
        for (int i = 0; i < x.n; i++) {
            if (x.w[i].gap_ms >= -1) {
                int ms = x.w[i].gap_ms >= 0 ? x.w[i].gap_ms : cfg->audio.voice_gap_ms;
                sbuf_append_silence(buf, (size_t)((double)ms / 1000.0 * TONE_SAMPLE_RATE));
            } else {
                append_clip(cfg, vocab, x.w[i].clip, buf);
            }
        }
        return;
    }
    /* config.c makes sure a fallback holds no variables of its own. */
    const config_message_t *fm = p->fallback[0] ? config_find_message(cfg, p->fallback) : NULL;
    if (fm)
        render_into(cfg, vocab, fm, morse_level, RENDER_PLAY, buf);
}

static void render_into(const config_t *cfg, vocab_cache_t *vocab, const config_message_t *m,
                        double morse_level, render_mode_t mode, sbuf_t *buf)
{
    const char *name = m->name;

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
                sbuf_append(buf, samples, n);
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
                    sbuf_append_silence(buf, (size_t)((double)ms / 1000.0 * TONE_SAMPLE_RATE));
                    continue;
                }
                if (word->var == VOICE_VAR_EXTERNAL) {
                    if (mode != RENDER_CHECK)
                        render_external(cfg, vocab, word->clip, morse_level, buf);
                    continue;
                }
                if (word->var == VOICE_VAR_MESSAGE) {
                    /* config.c allows one level, so this goes no deeper */
                    const config_message_t *ref = config_find_message(cfg, word->clip);
                    if (ref)
                        render_into(cfg, vocab, ref, morse_level, mode, buf);
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
                        append_clip(cfg, vocab, clips[c], buf);
                    continue;
                }
                append_clip(cfg, vocab, word->clip, buf);
            }
            break;
        }
        case ELEM_TONE: {
            size_t n;
            double amp = e->amp >= 0.0 ? e->amp : cfg->audio.tone_level;
            int16_t *samples = tone_render(e->freq1, e->freq2, e->ms, amp, &n);
            if (samples) {
                sbuf_append(buf, samples, n);
                free(samples);
            }
            break;
        }
        }
    }
}

static rendered_audio_t render(const config_t *cfg, vocab_cache_t *vocab, const char *name,
                               double morse_level_override, render_mode_t mode)
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
    render_into(cfg, vocab, m, morse_level, mode, &buf);
    sbuf_scale(&buf, cfg->audio.master_gain);

    result.samples = buf.data;
    result.n       = buf.len;
    return result;
}

rendered_audio_t message_render(const config_t *cfg, vocab_cache_t *vocab,
                                const char *name, double morse_level_override)
{
    return render(cfg, vocab, name, morse_level_override, RENDER_PLAY);
}
