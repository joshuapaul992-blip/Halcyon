/* Halcyon WAV library (userland/lib/wav.c): parse a RIFF WAVE file held in
 * memory and decode any stretch of it to 16-bit samples.
 *
 * Plays: PCM 8-bit (unsigned), 16, 24 and 32-bit (signed; other sample
 * sizes in those containers too, e.g. 20 bits in 3 bytes), IEEE float 32
 * and 64-bit, IMA ADPCM (4-bit, "DVI"), G.711 mu-law and A-law; also as
 * WAVE_FORMAT_EXTENSIBLE. Any number of channels (1-32): more than two are
 * mixed down to stereo by the usual layout (front, centre, LFE dropped,
 * back, side). Any rate (the caller resamples what its output cannot take).
 * Everything else is refused with a reason, and malformed files never make
 * it read outside the buffer (tests/sound fuzzes it).
 *
 * No allocation and no C library: the same file builds for Halcyon and for
 * the host tests. */
#ifndef WAV_H
#define WAV_H
#include <stddef.h>
#include <stdint.h>

enum { WAV_PCM = 1, WAV_FLOAT = 3, WAV_ALAW = 6, WAV_MULAW = 7, WAV_IMA_ADPCM = 0x11, WAV_EXTENSIBLE = 0xFFFE };

enum {
    WAV_OK = 0,
    WAV_E_NOTWAV = -1,      /* not a RIFF WAVE file */
    WAV_E_TRUNCATED = -2,   /* no format or data chunk, or the header is cut off */
    WAV_E_FORMAT = -3,      /* a compression this library does not decode (see wav_tag_name) */
    WAV_E_BADFMT = -4,      /* the format chunk's values make no sense */
    WAV_E_EMPTY = -5,       /* no sound in it */
};

typedef struct wav {
    const uint8_t *data;    /* the sample data (inside the caller's buffer) */
    uint64_t data_len;      /* bytes of it that are there */
    uint64_t frames;        /* sample frames (per channel) */
    int tag;                /* the format tag as stored (0xFFFE: see format) */
    int format;             /* WAV_PCM, WAV_FLOAT, WAV_ALAW, WAV_MULAW, WAV_IMA_ADPCM */
    int channels, rate;
    int bits;               /* valid bits per sample (4 for IMA ADPCM) */
    int container;          /* bytes per sample as stored (PCM / float / G.711) */
    int block_align;
    int frames_per_block;   /* IMA ADPCM: frames per block; otherwise 1 */
    int truncated;          /* the data chunk is longer than the file: what is there plays */
    uint32_t channel_mask;  /* WAVE_FORMAT_EXTENSIBLE, else 0 */
} wav_t;

/* parse the file in buf (it must stay valid while w is used): WAV_OK or WAV_E_* */
int  wav_open(wav_t *w, const void *buf, size_t size);
const char *wav_error(int err);                 /* "not a WAV file"... */
const char *wav_tag_name(int tag);              /* "MP3", "Microsoft ADPCM"...: for WAV_E_FORMAT */
const char *wav_format_name(const wav_t *w);    /* "PCM", "IEEE float", "IMA ADPCM", "mu-law", "A-law" */
int  wav_out_channels(const wav_t *w);          /* 1 for mono files, else 2 (more are mixed down) */
/* a rate in [min, max] to play it at: the file's own, or a whole multiple
 * of it (each frame repeated *rep times) or a whole fraction (every *dec-th
 * frame); 0 if there is none */
int  wav_stream_rate(const wav_t *w, int min, int max, int *rep, int *dec);
/* decode up to n frames from frame first into out (out_ch = 1 or 2
 * interleaved 16-bit samples); returns the frames written (0 at the end) */
long wav_read(const wav_t *w, uint64_t first, int16_t *out, long n, int out_ch);

#endif
