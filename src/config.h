#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define CFG_MAX_MESSAGES     64
#define CFG_MAX_ELEMENTS     32
#define CFG_MAX_ID_ROTATION  8
#define CFG_STR              128

typedef enum {
    ELEM_CW,
    ELEM_VOICE,
    ELEM_TONE,
} elem_type_t;

typedef struct {
    elem_type_t type;
    /* cw */
    char     cw_text[CFG_STR];
    /* voice */
    char     voice_clip[CFG_STR];
    /* tone */
    double   freq1;
    double   freq2;
    int      ms;
    double   amp;
} config_elem_t;

typedef struct {
    char           name[CFG_STR];
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
    int      morse_wpm;
    int      morse_pitch;
    double   morse_level;
    double   impolite_morse_level;
    double   voice_level;
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
    char startup_message[CFG_STR];
    char initial_ids[CFG_MAX_ID_ROTATION][CFG_STR];
    int  n_initial_ids;
    char mandatory_ids[CFG_MAX_ID_ROTATION][CFG_STR];
    int  n_mandatory_ids;
    char anxious_id[CFG_STR];
    char impolite_id[CFG_STR];
    char ct_message[CFG_STR];
    char ct_link_message[CFG_STR];
    char timeout_message[CFG_STR];
    char timeout_cancel_message[CFG_STR];
} config_events_t;

typedef enum {
    ACCESS_COR = 0,
    ACCESS_COR_CTCSS = 1,
} access_mode_t;

typedef struct {
    config_mmdvm_t   mmdvm;
    config_link_t    link;
    config_audio_t   audio;
    config_timers_t  timers;
    config_events_t  events;
    access_mode_t    access_mode;

    config_message_t messages[CFG_MAX_MESSAGES];
    int               nmessages;
} config_t;

void config_defaults(config_t *cfg);
int  config_load(config_t *cfg, const char *path);

/* Look up a message by name; returns NULL if not found or name is empty. */
const config_message_t *config_find_message(const config_t *cfg, const char *name);
