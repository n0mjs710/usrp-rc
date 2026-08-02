/*
 * Repeater controller state machine — C translation of rc/port.py.
 *
 * asyncio coroutines with "await drain_clips()" become continuations:
 * ctrl.on_drain (fires when the controller TX buffer empties) and
 * pad_cont (fires when a pre/post-message pad timer expires). Only one
 * such job is ever in flight at a time, mirroring the fact that port.py's
 * coroutines are serialized by the same clip queue.
 *
 * "cor_ctcss" access mode is accepted in config for structural completeness
 * but is not functionally distinct from "cor" here: USRP carries a single
 * keyup bit, so there is no independent CTCSS source to gate PENDING on.
 */

#include "port.h"
#include "message.h"
#include "sbuf.h"

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
    vocab_cache_t  *vocab;

    port_state_t  state;

    bool          mmdvm_active;
    bool          link_active;
    uint64_t      cor_up_ms;
    port_source_t last_source;

    double        tot_used_s;
    uint64_t      tot_start_ms;

    int  id_rot_initial;
    int  id_rot_mandatory;
    bool tx_activity;
    bool initial_id_pending;
    bool anxious_id_armed;
    bool voice_id_active;
    bool impolite_id_playing;
    int  id_epoch;

    /* timers — absolute monotonic ms deadlines; 0 = not armed */
    uint64_t hang_deadline;
    uint64_t ct_delay_deadline;
    uint64_t timeout_deadline;
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

    /* single-slot job context for standalone (was_ptt==false) transmissions */
    int          job_epoch;
    port_state_t job_state_before;
    bool         job_was_ptt;
    bool         job_post_pad_needed;

    void (*set_ptt_cb)(void *arg, bool active);
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
    p->ctrl.buf.len  = 0;
    p->ctrl.pos      = 0;
    p->ctrl.on_drain = NULL;
}

static void ctrl_buf_pull_frame(port_t *p, int16_t out[160], uint64_t now)
{
    ctrl_buf_t *c = &p->ctrl;
    size_t avail = c->buf.len - c->pos;
    size_t n     = avail < 160 ? avail : 160;
    if (n > 0)
        memcpy(out, c->buf.data + c->pos, n * sizeof(int16_t));
    if (n < 160)
        memset(out + n, 0, (160 - n) * sizeof(int16_t));
    c->pos += n;

    if (c->buf.len > 0 && c->pos >= c->buf.len) {
        c->buf.len = 0;
        c->pos     = 0;
        port_cont_fn cb = c->on_drain;
        c->on_drain = NULL;
        if (cb)
            cb(p, now);
    }
}

/* ── misc helpers ───────────────────────────────────────────────────────── */

static bool cor_active(const port_t *p) { return p->mmdvm_active || p->link_active; }

static void set_ptt(port_t *p, bool active, uint64_t now)
{
    (void)now;
    bool changed = (p->ptt != active);
    p->ptt = active;
    if (changed && p->set_ptt_cb)
        p->set_ptt_cb(p->set_ptt_arg, active);
}

static void queue_message(port_t *p, const char *name, double morse_level_override)
{
    if (!name || !name[0])
        return;
    rendered_audio_t r = message_render(p->cfg, p->vocab, name, morse_level_override);
    if (r.samples) {
        ctrl_buf_queue(p, r.samples, r.n);
        free(r.samples);
    }
}

static void schedule_hang(port_t *p, uint64_t now)
{
    p->hang_deadline = now + (uint64_t)(p->cfg->timers.hang * 1000.0);
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

    double lead = p->cfg->timers.id_anxious;
    if (p->cfg->events.anxious_id[0] && lead > 0.0 && lead < p->cfg->timers.id_interval) {
        p->id_sub_deadline = now + (uint64_t)((p->cfg->timers.id_interval - lead) * 1000.0);
    }
}

/* ── transitions ────────────────────────────────────────────────────────── */

static void do_impolite_id(port_t *p, uint64_t now);

static void port_transition(port_t *p, port_state_t new_state, uint64_t now)
{
    port_state_t old = p->state;
    p->state = new_state;

    if (new_state == PORT_ACTIVE) {
        p->tot_start_ms = now;
        p->timeout_deadline = now + (uint64_t)(
            (p->cfg->timers.timeout - p->tot_used_s > 0.0
                ? p->cfg->timers.timeout - p->tot_used_s : 0.0) * 1000.0);
    } else if (old == PORT_ACTIVE && new_state == PORT_TAIL) {
        p->tot_used_s += (double)(now - p->tot_start_ms) / 1000.0;
        p->timeout_deadline = 0;
    } else if (new_state == PORT_TIMEOUT) {
        p->tot_used_s = 0.0;
    }

    if (new_state == PORT_ACTIVE) {
        p->tx_activity = true;
        if ((old == PORT_IDLE || old == PORT_PENDING) && p->id_deadline == 0) {
            p->initial_id_pending = true;
        }
        if (p->voice_id_active && ctrl_buf_playing(p)) {
            p->initial_id_pending = false;
            p->id_epoch++;
            p->voice_id_active = false;
            ctrl_buf_clear(p);
            do_impolite_id(p, now);
        }
    }
}

/* ── timeout announce / recovery ───────────────────────────────────────── */

static void timeout_announce_after_drain(port_t *p, uint64_t now)
{
    if (p->state == PORT_TIMEOUT)
        set_ptt(p, false, now);
}

static void do_timeout_announce(port_t *p, uint64_t now)
{
    const char *name = p->cfg->events.timeout_message;
    if (name[0]) {
        rendered_audio_t r = message_render(p->cfg, p->vocab, name, -1.0);
        if (r.samples) {
            ctrl_buf_queue(p, r.samples, r.n);
            free(r.samples);
            p->ctrl.on_drain = timeout_announce_after_drain;
            return;
        }
    }
    if (p->state == PORT_TIMEOUT)
        set_ptt(p, false, now);
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

    const char *name = p->cfg->events.timeout_cancel_message;
    if (name[0]) {
        rendered_audio_t r = message_render(p->cfg, p->vocab, name, -1.0);
        if (r.samples) {
            ctrl_buf_queue(p, r.samples, r.n);
            free(r.samples);
            p->ctrl.on_drain = timeout_recovery_after_drain;
            return;
        }
    }
    if (p->state == PORT_TAIL)
        schedule_hang(p, now);
}

/* ── impolite ID (mix buffer, not ctrl buffer — see port.h) ──────────────── */

static void impolite_id_after_mix(port_t *p, uint64_t now)
{
    p->impolite_id_playing = false;
    schedule_id(p, now);
    if (p->state == PORT_TAIL && !cor_active(p)) {
        queue_message(p, p->cfg->events.ct_message, -1.0);
        schedule_hang(p, now);
    }
}

static void do_impolite_id(port_t *p, uint64_t now)
{
    const char *name = p->cfg->events.impolite_id;
    char picked[CFG_STR] = {0};

    if (!name[0]) {
        if (p->cfg->events.n_mandatory_ids == 0) {
            fprintf(stderr, "port: no impolite or mandatory ID configured\n");
            schedule_id(p, now);
            return;
        }
        int idx = p->id_rot_mandatory % p->cfg->events.n_mandatory_ids;
        strncpy(picked, p->cfg->events.mandatory_ids[idx], sizeof(picked) - 1);
        p->id_rot_mandatory = (idx + 1) % p->cfg->events.n_mandatory_ids;
        name = picked;
    }

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
        set_ptt(p, false, now);
    mandatory_id_finish(p, now);
}

static void mandatory_id_after_drain(port_t *p, uint64_t now)
{
    if (p->job_post_pad_needed && p->cfg->audio.post_message_ms > 0) {
        p->pad_deadline = now + (uint64_t)p->cfg->audio.post_message_ms;
        p->pad_cont     = mandatory_id_after_post_pad;
        return;
    }
    mandatory_id_after_post_pad(p, now);
}

static void mandatory_id_render_and_queue(port_t *p, uint64_t now)
{
    if (p->cfg->events.n_mandatory_ids == 0) {
        fprintf(stderr, "port: no mandatory_ids configured\n");
        mandatory_id_finish(p, now);
        return;
    }
    int idx = p->id_rot_mandatory % p->cfg->events.n_mandatory_ids;
    const char *name = p->cfg->events.mandatory_ids[idx];
    p->id_rot_mandatory = (idx + 1) % p->cfg->events.n_mandatory_ids;

    p->voice_id_active = message_has_voice(p->cfg, name);
    queue_message(p, name, -1.0);

    if (p->job_was_ptt)
        mandatory_id_finish(p, now);
    else
        p->ctrl.on_drain = mandatory_id_after_drain;
}

static void do_mandatory_id(port_t *p, uint64_t now)
{
    p->job_epoch        = p->id_epoch;
    p->job_was_ptt       = p->ptt;
    p->job_state_before  = p->state;

    if (p->cfg->events.n_mandatory_ids == 0) {
        fprintf(stderr, "port: no mandatory_ids configured\n");
        schedule_id(p, now);
        return;
    }

    const char *peek = p->cfg->events.mandatory_ids[p->id_rot_mandatory % p->cfg->events.n_mandatory_ids];
    p->job_post_pad_needed = !p->job_was_ptt && message_needs_padding(p->cfg, peek);

    if (!p->job_was_ptt) {
        set_ptt(p, true, now);
        if (p->job_post_pad_needed && p->cfg->audio.pre_message_ms > 0) {
            p->pad_deadline = now + (uint64_t)p->cfg->audio.pre_message_ms;
            p->pad_cont     = mandatory_id_render_and_queue;
            return;
        }
    }
    mandatory_id_render_and_queue(p, now);
}

/* ── initial ID (PTT already on; unconditional pre-pad, own drain-await) ── */

static void initial_id_after_drain(port_t *p, uint64_t now)
{
    p->voice_id_active = false;
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

static void initial_id_render_and_queue(port_t *p, uint64_t now)
{
    if (p->cfg->events.n_initial_ids == 0) {
        fprintf(stderr, "port: no initial_ids configured\n");
        initial_id_after_drain(p, now);
        return;
    }
    int idx = p->id_rot_initial % p->cfg->events.n_initial_ids;
    const char *name = p->cfg->events.initial_ids[idx];
    p->id_rot_initial = (idx + 1) % p->cfg->events.n_initial_ids;

    p->voice_id_active = message_has_voice(p->cfg, name);
    queue_message(p, name, -1.0);
    p->ctrl.on_drain = initial_id_after_drain;
}

static void do_initial_id(port_t *p, uint64_t now)
{
    p->job_epoch = p->id_epoch;
    int pre_ms = p->cfg->audio.pre_message_ms;
    if (pre_ms > 0) {
        p->pad_deadline = now + (uint64_t)pre_ms;
        p->pad_cont     = initial_id_render_and_queue;
        return;
    }
    initial_id_render_and_queue(p, now);
}

/* ── anxious ID (single message; PTT already on) ──────────────────────────── */

static void anxious_id_finish(port_t *p, uint64_t now)
{
    p->voice_id_active = false;
    if (p->id_epoch != p->job_epoch)
        return;
    p->tx_activity = false;
    schedule_id(p, now);
    if (p->state == PORT_TAIL && !cor_active(p))
        schedule_ct_delay(p, now);
}

static void anxious_id_render_and_queue(port_t *p, uint64_t now)
{
    const char *name = p->cfg->events.anxious_id;
    if (name[0]) {
        p->voice_id_active = message_has_voice(p->cfg, name);
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
        p->pad_deadline = now + (uint64_t)pre_ms;
        p->pad_cont     = anxious_id_render_and_queue;
        return;
    }
    anxious_id_render_and_queue(p, now);
}

/* ── startup sequence ──────────────────────────────────────────────────── */

static void startup_finish(port_t *p, uint64_t now)
{
    set_ptt(p, false, now);
    port_transition(p, PORT_IDLE, now);
}

static void startup_after_post_pad(port_t *p, uint64_t now)
{
    startup_finish(p, now);
}

static void startup_after_id_drain(port_t *p, uint64_t now)
{
    p->voice_id_active = false;
    schedule_id(p, now);
    p->tx_activity = false;

    int post_ms = p->cfg->audio.post_message_ms;
    if (post_ms > 0) {
        p->pad_deadline = now + (uint64_t)post_ms;
        p->pad_cont     = startup_after_post_pad;
        return;
    }
    startup_finish(p, now);
}

static void startup_queue_id(port_t *p, uint64_t now)
{
    if (p->cfg->events.n_initial_ids == 0) {
        startup_after_id_drain(p, now);
        return;
    }
    int idx = p->id_rot_initial % p->cfg->events.n_initial_ids;
    const char *name = p->cfg->events.initial_ids[idx];
    p->id_rot_initial = (idx + 1) % p->cfg->events.n_initial_ids;

    p->voice_id_active = message_has_voice(p->cfg, name);
    queue_message(p, name, -1.0);
    p->ctrl.on_drain = startup_after_id_drain;
}

static void startup_after_message_drain(port_t *p, uint64_t now)
{
    startup_queue_id(p, now);
}

static void startup_render_message(port_t *p, uint64_t now)
{
    const char *name = p->cfg->events.startup_message;
    rendered_audio_t r = message_render(p->cfg, p->vocab, name, -1.0);
    if (r.samples) {
        ctrl_buf_queue(p, r.samples, r.n);
        free(r.samples);
        p->ctrl.on_drain = startup_after_message_drain;
    } else {
        startup_queue_id(p, now);
    }
}

void port_start(port_t *p, uint64_t now)
{
    if (!p->cfg->events.startup_message[0]) {
        fprintf(stderr, "port: no startup_message configured — starting quietly in IDLE\n");
        return;
    }
    set_ptt(p, true, now);
    int pre_ms = p->cfg->audio.pre_message_ms;
    if (pre_ms > 0 && message_needs_padding(p->cfg, p->cfg->events.startup_message)) {
        p->pad_deadline = now + (uint64_t)pre_ms;
        p->pad_cont     = startup_render_message;
        return;
    }
    startup_render_message(p, now);
}

/* ── courtesy tone / hang timer callbacks ─────────────────────────────────── */

static void on_ct_delay(port_t *p, uint64_t now)
{
    p->ct_delay_deadline = 0;
    if (p->state != PORT_TAIL)
        return;

    const char *name = p->cfg->events.ct_message;
    if (p->last_source == SRC_LINK && p->cfg->events.ct_link_message[0])
        name = p->cfg->events.ct_link_message;
    queue_message(p, name, -1.0);

    p->tot_used_s = 0.0;
    schedule_hang(p, now);
}

static void on_hang(port_t *p, uint64_t now)
{
    p->hang_deadline = 0;
    if (cor_active(p))
        return;
    set_ptt(p, false, now);
    port_transition(p, PORT_IDLE, now);
}

static void on_timeout(port_t *p, uint64_t now)
{
    p->timeout_deadline = 0;
    port_transition(p, PORT_TIMEOUT, now);
    ctrl_buf_clear(p);
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

/* ── COR edge handling (access_mode "cor"; "cor_ctcss" collapses to this) ── */

static void cor_active_edge(port_t *p, uint64_t now)
{
    p->cor_up_ms         = now;
    p->hang_deadline     = 0;
    p->ct_delay_deadline = 0;

    if (p->state == PORT_IDLE || p->state == PORT_TAIL) {
        set_ptt(p, true, now);
        port_transition(p, PORT_ACTIVE, now);
    }
}

static void cor_idle_edge(port_t *p, uint64_t now)
{
    double duration_s = (double)(now - p->cor_up_ms) / 1000.0;

    if (p->state == PORT_ACTIVE || p->state == PORT_TAIL) {
        if (p->state == PORT_ACTIVE && duration_s < p->cfg->timers.kerchunk) {
            p->timeout_deadline    = 0;
            p->tot_used_s          = 0.0;
            p->initial_id_pending  = false;
            if (p->id_deadline == 0)
                schedule_id(p, now);
            set_ptt(p, false, now);
            port_transition(p, PORT_IDLE, now);
        } else {
            port_transition(p, PORT_TAIL, now);
            if (p->anxious_id_armed)
                do_anxious_id(p, now);
            else if (p->initial_id_pending) {
                p->initial_id_pending = false;
                do_initial_id(p, now);
            } else if (!p->impolite_id_playing) {
                schedule_ct_delay(p, now);
            }
        }
    } else if (p->state == PORT_TIMEOUT) {
        set_ptt(p, true, now);
        port_transition(p, PORT_TAIL, now);
        do_timeout_recovery(p, now);
    }
}

void port_on_mmdvm_keyup(port_t *p, bool active, uint64_t now)
{
    bool old_combined = cor_active(p);
    p->mmdvm_active = active;
    if (active)
        p->last_source = SRC_LOCAL;
    bool new_combined = cor_active(p);
    if (new_combined != old_combined) {
        if (new_combined) cor_active_edge(p, now);
        else              cor_idle_edge(p, now);
    }
}

void port_on_link_keyup(port_t *p, bool active, uint64_t now)
{
    bool old_combined = cor_active(p);
    p->link_active = active;
    if (active)
        p->last_source = SRC_LINK;
    bool new_combined = cor_active(p);
    if (new_combined != old_combined) {
        if (new_combined) cor_active_edge(p, now);
        else              cor_idle_edge(p, now);
    }
}

/* ── timer aggregation ─────────────────────────────────────────────────── */

uint64_t port_next_deadline_ms(const port_t *p)
{
    uint64_t candidates[] = {
        p->ct_delay_deadline, p->hang_deadline, p->timeout_deadline,
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
    if (p->timeout_deadline && now >= p->timeout_deadline)
        on_timeout(p, now);
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

port_state_t  port_state(const port_t *p)        { return p->state; }
bool          port_ptt(const port_t *p)           { return p->ptt; }
bool          port_mmdvm_active(const port_t *p)  { return p->mmdvm_active; }
bool          port_link_active(const port_t *p)   { return p->link_active; }
port_source_t port_last_source(const port_t *p)   { return p->last_source; }

bool port_mmdvm_gate_open(const port_t *p)
{
    return p->mmdvm_active && p->state != PORT_TIMEOUT;
}

bool port_link_gate_open(const port_t *p)
{
    return p->link_active && !p->mmdvm_active && p->state != PORT_TIMEOUT;
}

bool port_ctrl_pull(port_t *p, int16_t out[160], uint64_t now)
{
    if (!p->ptt)
        return false;
    ctrl_buf_pull_frame(p, out, now);
    return true;
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
    p->vocab = vocab;
    p->state = PORT_IDLE;
    p->last_source = SRC_LOCAL;
    *out = p;
    return 0;
}

void port_set_ptt_callback(port_t *p, void (*cb)(void *arg, bool active), void *arg)
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
    free(p);
}
