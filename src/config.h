#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define CFG_MAX_MESSAGES     64
#define CFG_MAX_ELEMENTS     32
#define CFG_MAX_ID_ROTATION  8
#define CFG_MAX_VOICE_WORDS  32
#define CFG_STR              128

/* Short identifier strings: vocab clip names, message names, and message-
 * name references (ct_message, initial_ids, etc.). None of these are free
 * text -- they're names you make up and refer back to -- so 32 bytes (31
 * usable characters) is deliberately tight to keep config_t's size down;
 * the longest real clip name in vocab_8k/ is 18 characters. Longer values
 * are silently truncated at parse time. Use CFG_STR instead for fields
 * that hold actual content (e.g. CW text) rather than a name. */
#define CFG_NAME_STR         32

typedef enum {
    ELEM_CW,
    ELEM_VOICE,
    ELEM_TONE,
} elem_type_t;

/* One word in a voice element's space-separated clip list: either a real
 * clip name, or a pause ("_" = cfg->audio.voice_gap_ms default, "_NNN" =
 * NNN ms explicit — recognized only when '_' is followed solely by digits
 * or nothing, so clip names like "_TEEN" are unaffected). */
typedef struct {
    bool     is_gap;
    int      gap_ms;             /* valid when is_gap; -1 = use configured default */
    char     clip[CFG_NAME_STR]; /* valid when !is_gap */
} config_voice_word_t;

typedef struct {
    elem_type_t type;
    /* cw */
    char     cw_text[CFG_STR];
    /* voice: one or more words, played back to back.
     * `clip = "THIS IS _ W ONE"` in TOML splits into 5 words here. */
    config_voice_word_t voice_words[CFG_MAX_VOICE_WORDS];
    int      n_voice_words;
    /* tone */
    double   freq1;
    double   freq2;
    int      ms;
    double   amp;
} config_elem_t;

typedef struct {
    char           name[CFG_NAME_STR];
    config_elem_t  elements[CFG_MAX_ELEMENTS];
    int            nelements;
} config_message_t;

typedef enum {
    LINK_CODEC_PCM  = 0,
    LINK_CODEC_OPUS = 1,
} link_codec_t;

typedef struct {
    char     local_address[64];
    uint16_t local_port;
    char     rpt_address[64];
    uint16_t rpt_port;
} config_mmdvm_t;

typedef struct {
    bool         enabled;   /* false = standalone local repeater; no link socket at all */
    char         remote_host[256];
    uint16_t     remote_port;
    uint16_t     local_port;
    char         bind_address[64];
    link_codec_t codec;
    int          opus_bitrate;
    int          opus_frame_ms;
} config_link_t;

typedef struct {
    double   master_gain;   /* final multiplier on all rendered controller audio */
    int      morse_wpm;
    int      morse_pitch;
    double   morse_level;
    double   impolite_morse_level;
    double   voice_level;
    int      voice_gap_ms;   /* default pause length for a bare "_" word */
    bool     voice_filter;   /* band-limit voice clips to match MMDVM-Host's RX filter -- see voice_filter.c */
    int      ste_delay_ms;
    int      pre_message_ms;
    int      post_message_ms;
} config_audio_t;

typedef struct {
    double hang;
    double ct_delay;
    double kerchunk;
    double timeout;
    double id_interval;
    double id_anxious;
} config_timers_t;

typedef struct {
    char startup_message[CFG_NAME_STR];
    char initial_ids[CFG_MAX_ID_ROTATION][CFG_NAME_STR];
    int  n_initial_ids;
    char mandatory_ids[CFG_MAX_ID_ROTATION][CFG_NAME_STR];
    int  n_mandatory_ids;
    char anxious_id[CFG_NAME_STR];
    char impolite_id[CFG_NAME_STR];
    char ct_message[CFG_NAME_STR];
    char ct_link_message[CFG_NAME_STR];
    char timeout_message[CFG_NAME_STR];
    char timeout_cancel_message[CFG_NAME_STR];
} config_events_t;

typedef enum {
    ACCESS_COR = 0,
    ACCESS_COR_CTCSS = 1,
} access_mode_t;

typedef struct {
    char level[16];   /* "error", "warn", "info", or "debug" */
} config_log_t;

typedef struct {
    config_mmdvm_t   mmdvm;
    config_link_t    link;
    config_audio_t   audio;
    config_timers_t  timers;
    config_events_t  events;
    access_mode_t    access_mode;
    config_log_t     log;

    config_message_t messages[CFG_MAX_MESSAGES];
    int               nmessages;
} config_t;

void config_defaults(config_t *cfg);
int  config_load(config_t *cfg, const char *path);

/* Look up a message by name; returns NULL if not found or name is empty. */
const config_message_t *config_find_message(const config_t *cfg, const char *name);
