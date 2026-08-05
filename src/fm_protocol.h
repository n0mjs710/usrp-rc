#pragma once

#include <stdint.h>
#include <stddef.h>

/* MMDVM-Host's native "FM Network" protocol -- the UDP link between
 * MMDVM-Host and its FM gateway, spoken over the ports configured in
 * MMDVM-Host.ini's [FM Network] section (Host binds LocalPort and sends to
 * GatewayPort; we are the gateway, so we bind GatewayPort and send to
 * LocalPort).
 *
 * Implemented against MMDVM-Host's FMNetwork.cpp as the definitive source
 * (fmgateway's FMNetwork.cpp is the same wire format, but its receive path
 * scales S16LE->float by /65536 where MMDVM-Host uses /32767 -- 6 dB low.
 * We follow MMDVM-Host).
 *
 * Wire format is deliberately tiny: a 3-byte ASCII tag, and for data an
 * S16LE mono payload at 8 kHz.
 *
 *   "FMS" + callsign + NUL   start of an RF transmission (Host -> gateway)
 *   "FMD" + <S16LE samples>  audio, either direction
 *   "FME"                    end of an RF transmission (Host -> gateway)
 *   "FMP"                    keepalive ping, either direction
 *
 * ASYMMETRY THAT MATTERS: MMDVM-Host only ever *accepts* FMD from the
 * gateway -- its clock() discards FMS and FME outright (FMNetwork.cpp).
 * There is no way to tell the modem "key up" or "unkey"; PTT is implicit in
 * whether audio is arriving. Stopping the FMD flow is the only unkey there
 * is, which is also why an outbound delivery stall is indistinguishable
 * from a deliberate unkey at the modem -- see note_mmdvm_voice_pacing() in
 * main.c. */

#define FM_TAG_LEN        3u
#define FM_MAX_SAMPLES    160u  /* Host never sends more (FMControl.cpp caps at 160) */
#define FM_MAX_PKT_LEN    (FM_TAG_LEN + FM_MAX_SAMPLES * sizeof(int16_t))

/* Host sends a ping every 5 s; ours are symmetric and equally ignorable. */
#define FM_PING_INTERVAL_MS 5000u

/* Longest callsign we keep from an FMS packet. MMDVM-Host sends whatever is
 * in [FM] Callsign, NUL-terminated. */
#define FM_CALLSIGN_MAX   32u

typedef enum {
    FM_PKT_NONE = 0,   /* unrecognised -- caller should ignore */
    FM_PKT_START,      /* FMS */
    FM_PKT_DATA,       /* FMD */
    FM_PKT_END,        /* FME */
    FM_PKT_PING,       /* FMP */
} fm_pkt_type_t;

typedef struct {
    fm_pkt_type_t type;
    /* valid when type == FM_PKT_DATA. Count is variable: MMDVM-Host forwards
     * whatever the modem handed it, rounded down to a whole sample pair and
     * capped at 160 -- it is NOT reliably a 20 ms / 160-sample frame. */
    int16_t  audio[FM_MAX_SAMPLES];
    unsigned nsamples;
    /* valid when type == FM_PKT_START; always NUL-terminated. */
    char     callsign[FM_CALLSIGN_MAX];
} fm_packet_t;

/* Parse a raw UDP payload. Returns 0 on success (out->type says what it
 * was), -1 if the datagram is not FM-protocol at all. An unrecognised
 * "FM?" tag parses as FM_PKT_NONE rather than an error, matching how
 * MMDVM-Host treats it. */
int fm_parse(const uint8_t *buf, size_t len, fm_packet_t *out);

/* Build "FMD" + nsamples S16LE samples into buf (must be >= FM_MAX_PKT_LEN).
 * Returns the total packet length. nsamples must be <= FM_MAX_SAMPLES. */
size_t fm_build_data(uint8_t *buf, const int16_t *audio, unsigned nsamples);

/* Build a bare 3-byte tag packet ("FMP", "FME", ...). Returns FM_TAG_LEN. */
size_t fm_build_ping(uint8_t *buf);
size_t fm_build_end(uint8_t *buf);

/* Build "FMS" + callsign + NUL. Returns the total packet length.
 * MMDVM-Host ignores this from a gateway; provided for completeness and for
 * loopback testing. */
size_t fm_build_start(uint8_t *buf, const char *callsign);
