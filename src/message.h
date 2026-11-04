#pragma once

#include "config.h"
#include "vocab.h"
#include <stdint.h>
#include <stddef.h>
#include <time.h>

/* Enough clips for any runtime variable, the longest being a number up to 99,999:
 * "ninety nine thousand nine hundred ninety nine". */
#define MESSAGE_VAR_MAX_CLIPS 8

/* Usage figures for the statistics variables. */
typedef struct {
    long keyups_day, keyups_week;
    long minutes_day, minutes_week;
} message_stats_t;

/* Where those come from when a message plays; without this they read as zero. */
typedef void (*message_stats_fn)(void *arg, message_stats_t *out);
void message_set_stats_source(message_stats_fn fn, void *arg);

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

/* Render voice clips by name, for replies built at run time rather than defined
 * in the config. A clip that doesn't exist is logged and skipped. Caller must
 * free(result.samples). */
rendered_audio_t message_render_clips(const config_t *cfg, vocab_cache_t *vocab,
                                      const char *const *clips, int nclips);

/* The clips that speak a runtime variable. Returns how many. */
int message_var_clips(voice_var_t var, const struct tm *when, const message_stats_t *stats,
                      const char *out[MESSAGE_VAR_MAX_CLIPS]);

/* Check every message's content against what's actually available: voice
 * clips exist in the vocab cache, CW text is all sendable. Logs each problem
 * and returns how many there were. Separate from config_load because the
 * vocab cache isn't loaded until after the config. */
int message_validate(const config_t *cfg, vocab_cache_t *vocab);
