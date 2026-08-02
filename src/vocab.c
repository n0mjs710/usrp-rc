#include "vocab.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>

#define VOCAB_EXPECTED_RATE 8000

typedef struct {
    char     name[64];
    int16_t *samples;
    size_t   n;
} vocab_clip_t;

struct vocab_cache {
    vocab_clip_t *clips;
    int           n;
    int           cap;
};

static uint32_t rd_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16le(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* Parse a canonical PCM WAV file. Returns malloc'd int16 samples (mono) via
 * *out, sample count via *out_n. Returns 0 on success, -1 on error. */
static int load_wav(const char *path, int16_t **out, size_t *out_n)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return -1;

    uint8_t hdr[12];
    if (fread(hdr, 1, 12, fp) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fclose(fp);
        return -1;
    }

    uint16_t channels = 0, bits = 0;
    uint32_t rate = 0;
    int16_t *samples = NULL;
    size_t   nsamples = 0;
    int      have_fmt = 0, have_data = 0;

    for (;;) {
        uint8_t chdr[8];
        if (fread(chdr, 1, 8, fp) != 8)
            break;
        uint32_t csize = rd_u32le(chdr + 4);

        if (memcmp(chdr, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (csize < 16 || fread(fmt, 1, 16, fp) != 16) { fclose(fp); free(samples); return -1; }
            channels = rd_u16le(fmt + 2);
            rate     = rd_u32le(fmt + 4);
            bits     = rd_u16le(fmt + 14);
            have_fmt = 1;
            if (csize > 16)
                fseek(fp, (long)(csize - 16), SEEK_CUR);
            if (csize & 1) fseek(fp, 1, SEEK_CUR);
        } else if (memcmp(chdr, "data", 4) == 0) {
            if (!have_fmt || channels != 1 || bits != 16) {
                fseek(fp, (long)csize + (long)(csize & 1), SEEK_CUR);
                continue;
            }
            nsamples = csize / 2;
            samples = malloc(nsamples * sizeof(int16_t));
            if (!samples) { fclose(fp); return -1; }
            uint8_t *raw = malloc(csize);
            if (!raw || fread(raw, 1, csize, fp) != csize) {
                free(raw); free(samples); fclose(fp); return -1;
            }
            for (size_t i = 0; i < nsamples; i++)
                samples[i] = (int16_t)rd_u16le(raw + i * 2);
            free(raw);
            have_data = 1;
            if (csize & 1) fseek(fp, 1, SEEK_CUR);
        } else {
            fseek(fp, (long)csize + (long)(csize & 1), SEEK_CUR);
        }
    }
    fclose(fp);

    if (!have_fmt || !have_data) {
        free(samples);
        return -1;
    }
    if (rate != VOCAB_EXPECTED_RATE) {
        fprintf(stderr, "vocab: %s: sample rate %u Hz != %d Hz — load anyway (unresampled)\n",
                path, rate, VOCAB_EXPECTED_RATE);
    }

    *out    = samples;
    *out_n  = nsamples;
    return 0;
}

static int has_clip(struct vocab_cache *vc, const char *name)
{
    for (int i = 0; i < vc->n; i++)
        if (strcmp(vc->clips[i].name, name) == 0)
            return 1;
    return 0;
}

static void load_dir(struct vocab_cache *vc, const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len < 5 || strcasecmp(ent->d_name + len - 4, ".wav") != 0)
            continue;

        char name[64] = {0};
        size_t namelen = len - 4;
        if (namelen >= sizeof(name))
            namelen = sizeof(name) - 1;
        for (size_t i = 0; i < namelen; i++)
            name[i] = (char)toupper((unsigned char)ent->d_name[i]);

        if (has_clip(vc, name))
            continue;   /* earlier directory already provided this clip */

        char path[768];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

        int16_t *samples;
        size_t   n;
        if (load_wav(path, &samples, &n) != 0) {
            fprintf(stderr, "vocab: failed to load %s\n", path);
            continue;
        }

        if (vc->n >= vc->cap) {
            int new_cap = vc->cap ? vc->cap * 2 : 256;
            vc->clips = realloc(vc->clips, (size_t)new_cap * sizeof(vocab_clip_t));
            vc->cap   = new_cap;
        }
        vocab_clip_t *c = &vc->clips[vc->n++];
        memcpy(c->name, name, sizeof(c->name));   /* name[] is already NUL-padded */
        c->samples = samples;
        c->n       = n;
    }
    closedir(d);
}

int vocab_cache_create(vocab_cache_t **out, const char *const *dirs, int ndirs)
{
    struct vocab_cache *vc = calloc(1, sizeof(*vc));
    if (!vc)
        return -1;

    for (int i = 0; i < ndirs; i++)
        load_dir(vc, dirs[i]);

    fprintf(stderr, "vocab: %d clips loaded\n", vc->n);
    *out = vc;
    return 0;
}

const int16_t *vocab_get(vocab_cache_t *vc, const char *name, size_t *out_n)
{
    if (!vc || !name)
        return NULL;
    char key[64] = {0};
    size_t i;
    for (i = 0; name[i] && i < sizeof(key) - 1; i++)
        key[i] = (char)toupper((unsigned char)name[i]);
    key[i] = '\0';

    for (int j = 0; j < vc->n; j++) {
        if (strcmp(vc->clips[j].name, key) == 0) {
            *out_n = vc->clips[j].n;
            return vc->clips[j].samples;
        }
    }
    return NULL;
}

void vocab_cache_destroy(vocab_cache_t *vc)
{
    if (!vc)
        return;
    for (int i = 0; i < vc->n; i++)
        free(vc->clips[i].samples);
    free(vc->clips);
    free(vc);
}
