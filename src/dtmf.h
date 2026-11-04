#pragma once

#include <stdint.h>
#include <stdbool.h>

/* DTMF tone detector for 8 kHz mono s16 audio: a sliding-window Goertzel bank
 * clocked purely by the samples fed to it, so it needs no wall clock and is
 * fully deterministic under test.
 *
 * It knows only about tones, not commands. Sequence handling, muting and timers
 * above the single-tone level live in command.c. */

/* A frame holds two analyses, and each can end one tone and begin another. */
#define DTMF_MAX_EVENTS 4

typedef enum {
    DTMF_TONE_ON,    /* held for min_tone_ms: now a valid digit */
    DTMF_TONE_OFF,   /* a valid digit ended; ms is how long it lasted */
} dtmf_event_type_t;

typedef struct {
    dtmf_event_type_t type;
    char              digit;   /* 0-9 * # A B C D */
    int               ms;      /* DTMF_TONE_ON: time held so far; OFF: total */
} dtmf_event_t;

typedef struct dtmf_decoder dtmf_decoder_t;

int  dtmf_decoder_create(dtmf_decoder_t **out, int min_tone_ms);
void dtmf_decoder_destroy(dtmf_decoder_t *d);

/* Forget everything; call between transmissions so one can't affect the next. */
void dtmf_decoder_reset(dtmf_decoder_t *d);

/* Feed one 160-sample frame. Returns the number of events written to out. */
int dtmf_decoder_push(dtmf_decoder_t *d, const int16_t frame[160],
                      dtmf_event_t out[DTMF_MAX_EVENTS]);

/* How long the current valid tone has been held; 0 if none is active. */
int dtmf_decoder_active_ms(const dtmf_decoder_t *d);
