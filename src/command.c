#include "command.h"
#include "dtmf.h"
#include "tone.h"
#include "sbuf.h"
#include "log.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_MS      20
#define FRAME_SAMPLES 160

struct command_session {
    const config_t *cfg;
    dtmf_decoder_t *decoder;

    int  now_ms;            /* time into this transmission */

    char digits[COMMAND_MAX_DIGITS + 1];
    int  ndigits;
    bool overflowed;
    bool capturing;         /* past a pad test or regen code: every key counts, to unkey */
    bool tone_rejected;     /* the current tone outlasted max_tone_ms */
    int  last_digit_ms;     /* when the last digit started */
    int  last_tone_ms;      /* when a tone was last heard; drives the mute hang */

    bool muted;

    int16_t *cover;
    size_t   cover_len;
    size_t   cover_period;
    size_t   cover_pos;
};

static void clear_digits(command_session_t *s)
{
    s->ndigits = 0;
    s->digits[0] = '\0';
    s->overflowed = false;
    s->capturing = false;
}

/* Match what was entered against the commands, highest tier first:
 *
 *   Programming: a stub for now. The config file is the programming interface;
 *                a DTMF unlock would go here if a use for one turns up.
 *   Control op:  no commands yet. They'll sit behind a definable prefix.
 *   User:        the pad test, time, usage figures, echo test, link on, link off, link status,
 *                DTMF regen, and repeater down/up (its code plus 0 or 1; no code by
 *                default, as anyone could use it until the control operator tier exists).
 */
static bool evaluate(command_session_t *s, command_t *out)
{
    if (s->ndigits == 0)
        return false;
    if (s->overflowed) {
        LOGI("dtmf: '%s...': ABANDONED more than %d digits\n", s->digits, COMMAND_MAX_DIGITS);
        clear_digits(s);
        return false;
    }

    const config_dtmf_t *d = &s->cfg->dtmf;

    /* Whole codes win over the pad test's prefix, which matches anything after it. */
    static const struct { command_type_t type; size_t offset; const char *name; } CODES[] = {
        {COMMAND_TIME,     offsetof(config_dtmf_t, time_code),     "time"},
        {COMMAND_STATS,    offsetof(config_dtmf_t, stats_code),    "usage"},
        {COMMAND_ECHO,     offsetof(config_dtmf_t, echo_code),     "echo test"},
        {COMMAND_LINK_ON,  offsetof(config_dtmf_t, link_on_code),  "link on"},
        {COMMAND_LINK_OFF, offsetof(config_dtmf_t, link_off_code), "link off"},
        {COMMAND_LINK_STATUS, offsetof(config_dtmf_t, link_status_code), "link status"},
    };
    for (size_t i = 0; i < sizeof(CODES) / sizeof(CODES[0]); i++) {
        const char *code = (const char *)d + CODES[i].offset;
        if (!code[0] || strcmp(s->digits, code) != 0)
            continue;
        bool needs_link = CODES[i].type == COMMAND_LINK_ON || CODES[i].type == COMMAND_LINK_OFF
                       || CODES[i].type == COMMAND_LINK_STATUS;
        if (needs_link && !s->cfg->link.enabled) {
            LOGI("dtmf: '%s': ERROR command = %s, no link service\n", s->digits, CODES[i].name);
            out->type = COMMAND_NO_LINK;
            out->digits[0] = '\0';
            clear_digits(s);
            return true;
        }
        LOGI("dtmf: '%s': ACCEPTED command = %s\n", s->digits, CODES[i].name);
        out->type = CODES[i].type;
        out->digits[0] = '\0';
        clear_digits(s);
        return true;
    }

    size_t rlen = strlen(d->repeater_code);
    if (rlen && (size_t)s->ndigits == rlen + 1 && strncmp(s->digits, d->repeater_code, rlen) == 0
        && (s->digits[rlen] == '0' || s->digits[rlen] == '1')) {
        out->type = COMMAND_REPEATER;
        out->digits[0] = s->digits[rlen];
        out->digits[1] = '\0';
        LOGI("dtmf: '%s': ACCEPTED command = repeater %s\n", s->digits, out->digits[0] == '1' ? "up" : "down");
        clear_digits(s);
        return true;
    }

    size_t glen = strlen(d->regen_code);
    if (glen && strncmp(s->digits, d->regen_code, glen) == 0 && s->digits[glen]) {
        out->type = COMMAND_REGEN;
        snprintf(out->digits, sizeof(out->digits), "%s", s->digits + glen);
        LOGI("dtmf: '%s': ACCEPTED command = regen %s\n", s->digits, out->digits);
        clear_digits(s);
        return true;
    }

    const char *prefix = d->pad_test_prefix;
    size_t plen = strlen(prefix);

    if (plen && strncmp(s->digits, prefix, plen) == 0 && s->digits[plen]) {
        out->type = COMMAND_PAD_TEST;
        snprintf(out->digits, sizeof(out->digits), "%s", s->digits + plen);
        LOGI("dtmf: '%s': ACCEPTED command = DTMF test + %s\n", s->digits, out->digits);
        clear_digits(s);
        return true;
    }

    LOGI("dtmf: '%s': ERROR no command matched\n", s->digits);
    clear_digits(s);
    return false;
}

/* Could more digits still make a command? The pad test and regen take anything after
 * their codes. */
static bool live(const command_session_t *s)
{
    const config_dtmf_t *d = &s->cfg->dtmf;
    const char *codes[] = {d->time_code, d->stats_code, d->echo_code, d->link_on_code,
                           d->link_off_code, d->link_status_code, d->repeater_code,
                           d->regen_code, d->pad_test_prefix};
    size_t n = (size_t)s->ndigits;
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        size_t len = strlen(codes[i]);
        if (!len)
            continue;
        if (strncmp(s->digits, codes[i], n < len ? n : len) != 0)
            continue;
        if (n <= len)
            return true;
        if (codes[i] == d->repeater_code && n == len + 1
            && (s->digits[len] == '0' || s->digits[len] == '1'))
            return true;
        if (codes[i] == d->regen_code || codes[i] == d->pad_test_prefix)
            return true;
    }
    return false;
}

static void on_digit(command_session_t *s, char digit, bool *terminated)
{
    if (s->ndigits > 0 && s->now_ms - s->last_digit_ms > s->cfg->dtmf.interdigit_ms) {
        LOGI("dtmf: '%s': ABANDONED inter-digit timer exceeded\n", s->digits);
        clear_digits(s);
    }

    if (!s->capturing && digit == '#') {
        /* ACC's cancel: forget everything entered so far this transmission. */
        if (s->ndigits > 0)
            LOGI("dtmf: '%s#': ABANDONED user terminated\n", s->digits);
        clear_digits(s);
        return;
    }
    if (!s->capturing && digit == 'D') {
        /* ACC's forced enter, for when a carrier won't drop on its own. */
        *terminated = true;
        return;
    }

    if (s->ndigits == 0)
        LOGI("dtmf: sequence started with '%c'\n", digit);

    s->last_digit_ms = s->now_ms;
    if (s->ndigits < COMMAND_MAX_DIGITS) {
        s->digits[s->ndigits++] = digit;
        s->digits[s->ndigits] = '\0';
    } else {
        s->overflowed = true;
    }

    /* The pad test and regen read every key after their code, # and D included,
     * so only the unkey ends them. */
    const config_dtmf_t *d = &s->cfg->dtmf;
    if (!s->capturing && !s->overflowed
        && ((d->pad_test_prefix[0] && strcmp(s->digits, d->pad_test_prefix) == 0)
            || (d->regen_code[0] && strcmp(s->digits, d->regen_code) == 0))) {
        s->capturing = true;
        LOGD("dtmf: '%s': every key to the unkey counts\n", s->digits);
    }

    if (!s->overflowed && !live(s)) {
        LOGI("dtmf: '%s': ABANDONED no command starts this way\n", s->digits);
        clear_digits(s);
    }
}

bool command_session_frame(command_session_t *s, const int16_t frame[160], command_t *out)
{
    s->now_ms += FRAME_MS;

    dtmf_event_t events[DTMF_MAX_EVENTS];
    int n = dtmf_decoder_push(s->decoder, frame, events);
    bool terminated = false;

    for (int i = 0; i < n; i++) {
        if (events[i].type == DTMF_TONE_ON) {
            LOGD("dtmf: digit '%c'\n", events[i].digit);
            if (!s->muted) {
                s->muted = true;
                s->cover_pos = 0;
                LOGD("dtmf: muting repeat audio\n");
            }
            s->tone_rejected = false;
            on_digit(s, events[i].digit, &terminated);
        } else {
            LOGD("dtmf: digit '%c' released after %d ms\n", events[i].digit, events[i].ms);
            s->tone_rejected = false;
        }
    }

    int held = dtmf_decoder_active_ms(s->decoder);
    if (held > 0 || n > 0)
        s->last_tone_ms = s->now_ms;

    if (held > s->cfg->dtmf.max_tone_ms && !s->tone_rejected) {
        LOGI("dtmf: '%s': ABANDONED tone timer exceeded\n", s->digits);
        s->tone_rejected = true;
        clear_digits(s);
    }

    if (s->muted && held == 0 && s->now_ms - s->last_tone_ms >= s->cfg->dtmf.mute_hang_ms) {
        s->muted = false;
        LOGD("dtmf: unmuting repeat audio\n");
    }

    return terminated && evaluate(s, out);
}

static void reset_for_next_tx(command_session_t *s)
{
    dtmf_decoder_reset(s->decoder);
    clear_digits(s);
    s->now_ms = 0;
    s->last_digit_ms = 0;
    s->last_tone_ms = 0;
    s->tone_rejected = false;
    s->muted = false;
    s->cover_pos = 0;
}

bool command_session_end_of_tx(command_session_t *s, command_t *out)
{
    bool have = false;

    if (s->ndigits > 0) {
        if (s->now_ms - s->last_digit_ms > s->cfg->dtmf.end_of_tx_ms) {
            /* ACC's defense against voice falsing: a real command ends with the
             * transmission, it isn't followed by a speech. */
            LOGI("dtmf: '%s': ABANDONED end-of-transmission timer exceeded\n", s->digits);
            clear_digits(s);
        } else {
            have = evaluate(s, out);
        }
    }
    reset_for_next_tx(s);
    return have;
}

void command_session_end_of_tx_discard(command_session_t *s, const char *why)
{
    if (s->ndigits > 0)
        LOGI("dtmf: '%s': ABANDONED %s\n", s->digits, why);
    clear_digits(s);
    reset_for_next_tx(s);
}

bool command_session_muted(const command_session_t *s)
{
    return s->muted;
}

void command_session_fill_mute(command_session_t *s, int16_t frame[160])
{
    for (int i = 0; i < FRAME_SAMPLES; i++) {
        frame[i] = s->cover_pos < s->cover_len ? s->cover[s->cover_pos] : 0;
        s->cover_pos++;
        if (s->cover_pos >= s->cover_period)
            s->cover_pos = 0;
    }
}

const char *command_digit_clip(char digit)
{
    static const char *const NUMBERS[] = {
        "ZERO", "ONE", "TWO", "THREE", "FOUR", "FIVE", "SIX", "SEVEN", "EIGHT", "NINE",
    };
    if (digit >= '0' && digit <= '9')
        return NUMBERS[digit - '0'];
    switch (digit) {
    case 'A': return "A";
    case 'B': return "B";
    case 'C': return "C";
    case 'D': return "D";
    case '*': return "SIERRA";
    case '#': return "P";
    default:  return NULL;
    }
}

/* Rendered once, at startup: this plays continuously during a mute, on the audio path,
 * so its levels don't follow a schedule window. */
static void render_cover_tone(command_session_t *s)
{
    const config_t *cfg = s->cfg;
    s->cover = tone_render((double)cfg->dtmf.cover_tone_hz, 0.0, cfg->dtmf.cover_tone_on_ms,
                          cfg->audio.tone_level, &s->cover_len);
    if (s->cover) {
        sbuf_t b = {.data = s->cover, .len = s->cover_len, .cap = s->cover_len};
        sbuf_scale(&b, cfg->audio.master_gain);
    }
    s->cover_period = s->cover_len + (size_t)cfg->dtmf.cover_tone_off_ms * (TONE_SAMPLE_RATE / 1000);
}

int command_session_create(command_session_t **out, const config_t *cfg)
{
    command_session_t *s = calloc(1, sizeof(*s));
    if (!s)
        return -1;
    s->cfg = cfg;

    if (dtmf_decoder_create(&s->decoder, cfg->dtmf.min_tone_ms) != 0) {
        free(s);
        return -1;
    }

    render_cover_tone(s);
    *out = s;
    return 0;
}


void command_session_destroy(command_session_t *s)
{
    if (!s)
        return;
    dtmf_decoder_destroy(s->decoder);
    free(s->cover);
    free(s);
}
