#include "config.h"
#include "usrp_protocol.h"
#include "fm_protocol.h"
#include "fm_reframe.h"
#include "opus_codec.h"
#include "jitter_buffer.h"
#include "vocab.h"
#include "message.h"
#include "ste.h"
#include "port.h"
#include "command.h"
#include "schedule.h"
#include "stats.h"
#include "tone.h"
#include "sbuf.h"
#include "util.h"
#include "log.h"
#include "version.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <time.h>

#define DEFAULT_CONFIG "/etc/usrp-rc/usrp-rc.conf"
#define MAX_EVENTS 8

/* No-packet timeout on each port before we treat it as unkeyed even without an
 * explicit UNKEY frame.
 * Matches rusrp's own default network_timeout_ms.
 *
 * On the mmdvm side this is now a pure backstop: MMDVM-Host's FM protocol
 * marks end-of-transmission explicitly with FME, so the normal path never
 * reaches the watchdog. Note FMP keepalives must NOT feed it — they arrive
 * every 5 s whether or not anyone is talking. */
#define WATCHDOG_MS 500

/* How often usage is written out, if it changed. Easy on the SD card; a power failure
 * loses at most this much of it. */
#define STATS_SAVE_INTERVAL_MS (15u * 60u * 1000u)

static volatile sig_atomic_t g_stop = 0;

static void sig_handler(int sig)
{
    (void)sig;
    g_stop = 1;
}

#define ECHO_SAMPLES (10 * 8000)

typedef struct {
    config_t        cfg;
    vocab_cache_t  *vocab;
    port_t         *port;
    ste_t          *ste;
    command_session_t *cmd;   /* NULL = dtmf.enabled is false */
    jitter_buffer_t *jb;
    opus_codec_t   *opus;   /* NULL in PCM mode */

    int mmdvm_sock;
    int link_sock;
    int epfd;

    stats_t  *stats;
    uint64_t  stats_deadline;

    config_t        base;     /* as loaded; cfg is this with any open schedule window applied */
    bool     schedule_active[CFG_MAX_SCHEDULE];
    uint64_t schedule_deadline;

    bool prev_mmdvm_keyup;
    bool prev_link_keyup;

    /* No mmdvm-side sequence counter: MMDVM-Host's FM protocol has no
     * sequence field. Only the link (still USRP) needs one. */
    uint32_t link_tx_seq;
    bool     link_tx_ended;   /* nothing more goes to the link this transmission */
    bool     link_up;         /* off by DTMF command: no audio to or from the link */

    int16_t *regen;           /* DTMF regen on its way to the link; NULL = none */
    size_t   regen_len;
    size_t   regen_pos;
    uint64_t regen_deadline;

    int16_t *echo;            /* echo test recording, ECHO_SAMPLES; allocated on first use */
    size_t   echo_len;
    bool     echo_recording;

    uint64_t mmdvm_watchdog_deadline;   /* armed only while keyed */
    uint64_t link_watchdog_deadline;
    uint64_t port_deadline;             /* port_next_deadline_ms(), for the timer table */

    uint64_t pace_deadline;   /* 20 ms mmdvm-TX pacing timer; armed while port PTT is on */
    uint64_t ptt_start_ms;    /* set on each PTT-on edge; used to log PTT-off duration */

    /* MMDVM-Host sends variable-length FMD payloads; everything downstream
     * wants exactly 160 samples. See fm_reframe.h. */
    fm_reframe_t mmdvm_rx_reframe;
    char         mmdvm_peer_callsign[FM_CALLSIGN_MAX];

    int16_t opus_accum[480];
    int     opus_accum_n;

    uint64_t mmdvm_recv_err_logged_ms;
    uint64_t link_recv_err_logged_ms;
} app_t;

/* Repeated recv() errors (most commonly ECONNREFUSED: the peer's ICMP port-
 * unreachable landing on our connected UDP socket, e.g. while a link peer
 * is down or not yet configured) would otherwise spam once per audio frame.
 * Log immediately, then at most once every 5 s per socket while it persists. */
#define RECV_ERR_LOG_INTERVAL_MS 5000

static void log_recv_error(const char *label, uint64_t *last_logged_ms, uint64_t now)
{
    if (*last_logged_ms != 0 && now - *last_logged_ms < RECV_ERR_LOG_INTERVAL_MS)
        return;
    *last_logged_ms = now;
    LOGE("%s recv: %s (peer unreachable? check [%s] config — suppressing repeats for %ds)\n",
            label, strerror(errno), label, RECV_ERR_LOG_INTERVAL_MS / 1000);
}

/* ── socket setup ──────────────────────────────────────────────────────── */

static int make_udp_socket(const char *bind_addr, uint16_t bind_port,
                           const char *peer_addr, uint16_t peer_port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = htons(bind_port) };
    if (inet_pton(AF_INET, bind_addr, &local.sin_addr) != 1) {
        LOGE("invalid bind address: %s\n", bind_addr);
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        LOGE("bind %s:%u: %s\n", bind_addr, bind_port, strerror(errno));
        close(fd);
        return -1;
    }

    struct sockaddr_in peer = { .sin_family = AF_INET, .sin_port = htons(peer_port) };
    if (inet_pton(AF_INET, peer_addr, &peer.sin_addr) != 1) {
        LOGE("invalid peer address: %s\n", peer_addr);
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&peer, sizeof(peer)) < 0) {
        LOGE("connect %s:%u: %s\n", peer_addr, peer_port, strerror(errno));
        close(fd);
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

/* ── outbound frame builders ───────────────────────────────────────────── */

/* MMDVM-Host's FM protocol has no PTT bit: the flow of frames is the PTT. */
static void send_mmdvm_voice(app_t *a, const int16_t frame[160])
{
    uint8_t buf[FM_MAX_PKT_LEN];
    size_t len = fm_build_data(buf, frame, 160u);
    send(a->mmdvm_sock, buf, len, 0);
}

static void flush_opus_uplink(app_t *a)
{
    uint8_t pkt[USRP_PKT_LEN];
    uint8_t opus_buf[USRP_OPUS_MAX_BYTES];
    int opus_len = opus_codec_encode(a->opus, a->opus_accum, opus_buf);
    if (opus_len > 0) {
        int frame_samples = opus_codec_frame_samples(a->opus);
        int sub_frames    = frame_samples / 160;
        uint32_t seq = a->link_tx_seq;
        a->link_tx_seq += (uint32_t)sub_frames;
        size_t pkt_len = usrp_build_opus_voice(pkt, seq, 1, opus_buf, (size_t)opus_len);
        send(a->link_sock, pkt, pkt_len, 0);
    }
    a->opus_accum_n = 0;
}

static void send_link_voice(app_t *a, const int16_t frame[160])
{
    if (a->link_sock < 0)
        return;
    if (a->cfg.link.codec == LINK_CODEC_PCM) {
        uint8_t buf[USRP_PKT_LEN];
        usrp_build_voice(buf, a->link_tx_seq++, 1, frame);
        send(a->link_sock, buf, USRP_PKT_LEN, 0);
        return;
    }
    int frame_samples = opus_codec_frame_samples(a->opus);
    memcpy(a->opus_accum + a->opus_accum_n, frame, 160 * sizeof(int16_t));
    a->opus_accum_n += 160;
    if (a->opus_accum_n >= frame_samples)
        flush_opus_uplink(a);
}

static void send_link_key(app_t *a, bool keyup)
{
    if (a->link_sock < 0)
        return;
    uint8_t buf[USRP_PKT_LEN];
    usrp_build_key(buf, a->link_tx_seq++, keyup ? 1 : 0);
    send(a->link_sock, buf, USRP_PKT_LEN, 0);
}

/* ── port PTT callback: arm/disarm mmdvm-TX pacing ──────────────────────── */

/* There is no key/unkey marker to send here. MMDVM-Host's FM protocol
 * accepts only FMD from a gateway (FMNetwork.cpp discards FMS/FME on that
 * direction), so PTT is expressed purely by starting and stopping the FMD
 * flow -- which is exactly what arming and disarming the pacing timer
 * below does. */
static void on_port_ptt(void *arg, bool active, const char *reason)
{
    app_t *a = arg;
    uint64_t now = monotonic_ms();
    a->pace_deadline = active ? now + 20 : 0;
    if (active) {
        a->ptt_start_ms = now;
        LOGI("ptt: ON   reason=%s\n", reason);
    } else {
        double duration_s = (double)(now - a->ptt_start_ms) / 1000.0;
        LOGI("ptt: OFF  reason=%s  duration=%.1fs\n", reason, duration_s);
    }
}

/* ── 20 ms mmdvm-TX pacing tick: controller audio or link-forward audio ──── */

static void pace_tick(app_t *a, uint64_t now)
{
    if (!port_ptt(a->port)) {
        a->pace_deadline = 0;
        return;
    }
    /* Step the deadline, not now + 20, so late wakeups don't slow the cadence;
     * after a long stall, start again from now rather than send a burst. */
    a->pace_deadline += 20;
    if (a->pace_deadline + 60 < now)
        a->pace_deadline = now + 20;

    if (port_mmdvm_gate_open(a->port))
        return;   /* repeat path already sends frames synchronously per packet */

    int16_t frame[160];
    if (port_link_gate_open(a->port))
        jitter_buffer_pull(a->jb, frame);
    else
        port_ctrl_pull(a->port, frame, now);
    port_mix_apply(a->port, frame, now);
    send_mmdvm_voice(a, frame);
}

/* ── mmdvm RX handling ─────────────────────────────────────────────────── */

static void mmdvm_key_edge(app_t *a, uint64_t now)
{
    bool to_link = a->link_up && !port_repeater_down(a->port);
    if (to_link && a->regen) {
        LOGI("link: still sending DTMF, this transmission stays local\n");
        to_link = false;
    }
    if (port_take_echo(a->port)) {
        /* Not to the link: listeners there can't hear the prompt or the playback. */
        LOGI("echo: recording, this transmission stays local\n");
        if (!a->echo)
            a->echo = xrealloc(NULL, ECHO_SAMPLES * sizeof(int16_t));
        a->echo_len = 0;
        a->echo_recording = true;
        to_link = false;
    }
    a->link_tx_ended = !to_link;
    if (to_link) {
        send_link_key(a, true);
        LOGI("link: KEY -> peer\n");
    }
    port_on_mmdvm_keyup(a->port, true, now);
}

/* Pad out any partial Opus frame and unkey. */
static void link_uplink_close(app_t *a)
{
    if (a->cfg.link.codec == LINK_CODEC_OPUS && a->opus_accum_n > 0) {
        int frame_samples = opus_codec_frame_samples(a->opus);
        memset(a->opus_accum + a->opus_accum_n, 0,
               (size_t)(frame_samples - a->opus_accum_n) * sizeof(int16_t));
        a->opus_accum_n = frame_samples;
        flush_opus_uplink(a);
    }
    send_link_key(a, false);
    LOGI("link: UNKEY -> peer\n");
}

/* End this transmission's uplink. */
static void link_tx_end(app_t *a)
{
    if (a->link_tx_ended)
        return;
    a->link_tx_ended = true;
    if (a->link_sock >= 0)
        link_uplink_close(a);
}

/* Regen goes to the link on its own 20 ms clock, as the receiver has unkeyed and the
 * repeater may not be transmitting yet. */
static void regen_stop(app_t *a, const char *why)
{
    if (!a->regen)
        return;
    LOGI("link: DTMF %s\n", why);
    free(a->regen);
    a->regen = NULL;
    a->regen_deadline = 0;
    link_uplink_close(a);
}

static void regen_start(app_t *a, int16_t *samples, size_t n, uint64_t now)
{
    regen_stop(a, "cut short");
    if (!samples)
        return;
    a->regen = samples;
    a->regen_len = n;
    a->regen_pos = 0;
    a->regen_deadline = now;
    send_link_key(a, true);
    LOGI("link: KEY -> peer (DTMF)\n");
}

static void regen_tick(app_t *a, uint64_t now)
{
    (void)now;
    int16_t frame[160] = {0};
    size_t left = a->regen_len - a->regen_pos;
    size_t take = left < 160 ? left : 160;
    memcpy(frame, a->regen + a->regen_pos, take * sizeof(int16_t));
    a->regen_pos += take;
    send_link_voice(a, frame);
    a->regen_deadline += 20;
    if (a->regen_pos >= a->regen_len)
        regen_stop(a, "sent");
}

/* The link's transmission is over: its unkey, the link turned off or the repeater
 * down (each already logged), or a watchdog timeout, which `why` names as a warning. */
static void link_rx_end(app_t *a, uint64_t now, const char *why)
{
    if (!a->prev_link_keyup)
        return;
    a->prev_link_keyup = false;
    a->link_watchdog_deadline = 0;
    uint64_t overflow = jitter_buffer_overflow_count(a->jb);
    uint64_t silence  = jitter_buffer_silence_count(a->jb);
    if (why)
        LOGW("link: RX ended, %s  overflow=%llu silence=%llu\n", why,
             (unsigned long long)overflow, (unsigned long long)silence);
    else
        LOGI("link: RX ended  overflow=%llu silence=%llu\n",
             (unsigned long long)overflow, (unsigned long long)silence);
    jitter_buffer_flush(a->jb);
    port_on_link_keyup(a->port, false, now);
}

static void set_link(app_t *a, bool up)
{
    if (a->link_up != up)
        LOGI("link: turned %s by command\n", up ? "on" : "off");
    a->link_up = up;
    port_set_link_up(a->port, up);
    if (!up) {
        regen_stop(a, "cut short");
        link_rx_end(a, monotonic_ms(), NULL);
        link_tx_end(a);
    }
}

static void reply_with_message(app_t *a, const char *name)
{
    if (!name[0])
        return;
    rendered_audio_t r = message_render(&a->cfg, a->vocab, name, -1.0);
    port_queue_reply(a->port, r.samples, r.n);
}

/* Repeater down survives a restart, so a power cut doesn't quietly put a repeater back
 * on the air that its operator took off. */
static void save_repeater_state(app_t *a, bool down)
{
    const char *path = a->cfg.state.file;
    if (!path[0])
        return;
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *fp = fopen(tmp, "w");
    bool ok = fp && fprintf(fp, "repeater = %s\n", down ? "down" : "up") > 0;
    if (fp)
        ok = fclose(fp) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) {
        LOGW("state: can't save %s: %s\n", path, strerror(errno));
        unlink(tmp);
    }
}

static bool load_repeater_down(const app_t *a)
{
    const char *path = a->cfg.state.file;
    if (!path[0])
        return false;
    FILE *fp = fopen(path, "r");
    if (!fp)
        return false;
    char line[64];
    bool down = false;
    while (fgets(line, sizeof(line), fp))
        if (strncmp(line, "repeater = down", 15) == 0)
            down = true;
    fclose(fp);
    return down;
}

static void run_command(app_t *a, const command_t *c)
{
    switch (c->type) {
    case COMMAND_PAD_TEST: {
        const char *clips[COMMAND_MAX_DIGITS];
        int n = 0;
        for (const char *d = c->digits; *d; d++)
            clips[n++] = command_digit_clip(*d);
        rendered_audio_t r = message_render_clips(&a->cfg, a->vocab, clips, n);
        port_queue_reply(a->port, r.samples, r.n);
        break;
    }
    case COMMAND_TIME:
        reply_with_message(a, a->cfg.events.time_message);
        break;
    case COMMAND_STATS:
        reply_with_message(a, a->cfg.events.stats_message);
        break;
    case COMMAND_LINK_ON:
        set_link(a, true);
        reply_with_message(a, a->cfg.events.link_on_message);
        break;
    case COMMAND_LINK_OFF:
        set_link(a, false);
        reply_with_message(a, a->cfg.events.link_off_message);
        break;
    case COMMAND_LINK_STATUS:
        reply_with_message(a, a->link_up ? a->cfg.events.link_on_message : a->cfg.events.link_off_message);
        break;
    case COMMAND_ECHO:
        port_arm_echo(a->port);
        reply_with_message(a, a->cfg.events.echo_message);
        break;
    case COMMAND_NO_LINK:
        reply_with_message(a, a->cfg.events.link_none_message);
        break;
    case COMMAND_REGEN: {
        const config_dtmf_t *d = &a->cfg.dtmf;
        size_t n;
        if (a->link_up && a->link_sock >= 0 && !port_repeater_down(a->port)) {
            /* Silence before the unkey, or ASL never sees the last digit end and
             * stays keyed. */
            int16_t *t = tone_render_dtmf(c->digits, d->regen_lead_ms, d->regen_tone_ms,
                                          d->regen_gap_ms, 2 * d->regen_gap_ms,
                                          d->regen_level, &n);
            regen_start(a, t, n, monotonic_ms());
        }
        int16_t *t = tone_render_dtmf(c->digits, 0, d->regen_tone_ms, d->regen_gap_ms,
                                      0, d->regen_level, &n);
        if (t) {
            sbuf_t b = {.data = t, .len = n, .cap = n};
            sbuf_scale(&b, a->cfg.audio.master_gain);
        }
        port_queue_reply(a->port, t, n);
        break;
    }
    case COMMAND_REPEATER: {
        bool down = c->digits[0] == '0';
        const char *name = down ? a->cfg.events.repeater_down_message : a->cfg.events.repeater_up_message;
        rendered_audio_t r = {0};
        if (name[0])
            r = message_render(&a->cfg, a->vocab, name, -1.0);
        port_set_repeater_down(a->port, down, r.samples, r.n, monotonic_ms());
        if (down) {
            regen_stop(a, "cut short");
            link_tx_end(a);
            link_rx_end(a, monotonic_ms(), NULL);
        }
        save_repeater_state(a, down);
        break;
    }
    }
}

/* The one rule for when a command may run. Nothing does during an echo test recording.
 * Timed out or down, only repeater up/down does: the control operator's way back. */
static bool command_allowed(const app_t *a, const command_t *c)
{
    const char *why = NULL;
    if (a->echo_recording)
        why = "echo test recording";
    else if (c->type != COMMAND_REPEATER && port_repeater_down(a->port))
        why = "repeater down";
    else if (c->type != COMMAND_REPEATER && port_local_timed_out(a->port))
        why = "timed out";
    if (why)
        LOGI("dtmf: command dropped, %s\n", why);
    return !why;
}

static void run_or_drop_command(app_t *a, const command_t *c)
{
    if (command_allowed(a, c))
        run_command(a, c);
}

static void mmdvm_unkey_edge(app_t *a, uint64_t now)
{
    ste_reset(a->ste);
    link_tx_end(a);

    /* Before the port sees the unkey, so a reply is in place when the tail starts.
     * Timed out, a command has to end in D: an unkey doesn't enter it. */
    command_t c;
    bool have = false;
    if (a->cmd && port_local_timed_out(a->port))
        command_session_end_of_tx_discard(a->cmd, "timed out, a command must end with D");
    else if (a->cmd)
        have = command_session_end_of_tx(a->cmd, &c) && command_allowed(a, &c);
    a->link_tx_ended = false;

    int16_t *echo = NULL;
    size_t echo_len = a->echo_len;
    if (a->echo_recording) {
        a->echo_recording = false;
        a->echo_len = 0;
        if (port_local_timed_out(a->port)) {
            LOGI("echo: timed out, nothing to play back\n");
        } else if (echo_len) {
            echo = xrealloc(NULL, echo_len * sizeof(int16_t));
            memcpy(echo, a->echo, echo_len * sizeof(int16_t));
        }
    }

    /* Repeater up/down acts on the repeater as a whole, so it follows the unkey; a reply
     * goes first so it's in place when the tail starts. */
    if (have && c.type == COMMAND_REPEATER) {
        port_on_mmdvm_keyup(a->port, false, now);
        run_command(a, &c);
        return;
    }
    if (have)
        run_command(a, &c);
    if (echo) {
        LOGI("echo: playing back %.1f s\n", (double)echo_len / 8000.0);
        port_queue_reply(a->port, echo, echo_len);
    }
    port_on_mmdvm_keyup(a->port, false, now);
}

/* One reframed 160-sample frame of received RF audio, fanned out to the
 * local repeat path and the link. Split out of handle_mmdvm() because a
 * single FMD datagram can now yield zero, one, or more of these. */
static void mmdvm_rx_frame(app_t *a, const int16_t frame_in[160], uint64_t now)
{
    /* Timed out, the receiver is still heard for commands ending in D, but its
     * audio goes nowhere. */
    if (!port_mmdvm_gate_open(a->port)) {
        if (port_local_timed_out(a->port) || port_repeater_down(a->port)) {
            link_tx_end(a);
            command_t c;
            if (a->cmd && command_session_frame(a->cmd, frame_in, &c))
                run_or_drop_command(a, &c);
        }
        return;
    }

    /* Ahead of the delay line, so with STE on the mute beats the tone out. */
    bool muted = false;
    if (a->cmd) {
        command_t c;
        if (command_session_frame(a->cmd, frame_in, &c))
            run_or_drop_command(a, &c);
        muted = command_session_muted(a->cmd);
    }

    /* STE delay line, shared by both destinations: real audio once the
     * buffer has filled, silence (not silence-by-omission) until then,
     * so both flows key up immediately but with a gapless, delayed
     * audio stream instead of a dropout. */
    int16_t ste_out[160];
    if (!ste_push(a->ste, frame_in, ste_out))
        memset(ste_out, 0, sizeof(ste_out));

    /* During a DTMF mute each path carries the cover tone, or silence where it is off. */
    int16_t cover[160], silence[160] = {0};
    const int16_t *local = ste_out, *link = ste_out;
    if (muted) {
        command_session_fill_mute(a->cmd, cover);
        local = a->cfg.dtmf.local_cover_tone ? cover : silence;
        link  = a->cfg.dtmf.link_cover_tone ? cover : silence;
    }

    /* What was repeated, so the playback can't carry DTMF either. */
    if (a->echo_recording && a->echo_len < ECHO_SAMPLES) {
        size_t n = ECHO_SAMPLES - a->echo_len < 160 ? ECHO_SAMPLES - a->echo_len : 160;
        memcpy(a->echo + a->echo_len, local, n * sizeof(int16_t));
        a->echo_len += n;
    }

    /* Flow 1: local repeat, mmdvm RX -> mmdvm TX (+ impolite-ID mix). */
    int16_t frame[160];
    memcpy(frame, local, sizeof(frame));
    port_mix_apply(a->port, frame, now);
    send_mmdvm_voice(a, frame);

    /* Flow 2: mmdvm RX -> link TX. */
    if (a->link_sock >= 0 && !a->link_tx_ended)
        send_link_voice(a, link);
}

/* Each packet of a transmission keys it, if it isn't already, and feeds the watchdog. */
static void mmdvm_rx_start(app_t *a, uint64_t now)
{
    a->mmdvm_watchdog_deadline = now + WATCHDOG_MS;
    if (a->prev_mmdvm_keyup)
        return;
    a->prev_mmdvm_keyup = true;
    mmdvm_key_edge(a, now);
}

/* The transmission is over: FME, or the watchdog, which `why` names as a warning. On the
 * watchdog, the partial frame is discarded rather than bleed into the next one. */
static void mmdvm_rx_end(app_t *a, uint64_t now, const char *why)
{
    if (!a->prev_mmdvm_keyup)
        return;
    a->prev_mmdvm_keyup = false;
    a->mmdvm_watchdog_deadline = 0;
    if (why) {
        LOGW("mmdvm: RX ended, %s\n", why);
        fm_reframe_reset(&a->mmdvm_rx_reframe);
    } else {
        LOGI("mmdvm: RX ended\n");
    }
    mmdvm_unkey_edge(a, now);
}

static void handle_mmdvm(app_t *a)
{
    uint8_t buf[FM_MAX_PKT_LEN + 64];
    fm_packet_t pkt;

    for (;;) {
        ssize_t n = recv(a->mmdvm_sock, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                log_recv_error("mmdvm", &a->mmdvm_recv_err_logged_ms, monotonic_ms());
            break;
        }
        if (fm_parse(buf, (size_t)n, &pkt) != 0)
            continue;

        uint64_t now = monotonic_ms();

        /* Keepalives are not traffic: they arrive every 5 s regardless of
         * whether anyone is talking, so they must not feed the watchdog. */
        if (pkt.type == FM_PKT_PING || pkt.type == FM_PKT_NONE)
            continue;

        switch (pkt.type) {
        case FM_PKT_START:
            /* Explicit start-of-transmission -- no edge inference needed.
             * A repeat START without an intervening END shouldn't happen,
             * but treat it idempotently rather than double-keying. */
            if (pkt.callsign[0]) {
                memcpy(a->mmdvm_peer_callsign, pkt.callsign, sizeof(a->mmdvm_peer_callsign));
                LOGI("mmdvm: RX started  callsign=%s\n", a->mmdvm_peer_callsign);
            } else {
                LOGI("mmdvm: RX started\n");
            }
            fm_reframe_reset(&a->mmdvm_rx_reframe);
            mmdvm_rx_start(a, now);
            break;

        case FM_PKT_DATA: {
            /* MMDVM-Host can start sending FMD without a preceding FMS if
             * we came up mid-transmission; key up on first audio too. */
            mmdvm_rx_start(a, now);
            fm_reframe_push(&a->mmdvm_rx_reframe, pkt.audio, pkt.nsamples);

            int16_t frame[160];
            while (fm_reframe_pop(&a->mmdvm_rx_reframe, frame))
                mmdvm_rx_frame(a, frame, now);
            break;
        }

        case FM_PKT_END: {
            /* Don't strand the tail: pad the partial frame out and push it
             * through, or it would be prepended to the next transmission. */
            int16_t frame[160];
            if (fm_reframe_flush(&a->mmdvm_rx_reframe, frame))
                mmdvm_rx_frame(a, frame, now);
            mmdvm_rx_end(a, now, NULL);
            break;
        }

        default:
            break;
        }
    }
}

/* ── link RX handling ──────────────────────────────────────────────────── */

static void handle_link(app_t *a)
{
    uint8_t buf[USRP_PKT_LEN + 64];
    usrp_packet_t pkt;

    for (;;) {
        ssize_t n = recv(a->link_sock, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                log_recv_error("link", &a->link_recv_err_logged_ms, monotonic_ms());
            return;
        }
        if (usrp_parse(buf, (size_t)n, &pkt) != 0)
            continue;

        if (!a->link_up)
            continue;   /* keep draining the socket, but don't act on any of it */

        uint64_t now = monotonic_ms();
        if (!pkt.keyup) {
            link_rx_end(a, now, NULL);
            continue;
        }
        a->link_watchdog_deadline = now + WATCHDOG_MS;
        if (!a->prev_link_keyup) {
            a->prev_link_keyup = true;
            port_on_link_keyup(a->port, true, now);
        }

        /* Link audio that isn't being repeated, as a local station has priority, the link
         * timed out or the repeater is down, is not kept: it would play late. */
        bool keep = port_link_gate_open(a->port);
        if (!keep)
            jitter_buffer_flush(a->jb);

        if (pkt.has_audio) {
            if (keep)
                jitter_buffer_push(a->jb, pkt.audio);
        } else if (pkt.type == USRP_TYPE_OPUS && a->opus) {
            int16_t pcm[480];
            int dn = opus_codec_decode(a->opus, pkt.opus, (int)pkt.opus_len, pcm);
            for (int i = 0; keep && i < dn / 160; i++)
                jitter_buffer_push(a->jb, pcm + i * 160);
        }
    }
}

/* ── usage ─────────────────────────────────────────────────────────────── */

static void on_port_activity(void *arg, double seconds)
{
    app_t *a = arg;
    stats_record(a->stats, time(NULL), seconds);
}

static void stats_for_messages(void *arg, message_stats_t *out)
{
    app_t *a = arg;
    time_t now = time(NULL);
    out->keyups_day   = stats_keyups(a->stats, now, 24);
    out->keyups_week  = stats_keyups(a->stats, now, 168);
    out->minutes_day  = stats_minutes(a->stats, now, 24);
    out->minutes_week = stats_minutes(a->stats, now, 168);
}

/* ── schedule ──────────────────────────────────────────────────────────── */

/* Runs once a minute, on the minute. Works from the wall clock each time, so a clock
 * step or a daylight saving change sorts itself out by the next run. */
static void schedule_update(app_t *a, uint64_t now)
{
    time_t t = time(NULL);
    struct tm when;
    localtime_r(&t, &when);

    bool active[CFG_MAX_SCHEDULE];
    schedule_evaluate(&a->base, &when, &a->cfg, active);
    for (int i = 0; i < a->base.nschedule; i++) {
        if (active[i] != a->schedule_active[i])
            LOGI("schedule: '%s' %s\n", a->base.schedule[i].name, active[i] ? "started" : "ended");
        a->schedule_active[i] = active[i];
    }
    a->schedule_deadline = now + (uint64_t)(60 - when.tm_sec) * 1000;
}

/* ── timer aggregation ─────────────────────────────────────────────────── */

static void stats_tick(app_t *a, uint64_t now)
{
    stats_save(a->stats);
    a->stats_deadline = now + STATS_SAVE_INTERVAL_MS;
}

static void port_tick(app_t *a, uint64_t now)
{
    port_check_timers(a->port, now);
}

/* Backstops: MMDVM-Host ends a transmission with FME, and the link with an unkey. */
static void mmdvm_watchdog(app_t *a, uint64_t now)
{
    mmdvm_rx_end(a, now, "watchdog timeout, no FME received");
}

static void link_watchdog(app_t *a, uint64_t now)
{
    link_rx_end(a, now, "watchdog timeout, no unkey received");
}

/* Every timer: an absolute deadline in app_t (0 = off), fired in this order. The schedule
 * goes first, as everything after it reads the settings, and the port's timers ahead of
 * the pacing tick that plays what they queue. */
static const struct {
    size_t offset;
    void (*fire)(app_t *a, uint64_t now);
} TIMERS[] = {
    {offsetof(app_t, schedule_deadline),       schedule_update},
    {offsetof(app_t, stats_deadline),          stats_tick},
    {offsetof(app_t, port_deadline),           port_tick},
    {offsetof(app_t, pace_deadline),           pace_tick},
    {offsetof(app_t, regen_deadline),          regen_tick},
    {offsetof(app_t, mmdvm_watchdog_deadline), mmdvm_watchdog},
    {offsetof(app_t, link_watchdog_deadline),  link_watchdog},
};
#define NTIMERS (sizeof(TIMERS) / sizeof(TIMERS[0]))

static uint64_t *deadline(app_t *a, size_t i)
{
    return (uint64_t *)((char *)a + TIMERS[i].offset);
}

static int next_timeout_ms(app_t *a, uint64_t now)
{
    a->port_deadline = port_next_deadline_ms(a->port);
    uint64_t min = 0;
    for (size_t i = 0; i < NTIMERS; i++) {
        uint64_t d = *deadline(a, i);
        if (d && (min == 0 || d < min))
            min = d;
    }
    if (min == 0) return -1;
    if (min <= now) return 0;
    uint64_t diff = min - now;
    return diff > (uint64_t)INT_MAX ? INT_MAX : (int)diff;
}

static void check_all_timers(app_t *a, uint64_t now)
{
    a->port_deadline = port_next_deadline_ms(a->port);
    for (size_t i = 0; i < NTIMERS; i++) {
        uint64_t d = *deadline(a, i);
        if (d && now >= d)
            TIMERS[i].fire(a, now);
    }
}

/* ── main ──────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    const char *config_path = DEFAULT_CONFIG;
    bool check_only = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0) {
            check_only = true;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("usrp-rc %s\n", USRP_RC_VERSION);
            return 0;
        } else if (argv[i][0] == '-') {
            LOGE("usage: usrp-rc [--check] [--version] [config-file]\n");
            return 2;
        } else {
            config_path = argv[i];
        }
    }

    /* Too large for the thread stack: config_t's schedule alone is tens of kB. */
    app_t *a = calloc(1, sizeof(*a));
    if (!a) {
        LOGE("usrp-rc: out of memory\n");
        return 1;
    }
    a->link_sock = -1;

    /* Carry on past a bad config far enough to check message content too, so
     * one run reports everything. Nothing below starts until all of it passes:
     * better off the air than on it with a setting quietly ignored. */
    int config_rc = config_load(&a->cfg, config_path);

    const char *vocab_dirs[] = {
        "user_8k", "vocab_8k",
        "/usr/local/share/usrp-rc/user_8k", "/usr/local/share/usrp-rc/vocab_8k",
    };
    if (vocab_cache_create(&a->vocab, vocab_dirs, 4) != 0) {
        LOGE("usrp-rc: failed to init vocab cache\n");
        return 1;
    }

    int content_errors = message_validate(&a->cfg, a->vocab);
    if (content_errors == 0)
        content_errors = message_check_schedule(&a->cfg, a->vocab);
    if (config_rc != 0 || content_errors != 0) {
        LOGE("usrp-rc: config errors in %s -- not starting\n", config_path);
        return 1;
    }

    int defined = a->cfg.nmessages;
    config_prune_messages(&a->cfg);
    a->base = a->cfg;

    if (check_only) {
        printf("usrp-rc: %s OK (%d messages defined, %d in use)\n", config_path, defined, a->cfg.nmessages);
        return 0;
    }

    if (a->cfg.link.enabled) {
        printf("usrp-rc: mmdvm %s:%u <-> %s:%u   link %s:%u <-> %s:%u (%s)\n",
               a->cfg.mmdvm.local_address, a->cfg.mmdvm.local_port,
               a->cfg.mmdvm.rpt_address,   a->cfg.mmdvm.rpt_port,
               a->cfg.link.bind_address,   a->cfg.link.local_port,
               a->cfg.link.remote_host,    a->cfg.link.remote_port,
               a->cfg.link.codec == LINK_CODEC_OPUS ? "opus" : "pcm");
    } else {
        printf("usrp-rc: mmdvm %s:%u <-> %s:%u   link disabled (standalone repeater)\n",
               a->cfg.mmdvm.local_address, a->cfg.mmdvm.local_port,
               a->cfg.mmdvm.rpt_address,   a->cfg.mmdvm.rpt_port);
    }
    printf("usrp-rc: %d messages defined, %d in use\n", defined, a->cfg.nmessages);

    if (port_create(&a->port, &a->cfg, a->vocab) != 0) {
        LOGE("usrp-rc: failed to init port state machine\n");
        return 1;
    }
    port_set_ptt_callback(a->port, on_port_ptt, a);

    if (stats_create(&a->stats, a->cfg.stats.file, time(NULL)) != 0) {
        LOGE("usrp-rc: failed to init usage statistics\n");
        return 1;
    }
    a->stats_deadline = monotonic_ms() + STATS_SAVE_INTERVAL_MS;
    port_set_activity_callback(a->port, on_port_activity, a);
    message_set_stats_source(stats_for_messages, a);

    a->link_up = a->cfg.link.enabled && a->cfg.link.start_on;
    port_set_link_up(a->port, a->link_up);
    if (load_repeater_down(a))
        port_restore_repeater_down(a->port);

    if (ste_create(&a->ste, a->cfg.audio.ste_delay_ms, a->cfg.audio.lead_trim_ms) != 0) {
        LOGE("usrp-rc: failed to init STE buffer\n");
        return 1;
    }

    if (a->cfg.dtmf.enabled && command_session_create(&a->cmd, &a->cfg) != 0) {
        LOGE("usrp-rc: failed to init DTMF control\n");
        return 1;
    }

    if (a->cfg.link.enabled) {
        if (jitter_buffer_create(&a->jb, JITTER_PREFILL_DEFAULT) != 0) {
            LOGE("usrp-rc: failed to init jitter buffer\n");
            return 1;
        }
        if (a->cfg.link.codec == LINK_CODEC_OPUS) {
            if (opus_codec_create(&a->opus, a->cfg.link.opus_bitrate, a->cfg.link.opus_frame_ms) != 0) {
                LOGE("usrp-rc: failed to init opus codec\n");
                return 1;
            }
        }
    }

    struct sigaction sa = { .sa_handler = sig_handler };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    a->mmdvm_sock = make_udp_socket(a->cfg.mmdvm.local_address, a->cfg.mmdvm.local_port,
                                    a->cfg.mmdvm.rpt_address,   a->cfg.mmdvm.rpt_port);
    if (a->mmdvm_sock < 0)
        return 1;

    if (a->cfg.link.enabled) {
        a->link_sock = make_udp_socket(a->cfg.link.bind_address, a->cfg.link.local_port,
                                       a->cfg.link.remote_host,  a->cfg.link.remote_port);
        if (a->link_sock < 0) {
            close(a->mmdvm_sock);
            return 1;
        }
    }

    a->epfd = epoll_create1(0);
    if (a->epfd < 0) {
        perror("epoll_create1");
        close(a->mmdvm_sock);
        close(a->link_sock);
        return 1;
    }

    struct epoll_event ev = {0};
    ev.events  = EPOLLIN;
    ev.data.fd = a->mmdvm_sock;
    epoll_ctl(a->epfd, EPOLL_CTL_ADD, a->mmdvm_sock, &ev);
    if (a->link_sock >= 0) {
        ev.data.fd = a->link_sock;
        epoll_ctl(a->epfd, EPOLL_CTL_ADD, a->link_sock, &ev);
    }

    /* Ahead of the startup message, so it already sees any open window. */
    if (a->base.nschedule > 0)
        schedule_update(a, monotonic_ms());

    port_start(a->port, monotonic_ms());

    printf("usrp-rc %s: ready\n", USRP_RC_VERSION);

    struct epoll_event events[MAX_EVENTS];
    while (!g_stop) {
        uint64_t now = monotonic_ms();
        int timeout_ms = next_timeout_ms(a, now);

        int n = epoll_wait(a->epfd, events, MAX_EVENTS, timeout_ms);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("epoll_wait");
            break;
        }

        /* Packets first: after a stall, one already waiting must refresh its
         * watchdog before the watchdog is checked. */
        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == a->mmdvm_sock)
                handle_mmdvm(a);
            else if (events[i].data.fd == a->link_sock)
                handle_link(a);
        }
        check_all_timers(a, monotonic_ms());
    }

    printf("usrp-rc: stopping\n");
    regen_stop(a, "cut short");
    if (a->prev_mmdvm_keyup)
        link_tx_end(a);   /* don't leave the peer keyed */
    close(a->epfd);
    close(a->mmdvm_sock);
    if (a->link_sock >= 0) close(a->link_sock);
    if (a->opus) opus_codec_destroy(a->opus);
    jitter_buffer_destroy(a->jb);
    ste_destroy(a->ste);
    command_session_destroy(a->cmd);
    stats_save(a->stats);
    stats_destroy(a->stats);
    port_destroy(a->port);
    free(a->echo);
    vocab_cache_destroy(a->vocab);
    config_free(&a->cfg);
    free(a);
    return 0;
}
