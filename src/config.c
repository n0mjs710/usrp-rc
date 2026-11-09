#include "config.h"
#include "log.h"
#include "util.h"
#include "usrp_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stddef.h>
#include <stdarg.h>
#include <errno.h>
#include <ctype.h>
#include <arpa/inet.h>

void config_defaults(config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    strncpy(cfg->mmdvm.local_address, "127.0.0.1", sizeof(cfg->mmdvm.local_address) - 1);
    cfg->mmdvm.local_port = 4810;
    strncpy(cfg->mmdvm.rpt_address, "127.0.0.1", sizeof(cfg->mmdvm.rpt_address) - 1);
    cfg->mmdvm.rpt_port = 3810;

    cfg->link.enabled       = true;
    cfg->link.remote_port  = 34001;
    cfg->link.local_port   = 34002;
    strncpy(cfg->link.bind_address, "0.0.0.0", sizeof(cfg->link.bind_address) - 1);
    cfg->link.codec         = LINK_CODEC_PCM;
    cfg->link.opus_bitrate  = 8000;
    cfg->link.opus_frame_ms = 60;
    cfg->link.start_on      = true;

    cfg->audio.master_gain          = 1.0;
    cfg->audio.morse_wpm            = 20;
    cfg->audio.morse_pitch          = 700;
    cfg->audio.morse_level          = 0.9;
    cfg->audio.impolite_morse_level = 0.3;
    cfg->audio.voice_level          = 0.9;
    cfg->audio.tone_level           = 0.5;
    cfg->audio.voice_gap_ms         = 150;
    cfg->audio.ste_delay_ms         = 0;
    cfg->audio.lead_trim_ms         = 0;
    cfg->audio.keyup_delay_ms       = 0;
    cfg->audio.message_gap_ms       = 0;

    cfg->timers.hang        = 2.5;
    cfg->timers.ct_delay    = 0.5;
    cfg->timers.kerchunk    = 0.5;
    cfg->timers.timeout     = 180.0;
    cfg->timers.link_timeout = 180.0;
    cfg->timers.id_interval = 600.0;
    cfg->timers.id_anxious  = 60.0;
    cfg->timers.tail_every  = 0;
    cfg->timers.tail_min_interval = 0.0;

    /* Off unless asked for: it mutes repeat audio and drops the link, which an
     * existing site shouldn't start doing just by upgrading. */
    cfg->dtmf.enabled       = false;
    cfg->dtmf.min_tone_ms   = 40;
    cfg->dtmf.max_tone_ms   = 5000;
    cfg->dtmf.interdigit_ms = 4000;
    cfg->dtmf.end_of_tx_ms  = 8000;
    cfg->dtmf.mute_hang_ms  = 2000;
    cfg->dtmf.cover_tone_hz     = 440;
    cfg->dtmf.cover_tone_on_ms  = 100;
    cfg->dtmf.cover_tone_off_ms = 900;
    cfg->dtmf.local_cover_tone  = true;
    cfg->dtmf.link_cover_tone   = true;
    cfg->dtmf.regen_tone_ms = 100;
    cfg->dtmf.regen_gap_ms  = 100;
    cfg->dtmf.regen_lead_ms = 200;
    cfg->dtmf.regen_level   = 0.5;
    /* ACC's own defaults where they exist: pad test 5, time is demo prefix 3 + 9,
     * link is prefix 7 + 2 (receive/transmit) or 3 (off). */
    strncpy(cfg->dtmf.pad_test_prefix, "5", sizeof(cfg->dtmf.pad_test_prefix) - 1);
    strncpy(cfg->dtmf.time_code, "39", sizeof(cfg->dtmf.time_code) - 1);
    strncpy(cfg->dtmf.stats_code, "38", sizeof(cfg->dtmf.stats_code) - 1);
    strncpy(cfg->dtmf.echo_code, "30", sizeof(cfg->dtmf.echo_code) - 1);
    strncpy(cfg->dtmf.regen_code, "31", sizeof(cfg->dtmf.regen_code) - 1);
    strncpy(cfg->dtmf.link_on_code, "72", sizeof(cfg->dtmf.link_on_code) - 1);
    strncpy(cfg->dtmf.link_off_code, "73", sizeof(cfg->dtmf.link_off_code) - 1);
    strncpy(cfg->dtmf.link_status_code, "70", sizeof(cfg->dtmf.link_status_code) - 1);

    strncpy(cfg->events.time_message, "time_readback", sizeof(cfg->events.time_message) - 1);
    strncpy(cfg->events.stats_message, "stats_readback", sizeof(cfg->events.stats_message) - 1);
    strncpy(cfg->events.echo_message, "echo_reply", sizeof(cfg->events.echo_message) - 1);
    strncpy(cfg->events.echo_cancel_message, "echo_cancel_reply", sizeof(cfg->events.echo_cancel_message) - 1);
    strncpy(cfg->stats.file, "/var/local/usrp-rc/stats", sizeof(cfg->stats.file) - 1);
    strncpy(cfg->state.file, "/var/local/usrp-rc/state", sizeof(cfg->state.file) - 1);
    strncpy(cfg->events.link_on_message, "link_on_reply", sizeof(cfg->events.link_on_message) - 1);
    strncpy(cfg->events.link_off_message, "link_off_reply", sizeof(cfg->events.link_off_message) - 1);
    strncpy(cfg->events.link_none_message, "link_none_reply", sizeof(cfg->events.link_none_message) - 1);
    strncpy(cfg->events.repeater_up_message, "repeater_up_reply", sizeof(cfg->events.repeater_up_message) - 1);
    strncpy(cfg->events.repeater_down_message, "repeater_down_reply", sizeof(cfg->events.repeater_down_message) - 1);


    strncpy(cfg->log.level, "info", sizeof(cfg->log.level) - 1);
}

/* ── field table ────────────────────────────────────────────────────────
 * Every scalar setting, in one place: where it lives in the file, where it
 * lands in config_t, and what's legal. Reading, type/range checking, and
 * unknown-key detection are all driven from this, so adding a setting here
 * is all it takes for it to be parsed and validated. */

typedef enum {
    FT_STR,
    FT_INT,
    FT_DOUBLE,
    FT_BOOL,
    FT_PORT,
    FT_ENUM,      /* index into choices[] stored as the enum */
    FT_NAME_LIST, /* comma-separated message names; count stored at count_offset */
} field_type_t;

typedef struct {
    const char        *section;   /* "" = top of the file, before any [section] */
    const char        *key;
    field_type_t       type;
    size_t             offset;
    size_t             size;
    double             min, max;  /* FT_INT / FT_DOUBLE only */
    const char *const *choices;   /* FT_ENUM, or restricts an FT_STR; NULL-terminated */
    size_t             count_offset;
} field_t;

static const char *const CODEC_CHOICES[]  = {"pcm", "opus", NULL};       /* link_codec_t order */
static const char *const LEVEL_CHOICES[]  = {"error", "warn", "info", "debug", NULL};

#define MEMBER_SIZE(type, m) sizeof(((type *)0)->m)
#define FIELD(sec, k, ft, m, lo, hi) \
    {sec, k, ft, offsetof(config_t, m), MEMBER_SIZE(config_t, m), lo, hi, NULL, 0}
#define STR(sec, k, m)      FIELD(sec, k, FT_STR, m, 0, 0)
#define PORT(sec, k, m)     FIELD(sec, k, FT_PORT, m, 0, 0)
#define BOOL(sec, k, m)     FIELD(sec, k, FT_BOOL, m, 0, 0)
#define INT(sec, k, m, lo, hi)  FIELD(sec, k, FT_INT, m, lo, hi)
#define DBL(sec, k, m, lo, hi)  FIELD(sec, k, FT_DOUBLE, m, lo, hi)
#define CHOICE(sec, k, ft, m, c) \
    {sec, k, ft, offsetof(config_t, m), MEMBER_SIZE(config_t, m), 0, 0, c, 0}
#define NAMES(k, m, cnt) \
    {"events", k, FT_NAME_LIST, offsetof(config_t, m), MEMBER_SIZE(config_t, m), 0, 0, NULL, \
     offsetof(config_t, cnt)}

static const field_t FIELDS[] = {
    STR("",       "callsign",    callsign),
    CHOICE("log", "level",       FT_STR,  log.level,   LEVEL_CHOICES),

    STR("mmdvm",  "local_address", mmdvm.local_address),
    PORT("mmdvm", "local_port",    mmdvm.local_port),
    STR("mmdvm",  "rpt_address",   mmdvm.rpt_address),
    PORT("mmdvm", "rpt_port",      mmdvm.rpt_port),

    BOOL("link", "enabled",       link.enabled),
    STR("link",  "remote_host",   link.remote_host),
    PORT("link", "remote_port",   link.remote_port),
    PORT("link", "local_port",    link.local_port),
    STR("link",  "bind_address",  link.bind_address),
    CHOICE("link", "codec", FT_ENUM, link.codec, CODEC_CHOICES),
    INT("link",  "opus_bitrate",  link.opus_bitrate, 4000, 64000),
    INT("link",  "opus_frame_ms", link.opus_frame_ms, 20, 60),
    BOOL("link", "start_on",      link.start_on),

    /* Generous on purpose: these catch typos and nonsense, not unusual sites.
     * Tone and sample scaling clamp to full scale, so >1.0 is legal but clips. */
    DBL("audio",  "master_gain",          audio.master_gain, 0, 10),
    INT("audio",  "morse_wpm",            audio.morse_wpm, 5, 60),
    INT("audio",  "morse_pitch",          audio.morse_pitch, 100, 3400),
    DBL("audio",  "morse_level",          audio.morse_level, 0, 2),
    DBL("audio",  "impolite_morse_level", audio.impolite_morse_level, 0, 2),
    DBL("audio",  "voice_level",          audio.voice_level, 0, 2),
    DBL("audio",  "tone_level",           audio.tone_level, 0, 2),
    INT("audio",  "voice_gap_ms",         audio.voice_gap_ms, 0, 10000),
    INT("audio",  "ste_delay_ms",         audio.ste_delay_ms, 0, 2000),
    INT("audio",  "lead_trim_ms",         audio.lead_trim_ms, 0, 2000),
    INT("audio",  "keyup_delay_ms",       audio.keyup_delay_ms, 0, 10000),
    INT("audio",  "message_gap_ms",       audio.message_gap_ms, 0, 10000),

    DBL("timers", "hang",        timers.hang, 0, 600),
    DBL("timers", "ct_delay",    timers.ct_delay, 0, 60),
    DBL("timers", "kerchunk",    timers.kerchunk, 0, 60),
    DBL("timers", "timeout",     timers.timeout, 0, 3600),   /* 0 = off */
    DBL("timers", "link_timeout", timers.link_timeout, 0, 3600),
    DBL("timers", "id_interval", timers.id_interval, 1, 86400),
    DBL("timers", "id_anxious",  timers.id_anxious, 0, 3600),
    INT("timers", "tail_every",  timers.tail_every, 0, 1000),
    DBL("timers", "tail_min_interval", timers.tail_min_interval, 0, 86400),

    BOOL("dtmf", "enabled",       dtmf.enabled),
    INT("dtmf",  "min_tone_ms",   dtmf.min_tone_ms, 30, 1000),
    INT("dtmf",  "max_tone_ms",   dtmf.max_tone_ms, 100, 60000),
    INT("dtmf",  "interdigit_ms", dtmf.interdigit_ms, 100, 60000),
    INT("dtmf",  "end_of_tx_ms",  dtmf.end_of_tx_ms, 100, 600000),
    INT("dtmf",  "mute_hang_ms",  dtmf.mute_hang_ms, 0, 60000),
    INT("dtmf",  "cover_tone_hz",     dtmf.cover_tone_hz, 100, 3400),
    INT("dtmf",  "cover_tone_on_ms",  dtmf.cover_tone_on_ms, 0, 10000),
    INT("dtmf",  "cover_tone_off_ms", dtmf.cover_tone_off_ms, 0, 10000),
    BOOL("dtmf", "local_cover_tone",  dtmf.local_cover_tone),
    BOOL("dtmf", "link_cover_tone",   dtmf.link_cover_tone),
    STR("dtmf",  "pad_test_prefix", dtmf.pad_test_prefix),
    STR("dtmf",  "time_code",     dtmf.time_code),
    STR("dtmf",  "stats_code",    dtmf.stats_code),
    STR("dtmf",  "echo_code",     dtmf.echo_code),
    STR("dtmf",  "link_on_code",  dtmf.link_on_code),
    STR("dtmf",  "link_off_code", dtmf.link_off_code),
    STR("dtmf",  "link_status_code", dtmf.link_status_code),
    STR("dtmf",  "repeater_code", dtmf.repeater_code),
    STR("dtmf",  "regen_code",    dtmf.regen_code),
    INT("dtmf",  "regen_tone_ms", dtmf.regen_tone_ms, 40, 1000),
    INT("dtmf",  "regen_gap_ms",  dtmf.regen_gap_ms, 40, 1000),
    INT("dtmf",  "regen_lead_ms", dtmf.regen_lead_ms, 0, 2000),
    DBL("dtmf",  "regen_level",   dtmf.regen_level, 0, 1),

    STR("stats", "file", stats.file),
    STR("state", "file", state.file),

    STR("events", "startup_message",        events.startup_message),
    NAMES("initial_ids",   events.initial_ids,   events.n_initial_ids),
    NAMES("mandatory_ids", events.mandatory_ids, events.n_mandatory_ids),
    STR("events", "anxious_id",             events.anxious_id),
    STR("events", "impolite_id",            events.impolite_id),
    STR("events", "ct_link_on_message",             events.ct_link_on_message),
    STR("events", "ct_link_message",        events.ct_link_message),
    STR("events", "ct_link_off_message",    events.ct_link_off_message),
    NAMES("tail_messages", events.tail_messages, events.n_tail_messages),
    STR("events", "time_message",           events.time_message),
    STR("events", "stats_message",          events.stats_message),
    STR("events", "echo_message",           events.echo_message),
    STR("events", "echo_cancel_message",    events.echo_cancel_message),
    STR("events", "link_on_message",        events.link_on_message),
    STR("events", "link_off_message",       events.link_off_message),
    STR("events", "link_none_message",      events.link_none_message),
    STR("events", "repeater_up_message",    events.repeater_up_message),
    STR("events", "repeater_down_message",  events.repeater_down_message),
    STR("events", "timeout_message",        events.timeout_message),
    STR("events", "timeout_cancel_message", events.timeout_cancel_message),
};
#define NFIELDS (sizeof(FIELDS) / sizeof(FIELDS[0]))

#define MESSAGES_SECTION "messages"
#define SCHEDULE_SECTION "schedule"

static const char *const SECTIONS[] = {
    "log", "mmdvm", "link", "audio", "timers", "dtmf", "stats", "state", "events", MESSAGES_SECTION, SCHEDULE_SECTION, NULL,
};

static const char *const ELEM_TYPES[] = {"cw", "voice", "tone", NULL};

/* Searched in order, like the vocab directories: a working-directory copy
 * for running from a source tree, then the installed one. */
static const char *const TONE_CATALOGS[] = {
    "tones.conf", "/usr/local/share/usrp-rc/tones.conf",
};

/* ── error reporting ────────────────────────────────────────────────────── */

/* Problems are counted rather than returned so that one pass reports all of
 * them -- fixing a config shouldn't take one restart per typo. */
static int         s_errors;
static int         s_line;   /* line the current error refers to; 0 = none */
static const char *s_file;   /* set while reading the tone catalog, so errors say which file */

__attribute__((format(printf, 1, 2)))
static void cfg_err(const char *fmt, ...)
{
    LOGE("config: ");
    if (s_file)
        LOGE("%s ", s_file);
    if (s_line)
        LOGE("line %d: ", s_line);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    s_errors++;
}

static size_t edit_distance(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    size_t row[65];
    if (la > 64 || lb > 64)
        return (size_t)-1;
    for (size_t j = 0; j <= lb; j++)
        row[j] = j;
    for (size_t i = 1; i <= la; i++) {
        size_t diag = row[0];
        row[0] = i;
        for (size_t j = 1; j <= lb; j++) {
            size_t up = row[j];
            size_t sub = diag + (tolower((unsigned char)a[i - 1]) != tolower((unsigned char)b[j - 1]));
            size_t best = sub;
            if (up + 1 < best)         best = up + 1;
            if (row[j - 1] + 1 < best) best = row[j - 1] + 1;
            row[j] = best;
            diag = up;
        }
    }
    return row[lb];
}

static bool looks_like(const char *typed, const char *known)
{
    size_t d = edit_distance(typed, known);
    size_t len = strlen(known);
    return d <= 2 && d < len && d < strlen(typed);
}

/* Is typed a better match for known than the current best? */
static bool closer(const char *typed, const char *known, const char *best)
{
    return looks_like(typed, known) && (!best || edit_distance(typed, known) < edit_distance(typed, best));
}

static const char *closest_of(const char *typed, const char *const *known)
{
    const char *best = NULL;
    for (int i = 0; known[i]; i++)
        if (closer(typed, known[i], best))
            best = known[i];
    return best;
}

static void make_path(char *buf, size_t bufsz, const char *section, const char *key)
{
    if (section[0])
        snprintf(buf, bufsz, "%s.%s", section, key);
    else
        snprintf(buf, bufsz, "%s", key);
}

/* Report a key the table doesn't know. A key that exists verbatim in another
 * section beats a spelling match -- it's almost certainly just misplaced. */
static void err_unknown_setting(const char *section, const char *key)
{
    char have[96];
    make_path(have, sizeof(have), section, key);

    if (strcmp(section, "events") == 0 && strcmp(key, "ct_message") == 0) {
        cfg_err("%s: renamed to events.ct_link_on_message (see UPGRADE.md)\n", have);
        return;
    }
    if (strcmp(section, "events") == 0 && strcmp(key, "tail_message") == 0) {
        cfg_err("%s: renamed to events.tail_messages, a comma-separated list\n", have);
        return;
    }
    if (strcmp(section, "audio") == 0 && strcmp(key, "pre_message_ms") == 0) {
        cfg_err("%s: renamed to audio.keyup_delay_ms (see UPGRADE.md)\n", have);
        return;
    }
    if (strcmp(section, "audio") == 0 && strcmp(key, "post_message_ms") == 0) {
        cfg_err("%s: renamed to audio.message_gap_ms, which is also the gap between messages (see UPGRADE.md)\n", have);
        return;
    }
    if (strcmp(section, "dtmf") == 0 && strncmp(key, "beep_", 5) == 0) {
        cfg_err("%s: renamed to dtmf.cover_tone_%s (see UPGRADE.md)\n", have, key + 5);
        return;
    }
    if ((strcmp(section, "audio") == 0 && strcmp(key, "voice_filter") == 0)
        || (!section[0] && strcmp(key, "access_mode") == 0)) {
        cfg_err("%s: removed, delete this line (see UPGRADE.md)\n", have);
        return;
    }
    if (strcmp(section, "timers") == 0 && strcmp(key, "tail_after_hang") == 0) {
        cfg_err("%s: replaced by timers.tail_min_interval, which limits how often a tail message plays\n", have);
        return;
    }

    const field_t *hint = NULL;
    for (size_t i = 0; i < NFIELDS && !hint; i++)
        if (strcmp(FIELDS[i].key, key) == 0)
            hint = &FIELDS[i];
    if (!hint) {
        for (size_t i = 0; i < NFIELDS; i++)
            if (strcmp(FIELDS[i].section, section) == 0 && closer(key, FIELDS[i].key, hint ? hint->key : NULL))
                hint = &FIELDS[i];
    }

    if (hint) {
        char want[96];
        make_path(want, sizeof(want), hint->section, hint->key);
        cfg_err("%s: unknown setting (did you mean %s?)\n", have, want);
    } else {
        cfg_err("%s: unknown setting\n", have);
    }
}

/* ── reading the file ───────────────────────────────────────────────────── */

/* The whole file is read into entries first, then interpreted, so the order
 * of sections doesn't matter and references can be checked against anything
 * in the file. */
typedef struct {
    int   line;
    char  section[16];
    char *key;
    char *value;
} entry_t;

typedef struct {
    entry_t *v;
    int      n;
    int      cap;
} entries_t;

static void entries_free(entries_t *es)
{
    for (int i = 0; i < es->n; i++) {
        free(es->v[i].key);
        free(es->v[i].value);
    }
    free(es->v);
    memset(es, 0, sizeof(*es));
}

static const entry_t *entry_find(const entries_t *es, const char *section, const char *key)
{
    for (int i = 0; i < es->n; i++)
        if (strcmp(es->v[i].section, section) == 0 && strcmp(es->v[i].key, key) == 0)
            return &es->v[i];
    return NULL;
}

/* Trim in place; returns the start of the trimmed text. */
static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

/* '#' starts a comment at the start of a line or after whitespace. It isn't a
 * Morse character or part of any name, so this can't eat real content. */
static void strip_comment(char *s)
{
    for (char *p = s; *p; p++) {
        if (*p == '#' && (p == s || isspace((unsigned char)p[-1]))) {
            *p = '\0';
            return;
        }
    }
}

static void read_section_header(char *line, char *section, size_t sectionsz, bool *skipping)
{
    size_t len = strlen(line);
    if (line[len - 1] != ']') {
        cfg_err("expected [section]: %s\n", line);
        *skipping = true;
        return;
    }
    line[len - 1] = '\0';
    char *name = trim(line + 1);

    for (int i = 0; SECTIONS[i]; i++) {
        if (strcmp(SECTIONS[i], name) == 0) {
            snprintf(section, sectionsz, "%s", name);
            *skipping = false;
            return;
        }
    }

    /* Don't also report every key under it. */
    *skipping = true;
    const char *hint = closest_of(name, SECTIONS);
    if (hint)
        cfg_err("[%s]: unknown section (did you mean [%s]?)\n", name, hint);
    else
        cfg_err("[%s]: unknown section\n", name);
}

static int read_entries(const char *path, entries_t *es)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;

    char   *buf = NULL;
    size_t  bufsz = 0;
    int     lineno = 0;
    char    section[16] = "";
    bool    skipping = false;

    while (getline(&buf, &bufsz, fp) != -1) {
        s_line = ++lineno;
        strip_comment(buf);
        char *line = trim(buf);
        if (!line[0])
            continue;

        if (line[0] == '[') {
            read_section_header(line, section, sizeof(section), &skipping);
            continue;
        }
        if (skipping)
            continue;

        char *eq = strchr(line, '=');
        if (!eq) {
            cfg_err("expected key = value: %s\n", line);
            continue;
        }
        *eq = '\0';
        char *key = trim(line);
        char *value = trim(eq + 1);

        if (!key[0] || strcspn(key, " \t") != strlen(key)) {
            cfg_err("'%s' is not a valid setting name\n", key);
            continue;
        }
        size_t vlen = strlen(value);
        if (vlen >= 2 && value[0] == '"' && value[vlen - 1] == '"') {
            cfg_err("%s: values don't take quotes\n", key);
            continue;
        }
        const entry_t *dup = entry_find(es, section, key);
        if (dup) {
            cfg_err("%s: already set on line %d\n", key, dup->line);
            continue;
        }

        if (es->n >= es->cap) {
            es->cap = es->cap ? es->cap * 2 : 128;
            es->v = xrealloc(es->v, (size_t)es->cap * sizeof(entry_t));
        }
        entry_t *e = &es->v[es->n++];
        e->line = lineno;
        snprintf(e->section, sizeof(e->section), "%s", section);
        e->key = strdup(key);
        e->value = strdup(value);
    }

    free(buf);
    fclose(fp);
    s_line = 0;
    return 0;
}

/* ── field reading ──────────────────────────────────────────────────────── */

static void err_choices(const char *path, const char *got, const char *const *choices)
{
    char list[96] = "";
    size_t used = 0;
    for (int i = 0; choices[i]; i++)
        used += (size_t)snprintf(list + used, sizeof(list) - used, "%s%s", i ? ", " : "", choices[i]);
    cfg_err("%s: '%s' is not valid (expected one of: %s)\n", path, got, list);
}

static int choice_index(const char *const *choices, const char *s)
{
    for (int i = 0; choices[i]; i++)
        if (strcasecmp(choices[i], s) == 0)
            return i;
    return -1;
}

static bool parse_int(const char *s, long long *out)
{
    char *end;
    errno = 0;
    *out = strtoll(s, &end, 10);
    return s[0] && !*end && errno == 0;
}

static bool parse_double(const char *s, double *out)
{
    char *end;
    *out = strtod(s, &end);
    return s[0] && !*end;
}

static void read_name_list(const char *value, const char *path, const field_t *f, config_t *cfg)
{
    char (*dst)[CFG_NAME_STR] = (char (*)[CFG_NAME_STR])((char *)cfg + f->offset);
    int max = (int)(f->size / CFG_NAME_STR);
    char *copy = strdup(value);
    char *save = NULL;
    int count = 0, seen = 0;
    for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        tok += strspn(tok, " \t");
        size_t len = strcspn(tok, " \t");
        if (tok[len + strspn(tok + len, " \t")]) {
            cfg_err("%s: '%s' needs commas between names\n", path, tok);
            continue;
        }
        tok[len] = '\0';
        if (!len)
            continue;
        seen++;
        if (strlen(tok) >= CFG_NAME_STR)
            cfg_err("%s: '%s' is too long (max %d characters)\n", path, tok, CFG_NAME_STR - 1);
        else if (count < max)
            strcpy(dst[count++], tok);
    }
    if (seen > max)
        cfg_err("%s: has %d entries (max %d)\n", path, seen, max);
    *(int *)((char *)cfg + f->count_offset) = count;
    free(copy);
}

static void read_field(const char *value, const char *path, const field_t *f, config_t *cfg)
{
    void *dst = (char *)cfg + f->offset;

    switch (f->type) {
    case FT_STR:
    case FT_ENUM: {
        int idx = f->choices ? choice_index(f->choices, value) : 0;
        if (idx < 0) {
            err_choices(path, value, f->choices);
        } else if (f->type == FT_ENUM) {
            memcpy(dst, &idx, sizeof(idx));
        } else if (strlen(value) >= f->size) {
            cfg_err("%s: is too long (max %zu characters)\n", path, f->size - 1);
        } else {
            strcpy((char *)dst, value);
        }
        return;
    }
    case FT_INT: {
        long long v;
        if (!parse_int(value, &v))
            cfg_err("%s: '%s' must be a whole number\n", path, value);
        else if ((double)v < f->min || (double)v > f->max)
            cfg_err("%s: %lld is out of range (%g to %g)\n", path, v, f->min, f->max);
        else
            *(int *)dst = (int)v;
        return;
    }
    case FT_PORT: {
        long long v;
        if (!parse_int(value, &v))
            cfg_err("%s: '%s' must be a whole number\n", path, value);
        else if (v < 1 || v > 65535)
            cfg_err("%s: %lld is not a valid port (1 to 65535)\n", path, v);
        else
            *(uint16_t *)dst = (uint16_t)v;
        return;
    }
    case FT_DOUBLE: {
        double v;
        if (!parse_double(value, &v))
            cfg_err("%s: '%s' must be a number\n", path, value);
        else if (!(v >= f->min && v <= f->max))
            cfg_err("%s: %g is out of range (%g to %g)\n", path, v, f->min, f->max);
        else
            *(double *)dst = v;
        return;
    }
    case FT_BOOL:
        if (strcasecmp(value, "true") == 0)
            *(bool *)dst = true;
        else if (strcasecmp(value, "false") == 0)
            *(bool *)dst = false;
        else
            cfg_err("%s: '%s' must be true or false\n", path, value);
        return;
    case FT_NAME_LIST:
        read_name_list(value, path, f, cfg);
        return;
    }
}

static void read_settings(const entries_t *es, config_t *cfg)
{
    for (int i = 0; i < es->n; i++) {
        const entry_t *e = &es->v[i];
        if (strcmp(e->section, MESSAGES_SECTION) == 0 || strcmp(e->section, SCHEDULE_SECTION) == 0)
            continue;
        s_line = e->line;

        const field_t *f = NULL;
        for (size_t j = 0; j < NFIELDS && !f; j++)
            if (strcmp(FIELDS[j].section, e->section) == 0 && strcmp(FIELDS[j].key, e->key) == 0)
                f = &FIELDS[j];
        if (!f) {
            err_unknown_setting(e->section, e->key);
            continue;
        }
        char path[96];
        make_path(path, sizeof(path), e->section, e->key);
        read_field(e->value, path, f, cfg);
    }
    s_line = 0;
}

/* The bare callsign: {call} has to work for both CW and spelled-out voice, so
 * suffixes like /R or REPEATER belong in the message that wants them. */
static void check_callsign(const entries_t *es, config_t *cfg)
{
    const entry_t *e = entry_find(es, "", "callsign");
    if (e)
        s_line = e->line;
    for (char *p = cfg->callsign; *p; p++) {
        if (!isalnum((unsigned char)*p)) {
            cfg_err("callsign: '%s' must be letters and digits only (add /R or REPEATER in the message itself)\n",
                    cfg->callsign);
            break;
        }
        *p = (char)toupper((unsigned char)*p);
    }
    s_line = 0;
}

/* ── message syntax ─────────────────────────────────────────────────────── */

/* "_" -> gap, default length (*out_ms = -1). "_NNN" -> gap, NNN ms.
 * Anything else (including "_TEEN" etc.) is not a gap. */
static bool parse_voice_gap(const char *tok, int *out_ms)
{
    if (tok[0] != '_')
        return false;
    const char *rest = tok + 1;
    if (*rest == '\0') {
        *out_ms = -1;
        return true;
    }
    for (const char *p = rest; *p; p++)
        if (!isdigit((unsigned char)*p))
            return false;
    long ms = strtol(rest, NULL, 10);
    *out_ms = ms > 1000000 ? 1000000 : (int)ms;
    return true;
}

static config_elem_t *new_element(config_message_t *out, const char *path)
{
    if (out->nelements == CFG_MAX_ELEMENTS)
        cfg_err("%s: more than %d elements (each tone counts as one)\n", path, CFG_MAX_ELEMENTS);
    if (out->nelements >= CFG_MAX_ELEMENTS) {
        out->nelements = CFG_MAX_ELEMENTS + 1;   /* report once */
        return NULL;
    }
    config_elem_t *elem = &out->elements[out->nelements++];
    memset(elem, 0, sizeof(*elem));
    return elem;
}

/* Replace {call} in text; anything else in braces is a mistake. */
static void read_cw(char *text, const char *path, const config_t *cfg, config_message_t *out)
{
    config_elem_t *elem = new_element(out, path);
    if (!elem)
        return;
    elem->type = ELEM_CW;

    if (!text[0]) {
        cfg_err("%s: cw needs text, e.g. cw: {call}/R\n", path);
        return;
    }

    size_t used = 0;
    const size_t cap = sizeof(elem->cw_text);
    for (char *p = text; *p; ) {
        const char *piece = p;
        size_t len = 1;
        if (strncmp(p, "{call}", 6) == 0) {
            if (!cfg->callsign[0])
                cfg_err("%s: uses {call} but no callsign is set\n", path);
            piece = cfg->callsign;
            len = strlen(piece);
            p += 6;
        } else if (*p == '{') {
            cfg_err("%s: unknown placeholder in '%s' (only {call} works in cw)\n", path, text);
            return;
        } else {
            p++;
        }
        if (used + len >= cap) {
            cfg_err("%s: cw text is too long (max %zu characters)\n", path, cap - 1);
            return;
        }
        memcpy(elem->cw_text + used, piece, len);
        used += len;
    }
    elem->cw_text[used] = '\0';
}

static void add_voice_clip(config_elem_t *elem, const char *path, const char *clip)
{
    if (elem->n_voice_words == CFG_MAX_VOICE_WORDS)
        cfg_err("%s: more than %d words\n", path, CFG_MAX_VOICE_WORDS);
    if (elem->n_voice_words >= CFG_MAX_VOICE_WORDS) {
        elem->n_voice_words = CFG_MAX_VOICE_WORDS + 1;   /* report once */
        return;
    }
    config_voice_word_t *w = &elem->voice_words[elem->n_voice_words++];
    if (strlen(clip) >= sizeof(w->clip)) {
        cfg_err("%s: clip name '%s' is too long (max %zu characters)\n", path, clip, sizeof(w->clip) - 1);
        return;
    }
    strcpy(w->clip, clip);
}

/* Spell the callsign the way it's spoken: letters are their own clips, digits
 * are ZERO..NINE. */
static void add_voice_callsign(config_elem_t *elem, const char *path, const char *callsign)
{
    static const char *const DIGITS[] = {
        "ZERO", "ONE", "TWO", "THREE", "FOUR", "FIVE", "SIX", "SEVEN", "EIGHT", "NINE",
    };
    for (const char *p = callsign; *p; p++) {
        char letter[2] = {*p, '\0'};
        add_voice_clip(elem, path, isdigit((unsigned char)*p) ? DIGITS[*p - '0'] : letter);
    }
}

static voice_var_t voice_var(const char *tok)
{
    if (strcmp(tok, "{time}") == 0)     return VOICE_VAR_TIME;
    if (strcmp(tok, "{ampm}") == 0)     return VOICE_VAR_AMPM;
    if (strcmp(tok, "{greeting}") == 0) return VOICE_VAR_GREETING;
    if (strcmp(tok, "{day}") == 0)      return VOICE_VAR_DAY;
    if (strcmp(tok, "{date}") == 0)     return VOICE_VAR_DATE;
    if (strcmp(tok, "{year}") == 0)     return VOICE_VAR_YEAR;
    if (strcmp(tok, "{keyups_day}") == 0)   return VOICE_VAR_KEYUPS_DAY;
    if (strcmp(tok, "{keyups_week}") == 0)  return VOICE_VAR_KEYUPS_WEEK;
    if (strcmp(tok, "{minutes_day}") == 0)  return VOICE_VAR_MINUTES_DAY;
    if (strcmp(tok, "{minutes_week}") == 0) return VOICE_VAR_MINUTES_WEEK;
    return VOICE_VAR_NONE;
}

static void read_voice(char *words, const char *path, const config_t *cfg, config_message_t *out)
{
    config_elem_t *elem = new_element(out, path);
    if (!elem)
        return;
    elem->type = ELEM_VOICE;

    char *save = NULL;
    for (char *tok = strtok_r(words, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
        int gap_ms;
        voice_var_t var = voice_var(tok);
        if (var != VOICE_VAR_NONE) {
            if (elem->n_voice_words == CFG_MAX_VOICE_WORDS)
                cfg_err("%s: more than %d words\n", path, CFG_MAX_VOICE_WORDS);
            if (elem->n_voice_words >= CFG_MAX_VOICE_WORDS) {
                elem->n_voice_words = CFG_MAX_VOICE_WORDS + 1;
                continue;
            }
            elem->voice_words[elem->n_voice_words++].var = var;
        } else if (strcmp(tok, "{call}") == 0) {
            if (!cfg->callsign[0])
                cfg_err("%s: uses {call} but no callsign is set\n", path);
            add_voice_callsign(elem, path, cfg->callsign);
        } else if (strchr(tok, '{')) {
            cfg_err("%s: unknown placeholder '%s' (see [messages] in the README for the list; each must be its own word)\n",
                    path, tok);
        } else if (parse_voice_gap(tok, &gap_ms)) {
            if (gap_ms > 60000)
                cfg_err("%s: pause '%s' is too long (max 60000 ms)\n", path, tok);
            if (elem->n_voice_words == CFG_MAX_VOICE_WORDS)
                cfg_err("%s: more than %d words\n", path, CFG_MAX_VOICE_WORDS);
            if (elem->n_voice_words >= CFG_MAX_VOICE_WORDS) {
                elem->n_voice_words = CFG_MAX_VOICE_WORDS + 1;
                continue;
            }
            config_voice_word_t *w = &elem->voice_words[elem->n_voice_words++];
            w->is_gap = true;
            w->gap_ms = gap_ms;
        } else {
            add_voice_clip(elem, path, tok);
        }
    }
    if (elem->n_voice_words > CFG_MAX_VOICE_WORDS)
        elem->n_voice_words = CFG_MAX_VOICE_WORDS;
    if (elem->n_voice_words == 0)
        cfg_err("%s: voice needs at least one word\n", path);
}

/* FREQ[+FREQ]:MS[@LEVEL], or _MS for silence. */
static bool parse_tone(const char *tok, config_elem_t *elem)
{
    char *end;
    elem->amp = -1.0;   /* audio.tone_level */

    if (tok[0] == '_') {
        int ms;
        if (!parse_voice_gap(tok, &ms) || ms < 1)
            return false;
        elem->ms = ms;
        elem->amp = 0.0;
        return true;
    }

    elem->freq1 = strtod(tok, &end);
    if (end == tok)
        return false;
    if (*end == '+') {
        const char *second = end + 1;
        elem->freq2 = strtod(second, &end);
        if (end == second)
            return false;
    }
    if (*end != ':')
        return false;
    const char *ms_text = end + 1;
    long ms = strtol(ms_text, &end, 10);
    if (end == ms_text)
        return false;
    elem->ms = ms > 1000000 ? 1000000 : (int)ms;
    if (*end == '@') {
        const char *level = end + 1;
        elem->amp = strtod(level, &end);
        if (end == level)
            return false;
    }
    return *end == '\0';
}

static void read_tones(char *tones, const char *path, config_message_t *out)
{
    char *save = NULL;
    int count = 0;
    for (char *tok = strtok_r(tones, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
        count++;
        config_elem_t *elem = new_element(out, path);
        if (!elem)
            continue;
        elem->type = ELEM_TONE;
        if (!parse_tone(tok, elem)) {
            cfg_err("%s: bad tone '%s' (expected FREQ:MS, FREQ+FREQ:MS, or _MS for silence; add @LEVEL to set its level)\n",
                    path, tok);
            continue;
        }
        if (!(elem->freq1 >= 0 && elem->freq1 <= 4000) || !(elem->freq2 >= 0 && elem->freq2 <= 4000))
            cfg_err("%s: tone '%s': frequency is out of range (0 to 4000 Hz)\n", path, tok);
        if (elem->ms < 1 || elem->ms > 60000)
            cfg_err("%s: tone '%s': duration is out of range (1 to 60000 ms)\n", path, tok);
        if (strchr(tok, '@') && !(elem->amp >= 0.0 && elem->amp <= 2.0))
            cfg_err("%s: tone '%s': level is out of range (0 to 2)\n", path, tok);
    }
    if (count == 0)
        cfg_err("%s: tone needs at least one note, e.g. tone: 660:50\n", path);
}

/* End of the text in [start, end), ignoring whitespace before end. */
static const char *trim_end(const char *start, const char *end)
{
    while (end > start && isspace((unsigned char)end[-1]))
        end--;
    return end;
}

static void read_message_part(char *part, const char *path, bool catalog,
                              const config_t *cfg, config_message_t *out)
{
    part = trim(part);
    if (!part[0]) {
        cfg_err("%s: empty part (expected cw:, voice:, or tone:)\n", path);
        return;
    }
    char *colon = strchr(part, ':');
    size_t word_len = strcspn(part, " \t:");
    if (!colon || part + word_len != trim_end(part, colon)) {
        char word[16];
        snprintf(word, sizeof(word), "%.*s", (int)word_len, part);
        if (choice_index(ELEM_TYPES, word) >= 0)
            cfg_err("%s: '%s' needs a colon after it, e.g. %s: ...\n", path, word, word);
        else
            cfg_err("%s: expected cw:, voice:, or tone: before '%s'\n", path, part);
        return;
    }
    *colon = '\0';
    part = trim(part);
    char *args = trim(colon + 1);

    if (catalog && strcmp(part, "tone") != 0) {
        cfg_err("%s: the tone catalog can only hold tones\n", path);
    } else if (strcmp(part, "cw") == 0) {
        read_cw(args, path, cfg, out);
    } else if (strcmp(part, "voice") == 0) {
        read_voice(args, path, cfg, out);
    } else if (strcmp(part, "tone") == 0) {
        read_tones(args, path, out);
    } else {
        const char *hint = closest_of(part, ELEM_TYPES);
        if (hint)
            cfg_err("%s: unknown type '%s' (did you mean %s?)\n", path, part, hint);
        else
            cfg_err("%s: unknown type '%s' (expected cw, voice, or tone)\n", path, part);
    }
}

static config_message_t *message_slot(config_t *cfg, const char *name, int n_catalog, const char *path)
{
    /* A site message may replace a catalog tone by reusing its name. */
    for (int i = 0; i < n_catalog; i++)
        if (strcmp(cfg->messages[i].name, name) == 0)
            return &cfg->messages[i];
    if (cfg->nmessages == cfg->messages_cap) {
        int cap = cfg->messages_cap ? cfg->messages_cap * 2 : 64;
        config_message_t *grown = realloc(cfg->messages, (size_t)cap * sizeof(*grown));
        if (!grown) {
            cfg_err("%s: out of memory\n", path);
            return NULL;
        }
        cfg->messages = grown;
        cfg->messages_cap = cap;
    }
    return &cfg->messages[cfg->nmessages++];
}

static void read_messages(const entries_t *es, config_t *cfg, bool catalog)
{
    int n_catalog = cfg->nmessages;

    for (int i = 0; i < es->n; i++) {
        const entry_t *e = &es->v[i];
        if (strcmp(e->section, MESSAGES_SECTION) != 0)
            continue;
        s_line = e->line;

        char path[64];
        snprintf(path, sizeof(path), "messages.%s", e->key);
        if (strlen(e->key) >= CFG_NAME_STR) {
            cfg_err("%s: name is too long (max %d characters)\n", path, CFG_NAME_STR - 1);
            continue;
        }
        if (!e->value[0]) {
            cfg_err("%s: has no content\n", path);
            continue;
        }
        config_message_t *out = message_slot(cfg, e->key, n_catalog, path);
        if (!out)
            continue;
        memset(out, 0, sizeof(*out));
        strcpy(out->name, e->key);
        out->line = e->line;

        char *copy = strdup(e->value);
        char *rest = copy;
        for (;;) {
            char *bar = strchr(rest, '|');
            if (bar)
                *bar = '\0';
            read_message_part(rest, path, catalog, cfg, out);
            if (!bar)
                break;
            rest = bar + 1;
        }
        free(copy);
        if (out->nelements > CFG_MAX_ELEMENTS)
            out->nelements = CFG_MAX_ELEMENTS;
    }
    s_line = 0;
}

/* Replies that must exist for the commands to work out of the box. Loaded ahead
 * of the config's own messages, so reusing one of these names replaces it. */
static void load_builtin_messages(config_t *cfg)
{
    static const struct { const char *name, *words; } BUILTIN[] = {
        {"time_readback",  "F_GOOD {greeting} _ F_THE F_TIME F_IS {time} {ampm}"},
        {"link_on_reply",  "LINK ON"},
        {"link_off_reply", "LINK OFF"},
        {"link_none_reply", "NO LINK SERVICE"},
        {"repeater_up_reply", "REPEATER UP"},
        {"repeater_down_reply", "REPEATER DOWN"},
        {"stats_readback", "{keyups_day} KEY UP _S _ {minutes_day} MINUTES"},
        {"echo_reply", "ECHO TEST READY"},
        {"echo_cancel_reply", "ECHO TEST CANCEL"},
    };
    for (size_t i = 0; i < sizeof(BUILTIN) / sizeof(BUILTIN[0]); i++) {
        char words[128];
        snprintf(words, sizeof(words), "%s", BUILTIN[i].words);
        config_message_t *m = message_slot(cfg, BUILTIN[i].name, 0, BUILTIN[i].name);
        if (!m)
            return;
        memset(m, 0, sizeof(*m));
        strcpy(m->name, BUILTIN[i].name);
        read_voice(words, BUILTIN[i].name, cfg, m);
    }
}

static void load_tone_catalog(config_t *cfg)
{
    for (size_t i = 0; i < sizeof(TONE_CATALOGS) / sizeof(TONE_CATALOGS[0]); i++) {
        entries_t es = {0};
        s_file = TONE_CATALOGS[i];
        if (read_entries(TONE_CATALOGS[i], &es) != 0) {
            s_file = NULL;
            continue;
        }
        for (int j = 0; j < es.n; j++) {
            if (strcmp(es.v[j].section, MESSAGES_SECTION) != 0) {
                s_line = es.v[j].line;
                cfg_err("%s: the tone catalog can only hold [messages]\n", es.v[j].key);
                s_line = 0;
            }
        }
        read_messages(&es, cfg, true);
        entries_free(&es);
        s_file = NULL;
        return;
    }
    LOGW("config: tone catalog not found -- only tones in this config are available\n");
}

/* ── schedule ───────────────────────────────────────────────────────────── */

static const char *ordinal_suffix(int n)
{
    if (n >= 11 && n <= 13)
        return "th";
    switch (n % 10) {
    case 1:  return "st";
    case 2:  return "nd";
    case 3:  return "rd";
    default: return "th";
    }
}

/* "15th", with the suffix that number really takes. Returns the number and sets
 * *rest to what follows it, or returns 0. */
static int parse_ordinal(const char *tok, const char **rest)
{
    int n = 0, i = 0;
    while (tok[i] >= '0' && tok[i] <= '9' && i < 3)
        n = n * 10 + (tok[i++] - '0');
    if (i == 0 || tok[0] == '0' || strncasecmp(tok + i, ordinal_suffix(n), 2) != 0)
        return 0;
    *rest = tok + i + 2;
    return n;
}

static int name_index(const char *const *names, int n, const char *text)
{
    for (int i = 0; i < n; i++)
        if (strcasecmp(text, names[i]) == 0)
            return i;
    return -1;
}

static bool parse_day_rule(const char *tok, config_day_rule_t *rule)
{
    static const char *const WEEKDAYS[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
    static const char *const MONTHS[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                         "jul", "aug", "sep", "oct", "nov", "dec"};
    /* February allows the 29th, which matches in leap years. */
    static const int MONTH_DAYS[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    if (strcasecmp(tok, "last-day") == 0) {
        rule->type = DAY_RULE_LAST_DAY;
        return true;
    }

    const char *rest;
    int n = parse_ordinal(tok, &rest);
    if (n > 0) {
        if (!rest[0] && n <= 31) {
            *rule = (config_day_rule_t){DAY_RULE_MONTH_DAY, n, 0};
            return true;
        }
        int wday = rest[0] == '-' ? name_index(WEEKDAYS, 7, rest + 1) : -1;
        if (wday >= 0 && n <= 5) {
            *rule = (config_day_rule_t){DAY_RULE_NTH_WEEKDAY, n, wday};
            return true;
        }
        return false;
    }

    const char *dash = strchr(tok, '-');
    if (dash && dash - tok == 3) {
        char month[4];
        memcpy(month, tok, 3);
        month[3] = '\0';
        int mon = name_index(MONTHS, 12, month);
        char *end;
        long day = strtol(dash + 1, &end, 10);
        if (mon >= 0 && dash[1] >= '1' && dash[1] <= '9' && !*end && day <= MONTH_DAYS[mon]) {
            *rule = (config_day_rule_t){DAY_RULE_DATE, mon, (int)day};
            return true;
        }
    }
    return false;
}

static bool parse_days(const char *text, config_schedule_t *out)
{
    static const char *const NAMES[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
    char copy[CFG_STR];
    if (snprintf(copy, sizeof(copy), "%s", text) >= (int)sizeof(copy))
        return false;
    out->days = 0;
    out->nrules = 0;
    char *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        int wday = name_index(NAMES, 7, tok);
        if (strcasecmp(tok, "daily") == 0) {
            out->days |= 0x7Fu;
        } else if (strcasecmp(tok, "weekdays") == 0) {
            out->days |= 0x3Eu;
        } else if (strcasecmp(tok, "weekends") == 0) {
            out->days |= 0x41u;
        } else if (wday >= 0) {
            out->days |= 1u << wday;
        } else {
            if (out->nrules == CFG_MAX_DAY_RULES || !parse_day_rule(tok, &out->rules[out->nrules]))
                return false;
            out->nrules++;
        }
    }
    return out->days != 0 || out->nrules > 0;
}

/* HH:MM-HH:MM, on a 24-hour clock. */
static bool parse_window(const char *text, int *start, int *end)
{
    int h1, m1, h2, m2, used = 0;
    if (sscanf(text, "%d:%d-%d:%d%n", &h1, &m1, &h2, &m2, &used) != 4 || text[used])
        return false;
    if (h1 < 0 || h1 > 23 || h2 < 0 || h2 > 23 || m1 < 0 || m1 > 59 || m2 < 0 || m2 > 59)
        return false;
    *start = h1 * 60 + m1;
    *end   = h2 * 60 + m2;
    return true;
}

static void check_message_ref(const config_t *cfg, const char *path, const char *name);

/* Why a setting can't go in a schedule window, or NULL if it can. */
static const char *not_schedulable(const field_t *f)
{
    static const char *const STARTUP_SECTIONS[] = {"", "log", "mmdvm", "link", "stats", "state", NULL};
    static const char *const STARTUP_KEYS[] = {
        "dtmf.enabled", "dtmf.min_tone_ms", "dtmf.cover_tone_hz", "dtmf.cover_tone_on_ms",
        "dtmf.cover_tone_off_ms", "dtmf.local_cover_tone", "dtmf.link_cover_tone",
        "dtmf.regen_code", "dtmf.regen_tone_ms", "dtmf.regen_gap_ms", "dtmf.regen_lead_ms",
        "dtmf.regen_level",
        "audio.ste_delay_ms", "audio.lead_trim_ms", NULL,
    };
    if (strcmp(f->section, "events") == 0 && strcmp(f->key, "startup_message") == 0)
        return "it only plays at startup";
    for (int i = 0; STARTUP_SECTIONS[i]; i++)
        if (strcmp(f->section, STARTUP_SECTIONS[i]) == 0)
            return "it only takes effect at startup";
    char path[96];
    make_path(path, sizeof(path), f->section, f->key);
    for (int i = 0; STARTUP_KEYS[i]; i++)
        if (strcmp(path, STARTUP_KEYS[i]) == 0)
            return "it only takes effect at startup";
    return NULL;
}

/* A schedule names a setting by its key, or section.key where the key alone is
 * ambiguous. */
static const field_t *find_schedule_field(const char *name, const char *path)
{
    const char *dot = strchr(name, '.');
    const field_t *found = NULL;
    int matches = 0;
    for (size_t i = 0; i < NFIELDS; i++) {
        const field_t *f = &FIELDS[i];
        bool hit = dot ? strncmp(f->section, name, (size_t)(dot - name)) == 0
                         && !f->section[dot - name] && strcmp(f->key, dot + 1) == 0
                       : strcmp(f->key, name) == 0;
        if (hit) {
            found = f;
            matches++;
        }
    }
    if (matches > 1) {
        cfg_err("%s: '%s' is in more than one section; write it as section.%s\n", path, name, name);
        return NULL;
    }
    if (found)
        return found;

    const char *hint = NULL;
    char want[96], best[96] = "";
    for (size_t i = 0; i < NFIELDS && dot && !hint; i++)
        if (strcmp(FIELDS[i].key, dot + 1) == 0) {
            make_path(best, sizeof(best), FIELDS[i].section, FIELDS[i].key);
            hint = best;
        }
    bool misplaced = hint != NULL;
    for (size_t i = 0; i < NFIELDS && !misplaced; i++) {
        if (not_schedulable(&FIELDS[i]))
            continue;
        if (dot)
            make_path(want, sizeof(want), FIELDS[i].section, FIELDS[i].key);
        else
            snprintf(want, sizeof(want), "%s", FIELDS[i].key);
        if (closer(name, want, hint)) {
            snprintf(best, sizeof(best), "%s", want);
            hint = best;
        }
    }
    if (hint)
        cfg_err("%s: '%s' is not a setting (did you mean %s?)\n", path, name, hint);
    else
        cfg_err("%s: '%s' is not a setting\n", path, name);
    return NULL;
}

static void read_override(char *tok, const char *path, const config_t *cfg, config_t *scratch,
                          config_schedule_t *out)
{
    char *eq = strchr(tok, '=');
    if (!eq) {
        cfg_err("%s: expected setting=value, got '%s'\n", path, tok);
        return;
    }
    *eq = '\0';
    override_op_t op = OVERRIDE_SET;
    if (eq > tok && (eq[-1] == '+' || eq[-1] == '-')) {
        op = eq[-1] == '+' ? OVERRIDE_ADD : OVERRIDE_REMOVE;
        eq[-1] = '\0';
    }
    if (strcmp(tok, "tail_message") == 0) {
        cfg_err("%s: tail_message is renamed to tail_messages, a comma-separated list\n", path);
        return;
    }
    const char *key = tok, *value = eq + 1;

    const field_t *f = find_schedule_field(key, path);
    if (!f)
        return;
    const char *why = not_schedulable(f);
    if (why) {
        char full[96];
        make_path(full, sizeof(full), f->section, f->key);
        cfg_err("%s: %s can't be scheduled: %s\n", path, full, why);
        return;
    }
    bool names = strcmp(f->section, "events") == 0;
    if (names && strcmp(f->key, "impolite_id") == 0 && !value[0]) {
        cfg_err("%s: impolite_id can't be empty: it's the ID of last resort\n", path);
        return;
    }
    if (op != OVERRIDE_SET && f->type != FT_NAME_LIST) {
        cfg_err("%s: %s takes one value, so it can't use += or -=\n", path, f->key);
        return;
    }
    if (op != OVERRIDE_SET && !value[0]) {
        cfg_err("%s: %s%s= needs at least one message\n", path, f->key, op == OVERRIDE_ADD ? "+" : "-");
        return;
    }
    if (out->nsets == CFG_MAX_SCHEDULE_SETS) {
        cfg_err("%s: more than %d settings\n", path, CFG_MAX_SCHEDULE_SETS);
        return;
    }
    if (strlen(value) >= sizeof(out->sets[0].value)) {
        cfg_err("%s: value for %s is too long\n", path, f->key);
        return;
    }

    config_override_t o = {
        .offset = f->offset,
        .size   = f->size,
        .count_offset = f->type == FT_NAME_LIST ? f->count_offset : 0,
        .op     = op,
        .names  = names,
    };
    if (f->type == FT_NAME_LIST) {
        strcpy(o.value, value);
    } else {
        /* Parsed and range-checked exactly as the same line in its own section. */
        char where[160];
        snprintf(where, sizeof(where), "%s: %s", path, f->key);
        int before = s_errors;
        read_field(value, where, f, scratch);
        if (s_errors != before)
            return;
        memcpy(o.value, (const char *)scratch + f->offset, f->size);
    }
    out->sets[out->nsets++] = o;

    if (!names)
        return;
    char copy[CFG_STR];
    strcpy(copy, value);
    int count = 0;
    char *save = NULL;
    for (char *name = strtok_r(copy, ",", &save); name; name = strtok_r(NULL, ",", &save)) {
        count++;
        if (strlen(name) >= CFG_NAME_STR)
            cfg_err("%s: '%s' is too long (max %d characters)\n", path, name, CFG_NAME_STR - 1);
        check_message_ref(cfg, path, name);
    }
    int max = f->type == FT_NAME_LIST ? (int)(f->size / CFG_NAME_STR) : 1;
    if (count > max)
        cfg_err("%s: %s takes at most %d name(s)\n", path, f->key, max);
}

static int name_position(char (*names)[CFG_NAME_STR], int count, const char *name)
{
    for (int i = 0; i < count; i++)
        if (strcmp(names[i], name) == 0)
            return i;
    return -1;
}

void config_apply_override(config_t *cfg, const config_override_t *o)
{
    char *dst = (char *)cfg + o->offset;

    if (!o->count_offset) {
        memcpy(dst, o->value, o->size);
        return;
    }

    char (*names)[CFG_NAME_STR] = (char (*)[CFG_NAME_STR])dst;
    int *count = (int *)((char *)cfg + o->count_offset);
    int max = (int)(o->size / CFG_NAME_STR);
    /* The ID is a legal requirement: a window can't remove the last one by accident. */
    int min = dst == (char *)cfg->events.mandatory_ids ? 1 : 0;

    if (o->op == OVERRIDE_SET)
        *count = 0;

    char copy[sizeof(o->value)];
    strcpy(copy, o->value);
    char *save = NULL;
    for (char *n = strtok_r(copy, ",", &save); n; n = strtok_r(NULL, ",", &save)) {
        int at = name_position(names, *count, n);
        if (o->op == OVERRIDE_REMOVE) {
            if (at >= 0 && *count > min) {
                memmove(names[at], names[at + 1], (size_t)(*count - at - 1) * CFG_NAME_STR);
                (*count)--;
            }
        } else if (*count < max && (o->op == OVERRIDE_SET || at < 0)) {
            snprintf(names[(*count)++], CFG_NAME_STR, "%s", n);
        }
    }
}

/* name = DAYS HH:MM-HH:MM setting=value [setting=value ...] */
static void read_schedule(const entries_t *es, config_t *cfg)
{
    config_t *scratch = calloc(1, sizeof(*scratch));
    if (!scratch) {
        cfg_err("out of memory\n");
        return;
    }
    for (int i = 0; i < es->n; i++) {
        const entry_t *e = &es->v[i];
        if (strcmp(e->section, SCHEDULE_SECTION) != 0)
            continue;
        s_line = e->line;

        char path[64];
        snprintf(path, sizeof(path), "schedule.%s", e->key);
        if (strlen(e->key) >= CFG_NAME_STR) {
            cfg_err("%s: name is too long (max %d characters)\n", path, CFG_NAME_STR - 1);
            continue;
        }
        if (cfg->nschedule == CFG_MAX_SCHEDULE) {
            cfg_err("%s: too many schedule entries (max %d)\n", path, CFG_MAX_SCHEDULE);
            continue;
        }

        config_schedule_t *out = &cfg->schedule[cfg->nschedule];
        memset(out, 0, sizeof(*out));
        strcpy(out->name, e->key);

        char *copy = strdup(e->value);
        char *save = NULL;
        char *days = strtok_r(copy, " \t", &save);
        char *window = days ? strtok_r(NULL, " \t", &save) : NULL;
        if (!window) {
            cfg_err("%s: expected DAYS HH:MM-HH:MM setting=value\n", path);
            free(copy);
            continue;
        }
        if (!parse_days(days, out))
            cfg_err("%s: '%s' is not valid days (use sun..sat, daily, weekdays, weekends, 2nd-tue, 15th, last-day or dec-8, comma-separated; at most %d of the last four kinds)\n",
                    path, days, CFG_MAX_DAY_RULES);
        if (!parse_window(window, &out->start_min, &out->end_min))
            cfg_err("%s: '%s' is not a valid time window (expected HH:MM-HH:MM, 24-hour)\n", path, window);
        else if (out->start_min == out->end_min)
            cfg_err("%s: window '%s' starts and ends at the same time\n", path, window);

        for (char *tok = strtok_r(NULL, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save))
            read_override(tok, path, cfg, scratch, out);
        if (out->nsets == 0)
            cfg_err("%s: changes nothing (add at least one setting=value)\n", path);

        cfg->nschedule++;
        free(copy);
    }
    free(scratch);
    s_line = 0;
}

/* ── validation ─────────────────────────────────────────────────────────── */

static void check_message_ref(const config_t *cfg, const char *path, const char *name)
{
    if (config_find_message(cfg, name))
        return;

    const char *hint = NULL;
    for (int i = 0; i < cfg->nmessages; i++)
        if (closer(name, cfg->messages[i].name, hint))
            hint = cfg->messages[i].name;
    if (hint)
        cfg_err("%s: no message named '%s' (did you mean '%s'?)\n", path, name, hint);
    else
        cfg_err("%s: no message named '%s'\n", path, name);
}

/* Check one [events] setting. Empty means that event plays nothing. */
static void check_event(const entries_t *es, const config_t *cfg, const char *key,
                        const char names[][CFG_NAME_STR], int n)
{
    const entry_t *e = entry_find(es, "events", key);
    s_line = e ? e->line : 0;
    char path[64];
    snprintf(path, sizeof(path), "events.%s", key);
    for (int i = 0; i < n; i++)
        if (names[i][0])
            check_message_ref(cfg, path, names[i]);
    s_line = 0;
}

/* The names an [events] setting holds. Every [events] setting is a message name, so
 * walking FIELDS covers all of them, including any added later. */
static int event_names(const config_t *cfg, const field_t *f, const char (**names)[CFG_NAME_STR])
{
    *names = (const char (*)[CFG_NAME_STR])((const char *)cfg + f->offset);
    return f->type == FT_NAME_LIST ? *(const int *)((const char *)cfg + f->count_offset) : 1;
}

static int count_names(const char *value)
{
    int count = 0;
    char copy[CFG_STR];
    snprintf(copy, sizeof(copy), "%s", value);
    char *save = NULL;
    for (char *n = strtok_r(copy, ",", &save); n; n = strtok_r(NULL, ",", &save))
        count++;
    return count;
}

/* Which windows overlap depends on the clock, so these are warnings about what could
 * happen. At run time, a list simply stops growing at its limit, and the last
 * mandatory ID stays. */
static void check_schedule_lists(const config_t *cfg)
{
    const config_events_t *ev = &cfg->events;
    static const struct { const char *key; size_t offset; size_t count_offset; } LISTS[] = {
        {"initial_ids",   offsetof(config_t, events.initial_ids),   offsetof(config_t, events.n_initial_ids)},
        {"mandatory_ids", offsetof(config_t, events.mandatory_ids), offsetof(config_t, events.n_mandatory_ids)},
        {"tail_messages", offsetof(config_t, events.tail_messages), offsetof(config_t, events.n_tail_messages)},
    };
    for (size_t l = 0; l < sizeof(LISTS) / sizeof(LISTS[0]); l++) {
        int most = *(const int *)((const char *)cfg + LISTS[l].count_offset);
        for (int i = 0; i < cfg->nschedule; i++)
            for (int j = 0; j < cfg->schedule[i].nsets; j++) {
                const config_override_t *o = &cfg->schedule[i].sets[j];
                if (o->offset == LISTS[l].offset && o->op == OVERRIDE_ADD)
                    most += count_names(o->value);
            }
        if (most > CFG_MAX_ID_ROTATION)
            LOGW("config: schedule: += could put more than %d names in %s; any past that are left out\n",
                 CFG_MAX_ID_ROTATION, LISTS[l].key);
    }

    for (int i = 0; i < cfg->nschedule; i++)
        for (int j = 0; j < cfg->schedule[i].nsets; j++) {
            const config_override_t *o = &cfg->schedule[i].sets[j];
            if (o->offset != offsetof(config_t, events.mandatory_ids))
                continue;
            if (o->op == OVERRIDE_SET && !o->value[0])
                LOGW("config: schedule.%s: mandatory_ids= leaves no mandatory ID during this window, so it will use impolite_id\n",
                     cfg->schedule[i].name);
            if (o->op != OVERRIDE_REMOVE)
                continue;
            int kept = 0;
            for (int n = 0; n < ev->n_mandatory_ids; n++) {
                char list[CFG_STR + 2];
                char want[CFG_NAME_STR + 2];
                snprintf(list, sizeof(list), ",%s,", o->value);
                snprintf(want, sizeof(want), ",%s,", ev->mandatory_ids[n]);
                if (!strstr(list, want))
                    kept++;
            }
            if (ev->n_mandatory_ids > 0 && kept == 0)
                LOGW("config: schedule.%s: mandatory_ids-= would remove every ID; the last one stays\n",
                     cfg->schedule[i].name);
        }
}

/* Where the settings being checked come from: NULL for the file as written, or a
 * schedule window, whose settings are checked as they'd be while it's open. */
static const char *s_where;

const config_dtmf_code_t CONFIG_DTMF_CODES[] = {
    {"time_code",        offsetof(config_dtmf_t, time_code),        CODE_WHOLE,   COMMAND_TIME,        "time"},
    {"stats_code",       offsetof(config_dtmf_t, stats_code),       CODE_WHOLE,   COMMAND_STATS,       "usage"},
    {"echo_code",        offsetof(config_dtmf_t, echo_code),        CODE_WHOLE,   COMMAND_ECHO,        "echo test"},
    {"link_on_code",     offsetof(config_dtmf_t, link_on_code),     CODE_WHOLE,   COMMAND_LINK_ON,     "link on"},
    {"link_off_code",    offsetof(config_dtmf_t, link_off_code),    CODE_WHOLE,   COMMAND_LINK_OFF,    "link off"},
    {"link_status_code", offsetof(config_dtmf_t, link_status_code), CODE_WHOLE,   COMMAND_LINK_STATUS, "link status"},
    {"repeater_code",    offsetof(config_dtmf_t, repeater_code),    CODE_UP_DOWN, COMMAND_REPEATER,    "repeater"},
    {"regen_code",       offsetof(config_dtmf_t, regen_code),       CODE_CAPTURE, COMMAND_REGEN,       "regen"},
    {"pad_test_prefix",  offsetof(config_dtmf_t, pad_test_prefix),  CODE_CAPTURE, COMMAND_PAD_TEST,    "DTMF test +"},
};
const size_t CONFIG_NDTMF_CODES = sizeof(CONFIG_DTMF_CODES) / sizeof(CONFIG_DTMF_CODES[0]);

#define WHERE_FMT "%s%s"
#define WHERE_ARGS s_where ? s_where : "", s_where ? ": " : ""

/* Rules between settings, so they apply to every combination a window can make. */
static void check_consistency(const config_t *cfg)
{
    const config_events_t *ev = &cfg->events;

    const config_dtmf_code_t *CODES = CONFIG_DTMF_CODES;
    const size_t NCODES = CONFIG_NDTMF_CODES;
#define CODE(i) ((const char *)&cfg->dtmf + CODES[i].offset)
    for (size_t i = 0; i < NCODES; i++) {
        const char *code = CODE(i);
        /* # and D are the cancel and enter keys, so a code could never be typed. */
        if (code[strspn(code, "0123456789*ABC")])
            cfg_err(WHERE_FMT "dtmf.%s: '%s' can only use 0-9, *, A, B and C\n", WHERE_ARGS, CODES[i].key, code);
        /* Only the pad test may be one key, as 5 is on nearly every controller. */
        if (CODES[i].type != COMMAND_PAD_TEST && strlen(code) == 1)
            cfg_err(WHERE_FMT "dtmf.%s: '%s' must be at least 2 keys\n", WHERE_ARGS, CODES[i].key, code);
        for (size_t j = 0; j < i; j++)
            if (code[0] && strcmp(code, CODE(j)) == 0)
                cfg_err(WHERE_FMT "dtmf.%s: '%s' is already used by dtmf.%s\n", WHERE_ARGS,
                        CODES[i].key, code, CODES[j].key);
    }
    for (size_t i = 0; i < NCODES; i++) {
        const char *own = CODE(i);
        if (!own[0])
            continue;
        /* A capture code takes every key after it, so no other code can start with one. */
        if (CODES[i].kind == CODE_CAPTURE) {
            for (size_t j = 0; j < NCODES; j++)
                if (j != i && strcmp(CODE(j), own) != 0 && strncmp(CODE(j), own, strlen(own)) == 0)
                    cfg_err(WHERE_FMT "dtmf.%s: '%s' starts with dtmf.%s '%s', which takes every key after it\n",
                            WHERE_ARGS, CODES[j].key, CODE(j), CODES[i].key, own);
        }
        /* An up/down command is its code plus 0 or 1, so those can't be other codes. */
        if (CODES[i].kind == CODE_UP_DOWN) {
            for (int arg = 0; arg < 2; arg++) {
                char full[CFG_DTMF_PREFIX_STR + 2];
                snprintf(full, sizeof(full), "%s%d", own, arg);
                for (size_t j = 0; j < NCODES; j++)
                    if (j != i && strcmp(full, CODE(j)) == 0)
                        cfg_err(WHERE_FMT "dtmf.%s: '%s' is already used by dtmf.%s\n", WHERE_ARGS,
                                CODES[i].key, full, CODES[j].key);
            }
        }
    }
#undef CODE
    if (cfg->dtmf.mute_hang_ms >= cfg->dtmf.interdigit_ms)
        cfg_err(WHERE_FMT "dtmf.mute_hang_ms (%d) must be less than dtmf.interdigit_ms (%d)\n", WHERE_ARGS,
                cfg->dtmf.mute_hang_ms, cfg->dtmf.interdigit_ms);

    if (ev->anxious_id[0] && cfg->timers.id_anxious >= cfg->timers.id_interval)
        cfg_err(WHERE_FMT "timers.id_anxious (%g) must be less than timers.id_interval (%g)\n", WHERE_ARGS,
                cfg->timers.id_anxious, cfg->timers.id_interval);
}

/* Settings that are legal but worth a second look. For a window, only what it
 * changes: the file's own settings have already had their say. */
static bool message_uses(const config_t *cfg, const char *name, int type)
{
    const config_message_t *m = name[0] ? config_find_message(cfg, name) : NULL;
    for (int i = 0; m && i < m->nelements; i++)
        if ((int)m->elements[i].type == type)
            return true;
    return false;
}

/* Does any ID other than the impolite one use this kind of part? */
static bool id_uses(const config_t *cfg, int type)
{
    const config_events_t *ev = &cfg->events;
    for (int i = 0; i < ev->n_initial_ids; i++)
        if (message_uses(cfg, ev->initial_ids[i], type))
            return true;
    for (int i = 0; i < ev->n_mandatory_ids; i++)
        if (message_uses(cfg, ev->mandatory_ids[i], type))
            return true;
    return message_uses(cfg, ev->anxious_id, type);
}

static bool cw_id_fast(const config_t *c)
{
    return c->audio.morse_wpm > 20
        && (id_uses(c, ELEM_CW) || message_uses(c, c->events.impolite_id, ELEM_CW));
}
static bool cw_id_silent(const config_t *c)     { return c->audio.morse_level == 0.0 && id_uses(c, ELEM_CW); }
static bool voice_id_silent(const config_t *c)
{
    return c->audio.voice_level == 0.0
        && (id_uses(c, ELEM_VOICE) || message_uses(c, c->events.impolite_id, ELEM_VOICE));
}
static bool impolite_silent(const config_t *c)
{
    return c->audio.impolite_morse_level == 0.0 && message_uses(c, c->events.impolite_id, ELEM_CW);
}

/* Settings that are legal but worth a second look. For a window, only what it
 * changes: the file's own settings have already had their say. The ID warnings
 * cite US rules; elsewhere they may not apply, so they don't stop a start. */
static void warn_settings(const config_t *cfg, const config_t *base)
{
#define NEWLY(member, test) (cfg->member test && !(base && base->member test))
#define NEWLY_P(pred) (pred(cfg) && !(base && pred(base)))
    if (NEWLY(timers.timeout, == 0.0))
        LOGW("config: " WHERE_FMT "timers.timeout is 0: a local carrier can hold the transmitter on with no limit\n",
             WHERE_ARGS);
    if (cfg->link.enabled && NEWLY(timers.link_timeout, == 0.0))
        LOGW("config: " WHERE_FMT "timers.link_timeout is 0: the link can hold the transmitter on with no limit\n",
             WHERE_ARGS);
    if (NEWLY(timers.id_interval, > 600.0))
        LOGW("config: " WHERE_FMT "timers.id_interval is %g s; in the US the limit is 10 minutes (FCC 97.119(a))\n",
             WHERE_ARGS, cfg->timers.id_interval);
    if (NEWLY_P(cw_id_fast))
        LOGW("config: " WHERE_FMT "audio.morse_wpm is %d and an ID uses CW; in the US an automatic CW ID "
             "may not exceed 20 wpm (FCC 97.119(b)(1))\n", WHERE_ARGS, cfg->audio.morse_wpm);
    if (NEWLY(audio.master_gain, == 0.0))
        LOGW("config: " WHERE_FMT "audio.master_gain is 0: IDs and all other controller audio are silent\n",
             WHERE_ARGS);
    if (NEWLY_P(cw_id_silent))
        LOGW("config: " WHERE_FMT "audio.morse_level is 0: CW IDs are silent\n", WHERE_ARGS);
    if (NEWLY_P(voice_id_silent))
        LOGW("config: " WHERE_FMT "audio.voice_level is 0: voice IDs are silent\n", WHERE_ARGS);
    if (NEWLY_P(impolite_silent))
        LOGW("config: " WHERE_FMT "audio.impolite_morse_level is 0: the impolite ID is silent\n", WHERE_ARGS);
#undef NEWLY_P
#undef NEWLY
}

/* The sockets take numeric IPv4 addresses only, so a name would stop the start. */
static void check_address(const char *path, const char *value)
{
    struct in_addr a;
    if (value[0] && inet_pton(AF_INET, value, &a) != 1)
        cfg_err("%s: '%s' must be an IPv4 address, such as 127.0.0.1\n", path, value);
}

static void validate(const entries_t *es, const config_t *cfg)
{
    const config_events_t *ev = &cfg->events;

    check_address("mmdvm.local_address", cfg->mmdvm.local_address);
    check_address("mmdvm.rpt_address", cfg->mmdvm.rpt_address);
    if (cfg->link.enabled) {
        if (cfg->link.remote_host[0] == '\0')
            cfg_err("link.remote_host is required (or set link.enabled = false)\n");
        check_address("link.remote_host", cfg->link.remote_host);
        check_address("link.bind_address", cfg->link.bind_address);
        if (cfg->link.codec == LINK_CODEC_OPUS && cfg->link.opus_frame_ms != 20
            && cfg->link.opus_frame_ms != 40 && cfg->link.opus_frame_ms != 60)
            cfg_err("link.opus_frame_ms must be 20, 40, or 60\n");
        /* A USRP Opus packet carries at most USRP_OPUS_MAX_BYTES of audio. */
        long frame_bytes = (long)cfg->link.opus_bitrate * cfg->link.opus_frame_ms / 8000;
        if (cfg->link.codec == LINK_CODEC_OPUS && frame_bytes > (long)USRP_OPUS_MAX_BYTES)
            cfg_err("link.opus_bitrate %d at %d ms frames is %ld bytes a frame, over the %u-byte "
                    "limit; lower one of them\n", cfg->link.opus_bitrate, cfg->link.opus_frame_ms,
                    frame_bytes, USRP_OPUS_MAX_BYTES);
    }

    for (size_t i = 0; i < NFIELDS; i++) {
        if (strcmp(FIELDS[i].section, "events") != 0)
            continue;
        const char (*names)[CFG_NAME_STR];
        int n = event_names(cfg, &FIELDS[i], &names);
        check_event(es, cfg, FIELDS[i].key, names, n);
    }
    if (!ev->impolite_id[0])
        cfg_err("events.impolite_id is required: it's the ID of last resort if mandatory_ids is empty\n");

    int before = s_errors;
    check_consistency(cfg);
    warn_settings(cfg, NULL);

    /* Each window on its own, over the file's settings. A window that clashes only
     * with another overlapping window isn't caught, but each on its own must work.
     * Skipped if the file itself is inconsistent, or every window repeats it. */
    if (s_errors == before && cfg->nschedule > 0) {
        config_t *open = malloc(sizeof(*open));
        if (open) {
            char where[64];
            for (int i = 0; i < cfg->nschedule; i++) {
                *open = *cfg;
                for (int j = 0; j < cfg->schedule[i].nsets; j++)
                    config_apply_override(open, &cfg->schedule[i].sets[j]);
                snprintf(where, sizeof(where), "schedule.%s", cfg->schedule[i].name);
                s_where = where;
                check_consistency(open);
                warn_settings(open, cfg);
                s_where = NULL;
            }
            free(open);
        }
    }

    check_schedule_lists(cfg);

    /* The count can be set by a window as well as the messages, so this only
     * warns when no combination could ever play one. */
    bool tail_used = ev->n_tail_messages > 0, tail_counted = cfg->timers.tail_every > 0;
    for (int i = 0; i < cfg->nschedule; i++)
        for (int j = 0; j < cfg->schedule[i].nsets; j++) {
            const config_override_t *o = &cfg->schedule[i].sets[j];
            if (o->offset == offsetof(config_t, events.tail_messages) && o->value[0])
                tail_used = true;
            if (o->offset == offsetof(config_t, timers.tail_every) && *(const int *)o->value > 0)
                tail_counted = true;
        }
    if (tail_used && !tail_counted)
        LOGW("config: a tail message is set, but timers.tail_every is 0, so it won't play\n");

    /* Not an error, as impolite_id covers it. Misspelling the key can't land here --
     * that's already an unknown-setting error. */
    if (ev->n_mandatory_ids == 0)
        LOGW("config: events.mandatory_ids is empty -- mandatory IDs will use events.impolite_id\n");
}

/* ── public ─────────────────────────────────────────────────────────────── */

int config_load(config_t *cfg, const char *path)
{
    config_defaults(cfg);
    s_errors = 0;
    s_line = 0;
    s_file = NULL;

    entries_t es = {0};
    if (read_entries(path, &es) != 0) {
        LOGE("config: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    read_settings(&es, cfg);
    check_callsign(&es, cfg);
    log_set_level(cfg->log.level);

    load_tone_catalog(cfg);
    load_builtin_messages(cfg);
    read_messages(&es, cfg, false);
    read_schedule(&es, cfg);

    validate(&es, cfg);
    entries_free(&es);
    return s_errors ? -1 : 0;
}

static bool message_in_use(const config_t *cfg, const char *name)
{
    for (size_t i = 0; i < NFIELDS; i++) {
        if (strcmp(FIELDS[i].section, "events") != 0)
            continue;
        const char (*names)[CFG_NAME_STR];
        int n = event_names(cfg, &FIELDS[i], &names);
        for (int j = 0; j < n; j++)
            if (strcmp(names[j], name) == 0)
                return true;
    }
    for (int i = 0; i < cfg->nschedule; i++)
        for (int j = 0; j < cfg->schedule[i].nsets; j++) {
            if (!cfg->schedule[i].sets[j].names)
                continue;
            char copy[CFG_STR];
            snprintf(copy, sizeof(copy), "%s", cfg->schedule[i].sets[j].value);
            char *save = NULL;
            for (char *n = strtok_r(copy, ",", &save); n; n = strtok_r(NULL, ",", &save))
                if (strcmp(n, name) == 0)
                    return true;
        }
    return false;
}

void config_prune_messages(config_t *cfg)
{
    int kept = 0;
    for (int i = 0; i < cfg->nmessages; i++) {
        if (!message_in_use(cfg, cfg->messages[i].name)) {
            LOGD("config: message '%s' isn't used, not keeping it\n", cfg->messages[i].name);
            continue;
        }
        if (kept != i)
            cfg->messages[kept] = cfg->messages[i];
        kept++;
    }
    cfg->nmessages = kept;
    if (kept > 0) {
        config_message_t *fit = realloc(cfg->messages, (size_t)kept * sizeof(*fit));
        if (fit) {
            cfg->messages = fit;
            cfg->messages_cap = kept;
        }
    }
}

void config_free(config_t *cfg)
{
    free(cfg->messages);
    cfg->messages = NULL;
    cfg->nmessages = 0;
    cfg->messages_cap = 0;
}

const config_message_t *config_find_message(const config_t *cfg, const char *name)
{
    if (!name || !name[0])
        return NULL;
    for (int i = 0; i < cfg->nmessages; i++) {
        if (strcmp(cfg->messages[i].name, name) == 0)
            return &cfg->messages[i];
    }
    return NULL;
}
