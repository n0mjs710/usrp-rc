#include "config.h"
#include "toml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

void config_defaults(config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    strncpy(cfg->mmdvm.local_address, "127.0.0.1", sizeof(cfg->mmdvm.local_address) - 1);
    cfg->mmdvm.local_port = 34001;
    strncpy(cfg->mmdvm.rpt_address, "127.0.0.1", sizeof(cfg->mmdvm.rpt_address) - 1);
    cfg->mmdvm.rpt_port = 32001;

    cfg->link.remote_port  = 34001;
    cfg->link.local_port   = 34002;
    strncpy(cfg->link.bind_address, "0.0.0.0", sizeof(cfg->link.bind_address) - 1);
    cfg->link.codec         = LINK_CODEC_PCM;
    cfg->link.opus_bitrate  = 8000;
    cfg->link.opus_frame_ms = 60;

    cfg->audio.morse_wpm            = 20;
    cfg->audio.morse_pitch          = 700;
    cfg->audio.morse_level          = 0.9;
    cfg->audio.impolite_morse_level = 0.3;
    cfg->audio.voice_level          = 0.9;
    cfg->audio.ste_delay_ms         = 0;
    cfg->audio.pre_message_ms       = 0;
    cfg->audio.post_message_ms      = 0;

    cfg->timers.hang        = 2.5;
    cfg->timers.ct_delay    = 0.5;
    cfg->timers.kerchunk    = 0.5;
    cfg->timers.timeout     = 180.0;
    cfg->timers.id_interval = 600.0;
    cfg->timers.id_anxious  = 60.0;

    cfg->access_mode = ACCESS_COR;
}

/* ── scalar helpers ─────────────────────────────────────────────────────── */

static void read_str(toml_table_t *tbl, const char *key, char *dst, size_t dstsz)
{
    toml_datum_t d = toml_string_in(tbl, key);
    if (d.ok) {
        strncpy(dst, d.u.s, dstsz - 1);
        dst[dstsz - 1] = '\0';
        free(d.u.s);
    }
}

static void read_uint16(toml_table_t *tbl, const char *key, uint16_t *dst)
{
    toml_datum_t d = toml_int_in(tbl, key);
    if (d.ok && d.u.i > 0 && d.u.i <= 65535)
        *dst = (uint16_t)d.u.i;
}

static void read_int(toml_table_t *tbl, const char *key, int *dst)
{
    toml_datum_t d = toml_int_in(tbl, key);
    if (d.ok)
        *dst = (int)d.u.i;
}

static void read_double(toml_table_t *tbl, const char *key, double *dst)
{
    toml_datum_t d = toml_double_in(tbl, key);
    if (d.ok)
        *dst = d.u.d;
}

/* Read a TOML array of strings into a fixed-size C array of fixed-size strings. */
static int read_str_array(toml_table_t *tbl, const char *key,
                          char dst[][CFG_STR], int max, size_t elem_sz)
{
    toml_array_t *arr = toml_array_in(tbl, key);
    if (!arr)
        return 0;
    int n = toml_array_nelem(arr);
    int count = 0;
    for (int i = 0; i < n && count < max; i++) {
        toml_datum_t d = toml_string_at(arr, i);
        if (d.ok) {
            strncpy(dst[count], d.u.s, elem_sz - 1);
            dst[count][elem_sz - 1] = '\0';
            free(d.u.s);
            count++;
        }
    }
    return count;
}

/* ── message elements ──────────────────────────────────────────────────── */

static void read_message_elements(toml_table_t *msg_tbl, config_message_t *out)
{
    toml_array_t *elems = toml_array_in(msg_tbl, "elements");
    if (!elems)
        return;

    int n = toml_array_nelem(elems);
    for (int i = 0; i < n && out->nelements < CFG_MAX_ELEMENTS; i++) {
        toml_table_t *e = toml_table_at(elems, i);
        if (!e)
            continue;

        char type[32] = {0};
        read_str(e, "type", type, sizeof(type));

        config_elem_t *elem = &out->elements[out->nelements];
        memset(elem, 0, sizeof(*elem));

        if (strcmp(type, "cw") == 0) {
            elem->type = ELEM_CW;
            read_str(e, "text", elem->cw_text, sizeof(elem->cw_text));
        } else if (strcmp(type, "voice") == 0) {
            elem->type = ELEM_VOICE;
            read_str(e, "clip", elem->voice_clip, sizeof(elem->voice_clip));
        } else if (strcmp(type, "tone") == 0 || strcmp(type, "ct") == 0) {
            elem->type = ELEM_TONE;
            elem->ms   = 80;
            elem->amp  = 0.8;
            read_double(e, "freq1", &elem->freq1);
            read_double(e, "freq2", &elem->freq2);
            read_int(e,    "ms",    &elem->ms);
            read_double(e, "amp",   &elem->amp);
        } else {
            fprintf(stderr, "config: message '%s' element %d: unknown type '%s' — skipped\n",
                    out->name, i, type);
            continue;
        }
        out->nelements++;
    }
}

static void read_messages(toml_table_t *root, config_t *cfg)
{
    toml_table_t *messages = toml_table_in(root, "messages");
    if (!messages)
        return;

    for (int i = 0; ; i++) {
        const char *key = toml_key_in(messages, i);
        if (!key)
            break;
        toml_table_t *msg_tbl = toml_table_in(messages, key);
        if (!msg_tbl)
            continue;   /* not a sub-table */

        if (cfg->nmessages >= CFG_MAX_MESSAGES) {
            fprintf(stderr, "config: too many [messages.*] entries (max %d) — '%s' skipped\n",
                    CFG_MAX_MESSAGES, key);
            continue;
        }

        config_message_t *out = &cfg->messages[cfg->nmessages];
        memset(out, 0, sizeof(*out));
        strncpy(out->name, key, sizeof(out->name) - 1);
        read_message_elements(msg_tbl, out);
        cfg->nmessages++;
    }
}

/* ── validation ─────────────────────────────────────────────────────────── */

static int validate(const config_t *cfg)
{
    int ok = 1;

    if (cfg->mmdvm.local_port == 0 || cfg->mmdvm.rpt_port == 0) {
        fprintf(stderr, "config: mmdvm ports must be 1-65535\n");
        ok = 0;
    }
    if (cfg->link.remote_host[0] == '\0') {
        fprintf(stderr, "config: link.remote_host is required\n");
        ok = 0;
    }
    if (cfg->link.remote_port == 0 || cfg->link.local_port == 0) {
        fprintf(stderr, "config: link ports must be 1-65535\n");
        ok = 0;
    }
    if (cfg->link.codec == LINK_CODEC_OPUS) {
        if (cfg->link.opus_bitrate < 4000 || cfg->link.opus_bitrate > 64000) {
            fprintf(stderr, "config: link.opus_bitrate must be 4000-64000 bps\n");
            ok = 0;
        }
        if (cfg->link.opus_frame_ms != 20 && cfg->link.opus_frame_ms != 40
            && cfg->link.opus_frame_ms != 60) {
            fprintf(stderr, "config: link.opus_frame_ms must be 20, 40, or 60\n");
            ok = 0;
        }
    }
    return ok ? 0 : -1;
}

/* ── public ─────────────────────────────────────────────────────────────── */

int config_load(config_t *cfg, const char *path)
{
    config_defaults(cfg);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "config: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    char errbuf[256];
    toml_table_t *root = toml_parse_file(fp, errbuf, sizeof(errbuf));
    fclose(fp);

    if (!root) {
        fprintf(stderr, "config: parse error in %s: %s\n", path, errbuf);
        return -1;
    }

    toml_table_t *t;

    if ((t = toml_table_in(root, "mmdvm"))) {
        read_str(t,     "local_address", cfg->mmdvm.local_address, sizeof(cfg->mmdvm.local_address));
        read_uint16(t,  "local_port",    &cfg->mmdvm.local_port);
        read_str(t,     "rpt_address",   cfg->mmdvm.rpt_address, sizeof(cfg->mmdvm.rpt_address));
        read_uint16(t,  "rpt_port",      &cfg->mmdvm.rpt_port);
    }

    if ((t = toml_table_in(root, "link"))) {
        read_str(t,    "remote_host",  cfg->link.remote_host, sizeof(cfg->link.remote_host));
        read_uint16(t, "remote_port",  &cfg->link.remote_port);
        read_uint16(t, "local_port",   &cfg->link.local_port);
        read_str(t,    "bind_address", cfg->link.bind_address, sizeof(cfg->link.bind_address));
        char codec[16] = {0};
        read_str(t, "codec", codec, sizeof(codec));
        if (codec[0]) {
            if (strcmp(codec, "opus") == 0)      cfg->link.codec = LINK_CODEC_OPUS;
            else if (strcmp(codec, "pcm") == 0)  cfg->link.codec = LINK_CODEC_PCM;
            else fprintf(stderr, "config: unknown link.codec '%s'; using pcm\n", codec);
        }
        read_int(t, "opus_bitrate",  &cfg->link.opus_bitrate);
        read_int(t, "opus_frame_ms", &cfg->link.opus_frame_ms);
    }

    if ((t = toml_table_in(root, "audio"))) {
        read_int(t,    "morse_wpm",            &cfg->audio.morse_wpm);
        read_int(t,    "morse_pitch",           &cfg->audio.morse_pitch);
        read_double(t, "morse_level",           &cfg->audio.morse_level);
        read_double(t, "impolite_morse_level",  &cfg->audio.impolite_morse_level);
        read_double(t, "voice_level",           &cfg->audio.voice_level);
        read_int(t,    "ste_delay_ms",          &cfg->audio.ste_delay_ms);
        read_int(t,    "pre_message_ms",        &cfg->audio.pre_message_ms);
        read_int(t,    "post_message_ms",       &cfg->audio.post_message_ms);
    }

    if ((t = toml_table_in(root, "timers"))) {
        read_double(t, "hang",        &cfg->timers.hang);
        read_double(t, "ct_delay",    &cfg->timers.ct_delay);
        read_double(t, "kerchunk",    &cfg->timers.kerchunk);
        read_double(t, "timeout",     &cfg->timers.timeout);
        read_double(t, "id_interval", &cfg->timers.id_interval);
        read_double(t, "id_anxious",  &cfg->timers.id_anxious);
    }

    if ((t = toml_table_in(root, "events"))) {
        read_str(t, "startup_message",        cfg->events.startup_message, sizeof(cfg->events.startup_message));
        read_str(t, "anxious_id",              cfg->events.anxious_id, sizeof(cfg->events.anxious_id));
        read_str(t, "impolite_id",             cfg->events.impolite_id, sizeof(cfg->events.impolite_id));
        read_str(t, "ct_message",              cfg->events.ct_message, sizeof(cfg->events.ct_message));
        read_str(t, "ct_link_message",         cfg->events.ct_link_message, sizeof(cfg->events.ct_link_message));
        read_str(t, "timeout_message",         cfg->events.timeout_message, sizeof(cfg->events.timeout_message));
        read_str(t, "timeout_cancel_message",  cfg->events.timeout_cancel_message, sizeof(cfg->events.timeout_cancel_message));
        cfg->events.n_initial_ids = read_str_array(t, "initial_ids",
            cfg->events.initial_ids, CFG_MAX_ID_ROTATION, CFG_STR);
        cfg->events.n_mandatory_ids = read_str_array(t, "mandatory_ids",
            cfg->events.mandatory_ids, CFG_MAX_ID_ROTATION, CFG_STR);
    }

    {
        char am[16] = {0};
        read_str(root, "access_mode", am, sizeof(am));
        if (am[0]) {
            if (strcmp(am, "cor") == 0)             cfg->access_mode = ACCESS_COR;
            else if (strcmp(am, "cor_ctcss") == 0)  cfg->access_mode = ACCESS_COR_CTCSS;
            else fprintf(stderr, "config: unknown access_mode '%s'; using cor\n", am);
        }
    }

    read_messages(root, cfg);

    toml_free(root);
    return validate(cfg);
}

const config_message_t *config_find_message(const config_t *cfg, const char *name)
{
    if (!name || !name[0])
        return NULL;
    for (int i = 0; i < cfg->nmessages; i++) {
        if (strcmp(cfg->messages[i].name, name) == 0)
            return &cfg->messages[i];
    }
    return NULL;
}
