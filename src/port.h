#pragma once

#include "config.h"
#include "vocab.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    PORT_IDLE,
    PORT_PENDING,   /* reserved for cor_ctcss; unreachable in v1 (single keyup bit) */
    PORT_ACTIVE,
    PORT_TAIL,
    PORT_TIMEOUT,
} port_state_t;

typedef enum {
    SRC_LOCAL,   /* last active transmission originated on the mmdvm port */
    SRC_LINK,    /* last active transmission originated on the link port */
} port_source_t;

typedef struct port port_t;

int  port_create(port_t **out, const config_t *cfg, vocab_cache_t *vocab);
void port_destroy(port_t *p);

/* Called once main.c is ready to accept PTT edges (KEY/UNKEY frames to
 * mmdvm) and pacing-timer (re)arm requests. */
void port_set_ptt_callback(port_t *p, void (*cb)(void *arg, bool active), void *arg);

/* Kicks off the startup message + initial ID sequence if configured.
 * Call once after wiring is complete. */
void port_start(port_t *p, uint64_t now);

/* Edge-triggered: call only when the keyup bit changes on that port. */
void port_on_mmdvm_keyup(port_t *p, bool active, uint64_t now);
void port_on_link_keyup(port_t *p, bool active, uint64_t now);

/* Timer aggregation for the epoll loop's timeout computation and firing. */
uint64_t port_next_deadline_ms(const port_t *p);   /* absolute ms; 0 = none armed */
void     port_check_timers(port_t *p, uint64_t now);

/* State queries. */
port_state_t  port_state(const port_t *p);
bool          port_ptt(const port_t *p);
bool          port_mmdvm_active(const port_t *p);
bool          port_link_active(const port_t *p);
port_source_t port_last_source(const port_t *p);

/* Forwarding gates: whether main.c should currently be repeating audio
 * received on that port out to the *other* port. Both are false during
 * TIMEOUT (repeater locked out). */
bool port_mmdvm_gate_open(const port_t *p);   /* mmdvm RX -> repeat + STE to link */
bool port_link_gate_open(const port_t *p);    /* link RX -> mmdvm TX (mmdvm has priority) */

/* Pulls the next 160-sample controller-audio frame (CT/hang-silence/ID/
 * timeout messages) for the mmdvm TX pacing timer. Returns false (nothing
 * written) if PTT is currently off; otherwise always fills 160 samples
 * (silence-padded on underrun) and returns true. */
bool port_ctrl_pull(port_t *p, int16_t out[160], uint64_t now);

/* Additively mixes any in-flight impolite-ID audio into an outgoing 160-
 * sample mmdvm TX frame (repeat, link-forward, or controller-pull path —
 * call this on every frame built for mmdvm TX, from any source). */
void port_mix_apply(port_t *p, int16_t *payload, uint64_t now);
