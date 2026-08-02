#include "morse.h"
#include "tone.h"
#include "sbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct {
    char        ch;
    const char *pattern;
} morse_entry_t;

static const morse_entry_t MORSE_TABLE[] = {
    {'A', ".-"},    {'B', "-..."},  {'C', "-.-."},  {'D', "-.."},
    {'E', "."},     {'F', "..-."},  {'G', "--."},   {'H', "...."},
    {'I', ".."},    {'J', ".---"},  {'K', "-.-"},   {'L', ".-.."},
    {'M', "--"},    {'N', "-."},    {'O', "---"},   {'P', ".--."},
    {'Q', "--.-"},  {'R', ".-."},   {'S', "..."},   {'T', "-"},
    {'U', "..-"},   {'V', "...-"},  {'W', ".--"},   {'X', "-..-"},
    {'Y', "-.--"},  {'Z', "--.."},
    {'0', "-----"}, {'1', ".----"}, {'2', "..---"}, {'3', "...--"},
    {'4', "....-"}, {'5', "....."}, {'6', "-...."}, {'7', "--..."},
    {'8', "---.."}, {'9', "----."},
    {'.', ".-.-.-"}, {',', "--..--"}, {'?', "..--.."}, {'\'', ".----."},
    {'!', "-.-.--"}, {'/', "-..-."},  {'(', "-.--."},  {')', "-.--.-"},
    {'&', ".-..."},  {':', "---..."}, {';', "-.-.-."}, {'=', "-...-"},
    {'+', ".-.-."},  {'-', "-....-"}, {'_', "..--.-"}, {'"', ".-..-."},
    {'@', ".--.-."},
};
#define MORSE_TABLE_N (sizeof(MORSE_TABLE) / sizeof(MORSE_TABLE[0]))

static const char *lookup(char ch)
{
    for (size_t i = 0; i < MORSE_TABLE_N; i++)
        if (MORSE_TABLE[i].ch == ch)
            return MORSE_TABLE[i].pattern;
    return NULL;
}

static void sbuf_append_silence_s(sbuf_t *b, double seconds)
{
    sbuf_append_silence(b, (size_t)(seconds * MORSE_SAMPLE_RATE));
}

int16_t *morse_render(const char *text, int wpm, double pitch_hz, double level,
                      size_t *out_n)
{
    *out_n = 0;
    if (!text || !text[0] || wpm <= 0)
        return NULL;

    double dit  = 1.2 / (double)wpm;   /* seconds */
    double dah  = dit * 3.0;
    double ele  = dit;
    double chr  = dit * 3.0;
    double word = dit * 7.0;

    sbuf_t buf = {0};
    int    first_char_in_word = 1;

    for (const char *p = text; *p; p++) {
        char ch = (char)toupper((unsigned char)*p);

        if (ch == ' ') {
            sbuf_append_silence_s(&buf, word);
            first_char_in_word = 1;
            continue;
        }

        const char *pattern = lookup(ch);
        if (!pattern) {
            fprintf(stderr, "morse: no code for '%c' — skipped\n", ch);
            continue;
        }

        if (!first_char_in_word)
            sbuf_append_silence_s(&buf, chr);
        first_char_in_word = 0;

        for (int i = 0; pattern[i]; i++) {
            if (i > 0)
                sbuf_append_silence_s(&buf, ele);
            double dur_s = (pattern[i] == '-') ? dah : dit;
            size_t n;
            int16_t *tone = tone_render(pitch_hz, 0.0, (int)(dur_s * 1000.0), level, &n);
            if (tone) {
                sbuf_append(&buf, tone, n);
                free(tone);
            }
        }
    }

    if (buf.len == 0) {
        free(buf.data);
        return NULL;
    }

    *out_n = buf.len;
    return buf.data;
}
