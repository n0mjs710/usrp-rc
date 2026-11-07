/* DTMF decoder and command sequencing, on synthesized audio. Runs without sockets
 * or a clock: both modules are driven only by the frames passed in.
 *
 * Run from the repo root. */
#include "../src/dtmf.h"
#include "../src/command.h"
#include "../src/tone.h"
#include "../src/config.h"
#include "../src/vocab.h"
#include "../src/log.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_fail;

static void check(const char *what, int ok)
{
    printf("  %s - %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        s_fail++;
}

/* ── audio synthesis ───────────────────────────────────────────────────── */

static const double LOW[4]  = {697, 770, 852, 941};
static const double HIGH[4] = {1209, 1336, 1477, 1633};
static const char   KEYS[] = "123A456B789C*0#D";

static double s_phase;   /* running sample count, so tones don't restart on a frame */

typedef struct {
    double low, high;      /* Hz; 0 = silent */
    double low_amp, high_amp;
    double noise;          /* peak amplitude of added white noise */
} sound_t;

static sound_t key_sound(char key, double amp)
{
    int i = (int)(strchr(KEYS, key) - KEYS);
    sound_t s = {LOW[i / 4], HIGH[i % 4], amp, amp, 0};
    return s;
}

static const sound_t SILENCE = {0, 0, 0, 0, 0};

static int16_t sample(const sound_t *s)
{
    double t = s_phase++ / 8000.0;
    double v = 0.0;
    if (s->low > 0)  v += s->low_amp * sin(2 * M_PI * s->low * t);
    if (s->high > 0) v += s->high_amp * sin(2 * M_PI * s->high * t);
    if (s->noise > 0) v += s->noise * (2.0 * rand() / RAND_MAX - 1.0);
    if (v > 1.0) v = 1.0;
    if (v < -1.0) v = -1.0;
    return (int16_t)(v * 32767.0);
}

/* ── decoder driving: arbitrary-length sounds across fixed 160-sample frames ── */

static int16_t s_frame[160];
static int     s_fill;

typedef void (*frame_fn)(const int16_t frame[160], void *arg);

static void play(const sound_t *s, int ms, frame_fn fn, void *arg)
{
    for (int i = 0; i < ms * 8; i++) {
        s_frame[s_fill++] = sample(s);
        if (s_fill == 160) {
            fn(s_frame, arg);
            s_fill = 0;
        }
    }
}

typedef struct {
    dtmf_decoder_t *d;
    char on[64];
    int  non;
    int  noff;
    int  last_off_ms;
} decode_run_t;

static void decode_frame(const int16_t frame[160], void *arg)
{
    decode_run_t *r = arg;
    dtmf_event_t ev[DTMF_MAX_EVENTS];
    int n = dtmf_decoder_push(r->d, frame, ev);
    for (int i = 0; i < n; i++) {
        if (ev[i].type == DTMF_TONE_ON) {
            if (r->non < 63)
                r->on[r->non++] = ev[i].digit;
        } else {
            r->noff++;
            r->last_off_ms = ev[i].ms;
        }
    }
}

static decode_run_t new_run(void)
{
    decode_run_t r;
    memset(&r, 0, sizeof(r));
    dtmf_decoder_create(&r.d, 40);
    s_fill = 0;
    return r;
}

/* Does one tone, between silences, decode as exactly `want` (0 = nothing)? */
static int single(const sound_t *s, int ms, char want)
{
    decode_run_t r = new_run();
    play(&SILENCE, 37, decode_frame, &r);
    play(s, ms, decode_frame, &r);
    play(&SILENCE, 200, decode_frame, &r);
    int ok = want ? (r.non == 1 && r.on[0] == want && r.noff == 1) : r.non == 0;
    dtmf_decoder_destroy(r.d);
    return ok;
}

static void test_decoder(void)
{
    printf("\ndecoder\n");

    int all = 1;
    for (const char *k = KEYS; *k; k++) {
        sound_t s = key_sound(*k, 0.2);
        all &= single(&s, 80, *k);
    }
    check("all 16 keys decode", all);

    sound_t five = key_sound('5', 0.2);
    check("50 ms tone is valid", single(&five, 50, '5'));
    check("20 ms burst is rejected", single(&five, 20, 0));
    check("10 ms burst is rejected", single(&five, 10, 0));

    /* Across every alignment with the analysis hop. */
    all = 1;
    for (int pad = 0; pad < 25; pad++) {
        decode_run_t r = new_run();
        play(&SILENCE, 37 + pad, decode_frame, &r);
        play(&five, 60, decode_frame, &r);
        play(&SILENCE, 200, decode_frame, &r);
        all &= r.non == 1 && r.on[0] == '5';
        dtmf_decoder_destroy(r.d);
    }
    check("60 ms tone decodes once at every alignment", all);

    check("quiet tone (-30 dBFS each) decodes", ({ sound_t s = key_sound('8', 0.032); single(&s, 80, '8'); }));
    check("very quiet tone (-46 dBFS each) is ignored", ({ sound_t s = key_sound('8', 0.005); single(&s, 80, 0); }));
    check("6 dB twist decodes, high tone low", ({ sound_t s = key_sound('1', 0.2); s.high_amp = 0.1; single(&s, 80, '1'); }));
    check("6 dB twist decodes, low tone low", ({ sound_t s = key_sound('1', 0.2); s.low_amp = 0.1; single(&s, 80, '1'); }));
    check("14 dB twist is rejected", ({ sound_t s = key_sound('1', 0.2); s.high_amp = 0.04; single(&s, 80, 0); }));
    check("1.5% off frequency decodes", ({ sound_t s = key_sound('D', 0.2); s.low *= 1.015; s.high *= 1.015; single(&s, 80, 'D'); }));
    check("a lone tone is rejected", ({ sound_t s = key_sound('5', 0.2); s.high = 0; single(&s, 200, 0); }));
    check("tone with noise 12 dB below decodes", ({ sound_t s = key_sound('9', 0.2); s.noise = 0.1; single(&s, 100, '9'); }));

    check("30 ms tone is under the 40 ms minimum", single(&five, 30, 0));

    /* Sequences. */
    {
        decode_run_t r = new_run();
        const char *seq = "5123";
        for (const char *k = seq; *k; k++) {
            sound_t s = key_sound(*k, 0.2);
            play(&s, 60, decode_frame, &r);
            play(&SILENCE, 60, decode_frame, &r);
        }
        r.on[r.non] = 0;
        check("digit sequence decodes in order", strcmp(r.on, seq) == 0);
        dtmf_decoder_destroy(r.d);
    }
    {
        decode_run_t r = new_run();
        for (int i = 0; i < 3; i++) {
            play(&five, 60, decode_frame, &r);
            play(&SILENCE, 60, decode_frame, &r);
        }
        check("a repeated key counts each press", r.non == 3 && r.noff == 3);
        dtmf_decoder_destroy(r.d);
    }
    {
        decode_run_t r = new_run();
        play(&five, 500, decode_frame, &r);
        play(&SILENCE, 100, decode_frame, &r);
        check("a long press is one digit, timed", r.non == 1 && r.noff == 1
              && r.last_off_ms > 450 && r.last_off_ms < 560);
        dtmf_decoder_destroy(r.d);
    }
}

/* ── command session ───────────────────────────────────────────────────── */

typedef struct {
    command_session_t *s;
    int        got;         /* commands completed mid-transmission */
    command_t  cmd;
    int        muted_frames;
    int        beep_frames;
} session_run_t;

static void session_frame(const int16_t frame[160], void *arg)
{
    session_run_t *r = arg;
    if (command_session_frame(r->s, frame, &r->cmd))
        r->got++;
    if (command_session_muted(r->s)) {
        r->muted_frames++;
        int16_t out[160];
        command_session_fill_mute(r->s, out);
        for (int i = 0; i < 160; i++) {
            if (out[i] != 0) {
                r->beep_frames++;
                break;
            }
        }
    }
}

static config_t s_cfg;

static session_run_t new_session(void)
{
    session_run_t r;
    memset(&r, 0, sizeof(r));
    command_session_create(&r.s, &s_cfg);
    s_fill = 0;
    return r;
}

static void press(session_run_t *r, char key)
{
    sound_t s = key_sound(key, 0.2);
    play(&s, 80, session_frame, r);
    play(&SILENCE, 80, session_frame, r);
}

static void press_all(session_run_t *r, const char *keys)
{
    for (const char *k = keys; *k; k++)
        press(r, *k);
}

/* The pad-test digits an entry produces at unkey; "" = no command. */
static const char *end(session_run_t *r)
{
    static char out[COMMAND_MAX_DIGITS + 1];
    command_t c;
    out[0] = 0;
    if (command_session_end_of_tx(r->s, &c) && c.type == COMMAND_PAD_TEST)
        strcpy(out, c.digits);
    return out;
}

/* Regen tones, fed straight to the decoder: every key comes back, in order. */
static void test_generator(void)
{
    printf("\ngenerator\n");

    const char *keys = "123A456B789C*0#D";
    size_t n;
    int16_t *t = tone_render_dtmf(keys, 200, 100, 100, 200, 0.5, &n);
    check("length is lead + tones + gaps between + trail", n == 8 * (200 + 16 * 100 + 15 * 100 + 200));

    decode_run_t r = new_run();
    int16_t frame[160] = {0};
    for (size_t i = 0; i < n + 160 * 10; i += 160) {
        for (size_t j = 0; j < 160; j++)
            frame[j] = i + j < n ? t[i + j] : 0;
        decode_frame(frame, &r);
    }
    r.on[r.non] = 0;
    check("every key decodes, in order", strcmp(r.on, keys) == 0);
    check("the lead is silent", t[0] == 0 && t[8 * 200 - 1] == 0);
    int quiet = 1;
    for (size_t i = n - 8 * 200; i < n; i++)
        quiet &= t[i] == 0;
    check("so is the trail", quiet);
    dtmf_decoder_destroy(r.d);
    free(t);
}

static void test_commands(void)
{
    printf("\ncommands\n");

    session_run_t r = new_session();
    play(&SILENCE, 200, session_frame, &r);
    press_all(&r, "5123");
    check("pad test reads back everything after the prefix", strcmp(end(&r), "123") == 0);

    press_all(&r, "5*0A");
    check("... including star and letters", strcmp(end(&r), "*0A") == 0);
    check("each transmission starts clean", strcmp(end(&r), "") == 0);

    press(&r, '5');
    check("prefix alone does nothing", strcmp(end(&r), "") == 0);
    press_all(&r, "7123");
    check("a sequence that matches nothing does nothing", strcmp(end(&r), "") == 0);

    /* The whole-code commands. */
    {
        command_t c;
        press_all(&r, "39");
        check("time code", command_session_end_of_tx(r.s, &c) && c.type == COMMAND_TIME);
        press_all(&r, "38");
        check("usage code", command_session_end_of_tx(r.s, &c) && c.type == COMMAND_STATS);
        press_all(&r, "72");
        check("link on code", command_session_end_of_tx(r.s, &c) && c.type == COMMAND_LINK_ON);
        press_all(&r, "73");
        check("link off code", command_session_end_of_tx(r.s, &c) && c.type == COMMAND_LINK_OFF);
        press_all(&r, "70");
        check("link status code", command_session_end_of_tx(r.s, &c) && c.type == COMMAND_LINK_STATUS);
        press_all(&r, "391");
        check("a code plus extra digits doesn't match", !command_session_end_of_tx(r.s, &c));

        s_cfg.link.enabled = false;
        press_all(&r, "73");
        check("link codes reply no link service with no link", command_session_end_of_tx(r.s, &c) && c.type == COMMAND_NO_LINK);
        press_all(&r, "70");
        check("... link status too", command_session_end_of_tx(r.s, &c) && c.type == COMMAND_NO_LINK);
        press_all(&r, "39");
        check("... but time still works", command_session_end_of_tx(r.s, &c) && c.type == COMMAND_TIME);
        s_cfg.link.enabled = true;

        strcpy(s_cfg.dtmf.time_code, "");
        press_all(&r, "39");
        check("an empty code turns its command off", !command_session_end_of_tx(r.s, &c));
        strcpy(s_cfg.dtmf.time_code, "39");

        press_all(&r, "110");
        check("no repeater code by default", !command_session_end_of_tx(r.s, &c));
        strcpy(s_cfg.dtmf.repeater_code, "11");
        press_all(&r, "110");
        check("repeater code + 0 is down", command_session_end_of_tx(r.s, &c) && c.type == COMMAND_REPEATER && c.digits[0] == '0');
        press_all(&r, "111");
        check("repeater code + 1 is up", command_session_end_of_tx(r.s, &c) && c.type == COMMAND_REPEATER && c.digits[0] == '1');
        press_all(&r, "112");
        check("any other argument does nothing", !command_session_end_of_tx(r.s, &c));
        press_all(&r, "11");
        check("the code alone does nothing", !command_session_end_of_tx(r.s, &c));
        strcpy(s_cfg.dtmf.repeater_code, "");

        press_all(&r, "69*31234");
        check("no regen code by default", !command_session_end_of_tx(r.s, &c));
        strcpy(s_cfg.dtmf.regen_code, "69");
        press_all(&r, "69*31234");
        check("regen sends everything after its code",
              command_session_end_of_tx(r.s, &c) && c.type == COMMAND_REGEN && strcmp(c.digits, "*31234") == 0);
        r.got = 0;
        press_all(&r, "69#1D");
        check("... # and D included, with no early entry", r.got == 0
              && command_session_end_of_tx(r.s, &c) && c.type == COMMAND_REGEN && strcmp(c.digits, "#1D") == 0);
        press_all(&r, "69");
        check("the regen code alone does nothing", !command_session_end_of_tx(r.s, &c));
        strcpy(s_cfg.dtmf.regen_code, "");
    }

    press_all(&r, "39#5123");
    check("# cancels, and a command can follow", strcmp(end(&r), "123") == 0);
    press_all(&r, "39#");
    check("# at the end leaves nothing", strcmp(end(&r), "") == 0);

    {
        command_t c;
        r.got = 0;
        press_all(&r, "39D");
        check("D forces evaluation mid-transmission", r.got == 1 && r.cmd.type == COMMAND_TIME);
        check("... and nothing is left for the unkey", !command_session_end_of_tx(r.s, &c));
    }

    press_all(&r, "51#2D");
    check("after the pad test prefix, # and D are read back", strcmp(end(&r), "1#2D") == 0);
    r.got = 0;
    press_all(&r, "512");
    play(&SILENCE, 4500, session_frame, &r);
    press_all(&r, "39D");
    check("a pause over the interdigit time abandons a pad test", r.got == 1 && r.cmd.type == COMMAND_TIME);
    end(&r);

    press(&r, '5');
    play(&SILENCE, 4500, session_frame, &r);
    press_all(&r, "5456");
    check("over the interdigit time, earlier digits are dropped", strcmp(end(&r), "456") == 0);

    press_all(&r, "512");
    play(&SILENCE, 3000, session_frame, &r);
    press(&r, '3');
    check("under the interdigit time, the sequence survives", strcmp(end(&r), "123") == 0);

    press_all(&r, "5123");
    play(&SILENCE, 8500, session_frame, &r);
    check("a long gap before the unkey discards the command", strcmp(end(&r), "") == 0);
    press_all(&r, "5123");
    play(&SILENCE, 7500, session_frame, &r);
    check("a shorter one doesn't", strcmp(end(&r), "123") == 0);

    {
        press_all(&r, "51");
        sound_t two = key_sound('2', 0.2);
        play(&two, 5200, session_frame, &r);
        play(&SILENCE, 100, session_frame, &r);
        check("a tone over max_tone_ms discards the sequence", strcmp(end(&r), "") == 0);
    }
    {
        press(&r, '5');
        sound_t two = key_sound('2', 0.2);
        play(&two, 4500, session_frame, &r);
        play(&SILENCE, 100, session_frame, &r);
        check("a long tone under it is kept", strcmp(end(&r), "2") == 0);
    }

    for (int i = 0; i < 40; i++)
        press(&r, '1');
    check("an overlong sequence is ignored", strcmp(end(&r), "") == 0);
    command_session_destroy(r.s);

    printf("\nmuting\n");

    r = new_session();
    play(&SILENCE, 500, session_frame, &r);
    check("nothing muted before a tone", r.muted_frames == 0);
    press(&r, '9');
    check("a digit no command starts with doesn't mute", !command_session_muted(r.s));
    press(&r, '#');
    check("nor does a cancel", !command_session_muted(r.s));
    press(&r, '7');
    check("a digit that can start a command mutes", command_session_muted(r.s));
    press(&r, '9');
    check("a digit that kills the entry unmutes", !command_session_muted(r.s));
    check("... and drops it", strcmp(end(&r), "") == 0);
    press_all(&r, "79");
    press_all(&r, "512");
    check("a fresh entry after a dead one works", strcmp(end(&r), "12") == 0);

    press(&r, '5');
    check("first digit mutes", command_session_muted(r.s));

    play(&SILENCE, 1800, session_frame, &r);
    check("still muted inside the hang", command_session_muted(r.s));
    press(&r, '1');
    play(&SILENCE, 1800, session_frame, &r);
    check("another digit extends the hang", command_session_muted(r.s));
    play(&SILENCE, 400, session_frame, &r);
    check("unmuted once the hang expires", !command_session_muted(r.s));

    press(&r, '5');
    check("a later digit mutes again", command_session_muted(r.s));
    end(&r);
    check("the unkey clears the mute", !command_session_muted(r.s));
    command_session_destroy(r.s);

    /* Beep cadence: 100 ms on of every 1000. */
    r = new_session();
    int16_t out[160];
    int on = 0, starts = 0, prev = 0;
    for (int f = 0; f < 500; f++) {
        command_session_fill_mute(r.s, out);
        int loud = 0;
        for (int i = 0; i < 160; i++)
            if (out[i] != 0)
                loud = 1;
        if (loud && !prev)
            starts++;
        prev = loud;
        on += loud;
    }
    check("the beep repeats once a second", starts == 10);
    check("for about 100 ms", on >= 50 && on <= 60);
    command_session_destroy(r.s);

    /* Reset between uses of the same session must leave the decoder clean: a tone
     * cut off by the unkey can't leak into the next transmission. */
    r = new_session();
    {
        sound_t two = key_sound('2', 0.2);
        play(&two, 30, session_frame, &r);
        end(&r);
        s_fill = 0;
        play(&two, 20, session_frame, &r);
        check("a partial tone doesn't carry across transmissions", !command_session_muted(r.s));
    }
    command_session_destroy(r.s);
}

/* Real speech: every clip in the stock vocabulary, one long "transmission". */
static void test_speech_false_detects(void)
{
    printf("\nfalse detection on speech\n");
    vocab_cache_t *vocab;
    const char *dirs[] = {"vocab_8k"};
    DIR *dir = opendir("vocab_8k");
    if (!dir || vocab_cache_create(&vocab, dirs, 1) != 0) {
        check("vocab_8k/ is readable (run from the repo root)", 0);
        return;
    }

    decode_run_t r = new_run();
    long samples = 0;
    int clips = 0;
    struct dirent *e;
    while ((e = readdir(dir))) {
        char name[256];
        snprintf(name, sizeof(name), "%s", e->d_name);
        char *dot = strstr(name, ".wav");
        if (!dot)
            continue;
        *dot = 0;
        size_t n;
        const int16_t *pcm = vocab_get(vocab, name, &n);
        if (!pcm)
            continue;
        clips++;
        samples += (long)n;
        for (size_t i = 0; i + 160 <= n; i += 160)
            decode_frame(pcm + i, &r);
    }
    closedir(dir);

    printf("  %d clips, %ld s of speech, %d false digit(s)\n", clips, samples / 8000, r.non);
    check("speech doesn't produce digits", clips > 100 && r.non == 0);
    dtmf_decoder_destroy(r.d);
    vocab_cache_destroy(vocab);
}

static void test_clips(void)
{
    printf("\nspeech\n");
    check("digits", strcmp(command_digit_clip('0'), "ZERO") == 0 && strcmp(command_digit_clip('9'), "NINE") == 0);
    check("letters", strcmp(command_digit_clip('A'), "A") == 0);
    check("star is Sierra", strcmp(command_digit_clip('*'), "SIERRA") == 0);
    check("pound is P", strcmp(command_digit_clip('#'), "P") == 0);
}

int main(void)
{
    log_set_level("error");   /* keep the digit-by-digit log out of the results */
    srand(1);
    config_defaults(&s_cfg);

    test_decoder();
    test_generator();
    test_commands();
    test_speech_false_detects();
    test_clips();

    if (s_fail) {
        printf("\n==== %d check(s) FAILED ====\n", s_fail);
        return 1;
    }
    printf("\n==== all checks passed ====\n");
    return 0;
}
