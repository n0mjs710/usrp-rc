#pragma once

#include "config.h"
#include "vocab.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    PORT_IDLE,
    PORT_ACTIVE,
    PORT_TAIL,
    PORT_TIMEOUT,   /* nothing repeated, and a source is timed out until it unkeys */
} port_state_t;

typedef enum {
    SRC_LOCAL,   /* last active transmission originated on the mmdvm port */
    SRC_LINK,    /* last active transmission originated on the link port */
} port_source_t;

typedef struct port port_t;

int  port_create(port_t **out, const config_t *cfg, vocab_cache_t *vocab);
void port_destroy(port_t *p);

/* Called when a transmission that counts as usage ends: not kerchunks or time spent
 * timed out, since neither is traffic being repeated. */
void port_set_activity_callback(port_t *p, void (*cb)(void *arg, double seconds), void *arg);

/* Called once main.c is ready to accept PTT edges (KEY/UNKEY frames to
 * mmdvm) and pacing-timer (re)arm requests. `reason` is a short static
 * string (e.g. "repeat", "mandatory-id", "hang-expired") naming why PTT
 * just changed, for logging — valid only for the duration of the call. */
void port_set_ptt_callback(port_t *p, void (*cb)(void *arg, bool active, const char *reason), void *arg);

/* Kicks off the startup message + initial ID sequence if configured.
 * Call once after wiring is complete. */
void port_start(port_t *p, uint64_t now);

/* Edge-triggered: call only when the keyup bit changes on that port. */
void port_on_mmdvm_keyup(port_t *p, bool active, uint64_t now);
void port_on_link_keyup(port_t *p, bool active, uint64_t now);

/* Queue a spoken command reply. It plays in the tail after the transmission, ahead
 * of the courtesy tone, and a new keyup interrupts it. Takes ownership of samples. */
void port_queue_reply(port_t *p, int16_t *samples, size_t n);

/* Echo test. Arming waits for the next local keyup, which port_take_echo() claims for
 * recording. If the tail ends first, echo_cancel_message plays. Going down clears it. */
void port_arm_echo(port_t *p);
bool port_take_echo(port_t *p);

/* Tell the port whether the link is on, which selects the local courtesy tone.
 * With no link configured, it stays off. */
void port_set_link_up(port_t *p, bool up);

/* Timer aggregation for the epoll loop's timeout computation and firing. */
uint64_t port_next_deadline_ms(const port_t *p);   /* absolute ms; 0 = none armed */
void     port_check_timers(port_t *p, uint64_t now);

/* State query. */
bool port_ptt(const port_t *p);

/* Forwarding gates: whether main.c should currently be repeating audio
 * received on that port out to the *other* port. A timed-out source's gate is
 * closed until it unkeys; the other source carries on. */
bool port_mmdvm_gate_open(const port_t *p);   /* mmdvm RX -> repeat + STE to link */
bool port_link_gate_open(const port_t *p);    /* link RX -> mmdvm TX (mmdvm has priority) */

/* Repeater down cuts every path through the repeater: local repeat, local to link,
 * and link to local. The receiver is still decoded for commands. Going down, the
 * reply plays and the station identifies; coming up, the reply plays. Both take
 * ownership of the reply, which may be NULL. */
void port_set_repeater_down(port_t *p, bool down, int16_t *reply, size_t n, uint64_t now);
/* Down at startup, from the saved state: nothing is transmitted. Call before port_start. */
void port_restore_repeater_down(port_t *p);
bool port_repeater_down(const port_t *p);

/* The local receiver is timed out: its audio goes nowhere, but DTMF commands
 * ending in D are still accepted. */
bool port_local_timed_out(const port_t *p);

/* Pulls the next 160-sample controller-audio frame (CT/hang-silence/ID/
 * timeout messages) for the mmdvm TX pacing timer, while PTT is on. Always
 * fills 160 samples, silence-padded on underrun. */
void port_ctrl_pull(port_t *p, int16_t out[160], uint64_t now);

/* Additively mixes any in-flight impolite-ID audio into an outgoing 160-
 * sample mmdvm TX frame (repeat, link-forward, or controller-pull path —
 * call this on every frame built for mmdvm TX, from any source). */
void port_mix_apply(port_t *p, int16_t *payload, uint64_t now);
