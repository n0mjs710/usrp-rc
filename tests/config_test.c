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
    {"text for bool",       BASE "[link]\nstart_on = yes\n", "link.start_on: 'yes' must be true or false"},
    {"removed voice_filter", BASE "[audio]\nvoice_filter = true\n", "audio.voice_filter: removed, delete this line"},
    {"bad port",            BASE "[mmdvm]\nlocal_port = 70000\n", "mmdvm.local_port: 70000 is not a valid port"},
    {"zero port",           BASE "[mmdvm]\nrpt_port = 0\n", "mmdvm.rpt_port: 0 is not a valid port"},
    {"bad codec",           BASE "codec = opsu\n", "link.codec: 'opsu' is not valid (expected one of: pcm, opus)"},
    {"bad access mode",     "access_mode = open\n" BASE, "access_mode: 'open' is not valid"},
    {"bad log level",       BASE "[log]\nlevel = loud\n", "log.level: 'loud' is not valid"},

    {"negative hang",       BASE "[timers]\nhang = -5.0\n", "timers.hang: -5 is out of range"},
    {"negative timeout",    BASE "[timers]\ntimeout = -1\n", "timers.timeout: -1 is out of range"},
    {"timeouts can be off", BASE "[timers]\ntimeout = 0\nlink_timeout = 0\n", NULL},
    {"link timeout range",  BASE "[timers]\nlink_timeout = 4000\n", "timers.link_timeout: 4000 is out of range"},
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
    {"date and usage variables", BASE "[messages]\na = voice: {day} {date} {year} {keyups_day} {keyups_week} {minutes_day} {minutes_week}\n", NULL},
    {"stats file setting",  BASE "[stats]\nfile = /tmp/x\n", NULL},
    {"stats file off",      BASE "[stats]\nfile =\n", NULL},
    {"variable in cw",      "callsign = W1XYZ\n" BASE "[messages]\na = cw: {time}\n", "only {call} works in cw"},
    {"variable joined to a word", BASE "[messages]\na = voice: {time}Z\n", "unknown placeholder '{time}Z'"},
    {"dtmf code uses D",    BASE "[dtmf]\ntime_code = 3D\n", "dtmf.time_code: '3D' can only use"},
    {"dtmf code used twice", BASE "[dtmf]\nlink_on_code = 39\n", "dtmf.link_on_code: '39' is already used by dtmf.time_code"},
    {"repeater code clash",  BASE "[dtmf]\nrepeater_code = 7\n", "dtmf.repeater_code: '70' is already used by dtmf.link_status_code"},
    {"repeater code",        BASE "[dtmf]\nrepeater_code = 11\n", NULL},
    {"dtmf codes can be off", BASE "[dtmf]\ntime_code =\nlink_on_code =\nlink_off_code =\n", NULL},
    {"ct dangling",         BASE "[events]\nct_link_off_message = nope\n", "events.ct_link_off_message: no message named 'nope'"},

    {"old ct_message name", BASE "[events]\nct_message = honk\n", "events.ct_message: renamed to events.ct_link_on_message"},
    {"tail settings",       BASE "[messages]\nm = cw: NET\n[events]\ntail_messages = m\n[timers]\ntail_every = 5\ntail_min_interval = 300\n", NULL},
    {"old tail_after_hang name", BASE "[timers]\ntail_after_hang = 300\n", "timers.tail_after_hang: replaced by timers.tail_min_interval"},
    {"old tail_message name", BASE "[events]\ntail_message = honk\n", "events.tail_message: renamed to events.tail_messages"},
    {"old tail_message in a schedule", BASE "[schedule]\na = mon 01:00-02:00 tail_message=honk\n", "tail_message is renamed to tail_messages"},
    {"tail message list", BASE "[messages]\nm = cw: A\nn = cw: B\n[events]\ntail_messages = m,n\n[schedule]\na = mon 01:00-02:00 tail_messages=n,m\n", NULL},
    {"tail message dangling", BASE "[events]\ntail_messages = nope\n", "events.tail_messages: no message named 'nope'"},

    {"schedule entry",      BASE "[messages]\nm = cw: NET\n[schedule]\nnet = wed 07:00-19:25 tail_messages=m\n", NULL},
    {"schedule day lists",  BASE "[messages]\nm = cw: NET\n[schedule]\na = mon,wed,fri 07:00-08:00 tail_messages=m\nb = weekends 07:00-08:00 tail_messages=m\nc = DAILY 07:00-08:00 tail_messages=\n", NULL},
    {"schedule overnight",  BASE "[messages]\nm = cw: NET\n[schedule]\na = fri 22:00-02:00 ct_link_on_message=m\n", NULL},
    {"schedule date rules", BASE "[messages]\nm = cw: A\n[schedule]\na = 2nd-tue,5th-sun,15th,31st,last-day,dec-8,feb-29,JUL-4 19:00-21:00 tail_messages=m\nb = mon,1st-fri 19:00-21:00 tail_messages=m\n", NULL},
    {"schedule 6th weekday", BASE "[messages]\nm = cw: A\n[schedule]\na = 6th-tue 19:00-21:00 tail_messages=m\n", "'6th-tue' is not valid days"},
    {"schedule 32nd", BASE "[messages]\nm = cw: A\n[schedule]\na = 32nd 19:00-21:00 tail_messages=m\n", "'32nd' is not valid days"},
    {"schedule wrong suffix", BASE "[messages]\nm = cw: A\n[schedule]\na = 2th-tue 19:00-21:00 tail_messages=m\n", "'2th-tue' is not valid days"},
    {"schedule 11st", BASE "[messages]\nm = cw: A\n[schedule]\na = 11st 19:00-21:00 tail_messages=m\n", "'11st' is not valid days"},
    {"schedule 0th", BASE "[messages]\nm = cw: A\n[schedule]\na = 0th 19:00-21:00 tail_messages=m\n", "'0th' is not valid days"},
    {"schedule bare number", BASE "[messages]\nm = cw: A\n[schedule]\na = 15 19:00-21:00 tail_messages=m\n", "'15' is not valid days"},
    {"schedule feb-30", BASE "[messages]\nm = cw: A\n[schedule]\na = feb-30 19:00-21:00 tail_messages=m\n", "'feb-30' is not valid days"},
    {"schedule apr-31", BASE "[messages]\nm = cw: A\n[schedule]\na = apr-31 19:00-21:00 tail_messages=m\n", "'apr-31' is not valid days"},
    {"schedule dec-0", BASE "[messages]\nm = cw: A\n[schedule]\na = dec-0 19:00-21:00 tail_messages=m\n", "'dec-0' is not valid days"},
    {"schedule dec-8x", BASE "[messages]\nm = cw: A\n[schedule]\na = dec-8x 19:00-21:00 tail_messages=m\n", "'dec-8x' is not valid days"},
    {"schedule last-fri", BASE "[messages]\nm = cw: A\n[schedule]\na = last-fri 19:00-21:00 tail_messages=m\n", "'last-fri' is not valid days"},
    {"schedule too many rules", BASE "[messages]\nm = cw: A\n[schedule]\na = 1st,2nd,3rd,4th,5th,6th,7th,8th,9th 19:00-21:00 tail_messages=m\n", "'1st,2nd,3rd,4th,5th,6th,7th,8th,9th' is not valid days"},
    {"impolite_id empty", BASE "[events]\nimpolite_id =\n", "events.impolite_id is required"},
    {"impolite_id empty in a window", BASE "[events]\nimpolite_id = honk\n[schedule]\na = mon 01:00-02:00 impolite_id=\n", "impolite_id can't be empty"},
    {"impolite_id changed in a window", BASE "[events]\nimpolite_id = honk\n[schedule]\na = mon 01:00-02:00 impolite_id=beep\n", NULL},
    {"schedule += and -=", BASE "[messages]\nm = cw: A\nn = cw: B\n[schedule]\na = mon 01:00-02:00 tail_messages+=m,n mandatory_ids-=m\n", NULL},
    {"schedule += on one message", BASE "[messages]\nm = cw: A\n[schedule]\na = mon 01:00-02:00 ct_link_on_message+=m\n", "ct_link_on_message takes one value, so it can't use += or -="},
    {"schedule -= on one message", BASE "[messages]\nm = cw: A\n[schedule]\na = mon 01:00-02:00 ct_link_on_message-=m\n", "ct_link_on_message takes one value, so it can't use += or -="},
    {"schedule += empty", BASE "[schedule]\na = mon 01:00-02:00 tail_messages+=\n", "tail_messages+= needs at least one message"},
    {"schedule -= empty", BASE "[schedule]\na = mon 01:00-02:00 mandatory_ids-=\n", "mandatory_ids-= needs at least one message"},
    {"schedule += dangling", BASE "[schedule]\na = mon 01:00-02:00 tail_messages+=nope\n", "no message named 'nope'"},
    {"schedule list value", BASE "[messages]\nm = cw: A\nn = cw: B\n[schedule]\na = mon 01:00-02:00 mandatory_ids=m,n\n", NULL},
    {"schedule bad day",    BASE "[schedule]\na = wednsday 07:00-08:00 ct_link_on_message=\n", "schedule.a: 'wednsday' is not valid days"},
    {"schedule bad time",   BASE "[schedule]\na = wed 25:00-26:00 ct_link_on_message=\n", "schedule.a: '25:00-26:00' is not a valid time window"},
    {"schedule empty window", BASE "[schedule]\na = wed 07:00-07:00 ct_link_on_message=\n", "starts and ends at the same time"},
    {"schedule no settings", BASE "[schedule]\na = wed 07:00-08:00\n", "schedule.a: changes nothing"},
    {"schedule too short",  BASE "[schedule]\na = wed\n", "schedule.a: expected DAYS HH:MM-HH:MM"},
    {"schedule a timer",    BASE "[schedule]\na = wed 07:00-08:00 hang=3 timeout=600 timers.ct_delay=0\n", NULL},
    {"schedule audio",      BASE "[schedule]\na = wed 07:00-08:00 morse_wpm=15 morse_pitch=600 master_gain=0.8\n", NULL},
    {"schedule dtmf",       BASE "[schedule]\na = wed 07:00-08:00 link_on_code= interdigit_ms=6000\n", NULL},
    {"schedule out of range", BASE "[schedule]\na = wed 07:00-08:00 hang=900\n", "schedule.a: hang: 900 is out of range (0 to 600)"},
    {"schedule not a number", BASE "[schedule]\na = wed 07:00-08:00 hang=long\n", "schedule.a: hang: 'long' must be a number"},
    {"schedule unknown",    BASE "[schedule]\na = wed 07:00-08:00 hnag=3\n", "schedule.a: 'hnag' is not a setting (did you mean hang?)"},
    {"schedule wrong section", BASE "[schedule]\na = wed 07:00-08:00 audio.hang=3\n", "schedule.a: 'audio.hang' is not a setting (did you mean timers.hang?)"},
    {"schedule ambiguous",  BASE "[schedule]\na = wed 07:00-08:00 enabled=false\n", "'enabled' is in more than one section; write it as section.enabled"},
    {"schedule beep",       BASE "[schedule]\na = wed 07:00-08:00 beep_hz=600\n", "dtmf.beep_hz can't be scheduled: it only takes effect at startup"},
    {"schedule startup only", BASE "[schedule]\na = wed 07:00-08:00 link.enabled=false\n", "schedule.a: link.enabled can't be scheduled: it only takes effect at startup"},
    {"schedule callsign",   BASE "[schedule]\na = wed 07:00-08:00 callsign=N0CALL\n", "callsign can't be scheduled: it only takes effect at startup"},
    {"schedule startup",    BASE "[schedule]\na = wed 07:00-08:00 startup_message=\n", "events.startup_message can't be scheduled: it only plays at startup"},
    {"schedule code clash", BASE "[schedule]\na = wed 07:00-08:00 time_code=38\n", "schedule.a: dtmf.stats_code: '38' is already used by dtmf.time_code"},
    {"schedule mute vs interdigit", BASE "[schedule]\na = wed 07:00-08:00 interdigit_ms=1000\n", "schedule.a: dtmf.mute_hang_ms (2000) must be less than dtmf.interdigit_ms (1000)"},
    {"schedule no equals",  BASE "[schedule]\na = wed 07:00-08:00 tail_messages\n", "expected setting=value"},
    {"schedule dangling, with hint", BASE "[schedule]\na = wed 07:00-08:00 ct_link_on_message=yelow_jacket\n", "no message named 'yelow_jacket' (did you mean 'yellow_jacket'?)"},
    {"schedule single takes one", BASE "[messages]\nm = cw: A\n[schedule]\na = mon 01:00-02:00 ct_link_on_message=m,m\n", "ct_link_on_message takes at most 1 name"},

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
    config_free(&s_cfg);
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

/* impolite_id is required, so add one unless the test is about it. At the end, to leave
 * the line numbers in expected messages alone. */
static int load_text(const char *text)
{
    static char full[8192];
    const char *extra = strstr(text, "impolite_id") ? "" : "\n[events]\nimpolite_id = honk\n";
    snprintf(full, sizeof(full), "%s%s", text, extra);
    write_file(CONF_TMP, full);
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
    int rc = load_text(BASE "[timers]\nhnag = 1.0\ntimeout = -1\n[events]\nct_link_on_message = nope\n");
    check("load fails", rc != 0);
    check("reports the unknown key", strstr(s_log, "line 4: timers.hnag") != NULL);
    check("reports the bad range", strstr(s_log, "line 5: timers.timeout") != NULL);
    check("reports the dangling reference", strstr(s_log, "line 7: events.ct_link_on_message") != NULL);

    printf("[3] values land where they should\n");
    rc = load_text(BASE "[timers]\nhang = 9\n");
    check("hang loaded", rc == 0 && s_cfg.timers.hang == 9.0);
    check("no mandatory IDs warns", strstr(s_log, "mandatory IDs will use events.impolite_id") != NULL);
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
    check("all 28 tones and 7 built-in replies loaded", s_cfg.nmessages == 35);
    check("loud warning keeps its own level", element("timeout_warn", 0) && element("timeout_warn", 0)->amp == 0.9);

    rc = load_text(BASE "[messages]\nyellow_jacket = tone: 100:10\nmine = tone: 200:20\n");
    const config_message_t *over = config_find_message(&s_cfg, "yellow_jacket");
    check("same name replaces the catalog tone", rc == 0 && over && over->nelements == 1 && over->elements[0].freq1 == 100);
    check("replacing doesn't add a message, a new tone does", s_cfg.nmessages == 36);

    printf("[5b] schedule settings and warnings\n");
    rc = load_text(BASE "[schedule]\nnet = wed 19:00-20:00 timeout=0\n");
    check("a window that turns the time-out off warns, naming it",
          rc == 0 && strstr(s_log, "schedule.net: timers.timeout is 0") != NULL);
    rc = load_text(BASE "[timers]\ntimeout = 0\n[schedule]\nnet = wed 19:00-20:00 hang=1\n");
    check("a window doesn't repeat the file's own warning",
          rc == 0 && strstr(s_log, "timers.timeout is 0") && !strstr(s_log, "schedule.net: timers.timeout"));
    rc = load_text(BASE "[messages]\nm = cw: A\n[events]\ntail_messages = m\n");
    check("tail_every defaults to 0, which warns when a tail message is set",
          rc == 0 && s_cfg.timers.tail_every == 0 && strstr(s_log, "tail_every is 0"));
    rc = load_text(BASE "[messages]\nm = cw: A\n[events]\ntail_messages = m\n[schedule]\nnight = daily 22:00-06:00 tail_every=5\n");
    check("... but not when a window sets the count", rc == 0 && !strstr(s_log, "tail_every is 0"));
    {
        static config_t run;
        bool active[CFG_MAX_SCHEDULE];
        struct tm t = {.tm_wday = 3, .tm_hour = 19, .tm_min = 30};
        load_text(BASE "[schedule]\nnet = wed 19:00-20:00 timeout=600 hang=1.5 morse_wpm=15 access_mode=cor link_on_code=\n");
        check("access_mode can't be scheduled", strstr(s_log, "access_mode can't be scheduled") != NULL);
        load_text(BASE "[schedule]\nnet = wed 19:00-20:00 timeout=600 hang=1.5 morse_wpm=15 link_on_code=\n");
        schedule_evaluate(&s_cfg, &t, &run, active);
        check("numbers and codes apply while the window is open",
              run.timers.timeout == 600.0 && run.timers.hang == 1.5 && run.audio.morse_wpm == 15
              && run.dtmf.link_on_code[0] == 0);
        check("the file's own settings are untouched",
              s_cfg.timers.timeout == 180.0 && s_cfg.audio.morse_wpm == 20 && strcmp(s_cfg.dtmf.link_on_code, "72") == 0);
        t.tm_hour = 20;
        schedule_evaluate(&s_cfg, &t, &run, active);
        check("and end with it", run.timers.timeout == 180.0 && run.timers.hang == s_cfg.timers.hang);
    }

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
        struct tm year = {.tm_year = 2026 - 1900};
        check("{year}", message_var_clips(VOICE_VAR_YEAR, &year, &stats, clips) == 4
              && strcmp(clips[0], "TWO") == 0 && strcmp(clips[1], "THOUSAND") == 0
              && strcmp(clips[2], "TWENTY") == 0 && strcmp(clips[3], "SIX") == 0);
    }

    load_text(BASE "[messages]\na = voice: REPEATER | cw: W1XYZ/R\n");
    check("valid content passes", message_validate(&s_cfg, vocab) == 0);

    vocab_cache_destroy(vocab);

    printf("[8] schedule\n");
    load_text(BASE "[messages]\nm = cw: A\nn = cw: B\nq = cw: C\n[events]\nct_link_on_message = m\nmandatory_ids = m\n"
              "[schedule]\n"
              "net = wed 07:00-19:25 tail_messages=n ct_link_on_message=n\n"
              "night = fri 22:00-02:00 ct_link_on_message=q mandatory_ids=n,q\n"
              "later = wed 19:00-20:00 ct_link_on_message=q\n");
    {
        enum { SUN, MON, TUE, WED, THU, FRI, SAT };
        struct tm t = {.tm_wday = WED, .tm_hour = 6, .tm_min = 59};
        static config_t run;
        bool active[CFG_MAX_SCHEDULE];

        schedule_evaluate(&s_cfg, &t, &run, active);
        check("before the window, the config's events apply",
              !active[0] && run.events.n_tail_messages == 0 && strcmp(run.events.ct_link_on_message, "m") == 0);

        t.tm_hour = 7; t.tm_min = 0;
        schedule_evaluate(&s_cfg, &t, &run, active);
        check("the start minute is inside", active[0] && strcmp(run.events.tail_messages[0], "n") == 0
              && strcmp(run.events.ct_link_on_message, "n") == 0);

        t.tm_hour = 19; t.tm_min = 10;
        schedule_evaluate(&s_cfg, &t, &run, active);
        check("where windows overlap, the later entry wins",
              active[0] && active[2] && strcmp(run.events.ct_link_on_message, "q") == 0 && strcmp(run.events.tail_messages[0], "n") == 0);

        t.tm_min = 25;
        schedule_evaluate(&s_cfg, &t, &run, active);
        check("the end minute is outside", !active[0] && run.events.n_tail_messages == 0);

        t.tm_wday = THU; t.tm_hour = 12; t.tm_min = 0;
        schedule_evaluate(&s_cfg, &t, &run, active);
        check("other days are untouched", !active[0] && !active[1] && strcmp(run.events.ct_link_on_message, "m") == 0);

        t.tm_wday = FRI; t.tm_hour = 23;
        schedule_evaluate(&s_cfg, &t, &run, active);
        check("overnight: open late on its start day", active[1]);
        check("a list setting replaces the rotation",
              run.events.n_mandatory_ids == 2 && strcmp(run.events.mandatory_ids[1], "q") == 0);

        t.tm_wday = SAT; t.tm_hour = 1; t.tm_min = 59;
        schedule_evaluate(&s_cfg, &t, &run, active);
        check("overnight: still open early the next day", active[1]);
        t.tm_hour = 2; t.tm_min = 0;
        schedule_evaluate(&s_cfg, &t, &run, active);
        check("overnight: closed at the end", !active[1] && run.events.n_mandatory_ids == 1);

        t.tm_wday = FRI; t.tm_hour = 1;
        schedule_evaluate(&s_cfg, &t, &run, active);
        check("overnight: not open early on its own start day", !active[1]);

        t.tm_wday = SUN; t.tm_hour = 1;
        load_text(BASE "[messages]\nm = cw: A\n[schedule]\nx = sat 23:00-01:30 tail_messages=m\n");
        schedule_evaluate(&s_cfg, &t, &run, active);
        check("overnight wraps Saturday into Sunday", active[0]);
        (void)MON; (void)TUE;
    }

    /* += and -=: every window here opens on Wednesday at 12:00, and they apply in file order. */
    {
        struct tm t = {.tm_wday = 3, .tm_hour = 12, .tm_min = 0};
        static config_t run;
        bool active[CFG_MAX_SCHEDULE];
        #define M "[messages]\na = cw: A\nb = cw: B\nc = cw: C\nd = cw: D\ne = cw: E\nf = cw: F\ng = cw: G\nh = cw: H\ni = cw: I\n"

        load_text(BASE M "[events]\ntail_messages = a\n[schedule]\nx = wed 00:00-23:59 tail_messages+=b\ny = wed 00:00-23:59 tail_messages+=c\n");
        schedule_evaluate(&s_cfg, &t, &run, active);
        check("+= builds on [events] and on earlier windows",
              run.events.n_tail_messages == 3 && strcmp(run.events.tail_messages[0], "a") == 0 && strcmp(run.events.tail_messages[1], "b") == 0 && strcmp(run.events.tail_messages[2], "c") == 0);
        check("the config's own list is untouched", s_cfg.events.n_tail_messages == 1);

        load_text(BASE M "[schedule]\nx = wed 00:00-23:59 tail_messages+=b\ny = 2nd-wed 00:00-23:59 tail_messages+=c\n");
        t.tm_year = 2026 - 1900; t.tm_mon = 10; t.tm_mday = 11;
        schedule_evaluate(&s_cfg, &t, &run, active);
        check("your Wednesday case: the 2nd Wednesday gets both", run.events.n_tail_messages == 2);
        t.tm_mday = 4;
        schedule_evaluate(&s_cfg, &t, &run, active);
        check("any other Wednesday gets the weekly one", run.events.n_tail_messages == 1 && strcmp(run.events.tail_messages[0], "b") == 0);

        load_text(BASE M "[events]\ntail_messages = a\n[schedule]\nx = wed 00:00-23:59 tail_messages+=a\n");
        schedule_evaluate(&s_cfg, &t, &run, active);
        check("+= doesn't add a name already there", run.events.n_tail_messages == 1);

        load_text(BASE M "[events]\ntail_messages = a,b,c\n[schedule]\nx = wed 00:00-23:59 tail_messages-=b\n");
        schedule_evaluate(&s_cfg, &t, &run, active);
        check("-= removes a name and keeps the order",
              run.events.n_tail_messages == 2 && strcmp(run.events.tail_messages[0], "a") == 0 && strcmp(run.events.tail_messages[1], "c") == 0);

        load_text(BASE M "[events]\ntail_messages = a,b\n[schedule]\nx = wed 00:00-23:59 tail_messages-=c\n");
        schedule_evaluate(&s_cfg, &t, &run, active);
        check("-= of a name not in the list changes nothing", run.events.n_tail_messages == 2);

        load_text(BASE M "[events]\ntail_messages = a,b\n[schedule]\nx = wed 00:00-23:59 tail_messages-=a,b\n");
        schedule_evaluate(&s_cfg, &t, &run, active);
        check("-= can empty an ordinary list", run.events.n_tail_messages == 0);

        load_text(BASE M "[events]\nmandatory_ids = a,b\n[schedule]\nx = wed 00:00-23:59 mandatory_ids-=a\n");
        schedule_evaluate(&s_cfg, &t, &run, active);
        check("-= takes one ID out of the rotation",
              run.events.n_mandatory_ids == 1 && strcmp(run.events.mandatory_ids[0], "b") == 0);

        load_text(BASE M "[events]\nmandatory_ids = a,b\n[schedule]\nx = wed 00:00-23:59 mandatory_ids-=a,b\n");
        schedule_evaluate(&s_cfg, &t, &run, active);
        check("-= can't remove the last mandatory ID",
              run.events.n_mandatory_ids == 1 && strcmp(run.events.mandatory_ids[0], "b") == 0);

        load_text(BASE M "[events]\ntail_messages = a,b,c,d,e,f,g,h\n[schedule]\nx = wed 00:00-23:59 tail_messages+=i\n");
        schedule_evaluate(&s_cfg, &t, &run, active);
        check("+= stops at the list's limit", run.events.n_tail_messages == 8 && strcmp(run.events.tail_messages[7], "h") == 0);

        load_text(BASE M "[events]\ntail_messages = a\n[schedule]\nx = wed 00:00-23:59 tail_messages+=b\ny = wed 00:00-23:59 tail_messages=c\n");
        schedule_evaluate(&s_cfg, &t, &run, active);
        check("a later = replaces what += built", run.events.n_tail_messages == 1 && strcmp(run.events.tail_messages[0], "c") == 0);

        t.tm_wday = 4;
        load_text(BASE M "[events]\ntail_messages = a\n[schedule]\nx = wed 00:00-23:59 tail_messages+=b\n");
        schedule_evaluate(&s_cfg, &t, &run, active);
        check("a closed window changes nothing", run.events.n_tail_messages == 1);
        #undef M
    }

    /* Rules against a real calendar. timegm and gmtime_r supply the dates, so this
     * doesn't share any arithmetic with what it's checking. */
    {
        load_text(BASE "[messages]\nm = cw: A\n[schedule]\n"
                  "nth = 2nd-tue 19:00-21:00 tail_messages=m\n"
                  "fifth = 5th-tue 19:00-21:00 tail_messages=m\n"
                  "mid = 15th 19:00-21:00 tail_messages=m\n"
                  "d31 = 31st 19:00-21:00 tail_messages=m\n"
                  "last = last-day 19:00-21:00 tail_messages=m\n"
                  "date = dec-8 19:00-21:00 tail_messages=m\n"
                  "leap = feb-29 19:00-21:00 tail_messages=m\n"
                  "late = last-day 22:00-02:00 tail_messages=m\n");
        enum { NTH, FIFTH, MID, D31, LAST, DATE, LEAP, LATE };
        int hits[8] = {0}, bad_nth = 0, bad_last = 0, bad_late = 0;
        static config_t run;
        bool active[CFG_MAX_SCHEDULE];
        struct tm start = {.tm_year = 2024 - 1900, .tm_mon = 0, .tm_mday = 1};
        time_t t = timegm(&start);
        for (; ; t += 86400) {
            struct tm d, next;
            gmtime_r(&t, &d);
            if (d.tm_year == 2029 - 1900)
                break;
            time_t tn = t + 86400;
            gmtime_r(&tn, &next);
            d.tm_hour = 20;
            d.tm_min = 0;
            schedule_evaluate(&s_cfg, &d, &run, active);
            for (int i = NTH; i <= LEAP; i++)
                hits[i] += active[i];
            if (active[NTH] != (d.tm_wday == 2 && d.tm_mday >= 8 && d.tm_mday <= 14))
                bad_nth++;
            if (active[LAST] != (next.tm_mday == 1))
                bad_last++;
            /* 01:00 on the 1st belongs to the night before; 01:00 on any other day doesn't. */
            d.tm_hour = 1;
            schedule_evaluate(&s_cfg, &d, &run, active);
            if (active[LATE] != (d.tm_mday == 1))
                bad_late++;
            d.tm_hour = 2;
            schedule_evaluate(&s_cfg, &d, &run, active);
            if (active[LATE])
                bad_late++;
        }
        check("2nd-tue: only those days, 60 in 5 years", bad_nth == 0 && hits[NTH] == 60);
        check("5th-tue: only in months that have one", hits[FIFTH] == 21);
        check("15th: every month", hits[MID] == 60);
        check("31st: 7 months a year", hits[D31] == 35);
        check("last-day: every month, on the right day", bad_last == 0 && hits[LAST] == 60);
        check("dec-8: once a year", hits[DATE] == 5);
        check("feb-29: 2024 and 2028 only", hits[LEAP] == 2);
        check("last-day overnight carries into the 1st", bad_late == 0);
    }

    printf("[10] only messages that can play are kept\n");
    {
        static const char *const KEYS[] = {
            "startup_message", "initial_ids", "mandatory_ids", "anxious_id", "impolite_id",
            "ct_link_on_message", "ct_link_message", "ct_link_off_message", "tail_messages",
            "time_message", "stats_message", "link_on_message", "link_off_message", "link_none_message",
            "repeater_up_message", "repeater_down_message",
            "timeout_message", "timeout_cancel_message",
        };
        #define MSGS BASE "[messages]\nkeep = cw: A\ndrop = cw: B\n"
        char conf[1024], what[128];
        for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
            snprintf(conf, sizeof(conf), MSGS "[events]\n%s = keep\n", KEYS[i]);
            int rc = load_text(conf);
            config_prune_messages(&s_cfg);
            snprintf(what, sizeof(what), "[events] %s", KEYS[i]);
            check(what, rc == 0 && config_find_message(&s_cfg, "keep") && !config_find_message(&s_cfg, "drop"));

            if (strcmp(KEYS[i], "startup_message") == 0)
                continue;   /* can't be scheduled */
            snprintf(conf, sizeof(conf), MSGS "[events]\nimpolite_id = honk\n"
                     "[schedule]\nw = daily 00:00-01:00 %s=keep\n", KEYS[i]);
            rc = load_text(conf);
            config_prune_messages(&s_cfg);
            snprintf(what, sizeof(what), "[schedule] %s=", KEYS[i]);
            check(what, rc == 0 && config_find_message(&s_cfg, "keep") && !config_find_message(&s_cfg, "drop"));
        }

        int rc = load_text(MSGS "[messages]\nkeep2 = cw: C\n[events]\nimpolite_id = honk\n"
                           "[schedule]\nw = daily 00:00-01:00 tail_messages+=keep,keep2\n");
        config_prune_messages(&s_cfg);
        check("every name in a += list", rc == 0 && config_find_message(&s_cfg, "keep")
              && config_find_message(&s_cfg, "keep2") && !config_find_message(&s_cfg, "drop"));

        rc = load_text(BASE "[events]\nimpolite_id = yellow_jacket\n");
        config_prune_messages(&s_cfg);
        check("a catalog tone is kept only if assigned", rc == 0 && config_find_message(&s_cfg, "yellow_jacket")
              && !config_find_message(&s_cfg, "bumble_bee"));
        check("a default event's built-in reply is kept", config_find_message(&s_cfg, "time_readback") != NULL);
        check("and the rendered message still works", message_render(&s_cfg, vocab, "yellow_jacket", -1.0).n == 1200);

        char many[4096] = BASE "[messages]\n";
        for (int i = 0; i < 70; i++)
            snprintf(many + strlen(many), sizeof(many) - strlen(many), "m%d = cw: A\n", i);
        check("no limit on how many are defined", load_text(many) == 0 && s_cfg.nmessages == 105);
    }

    printf("[11] clips and messages over 3 minutes\n");
    {
        char dir[] = "/tmp/usrp_rc_clip_test_XXXXXX";
        if (!mkdtemp(dir))
            return 1;
        static const struct { const char *name; uint32_t secs; } CLIPS[] = {{"just_fits", 180}, {"too_long", 181}};
        char path[128];
        for (int i = 0; i < 2; i++) {
            snprintf(path, sizeof(path), "%s/%s.wav", dir, CLIPS[i].name);
            uint32_t bytes = CLIPS[i].secs * 16000, riff = bytes + 36, fmt = 16, rate = 8000, byte_rate = 16000;
            uint16_t pcm = 1, mono = 1, align = 2, bits = 16;
            FILE *fp = fopen(path, "wb");
            fwrite("RIFF", 1, 4, fp); fwrite(&riff, 4, 1, fp); fwrite("WAVEfmt ", 1, 8, fp);
            fwrite(&fmt, 4, 1, fp); fwrite(&pcm, 2, 1, fp); fwrite(&mono, 2, 1, fp);
            fwrite(&rate, 4, 1, fp); fwrite(&byte_rate, 4, 1, fp); fwrite(&align, 2, 1, fp); fwrite(&bits, 2, 1, fp);
            fwrite("data", 1, 4, fp); fwrite(&bytes, 4, 1, fp);
            fseek(fp, bytes - 1, SEEK_CUR);
            fputc(0, fp);
            fclose(fp);
        }
        vocab_cache_t *vc;
        const char *clip_dirs[] = { dir, "vocab_8k" };
        vocab_cache_create(&vc, clip_dirs, 2);
        size_t n = 0;
        check("a clip at the limit loads", vocab_get(vc, "just_fits", &n) && n == 1440000);
        check("a clip over it doesn't", vocab_get(vc, "too_long", &n) == NULL);

        load_text(BASE "[messages]\nlong = voice: too_long\n");
        check("a message using that clip fails the clip check", message_validate(&s_cfg, vc) == 1);
        load_text(BASE "[messages]\nlong = voice: just_fits _1\n");
        check("clips that add up past the limit are an error", message_validate(&s_cfg, vc) == 1);

        load_text(BASE "[messages]\nlong = tone: 440:60000 440:60000 440:60000 440:1\n");
        check("tones past the limit are an error, even unused", message_validate(&s_cfg, vc) == 1);
        load_text(BASE "[messages]\nfits = tone: 440:60000 440:60000 440:60000\n");
        check("exactly 3 minutes is fine", message_validate(&s_cfg, vc) == 0);

        load_text(BASE "[audio]\nvoice_gap_ms = 0\n[messages]\nlong = voice: just_fits _\n"
                  "[schedule]\nslow = wed 19:00-20:00 voice_gap_ms=150\nother = wed 19:00-20:00 hang=1\n");
        check("a message that fits as configured...", message_validate(&s_cfg, vc) == 0);
        check("... is over during a window that lengthens it", message_check_schedule(&s_cfg, vc) == 1);

        vocab_cache_destroy(vc);
        for (int i = 0; i < 2; i++) {
            snprintf(path, sizeof(path), "%s/%s.wav", dir, CLIPS[i].name);
            unlink(path);
        }
        rmdir(dir);
    }

    if (s_fail) {
        printf("\n==== %d check(s) FAILED ====\n", s_fail);
        return 1;
    }
    printf("\n==== all checks passed ====\n");
    return 0;
}
