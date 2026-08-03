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
 * Run from the repo root (paths to tests/harness.toml and vocab_8k/ are
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
 * mirroring main.c's pace_tick() exactly -- pulls one ctrl-buffer frame
 * only when neither real-audio gate is open. Returns true if that pulled
 * frame carried audible (non-all-zero) content. */
static bool tick(port_t *p)
{
    g_now += TICK_MS;
    port_check_timers(p, g_now);
    if (port_ptt(p) && !port_mmdvm_gate_open(p) && !port_link_gate_open(p)) {
        int16_t frame[160];
        port_ctrl_pull(p, frame, g_now);
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

/* ── driver ───────────────────────────────────────────────────────────── */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    config_t base;
    if (config_load(&base, "tests/harness.toml") != 0) {
        fprintf(stderr, "harness: failed to load tests/harness.toml -- run from the repo root\n");
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
