/*
 * Deterministic, fast harness for src/port.c's repeater-controller state
 * machine. Drives it with a fake clock (no real sleeping) through
 * scripted COS timelines and inspects the resulting stderr event log
 * (port.c already fprintf()s every state-machine-relevant event) plus
 * PTT-callback timing and raw ctrl-buffer audio presence, to confirm the
 * interrupt-handling fixes without waiting hours/days to hit these
 * corner cases on the air.
 *
 * Not a general-purpose test framework -- just enough scaffolding to
 * script and assert on the specific scenarios this fix is meant to cover.
 * Run from the repo root (paths to tests/harness.conf and vocab_8k/ are
 * relative); `make check` does this for you.
 */

#include "../src/config.h"
#include "../src/port.h"
#include "../src/vocab.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>

#define TICK_MS 20

static uint64_t g_now;

/* ── PTT transition capture ──────────────────────────────────────────── */

typedef struct { uint64_t ms; bool active; char reason[32]; } ptt_event_t;

static ptt_event_t g_ptt[64];
static int         g_ptt_n;

static void on_ptt(void *arg, bool active, const char *reason)
{
    (void)arg;
    if (g_ptt_n < 64) {
        g_ptt[g_ptt_n].ms     = g_now;
        g_ptt[g_ptt_n].active = active;
        strncpy(g_ptt[g_ptt_n].reason, reason, sizeof(g_ptt[g_ptt_n].reason) - 1);
        g_ptt_n++;
    }
}

static bool ptt_has_reason(const char *reason)
{
    for (int i = 0; i < g_ptt_n; i++)
        if (strcmp(g_ptt[i].reason, reason) == 0)
            return true;
    return false;
}

/* ── stderr capture (port.c logs every state-machine event via fprintf) ─ */

static int  g_saved_fd = -1;
static int  g_cap_fd   = -1;
static char g_cap_path[] = "/tmp/port_harness_cap_XXXXXX";

static void cap_start(void)
{
    fflush(stderr);
    g_saved_fd = dup(STDERR_FILENO);
    strcpy(g_cap_path, "/tmp/port_harness_cap_XXXXXX");
    g_cap_fd = mkstemp(g_cap_path);
    dup2(g_cap_fd, STDERR_FILENO);
}

/* Reads everything captured so far without stopping capture. Uses its own
 * independent read handle (opened fresh, by path) rather than seeking on
 * the shared write fd -- fd 2 and g_cap_fd are dup()'d (same open file
 * description, same offset), so seeking on one to read moves the other's
 * write position too; a separate fopen()/fclose() avoids that entirely.
 * Caller frees the result. */
static char *cap_peek(void)
{
    fflush(stderr);
    FILE *rf = fopen(g_cap_path, "r");
    if (!rf)
        return strdup("");
    fseek(rf, 0, SEEK_END);
    long len = ftell(rf);
    rewind(rf);
    char *buf = malloc((size_t)len + 1);
    size_t got = fread(buf, 1, (size_t)len, rf);
    buf[got] = 0;
    fclose(rf);
    return buf;
}

static char *cap_stop(void)
{
    char *buf = cap_peek();
    fflush(stderr);
    dup2(g_saved_fd, STDERR_FILENO);
    close(g_saved_fd);
    g_saved_fd = -1;
    close(g_cap_fd);
    unlink(g_cap_path);
    g_cap_fd = -1;
    return buf;
}

static int count_occurrences(const char *hay, const char *needle)
{
    int c = 0;
    size_t nlen = strlen(needle);
    const char *p = hay;
    while ((p = strstr(p, needle)) != NULL) { c++; p += nlen; }
    return c;
}

/* ── fake-clock tick / COS helpers ────────────────────────────────────── */

/* Advances the fake clock by one 20ms frame, runs the timer checks, and --
 * mirroring main.c's handle_mmdvm()/pace_tick() -- pulls one ctrl-buffer
 * frame only when neither real-audio gate is open, then applies any
 * in-flight impolite-ID mix to whatever frame resulted (real repeat audio
 * in production; the harness has none, so a zeroed frame -- but the mix
 * still needs draining on schedule regardless). Returns true if the
 * resulting frame carried audible (non-all-zero) content from the
 * ctrl-buffer pull specifically. */
static bool tick(port_t *p)
{
    g_now += TICK_MS;
    port_check_timers(p, g_now);
    if (!port_ptt(p))
        return false;

    int16_t frame[160] = {0};
    bool have_ctrl_content = false;
    if (!port_mmdvm_gate_open(p) && !port_link_gate_open(p)) {
        port_ctrl_pull(p, frame, g_now);
        have_ctrl_content = true;
    }
    port_mix_apply(p, frame, g_now);

    if (have_ctrl_content) {
        for (int i = 0; i < 160; i++)
            if (frame[i] != 0)
                return true;
    }
    return false;
}

static void wait_ms(port_t *p, int ms)
{
    for (int t = 0; t < ms; t += TICK_MS)
        tick(p);
}

/* Ticks until PTT drops, or a generous safety budget runs out -- a stuck
 * PTT is itself a very informative failure (the caller's CHECK on the
 * return value catches it). */
static bool wait_until_idle(port_t *p, int max_ms)
{
    for (int t = 0; t < max_ms; t += TICK_MS) {
        tick(p);
        if (!port_ptt(p))
            return true;
    }
    return false;
}

/* Ticks until `needle` shows up in the capture so far, or budget expires. */
static bool wait_for_log(port_t *p, const char *needle, int max_ms)
{
    for (int t = 0; t < max_ms; t += TICK_MS) {
        tick(p);
        char *buf = cap_peek();
        bool found = strstr(buf, needle) != NULL;
        free(buf);
        if (found)
            return true;
    }
    return false;
}

/* Assumes audio is playing right now; measures the silence gap between
 * the end of the current run and the start of the next one. -1 if either
 * transition doesn't happen within budget. */
static int measure_gap_after_current_run(port_t *p, int max_ms)
{
    int elapsed = 0;
    bool cur = true;
    while (cur && elapsed < max_ms) { cur = tick(p); elapsed += TICK_MS; }
    if (elapsed >= max_ms) return -1;
    int gap_start = elapsed;
    while (!cur && elapsed < max_ms) { cur = tick(p); elapsed += TICK_MS; }
    if (elapsed >= max_ms) return -1;
    return elapsed - gap_start;
}

static void key_local(port_t *p, bool active) { port_on_mmdvm_keyup(p, active, g_now); }

/* ── scenario scaffolding ─────────────────────────────────────────────── */

#define CHECK(cond, ...) do { \
        if (cond) { printf("  ok   - " __VA_ARGS__); printf("\n"); } \
        else      { printf("  FAIL - " __VA_ARGS__); printf("\n"); ok = false; } \
    } while (0)

static port_t *fresh_port(config_t *cfg, vocab_cache_t *vocab)
{
    port_t *p;
    if (port_create(&p, cfg, vocab) != 0) {
        fprintf(stderr, "port_create failed\n");
        exit(1);
    }
    port_set_ptt_callback(p, on_ptt, NULL);
    g_now   = 0;
    g_ptt_n = 0;
    return p;
}

/* Drives a first, uninterrupted transmission from a totally fresh port to
 * a clean IDLE settle -- arms id_deadline (via the initial ID's own
 * schedule_id() call) and exercises the ordinary single-transmission path
 * once. Shared setup for scenarios that need id_deadline already armed. */
static void prime(port_t *p)
{
    key_local(p, true);
    wait_ms(p, 150);        /* over the 100ms kerchunk threshold */
    key_local(p, false);
    wait_until_idle(p, 3000);
}

/* prime()'s own initial-ID completion resets tx_activity to false (see
 * initial_id_after_drain) -- on_id() declines to fire ANY periodic ID
 * (mandatory or impolite) while tx_activity is false, since nothing has
 * happened since the last one. A second, ordinary transmission (which
 * doesn't re-trigger initial_id_pending, already consumed by prime())
 * sets tx_activity back to true and leaves it true through its own
 * plain courtesy-tone/hang completion, so the next periodic id_deadline
 * actually has something to report. */
static void activity_pulse(port_t *p)
{
    key_local(p, true);
    wait_ms(p, 150);
    key_local(p, false);
    wait_until_idle(p, 3000);
}

/* ── scenarios ────────────────────────────────────────────────────────── */

static bool scenario_normal_flow_and_cw_survives(config_t *base, vocab_cache_t *vocab)
{
    printf("\n[1] normal single-transmission flow, then a CW mandatory ID survives an interrupt\n");
    bool ok = true;
    config_t cfg = *base;
    port_t *p = fresh_port(&cfg, vocab);

    cap_start();
    prime(p);
    char *log1 = cap_stop();
    CHECK(count_occurrences(log1, "id: initial") == 1, "priming played exactly one initial ID");
    CHECK(count_occurrences(log1, "hang: armed") == 1, "priming armed the hang timer exactly once");
    CHECK(count_occurrences(log1, "id: mandatory") == 0, "no mandatory ID fired during priming");
    CHECK(count_occurrences(log1, "id: anxious") == 0, "no anxious ID fired during priming (disabled by default)");
    free(log1);

    activity_pulse(p);   /* re-arm tx_activity so the mandatory ID has something to report */

    cap_start();
    bool started = wait_for_log(p, "id: mandatory", 3000);
    CHECK(started, "mandatory CW ID fired on schedule after priming settled");
    wait_ms(p, 60);   /* solidly mid-CW */

    key_local(p, true);          /* interrupt */
    wait_ms(p, 150);              /* over kerchunk */
    key_local(p, false);
    wait_until_idle(p, 3000);

    char *log2 = cap_stop();
    CHECK(count_occurrences(log2, "id: mandatory") == 1, "the CW ID was never re-rendered/restarted");
    CHECK(count_occurrences(log2, "id: initial") == 0, "no redundant initial ID after the interrupt (bug #1)");
    CHECK(count_occurrences(log2, "id: impolite") == 0, "no impolite ID either -- CW is left alone, not cut");
    CHECK(count_occurrences(log2, "cor: kerchunk") == 0, "the interrupt wasn't misclassified as a kerchunk");
    CHECK(count_occurrences(log2, "cor: ACTIVE") == 1 && count_occurrences(log2, "cor: IDLE") == 1,
          "exactly one interrupting transmission cycle observed");
    free(log2);

    port_destroy(p);
    printf(ok ? "[1] PASS\n" : "[1] FAIL\n");
    return ok;
}

static bool scenario_voice_mandatory_interrupt_fires_impolite(config_t *base, vocab_cache_t *vocab)
{
    printf("\n[2] voice mandatory ID interrupted -> impolite ID fires (regression check)\n");
    bool ok = true;
    config_t cfg = *base;
    strncpy(cfg.events.mandatory_ids[0], "id_voice", sizeof(cfg.events.mandatory_ids[0]) - 1);
    port_t *p = fresh_port(&cfg, vocab);

    prime(p);
    activity_pulse(p);

    cap_start();
    bool started = wait_for_log(p, "id: mandatory", 3000);
    CHECK(started, "voice mandatory ID fired on schedule");
    wait_ms(p, 60);

    key_local(p, true);
    wait_ms(p, 150);
    key_local(p, false);
    wait_until_idle(p, 3000);

    char *log = cap_stop();
    CHECK(count_occurrences(log, "id: mandatory") == 1, "voice ID rendered exactly once");
    CHECK(count_occurrences(log, "id: impolite") == 1, "impolite ID fired immediately (voice ID cut short)");
    CHECK(count_occurrences(log, "id: initial") == 0, "no redundant initial ID afterward");
    free(log);

    port_destroy(p);
    printf(ok ? "[2] PASS\n" : "[2] FAIL\n");
    return ok;
}

static bool scenario_courtesy_tone_no_stacking(config_t *base, vocab_cache_t *vocab)
{
    printf("\n[3] rapid back-and-forth doesn't stack/duplicate the courtesy tone (bug #2)\n");
    bool ok = true;
    config_t cfg = *base;
    port_t *p = fresh_port(&cfg, vocab);

    prime(p);

    key_local(p, true);            /* transmission A */
    wait_ms(p, 150);
    key_local(p, false);
    wait_ms(p, 250);                /* past ct_delay (200ms); ~50ms into CT1 */

    key_local(p, true);            /* transmission B interrupts CT1 mid-play */
    wait_ms(p, 150);
    key_local(p, false);

    int runs = 0;
    bool prev = false;
    for (int t = 0; t < 1500; t += TICK_MS) {
        bool cur = tick(p);
        if (cur && !prev) runs++;
        prev = cur;
    }
    CHECK(runs == 1, "exactly one courtesy-tone burst played after B settled (got %d; "
                      "pre-fix this was 2 -- a clipped leftover of CT1 plus a fresh CT2)", runs);

    port_destroy(p);
    printf(ok ? "[3] PASS\n" : "[3] FAIL\n");
    return ok;
}

static bool scenario_anxious_between_ct_delay_and_ct(config_t *base, vocab_cache_t *vocab)
{
    printf("\n[4] anxious ID injects between ct_delay and the courtesy tone, with a fresh pause before CT\n");
    bool ok = true;
    config_t cfg = *base;
    /* id_voice (a single vocab word, no internal silence) rather than
     * id_cw here specifically so measure_gap_after_current_run() below
     * sees one continuous run -- CW's inter-letter/word gaps would
     * otherwise be indistinguishable from the post-message pause we're
     * trying to measure. */
    strncpy(cfg.events.anxious_id, "id_voice", sizeof(cfg.events.anxious_id) - 1);
    cfg.timers.id_interval = 1.00;   /* fast, predictable timing for this scenario, */
    cfg.timers.id_anxious  = 0.90;   /* independent of the base config's id_interval */
    port_t *p = fresh_port(&cfg, vocab);

    cap_start();
    key_local(p, true);
    wait_ms(p, 150);
    key_local(p, false);
    /* This first-ever transmission's own tail exercises the sequence:
     * initial ID -> schedule_id (arms id_sub_deadline @ +0.1s and
     * ct_delay @ +0.2s from the same instant) -> id_sub_deadline fires
     * first, arming anxious_id_armed -> ct_delay fires -> anxious ID
     * (not the CT) -> anxious_id_finish re-arms a fresh ct_delay -> CT. */
    bool got_initial = wait_for_log(p, "id: initial", 3000);
    CHECK(got_initial, "initial ID fired first");

    bool got_anxious = wait_for_log(p, "id: anxious", 3000);
    CHECK(got_anxious, "anxious ID fired next, ahead of the courtesy tone");

    /* anxious_id_finish() re-arms id_sub_deadline via schedule_id() with
     * this same (deliberately tight, for a fast test) id_anxious/
     * id_interval ratio -- left alone, it would re-fire before the fresh
     * ct_delay it also re-arms, preempting the courtesy tone forever
     * (this exact ratio is what makes the anxious ID preempt ct_delay at
     * all; the same ratio re-applied always does it again). That
     * requires id_interval - id_anxious < ct_delay, which no sane
     * deployment would configure (id_anxious is meant to be a short lead
     * before a long interval) -- disable further re-arming now that
     * we've confirmed the one preemption we're testing, same as a real
     * deployment's next cycle wouldn't be due for hundreds of seconds.
     * p->cfg points at this function's `cfg`, so schedule_id() (which
     * re-reads it live) sees this on its very next call. */
    cfg.timers.id_anxious = 0.0;

    int gap = measure_gap_after_current_run(p, 3000);
    CHECK(gap >= 160 && gap <= 260,
          "courtesy tone followed the anxious ID after a fresh ~200ms ct_delay pause (got %dms)", gap);

    bool settled = wait_until_idle(p, 3000);
    CHECK(settled, "port returned to idle (PTT off) without getting stuck");

    char *log = cap_stop();
    CHECK(count_occurrences(log, "id: mandatory") == 0, "no mandatory ID involved in this scenario");
    free(log);

    port_destroy(p);
    printf(ok ? "[4] PASS\n" : "[4] FAIL\n");
    return ok;
}

static bool scenario_kerchunk_ignored(config_t *base, vocab_cache_t *vocab)
{
    printf("\n[5] brief sub-threshold keyup is still ignored as a kerchunk (regression check)\n");
    bool ok = true;
    config_t cfg = *base;
    port_t *p = fresh_port(&cfg, vocab);

    cap_start();
    key_local(p, true);
    wait_ms(p, 50);         /* under the 100ms kerchunk threshold */
    key_local(p, false);
    wait_ms(p, 300);
    char *log = cap_stop();

    CHECK(count_occurrences(log, "cor: kerchunk") == 1, "logged as a kerchunk");
    CHECK(count_occurrences(log, "id: initial") == 0, "no initial ID fired for a kerchunk");
    CHECK(!port_ptt(p), "PTT is off after the kerchunk settles");
    free(log);

    port_destroy(p);
    printf(ok ? "[5] PASS\n" : "[5] FAIL\n");
    return ok;
}

static bool scenario_timeout_recovery(config_t *base, vocab_cache_t *vocab)
{
    printf("\n[6] TOT lockout announces, recovers, and doesn't corrupt subsequent ID/CT state (regression check)\n");
    bool ok = true;
    config_t cfg = *base;
    port_t *p = fresh_port(&cfg, vocab);

    key_local(p, true);
    wait_ms(p, 5500);       /* past the 5.0s timeout */
    CHECK(ptt_has_reason("timeout-announced"), "PTT dropped with reason timeout-announced");

    key_local(p, false);    /* release, well past the TOT */
    bool settled = wait_until_idle(p, 5000);
    CHECK(settled, "recovered back to idle without getting stuck");
    CHECK(ptt_has_reason("timeout-recovery"), "PTT re-asserted with reason timeout-recovery on release");

    /* one more ordinary cycle afterward should behave normally */
    g_ptt_n = 0;
    cap_start();
    key_local(p, true);
    wait_ms(p, 150);
    key_local(p, false);
    wait_until_idle(p, 3000);
    char *log = cap_stop();
    CHECK(count_occurrences(log, "cor: kerchunk") == 0,
          "post-recovery transmission behaves normally, not misclassified");
    free(log);

    port_destroy(p);
    printf(ok ? "[6] PASS\n" : "[6] FAIL\n");
    return ok;
}

static bool scenario_impolite_resets_tx_activity(config_t *base, vocab_cache_t *vocab)
{
    printf("\n[7] impolite-ID substitution resets tx_activity, so a later silent period truly goes idle\n");
    bool ok = true;
    config_t cfg = *base;
    cfg.timers.id_interval = 3.00;   /* long enough that priming settles well before it first expires */
    port_t *p = fresh_port(&cfg, vocab);

    prime(p);   /* arms id_deadline via the initial ID's own completion */

    cap_start();
    key_local(p, true);     /* transmission B: hold across id_deadline's expiry */
    wait_ms(p, 2500);       /* id_deadline fires mid-hold -> impolite substitution, not mandatory */
    key_local(p, false);

    char *log1 = cap_stop();
    CHECK(count_occurrences(log1, "id: impolite") == 1,
          "impolite ID substituted while COR was active at expiry");
    CHECK(count_occurrences(log1, "id: mandatory") == 0,
          "not the ctrl-buffer mandatory path -- COR was active at expiry");
    free(log1);

    bool settled = wait_until_idle(p, 5000);
    CHECK(settled, "settled back to idle after the impolite substitution's own CT/hang tail");

    /* The id_deadline rearmed by impolite_id_after_mix() is what's under
     * test here: with no further activity, tx_activity must have been
     * reset to false so on_id() declines to fire again when it expires --
     * otherwise this wrongly re-IDs during genuine silence and never
     * reaches "go back to idle." */
    cap_start();
    wait_ms(p, 3200);   /* span the full rearmed id_interval with nothing happening */
    char *log2 = cap_stop();
    CHECK(count_occurrences(log2, "id: mandatory") == 0 && count_occurrences(log2, "id: impolite") == 0,
          "no further ID fired during a genuinely silent period (tx_activity correctly reset)");
    free(log2);

    port_destroy(p);
    printf(ok ? "[7] PASS\n" : "[7] FAIL\n");
    return ok;
}

static bool scenario_command_reply(config_t *base, vocab_cache_t *vocab)
{
    printf("\n[8] command reply plays in the tail ahead of the CT, and a keyup interrupts it\n");
    bool ok = true;
    config_t cfg = *base;
    port_t *p = fresh_port(&cfg, vocab);
    prime(p);

    /* A reply is 200 ms of steady non-zero samples, easy to tell from the 60 ms CT. */
    const size_t REPLY_SAMPLES = 1600;
    int16_t *reply = malloc(REPLY_SAMPLES * sizeof(int16_t));
    for (size_t i = 0; i < REPLY_SAMPLES; i++) reply[i] = 1000;

    key_local(p, true);
    wait_ms(p, 150);
    port_queue_reply(p, reply, REPLY_SAMPLES);
    key_local(p, false);

    int first_run = 0, runs = 0, run_ms = 0;
    bool prev = false;
    for (int t = 0; t < 1500; t += TICK_MS) {
        bool cur = tick(p);
        if (cur) run_ms += TICK_MS;
        if (cur && !prev) runs++;
        if (!cur && prev && first_run == 0) first_run = run_ms;
        prev = cur;
    }
    CHECK(first_run >= 180 && first_run <= 240, "reply played first, in full (first run %d ms)", first_run);
    CHECK(runs == 2, "the courtesy tone still followed it (%d bursts)", runs);
    CHECK(!port_ptt(p), "PTT released afterward");

    /* Interrupted before it starts. */
    reply = malloc(REPLY_SAMPLES * sizeof(int16_t));
    for (size_t i = 0; i < REPLY_SAMPLES; i++) reply[i] = 1000;
    key_local(p, true);
    wait_ms(p, 150);
    port_queue_reply(p, reply, REPLY_SAMPLES);
    key_local(p, false);
    wait_ms(p, 60);          /* still inside ct_delay */
    key_local(p, true);
    wait_ms(p, 150);
    key_local(p, false);
    run_ms = 0;
    for (int t = 0; t < 1000; t += TICK_MS)
        if (tick(p)) run_ms += TICK_MS;
    CHECK(run_ms <= 80, "keyup before it started dropped the reply (%d ms of audio after)", run_ms);

    /* Interrupted while playing. */
    reply = malloc(REPLY_SAMPLES * sizeof(int16_t));
    for (size_t i = 0; i < REPLY_SAMPLES; i++) reply[i] = 1000;
    key_local(p, true);
    wait_ms(p, 150);
    port_queue_reply(p, reply, REPLY_SAMPLES);
    key_local(p, false);
    wait_ms(p, 260);         /* 60 ms into the reply */
    key_local(p, true);
    wait_ms(p, 150);
    key_local(p, false);
    run_ms = 0;
    for (int t = 0; t < 1000; t += TICK_MS)
        if (tick(p)) run_ms += TICK_MS;
    CHECK(run_ms <= 80, "keyup during it cut the reply (%d ms of audio after)", run_ms);

    /* A quick command isn't a kerchunk. */
    reply = malloc(REPLY_SAMPLES * sizeof(int16_t));
    for (size_t i = 0; i < REPLY_SAMPLES; i++) reply[i] = 1000;
    cap_start();
    key_local(p, true);
    wait_ms(p, 40);          /* under the 100 ms threshold */
    port_queue_reply(p, reply, REPLY_SAMPLES);
    key_local(p, false);
    run_ms = 0;
    for (int t = 0; t < 1000; t += TICK_MS)
        if (tick(p)) run_ms += TICK_MS;
    char *log = cap_stop();
    CHECK(count_occurrences(log, "cor: kerchunk") == 0, "not treated as a kerchunk");
    CHECK(run_ms >= 180, "the reply still played (%d ms of audio)", run_ms);
    free(log);

    port_destroy(p);
    printf(ok ? "[8] PASS\n" : "[8] FAIL\n");
    return ok;
}

static bool scenario_link_state_courtesy_tone(config_t *base, vocab_cache_t *vocab)
{
    printf("\n[9] local courtesy tone follows the link state\n");
    bool ok = true;
    config_t cfg = *base;
    cfg.timers.id_interval = 600.0;   /* keep a scheduled ID out of the measurements */
    port_t *p = fresh_port(&cfg, vocab);
    prime(p);

    int ms[2];
    for (int i = 0; i < 2; i++) {
        port_set_link_up(p, i == 0);
        key_local(p, true);
        wait_ms(p, 150);
        key_local(p, false);
        ms[i] = 0;
        for (int t = 0; t < 1000; t += TICK_MS)
            if (tick(p)) ms[i] += TICK_MS;
    }
    CHECK(ms[0] <= 80, "link on plays the normal tone (%d ms)", ms[0]);
    CHECK(ms[1] >= 180, "link off plays its own tone (%d ms)", ms[1]);

    /* Unset, there's nothing different to play. */
    cfg.events.ct_link_off_message[0] = 0;
    key_local(p, true);
    wait_ms(p, 150);
    key_local(p, false);
    int fallback = 0;
    for (int t = 0; t < 1000; t += TICK_MS)
        if (tick(p)) fallback += TICK_MS;
    CHECK(fallback > 0 && fallback <= 80, "with none set, link off falls back to the normal tone (%d ms)", fallback);

    port_destroy(p);
    printf(ok ? "[9] PASS\n" : "[9] FAIL\n");
    return ok;
}

static bool scenario_tail_message_count(config_t *base, vocab_cache_t *vocab)
{
    printf("\n[10] tail message plays on every Nth tail, before the carrier drops\n");
    bool ok = true;
    config_t cfg = *base;
    cfg.timers.id_interval = 600.0;
    snprintf(cfg.events.tail_messages[0], CFG_NAME_STR, "id_cw");
    cfg.events.n_tail_messages = 1;
    cfg.timers.tail_every = 3;
    port_t *p = fresh_port(&cfg, vocab);
    prime(p);                 /* tail 1 */

    cap_start();
    activity_pulse(p);        /* tail 2 */
    char *log = cap_stop();
    CHECK(count_occurrences(log, "tail: ") == 0, "not on tail 2 of 3");
    free(log);

    cap_start();
    activity_pulse(p);        /* tail 3 */
    log = cap_stop();
    CHECK(count_occurrences(log, "tail: ") == 1, "on tail 3 of 3");
    CHECK(count_occurrences(log, "tail message done") == 1, "carrier dropped once it finished");
    CHECK(!port_ptt(p), "PTT off afterward");
    free(log);

    cap_start();
    activity_pulse(p);        /* tail 1 of the next count */
    log = cap_stop();
    CHECK(count_occurrences(log, "tail: ") == 0, "the count starts over");
    free(log);
    activity_pulse(p);

    /* tail 3 again, but a keyup cuts it off */
    cap_start();
    key_local(p, true);
    wait_ms(p, 150);
    key_local(p, false);
    wait_for_log(p, "tail: ", 3000);
    wait_ms(p, 60);
    key_local(p, true);
    wait_ms(p, 150);
    key_local(p, false);
    wait_until_idle(p, 5000);
    log = cap_stop();
    CHECK(count_occurrences(log, "tail: ") == 1, "a message cut off by a keyup counts as played");
    free(log);

    /* Nothing to play: no counting, and the carrier drops normally. */
    cfg.events.n_tail_messages = 0;
    cap_start();
    activity_pulse(p);
    log = cap_stop();
    CHECK(count_occurrences(log, "tail: ") == 0 && !port_ptt(p), "no tail message set: an ordinary tail");
    free(log);

    port_destroy(p);
    printf(ok ? "[10] PASS\n" : "[10] FAIL\n");
    return ok;
}

static bool scenario_tail_message_min_interval(config_t *base, vocab_cache_t *vocab)
{
    printf("\n[11] tail_min_interval limits tail messages, and nothing keys up on its own\n");
    bool ok = true;
    config_t cfg = *base;
    cfg.timers.id_interval = 600.0;
    snprintf(cfg.events.tail_messages[0], CFG_NAME_STR, "id_cw");
    cfg.events.n_tail_messages = 1;
    cfg.timers.tail_every = 1;
    cfg.timers.tail_min_interval = 6.0;
    port_t *p = fresh_port(&cfg, vocab);
    prime(p);

    /* prime() played one, so wait that out. */
    wait_until_idle(p, 8000);
    wait_ms(p, 6500);

    /* Cut off by a keyup: still played, so the interval runs from there. */
    cap_start();
    key_local(p, true);
    wait_ms(p, 150);
    key_local(p, false);
    wait_for_log(p, "tail: ", 3000);
    wait_ms(p, 60);
    key_local(p, true);
    wait_ms(p, 150);
    key_local(p, false);
    wait_until_idle(p, 5000);
    char *log = cap_stop();
    CHECK(count_occurrences(log, "tail: ") == 1, "a message cut off by a keyup counts as played");
    free(log);

    /* More tails too soon, with the count reached each time. */
    cap_start();
    activity_pulse(p);
    activity_pulse(p);
    log = cap_stop();
    CHECK(count_occurrences(log, "tail: ") == 0, "none inside the interval, even with the count reached");
    CHECK(count_occurrences(log, "hang expired") == 2 && !port_ptt(p), "ordinary tails instead");
    free(log);

    /* The FCC point: however long it's quiet and however overdue, no key-up. */
    cap_start();
    wait_ms(p, 6500);
    log = cap_stop();
    CHECK(!port_ptt(p) && count_occurrences(log, "tail: ") == 0, "never keys up on its own");
    free(log);

    cap_start();
    activity_pulse(p);
    log = cap_stop();
    CHECK(count_occurrences(log, "tail: ") == 1, "plays once at the first hang end after the interval");
    CHECK(!port_ptt(p), "PTT off afterward");
    free(log);

    cap_start();
    activity_pulse(p);
    log = cap_stop();
    CHECK(count_occurrences(log, "tail: ") == 0, "the tails held back earlier don't add up to more plays");
    free(log);

    port_destroy(p);
    printf(ok ? "[11] PASS\n" : "[11] FAIL\n");
    return ok;
}

static bool scenario_tail_message_rotation(config_t *base, vocab_cache_t *vocab)
{
    printf("\n[13] tail messages rotate through their list\n");
    bool ok = true;
    config_t cfg = *base;
    cfg.timers.id_interval = 600.0;
    snprintf(cfg.events.tail_messages[0], CFG_NAME_STR, "id_cw");
    snprintf(cfg.events.tail_messages[1], CFG_NAME_STR, "id_voice");
    cfg.events.n_tail_messages = 2;
    cfg.timers.tail_every = 1;
    port_t *p = fresh_port(&cfg, vocab);

    cap_start();
    for (int i = 0; i < 3; i++) {
        key_local(p, true);
        wait_ms(p, 150);
        key_local(p, false);
        wait_for_log(p, "tail: ", 5000);
        wait_until_idle(p, 15000);
    }
    char *log = cap_stop();
    const char *first  = strstr(log, "tail: 'id_cw'");
    const char *second = first ? strstr(first, "tail: 'id_voice'") : NULL;
    const char *third  = second ? strstr(second, "tail: 'id_cw'") : NULL;
    CHECK(third != NULL, "first, second, then back to the first");
    CHECK(count_occurrences(log, "tail: ") == 3, "one per tail");
    free(log);

    port_destroy(p);
    printf(ok ? "[13] PASS\n" : "[13] FAIL\n");
    return ok;
}

static bool scenario_empty_mandatory_uses_impolite(config_t *base, vocab_cache_t *vocab)
{
    printf("\n[14] with no mandatory ID, the timer keys up with impolite_id; with neither, no ID\n");
    bool ok = true;
    config_t cfg = *base;
    cfg.timers.id_interval = 1.5;
    cfg.timers.id_anxious = 0.0;
    cfg.events.n_mandatory_ids = 0;
    snprintf(cfg.events.impolite_id, sizeof(cfg.events.impolite_id), "id_voice");
    port_t *p = fresh_port(&cfg, vocab);
    prime(p);
    activity_pulse(p);

    cap_start();
    bool played = wait_for_log(p, "impolite_id, as there's no mandatory ID", 3000);
    CHECK(played, "the timer plays impolite_id");
    CHECK(port_ptt(p), "keyed up to do it");
    bool settled = wait_until_idle(p, 8000);
    CHECK(settled && !port_ptt(p), "released afterward");
    free(cap_stop());
    port_destroy(p);

    cfg.events.impolite_id[0] = '\0';
    p = fresh_port(&cfg, vocab);
    prime(p);
    activity_pulse(p);
    cap_start();
    bool warned = wait_for_log(p, "NO ID SENT", 3000);
    char *log = cap_stop();
    CHECK(warned, "with neither set, the log says no ID went out");
    CHECK(!port_ptt(p), "and nothing keys up");
    free(log);
    port_destroy(p);

    /* A solitary key-up with an initial ID owes nothing more. */
    cfg.timers.id_interval = 1.5;
    cfg.events.n_mandatory_ids = 1;
    snprintf(cfg.events.impolite_id, sizeof(cfg.events.impolite_id), "id_voice");
    p = fresh_port(&cfg, vocab);
    cap_start();
    prime(p);
    wait_ms(p, 3000);
    log = cap_stop();
    CHECK(count_occurrences(log, "id: initial") == 1 && count_occurrences(log, "id: mandatory") == 0,
          "initial ID on a solitary key-up: no further ID");
    free(log);
    port_destroy(p);

    /* Without an initial ID, the same key-up owes one at the timer. */
    cfg.events.n_initial_ids = 0;
    p = fresh_port(&cfg, vocab);
    cap_start();
    prime(p);
    bool owed = wait_for_log(p, "id: mandatory", 3000);
    log = cap_stop();
    CHECK(count_occurrences(log, "id: initial") == 0, "no initial ID: nothing plays on the key-up");
    CHECK(owed && port_ptt(p), "the timer keys up with the mandatory ID");
    free(log);
    wait_until_idle(p, 8000);
    port_destroy(p);

    /* ... or impolite_id, with no mandatory ID either. */
    cfg.events.n_mandatory_ids = 0;
    p = fresh_port(&cfg, vocab);
    prime(p);
    cap_start();
    owed = wait_for_log(p, "impolite_id, as there's no mandatory ID", 3000);
    free(cap_stop());
    CHECK(owed && port_ptt(p), "with no mandatory ID either, it uses impolite_id");
    wait_until_idle(p, 8000);
    port_destroy(p);

    /* ... or the anxious ID, if a tail reaches its window first. */
    cfg.events.n_mandatory_ids = 1;
    cfg.timers.id_interval = 3.0;
    cfg.timers.id_anxious = 2.5;
    snprintf(cfg.events.anxious_id, sizeof(cfg.events.anxious_id), "id_voice");
    p = fresh_port(&cfg, vocab);
    prime(p);
    wait_ms(p, 700);
    cap_start();
    activity_pulse(p);
    log = cap_stop();
    CHECK(count_occurrences(log, "id: anxious") == 1, "a tail in the window plays the anxious ID");
    free(log);
    port_destroy(p);
    cfg.events.n_initial_ids = 1;

    /* With no anxious ID, nothing plays early: the timer waits for the mandatory ID. */
    cfg.events.n_initial_ids = 1;
    cfg.events.anxious_id[0] = '\0';
    cfg.timers.id_interval = 2.0;
    cfg.timers.id_anxious = 1.0;
    p = fresh_port(&cfg, vocab);
    prime(p);
    activity_pulse(p);
    cap_start();
    wait_for_log(p, "id: mandatory", 3000);
    log = cap_stop();
    CHECK(count_occurrences(log, "id: anxious") == 0 && count_occurrences(log, "id: impolite") == 0,
          "with no anxious_id, it waits for the mandatory ID");
    CHECK(count_occurrences(log, "id: mandatory") == 1, "and that plays");
    free(log);
    wait_until_idle(p, 8000);
    port_destroy(p);

    printf(ok ? "[14] PASS\n" : "[14] FAIL\n");
    return ok;
}

static double g_activity_seconds;
static int    g_activity_count;

static void on_activity(void *arg, double seconds)
{
    (void)arg;
    g_activity_seconds += seconds;
    g_activity_count++;
}

static bool scenario_usage_counting(config_t *base, vocab_cache_t *vocab)
{
    printf("\n[12] usage counts transmissions, not kerchunks or lockouts\n");
    bool ok = true;
    config_t cfg = *base;
    cfg.timers.id_interval = 600.0;
    port_t *p = fresh_port(&cfg, vocab);
    port_set_activity_callback(p, on_activity, NULL);
    g_activity_seconds = 0.0;
    g_activity_count = 0;

    key_local(p, true);
    wait_ms(p, 1000);
    key_local(p, false);
    wait_until_idle(p, 3000);
    CHECK(g_activity_count == 1, "a transmission counts once (%d)", g_activity_count);
    CHECK(g_activity_seconds > 0.9 && g_activity_seconds < 1.1, "for its length (%.2f s)", g_activity_seconds);

    key_local(p, true);
    wait_ms(p, 50);
    key_local(p, false);
    wait_ms(p, 300);
    CHECK(g_activity_count == 1, "a kerchunk doesn't count (%d)", g_activity_count);

    key_local(p, true);
    wait_ms(p, 5500);        /* past the 5 s timeout */
    key_local(p, false);
    wait_until_idle(p, 5000);
    CHECK(g_activity_count == 1, "a timed-out transmission doesn't count (%d)", g_activity_count);

    port_on_link_keyup(p, true, g_now);
    wait_ms(p, 500);
    port_on_link_keyup(p, false, g_now);
    CHECK(g_activity_count == 2, "link traffic counts too (%d)", g_activity_count);

    port_destroy(p);
    printf(ok ? "[12] PASS\n" : "[12] FAIL\n");
    return ok;
}

/* ── driver ───────────────────────────────────────────────────────────── */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    config_t base;
    if (config_load(&base, "tests/harness.conf") != 0) {
        fprintf(stderr, "harness: failed to load tests/harness.conf -- run from the repo root\n");
        return 1;
    }

    vocab_cache_t *vocab;
    const char *dirs[] = { "vocab_8k" };
    if (vocab_cache_create(&vocab, dirs, 1) != 0) {
        fprintf(stderr, "harness: failed to load vocab_8k/ -- run from the repo root\n");
        return 1;
    }

    typedef bool (*scenario_fn)(config_t *, vocab_cache_t *);
    scenario_fn scenarios[] = {
        scenario_normal_flow_and_cw_survives,
        scenario_voice_mandatory_interrupt_fires_impolite,
        scenario_courtesy_tone_no_stacking,
        scenario_anxious_between_ct_delay_and_ct,
        scenario_kerchunk_ignored,
        scenario_timeout_recovery,
        scenario_impolite_resets_tx_activity,
        scenario_command_reply,
        scenario_link_state_courtesy_tone,
        scenario_tail_message_count,
        scenario_tail_message_min_interval,
        scenario_usage_counting,
        scenario_tail_message_rotation,
        scenario_empty_mandatory_uses_impolite,
    };
    int n = (int)(sizeof(scenarios) / sizeof(scenarios[0]));

    int pass = 0;
    for (int i = 0; i < n; i++)
        if (scenarios[i](&base, vocab))
            pass++;

    vocab_cache_destroy(vocab);

    printf("\n==== %d/%d scenarios passed ====\n", pass, n);
    return pass == n ? 0 : 1;
}
