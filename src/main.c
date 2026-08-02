#include "config.h"
#include "usrp_protocol.h"
#include "opus_codec.h"
#include "jitter_buffer.h"
#include "vocab.h"
#include "ste.h"
#include "port.h"
#include "util.h"

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
 * USRP port before we treat the link as unkeyed even without an explicit
 * UNKEY frame. Matches rusrp's own default network_timeout_ms. */
#define WATCHDOG_MS 500

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

    uint32_t mmdvm_tx_seq;
    uint32_t link_tx_seq;

    uint64_t mmdvm_watchdog_deadline;
    uint64_t link_watchdog_deadline;

    uint64_t pace_deadline;   /* 20 ms mmdvm-TX pacing timer; armed while port PTT is on */

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
    fprintf(stderr, "%s recv: %s (peer unreachable? check [%s] config — suppressing repeats for %ds)\n",
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
        fprintf(stderr, "invalid bind address: %s\n", bind_addr);
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        fprintf(stderr, "bind %s:%u: %s\n", bind_addr, bind_port, strerror(errno));
        close(fd);
        return -1;
    }

    struct sockaddr_in peer = { .sin_family = AF_INET, .sin_port = htons(peer_port) };
    if (inet_pton(AF_INET, peer_addr, &peer.sin_addr) != 1) {
        fprintf(stderr, "invalid peer address: %s\n", peer_addr);
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&peer, sizeof(peer)) < 0) {
        fprintf(stderr, "connect %s:%u: %s\n", peer_addr, peer_port, strerror(errno));
        close(fd);
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

/* ── outbound frame builders ───────────────────────────────────────────── */

static void send_mmdvm_voice(app_t *a, const int16_t frame[160], bool keyup)
{
    uint8_t buf[USRP_PKT_LEN];
    usrp_build_voice(buf, a->mmdvm_tx_seq++, keyup ? 1 : 0, frame);
    send(a->mmdvm_sock, buf, USRP_PKT_LEN, 0);
}

static void send_mmdvm_key(app_t *a, bool keyup)
{
    uint8_t buf[USRP_PKT_LEN];
    usrp_build_key(buf, a->mmdvm_tx_seq++, keyup ? 1 : 0);
    send(a->mmdvm_sock, buf, USRP_PKT_LEN, 0);
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

/* ── port PTT callback: KEY/UNKEY marker to mmdvm + arm/disarm pacing ────── */

static void on_port_ptt(void *arg, bool active)
{
    app_t *a = arg;
    uint64_t now = monotonic_ms();
    send_mmdvm_key(a, active);
    a->pace_deadline = active ? now + 20 : 0;
    fprintf(stderr, "mmdvm: PTT %s\n", active ? "ON" : "OFF");
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
    port_on_mmdvm_keyup(a->port, true, now);
    fprintf(stderr, "mmdvm: COR ACTIVE\n");
}

static void mmdvm_unkey_edge(app_t *a, uint64_t now)
{
    if (a->link_sock >= 0) {
        ste_reset(a->ste);
        if (a->cfg.link.codec == LINK_CODEC_OPUS && a->opus_accum_n > 0) {
            int frame_samples = opus_codec_frame_samples(a->opus);
            memset(a->opus_accum + a->opus_accum_n, 0,
                   (size_t)(frame_samples - a->opus_accum_n) * sizeof(int16_t));
            a->opus_accum_n = frame_samples;
            flush_opus_uplink(a);
        }
    }
    send_link_key(a, false);
    port_on_mmdvm_keyup(a->port, false, now);
    fprintf(stderr, "mmdvm: COR IDLE\n");
}

static void handle_mmdvm(app_t *a)
{
    uint8_t buf[USRP_PKT_LEN + 64];
    usrp_packet_t pkt;

    for (;;) {
        ssize_t n = recv(a->mmdvm_sock, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                log_recv_error("mmdvm", &a->mmdvm_recv_err_logged_ms, monotonic_ms());
            return;
        }
        if (usrp_parse(buf, (size_t)n, &pkt) != 0)
            continue;

        uint64_t now = monotonic_ms();
        a->mmdvm_watchdog_deadline = now + WATCHDOG_MS;

        bool keyup = pkt.keyup != 0;
        if (keyup != a->prev_mmdvm_keyup) {
            a->prev_mmdvm_keyup = keyup;
            if (keyup) mmdvm_key_edge(a, now);
            else       mmdvm_unkey_edge(a, now);
        }

        if (!port_mmdvm_gate_open(a->port))
            continue;

        /* Flow 1: local repeat, mmdvm RX -> mmdvm TX (+ impolite-ID mix). */
        int16_t frame[160];
        memcpy(frame, pkt.audio, sizeof(frame));
        port_mix_apply(a->port, frame, now);
        send_mmdvm_voice(a, frame, true);

        /* Flow 2: mmdvm RX -> link TX, through the STE delay gate. */
        if (a->link_sock >= 0) {
            int16_t ste_out[160];
            if (ste_push(a->ste, pkt.audio, ste_out))
                send_link_voice(a, ste_out);
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

        uint64_t now = monotonic_ms();
        a->link_watchdog_deadline = now + WATCHDOG_MS;

        bool keyup = pkt.keyup != 0;
        if (keyup != a->prev_link_keyup) {
            a->prev_link_keyup = keyup;
            if (!keyup)
                jitter_buffer_flush(a->jb);
            port_on_link_keyup(a->port, keyup, now);
            fprintf(stderr, "link: COR %s\n", keyup ? "ACTIVE" : "IDLE");
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
    uint64_t cand[4];
    int nc = 0;

    uint64_t pd = port_next_deadline_ms(a->port);
    if (pd) cand[nc++] = pd;
    if (a->pace_deadline) cand[nc++] = a->pace_deadline;
    if (a->prev_mmdvm_keyup && a->mmdvm_watchdog_deadline) cand[nc++] = a->mmdvm_watchdog_deadline;
    if (a->prev_link_keyup && a->link_watchdog_deadline)   cand[nc++] = a->link_watchdog_deadline;

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
        fprintf(stderr, "mmdvm: watchdog timeout — treating as unkey\n");
        a->prev_mmdvm_keyup = false;
        a->mmdvm_watchdog_deadline = 0;
        mmdvm_unkey_edge(a, now);
    }
    if (a->prev_link_keyup && a->link_watchdog_deadline && now >= a->link_watchdog_deadline) {
        fprintf(stderr, "link: watchdog timeout — treating as unkey\n");
        a->prev_link_keyup = false;
        a->link_watchdog_deadline = 0;
        jitter_buffer_flush(a->jb);
        port_on_link_keyup(a->port, false, now);
    }
}

/* ── main ──────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    const char *config_path = DEFAULT_CONFIG;
    if (argc > 1)
        config_path = argv[1];

    app_t a = {0};
    a.link_sock = -1;

    if (config_load(&a.cfg, config_path) != 0) {
        fprintf(stderr, "usrp-rc: failed to load config: %s\n", config_path);
        return 1;
    }

    if (a.cfg.link.enabled) {
        printf("usrp-rc: mmdvm %s:%u <-> %s:%u   link %s:%u <-> %s:%u (%s)\n",
               a.cfg.mmdvm.local_address, a.cfg.mmdvm.local_port,
               a.cfg.mmdvm.rpt_address,   a.cfg.mmdvm.rpt_port,
               a.cfg.link.bind_address,   a.cfg.link.local_port,
               a.cfg.link.remote_host,    a.cfg.link.remote_port,
               a.cfg.link.codec == LINK_CODEC_OPUS ? "opus" : "pcm");
    } else {
        printf("usrp-rc: mmdvm %s:%u <-> %s:%u   link disabled (standalone repeater)\n",
               a.cfg.mmdvm.local_address, a.cfg.mmdvm.local_port,
               a.cfg.mmdvm.rpt_address,   a.cfg.mmdvm.rpt_port);
    }
    printf("usrp-rc: %d messages loaded\n", a.cfg.nmessages);

    const char *vocab_dirs[] = {
        "user_8k", "vocab_8k",
        "/etc/usrp-rc/user_8k", "/etc/usrp-rc/vocab_8k",
    };
    if (vocab_cache_create(&a.vocab, vocab_dirs, 4) != 0) {
        fprintf(stderr, "usrp-rc: failed to init vocab cache\n");
        return 1;
    }

    if (port_create(&a.port, &a.cfg, a.vocab) != 0) {
        fprintf(stderr, "usrp-rc: failed to init port state machine\n");
        return 1;
    }
    port_set_ptt_callback(a.port, on_port_ptt, &a);

    if (a.cfg.link.enabled) {
        if (ste_create(&a.ste, a.cfg.audio.ste_delay_ms) != 0) {
            fprintf(stderr, "usrp-rc: failed to init STE buffer\n");
            return 1;
        }
        if (jitter_buffer_create(&a.jb, JITTER_BUF_DEFAULT) != 0) {
            fprintf(stderr, "usrp-rc: failed to init jitter buffer\n");
            return 1;
        }
        if (a.cfg.link.codec == LINK_CODEC_OPUS) {
            if (opus_codec_create(&a.opus, a.cfg.link.opus_bitrate, a.cfg.link.opus_frame_ms) != 0) {
                fprintf(stderr, "usrp-rc: failed to init opus codec\n");
                return 1;
            }
        }
    }

    struct sigaction sa = { .sa_handler = sig_handler };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    a.mmdvm_sock = make_udp_socket(a.cfg.mmdvm.local_address, a.cfg.mmdvm.local_port,
                                   a.cfg.mmdvm.rpt_address,   a.cfg.mmdvm.rpt_port);
    if (a.mmdvm_sock < 0)
        return 1;

    if (a.cfg.link.enabled) {
        a.link_sock = make_udp_socket(a.cfg.link.bind_address, a.cfg.link.local_port,
                                      a.cfg.link.remote_host,  a.cfg.link.remote_port);
        if (a.link_sock < 0) {
            close(a.mmdvm_sock);
            return 1;
        }
    }

    a.epfd = epoll_create1(0);
    if (a.epfd < 0) {
        perror("epoll_create1");
        close(a.mmdvm_sock);
        close(a.link_sock);
        return 1;
    }

    struct epoll_event ev = {0};
    ev.events  = EPOLLIN;
    ev.data.fd = a.mmdvm_sock;
    epoll_ctl(a.epfd, EPOLL_CTL_ADD, a.mmdvm_sock, &ev);
    if (a.link_sock >= 0) {
        ev.data.fd = a.link_sock;
        epoll_ctl(a.epfd, EPOLL_CTL_ADD, a.link_sock, &ev);
    }

    port_start(a.port, monotonic_ms());

    printf("usrp-rc: ready\n");

    struct epoll_event events[MAX_EVENTS];
    while (!g_stop) {
        uint64_t now = monotonic_ms();
        int timeout_ms = next_timeout_ms(&a, now);

        int n = epoll_wait(a.epfd, events, MAX_EVENTS, timeout_ms);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("epoll_wait");
            break;
        }

        now = monotonic_ms();
        check_all_timers(&a, now);

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == a.mmdvm_sock)
                handle_mmdvm(&a);
            else if (events[i].data.fd == a.link_sock)
                handle_link(&a);
        }
    }

    printf("usrp-rc: stopping\n");
    close(a.epfd);
    close(a.mmdvm_sock);
    if (a.link_sock >= 0) close(a.link_sock);
    if (a.opus) opus_codec_destroy(a.opus);
    jitter_buffer_destroy(a.jb);
    ste_destroy(a.ste);
    port_destroy(a.port);
    vocab_cache_destroy(a.vocab);
    return 0;
}
