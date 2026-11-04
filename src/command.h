#pragma once

#include "config.h"

#include <stdint.h>
#include <stdbool.h>

/* Over-the-air DTMF control: turns decoded tones into commands and decides what
 * happens to the audio while they're being entered.
 *
 * Commands follow the ACC three-tier structure: user commands, control operator
 * commands behind a prefix, and programming commands behind an unlock. Only the
 * user tier has commands so far (see command.c).
 *
 * Everything is clocked by the audio frames passed in, one per 20 ms of
 * received audio, so nothing here needs a timer of its own. */

#define COMMAND_MAX_DIGITS 32

typedef enum {
    COMMAND_PAD_TEST,   /* read `digits` back to the user */
    COMMAND_TIME,
    COMMAND_LINK_ON,
    COMMAND_LINK_OFF,
} command_type_t;

typedef struct {
    command_type_t type;
    char           digits[COMMAND_MAX_DIGITS + 1];
} command_t;

typedef struct command_session command_session_t;

int  command_session_create(command_session_t **out, const config_t *cfg);
void command_session_destroy(command_session_t *s);

/* Feed one 160-sample frame of received audio, before any delay or muting.
 * Returns true if the terminator digit (D) completed a valid command. */
bool command_session_frame(command_session_t *s, const int16_t frame[160], command_t *out);

/* The transmission ended: evaluate what was entered and start clean for the next
 * one. Returns true if a valid command was entered. */
bool command_session_end_of_tx(command_session_t *s, command_t *out);

/* Should repeat audio be replaced right now? Set from the first valid tone until
 * the mute hang expires with no further tone. */
bool command_session_muted(const command_session_t *s);

/* Should the link stay down? Set by the first valid tone, for the rest of the
 * transmission. */
bool command_session_link_cut(const command_session_t *s);

/* Replace a frame with what plays during the mute: silence and the beep cadence. */
void command_session_fill_mute(command_session_t *s, int16_t frame[160]);

/* The voice clip name that speaks a digit: ZERO..NINE, A, B, C, D, SIERRA for
 * star (ACC's convention). NULL if it isn't a sequence digit. */
const char *command_digit_clip(char digit);
