#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct vocab_cache vocab_cache_t;

/* Eagerly loads every *.wav in each directory (must be 8 kHz mono 16-bit
 * PCM) into memory. Directories are given in priority order — a clip name
 * already loaded from an earlier directory is not overwritten by a later
 * one, so pass user_8k/ before vocab_8k/ to let user clips override stock
 * ones. Missing directories are skipped silently. */
int vocab_cache_create(vocab_cache_t **out, const char *const *dirs, int ndirs);

/* Look up a clip by name (case-insensitive). Returns NULL if not found;
 * otherwise *out_n receives the sample count. The returned pointer is
 * owned by the cache — do not free it. */
const int16_t *vocab_get(vocab_cache_t *vc, const char *name, size_t *out_n);

void vocab_cache_destroy(vocab_cache_t *vc);
