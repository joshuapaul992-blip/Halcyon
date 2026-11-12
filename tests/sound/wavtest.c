/* Host tests of the WAV library (userland/lib/wav.c), built with ASan and
 * UBSan: every format against known values or an independent encoder
 * (G.711 and IMA ADPCM encoders written here from the standards), the
 * mix-down, random access, truncated and odd files, every rejection, and a
 * fuzzer that mutates valid files and decodes them at random positions.
 *   wavtest [-n fuzz-iterations] [system sound files...]
 * The files given (tools/mksounds.py's output) must parse as 16-bit PCM. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wav.h"

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------------- building files ---------------- */
typedef struct { uint8_t *b; size_t n, cap; } buf_t;
static void put(buf_t *f, const void *p, size_t n) {
    if (f->n + n > f->cap) { f->cap = (f->n + n) * 2 + 64; f->b = realloc(f->b, f->cap); }
    memcpy(f->b + f->n, p, n);
    f->n += n;
}
static void p16(buf_t *f, unsigned v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; put(f, b, 2); }
static void p32(buf_t *f, uint32_t v) { uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; put(f, b, 4); }
static void chunk(buf_t *f, const char *id, const void *data, uint32_t len) {
    put(f, id, 4);
    p32(f, len);
    put(f, data, len);
    if (len & 1) { uint8_t z = 0; put(f, &z, 1); }
}

/* a WAV file: fmt chunk fields, optional extra fmt bytes, data */
static buf_t mkwav(int tag, int ch, int rate, int bits, int align, const void *extra, int extra_len, const void *data, uint32_t len) {
    buf_t fmt = { 0 }, f = { 0 };
    p16(&fmt, (unsigned)tag); p16(&fmt, (unsigned)ch); p32(&fmt, (uint32_t)rate);
    p32(&fmt, (uint32_t)(rate * align)); p16(&fmt, (unsigned)align); p16(&fmt, (unsigned)bits);
    if (extra_len) put(&fmt, extra, (size_t)extra_len);
    put(&f, "RIFF", 4); p32(&f, 0); put(&f, "WAVE", 4);
    chunk(&f, "fmt ", fmt.b, (uint32_t)fmt.n);
    chunk(&f, "data", data, len);
    uint32_t riff = (uint32_t)f.n - 8;
    memcpy(f.b + 4, &riff, 4);
    free(fmt.b);
    return f;
}

static double tone(long i, double hz, int rate) { return sin(2 * M_PI * hz * (double)i / rate); }

/* ---------------- reference encoders (from the standards, independent of wav.c) ---------------- */
static uint8_t enc_ulaw(int pcm) {
    static const int end14[8] = { 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF };
    int mask, seg;
    pcm >>= 2;                                       /* 14-bit */
    if (pcm < 0) { pcm = -pcm; mask = 0x7F; } else mask = 0xFF;
    if (pcm > 8159) pcm = 8159;
    pcm += 0x84 >> 2;                                /* the bias */
    for (seg = 0; seg < 8 && pcm > end14[seg]; seg++) {}
    if (seg >= 8) return (uint8_t)(0x7F ^ mask);
    return (uint8_t)(((seg << 4) | ((pcm >> (seg + 1)) & 0xF)) ^ mask);
}
static uint8_t enc_alaw(int pcm) {
    static const int end13[8] = { 0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF };
    int mask;
    pcm >>= 3;                                       /* 13-bit */
    if (pcm >= 0) mask = 0xD5; else { mask = 0x55; pcm = -pcm - 1; }
    int seg;
    for (seg = 0; seg < 8 && pcm > end13[seg]; seg++) {}
    if (seg >= 8) return (uint8_t)(0x7F ^ mask);
    int aval = seg << 4;
    aval |= seg < 2 ? (pcm >> 1) & 0xF : (pcm >> seg) & 0xF;
    return (uint8_t)(aval ^ mask);
}

static const int ima_steps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
static const int ima_idx[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
typedef struct { int pred, index; } enc_t;
/* one sample: the 4-bit code, and the decoder's value in *rec */
static int ima_enc1(enc_t *e, int x, int *rec) {
    int step = ima_steps[e->index], diff = x - e->pred, code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    int vpdiff = step >> 3;
    if (diff >= step) { code |= 4; diff -= step; vpdiff += step; }
    step >>= 1;
    if (diff >= step) { code |= 2; diff -= step; vpdiff += step; }
    step >>= 1;
    if (diff >= step) { code |= 1; vpdiff += step; }
    e->pred += (code & 8) ? -vpdiff : vpdiff;
    if (e->pred > 32767) e->pred = 32767;
    if (e->pred < -32768) e->pred = -32768;
    e->index += ima_idx[code & 7];
    if (e->index < 0) e->index = 0;
    if (e->index > 88) e->index = 88;
    *rec = e->pred;
    return code;
}
/* encode frames (ch interleaved) into WAV IMA blocks; rec gets the decoder's expected output */
static buf_t ima_encode(const int16_t *pcm, long frames, int ch, int align, int *fpb_out, int16_t *rec) {
    buf_t d = { 0 };
    int fpb = (align - 4 * ch) * 2 / ch + 1;
    *fpb_out = fpb;
    enc_t e[8];
    for (long f0 = 0; f0 < frames; f0 += fpb) {
        size_t start = d.n;
        for (int c = 0; c < ch; c++) {
            int16_t first = pcm[f0 * ch + c];
            e[c].pred = first;
            if (f0 == 0) e[c].index = 0;
            rec[f0 * ch + c] = first;
            p16(&d, (uint16_t)first);
            uint8_t idx = (uint8_t)e[c].index, z = 0;
            put(&d, &idx, 1); put(&d, &z, 1);
        }
        for (int g = 0; g < (fpb - 1) / 8; g++) {
            for (int c = 0; c < ch; c++) {
                uint8_t bytes[4];
                for (int k = 0; k < 8; k++) {
                    long f = f0 + 1 + g * 8 + k;
                    int x = f < frames ? pcm[f * ch + c] : 0, r;
                    int code = ima_enc1(&e[c], x, &r);
                    if (f < frames) rec[f * ch + c] = (int16_t)r;
                    if (k & 1) bytes[k / 2] |= (uint8_t)(code << 4); else bytes[k / 2] = (uint8_t)code;
                }
                put(&d, bytes, 4);
            }
        }
        while (d.n - start < (size_t)align) { uint8_t z = 0; put(&d, &z, 1); }
    }
    return d;
}

/* ---------------- the tests ---------------- */
static void test_pcm(void) {
    int16_t s16[1000];
    for (int i = 0; i < 1000; i++) s16[i] = (int16_t)(20000 * tone(i, 440, 8000));
    buf_t f = mkwav(1, 1, 8000, 16, 2, NULL, 0, s16, sizeof s16);
    wav_t w;
    CHECK(wav_open(&w, f.b, f.n) == WAV_OK, "pcm16 open");
    CHECK(w.frames == 1000 && w.rate == 8000 && w.channels == 1 && w.format == WAV_PCM && wav_out_channels(&w) == 1, "pcm16 info");
    int16_t out[2000];
    CHECK(wav_read(&w, 0, out, 1000, 1) == 1000 && !memcmp(out, s16, sizeof s16), "pcm16 samples");
    CHECK(wav_read(&w, 990, out, 100, 2) == 10 && out[0] == s16[990] && out[1] == s16[990] && out[19] == s16[999], "pcm16 to stereo, end");
    CHECK(wav_read(&w, 1000, out, 10, 1) == 0 && wav_read(&w, 0, out, 10, 3) == 0, "past the end / bad channels");
    free(f.b);

    /* 8-bit unsigned stereo */
    uint8_t u8[8] = { 0, 255, 128, 128, 1, 200, 127, 129 };
    f = mkwav(1, 2, 11025, 8, 2, NULL, 0, u8, sizeof u8);
    CHECK(wav_open(&w, f.b, f.n) == WAV_OK && w.frames == 4, "pcm8 open");
    CHECK(wav_read(&w, 0, out, 4, 2) == 4 && out[0] == -32768 && out[1] == 127 * 256 && out[2] == 0 && out[4] == -127 * 256 &&
          out[5] == 72 * 256, "pcm8 values");
    CHECK(wav_read(&w, 0, out, 1, 1) == 1 && out[0] == (-32768 + 127 * 256) / 2, "pcm8 stereo to mono");
    free(f.b);

    /* 24-bit and 32-bit: the top 16 bits */
    uint8_t s24[6] = { 0x11, 0x34, 0x12, 0xFF, 0xFF, 0x80 };
    f = mkwav(1, 1, 48000, 24, 3, NULL, 0, s24, sizeof s24);
    CHECK(wav_open(&w, f.b, f.n) == WAV_OK && w.frames == 2 && w.container == 3, "pcm24 open");
    CHECK(wav_read(&w, 0, out, 2, 1) == 2 && out[0] == 0x1234 && out[1] == (int16_t)0x80FF, "pcm24 values");
    free(f.b);
    uint8_t s32[8] = { 0xAA, 0xBB, 0x78, 0x56, 0, 0, 0, 0x80 };
    f = mkwav(1, 1, 96000, 32, 4, NULL, 0, s32, sizeof s32);
    CHECK(wav_open(&w, f.b, f.n) == WAV_OK && wav_read(&w, 0, out, 2, 1) == 2 && out[0] == 0x5678 && out[1] == -32768, "pcm32 values");
    free(f.b);
    /* 20 bits in 3 bytes */
    f = mkwav(1, 1, 48000, 20, 3, NULL, 0, s24, sizeof s24);
    CHECK(wav_open(&w, f.b, f.n) == WAV_OK && w.bits == 20 && wav_read(&w, 0, out, 1, 1) == 1 && out[0] == 0x1234, "pcm 20-in-24");
    free(f.b);
}

static void test_float(void) {
    float v[6] = { 0.0f, 0.5f, -0.5f, 1.5f, -2.0f, NAN };
    buf_t f = mkwav(3, 1, 44100, 32, 4, NULL, 0, v, sizeof v);
    wav_t w;
    int16_t out[12];
    CHECK(wav_open(&w, f.b, f.n) == WAV_OK && w.format == WAV_FLOAT && w.frames == 6, "float32 open");
    CHECK(wav_read(&w, 0, out, 6, 1) == 6 && out[0] == 0 && out[1] == 16384 && out[2] == -16384 && out[3] == 32767 &&
          out[4] == -32768 && out[5] == 0, "float32 values %d %d %d %d %d %d", out[0], out[1], out[2], out[3], out[4], out[5]);
    free(f.b);
    double d[2] = { 0.25, -1.0 };
    f = mkwav(3, 2, 44100, 64, 16, NULL, 0, d, sizeof d);
    CHECK(wav_open(&w, f.b, f.n) == WAV_OK && w.frames == 1 && wav_read(&w, 0, out, 1, 2) == 1 && out[0] == 8192 && out[1] == -32768,
          "float64 stereo");
    free(f.b);
}

/* WAVE_FORMAT_EXTENSIBLE, 6 channels of 24 bits: the mix-down */
static void test_extensible(void) {
    /* cbSize 22, 24 valid bits, mask 0x3F (5.1), KSDATAFORMAT_SUBTYPE_PCM */
    uint8_t ext2[24] = { 22, 0, 24, 0, 0x3F, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71 };
    int16_t vals[6] = { 1000, -2000, 3000, 30000, 400, -600 };   /* FL FR FC LFE BL BR */
    uint8_t data[18];
    for (int c = 0; c < 6; c++) { data[c * 3] = 0x7F; data[c * 3 + 1] = (uint8_t)vals[c]; data[c * 3 + 2] = (uint8_t)(vals[c] >> 8); }
    buf_t f = mkwav(0xFFFE, 6, 48000, 24, 18, ext2, 24, data, sizeof data);
    wav_t w;
    int16_t out[2];
    CHECK(wav_open(&w, f.b, f.n) == WAV_OK && w.format == WAV_PCM && w.channels == 6 && w.channel_mask == 0x3F &&
          wav_out_channels(&w) == 2, "extensible open");
    long l = (1000L * 1000 + 3000L * 707 + 400L * 707) / (1000 + 707 + 707);
    long r = (-2000L * 1000 + 3000L * 707 - 600L * 707) / (1000 + 707 + 707);
    CHECK(wav_read(&w, 0, out, 1, 2) == 1 && out[0] == l && out[1] == r, "5.1 mix-down: %d %d (want %ld %ld)", out[0], out[1], l, r);
    free(f.b);
    ext2[23] = 0x72;                                     /* not a known subformat GUID */
    f = mkwav(0xFFFE, 6, 48000, 24, 18, ext2, 24, data, sizeof data);
    CHECK(wav_open(&w, f.b, f.n) == WAV_E_FORMAT, "unknown subformat");
    free(f.b);
}

static void test_g711(void) {
    /* known values (ITU-T G.711) */
    uint8_t u[4] = { 0xFF, 0x00, 0x80, 0x7F }, a[4] = { 0xD5, 0x55, 0xAA, 0x2A };
    buf_t f = mkwav(7, 1, 8000, 8, 1, NULL, 0, u, 4);
    wav_t w;
    int16_t out[8192];
    CHECK(wav_open(&w, f.b, f.n) == WAV_OK && wav_read(&w, 0, out, 4, 1) == 4 && out[0] == 0 && out[1] == -32124 &&
          out[2] == 32124 && out[3] == 0, "mu-law known values %d %d %d %d", out[0], out[1], out[2], out[3]);
    free(f.b);
    f = mkwav(6, 1, 8000, 8, 1, NULL, 0, a, 4);
    CHECK(wav_open(&w, f.b, f.n) == WAV_OK && wav_read(&w, 0, out, 4, 1) == 4 && out[0] == 8 && out[1] == -8 &&
          out[2] == 32256 && out[3] == -32256, "A-law known values %d %d %d %d", out[0], out[1], out[2], out[3]);
    free(f.b);
    /* every value through the reference encoder: within half a step of its segment */
    uint8_t ue[8192], ae[8192];
    int16_t src[8192];
    for (int i = 0; i < 8192; i++) { src[i] = (int16_t)(i * 8 - 32768); ue[i] = enc_ulaw(src[i]); ae[i] = enc_alaw(src[i]); }
    f = mkwav(7, 1, 8000, 8, 1, NULL, 0, ue, sizeof ue);
    int worst = 0;
    if (wav_open(&w, f.b, f.n) == WAV_OK && wav_read(&w, 0, out, 8192, 1) == 8192)
        for (int i = 0; i < 8192; i++) {
            int err = abs(out[i] - src[i]), mag = abs(src[i]);
            int allowed = mag < 256 ? 16 : mag / 16 + 8;     /* the step grows with the segment */
            if (err > allowed && err - allowed > worst) worst = err - allowed;
        }
    else worst = 99999;
    CHECK(worst == 0, "mu-law round trip off by %d", worst);
    free(f.b);
    f = mkwav(6, 1, 8000, 8, 1, NULL, 0, ae, sizeof ae);
    worst = 0;
    if (wav_open(&w, f.b, f.n) == WAV_OK && wav_read(&w, 0, out, 8192, 1) == 8192)
        for (int i = 0; i < 8192; i++) {
            int err = abs(out[i] - src[i]), mag = abs(src[i]);
            int allowed = mag < 512 ? 16 : mag / 16 + 16;
            if (err > allowed && err - allowed > worst) worst = err - allowed;
        }
    else worst = 99999;
    CHECK(worst == 0, "A-law round trip off by %d", worst);
    free(f.b);
}

static void test_ima(int ch, int align) {
    long frames = 5000;
    int16_t *pcm = malloc(sizeof(int16_t) * frames * ch), *rec = calloc((size_t)(frames + 2048) * ch, 2);
    int16_t *out = malloc(sizeof(int16_t) * frames * 2);
    for (long i = 0; i < frames; i++)
        for (int c = 0; c < ch; c++) pcm[i * ch + c] = (int16_t)(16000 * tone(i, 440 + 220 * c, 22050) + 4000 * tone(i, 3000, 22050));
    int fpb;
    buf_t d = ima_encode(pcm, frames, ch, align, &fpb, rec);
    uint8_t extra[4] = { 2, 0, (uint8_t)fpb, (uint8_t)(fpb >> 8) };
    buf_t f = mkwav(0x11, ch, 22050, 4, align, extra, 4, d.b, (uint32_t)d.n);
    wav_t w;
    int r = wav_open(&w, f.b, f.n);
    long whole = (frames + fpb - 1) / fpb * fpb;
    CHECK(r == WAV_OK && w.format == WAV_IMA_ADPCM && w.frames_per_block == fpb && (long)w.frames == whole,
          "ima %dch open: %d, %d frames/block, %llu frames", ch, r, w.frames_per_block, (unsigned long long)w.frames);
    int oc = wav_out_channels(&w);
    /* sequentially, in odd-sized pieces */
    long got = 0;
    for (long piece = 1; got < frames; piece = piece * 3 % 997 + 1) {
        long n = wav_read(&w, (uint64_t)got, out + got * oc, piece > frames - got ? frames - got : piece, oc);
        if (n <= 0) break;
        got += n;
    }
    int same = got == frames;
    double sig = 0, noise = 0;
    for (long i = 0; i < frames && same; i++) {
        int16_t want = ch == 1 ? rec[i] : rec[i * ch];
        if (out[i * oc] != want) same = 0;
        double e = (double)out[i * oc] - pcm[i * ch];
        sig += (double)pcm[i * ch] * pcm[i * ch];
        noise += e * e;
    }
    CHECK(same, "ima %dch: decoded = the encoder's reconstruction (%ld frames)", ch, got);
    double snr = 10 * log10(sig / (noise + 1));
    CHECK(snr > 25, "ima %dch SNR %.1f dB", ch, snr);
    /* random access agrees */
    int ok = 1;
    for (int t = 0; t < 200 && ok; t++) {
        uint64_t at = (uint64_t)(rand() % frames);
        int16_t o2[4];
        if (wav_read(&w, at, o2, 1, oc) != 1 || o2[0] != out[at * oc]) ok = 0;
    }
    CHECK(ok, "ima %dch random access", ch);
    /* the fact chunk shortens; a cut-off last block plays its whole groups */
    buf_t g = { 0 };
    put(&g, f.b, f.n);
    uint32_t fact = 1234;
    chunk(&g, "fact", &fact, 4);
    CHECK(wav_open(&w, g.b, g.n) == WAV_OK && w.frames == 1234, "ima fact chunk");
    free(g.b);
    CHECK(wav_open(&w, f.b, f.n - (size_t)align / 2) == WAV_OK && w.truncated && (long)w.frames > whole - fpb && (long)w.frames < whole,
          "ima cut-off block: %llu", (unsigned long long)w.frames);
    free(f.b); free(d.b); free(pcm); free(rec); free(out);
}

static void test_rejects(void) {
    wav_t w;
    int16_t s[4] = { 1, 2, 3, 4 };
    uint8_t junk[64] = "RIFX\0\0\0\0WAVE";
    CHECK(wav_open(&w, junk, sizeof junk) == WAV_E_NOTWAV, "RIFX");
    CHECK(wav_open(&w, "RIFF", 4) == WAV_E_NOTWAV && wav_open(&w, NULL, 100) == WAV_E_NOTWAV, "too short / NULL");
    memcpy(junk, "RF64", 4);
    CHECK(wav_open(&w, junk, sizeof junk) == WAV_E_FORMAT, "RF64");
    struct { int tag, ch, rate, bits, align, want; const char *what; } bad[] = {
        { 0x55, 1, 44100, 0, 1, WAV_E_FORMAT, "MP3" }, { 2, 1, 44100, 4, 256, WAV_E_FORMAT, "MS ADPCM" },
        { 1, 0, 44100, 16, 2, WAV_E_BADFMT, "0 channels" }, { 1, 33, 44100, 16, 66, WAV_E_BADFMT, "33 channels" },
        { 1, 1, 0, 16, 2, WAV_E_BADFMT, "rate 0" }, { 1, 1, 44100, 16, 0, WAV_E_BADFMT, "block align 0" },
        { 1, 1, 44100, 0, 2, WAV_E_BADFMT, "0 bits" }, { 1, 1, 44100, 24, 2, WAV_E_BADFMT, "24 bits in 2 bytes" },
        { 1, 2, 44100, 16, 3, WAV_E_BADFMT, "align not a multiple of channels" }, { 1, 1, 44100, 40, 5, WAV_E_BADFMT, "5-byte samples" },
        { 3, 1, 44100, 16, 2, WAV_E_BADFMT, "16-bit float" }, { 7, 1, 8000, 16, 2, WAV_E_BADFMT, "16-bit mu-law" },
        { 0x11, 1, 8000, 3, 256, WAV_E_BADFMT, "3-bit IMA" }, { 0x11, 2, 8000, 4, 4, WAV_E_BADFMT, "IMA block smaller than its headers" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        buf_t f = mkwav(bad[i].tag, bad[i].ch, bad[i].rate, bad[i].bits, bad[i].align, NULL, 0, s, sizeof s);
        int r = wav_open(&w, f.b, f.n);
        CHECK(r == bad[i].want, "%s: %d (want %d)", bad[i].what, r, bad[i].want);
        free(f.b);
    }
    buf_t f = mkwav(0x55, 1, 44100, 0, 1, NULL, 0, s, sizeof s);
    CHECK(wav_open(&w, f.b, f.n) == WAV_E_FORMAT && !strcmp(wav_tag_name(w.tag), "MP3"), "MP3 named");
    free(f.b);
    f = mkwav(1, 1, 44100, 16, 2, NULL, 0, s, 0);
    CHECK(wav_open(&w, f.b, f.n) == WAV_E_EMPTY, "no samples");
    free(f.b);
    /* a short fmt chunk; no data chunk; no fmt chunk */
    buf_t g = { 0 };
    put(&g, "RIFF\0\0\0\0WAVE", 12);
    chunk(&g, "fmt ", "\1\0\1\0", 4);
    chunk(&g, "data", s, 8);
    CHECK(wav_open(&w, g.b, g.n) == WAV_E_BADFMT, "fmt chunk of 4 bytes");
    free(g.b);
    f = mkwav(1, 1, 44100, 16, 2, NULL, 0, s, sizeof s);
    CHECK(wav_open(&w, f.b, 12 + 8 + 16) == WAV_E_TRUNCATED, "no data chunk");
    memcpy(f.b + 12, "junk", 4);
    CHECK(wav_open(&w, f.b, f.n) == WAV_E_TRUNCATED, "no fmt chunk");
    free(f.b);
    for (int e = 0; e >= -6; e--) CHECK(wav_error(e) && *wav_error(e), "message for %d", e);
}

/* chunks before data with odd sizes; a data chunk longer than the file */
static void test_layout(void) {
    int16_t s[100];
    for (int i = 0; i < 100; i++) s[i] = (int16_t)(i * 300 - 15000);
    buf_t f = { 0 };
    put(&f, "RIFF\0\0\0\0WAVE", 12);
    chunk(&f, "LIST", "INFOISFT\3\0\0\0ab", 15);           /* odd: padded */
    uint8_t fmt[16] = { 1, 0, 1, 0, 0x44, 0xAC, 0, 0, 0x88, 0x58, 1, 0, 2, 0, 16, 0 };
    chunk(&f, "fmt ", fmt, 16);
    chunk(&f, "data", s, sizeof s);
    wav_t w;
    int16_t out[100];
    CHECK(wav_open(&w, f.b, f.n) == WAV_OK && w.frames == 100 && wav_read(&w, 0, out, 100, 1) == 100 && !memcmp(out, s, sizeof s),
          "odd LIST chunk before fmt");
    /* cut the file: the data chunk claims more than there is */
    CHECK(wav_open(&w, f.b, f.n - 41) == WAV_OK && w.truncated && w.frames == 79, "truncated: %llu frames", (unsigned long long)w.frames);
    CHECK(wav_open(&w, f.b, f.n - 200) == WAV_E_EMPTY, "truncated to nothing");
    free(f.b);
}

/* ---------------- fuzzing ---------------- */
static uint32_t rng = 12345;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void fuzz(long iters) {
    /* seeds: one of each format */
    buf_t seeds[8];
    int ns = 0;
    int16_t s16[600];
    for (int i = 0; i < 600; i++) s16[i] = (int16_t)(12000 * tone(i, 500, 8000));
    seeds[ns++] = mkwav(1, 2, 8000, 16, 4, NULL, 0, s16, sizeof s16);
    seeds[ns++] = mkwav(1, 1, 8000, 8, 1, NULL, 0, s16, 300);
    seeds[ns++] = mkwav(1, 3, 8000, 24, 9, NULL, 0, s16, 900);
    seeds[ns++] = mkwav(3, 1, 8000, 32, 4, NULL, 0, s16, 400);
    seeds[ns++] = mkwav(7, 2, 8000, 8, 2, NULL, 0, s16, 600);
    seeds[ns++] = mkwav(6, 1, 8000, 8, 1, NULL, 0, s16, 600);
    int fpb;
    int16_t *rec = calloc(4096, 2);
    buf_t d = ima_encode(s16, 600, 1, 64, &fpb, rec);
    uint8_t extra[4] = { 2, 0, (uint8_t)fpb, 0 };
    seeds[ns++] = mkwav(0x11, 1, 8000, 4, 64, extra, 4, d.b, (uint32_t)d.n);
    free(d.b);
    d = ima_encode(s16, 300, 2, 128, &fpb, rec);
    extra[2] = (uint8_t)fpb;
    seeds[ns++] = mkwav(0x11, 2, 8000, 4, 128, extra, 4, d.b, (uint32_t)d.n);
    free(d.b);
    free(rec);
    long opened = 0, frames_read = 0;
    uint8_t *b = malloc(8192);
    int16_t out[2 * 512];
    for (long it = 0; it < iters; it++) {
        buf_t *sd = &seeds[rnd() % (uint32_t)ns];
        size_t n = sd->n;
        memcpy(b, sd->b, n);
        int muts = 1 + (int)(rnd() % 8);
        for (int m = 0; m < muts; m++) {
            switch (rnd() % 6) {
            case 0: if (n) b[rnd() % n] = (uint8_t)rnd(); break;              /* a byte */
            case 1: if (n) b[rnd() % n] ^= (uint8_t)(1u << (rnd() % 8)); break;   /* a bit */
            case 2: { size_t at = 12 + rnd() % 40; if (at + 4 <= n) { uint32_t v = rnd() % 3 ? rnd() % 0x20000 : rnd(); memcpy(b + at, &v, 4); } break; }
            case 3: n = rnd() % (n + 1); break;                                /* cut */
            case 4: { size_t at = 20 + rnd() % 16; if (at + 2 <= n) { uint16_t v = (uint16_t)(rnd() % 3 ? rnd() % 70 : rnd()); memcpy(b + at, &v, 2); } break; }
            case 5: if (n >= 36 && n + 16 < 8192) { memmove(b + 36 + 16, b + 36, n - 36); memcpy(b + 36, "junk\x07\0\0\0abcdefgh", 16); n += 16; } break;
            }
        }
        /* an exact-size copy so ASan sees any read past the end */
        uint8_t *exact = malloc(n ? n : 1);
        memcpy(exact, b, n);
        wav_t w;
        if (wav_open(&w, exact, n) == WAV_OK) {
            opened++;
            if (w.frames == 0 || w.data < exact || w.data + w.data_len > exact + n) { fails++; printf("FAIL fuzz: bad data range\n"); }
            int oc = 1 + (int)(rnd() % 2);
            for (int k = 0; k < 4; k++) {
                uint64_t at = rnd() % (w.frames + 2);
                long got = wav_read(&w, at, out, 1 + (long)(rnd() % 512), oc);
                if (got < 0 || (at < w.frames && got == 0) || (uint64_t)got > w.frames - (at < w.frames ? at : w.frames)) {
                    fails++;
                    printf("FAIL fuzz: read at %llu of %llu gave %ld\n", (unsigned long long)at, (unsigned long long)w.frames, got);
                }
                frames_read += got;
            }
        }
        free(exact);
    }
    checks++;
    printf("fuzz: %ld files, %ld parsed, %ld frames decoded\n", iters, opened, frames_read);
    for (int i = 0; i < ns; i++) free(seeds[i].b);
    free(b);
}

static void check_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { checks++; fails++; printf("FAIL: cannot open %s\n", path); return; }
    static uint8_t buf[4 << 20];
    size_t n = fread(buf, 1, sizeof buf, fp);
    fclose(fp);
    wav_t w;
    int r = wav_open(&w, buf, n);
    /* the system sounds: 16-bit PCM, short; the demo piece: IMA ADPCM made by mksounds.py's own encoder */
    int adpcm = strstr(path, "/music/") != NULL;
    CHECK(r == WAV_OK && w.format == (adpcm ? WAV_IMA_ADPCM : WAV_PCM) && w.bits == (adpcm ? 4 : 16) && w.rate == 22050 &&
          !w.truncated && w.frames >= 22050 / 10 && w.frames <= 22050 * (adpcm ? 14 : 6), "%s: %s, %d Hz, %d bits, %llu frames",
          path, wav_error(r), w.rate, w.bits, (unsigned long long)w.frames);
    /* not clipped, not silent; and smooth (a broken encoder or decoder gives noise: large sample-to-sample steps) */
    int16_t out[2 * 4096];
    int peak = 0, prev[2] = { 0, 0 };
    double sig = 0, step = 0;
    for (uint64_t at = 0; at < w.frames;) {
        long got = wav_read(&w, at, out, 4096, 2);
        if (got <= 0) break;
        for (long i = 0; i < got * 2; i++) {
            if (abs(out[i]) > peak) peak = abs(out[i]);
            double d = out[i] - prev[i & 1];
            sig += (double)out[i] * out[i];
            step += d * d;
            prev[i & 1] = out[i];
        }
        at += (uint64_t)got;
    }
    CHECK(peak > 8000 && peak < 32000 && step < 0.5 * sig, "%s: peak %d, step/signal energy %.2f", path, peak, step / (sig + 1));
}

int main(int argc, char **argv) {
    long iters = 200000;
    int a = 1;
    if (argc > 2 && !strcmp(argv[1], "-n")) { iters = atol(argv[2]); a = 3; }
    test_pcm();
    test_float();
    test_extensible();
    test_g711();
    test_ima(1, 256);
    test_ima(2, 512);
    test_ima(1, 36);
    test_rejects();
    test_layout();
    for (; a < argc; a++) check_file(argv[a]);
    fuzz(iters);
    printf("wav: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
