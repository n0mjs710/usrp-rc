/* Config loader: every way of getting a config wrong must be reported, by
 * name and line, and fail the load. Each case runs a real file through
 * config_load with stderr captured so the message itself can be asserted on.
 *
 * Run from the repo root (usrp-rc.conf.sample, tests/harness.conf, tones.conf
 * and vocab_8k/ are relative paths). */
#include "../src/config.h"
#include "../src/message.h"
#include "../src/vocab.h"
#include "../src/log.h"
#include "../src/schedule.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BASE "[link]\nenabled = false\n"

typedef struct {
    const char *name;
    const char *conf;
    const char *expect;   /* NULL = must load cleanly; else must fail, logging this */
} test_case_t;

static const test_case_t CASES[] = {
    {"minimal config",            BASE, NULL},
    {"integer accepted for float", BASE "[timers]\nhang = 9\n", NULL},
    {"empty event = turned off",  BASE "[events]\nct_link_on_message =\n", NULL},
    {"comment after a value",     BASE "[timers]\nhang = 9   # seconds\n", NULL},
    {"sections in any order",     "[messages]\nm = cw: ID\n[events]\nct_link_on_message = m\n" BASE, NULL},
    {"catalog tone by name",      BASE "[events]\nct_link_on_message = yellow_jacket\n", NULL},
    {"silent tone",               BASE "[messages]\nm = tone: 0:30 _30\n", NULL},
    {"dual tone with level",      BASE "[messages]\nm = tone: 500+700:100@0.25\n", NULL},

    {"misspelled key",      BASE "[timers]\nhnag = 9.0\n", "line 4: timers.hnag: unknown setting (did you mean timers.hang?)"},
    {"misspelled section",  BASE "[timer]\nhang = 9.0\n", "line 3: [timer]: unknown section (did you mean [timers]?)"},
    {"key in wrong section", BASE "[audio]\nhang = 9.0\n", "line 4: audio.hang: unknown setting (did you mean timers.hang?)"},
    {"unknown top-level key", "wibble = 1\n" BASE, "line 1: wibble: unknown setting"},
    {"no equals sign",      BASE "[timers]\nhang 9\n", "line 4: expected key = value: hang 9"},
    {"unterminated section", "[timers\nhang = 1.0\n", "line 1: expected [section]"},
    {"key set twice",       BASE "[timers]\nhang = 1.0\nhang = 2.0\n", "line 5: hang: already set on line 4"},
    {"quoted value",        BASE "[events]\nct_link_on_message = \"honk\"\n", "line 4: ct_link_on_message: values don't take quotes"},
    {"old message section", BASE "[messages.a]\nelements = []\n", "old TOML layout"},

    {"text for number",     BASE "[timers]\nhang = fast\n", "timers.hang: 'fast' must be a number"},
    {"trailing junk",       BASE "[timers]\nhang = 9s\n", "timers.hang: '9s' must be a number"},
    {"float for int",       BASE "[audio]\nmorse_wpm = 20.5\n", "audio.morse_wpm: '20.5' must be a whole number"},
    {"text for bool",       BASE "[audio]\nvoice_filter = yes\n", "audio.voice_filter: 'yes' must be true or false"},
    {"bad port",            BASE "[mmdvm]\nlocal_port = 70000\n", "mmdvm.local_port: 70000 is not a valid port"},
    {"zero port",           BASE "[mmdvm]\nrpt_port = 0\n", "mmdvm.rpt_port: 0 is not a valid port"},
    {"bad codec",           BASE "codec = opsu\n", "link.codec: 'opsu' is not valid (expected one of: pcm, opus)"},
    {"bad access mode",     "access_mode = open\n" BASE, "access_mode: 'open' is not valid"},
    {"bad log level",       BASE "[log]\nlevel = loud\n", "log.level: 'loud' is not valid"},

    {"negative hang",       BASE "[timers]\nhang = -5.0\n", "timers.hang: -5 is out of range"},
    {"zero timeout",        BASE "[timers]\ntimeout = 0.0\n", "timers.timeout: 0 is out of range"},
    {"nan rejected",        BASE "[timers]\nhang = nan\n", "is out of range"},
    {"absurd gain",         BASE "[audio]\nmaster_gain = 50.0\n", "audio.master_gain: 50 is out of range"},
    {"negative wpm",        BASE "[audio]\nmorse_wpm = -3\n", "audio.morse_wpm: -3 is out of range"},
    {"anxious not before deadline", BASE "[messages]\nm = cw: ID\n[events]\nanxious_id = m\n[timers]\nid_interval = 60\nid_anxious = 60\n",
                                    "timers.id_anxious (60) must be less than"},
    {"missing remote_host", "[link]\nenabled = true\n", "link.remote_host is required"},
    {"bad opus frame",      "[link]\nremote_host = x\ncodec = opus\nopus_frame_ms = 30\n", "link.opus_frame_ms must be"},

    {"dtmf settings",       BASE "[dtmf]\nenabled = true\nmin_tone_ms = 50\npad_test_prefix = 85\n", NULL},
    {"dtmf prefix can be off", BASE "[dtmf]\npad_test_prefix =\n", NULL},
    {"dtmf prefix uses #",  BASE "[dtmf]\npad_test_prefix = 8#\n", "dtmf.pad_test_prefix: '8#' can only use"},
    {"dtmf prefix too long", BASE "[dtmf]\npad_test_prefix = 12345678\n", "dtmf.pad_test_prefix: is too long"},
    {"dtmf min tone range", BASE "[dtmf]\nmin_tone_ms = 5\n", "dtmf.min_tone_ms: 5 is out of range"},
    {"dtmf mute hang vs interdigit", BASE "[dtmf]\nmute_hang_ms = 4000\ninterdigit_ms = 4000\n",
                            "dtmf.mute_hang_ms (4000) must be less than dtmf.interdigit_ms (4000)"},

    {"voice variables",     BASE "[messages]\na = voice: F_THE F_TIME F_IS {time} {ampm} _ {greeting}\n", NULL},
    {"replacing a built-in reply", BASE "[messages]\nlink_on_reply = cw: LINK\n", NULL},
    {"silencing a reply",   BASE "[events]\nlink_off_message =\n", NULL},
    {"date and usage variables", BASE "[messages]\na = voice: {day} {date} {keyups_day} {keyups_week} {minutes_day} {minutes_week}\n", NULL},
    {"stats file setting",  BASE "[stats]\nfile = /tmp/x\n", NULL},
    {"stats file off",      BASE "[stats]\nfile =\n", NULL},
    {"variable in cw",      "callsign = W1XYZ\n" BASE "[messages]\na = cw: {time}\n", "only {call} works in cw"},
    {"variable joined to a word", BASE "[messages]\na = voice: {time}Z\n", "unknown placeholder '{time}Z'"},
    {"dtmf code uses D",    BASE "[dtmf]\ntime_code = 3D\n", "dtmf.time_code: '3D' can only use"},
    {"dtmf code used twice", BASE "[dtmf]\nlink_on_code = 39\n", "dtmf.link_on_code: '39' is already used by dtmf.time_code"},
    {"dtmf codes can be off", BASE "[dtmf]\ntime_code =\nlink_on_code =\nlink_off_code =\n", NULL},
    {"ct dangling",         BASE "[events]\nct_link_off_message = nope\n", "events.ct_link_off_message: no message named 'nope'"},

    {"old ct_message name", BASE "[events]\nct_message = honk\n", "events.ct_message: renamed to events.ct_link_on_message"},
    {"tail settings",       BASE "[messages]\nm = cw: NET\n[events]\ntail_message = m\n[timers]\ntail_every = 5\ntail_after_hang = 300\n", NULL},
    {"tail message dangling", BASE "[events]\ntail_message = nope\n", "events.tail_message: no message named 'nope'"},

    {"schedule entry",      BASE "[messages]\nm = cw: NET\n[schedule]\nnet = wed 07:00-19:25 tail_message=m\n", NULL},
    {"schedule day lists",  BASE "[messages]\nm = cw: NET\n[schedule]\na = mon,wed,fri 07:00-08:00 tail_message=m\nb = weekends 07:00-08:00 tail_message=m\nc = DAILY 07:00-08:00 tail_message=\n", NULL},
    {"schedule overnight",  BASE "[messages]\nm = cw: NET\n[schedule]\na = fri 22:00-02:00 ct_link_on_message=m\n", NULL},
    {"schedule list value", BASE "[messages]\nm = cw: A\nn = cw: B\n[schedule]\na = mon 01:00-02:00 mandatory_ids=m,n\n", NULL},
    {"schedule bad day",    BASE "[schedule]\na = wednsday 07:00-08:00 ct_link_on_message=\n", "schedule.a: 'wednsday' is not valid days"},
    {"schedule bad time",   BASE "[schedule]\na = wed 25:00-26:00 ct_link_on_message=\n", "schedule.a: '25:00-26:00' is not a valid time window"},
    {"schedule empty window", BASE "[schedule]\na = wed 07:00-07:00 ct_link_on_message=\n", "starts and ends at the same time"},
    {"schedule no settings", BASE "[schedule]\na = wed 07:00-08:00\n", "schedule.a: changes nothing"},
    {"schedule too short",  BASE "[schedule]\na = wed\n", "schedule.a: expected DAYS HH:MM-HH:MM"},
    {"schedule not a setting", BASE "[schedule]\na = wed 07:00-08:00 hang=3\n", "schedule.a: 'hang' can't be scheduled"},
    {"schedule startup",    BASE "[schedule]\na = wed 07:00-08:00 startup_message=\n", "'startup_message' can't be scheduled"},
    {"schedule no equals",  BASE "[schedule]\na = wed 07:00-08:00 tail_message\n", "expected setting=value"},
    {"schedule dangling, with hint", BASE "[schedule]\na = wed 07:00-08:00 ct_link_on_message=yelow_jacket\n", "no message named 'yelow_jacket' (did you mean 'yellow_jacket'?)"},
    {"schedule single takes one", BASE "[messages]\nm = cw: A\n[schedule]\na = mon 01:00-02:00 tail_message=m,m\n", "tail_message takes at most 1 name"},

    {"misspelled mandatory_ids", BASE "[events]\nmandatory_id = honk\n", "events.mandatory_id: unknown setting (did you mean events.mandatory_ids?)"},
    {"mandatory_ids dangling",   BASE "[events]\nmandatory_ids = nope\n", "line 4: events.mandatory_ids: no message named 'nope'"},
    {"list without commas", BASE "[events]\ninitial_ids = honk honk\n", "events.initial_ids: 'honk honk' needs commas"},
    {"too many ids",        BASE "[events]\ninitial_ids = honk,honk,honk,honk,honk,honk,honk,honk,honk\n", "events.initial_ids: has 9 entries (max 8)"},
    {"event dangling, with hint", BASE "[events]\nct_link_on_message = yelow_jacket\n",
                                  "line 4: events.ct_link_on_message: no message named 'yelow_jacket' (did you mean 'yellow_jacket'?)"},

    {"missing tone frequency", BASE "[messages]\na = tone: :100\n", "bad tone ':100'"},
    {"tone wrong separator", BASE "[messages]\na = tone: 660;50\n", "line 4: messages.a: bad tone '660;50'"},
    {"tone missing ms",     BASE "[messages]\na = tone: 660\n", "bad tone '660'"},
    {"tone trailing junk",  BASE "[messages]\na = tone: 660:50ms\n", "bad tone '660:50ms'"},
    {"tone bare underscore", BASE "[messages]\na = tone: _\n", "bad tone '_'"},
    {"tone level range",    BASE "[messages]\na = tone: 800:100@7\n", "level is out of range"},
    {"tone negative level", BASE "[messages]\na = tone: 800:100@-1\n", "level is out of range"},
    {"tone negative ms",    BASE "[messages]\na = tone: 800:-100\n", "duration is out of range"},
    {"tone above Nyquist",  BASE "[messages]\na = tone: 5000:100\n", "frequency is out of range"},
    {"empty tone",          BASE "[messages]\na = tone:\n", "tone needs at least one note"},
    {"too many tones",      BASE "[messages]\na = tone: 1:1 1:1 1:1 1:1 1:1 1:1 1:1 1:1 1:1 1:1 1:1 1:1 1:1 1:1 1:1 1:1 1:1 1:1 1:1 1:1 1:1 1:1 1:1 1:1 1:1 1:1 1:1 1:1 1:1 1:1 1:1 1:1 1:1\n",
                            "more than 32 elements"},
    {"unknown part type",   BASE "[messages]\na = tnoe: 800:100\n", "unknown type 'tnoe' (did you mean tone?)"},
    {"type without a colon", BASE "[messages]\na = voice REPEATER\n", "line 4: messages.a: 'voice' needs a colon after it, e.g. voice: ..."},
    {"tone without a colon", BASE "[messages]\na = tone 660:50\n", "'tone' needs a colon after it"},
    {"later part without a colon", BASE "[messages]\na = cw: ID | voice REPEATER\n", "'voice' needs a colon after it"},
    {"no type at all",      BASE "[messages]\na = REPEATER\n", "expected cw:, voice:, or tone: before 'REPEATER'"},
    {"space before the colon is fine", BASE "[messages]\na = voice : REPEATER\n", NULL},
    {"message has no content", BASE "[messages]\na =\n", "messages.a: has no content"},
    {"empty part",          BASE "[messages]\na = cw: ID |\n", "empty part"},
    {"cw without text",     BASE "[messages]\na = cw:\n", "cw needs text"},
    {"cw too long",         BASE "[messages]\na = cw: AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n", "cw text is too long"},
    {"voice without words", BASE "[messages]\na = voice:\n", "voice needs at least one word"},
    {"long message name",   BASE "[messages]\naaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa = cw: X\n", "name is too long"},
    {"long clip name",      BASE "[messages]\na = voice: AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n", "is too long"},
    {"too many voice words", BASE "[messages]\na = voice: A A A A A A A A A A A A A A A A A A A A A A A A A A A A A A A A A\n", "more than 32 words"},

    {"{call} with no callsign", BASE "[messages]\na = cw: {call}\n", "uses {call} but no callsign is set"},
    {"voice {call} with no callsign", BASE "[messages]\na = voice: {call}\n", "uses {call} but no callsign is set"},
    {"callsign with a suffix", "callsign = W1XYZ/R\n" BASE, "line 1: callsign: 'W1XYZ/R' must be letters and digits only"},
    {"callsign too long",   "callsign = AAAAAAAAAAAAAAAAAAAA\n" BASE, "callsign: is too long"},
    {"unknown cw placeholder", "callsign = W1XYZ\n" BASE "[messages]\na = cw: {cal}\n", "unknown placeholder"},
    {"unknown voice placeholder", "callsign = W1XYZ\n" BASE "[messages]\na = voice: {cal}\n", "unknown placeholder"},
    {"{call} joined to a voice word", "callsign = W1XYZ\n" BASE "[messages]\na = voice: {call}/R\n", "unknown placeholder"},
};

static const char *CONF_TMP = "build/tests/config_test.conf";
static const char *LOG_TMP  = "build/tests/config_test.log";

static config_t s_cfg;   /* too large for the stack */
static char     s_log[8192];

static void write_file(const char *path, const char *text)
{
    FILE *fp = fopen(path, "w");
    if (!fp) {
        perror(path);
        exit(1);
    }
    fputs(text, fp);
    fclose(fp);
}

/* config_load with stderr captured into s_log. */
static int capture_load(const char *path)
{
    fflush(stderr);
    int saved = dup(STDERR_FILENO);
    if (!freopen(LOG_TMP, "w", stderr))
        exit(1);
    g_log_level = LOG_LEVEL_INFO;
    int rc = config_load(&s_cfg, path);
    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);

    FILE *fp = fopen(LOG_TMP, "r");
    size_t n = fp ? fread(s_log, 1, sizeof(s_log) - 1, fp) : 0;
    s_log[n] = '\0';
    if (fp)
        fclose(fp);
    return rc;
}

static int load_text(const char *text)
{
    write_file(CONF_TMP, text);
    return capture_load(CONF_TMP);
}

static int s_fail;

static void check(const char *name, int ok)
{
    printf("  %-4s - %s\n", ok ? "ok" : "FAIL", name);
    if (!ok)
        s_fail++;
}

static const config_elem_t *element(const char *message, int i)
{
    const config_message_t *m = config_find_message(&s_cfg, message);
    return m && i < m->nelements ? &m->elements[i] : NULL;
}

int main(void)
{
    printf("[1] each config fails and names the problem, or loads cleanly\n");
    for (size_t i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        const test_case_t *c = &CASES[i];
        int rc = load_text(c->conf);
        int ok = c->expect ? (rc != 0 && strstr(s_log, c->expect)) : rc == 0;
        check(c->name, ok);
        if (!ok)
            printf("         rc=%d log: %s\n", rc, s_log);
    }

    printf("[2] one pass reports every problem, not just the first\n");
    int rc = load_text(BASE "[timers]\nhnag = 1.0\ntimeout = 0.0\n[events]\nct_link_on_message = nope\n");
    check("load fails", rc != 0);
    check("reports the unknown key", strstr(s_log, "line 4: timers.hnag") != NULL);
    check("reports the bad range", strstr(s_log, "line 5: timers.timeout") != NULL);
    check("reports the dangling reference", strstr(s_log, "line 7: events.ct_link_on_message") != NULL);

    printf("[3] values land where they should\n");
    rc = load_text(BASE "[timers]\nhang = 9\n");
    check("hang loaded", rc == 0 && s_cfg.timers.hang == 9.0);
    check("no mandatory IDs warns", strstr(s_log, "will not transmit an ID") != NULL);
    check("mmdvm ports default to the MMDVM-Host pairing",
          s_cfg.mmdvm.local_port == 4810 && s_cfg.mmdvm.rpt_port == 3810);

    rc = load_text(BASE "[events]\nmandatory_ids = a, b\n[messages]\na = cw: ID\nb = cw: ID\n");
    check("name list splits on commas", rc == 0 && s_cfg.events.n_mandatory_ids == 2
          && strcmp(s_cfg.events.mandatory_ids[1], "b") == 0);

    rc = load_text(BASE "[messages]\nm = tone: 500+700:100 _30 1200:60@0.9\n");
    const config_elem_t *e0 = element("m", 0), *e1 = element("m", 1), *e2 = element("m", 2);
    check("dual tone", rc == 0 && e0 && e0->freq1 == 500 && e0->freq2 == 700 && e0->ms == 100 && e0->amp < 0);
    check("silence", e1 && e1->freq1 == 0 && e1->freq2 == 0 && e1->ms == 30 && e1->amp == 0);
    check("per-note level", e2 && e2->freq1 == 1200 && e2->amp == 0.9);

    rc = load_text(BASE "[messages]\nm = voice: REPEATER _ _400 | tone: 700:50 900:50 | cw: HI  THERE\n");
    const config_message_t *mixed = config_find_message(&s_cfg, "m");
    check("| joins parts, each note is an element", rc == 0 && mixed && mixed->nelements == 4);
    e0 = element("m", 0);
    check("voice words and pauses", e0 && e0->type == ELEM_VOICE && e0->n_voice_words == 3
          && e0->voice_words[1].is_gap && e0->voice_words[1].gap_ms == -1
          && e0->voice_words[2].gap_ms == 400);
    e2 = element("m", 3);
    check("cw keeps its spaces", e2 && e2->type == ELEM_CW && strcmp(e2->cw_text, "HI  THERE") == 0);

    rc = load_text(BASE "[messages]\nm = cw: DE W1XYZ: QSL\n");
    e0 = element("m", 0);
    check("cw text can contain a colon", rc == 0 && e0 && strcmp(e0->cw_text, "DE W1XYZ: QSL") == 0);

    printf("[4] {call} is the bare callsign\n");
    rc = load_text("callsign = w1xyz\n" BASE "[messages]\nc = cw: {call}/R DE {call}\nv = voice: THISIS {call} REPEATER\n");
    check("loads", rc == 0);
    e0 = element("c", 0);
    check("cw substitutes, suffix comes from the message", e0 && strcmp(e0->cw_text, "W1XYZ/R DE W1XYZ") == 0);
    e1 = element("v", 0);
    static const char *const SPOKEN[] = {"THISIS", "W", "ONE", "X", "Y", "Z", "REPEATER"};
    int spoken = e1 && e1->n_voice_words == 7;
    for (int i = 0; spoken && i < 7; i++)
        spoken = strcmp(e1->voice_words[i].clip, SPOKEN[i]) == 0;
    check("voice spells it out, suffix comes from the message", spoken);

    printf("[5] tone catalog\n");
    rc = load_text(BASE);
    const config_elem_t *catalog = element("yellow_jacket", 2);
    check("available without being in the config", rc == 0 && catalog && catalog->freq1 == 660 && catalog->ms == 50);
    check("all 28 tones and 4 built-in replies loaded", s_cfg.nmessages == 32);
    check("loud warning keeps its own level", element("timeout_warn", 0) && element("timeout_warn", 0)->amp == 0.9);

    rc = load_text(BASE "[messages]\nyellow_jacket = tone: 100:10\nmine = tone: 200:20\n");
    const config_message_t *over = config_find_message(&s_cfg, "yellow_jacket");
    check("same name replaces the catalog tone", rc == 0 && over && over->nelements == 1 && over->elements[0].freq1 == 100);
    check("replacing doesn't add a message, a new tone does", s_cfg.nmessages == 33);

    printf("[6] shipped configs load\n");
    check("usrp-rc.conf.sample", capture_load("usrp-rc.conf.sample") == 0);
    check("tests/harness.conf", capture_load("tests/harness.conf") == 0);
    check("an old .toml path is refused with directions",
          capture_load("usrp-rc.toml") != 0 && strstr(s_log, "tools/toml2conf.py"));

    printf("[7] message content is checked against the vocabulary\n");
    vocab_cache_t *vocab;
    const char *dirs[] = { "vocab_8k" };
    if (vocab_cache_create(&vocab, dirs, 1) != 0)
        return 1;

    load_text(BASE "[messages]\na = voice: REPEATER _ _400 NOT_A_CLIP | cw: W1XYZ/R\n");
    check("exactly the missing clip is reported", message_validate(&s_cfg, vocab) == 1);

    load_text(BASE "[messages]\na = cw: W1XYZ~\n");
    check("unsendable CW character is reported", message_validate(&s_cfg, vocab) == 1);

    /* Every letter and digit a {call} can expand to has to exist as a clip. */
    load_text("callsign = ABCDEFGHIJKLM\n" BASE "[messages]\na = voice: {call}\n");
    check("voice {call} clips exist (A-M)", message_validate(&s_cfg, vocab) == 0);
    load_text("callsign = NOPQRSTUVWXYZ\n" BASE "[messages]\na = voice: {call}\n");
    check("voice {call} clips exist (N-Z)", message_validate(&s_cfg, vocab) == 0);
    load_text("callsign = 0123456789\n" BASE "[messages]\na = voice: {call}\n");
    check("voice {call} clips exist (digits)", message_validate(&s_cfg, vocab) == 0);

    load_text(BASE);
    check("the built-in replies' clips and variables all exist", message_validate(&s_cfg, vocab) == 0);

    {
        message_stats_t stats = {0};
        static const struct { int h, m; const char *want; } TIMES[] = {
            {0, 0,   "F_TWELVE F_OCLOCK"},
            {9, 5,   "F_NINE F_OH F_FIVE"},
            {9, 15,  "F_NINE F_FIFTEEN"},
            {11, 10, "F_ELEVEN F_TEN"},
            {12, 0,  "F_TWELVE F_OCLOCK"},
            {13, 20, "F_ONE F_TWENTY"},
            {14, 42, "F_TWO F_FORTY F_TWO"},
            {23, 59, "F_ELEVEN F_FIFTY F_NINE"},
        };
        int good = 1;
        for (size_t i = 0; i < sizeof(TIMES) / sizeof(TIMES[0]); i++) {
            struct tm when = {.tm_hour = TIMES[i].h, .tm_min = TIMES[i].m};
            const char *clips[MESSAGE_VAR_MAX_CLIPS];
            int n = message_var_clips(VOICE_VAR_TIME, &when, &stats, clips);
            char got[64] = "";
            for (int c = 0; c < n; c++) {
                strcat(got, c ? " " : "");
                strcat(got, clips[c]);
            }
            if (strcmp(got, TIMES[i].want) != 0) {
                printf("  %02d:%02d spoke '%s', wanted '%s'\n", TIMES[i].h, TIMES[i].m, got, TIMES[i].want);
                good = 0;
            }
        }
        check("{time} speaks the right words", good);

        static const struct { int h; const char *ampm, *greeting; } PARTS[] = {
            {0, "F_A_M", "F_MORNING"}, {11, "F_A_M", "F_MORNING"}, {12, "F_P_M", "F_AFTERNOON"},
            {17, "F_P_M", "F_AFTERNOON"}, {18, "F_P_M", "F_EVENING"}, {23, "F_P_M", "F_EVENING"},
        };
        good = 1;
        for (size_t i = 0; i < sizeof(PARTS) / sizeof(PARTS[0]); i++) {
            struct tm when = {.tm_hour = PARTS[i].h};
            const char *clips[MESSAGE_VAR_MAX_CLIPS];
            message_var_clips(VOICE_VAR_AMPM, &when, &stats, clips);
            good &= strcmp(clips[0], PARTS[i].ampm) == 0;
            message_var_clips(VOICE_VAR_GREETING, &when, &stats, clips);
            good &= strcmp(clips[0], PARTS[i].greeting) == 0;
        }
        check("{ampm} and {greeting} change at the right hours", good);

        static const struct { long v; const char *want; } NUMS[] = {
            {0, "ZERO"}, {7, "SEVEN"}, {13, "THIRTEEN"}, {20, "TWENTY"}, {42, "FORTY TWO"},
            {100, "ONE HUNDRED"}, {305, "THREE HUNDRED FIVE"}, {1000, "ONE THOUSAND"},
            {1010, "ONE THOUSAND TEN"}, {2345, "TWO THOUSAND THREE HUNDRED FORTY FIVE"},
            {99999, "NINETY NINE THOUSAND NINE HUNDRED NINETY NINE"},
            {250000, "NINETY NINE THOUSAND NINE HUNDRED NINETY NINE"},
        };
        good = 1;
        for (size_t i = 0; i < sizeof(NUMS) / sizeof(NUMS[0]); i++) {
            struct tm when = {0};
            message_stats_t st = {.keyups_week = NUMS[i].v};
            const char *clips[MESSAGE_VAR_MAX_CLIPS];
            int n = message_var_clips(VOICE_VAR_KEYUPS_WEEK, &when, &st, clips);
            char got[96] = "";
            for (int c = 0; c < n; c++) {
                strcat(got, c ? " " : "");
                strcat(got, clips[c]);
            }
            if (strcmp(got, NUMS[i].want) != 0) {
                printf("  %ld spoke '%s', wanted '%s'\n", NUMS[i].v, got, NUMS[i].want);
                good = 0;
            }
        }
        check("numbers speak the right words", good);

        struct tm when = {.tm_wday = 3, .tm_mon = 10, .tm_mday = 24};
        const char *clips[MESSAGE_VAR_MAX_CLIPS];
        check("{day}", message_var_clips(VOICE_VAR_DAY, &when, &stats, clips) == 1
              && strcmp(clips[0], "WEDNESDAY") == 0);
        check("{date}", message_var_clips(VOICE_VAR_DATE, &when, &stats, clips) == 3
              && strcmp(clips[0], "NOVEMBER") == 0 && strcmp(clips[1], "TWENTY") == 0
              && strcmp(clips[2], "FOUR") == 0);
    }

    load_text(BASE "[messages]\na = voice: REPEATER | cw: W1XYZ/R\n");
    check("valid content passes", message_validate(&s_cfg, vocab) == 0);

    vocab_cache_destroy(vocab);

    printf("[8] schedule\n");
    load_text(BASE "[messages]\nm = cw: A\nn = cw: B\nq = cw: C\n[events]\nct_link_on_message = m\nmandatory_ids = m\n"
              "[schedule]\n"
              "net = wed 07:00-19:25 tail_message=n ct_link_on_message=n\n"
              "night = fri 22:00-02:00 ct_link_on_message=q mandatory_ids=n,q\n"
              "later = wed 19:00-20:00 ct_link_on_message=q\n");
    {
        enum { SUN, MON, TUE, WED, THU, FRI, SAT };
        struct tm t = {.tm_wday = WED, .tm_hour = 6, .tm_min = 59};
        config_events_t ev;
        bool active[CFG_MAX_SCHEDULE];

        schedule_evaluate(&s_cfg, &t, &ev, active);
        check("before the window, the config's events apply",
              !active[0] && !ev.tail_message[0] && strcmp(ev.ct_link_on_message, "m") == 0);

        t.tm_hour = 7; t.tm_min = 0;
        schedule_evaluate(&s_cfg, &t, &ev, active);
        check("the start minute is inside", active[0] && strcmp(ev.tail_message, "n") == 0
              && strcmp(ev.ct_link_on_message, "n") == 0);

        t.tm_hour = 19; t.tm_min = 10;
        schedule_evaluate(&s_cfg, &t, &ev, active);
        check("where windows overlap, the later entry wins",
              active[0] && active[2] && strcmp(ev.ct_link_on_message, "q") == 0 && strcmp(ev.tail_message, "n") == 0);

        t.tm_min = 25;
        schedule_evaluate(&s_cfg, &t, &ev, active);
        check("the end minute is outside", !active[0] && !ev.tail_message[0]);

        t.tm_wday = THU; t.tm_hour = 12; t.tm_min = 0;
        schedule_evaluate(&s_cfg, &t, &ev, active);
        check("other days are untouched", !active[0] && !active[1] && strcmp(ev.ct_link_on_message, "m") == 0);

        t.tm_wday = FRI; t.tm_hour = 23;
        schedule_evaluate(&s_cfg, &t, &ev, active);
        check("overnight: open late on its start day", active[1]);
        check("a list setting replaces the rotation",
              ev.n_mandatory_ids == 2 && strcmp(ev.mandatory_ids[1], "q") == 0);

        t.tm_wday = SAT; t.tm_hour = 1; t.tm_min = 59;
        schedule_evaluate(&s_cfg, &t, &ev, active);
        check("overnight: still open early the next day", active[1]);
        t.tm_hour = 2; t.tm_min = 0;
        schedule_evaluate(&s_cfg, &t, &ev, active);
        check("overnight: closed at the end", !active[1] && ev.n_mandatory_ids == 1);

        t.tm_wday = FRI; t.tm_hour = 1;
        schedule_evaluate(&s_cfg, &t, &ev, active);
        check("overnight: not open early on its own start day", !active[1]);

        t.tm_wday = SUN; t.tm_hour = 1;
        load_text(BASE "[messages]\nm = cw: A\n[schedule]\nx = sat 23:00-01:30 tail_message=m\n");
        schedule_evaluate(&s_cfg, &t, &ev, active);
        check("overnight wraps Saturday into Sunday", active[0]);
        (void)MON; (void)TUE;
    }

    if (s_fail) {
        printf("\n==== %d check(s) FAILED ====\n", s_fail);
        return 1;
    }
    printf("\n==== all checks passed ====\n");
    return 0;
}
