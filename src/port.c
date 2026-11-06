/*
 * Repeater controller state machine.
 *
 * Work that has to wait is a continuation: ctrl.on_drain runs when the controller
 * audio buffer empties, and pad_cont when a pre/post-message pad expires. Only one
 * of each is in flight at a time, as all controller audio goes through the one
 * buffer.
 */

#include "port.h"
#include "message.h"
#include "sbuf.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*port_cont_fn)(port_t *p, uint64_t now);

typedef struct {
    sbuf_t       buf;
    size_t       pos;
    port_cont_fn on_drain;
} ctrl_buf_t;

struct port {
    const config_t *cfg;
    const config_events_t *ev;   /* what plays for each event, right now */
    vocab_cache_t  *vocab;

    port_state_t  state;

    bool          mmdvm_active;
    bool          link_active;
    uint64_t      cor_up_ms;
    port_source_t last_source;

    /* Time-out timers, one for each source. The local timer resets at the courtesy
     * tone and when a local station takes over from the link. The link timer resets
     * each time the link unkeys, as link stations can't hear the courtesy tone. */
    double        local_tot_used_s;
    uint64_t      local_tot_start_ms;
    bool          local_timed_out;
    bool          link_timed_out;

    /* Repeater down: no path through the repeater. The receiver is still decoded
     * for commands. */
    bool          down;

    int  id_rot_initial;
    int  id_rot_mandatory;
    bool tx_activity;
    bool initial_id_pending;
    bool anxious_id_armed;
    bool voice_id_active;
    bool id_playing;        /* true for the entire lifecycle of any ID job
                              * (mandatory/initial/anxious/startup),
                              * including its post-message pad phase. */
    bool impolite_id_playing;
    int  id_epoch;

    /* timers — absolute monotonic ms deadlines; 0 = not armed */
    uint64_t hang_deadline;
    uint64_t ct_delay_deadline;
    uint64_t local_timeout_deadline;
    uint64_t link_timeout_deadline;
    uint64_t id_deadline;
    uint64_t id_sub_deadline;
    uint64_t pad_deadline;
    port_cont_fn pad_cont;

    bool ptt;

    ctrl_buf_t ctrl;

    /* impolite-ID mix buffer (additive overlay on repeat/link-forward/
     * controller-pull frames) */
    int16_t     *mix_buf;
    size_t       mix_len;
    size_t       mix_pos;
    port_cont_fn mix_on_drain;

    bool         link_up;

    int          tails_since_message;
    uint64_t     tail_last_ms;
    int          tail_rot;
    char         tail_name[CFG_NAME_STR];

    /* command reply waiting for the tail; malloc'd, NULL = none */
    int16_t     *reply;
    size_t       reply_len;

    /* single-slot job context for standalone (was_ptt==false) transmissions */
    int          job_epoch;
    port_state_t job_state_before;
    bool         job_was_ptt;
    bool         job_post_pad_needed;

    void (*activity_cb)(void *arg, double seconds);
    void  *activity_arg;

    void (*set_ptt_cb)(void *arg, bool active, const char *reason);
    void  *set_ptt_arg;
};

/* ── ctrl buffer ────────────────────────────────────────────────────────── */

static void ctrl_buf_queue(port_t *p, const int16_t *samples, size_t n)
{
    sbuf_append(&p->ctrl.buf, samples, n);
}

static bool ctrl_buf_playing(const port_t *p)
{
    return p->ctrl.pos < p->ctrl.buf.len;
}

static void ctrl_buf_clear(port_t *p)
{
    p->ctrl.buf.len    = 0;
    p->ctrl.pos        = 0;
    p->ctrl.on_drain   = NULL;
    p->voice_id_active = false;
    p->id_playing      = false;
}

static void ctrl_buf_pull_frame(port_t *p, int16_t out[160], uint64_t now)
{
    ctrl_buf_t *c = &p->ctrl;
    size_t filled = 0;

    /* Loop rather than single-shot: on_drain (fired below) can queue the
     * next message synchronously (e.g. startup message -> initial ID), and
     * that new audio needs to fill out the *rest of this same frame*, not
     * wait for the next 20 ms tick -- otherwise the tail of every frame
     * that ends mid-buffer gets silence-padded and the first samples of
     * whatever chains on next are silently lost, producing an audible
     * dropout at every message handoff. */
    while (filled < 160) {
        size_t avail = c->buf.len - c->pos;
        size_t n     = avail < (160 - filled) ? avail : (160 - filled);
        if (n > 0) {
            memcpy(out + filled, c->buf.data + c->pos, n * sizeof(int16_t));
            c->pos += n;
            filled += n;
        }

        if (c->pos < c->buf.len)
            break;   /* frame full; remaining buffer carries to the next tick */

        /* Fires on_drain once pos catches up to len -- including
         * immediately, when a message rendered to nothing (missing/failed
         * clip) and len was already 0. A `len > 0` guard here would
         * silently swallow that case: on_drain would never fire, and a
         * standalone ID job's PTT would never release, keying the
         * transmitter on silence indefinitely. */
        c->buf.len = 0;
        c->pos     = 0;
        p->voice_id_active = false;
        p->id_playing      = false;
        port_cont_fn cb = c->on_drain;
        c->on_drain = NULL;
        if (!cb)
            break;
        cb(p, now);
        if (c->buf.len == 0)
            break;   /* nothing new queued */
    }

    if (filled < 160)
        memset(out + filled, 0, (160 - filled) * sizeof(int16_t));
}

/* ── misc helpers ───────────────────────────────────────────────────────── */

/* A timed-out source no longer counts as active: it's neither repeated nor holds the
 * repeater up. */
static bool local_active(const port_t *p) { return !p->down && p->mmdvm_active && !p->local_timed_out; }
static bool link_active(const port_t *p)  { return !p->down && p->link_active && !p->link_timed_out; }
static bool cor_active(const port_t *p)   { return local_active(p) || link_active(p); }

/* Where the repeater rests when nothing is being repeated. */
static port_state_t rest_state(const port_t *p)
{
    return p->local_timed_out || p->link_timed_out ? PORT_TIMEOUT : PORT_IDLE;
}

static void set_ptt(port_t *p, bool active, const char *reason)
{
    bool changed = (p->ptt != active);
    p->ptt = active;
    if (changed && p->set_ptt_cb)
        p->set_ptt_cb(p->set_ptt_arg, active, reason);
}

/* False if there was nothing to play. */
static bool queue_message(port_t *p, const char *name, double morse_level_override)
{
    if (!name || !name[0])
        return false;
    rendered_audio_t r = message_render(p->cfg, p->vocab, name, morse_level_override);
    if (!r.samples)
        return false;
    ctrl_buf_queue(p, r.samples, r.n);
    free(r.samples);
    return true;
}

static void start_pad(port_t *p, uint64_t now, int ms, port_cont_fn then)
{
    p->pad_deadline = now + (uint64_t)ms;
    p->pad_cont     = then;
}

static void reply_discard(port_t *p)
{
    free(p->reply);
    p->reply     = NULL;
    p->reply_len = 0;
}

static void schedule_hang(port_t *p, uint64_t now)
{
    p->hang_deadline = now + (uint64_t)(p->cfg->timers.hang * 1000.0);
    LOGI("hang: armed  %.1fs\n", p->cfg->timers.hang);
}

static void schedule_ct_delay(port_t *p, uint64_t now)
{
    p->ct_delay_deadline = now + (uint64_t)(p->cfg->timers.ct_delay * 1000.0);
}

static void schedule_id(port_t *p, uint64_t now)
{
    p->id_deadline     = now + (uint64_t)(p->cfg->timers.id_interval * 1000.0);
    p->id_sub_deadline = 0;
    p->anxious_id_armed = false;
    p->voice_id_active  = false;
    p->id_playing       = false;

    double lead = p->cfg->timers.id_anxious;
    if (p->ev->anxious_id[0] && lead > 0.0 && lead < p->cfg->timers.id_interval) {
        p->id_sub_deadline = now + (uint64_t)((p->cfg->timers.id_interval - lead) * 1000.0);
    }
}

/* ── transitions ────────────────────────────────────────────────────────── */

static void do_impolite_id(port_t *p, uint64_t now);
static void impolite_id_after_mix(port_t *p, uint64_t now);
static void ct_after_drain(port_t *p, uint64_t now);

/* A keyup over any ID: voice gives way to the impolite ID, and CW carries on under the
 * repeated audio. Either way it finishes as the impolite ID does, and counts as sent. */
static void id_keyed_over(port_t *p, uint64_t now)
{
    if (p->voice_id_active) {
        do_impolite_id(p, now);
        return;
    }
    if (p->impolite_id_playing)
        return;   /* already identifying under the audio */
    size_t n = p->ctrl.buf.len - p->ctrl.pos;
    int16_t *rest = malloc(n * sizeof(int16_t));
    if (!rest)
        return;
    memcpy(rest, p->ctrl.buf.data + p->ctrl.pos, n * sizeof(int16_t));
    LOGI("id: keyed over, CW carries on underneath\n");
    free(p->mix_buf);
    p->mix_buf = rest;
    p->mix_len = n;
    p->mix_pos = 0;
    p->impolite_id_playing = true;
    p->mix_on_drain = impolite_id_after_mix;
}

static void port_transition(port_t *p, port_state_t new_state, uint64_t now)
{
    port_state_t old = p->state;
    p->state = new_state;

    if (new_state == PORT_IDLE)
        p->local_tot_used_s = 0.0;

    if (new_state == PORT_ACTIVE) {
        p->tx_activity = true;
        if (ctrl_buf_playing(p)) {
            if (p->id_playing)
                id_keyed_over(p, now);
            p->id_epoch++;
            ctrl_buf_clear(p);
        }
        if (old == PORT_IDLE && p->id_deadline == 0 && !p->impolite_id_playing)
            p->initial_id_pending = true;
    }
}

/* ── timeout announce / recovery ───────────────────────────────────────── */

static void timeout_announce_after_drain(port_t *p, uint64_t now)
{
    (void)now;
    if (p->state == PORT_TIMEOUT)
        set_ptt(p, false, "timeout-announced");
}

static void do_timeout_announce(port_t *p, uint64_t now)
{
    if (queue_message(p, p->ev->timeout_message, -1.0))
        p->ctrl.on_drain = timeout_announce_after_drain;
    else
        timeout_announce_after_drain(p, now);
}

static void timeout_recovery_after_drain(port_t *p, uint64_t now)
{
    if (p->state == PORT_TAIL)
        schedule_hang(p, now);
}

static void do_timeout_recovery(port_t *p, uint64_t now)
{
    p->tx_activity = true;
    if (p->id_deadline == 0)
        p->initial_id_pending = true;

    if (queue_message(p, p->ev->timeout_cancel_message, -1.0))
        p->ctrl.on_drain = timeout_recovery_after_drain;
    else
        timeout_recovery_after_drain(p, now);
}

/* ── courtesy tone ─────────────────────────────────────────────────────── */

/* Each case has its own setting, and an empty one plays no tone. */
static const char *courtesy_tone(const port_t *p)
{
    const config_events_t *ev = p->ev;
    if (p->last_source == SRC_LINK)
        return ev->ct_link_message;
    return p->link_up ? ev->ct_link_on_message : ev->ct_link_off_message;
}

/* The local time-out timer resets here, whether or not a tone is set. */
static void play_courtesy_tone(port_t *p, uint64_t now)
{
    p->local_tot_used_s = 0.0;
    queue_message(p, courtesy_tone(p), -1.0);
    if (ctrl_buf_playing(p))
        p->ctrl.on_drain = ct_after_drain;
    else
        schedule_hang(p, now);
}

/* ── impolite ID (mix buffer, not ctrl buffer — see port.h) ──────────────── */

static void impolite_id_after_mix(port_t *p, uint64_t now)
{
    p->impolite_id_playing = false;
    p->tx_activity         = false;
    schedule_id(p, now);
    if (p->state == PORT_TAIL && !cor_active(p)) {
        if (p->reply) {
            schedule_ct_delay(p, now);   /* the reply goes first, as on any other tail */
            return;
        }
        play_courtesy_tone(p, now);
    }
}

static void do_impolite_id(port_t *p, uint64_t now)
{
    const char *name = p->ev->impolite_id;   /* required, so never empty */
    LOGI("id: impolite  '%s'\n", name);
    p->impolite_id_playing = true;
    rendered_audio_t r = message_render(p->cfg, p->vocab, name,
                                        p->cfg->audio.impolite_morse_level);
    free(p->mix_buf);
    p->mix_buf = r.samples;
    p->mix_len = r.n;
    p->mix_pos = 0;

    if (!r.samples) {
        impolite_id_after_mix(p, now);
        return;
    }
    p->mix_on_drain = impolite_id_after_mix;
}

/* ── mandatory ID (ctrl buffer; was_ptt-dependent pad handling) ──────────── */

static void mandatory_id_finish(port_t *p, uint64_t now)
{
    if (p->id_epoch != p->job_epoch)
        return;
    p->tx_activity = false;
    schedule_id(p, now);
}

static void mandatory_id_after_post_pad(port_t *p, uint64_t now)
{
    if (p->state == p->job_state_before)
        set_ptt(p, false, "mandatory-id-done");
    mandatory_id_finish(p, now);
}

static void mandatory_id_after_drain(port_t *p, uint64_t now)
{
    if (p->job_post_pad_needed && p->cfg->audio.post_message_ms > 0) {
        start_pad(p, now, p->cfg->audio.post_message_ms, mandatory_id_after_post_pad);
        return;
    }
    mandatory_id_after_post_pad(p, now);
}

/* What the ID timer plays with nobody talking. With no mandatory ID, impolite_id
 * (which is required) stands in, because the station still has to identify. */
static const char *mandatory_id_next(const port_t *p)
{
    if (p->ev->n_mandatory_ids > 0)
        return p->ev->mandatory_ids[p->id_rot_mandatory % p->ev->n_mandatory_ids];
    return p->ev->impolite_id;
}

static void mandatory_id_render_and_queue(port_t *p, uint64_t now)
{
    const char *name = mandatory_id_next(p);
    if (p->ev->n_mandatory_ids > 0) {
        int idx = p->id_rot_mandatory % p->ev->n_mandatory_ids;
        p->id_rot_mandatory = (idx + 1) % p->ev->n_mandatory_ids;
        LOGI("id: mandatory  '%s'  (%d/%d)\n", name, idx + 1, p->ev->n_mandatory_ids);
    } else {
        LOGI("id: mandatory  '%s'  (impolite_id, as there's no mandatory ID)\n", name);
    }
    /* Keyed already, the ID joins whatever is playing and counts as sent; it's
     * still an ID playing until the buffer drains, should anyone key over it. */
    if (p->job_was_ptt)
        mandatory_id_finish(p, now);
    p->voice_id_active = message_has_voice(p->cfg, name);
    p->id_playing = true;
    queue_message(p, name, -1.0);
    if (!p->job_was_ptt)
        p->ctrl.on_drain = mandatory_id_after_drain;
}

static void do_mandatory_id(port_t *p, uint64_t now)
{
    p->job_epoch        = p->id_epoch;
    p->job_was_ptt       = p->ptt;
    p->job_state_before  = p->state;

    p->job_post_pad_needed = !p->job_was_ptt && message_needs_padding(p->cfg, mandatory_id_next(p));

    if (!p->job_was_ptt) {
        set_ptt(p, true, "mandatory-id");
        if (p->job_post_pad_needed && p->cfg->audio.pre_message_ms > 0) {
            start_pad(p, now, p->cfg->audio.pre_message_ms, mandatory_id_render_and_queue);
            return;
        }
    }
    mandatory_id_render_and_queue(p, now);
}

/* ── initial ID (PTT already on; unconditional pre-pad, own drain-await) ── */

static void initial_id_after_drain(port_t *p, uint64_t now)
{
    p->voice_id_active = false;
    p->id_playing      = false;
    if (p->id_epoch != p->job_epoch) {
        if (p->id_deadline == 0)
            schedule_id(p, now);
        return;
    }
    if (p->state != PORT_ACTIVE)
        p->tx_activity = false;
    if (p->id_deadline == 0)
        schedule_id(p, now);
    if (p->state == PORT_TAIL && !cor_active(p))
        schedule_ct_delay(p, now);
}

/* The next initial ID, or NULL if none are set. */
static const char *initial_id_next(port_t *p)
{
    int n = p->ev->n_initial_ids;
    if (n == 0)
        return NULL;
    int idx = p->id_rot_initial % n;
    p->id_rot_initial = (idx + 1) % n;
    LOGI("id: initial  '%s'  (%d/%d)\n", p->ev->initial_ids[idx], idx + 1, n);
    return p->ev->initial_ids[idx];
}

static void initial_id_render_and_queue(port_t *p, uint64_t now)
{
    const char *name = initial_id_next(p);
    if (!name) {
        /* An initial ID covers a solitary key-up, as the end of communications. Without
         * one, the station hasn't identified, so the timer still owes an ID. */
        if (p->id_deadline == 0)
            schedule_id(p, now);
        p->tx_activity = true;
        if (p->state == PORT_TAIL && !cor_active(p))
            schedule_ct_delay(p, now);
        return;
    }
    p->voice_id_active = message_has_voice(p->cfg, name);
    p->id_playing = true;
    queue_message(p, name, -1.0);
    p->ctrl.on_drain = initial_id_after_drain;
}

static void do_initial_id(port_t *p, uint64_t now)
{
    p->job_epoch = p->id_epoch;
    int pre_ms = p->cfg->audio.pre_message_ms;
    if (pre_ms > 0 && p->ev->n_initial_ids > 0) {
        start_pad(p, now, pre_ms, initial_id_render_and_queue);
        return;
    }
    initial_id_render_and_queue(p, now);
}

/* ── anxious ID (single message; PTT already on) ──────────────────────────── */

static void anxious_id_finish(port_t *p, uint64_t now)
{
    p->voice_id_active = false;
    p->id_playing      = false;
    if (p->id_epoch != p->job_epoch)
        return;
    p->tx_activity = false;
    schedule_id(p, now);
    if (p->state == PORT_TAIL && !cor_active(p))
        schedule_ct_delay(p, now);
}

static void anxious_id_render_and_queue(port_t *p, uint64_t now)
{
    const char *name = p->ev->anxious_id;
    if (name[0]) {
        LOGI("id: anxious  '%s'\n", name);
        p->voice_id_active = message_has_voice(p->cfg, name);
        p->id_playing = true;
        queue_message(p, name, -1.0);
        p->ctrl.on_drain = anxious_id_finish;
        return;
    }
    anxious_id_finish(p, now);
}

static void do_anxious_id(port_t *p, uint64_t now)
{
    p->job_epoch = p->id_epoch;
    int pre_ms = p->cfg->audio.pre_message_ms;
    if (pre_ms > 0) {
        start_pad(p, now, pre_ms, anxious_id_render_and_queue);
        return;
    }
    anxious_id_render_and_queue(p, now);
}

/* ── startup sequence ──────────────────────────────────────────────────── */

static void startup_finish(port_t *p, uint64_t now)
{
    set_ptt(p, false, "startup-done");
    port_transition(p, PORT_IDLE, now);
}

static void startup_after_id_drain(port_t *p, uint64_t now)
{
    p->voice_id_active = false;
    p->id_playing      = false;
    schedule_id(p, now);
    p->tx_activity = false;

    int post_ms = p->cfg->audio.post_message_ms;
    if (post_ms > 0) {
        start_pad(p, now, post_ms, startup_finish);
        return;
    }
    startup_finish(p, now);
}

static void startup_queue_id(port_t *p, uint64_t now)
{
    const char *name = initial_id_next(p);
    if (!name) {
        /* The startup message transmitted without identifying, so an ID is owed. */
        startup_after_id_drain(p, now);
        p->tx_activity = true;
        return;
    }
    p->voice_id_active = message_has_voice(p->cfg, name);
    p->id_playing = true;
    queue_message(p, name, -1.0);
    p->ctrl.on_drain = startup_after_id_drain;
}

static void startup_render_message(port_t *p, uint64_t now)
{
    if (queue_message(p, p->ev->startup_message, -1.0))
        p->ctrl.on_drain = startup_queue_id;
    else
        startup_queue_id(p, now);
}

void port_start(port_t *p, uint64_t now)
{
    if (p->down) {
        LOGI("port: repeater down — no startup message\n");
        return;
    }
    if (!p->ev->startup_message[0]) {
        LOGI("port: no startup_message configured — starting quietly in IDLE\n");
        return;
    }
    set_ptt(p, true, "startup");
    int pre_ms = p->cfg->audio.pre_message_ms;
    if (pre_ms > 0 && message_needs_padding(p->cfg, p->ev->startup_message)) {
        start_pad(p, now, pre_ms, startup_render_message);
        return;
    }
    startup_render_message(p, now);
}

/* ── courtesy tone / hang timer callbacks ─────────────────────────────────── */

/* Deferred continuation for the CT audio queued in on_ct_delay() and
 * impolite_id_after_mix() -- hang must not start counting until the CT
 * has actually finished playing, or it can overlap/cut off the CT. */
static void ct_after_drain(port_t *p, uint64_t now)
{
    if (p->state == PORT_TAIL)
        schedule_hang(p, now);
}

/* Same pause before the CT as follows any other message. */
static void reply_after_drain(port_t *p, uint64_t now)
{
    if (p->state == PORT_TAIL && !cor_active(p))
        schedule_ct_delay(p, now);
}

static void on_ct_delay(port_t *p, uint64_t now)
{
    p->ct_delay_deadline = 0;
    if (p->state != PORT_TAIL)
        return;
    /* An ID that came due in the delay finishes first, then the same pause as after
     * any other message. */
    if (ctrl_buf_playing(p)) {
        if (!p->ctrl.on_drain)
            p->ctrl.on_drain = reply_after_drain;
        return;
    }

    if (p->reply) {
        ctrl_buf_queue(p, p->reply, p->reply_len);
        reply_discard(p);
        p->ctrl.on_drain = reply_after_drain;
        return;
    }

    if (p->anxious_id_armed) {
        p->anxious_id_armed = false;
        do_anxious_id(p, now);   /* anxious_id_finish re-arms a fresh
                                   * ct_delay, so the CT lands after the
                                   * same pause as it would following any
                                   * other message */
        return;
    }

    play_courtesy_tone(p, now);
}

/* ── tail message ────────────────────────────────────────────────────────── */

static void tail_finish(port_t *p, uint64_t now)
{
    if (cor_active(p))
        return;
    set_ptt(p, false, "tail-message-done");
    if (p->state == PORT_TAIL) {
        port_state_t rest = rest_state(p);
        LOGI("state: TAIL -> %s  (tail message done)\n", rest == PORT_TIMEOUT ? "TIMEOUT" : "IDLE");
        port_transition(p, rest, now);
    }
}

static void tail_after_drain(port_t *p, uint64_t now)
{
    int post_ms = p->cfg->audio.post_message_ms;
    if (post_ms > 0 && message_needs_padding(p->cfg, p->tail_name)) {
        start_pad(p, now, post_ms, tail_finish);
        return;
    }
    tail_finish(p, now);
}

/* Returns false if there was nothing to play. */
static bool tail_play(port_t *p, uint64_t now)
{
    int idx = p->tail_rot % p->ev->n_tail_messages;
    /* A copy, as the schedule can change the list while this plays. */
    snprintf(p->tail_name, sizeof(p->tail_name), "%s", p->ev->tail_messages[idx]);
    if (!queue_message(p, p->tail_name, -1.0))
        return false;
    p->tail_rot = (idx + 1) % p->ev->n_tail_messages;
    LOGI("tail: '%s'\n", p->tail_name);
    p->ctrl.on_drain = tail_after_drain;
    p->tails_since_message = 0;
    p->tail_last_ms = now;
    return true;
}

static void on_hang(port_t *p, uint64_t now)
{
    p->hang_deadline = 0;
    if (cor_active(p))
        return;
    /* Never drop PTT over audio: an ID that came due in the hang finishes first. */
    if (ctrl_buf_playing(p)) {
        if (!p->ctrl.on_drain)
            p->ctrl.on_drain = on_hang;
        return;
    }
    reply_discard(p);

    /* Checked once, as each hang ends: enough tails and enough time since a message
     * last started. A keyup over one doesn't undo that it played. */
    if (p->ev->n_tail_messages > 0 && p->cfg->timers.tail_every > 0) {
        uint64_t min_ms = (uint64_t)(p->cfg->timers.tail_min_interval * 1000.0);
        bool too_soon = p->tail_last_ms && now - p->tail_last_ms < min_ms;
        if (++p->tails_since_message >= p->cfg->timers.tail_every && !too_soon && tail_play(p, now))
            return;
    }

    port_state_t rest = rest_state(p);
    LOGI("state: TAIL -> %s  (hang expired)\n", rest == PORT_TIMEOUT ? "TIMEOUT" : "IDLE");
    set_ptt(p, false, "hang-expired");
    port_transition(p, rest, now);
}

static void on_timeout(port_t *p, port_source_t source, uint64_t now)
{
    if (source == SRC_LOCAL) {
        p->local_timeout_deadline = 0;
        p->local_timed_out        = true;
        p->local_tot_used_s       = 0.0;
    } else {
        p->link_timeout_deadline = 0;
        p->link_timed_out        = true;
    }
    const char *src = source == SRC_LOCAL ? "local" : "link";

    /* The other source carries on being repeated. */
    if (cor_active(p)) {
        LOGW("timeout: %s timed out; repeating %s only until it unkeys\n",
             src, source == SRC_LOCAL ? "link" : "local");
        return;
    }
    LOGW("state: -> TIMEOUT  (%s tx time exceeded)\n", src);
    port_transition(p, PORT_TIMEOUT, now);
    ctrl_buf_clear(p);
    reply_discard(p);
    do_timeout_announce(p, now);
}

static void on_id(port_t *p, uint64_t now)
{
    p->id_deadline      = 0;
    p->id_sub_deadline  = 0;
    p->anxious_id_armed = false;

    if (p->state == PORT_ACTIVE)
        p->tx_activity = true;
    if (!p->tx_activity)
        return;

    if (p->state == PORT_ACTIVE)
        do_impolite_id(p, now);
    else
        do_mandatory_id(p, now);
}

/* ── COR edge handling ─────────────────────────────────────────────────── */

static void cor_active_edge(port_t *p, port_source_t source, uint64_t now)
{
    p->cor_up_ms         = now;
    p->hang_deadline     = 0;
    p->ct_delay_deadline = 0;

    /* A keyup interrupts a reply that hasn't started; one already playing is
     * cut when the transition to ACTIVE clears the ctrl buffer. */
    reply_discard(p);

    const char *src = source == SRC_LOCAL ? "local" : "link";
    const char *action = source == SRC_LOCAL
        ? (p->cfg->link.enabled && p->link_up ? "repeat + link-fwd" : "repeat")
        : "rf-fwd";
    LOGI("cor: ACTIVE  source=%s  -> %s\n", src, action);

    if (p->state == PORT_IDLE || p->state == PORT_TAIL || p->state == PORT_TIMEOUT) {
        set_ptt(p, true, source == SRC_LOCAL ? "repeat" : "link-fwd");
        port_transition(p, PORT_ACTIVE, now);
    }
}

static void cor_idle_edge(port_t *p, port_source_t source, uint64_t now)
{
    double duration_s = (double)(now - p->cor_up_ms) / 1000.0;
    const char *src = source == SRC_LOCAL ? "local" : "link";
    LOGI("cor: IDLE  source=%s  held=%.1fs\n", src, duration_s);

    if (p->state == PORT_ACTIVE || p->state == PORT_TAIL) {
        /* A valid command is never a kerchunk, however quickly it was entered. */
        if (p->state == PORT_ACTIVE && duration_s < p->cfg->timers.kerchunk && !p->reply) {
            LOGD("cor: kerchunk (%.1fs < %.1fs)  — ignored\n",
                    duration_s, p->cfg->timers.kerchunk);
            p->local_tot_used_s    = 0.0;
            p->initial_id_pending  = false;
            if (p->id_deadline == 0)
                schedule_id(p, now);
            set_ptt(p, false, "kerchunk");
            port_transition(p, rest_state(p), now);
        } else {
            if (p->activity_cb)
                p->activity_cb(p->activity_arg, duration_s);
            port_transition(p, PORT_TAIL, now);
            if (p->initial_id_pending) {
                p->initial_id_pending = false;
                do_initial_id(p, now);
            } else if (!p->impolite_id_playing) {
                schedule_ct_delay(p, now);
            }
        }
    }
}

/* A timed-out source has unkeyed. Recovery plays once nothing is timed out and nothing
 * else is being repeated. */
static void timeout_released(port_t *p, port_source_t source, uint64_t now)
{
    LOGI("timeout: %s released\n", source == SRC_LOCAL ? "local" : "link");
    if (p->state != PORT_TIMEOUT || p->local_timed_out || p->link_timed_out)
        return;
    LOGI("state: TIMEOUT -> TAIL  (recovered)\n");
    set_ptt(p, true, "timeout-recovery");
    port_transition(p, PORT_TAIL, now);
    do_timeout_recovery(p, now);
}

static void start_local_timer(port_t *p, uint64_t now)
{
    double limit = p->cfg->timers.timeout;
    p->local_tot_start_ms = now;
    if (limit <= 0.0) {
        p->local_timeout_deadline = 0;
        return;
    }
    double left = limit - p->local_tot_used_s;
    p->local_timeout_deadline = now + (uint64_t)((left > 0.0 ? left : 0.0) * 1000.0);
}

void port_on_mmdvm_keyup(port_t *p, bool active, uint64_t now)
{
    bool old_combined = cor_active(p);
    bool released = !active && p->local_timed_out;

    if (active && p->down) {
        p->local_timeout_deadline = 0;
    } else if (active) {
        /* A local station taking over from the link starts with a full timer. */
        if (p->link_active || p->last_source == SRC_LINK)
            p->local_tot_used_s = 0.0;
        start_local_timer(p, now);
    } else {
        if (!p->local_timed_out && p->mmdvm_active)
            p->local_tot_used_s += (double)(now - p->local_tot_start_ms) / 1000.0;
        p->local_timeout_deadline = 0;
        p->local_timed_out = false;
    }
    p->mmdvm_active = active;

    /* The courtesy tone goes with whichever source was active last. */
    if (active)
        p->last_source = SRC_LOCAL;
    else if (link_active(p))
        p->last_source = SRC_LINK;

    bool new_combined = cor_active(p);
    if (new_combined != old_combined) {
        if (new_combined) cor_active_edge(p, SRC_LOCAL, now);
        else              cor_idle_edge(p, SRC_LOCAL, now);
    }
    if (released)
        timeout_released(p, SRC_LOCAL, now);
}

void port_on_link_keyup(port_t *p, bool active, uint64_t now)
{
    bool old_combined = cor_active(p);
    bool released = !active && p->link_timed_out;

    if (active) {
        double limit = p->cfg->timers.link_timeout;
        p->link_timeout_deadline = limit > 0.0 && !p->down ? now + (uint64_t)(limit * 1000.0) : 0;
    } else {
        p->link_timeout_deadline = 0;
        p->link_timed_out = false;
    }
    p->link_active = active;

    if (active)
        p->last_source = SRC_LINK;
    else if (local_active(p))
        p->last_source = SRC_LOCAL;

    bool new_combined = cor_active(p);
    if (new_combined != old_combined) {
        if (new_combined) cor_active_edge(p, SRC_LINK, now);
        else              cor_idle_edge(p, SRC_LINK, now);
    }
    if (released)
        timeout_released(p, SRC_LINK, now);
}

static void timeout_reply_after_drain(port_t *p, uint64_t now)
{
    (void)now;
    if (p->state == PORT_TIMEOUT)
        set_ptt(p, false, "timeout-reply-done");
}

void port_queue_reply(port_t *p, int16_t *samples, size_t n)
{
    reply_discard(p);
    /* Down, only the up/down command itself transmits. */
    if (!samples || n == 0 || p->down) {
        if (p->down && samples)
            LOGI("port: repeater down, no reply\n");
        free(samples);
        return;
    }
    /* Timed out, nothing is being repeated, so there's no tail to wait for: key up,
     * reply and drop back. Receiver audio stays off. */
    if (p->state == PORT_TIMEOUT) {
        p->tx_activity = true;
        set_ptt(p, true, "timeout-reply");
        ctrl_buf_queue(p, samples, n);
        free(samples);
        p->ctrl.on_drain = timeout_reply_after_drain;
        return;
    }
    p->reply     = samples;
    p->reply_len = n;
}

/* ── repeater down / up ────────────────────────────────────────────────── */

static void down_after_drain(port_t *p, uint64_t now)
{
    set_ptt(p, false, "repeater-down");
    p->tx_activity = false;
    schedule_id(p, now);
}

static void up_after_drain(port_t *p, uint64_t now)
{
    (void)now;
    if (p->state == PORT_IDLE)
        set_ptt(p, false, "repeater-up");
}

/* Stop everything in progress: down supersedes any transmission, tail or time-out. */
static void quiesce(port_t *p, uint64_t now)
{
    p->hang_deadline = p->ct_delay_deadline = 0;
    p->local_timeout_deadline = p->link_timeout_deadline = 0;
    p->local_timed_out = p->link_timed_out = false;
    p->local_tot_used_s = 0.0;
    p->pad_deadline = 0;
    p->pad_cont = NULL;
    p->initial_id_pending = false;
    p->anxious_id_armed = false;
    p->id_deadline = p->id_sub_deadline = 0;
    p->impolite_id_playing = false;
    p->mix_len = p->mix_pos = 0;
    p->mix_on_drain = NULL;
    p->id_epoch++;
    reply_discard(p);
    ctrl_buf_clear(p);
    port_transition(p, PORT_IDLE, now);
}

void port_set_repeater_down(port_t *p, bool down, int16_t *reply, size_t n, uint64_t now)
{
    if (down) {
        bool was_down = p->down;
        p->down = true;
        quiesce(p, now);
        LOGW("port: repeater DOWN%s\n", was_down ? " (already down)" : "");

        /* The reply is a transmission, so the station identifies on its way down,
         * and then nothing more is owed. */
        if (!(reply && n) && !p->tx_activity) {
            free(reply);
            set_ptt(p, false, "repeater-down");
            return;
        }
        set_ptt(p, true, "repeater-down");
        if (reply && n)
            ctrl_buf_queue(p, reply, n);
        const char *id = mandatory_id_next(p);
        if (p->ev->n_mandatory_ids > 0)
            p->id_rot_mandatory = (p->id_rot_mandatory + 1) % p->ev->n_mandatory_ids;
        LOGI("id: mandatory  '%s'  (repeater going down)\n", id);
        queue_message(p, id, -1.0);
        p->ctrl.on_drain = down_after_drain;
        free(reply);
        return;
    }

    bool was_down = p->down;
    p->down = false;
    LOGW("port: repeater UP%s\n", was_down ? "" : " (already up)");
    if (was_down)
        quiesce(p, now);

    /* A station still keyed (a command ended with D) is repeated at once, and the
     * reply waits for its tail like any other. */
    if (cor_active(p)) {
        if (p->mmdvm_active)
            start_local_timer(p, now);
        if (p->link_active && p->cfg->timers.link_timeout > 0.0)
            p->link_timeout_deadline = now + (uint64_t)(p->cfg->timers.link_timeout * 1000.0);
        cor_active_edge(p, p->mmdvm_active ? SRC_LOCAL : SRC_LINK, now);
        port_queue_reply(p, reply, n);
        return;
    }
    if (!reply || !n) {
        free(reply);
        return;
    }
    p->tx_activity = true;
    if (p->id_deadline == 0)
        schedule_id(p, now);
    set_ptt(p, true, "repeater-up");
    ctrl_buf_queue(p, reply, n);
    free(reply);
    p->ctrl.on_drain = up_after_drain;
}

void port_restore_repeater_down(port_t *p)
{
    p->down = true;
    LOGW("port: repeater is DOWN, as it was before the restart\n");
}

bool port_repeater_down(const port_t *p)
{
    return p->down;
}

void port_set_link_up(port_t *p, bool up)
{
    p->link_up = up;
}

/* ── timer aggregation ─────────────────────────────────────────────────── */

uint64_t port_next_deadline_ms(const port_t *p)
{
    uint64_t candidates[] = {
        p->ct_delay_deadline, p->hang_deadline, p->local_timeout_deadline, p->link_timeout_deadline,
        p->id_sub_deadline, p->id_deadline, p->pad_deadline,
    };
    uint64_t min = 0;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (candidates[i] != 0 && (min == 0 || candidates[i] < min))
            min = candidates[i];
    }
    return min;
}

void port_check_timers(port_t *p, uint64_t now)
{
    if (p->ct_delay_deadline && now >= p->ct_delay_deadline)
        on_ct_delay(p, now);
    if (p->hang_deadline && now >= p->hang_deadline)
        on_hang(p, now);
    if (p->local_timeout_deadline && now >= p->local_timeout_deadline)
        on_timeout(p, SRC_LOCAL, now);
    if (p->link_timeout_deadline && now >= p->link_timeout_deadline)
        on_timeout(p, SRC_LINK, now);
    if (p->id_sub_deadline && now >= p->id_sub_deadline) {
        p->id_sub_deadline  = 0;
        p->anxious_id_armed = true;
    }
    if (p->id_deadline && now >= p->id_deadline)
        on_id(p, now);
    if (p->pad_deadline && now >= p->pad_deadline) {
        p->pad_deadline = 0;
        port_cont_fn cb = p->pad_cont;
        p->pad_cont = NULL;
        if (cb)
            cb(p, now);
    }
}

/* ── public accessors ─────────────────────────────────────────────────── */

bool port_ptt(const port_t *p) { return p->ptt; }

bool port_mmdvm_gate_open(const port_t *p)
{
    return local_active(p);
}

bool port_link_gate_open(const port_t *p)
{
    return link_active(p) && !local_active(p);
}

bool port_local_timed_out(const port_t *p)
{
    return p->local_timed_out;
}

void port_ctrl_pull(port_t *p, int16_t out[160], uint64_t now)
{
    ctrl_buf_pull_frame(p, out, now);
}

void port_mix_apply(port_t *p, int16_t *payload, uint64_t now)
{
    if (p->mix_pos >= p->mix_len)
        return;
    size_t n = p->mix_len - p->mix_pos;
    if (n > 160)
        n = 160;
    for (size_t i = 0; i < n; i++) {
        int32_t s = (int32_t)payload[i] + (int32_t)p->mix_buf[p->mix_pos + i];
        if (s > 32767)  s = 32767;
        if (s < -32768) s = -32768;
        payload[i] = (int16_t)s;
    }
    p->mix_pos += n;
    if (p->mix_pos >= p->mix_len) {
        port_cont_fn cb = p->mix_on_drain;
        p->mix_on_drain = NULL;
        p->mix_len = 0;
        p->mix_pos = 0;
        if (cb)
            cb(p, now);
    }
}

/* ── lifecycle ─────────────────────────────────────────────────────────── */

int port_create(port_t **out, const config_t *cfg, vocab_cache_t *vocab)
{
    port_t *p = calloc(1, sizeof(*p));
    if (!p)
        return -1;
    p->cfg   = cfg;
    p->ev    = &cfg->events;
    p->vocab = vocab;
    p->state = PORT_IDLE;
    p->last_source = SRC_LOCAL;
    p->link_up = true;
    *out = p;
    return 0;
}

void port_set_activity_callback(port_t *p, void (*cb)(void *arg, double seconds), void *arg)
{
    p->activity_cb  = cb;
    p->activity_arg = arg;
}

void port_set_ptt_callback(port_t *p, void (*cb)(void *arg, bool active, const char *reason), void *arg)
{
    p->set_ptt_cb  = cb;
    p->set_ptt_arg = arg;
}

void port_destroy(port_t *p)
{
    if (!p)
        return;
    free(p->ctrl.buf.data);
    free(p->mix_buf);
    free(p->reply);
    free(p);
}
