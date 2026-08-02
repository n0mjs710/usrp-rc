#pragma once

#include "config.h"
#include "vocab.h"
#include <stdint.h>
#include <stddef.h>

typedef struct {
    int16_t *samples;   /* malloc'd; NULL if empty */
    size_t   n;
} rendered_audio_t;

/* True if any element in the named message is CW or VOICE (used to decide
 * whether pre/post message padding applies, and whether an ID is a "voice
 * ID" subject to epoch interruption). */
int message_has_voice(const config_t *cfg, const char *name);
int message_needs_padding(const config_t *cfg, const char *name);

/* Render a named message to 8 kHz mono s16 samples.
 * morse_level_override: if >= 0.0, used instead of cfg->audio.morse_level
 * for CW elements in this message (impolite-ID ducking). Pass -1.0 for the
 * configured level. Returns {NULL, 0} if the message is missing/empty.
 * Caller must free(result.samples). */
rendered_audio_t message_render(const config_t *cfg, vocab_cache_t *vocab,
                                const char *name, double morse_level_override);
