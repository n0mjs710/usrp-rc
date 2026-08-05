/* Standalone test for the arrival-order FIFO jitter buffer (jitter_buffer.c).
 * Covers the behavior that replaced the seq-windowed design after the
 * 2026-08-05 incident: no sequence-number filtering, a small prefill ramp,
 * and bounded overflow that drops oldest-not-newest. Run via `make
 * check-jitter`. */

#include "../src/jitter_buffer.h"
#include "../src/usrp_protocol.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL - %s\n", msg); \
        failures++; \
    } else { \
        printf("  ok   - %s\n", msg); \
    } \
} while (0)

static void make_frame(int16_t *out, int16_t fill)
{
    for (unsigned i = 0; i < USRP_AUDIO_FRAMES; i++)
        out[i] = fill;
}

static void test_priming_then_flows(void)
{
    printf("\n[1] priming ramp: silence until prefill target reached, then real frames flow\n");
    jitter_buffer_t *jb;
    jitter_buffer_create(&jb, 40u); /* 2 frames */

    int16_t f1[USRP_AUDIO_FRAMES], f2[USRP_AUDIO_FRAMES], out[USRP_AUDIO_FRAMES];
    make_frame(f1, 111);
    make_frame(f2, 222);

    jitter_buffer_push(jb, f1);
    bool real = jitter_buffer_pull(jb, out);
    CHECK(!real, "pull with only 1/2 prefill frames buffered returns silence (still priming)");

    jitter_buffer_push(jb, f2);
    real = jitter_buffer_pull(jb, out);
    CHECK(real, "pull once prefill target reached returns a real frame");
    CHECK(out[0] == 111, "first real frame out is the first one pushed (arrival order, not seq order)");

    jitter_buffer_destroy(jb);
}

static void test_steady_state_order(void)
{
    printf("\n[2] steady-state: frames drain in strict arrival (push) order\n");
    jitter_buffer_t *jb;
    jitter_buffer_create(&jb, 20u); /* 1 frame prefill -- minimal ramp */

    int16_t out[USRP_AUDIO_FRAMES];
    for (int16_t v = 0; v < 5; v++) {
        int16_t f[USRP_AUDIO_FRAMES];
        make_frame(f, v);
        jitter_buffer_push(jb, f);
    }

    int ok = 1;
    for (int16_t v = 0; v < 5; v++) {
        bool real = jitter_buffer_pull(jb, out);
        if (!real || out[0] != v) ok = 0;
    }
    CHECK(ok, "5 pushed frames drain in the exact order pushed");

    jitter_buffer_destroy(jb);
}

static void test_overflow_drops_oldest(void)
{
    printf("\n[3] overflow: pushing past capacity drops oldest, keeps freshest, counts it\n");
    jitter_buffer_t *jb;
    jitter_buffer_create(&jb, 20u);

    /* Capacity is 16 frames (MAX_SLOTS). Push 20 without pulling. */
    for (int16_t v = 0; v < 20; v++) {
        int16_t f[USRP_AUDIO_FRAMES];
        make_frame(f, v);
        jitter_buffer_push(jb, f);
    }

    uint64_t overflow = jitter_buffer_overflow_count(jb);
    CHECK(overflow == 4, "4 pushes beyond the 16-frame capacity were counted as overflow");

    int16_t out[USRP_AUDIO_FRAMES];
    bool real = jitter_buffer_pull(jb, out);
    CHECK(real && out[0] == 4, "oldest surviving frame is #4 (0..3 were dropped to make room)");

    jitter_buffer_destroy(jb);
}

static void test_genuine_underrun(void)
{
    printf("\n[4] genuine underrun: buffer runs dry mid-stream after priming\n");
    jitter_buffer_t *jb;
    jitter_buffer_create(&jb, 20u); /* 1 frame prefill */

    int16_t f[USRP_AUDIO_FRAMES], out[USRP_AUDIO_FRAMES];
    make_frame(f, 42);
    jitter_buffer_push(jb, f);

    bool real = jitter_buffer_pull(jb, out);
    CHECK(real, "prefill satisfied, first pull is real");

    jitter_buffer_reset_silence_count(jb);
    real = jitter_buffer_pull(jb, out);
    CHECK(!real, "pull with nothing buffered (post-priming) returns silence");
    int16_t all_zero = 0;
    for (unsigned i = 0; i < USRP_AUDIO_FRAMES; i++) all_zero |= out[i];
    CHECK(all_zero == 0, "underrun silence is actually zeroed, not stale data");

    jitter_buffer_latch_silence(jb);
    uint64_t silence = jitter_buffer_latched_silence_count(jb);
    CHECK(silence == 1, "genuine underrun is counted as silence (unlike priming silence)");

    jitter_buffer_destroy(jb);
}

static void test_flush_rearms_priming(void)
{
    printf("\n[5] flush mid-stream drains pending audio and re-arms the prefill ramp\n");
    jitter_buffer_t *jb;
    jitter_buffer_create(&jb, 40u); /* 2 frame prefill */

    int16_t f[USRP_AUDIO_FRAMES], out[USRP_AUDIO_FRAMES];
    make_frame(f, 7);
    jitter_buffer_push(jb, f);
    jitter_buffer_push(jb, f);
    jitter_buffer_pull(jb, out); /* consumes prefill, now steady-state */

    jitter_buffer_flush(jb);

    make_frame(f, 9);
    jitter_buffer_push(jb, f);
    bool real = jitter_buffer_pull(jb, out);
    CHECK(!real, "after flush, a single push is not enough -- priming ramp restarted");

    jitter_buffer_push(jb, f);
    real = jitter_buffer_pull(jb, out);
    CHECK(real, "second push after flush satisfies the re-armed prefill target");

    jitter_buffer_destroy(jb);
}

static void test_no_sequence_gating(void)
{
    printf("\n[6] no sequence-number gating: this is the whole point of the rewrite\n");
    jitter_buffer_t *jb;
    jitter_buffer_create(&jb, 20u);

    /* Under the old design, pushing frames with wildly discontinuous or
     * out-of-window sequence numbers would get silently discarded as
     * "late". This buffer takes no sequence number at all -- there is
     * nothing to discard on that basis. Any frame pushed is accepted in
     * arrival order, full stop. */
    int16_t f[USRP_AUDIO_FRAMES], out[USRP_AUDIO_FRAMES];
    make_frame(f, 1);
    jitter_buffer_push(jb, f); /* would have been "seq 0" */
    make_frame(f, 2);
    jitter_buffer_push(jb, f); /* would have been e.g. "seq 999999" -- huge jump, doesn't matter */

    bool real = jitter_buffer_pull(jb, out);
    CHECK(real && out[0] == 1, "first frame accepted regardless of any notion of sequence continuity");
    real = jitter_buffer_pull(jb, out);
    CHECK(real && out[0] == 2, "second frame accepted too -- arrival order is the only order that exists");

    uint64_t overflow = jitter_buffer_overflow_count(jb);
    CHECK(overflow == 0, "no overflow/discard occurred -- a seq discontinuity is not a buffer event here");

    jitter_buffer_destroy(jb);
}

int main(void)
{
    test_priming_then_flows();
    test_steady_state_order();
    test_overflow_drops_oldest();
    test_genuine_underrun();
    test_flush_rearms_priming();
    test_no_sequence_gating();

    printf("\n==== %s ====\n", failures == 0 ? "all checks passed" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}
