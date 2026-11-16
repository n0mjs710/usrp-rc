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
    dtmf_decoder_t *decoder;   /* NULL for a session that gets keys, not audio */
    char label[48];            /* starts each log line: which receiver */

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

/* Every command, in matching order: whole codes win over the code + argument forms.
 *
 *   Programming: a stub for now. The config file is the programming interface;
 *                a DTMF unlock would go here if a use for one turns up.
 *   Control op:  fixed codes behind dtmf.control_op_prefix. Both tiers share the one
 *                table: a control op entry's code is the prefix + its number.
 *   User:        each code is its own setting.
 */
#define CODES  CONFIG_DTMF_CODES
#define NCODES CONFIG_NDTMF_CODES

/* Valid until the next call. */
static const char *code_of(const command_session_t *s, size_t i)
{
    static char buf[CFG_DTMF_CODE_STR];
    return config_dtmf_code(&s->cfg->dtmf, i, buf);
}

static bool is_link_command(command_type_t t)
{
    return t == COMMAND_LINK_ON || t == COMMAND_LINK_OFF || t == COMMAND_LINK_STATUS;
}

/* Does what was entered match CODES[i]? `whole` asks for a complete command; without
 * it, whether more keys could still complete one. */
static bool matches(const command_session_t *s, size_t i, bool whole)
{
    const char *code = code_of(s, i);
    size_t len = strlen(code), n = (size_t)s->ndigits;
    if (!len || strncmp(s->digits, code, n < len ? n : len) != 0)
        return false;
    if (n < len)
        return !whole;
    switch (CODES[i].kind) {
    case CODE_WHOLE:   return n == len;
    case CODE_CAPTURE: return n > len || !whole;
    case CODE_KEYS: {
        size_t want = len + (size_t)CODES[i].nkeys;
        if (n > want || strspn(s->digits + len, "0123456789") != n - len)
            return false;
        return n == want || !whole;
    }
    }
    return false;
}

/* Why the keys after a code + keys can't run, or NULL if they can. */
static const char *bad_keys(const command_session_t *s, command_type_t type, const char *arg)
{
    const config_events_t *ev = &s->cfg->events;
    if (type == COMMAND_READ)
        return s->cfg->dtmf.read_messages[atoi(arg)][0] ? NULL : "no message";
    if (type == COMMAND_WINDOW) {
        if (arg[2] != '0' && arg[2] != '1')
            return "not start 1 or stop 0:";
        int id = (arg[0] - '0') * 10 + (arg[1] - '0');
        for (int i = 0; i < s->cfg->nschedule; i++)
            if (s->cfg->schedule[i].id == id)
                return NULL;
        return "no window for";
    }
    if (type != COMMAND_FORCE_ID)
        return NULL;
    switch (*arg) {
    case '0': return ev->n_initial_ids ? NULL : "no initial ID";
    case '1': return NULL;   /* impolite_id is required */
    case '2': return ev->anxious_id[0] ? NULL : "no anxious ID";
    case '3': return NULL;   /* with no mandatory ID, impolite_id stands in */
    default:  return "no ID type";
    }
}

static bool evaluate(command_session_t *s, command_t *out)
{
    if (s->ndigits == 0)
        return false;
    if (s->overflowed) {
        LOGI("%s: '%s...': ABANDONED more than %d digits\n", s->label, s->digits, COMMAND_MAX_DIGITS);
        clear_digits(s);
        return false;
    }

    for (size_t i = 0; i < NCODES; i++) {
        if (!matches(s, i, true))
            continue;
        const char *arg = s->digits + strlen(code_of(s, i));
        out->type = CODES[i].type;
        out->control_op = CODES[i].op_code != NULL;
        snprintf(out->digits, sizeof(out->digits), "%s", arg);
        if (is_link_command(out->type) && !s->cfg->link.enabled) {
            LOGI("%s: '%s': ERROR command = %s, no link service\n", s->label, s->digits, CODES[i].name);
            out->type = COMMAND_NO_LINK;
        } else if (CODES[i].kind == CODE_KEYS) {
            const char *why = bad_keys(s, out->type, arg);
            if (why) {
                LOGI("%s: '%s': ERROR %s %s\n", s->label, s->digits, why, arg);
                clear_digits(s);
                return false;
            }
            LOGI("%s: '%s': ACCEPTED command = %s %s\n", s->label, s->digits, CODES[i].name, arg);
        } else if (CODES[i].kind == CODE_CAPTURE) {
            LOGI("%s: '%s': ACCEPTED command = %s %s\n", s->label, s->digits, CODES[i].name, arg);
        } else {
            LOGI("%s: '%s': ACCEPTED command = %s\n", s->label, s->digits, CODES[i].name);
        }
        clear_digits(s);
        return true;
    }

    LOGI("%s: '%s': ERROR no command matched\n", s->label, s->digits);
    clear_digits(s);
    return false;
}

/* Could more digits still make a command? */
static bool live(const command_session_t *s)
{
    for (size_t i = 0; i < NCODES; i++)
        if (matches(s, i, false))
            return true;
    return false;
}

/* Past a capture code, every key counts to the unkey. */
static bool at_capture_code(const command_session_t *s)
{
    for (size_t i = 0; i < NCODES; i++)
        if (CODES[i].kind == CODE_CAPTURE && code_of(s, i)[0] && strcmp(s->digits, code_of(s, i)) == 0)
            return true;
    return false;
}

static void on_digit(command_session_t *s, char digit, bool *terminated)
{
    if (s->ndigits > 0 && s->now_ms - s->last_digit_ms > s->cfg->dtmf.interdigit_ms) {
        LOGI("%s: '%s': ABANDONED inter-digit timer exceeded\n", s->label, s->digits);
        clear_digits(s);
    }

    if (!s->capturing && digit == '#') {
        /* ACC's cancel: forget everything entered so far this transmission. */
        if (s->ndigits > 0)
            LOGI("%s: '%s#': ABANDONED user terminated\n", s->label, s->digits);
        clear_digits(s);
        return;
    }
    if (!s->capturing && digit == 'D') {
        /* ACC's forced enter, for when a carrier won't drop on its own. */
        *terminated = true;
        return;
    }

    if (s->ndigits == 0)
        LOGI("%s: sequence started with '%c'\n", s->label, digit);

    s->last_digit_ms = s->now_ms;
    if (s->ndigits < COMMAND_MAX_DIGITS) {
        s->digits[s->ndigits++] = digit;
        s->digits[s->ndigits] = '\0';
    } else {
        s->overflowed = true;
    }

    if (!s->capturing && !s->overflowed && at_capture_code(s)) {
        s->capturing = true;
        LOGD("%s: '%s': every key to the unkey counts\n", s->label, s->digits);
    }

    if (!s->overflowed && !live(s)) {
        LOGI("%s: '%s': ABANDONED no command starts this way\n", s->label, s->digits);
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
            LOGD("%s: digit '%c'\n", s->label, events[i].digit);
            if (!s->muted) {
                s->muted = true;
                s->cover_pos = 0;
                LOGD("dtmf: muting repeat audio\n");
            }
            s->tone_rejected = false;
            on_digit(s, events[i].digit, &terminated);
        } else {
            LOGD("%s: digit '%c' released after %d ms\n", s->label, events[i].digit, events[i].ms);
            s->tone_rejected = false;
        }
    }

    int held = dtmf_decoder_active_ms(s->decoder);
    if (held > 0 || n > 0)
        s->last_tone_ms = s->now_ms;

    if (held > s->cfg->dtmf.max_tone_ms && !s->tone_rejected) {
        LOGI("%s: '%s': ABANDONED tone timer exceeded\n", s->label, s->digits);
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
    if (s->decoder)
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
            LOGI("%s: '%s': ABANDONED end-of-transmission timer exceeded\n", s->label, s->digits);
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
        LOGI("%s: '%s': ABANDONED %s\n", s->label, s->digits, why);
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
    case '*': return "STAR";
    case '#': return "POUND";
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
    snprintf(s->label, sizeof(s->label), "dtmf");

    if (dtmf_decoder_create(&s->decoder, cfg->dtmf.min_tone_ms) != 0) {
        free(s);
        return -1;
    }

    render_cover_tone(s);
    *out = s;
    return 0;
}

int command_session_create_keys(command_session_t **out, const config_t *cfg, const char *label)
{
    command_session_t *s = calloc(1, sizeof(*s));
    if (!s)
        return -1;
    s->cfg = cfg;
    snprintf(s->label, sizeof(s->label), "%s", label);
    *out = s;
    return 0;
}

/* No clock runs: a typed entry has no tones to time and no voice to false on. */
bool command_session_key(command_session_t *s, char key, command_t *out)
{
    bool terminated = false;
    on_digit(s, key, &terminated);
    return terminated && evaluate(s, out);
}


void command_session_destroy(command_session_t *s)
{
    if (!s)
        return;
    if (s->decoder)
        dtmf_decoder_destroy(s->decoder);
    free(s->cover);
    free(s);
}
