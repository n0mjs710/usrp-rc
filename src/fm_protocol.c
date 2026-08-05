#include "fm_protocol.h"

#include <string.h>

/* The wire is explicitly S16LE (MMDVM-Host writes val>>0 then val>>8), so
 * pack/unpack byte-wise rather than memcpy'ing native shorts. Costs nothing
 * on the Pi and keeps the format correct if this ever runs big-endian. */

static inline int16_t rd16le(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline void wr16le(uint8_t *p, int16_t v)
{
    p[0] = (uint8_t)((uint16_t)v & 0xFFu);
    p[1] = (uint8_t)(((uint16_t)v >> 8) & 0xFFu);
}

int fm_parse(const uint8_t *buf, size_t len, fm_packet_t *out)
{
    out->type     = FM_PKT_NONE;
    out->nsamples = 0;
    out->callsign[0] = '\0';

    if (len < FM_TAG_LEN)
        return -1;
    if (memcmp(buf, "FM", 2) != 0)
        return -1;

    if (memcmp(buf, "FMP", FM_TAG_LEN) == 0) {
        out->type = FM_PKT_PING;
        return 0;
    }

    if (memcmp(buf, "FME", FM_TAG_LEN) == 0) {
        out->type = FM_PKT_END;
        return 0;
    }

    if (memcmp(buf, "FMS", FM_TAG_LEN) == 0) {
        out->type = FM_PKT_START;
        /* Callsign is NUL-terminated by the sender, but never trust that:
         * bound the copy by both the datagram length and our buffer. */
        size_t avail = len - FM_TAG_LEN;
        if (avail > FM_CALLSIGN_MAX - 1)
            avail = FM_CALLSIGN_MAX - 1;
        size_t n = 0;
        while (n < avail && buf[FM_TAG_LEN + n] != '\0')
            n++;
        memcpy(out->callsign, buf + FM_TAG_LEN, n);
        out->callsign[n] = '\0';
        return 0;
    }

    if (memcmp(buf, "FMD", FM_TAG_LEN) == 0) {
        out->type = FM_PKT_DATA;
        unsigned n = (unsigned)((len - FM_TAG_LEN) / sizeof(int16_t));
        if (n > FM_MAX_SAMPLES)
            n = FM_MAX_SAMPLES;
        for (unsigned i = 0; i < n; i++)
            out->audio[i] = rd16le(buf + FM_TAG_LEN + i * 2u);
        out->nsamples = n;
        return 0;
    }

    /* An "FM?" tag we don't know: not an error, just nothing to do --
     * MMDVM-Host logs and drops these too. */
    return 0;
}

size_t fm_build_data(uint8_t *buf, const int16_t *audio, unsigned nsamples)
{
    if (nsamples > FM_MAX_SAMPLES)
        nsamples = FM_MAX_SAMPLES;

    memcpy(buf, "FMD", FM_TAG_LEN);
    for (unsigned i = 0; i < nsamples; i++)
        wr16le(buf + FM_TAG_LEN + i * 2u, audio[i]);

    return FM_TAG_LEN + (size_t)nsamples * sizeof(int16_t);
}

size_t fm_build_ping(uint8_t *buf)
{
    memcpy(buf, "FMP", FM_TAG_LEN);
    return FM_TAG_LEN;
}

size_t fm_build_end(uint8_t *buf)
{
    memcpy(buf, "FME", FM_TAG_LEN);
    return FM_TAG_LEN;
}

size_t fm_build_start(uint8_t *buf, const char *callsign)
{
    size_t n = strlen(callsign);
    if (n > FM_CALLSIGN_MAX - 1)
        n = FM_CALLSIGN_MAX - 1;

    memcpy(buf, "FMS", FM_TAG_LEN);
    memcpy(buf + FM_TAG_LEN, callsign, n);
    buf[FM_TAG_LEN + n] = '\0';

    return FM_TAG_LEN + n + 1u;
}
