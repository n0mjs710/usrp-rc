#include "message.h"
#include "tone.h"
#include "morse.h"
#include "sbuf.h"

#include <stdio.h>
#include <stdlib.h>

int message_has_voice(const config_t *cfg, const char *name)
{
    const config_message_t *m = config_find_message(cfg, name);
    if (!m)
        return 0;
    for (int i = 0; i < m->nelements; i++)
        if (m->elements[i].type == ELEM_VOICE)
            return 1;
    return 0;
}

int message_needs_padding(const config_t *cfg, const char *name)
{
    const config_message_t *m = config_find_message(cfg, name);
    if (!m)
        return 0;
    for (int i = 0; i < m->nelements; i++)
        if (m->elements[i].type == ELEM_CW || m->elements[i].type == ELEM_VOICE)
            return 1;
    return 0;
}

rendered_audio_t message_render(const config_t *cfg, vocab_cache_t *vocab,
                                const char *name, double morse_level_override)
{
    rendered_audio_t result = {0};
    const config_message_t *m = config_find_message(cfg, name);
    if (!m) {
        fprintf(stderr, "message: '%s' not found\n", name ? name : "(null)");
        return result;
    }
    if (m->nelements == 0) {
        fprintf(stderr, "message: '%s' has no elements\n", name);
        return result;
    }

    double morse_level = (morse_level_override >= 0.0)
                        ? morse_level_override : cfg->audio.morse_level;

    sbuf_t buf = {0};

    for (int i = 0; i < m->nelements; i++) {
        const config_elem_t *e = &m->elements[i];

        switch (e->type) {
        case ELEM_CW: {
            if (!e->cw_text[0]) {
                fprintf(stderr, "message: '%s' CW element has no text\n", name);
                break;
            }
            size_t n;
            int16_t *samples = morse_render(e->cw_text, cfg->audio.morse_wpm,
                                            (double)cfg->audio.morse_pitch,
                                            morse_level, &n);
            if (samples) {
                sbuf_append(&buf, samples, n);
                free(samples);
            }
            break;
        }
        case ELEM_VOICE: {
            if (!e->voice_clip[0]) {
                fprintf(stderr, "message: '%s' VOICE element has no clip\n", name);
                break;
            }
            size_t n;
            const int16_t *samples = vocab_get(vocab, e->voice_clip, &n);
            if (!samples) {
                fprintf(stderr, "message: voice clip '%s' not found\n", e->voice_clip);
                break;
            }
            sbuf_append_scaled(&buf, samples, n, cfg->audio.voice_level);
            break;
        }
        case ELEM_TONE: {
            size_t n;
            int16_t *samples = tone_render(e->freq1, e->freq2, e->ms, e->amp, &n);
            if (samples) {
                sbuf_append(&buf, samples, n);
                free(samples);
            }
            break;
        }
        }
    }

    result.samples = buf.data;
    result.n       = buf.len;
    return result;
}
