/* C ABI of the weft-codec crate: MP3 encoding over LAME, and a gapless MP3 decoder
   (used for import, and by tests as an independent reference). Strings are UTF-8. */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WeftMp3Encoder WeftMp3Encoder;

/* vbr = 0: CBR at bitrate_kbps; otherwise VBR at vbr_quality (0 best .. 9 smallest). */
typedef struct WeftMp3Settings
{
    int sample_rate;
    int channels;
    int vbr;
    int bitrate_kbps;
    int vbr_quality;
    const char* title;
    const char* artist;
    const char* album;
    const char* year;
} WeftMp3Settings;

WeftMp3Encoder* weft_mp3_open (const char* path, const WeftMp3Settings* settings, char* err, size_t err_len);
int weft_mp3_write (WeftMp3Encoder* enc, const float* left, const float* right, size_t frames, char* err, size_t err_len);
/* Flushes, writes the LAME/Info header, closes and frees. */
int weft_mp3_finish (WeftMp3Encoder* enc, char* err, size_t err_len);
/* Closes and frees without finishing. */
void weft_mp3_abort (WeftMp3Encoder* enc);

typedef struct WeftMp3Decoded
{
    float* samples; /* interleaved */
    size_t frames;
    int channels;
    int sample_rate;
    char title[256];
    char artist[256];
} WeftMp3Decoded;

int weft_mp3_decode (const char* path, WeftMp3Decoded* out, char* err, size_t err_len);
void weft_mp3_free (WeftMp3Decoded* out);

#ifdef __cplusplus
}
#endif
