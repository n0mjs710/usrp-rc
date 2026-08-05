#include "config.h"
#include "usrp_protocol.h"
#include "fm_protocol.h"
#include "fm_reframe.h"
#include "opus_codec.h"
#include "jitter_buffer.h"
#include "vocab.h"
#include "ste.h"
#include "port.h"
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

#define DEFAULT_CONFIG "/etc/usrp-rc/usrp-rc.toml"
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
    jitter_buffer_t *jb;
    opus_codec_t   *opus;   /* NULL in PCM mode */

    int mmdvm_sock;
    int link_sock;
    int epfd;

    bool prev_mmdvm_keyup;
    bool prev_link_keyup;

    /* No mmdvm-side sequence counter: MMDVM-Host's FM protocol has no
     * sequence field. Only the link (still USRP) needs one. */
    uint32_t link_tx_seq;

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
#define MMDVM_VOICE_GAP_WARN_MS   35U   /* nominal cadence is 20ms */
#define MMDVM_VOICE_GAP_DANGER_MS 75U   /* modem's link-mode ext-audio restart threshold */

static void note_mmdvm_voice_pacing(app_t *a, uint64_t now)
{
    if (a->mmdvm_voice_last_ms != 0) {
        uint64_t gap = now - a->mmdvm_voice_last_ms;
        if (gap >= MMDVM_VOICE_GAP_WARN_MS) {
            if (a->mmdvm_voice_burst_run > 1)
                LOGW("audio: burst of %d mmdvm-TX frames sent back-to-back just before this gap\n",
                        a->mmdvm_voice_burst_run);
            LOGW("audio: %llums gap between mmdvm-TX frames (nominal 20ms)%s\n",
                    (unsigned long long)gap,
                    gap >= MMDVM_VOICE_GAP_DANGER_MS ? "  -- at/past the modem's link-mode drop threshold" : "");
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
    a->mmdvm_voice_last_ms = now;
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
    send_link_key(a, true);
    if (a->link_sock >= 0)
        LOGI("link: KEY -> peer\n");
    port_on_mmdvm_keyup(a->port, true, now);
}

static void mmdvm_unkey_edge(app_t *a, uint64_t now)
{
    ste_reset(a->ste);
    if (a->link_sock >= 0) {
        if (a->cfg.link.codec == LINK_CODEC_OPUS && a->opus_accum_n > 0) {
            int frame_samples = opus_codec_frame_samples(a->opus);
            memset(a->opus_accum + a->opus_accum_n, 0,
                   (size_t)(frame_samples - a->opus_accum_n) * sizeof(int16_t));
            a->opus_accum_n = frame_samples;
            flush_opus_uplink(a);
        }
    }
    send_link_key(a, false);
    if (a->link_sock >= 0)
        LOGI("link: UNKEY -> peer\n");
    port_on_mmdvm_keyup(a->port, false, now);
}

/* Companion to note_mmdvm_voice_pacing() on the receive side: is MMDVM-Host
 * actually delivering mmdvm-RX packets to us on the expected ~20ms
 * cadence in the first place? If gaps show up here, the irregularity
 * predates us and no amount of tuning our own send timing will fix it.
 * `packets_this_call`, checked after the drain loop below, is direct
 * evidence (not an inference) that our loop fell behind: it's a literal
 * count of how many datagrams were already sitting in the kernel socket
 * buffer by the time we got back to reading it. */
#define MMDVM_RX_GAP_WARN_MS 35U

/* One reframed 160-sample frame of received RF audio, fanned out to the
 * local repeat path and the link. Split out of handle_mmdvm() because a
 * single FMD datagram can now yield zero, one, or more of these. */
static void mmdvm_rx_frame(app_t *a, const int16_t frame_in[160], uint64_t now)
{
    if (!port_mmdvm_gate_open(a->port))
        return;

    /* STE delay line, shared by both destinations: real audio once the
     * buffer has filled, silence (not silence-by-omission) until then,
     * so both flows key up immediately but with a gapless, delayed
     * audio stream instead of a dropout. */
    int16_t ste_out[160];
    if (!ste_push(a->ste, frame_in, ste_out))
        memset(ste_out, 0, sizeof(ste_out));

    /* Flow 1: local repeat, mmdvm RX -> mmdvm TX (+ impolite-ID mix). */
    int16_t frame[160];
    memcpy(frame, ste_out, sizeof(frame));
    port_mix_apply(a->port, frame, now);
    send_mmdvm_voice(a, frame, true);

    /* Flow 2: mmdvm RX -> link TX. */
    if (a->link_sock >= 0)
        send_link_voice(a, ste_out);
}

static void handle_mmdvm(app_t *a)
{
    uint8_t buf[FM_MAX_PKT_LEN + 64];
    fm_packet_t pkt;
    int packets_this_call = 0;

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
         * the pacing monitor, or the fell-behind counter. */
        if (pkt.type == FM_PKT_PING || pkt.type == FM_PKT_NONE)
            continue;

        packets_this_call++;
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

    if (packets_this_call > 1)
        LOGW("audio: %d mmdvm-RX packets were queued and drained in one pass -- our loop fell behind\n",
                packets_this_call);
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

        uint64_t now = monotonic_ms();
        a->link_watchdog_deadline = now + WATCHDOG_MS;

        bool keyup = pkt.keyup != 0;
        if (keyup != a->prev_link_keyup) {
            a->prev_link_keyup = keyup;
            if (keyup) {
                jitter_buffer_reset_silence_count(a->jb);
            } else {
                jitter_buffer_latch_silence(a->jb);
                uint64_t late    = jitter_buffer_late_count(a->jb);
                uint64_t silence = jitter_buffer_latched_silence_count(a->jb);
                float    jitter  = jitter_buffer_estimate_ms(a->jb);
                LOGI("link: RX ended  late=%llu silence=%llu jitter=%.1fms\n",
                        (unsigned long long)late, (unsigned long long)silence, jitter);
                jitter_buffer_flush(a->jb);
            }
            port_on_link_keyup(a->port, keyup, now);
        }

        if (!keyup)
            continue;

        if (pkt.type == USRP_TYPE_VOICE) {
            jitter_buffer_push(a->jb, pkt.seq, pkt.audio);
        } else if (pkt.type == USRP_TYPE_OPUS && a->opus) {
            int16_t pcm[480];
            int dn = opus_codec_decode(a->opus, pkt.opus, (int)pkt.opus_len, pcm);
            if (dn > 0) {
                int sub = dn / 160;
                for (int i = 0; i < sub; i++)
                    jitter_buffer_push(a->jb, pkt.seq + (uint32_t)i, pcm + i * 160);
            }
        }
    }
}

/* ── timer aggregation ─────────────────────────────────────────────────── */

static int next_timeout_ms(app_t *a, uint64_t now)
{
    uint64_t min = 0;
    uint64_t cand[6];
    int nc = 0;

    uint64_t pd = port_next_deadline_ms(a->port);
    if (pd) cand[nc++] = pd;
    if (a->pace_deadline) cand[nc++] = a->pace_deadline;
    if (a->prev_mmdvm_keyup && a->mmdvm_watchdog_deadline) cand[nc++] = a->mmdvm_watchdog_deadline;
    if (a->prev_link_keyup && a->link_watchdog_deadline)   cand[nc++] = a->link_watchdog_deadline;
    if (a->heartbeat_deadline) cand[nc++] = a->heartbeat_deadline;
    if (a->fm_ping_deadline) cand[nc++] = a->fm_ping_deadline;

    for (int i = 0; i < nc; i++)
        if (min == 0 || cand[i] < min) min = cand[i];

    if (min == 0) return -1;
    if (min <= now) return 0;
    uint64_t diff = min - now;
    return diff > (uint64_t)INT_MAX ? INT_MAX : (int)diff;
}

static void check_all_timers(app_t *a, uint64_t now)
{
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
        uint64_t late    = jitter_buffer_late_count(a->jb);
        uint64_t silence = jitter_buffer_latched_silence_count(a->jb);
        float    jitter  = jitter_buffer_estimate_ms(a->jb);
        LOGW("link: watchdog timeout — treating as unkey  late=%llu silence=%llu jitter=%.1fms\n",
                (unsigned long long)late, (unsigned long long)silence, jitter);
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
    if (argc > 1)
        config_path = argv[1];

    /* app_t embeds config_t directly (messages[64] x elements[32] x
     * voice_words[32] adds up to several MB) -- heap-allocate it rather
     * than risk overflowing the thread stack. */
    app_t *a = calloc(1, sizeof(*a));
    if (!a) {
        LOGE("usrp-rc: out of memory\n");
        return 1;
    }
    a->link_sock = -1;

    if (config_load(&a->cfg, config_path) != 0) {
        LOGE("usrp-rc: failed to load config: %s\n", config_path);
        return 1;
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

    const char *vocab_dirs[] = {
        "user_8k", "vocab_8k",
        "/usr/local/share/usrp-rc/user_8k", "/usr/local/share/usrp-rc/vocab_8k",
    };
    if (vocab_cache_create(&a->vocab, vocab_dirs, 4) != 0) {
        LOGE("usrp-rc: failed to init vocab cache\n");
        return 1;
    }

    if (port_create(&a->port, &a->cfg, a->vocab) != 0) {
        LOGE("usrp-rc: failed to init port state machine\n");
        return 1;
    }
    port_set_ptt_callback(a->port, on_port_ptt, a);

    if (ste_create(&a->ste, a->cfg.audio.ste_delay_ms) != 0) {
        LOGE("usrp-rc: failed to init STE buffer\n");
        return 1;
    }

    if (a->cfg.link.enabled) {
        if (jitter_buffer_create(&a->jb, JITTER_BUF_DEFAULT) != 0) {
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
    port_destroy(a->port);
    vocab_cache_destroy(a->vocab);
    free(a);
    return 0;
}
