#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define CFG_MAX_ELEMENTS     32
#define CFG_MAX_ID_ROTATION  8
#define CFG_MAX_VOICE_WORDS  32
#define CFG_STR              128

/* Short identifier strings: vocab clip names, message names, and message-
 * name references (ct_link_on_message, initial_ids, etc.). None of these are free
 * text -- they're names you make up and refer back to -- so 32 bytes (31
 * usable characters) is deliberately tight to keep config_t's size down;
 * the longest real clip name in vocab_8k/ is 18 characters. Longer values
 * are a config error. Use CFG_STR instead for fields that hold actual
 * content (e.g. CW text) rather than a name. */
#define CFG_NAME_STR         32

typedef enum {
    ELEM_CW,
    ELEM_VOICE,
    ELEM_TONE,
} elem_type_t;

/* One word in a voice element's space-separated clip list: either a real
 * clip name, or a pause ("_" = cfg->audio.voice_gap_ms default, "_NNN" =
 * NNN ms explicit — recognized only when '_' is followed solely by digits
 * or nothing, so clip names like "_TEEN" are unaffected). */
typedef enum {
    VOICE_VAR_NONE = 0,
    VOICE_VAR_TIME,       /* {time}: the current time, in 12-hour form */
    VOICE_VAR_AMPM,       /* {ampm} */
    VOICE_VAR_GREETING,   /* {greeting}: morning, afternoon or evening */
    VOICE_VAR_DAY,        /* {day}: day of the week */
    VOICE_VAR_DATE,       /* {date}: month and day */
    VOICE_VAR_YEAR,       /* {year}: as a whole number, "two thousand twenty six" */
    VOICE_VAR_KEYUPS_DAY,    /* {keyups_day}: key-ups in the last 24 hours */
    VOICE_VAR_KEYUPS_WEEK,   /* {keyups_week}: ... in the last 7 days */
    VOICE_VAR_MINUTES_DAY,   /* {minutes_day}: minutes of traffic in the last 24 hours */
    VOICE_VAR_MINUTES_WEEK,  /* {minutes_week}: ... in the last 7 days */
} voice_var_t;

typedef struct {
    bool        is_gap;
    int         gap_ms;             /* valid when is_gap; -1 = use configured default */
    voice_var_t var;                /* filled in when the message plays */
    char        clip[CFG_NAME_STR]; /* valid when !is_gap and no var */
} config_voice_word_t;

typedef struct {
    elem_type_t type;
    /* cw */
    char     cw_text[CFG_STR];
    /* voice: one or more words, played back to back.
     * `voice: THISIS _ W ONE` in the config splits into 4 words here. */
    config_voice_word_t voice_words[CFG_MAX_VOICE_WORDS];
    int      n_voice_words;
    /* tone */
    double   freq1;
    double   freq2;
    int      ms;
    double   amp;   /* < 0 = use cfg->audio.tone_level */
} config_elem_t;

typedef struct {
    char           name[CFG_NAME_STR];
    config_elem_t  elements[CFG_MAX_ELEMENTS];
    int            nelements;
    int            line;   /* where it was defined, for error messages */
} config_message_t;

typedef enum {
    LINK_CODEC_PCM  = 0,
    LINK_CODEC_OPUS = 1,
} link_codec_t;

typedef struct {
    char     local_address[64];
    uint16_t local_port;
    char     rpt_address[64];
    uint16_t rpt_port;
} config_mmdvm_t;

typedef struct {
    bool         enabled;   /* false = standalone local repeater; no link socket at all */
    char         remote_host[256];
    uint16_t     remote_port;
    uint16_t     local_port;
    char         bind_address[64];
    link_codec_t codec;
    int          opus_bitrate;
    int          opus_frame_ms;
    bool         start_on;   /* link state at startup; DTMF commands change it after that */
} config_link_t;

typedef struct {
    double   master_gain;   /* final multiplier on all rendered controller audio */
    int      morse_wpm;
    int      morse_pitch;
    double   morse_level;
    double   impolite_morse_level;
    double   voice_level;
    double   tone_level;     /* tones without their own @LEVEL */
    int      voice_gap_ms;   /* default pause length for a bare "_" word */
    int      ste_delay_ms;
    int      lead_trim_ms;   /* mute this much mmdvm-RX audio at the start of each transmission */
    int      pre_message_ms;
    int      post_message_ms;
} config_audio_t;

typedef struct {
    int    tail_every;        /* play the tail message every this many tails; 0 = off */
    double tail_min_interval; /* but at least this many seconds apart; 0 = no limit */
    double hang;
    double ct_delay;
    double kerchunk;
    double timeout;        /* local; 0 = off */
    double link_timeout;   /* link; resets each time the link unkeys; 0 = off */
    double id_interval;
    double id_anxious;
} config_timers_t;

/* Enough for ACC's longest command prefix (7 digits). */
#define CFG_DTMF_PREFIX_STR 8

typedef struct {
    bool enabled;
    int  min_tone_ms;     /* a tone must hold this long to count as a digit */
    int  max_tone_ms;     /* a tone held longer than this discards the sequence */
    int  interdigit_ms;   /* a longer gap between digits discards what came before */
    int  end_of_tx_ms;    /* a longer gap from last digit to unkey discards the sequence */
    int  mute_hang_ms;    /* repeat audio stays muted this long after the last tone */
    int  beep_hz;
    int  beep_on_ms;
    int  beep_off_ms;
    char pad_test_prefix[CFG_DTMF_PREFIX_STR];   /* empty = pad test off */
    char time_code[CFG_DTMF_PREFIX_STR];         /* empty = command off, likewise below */
    char stats_code[CFG_DTMF_PREFIX_STR];
    char link_on_code[CFG_DTMF_PREFIX_STR];
    char link_off_code[CFG_DTMF_PREFIX_STR];
    char link_status_code[CFG_DTMF_PREFIX_STR];
    char repeater_code[CFG_DTMF_PREFIX_STR];     /* + 0 = down, + 1 = up; empty by default */
    char regen_code[CFG_DTMF_PREFIX_STR];        /* + keys to send; empty by default */
    int    regen_tone_ms;
    int    regen_gap_ms;
    int    regen_lead_ms;   /* silence after keying the link, before the first tone */
    double regen_level;
} config_dtmf_t;

typedef struct {
    char startup_message[CFG_NAME_STR];
    char initial_ids[CFG_MAX_ID_ROTATION][CFG_NAME_STR];
    int  n_initial_ids;
    char mandatory_ids[CFG_MAX_ID_ROTATION][CFG_NAME_STR];
    int  n_mandatory_ids;
    char anxious_id[CFG_NAME_STR];
    char impolite_id[CFG_NAME_STR];
    char ct_link_on_message[CFG_NAME_STR];
    char ct_link_message[CFG_NAME_STR];
    char ct_link_off_message[CFG_NAME_STR];   /* local traffic while the link is off */
    char tail_messages[CFG_MAX_ID_ROTATION][CFG_NAME_STR];
    int  n_tail_messages;
    char time_message[CFG_NAME_STR];
    char stats_message[CFG_NAME_STR];
    char link_on_message[CFG_NAME_STR];
    char link_off_message[CFG_NAME_STR];
    char link_none_message[CFG_NAME_STR];      /* link commands when there's no link */
    char repeater_up_message[CFG_NAME_STR];
    char repeater_down_message[CFG_NAME_STR];
    char timeout_message[CFG_NAME_STR];
    char timeout_cancel_message[CFG_NAME_STR];
} config_events_t;

typedef enum {
    ACCESS_COR = 0,
    ACCESS_COR_CTCSS = 1,
} access_mode_t;

typedef struct {
    char level[16];   /* "error", "warn", "info", or "debug" */
} config_log_t;

typedef struct {
    char file[256];   /* where usage survives a restart; empty = memory only */
} config_stats_t;

typedef struct {
    char file[256];   /* where repeater down survives a restart; empty = not kept */
} config_state_t;

#define CFG_MAX_SCHEDULE      32
#define CFG_MAX_SCHEDULE_SETS 16
#define CFG_MAX_DAY_RULES     8

/* One setting changed for the length of a schedule window. Offsets are into
 * config_t. A list keeps its text, for += and -=; anything else holds the value
 * already parsed, ready to copy in. */
typedef enum { OVERRIDE_SET, OVERRIDE_ADD, OVERRIDE_REMOVE } override_op_t;

typedef struct {
    size_t offset;
    size_t size;
    size_t count_offset;   /* for a list of names; 0 = not a list */
    override_op_t op;      /* add and remove only apply to a list */
    bool   names;          /* value is message names, as text */
    char   value[CFG_STR];
} config_override_t;

/* A day that isn't simply a day of the week. A rule that a given month or year has
 * no match for, like a 5th Tuesday, just doesn't match. */
typedef enum {
    DAY_RULE_NTH_WEEKDAY,   /* "2nd-tue": a = 1-5, b = tm_wday */
    DAY_RULE_MONTH_DAY,     /* "15th": a = day of the month */
    DAY_RULE_LAST_DAY,      /* "last-day" */
    DAY_RULE_DATE,          /* "dec-8": a = tm_mon, b = day of the month; every year */
} day_rule_type_t;

typedef struct {
    day_rule_type_t type;
    int             a, b;
} config_day_rule_t;

typedef struct {
    char              name[CFG_NAME_STR];
    unsigned          days;        /* bit 0 = Sunday, as struct tm's tm_wday */
    config_day_rule_t rules[CFG_MAX_DAY_RULES];
    int               nrules;
    int               start_min;   /* minutes past midnight */
    int               end_min;     /* exclusive; below start_min = runs past midnight */
    config_override_t sets[CFG_MAX_SCHEDULE_SETS];
    int               nsets;
} config_schedule_t;

#define CFG_CALLSIGN_STR 16

typedef struct {
    char             callsign[CFG_CALLSIGN_STR];  /* letters and digits; {call} in messages */
    config_mmdvm_t   mmdvm;
    config_link_t    link;
    config_audio_t   audio;
    config_timers_t  timers;
    config_events_t  events;
    config_dtmf_t    dtmf;
    config_stats_t   stats;
    config_state_t   state;
    access_mode_t    access_mode;
    config_log_t     log;

    config_message_t *messages;   /* everything defined, until config_prune_messages() */
    int               nmessages;
    int               messages_cap;

    config_schedule_t schedule[CFG_MAX_SCHEDULE];
    int               nschedule;
} config_t;

void config_defaults(config_t *cfg);

/* Load and validate, along with the tone catalog. Every problem found is
 * logged, not just the first. Returns 0 only if the file is entirely valid:
 * unknown settings, wrong types, out-of-range values and dangling message
 * references are all errors. On failure cfg is still populated as far as
 * parsing got, so a caller can carry on checking and report further problems,
 * but must not run with it. */
int  config_load(config_t *cfg, const char *path);

/* Drop every message that no [events] setting or schedule window names. Run after
 * message_validate(), so unused messages are still checked. */
void config_prune_messages(config_t *cfg);

void config_free(config_t *cfg);

/* Apply one schedule setting. */
void config_apply_override(config_t *cfg, const config_override_t *o);

/* Look up a message by name; returns NULL if not found or name is empty. */
const config_message_t *config_find_message(const config_t *cfg, const char *name);
