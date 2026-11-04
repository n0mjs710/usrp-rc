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
#include "util.h"
#include "log.h"

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

/* Not exposed in the TOML schema (see plan) — no-packet timeout on each
 * port before we treat it as unkeyed even without an explicit UNKEY frame.
 * Matches rusrp's own default network_timeout_ms.
 *
 * On the mmdvm side this is now a pure backstop: MMDVM-Host's FM protocol
 * marks end-of-transmission explicitly with FME, so the normal path never
 * reaches the watchdog. Note FMP keepalives must NOT feed it — they arrive
 * every 5 s whether or not anyone is talking. */
#define WATCHDOG_MS 500

/* Periodic link-peer jitter/silence telemetry log (jitter_buffer.c tracks
 * these continuously; this is just the missing consumer). */
#define HEARTBEAT_INTERVAL_MS 60000u

/* How often usage is written out, if it changed. Easy on the SD card; a power failure
 * loses at most this much of it. */
#define STATS_SAVE_INTERVAL_MS (15u * 60u * 1000u)

static volatile sig_atomic_t g_stop = 0;

static void sig_handler(int sig)
{
    (void)sig;
    g_stop = 1;
}

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

    config_events_t events;   /* the config's, as changed by any open schedule window */
    bool     schedule_active[CFG_MAX_SCHEDULE];
    uint64_t schedule_deadline;

    bool prev_mmdvm_keyup;
    bool prev_link_keyup;

    /* No mmdvm-side sequence counter: MMDVM-Host's FM protocol has no
     * sequence field. Only the link (still USRP) needs one. */
    uint32_t link_tx_seq;
    bool     link_tx_ended;   /* nothing more goes to the link this transmission */
    bool     link_up;         /* off by DTMF command: no audio to or from the link */

    uint64_t mmdvm_watchdog_deadline;
    uint64_t link_watchdog_deadline;

    uint64_t pace_deadline;   /* 20 ms mmdvm-TX pacing timer; armed while port PTT is on */
    uint64_t heartbeat_deadline;   /* periodic link jitter/silence telemetry; 0 = no link */
    uint64_t fm_ping_deadline;     /* FMP keepalive to MMDVM-Host */
    uint64_t ptt_start_ms;    /* set on each PTT-on edge; used to log PTT-off duration */

    /* MMDVM-Host sends variable-length FMD payloads; everything downstream
     * wants exactly 160 samples. See fm_reframe.h. */
    fm_reframe_t mmdvm_rx_reframe;
    char         mmdvm_peer_callsign[FM_CALLSIGN_MAX];

    int16_t opus_accum[480];
    int     opus_accum_n;

    uint64_t mmdvm_recv_err_logged_ms;
    uint64_t link_recv_err_logged_ms;

    uint64_t mmdvm_voice_last_ms;    /* pacing monitor: see note_mmdvm_voice_pacing() */
    uint64_t mmdvm_voice_last_cpu_ms; /* CPU-time twin of the above, for stall attribution */
    int      mmdvm_voice_burst_run;
    uint64_t mmdvm_rx_last_ms;       /* receive-side pacing monitor: see handle_mmdvm() */
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

/* MMDVM-Host's FM network path (LinkMode=1) does no jitter-smoothing of its
 * own -- it forwards whatever it has, whenever asked, and the modem
 * firmware's ext-audio buffer only restarts once >75ms has re-accumulated
 * after running dry, with zero debounce on the drop itself. A gap on our
 * outbound side anywhere near that -- or a burst of frames sent
 * back-to-back after one, evidence of a stall having let packets queue up
 * before draining all at once -- is a plausible trigger for an on-air
 * carrier drop. This logs only the rare anomaly, not every frame.
 *
 * Speaking FM natively removed the fmgateway relay hop from this path but
 * did NOT change the underlying fragility: MMDVM-Host accepts no key/unkey
 * marker from a gateway, so "we stopped sending" and "we stalled" are the
 * same event to the modem. This instrumentation stays until the latency
 * work is done. */
#define MMDVM_VOICE_GAP_NOTE_MS   35U   /* nominal cadence is 20ms; a missed tick, logged at debug */
#define MMDVM_VOICE_GAP_WARN_MS   60U   /* firmware's FM_LINK_EXT_GAP_MS underrun tolerance */
#define MMDVM_VOICE_GAP_DANGER_MS 75U   /* modem's link-mode ext-audio restart threshold */

static void note_mmdvm_voice_pacing(app_t *a, uint64_t now)
{
    uint64_t now_cpu = cpu_time_ms();
    if (a->mmdvm_voice_last_ms != 0) {
        uint64_t gap = now - a->mmdvm_voice_last_ms;
        if (gap >= MMDVM_VOICE_GAP_NOTE_MS) {
            uint64_t cpu_gap = now_cpu - a->mmdvm_voice_last_cpu_ms;
            if (gap < MMDVM_VOICE_GAP_WARN_MS) {
                LOGD("audio: %llums gap between mmdvm-TX frames, %llums of it our own CPU time (normal; the modem tolerates 60ms)\n",
                        (unsigned long long)gap, (unsigned long long)cpu_gap);
            } else {
                if (a->mmdvm_voice_burst_run > 1)
                    LOGW("audio: burst of %d mmdvm-TX frames sent back-to-back just before this gap\n",
                            a->mmdvm_voice_burst_run);
                LOGW("audio: %llums gap between mmdvm-TX frames (nominal 20ms), %llums of that was our own CPU time%s\n",
                        (unsigned long long)gap, (unsigned long long)cpu_gap,
                        gap >= MMDVM_VOICE_GAP_DANGER_MS ? "  -- at/past the modem's link-mode drop threshold" : "");
            }
            a->mmdvm_voice_burst_run = 0;
        } else if (gap <= 2) {
            a->mmdvm_voice_burst_run++;
        } else {
            if (a->mmdvm_voice_burst_run > 1)
                LOGW("audio: burst of %d mmdvm-TX frames sent back-to-back\n",
                        a->mmdvm_voice_burst_run);
            a->mmdvm_voice_burst_run = 0;
        }
    }
    a->mmdvm_voice_last_ms     = now;
    a->mmdvm_voice_last_cpu_ms = now_cpu;
}

/* `keyup` is vestigial on this path and deliberately ignored: MMDVM-Host's
 * FM protocol carries no PTT bit. It stays in the signature because every
 * caller is a place where the distinction still matters conceptually, and
 * because the link side (still USRP) does have one. */
static void send_mmdvm_voice(app_t *a, const int16_t frame[160], bool keyup)
{
    (void)keyup;
    uint8_t buf[FM_MAX_PKT_LEN];
    size_t len = fm_build_data(buf, frame, 160u);
    send(a->mmdvm_sock, buf, len, 0);
    note_mmdvm_voice_pacing(a, monotonic_ms());
}

static void send_mmdvm_ping(app_t *a)
{
    uint8_t buf[FM_TAG_LEN];
    size_t len = fm_build_ping(buf);
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
        /* Clean slate for the pacing monitor -- the idle gap between
         * transmissions is expected and not a delivery problem. */
        a->mmdvm_voice_last_ms   = 0;
        a->mmdvm_voice_burst_run = 0;
    }
}

/* ── 20 ms mmdvm-TX pacing tick: controller audio or link-forward audio ──── */

static void pace_tick(app_t *a, uint64_t now)
{
    if (!port_ptt(a->port)) {
        a->pace_deadline = 0;
        return;
    }
    a->pace_deadline = now + 20;

    if (port_mmdvm_gate_open(a->port))
        return;   /* repeat path already sends frames synchronously per packet */

    int16_t frame[160];
    if (port_link_gate_open(a->port)) {
        jitter_buffer_pull(a->jb, frame);
        port_mix_apply(a->port, frame, now);
        send_mmdvm_voice(a, frame, true);
    } else {
        port_ctrl_pull(a->port, frame, now);
        port_mix_apply(a->port, frame, now);
        send_mmdvm_voice(a, frame, true);
    }
}

/* ── mmdvm RX handling ─────────────────────────────────────────────────── */

static void mmdvm_key_edge(app_t *a, uint64_t now)
{
    a->link_tx_ended = !a->link_up;
    if (a->link_up) {
        send_link_key(a, true);
        LOGI("link: KEY -> peer\n");
    }
    port_on_mmdvm_keyup(a->port, true, now);
}

/* End this transmission's uplink: pad out any partial Opus frame and unkey. */
static void link_tx_end(app_t *a)
{
    if (a->link_tx_ended)
        return;
    a->link_tx_ended = true;
    if (a->link_sock < 0)
        return;
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

/* Stop listening to the link: the peer is treated as having unkeyed. */
static void link_rx_end(app_t *a, uint64_t now)
{
    if (!a->prev_link_keyup)
        return;
    a->prev_link_keyup = false;
    a->link_watchdog_deadline = 0;
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
        link_rx_end(a, monotonic_ms());
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
        reply_with_message(a, a->events.time_message);
        break;
    case COMMAND_STATS:
        reply_with_message(a, a->events.stats_message);
        break;
    case COMMAND_LINK_ON:
        set_link(a, true);
        reply_with_message(a, a->events.link_on_message);
        break;
    case COMMAND_LINK_OFF:
        set_link(a, false);
        reply_with_message(a, a->events.link_off_message);
        break;
    }
}

static void mmdvm_unkey_edge(app_t *a, uint64_t now)
{
    ste_reset(a->ste);
    link_tx_end(a);

    /* Before the port sees the unkey, so a reply is in place when the tail starts. */
    command_t c;
    if (a->cmd && command_session_end_of_tx(a->cmd, &c))
        run_command(a, &c);
    a->link_tx_ended = false;

    port_on_mmdvm_keyup(a->port, false, now);
}

/* Companion to note_mmdvm_voice_pacing() on the receive side: is MMDVM-Host
 * actually delivering mmdvm-RX packets to us on the expected ~20ms
 * cadence in the first place? If gaps show up here, the irregularity
 * predates us and no amount of tuning our own send timing will fix it.
 *
 * `audio_this_call` counts FMD datagrams found waiting together. FMS and FME are
 * left out, as MMDVM-Host sends them right next to audio on every transmission.
 * Three together means audio was held up about as long as the gap warning's limit;
 * two is ordinary scheduling, which the jitter buffer absorbs. It can't say whether
 * the delay was ours or MMDVM-Host's. */
#define MMDVM_RX_GAP_WARN_MS 35U
#define MMDVM_RX_BURST_WARN  3

/* One reframed 160-sample frame of received RF audio, fanned out to the
 * local repeat path and the link. Split out of handle_mmdvm() because a
 * single FMD datagram can now yield zero, one, or more of these. */
static void mmdvm_rx_frame(app_t *a, const int16_t frame_in[160], uint64_t now)
{
    if (!port_mmdvm_gate_open(a->port))
        return;

    /* Ahead of the delay line, so with STE on the mute beats the tone out. */
    bool muted = false;
    if (a->cmd) {
        command_t c;
        if (command_session_frame(a->cmd, frame_in, &c))
            run_command(a, &c);
        muted = command_session_muted(a->cmd);
        if (command_session_link_cut(a->cmd))
            link_tx_end(a);
    }

    /* STE delay line, shared by both destinations: real audio once the
     * buffer has filled, silence (not silence-by-omission) until then,
     * so both flows key up immediately but with a gapless, delayed
     * audio stream instead of a dropout. */
    int16_t ste_out[160];
    if (!ste_push(a->ste, frame_in, ste_out))
        memset(ste_out, 0, sizeof(ste_out));

    /* Flow 1: local repeat, mmdvm RX -> mmdvm TX (+ impolite-ID mix). */
    int16_t frame[160];
    if (muted)
        command_session_fill_mute(a->cmd, frame);
    else
        memcpy(frame, ste_out, sizeof(frame));
    port_mix_apply(a->port, frame, now);
    send_mmdvm_voice(a, frame, true);

    /* Flow 2: mmdvm RX -> link TX. */
    if (a->link_sock >= 0 && !a->link_tx_ended)
        send_link_voice(a, ste_out);
}

static void handle_mmdvm(app_t *a)
{
    uint8_t buf[FM_MAX_PKT_LEN + 64];
    fm_packet_t pkt;
    int audio_this_call = 0;

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
         * whether anyone is talking, so they must not feed the watchdog,
         * the pacing monitor, or the audio count. */
        if (pkt.type == FM_PKT_PING || pkt.type == FM_PKT_NONE)
            continue;

        if (pkt.type == FM_PKT_DATA)
            audio_this_call++;
        a->mmdvm_watchdog_deadline = now + WATCHDOG_MS;

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
            a->mmdvm_rx_last_ms = now;
            if (!a->prev_mmdvm_keyup) {
                a->prev_mmdvm_keyup = true;
                mmdvm_key_edge(a, now);
            }
            break;

        case FM_PKT_DATA: {
            /* MMDVM-Host can start sending FMD without a preceding FMS if
             * we came up mid-transmission; key up on first audio too. */
            if (!a->prev_mmdvm_keyup) {
                a->prev_mmdvm_keyup = true;
                mmdvm_key_edge(a, now);
                a->mmdvm_rx_last_ms = now;
            } else if (a->mmdvm_rx_last_ms != 0) {
                uint64_t gap = now - a->mmdvm_rx_last_ms;
                if (gap >= MMDVM_RX_GAP_WARN_MS)
                    LOGW("audio: %llums gap between received mmdvm-RX packets (nominal 20ms) -- upstream of us\n",
                            (unsigned long long)gap);
            }
            a->mmdvm_rx_last_ms = now;

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

            a->mmdvm_rx_last_ms = 0;
            if (a->prev_mmdvm_keyup) {
                a->prev_mmdvm_keyup = false;
                a->mmdvm_watchdog_deadline = 0;
                mmdvm_unkey_edge(a, now);
            }
            LOGI("mmdvm: RX ended\n");
            break;
        }

        default:
            break;
        }
    }

    if (audio_this_call >= MMDVM_RX_BURST_WARN)
        LOGW("audio: %d mmdvm-RX audio packets arrived together -- a brief stall, here or in MMDVM-Host\n",
                audio_this_call);
    else if (audio_this_call > 1)
        LOGD("audio: %d mmdvm-RX audio packets arrived together (normal)\n", audio_this_call);
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
        a->link_watchdog_deadline = now + WATCHDOG_MS;

        bool keyup = pkt.keyup != 0;
        if (keyup != a->prev_link_keyup) {
            a->prev_link_keyup = keyup;
            if (keyup) {
                jitter_buffer_reset_silence_count(a->jb);
            } else {
                jitter_buffer_latch_silence(a->jb);
                uint64_t overflow = jitter_buffer_overflow_count(a->jb);
                uint64_t silence  = jitter_buffer_latched_silence_count(a->jb);
                float    jitter   = jitter_buffer_estimate_ms(a->jb);
                LOGI("link: RX ended  overflow=%llu silence=%llu jitter=%.1fms\n",
                        (unsigned long long)overflow, (unsigned long long)silence, jitter);
                jitter_buffer_flush(a->jb);
            }
            port_on_link_keyup(a->port, keyup, now);
        }

        if (!keyup)
            continue;

        if (pkt.type == USRP_TYPE_VOICE) {
            jitter_buffer_push(a->jb, pkt.audio);
        } else if (pkt.type == USRP_TYPE_OPUS && a->opus) {
            int16_t pcm[480];
            int dn = opus_codec_decode(a->opus, pkt.opus, (int)pkt.opus_len, pcm);
            if (dn > 0) {
                int sub = dn / 160;
                for (int i = 0; i < sub; i++)
                    jitter_buffer_push(a->jb, pcm + i * 160);
            }
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
    schedule_evaluate(&a->cfg, &when, &a->events, active);
    for (int i = 0; i < a->cfg.nschedule; i++) {
        if (active[i] != a->schedule_active[i])
            LOGI("schedule: '%s' %s\n", a->cfg.schedule[i].name, active[i] ? "started" : "ended");
        a->schedule_active[i] = active[i];
    }
    a->schedule_deadline = now + (uint64_t)(60 - when.tm_sec) * 1000;
}

/* ── timer aggregation ─────────────────────────────────────────────────── */

static int next_timeout_ms(app_t *a, uint64_t now)
{
    uint64_t min = 0;
    uint64_t cand[8];
    int nc = 0;

    uint64_t pd = port_next_deadline_ms(a->port);
    if (pd) cand[nc++] = pd;
    if (a->pace_deadline) cand[nc++] = a->pace_deadline;
    if (a->prev_mmdvm_keyup && a->mmdvm_watchdog_deadline) cand[nc++] = a->mmdvm_watchdog_deadline;
    if (a->prev_link_keyup && a->link_watchdog_deadline)   cand[nc++] = a->link_watchdog_deadline;
    if (a->heartbeat_deadline) cand[nc++] = a->heartbeat_deadline;
    if (a->fm_ping_deadline) cand[nc++] = a->fm_ping_deadline;
    if (a->schedule_deadline) cand[nc++] = a->schedule_deadline;
    if (a->stats_deadline) cand[nc++] = a->stats_deadline;

    for (int i = 0; i < nc; i++)
        if (min == 0 || cand[i] < min) min = cand[i];

    if (min == 0) return -1;
    if (min <= now) return 0;
    uint64_t diff = min - now;
    return diff > (uint64_t)INT_MAX ? INT_MAX : (int)diff;
}

static void check_all_timers(app_t *a, uint64_t now)
{
    if (a->schedule_deadline && now >= a->schedule_deadline)
        schedule_update(a, now);

    if (a->stats_deadline && now >= a->stats_deadline) {
        stats_save(a->stats);
        a->stats_deadline = now + STATS_SAVE_INTERVAL_MS;
    }

    port_check_timers(a->port, now);

    if (a->pace_deadline && now >= a->pace_deadline)
        pace_tick(a, now);

    if (a->prev_mmdvm_keyup && a->mmdvm_watchdog_deadline && now >= a->mmdvm_watchdog_deadline) {
        /* Backstop only — MMDVM-Host normally ends a transmission with FME.
         * Reaching here means the FME went missing or Host stopped mid-
         * transmission, so discard the partial frame rather than letting it
         * bleed into whatever comes next. */
        LOGW("mmdvm: watchdog timeout — treating as unkey (no FME received)\n");
        a->prev_mmdvm_keyup = false;
        a->mmdvm_watchdog_deadline = 0;
        a->mmdvm_rx_last_ms = 0;
        fm_reframe_reset(&a->mmdvm_rx_reframe);
        mmdvm_unkey_edge(a, now);
    }

    if (a->fm_ping_deadline && now >= a->fm_ping_deadline) {
        send_mmdvm_ping(a);
        a->fm_ping_deadline = now + FM_PING_INTERVAL_MS;
    }
    if (a->prev_link_keyup && a->link_watchdog_deadline && now >= a->link_watchdog_deadline) {
        a->prev_link_keyup = false;
        a->link_watchdog_deadline = 0;
        jitter_buffer_latch_silence(a->jb);
        uint64_t overflow = jitter_buffer_overflow_count(a->jb);
        uint64_t silence  = jitter_buffer_latched_silence_count(a->jb);
        float    jitter   = jitter_buffer_estimate_ms(a->jb);
        LOGW("link: watchdog timeout — treating as unkey  overflow=%llu silence=%llu jitter=%.1fms\n",
                (unsigned long long)overflow, (unsigned long long)silence, jitter);
        jitter_buffer_flush(a->jb);
        port_on_link_keyup(a->port, false, now);
    }

    if (a->heartbeat_deadline && now >= a->heartbeat_deadline) {
        float    jitter  = jitter_buffer_estimate_ms(a->jb);
        uint64_t silence = jitter_buffer_hb_silence_count(a->jb);
        LOGD("link: heartbeat  jitter=%.1fms  silence(%us)=%llu\n",
                jitter, HEARTBEAT_INTERVAL_MS / 1000u, (unsigned long long)silence);
        a->heartbeat_deadline = now + HEARTBEAT_INTERVAL_MS;
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
        } else if (argv[i][0] == '-') {
            LOGE("usage: usrp-rc [--check] [config-file]\n");
            return 2;
        } else {
            config_path = argv[i];
        }
    }

    /* app_t embeds config_t directly (messages[64] x elements[32] x
     * voice_words[32] adds up to several MB) -- heap-allocate it rather
     * than risk overflowing the thread stack. */
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
    if (config_rc != 0 || content_errors != 0) {
        LOGE("usrp-rc: config errors in %s -- not starting\n", config_path);
        return 1;
    }

    if (check_only) {
        printf("usrp-rc: %s OK (%d messages)\n", config_path, a->cfg.nmessages);
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
    printf("usrp-rc: %d messages loaded\n", a->cfg.nmessages);

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
    /* With no link at all, there's no "link off" for the courtesy tone to show. */
    port_set_link_up(a->port, a->link_up || !a->cfg.link.enabled);

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
        a->heartbeat_deadline = monotonic_ms() + HEARTBEAT_INTERVAL_MS;
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

    a->events = a->cfg.events;

    /* Ahead of the startup message, so it already sees any open window. */
    if (a->cfg.nschedule > 0) {
        schedule_update(a, monotonic_ms());
        port_set_events(a->port, &a->events);
    }

    port_start(a->port, monotonic_ms());

    /* Symmetric with MMDVM-Host's own 5 s FMP. Host ignores inbound pings,
     * so this is purely so a packet capture / Debug=1 log shows both ends
     * alive rather than one-way traffic. */
    a->fm_ping_deadline = monotonic_ms() + FM_PING_INTERVAL_MS;

    printf("usrp-rc: ready\n");

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

        now = monotonic_ms();
        check_all_timers(a, now);

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == a->mmdvm_sock)
                handle_mmdvm(a);
            else if (events[i].data.fd == a->link_sock)
                handle_link(a);
        }
    }

    printf("usrp-rc: stopping\n");
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
    vocab_cache_destroy(a->vocab);
    free(a);
    return 0;
}
