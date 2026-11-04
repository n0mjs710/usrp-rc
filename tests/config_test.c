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
    {"empty event = turned off",  BASE "[events]\nct_message =\n", NULL},
    {"comment after a value",     BASE "[timers]\nhang = 9   # seconds\n", NULL},
    {"sections in any order",     "[messages]\nm = cw: ID\n[events]\nct_message = m\n" BASE, NULL},
    {"catalog tone by name",      BASE "[events]\nct_message = yellow_jacket\n", NULL},
    {"silent tone",               BASE "[messages]\nm = tone: 0:30 _30\n", NULL},
    {"dual tone with level",      BASE "[messages]\nm = tone: 500+700:100@0.25\n", NULL},

    {"misspelled key",      BASE "[timers]\nhnag = 9.0\n", "line 4: timers.hnag: unknown setting (did you mean timers.hang?)"},
    {"misspelled section",  BASE "[timer]\nhang = 9.0\n", "line 3: [timer]: unknown section (did you mean [timers]?)"},
    {"key in wrong section", BASE "[audio]\nhang = 9.0\n", "line 4: audio.hang: unknown setting (did you mean timers.hang?)"},
    {"unknown top-level key", "wibble = 1\n" BASE, "line 1: wibble: unknown setting"},
    {"no equals sign",      BASE "[timers]\nhang 9\n", "line 4: expected key = value: hang 9"},
    {"unterminated section", "[timers\nhang = 1.0\n", "line 1: expected [section]"},
    {"key set twice",       BASE "[timers]\nhang = 1.0\nhang = 2.0\n", "line 5: hang: already set on line 4"},
    {"quoted value",        BASE "[events]\nct_message = \"honk\"\n", "line 4: ct_message: values don't take quotes"},
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
    {"variable in cw",      "callsign = W1XYZ\n" BASE "[messages]\na = cw: {time}\n", "only {call} works in cw"},
    {"variable joined to a word", BASE "[messages]\na = voice: {time}Z\n", "unknown placeholder '{time}Z'"},
    {"dtmf code uses D",    BASE "[dtmf]\ntime_code = 3D\n", "dtmf.time_code: '3D' can only use"},
    {"dtmf code used twice", BASE "[dtmf]\nlink_on_code = 39\n", "dtmf.link_on_code: '39' is already used by dtmf.time_code"},
    {"dtmf codes can be off", BASE "[dtmf]\ntime_code =\nlink_on_code =\nlink_off_code =\n", NULL},
    {"ct dangling",         BASE "[events]\nct_link_off_message = nope\n", "events.ct_link_off_message: no message named 'nope'"},

    {"misspelled mandatory_ids", BASE "[events]\nmandatory_id = honk\n", "events.mandatory_id: unknown setting (did you mean events.mandatory_ids?)"},
    {"mandatory_ids dangling",   BASE "[events]\nmandatory_ids = nope\n", "line 4: events.mandatory_ids: no message named 'nope'"},
    {"too many ids",        BASE "[events]\ninitial_ids = honk honk honk honk honk honk honk honk honk\n", "events.initial_ids: has 9 entries (max 8)"},
    {"event dangling, with hint", BASE "[events]\nct_message = yelow_jacket\n",
                                  "line 4: events.ct_message: no message named 'yelow_jacket' (did you mean 'yellow_jacket'?)"},

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
    int rc = load_text(BASE "[timers]\nhnag = 1.0\ntimeout = 0.0\n[events]\nct_message = nope\n");
    check("load fails", rc != 0);
    check("reports the unknown key", strstr(s_log, "line 4: timers.hnag") != NULL);
    check("reports the bad range", strstr(s_log, "line 5: timers.timeout") != NULL);
    check("reports the dangling reference", strstr(s_log, "line 7: events.ct_message") != NULL);

    printf("[3] values land where they should\n");
    rc = load_text(BASE "[timers]\nhang = 9\n");
    check("hang loaded", rc == 0 && s_cfg.timers.hang == 9.0);
    check("no mandatory IDs warns", strstr(s_log, "will not transmit an ID") != NULL);
    check("mmdvm ports default to the MMDVM-Host pairing",
          s_cfg.mmdvm.local_port == 4810 && s_cfg.mmdvm.rpt_port == 3810);

    rc = load_text(BASE "[events]\nmandatory_ids = a b\n[messages]\na = cw: ID\nb = cw: ID\n");
    check("name list splits on spaces", rc == 0 && s_cfg.events.n_mandatory_ids == 2
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
    check("all 28 tones and 3 built-in replies loaded", s_cfg.nmessages == 31);
    check("loud warning keeps its own level", element("timeout_warn", 0) && element("timeout_warn", 0)->amp == 0.9);

    rc = load_text(BASE "[messages]\nyellow_jacket = tone: 100:10\nmine = tone: 200:20\n");
    const config_message_t *over = config_find_message(&s_cfg, "yellow_jacket");
    check("same name replaces the catalog tone", rc == 0 && over && over->nelements == 1 && over->elements[0].freq1 == 100);
    check("replacing doesn't add a message, a new tone does", s_cfg.nmessages == 32);

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
            int n = message_var_clips(VOICE_VAR_TIME, &when, clips);
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
            message_var_clips(VOICE_VAR_AMPM, &when, clips);
            good &= strcmp(clips[0], PARTS[i].ampm) == 0;
            message_var_clips(VOICE_VAR_GREETING, &when, clips);
            good &= strcmp(clips[0], PARTS[i].greeting) == 0;
        }
        check("{ampm} and {greeting} change at the right hours", good);
    }

    load_text(BASE "[messages]\na = voice: REPEATER | cw: W1XYZ/R\n");
    check("valid content passes", message_validate(&s_cfg, vocab) == 0);

    vocab_cache_destroy(vocab);

    if (s_fail) {
        printf("\n==== %d check(s) FAILED ====\n", s_fail);
        return 1;
    }
    printf("\n==== all checks passed ====\n");
    return 0;
}
