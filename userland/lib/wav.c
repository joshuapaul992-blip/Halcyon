/* Halcyon WAV library: see wav.h */
#include "wav.h"

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static int same4(const uint8_t *p, const char *s) { return p[0] == (uint8_t)s[0] && p[1] == (uint8_t)s[1] && p[2] == (uint8_t)s[2] && p[3] == (uint8_t)s[3]; }

/* the KSDATAFORMAT_SUBTYPE_* GUIDs end like this (the format tag is first) */
static const uint8_t guid_tail[14] = { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };

const char *wav_error(int err) {
    switch (err) {
    case WAV_OK: return "no error";
    case WAV_E_NOTWAV: return "it is not a WAV file";
    case WAV_E_TRUNCATED: return "the file is damaged or incomplete";
    case WAV_E_FORMAT: return "its sound is compressed in a format that cannot be played";
    case WAV_E_BADFMT: return "its format information is invalid";
    case WAV_E_EMPTY: return "it holds no sound";
    }
    return "unknown error";
}

const char *wav_tag_name(int tag) {
    switch (tag) {
    case 0x0001: return "PCM";
    case 0x0002: return "Microsoft ADPCM";
    case 0x0003: return "IEEE float";
    case 0x0006: return "A-law";
    case 0x0007: return "mu-law";
    case 0x0011: return "IMA ADPCM";
    case 0x0022: return "TrueSpeech";
    case 0x0031: return "GSM 6.10";
    case 0x0040: return "G.721 ADPCM";
    case 0x0050: return "MPEG audio";
    case 0x0055: return "MP3";
    case 0x0092: return "Dolby AC-3 (S/PDIF)";
    case 0x0160: case 0x0161: case 0x0162: case 0x0163: return "Windows Media Audio";
    case 0x00FF: case 0x1600: case 0x1601: return "AAC";
    case 0x2000: return "Dolby AC-3";
    case 0x2001: return "DTS";
    case 0xF1AC: return "FLAC";
    case 0x6750: case 0x674F: return "Vorbis";
    }
    return "an unknown compression";
}

const char *wav_format_name(const wav_t *w) {
    switch (w->format) {
    case WAV_PCM: return "PCM";
    case WAV_FLOAT: return "IEEE float";
    case WAV_ALAW: return "A-law";
    case WAV_MULAW: return "mu-law";
    case WAV_IMA_ADPCM: return "IMA ADPCM";
    }
    return "?";
}

int wav_out_channels(const wav_t *w) { return w->channels == 1 ? 1 : 2; }

int wav_stream_rate(const wav_t *w, int min, int max, int *rep, int *dec) {
    *rep = *dec = 1;
    if (w->rate <= 0 || min <= 0 || max < min) return 0;
    if (w->rate < min) {
        *rep = (min + w->rate - 1) / w->rate;
        return (long)w->rate * *rep <= max ? w->rate * *rep : 0;
    }
    if (w->rate > max) {
        *dec = (w->rate + max - 1) / max;
        return w->rate / *dec >= min ? w->rate / *dec : 0;
    }
    return w->rate;
}

int wav_open(wav_t *w, const void *buf, size_t size) {
    const uint8_t *b = buf;
    for (size_t i = 0; i < sizeof *w; i++) ((uint8_t *)w)[i] = 0;
    if (!b || size < 12) return WAV_E_NOTWAV;
    if (same4(b, "RF64") && same4(b + 8, "WAVE")) return WAV_E_FORMAT;
    if (!same4(b, "RIFF") || !same4(b + 8, "WAVE")) return WAV_E_NOTWAV;
    const uint8_t *fmt = NULL, *data = NULL;
    uint32_t fmt_len = 0;
    uint64_t data_len = 0, fact = 0;
    int have_fact = 0;
    size_t pos = 12;
    /* the chunks, in any order; the RIFF size is not trusted (often wrong) */
    while (pos + 8 <= size) {
        uint32_t len = rd32(b + pos + 4);
        size_t avail = size - pos - 8;
        const uint8_t *c = b + pos + 8;
        if (same4(b + pos, "fmt ") && !fmt) {
            if (len > avail) return WAV_E_TRUNCATED;
            fmt = c;
            fmt_len = len;
        } else if (same4(b + pos, "fact") && len >= 4 && len <= avail) {
            fact = rd32(c);
            have_fact = 1;
        } else if (same4(b + pos, "data") && !data) {
            data = c;
            data_len = len;
            if (len > avail) { data_len = avail; w->truncated = 1; }
        }
        if (len > avail) break;
        pos += 8 + (size_t)len + (len & 1);
    }
    if (!fmt || !data) return WAV_E_TRUNCATED;
    if (fmt_len < 16) return WAV_E_BADFMT;
    int tag = rd16(fmt);
    w->tag = tag;
    w->channels = rd16(fmt + 2);
    uint32_t rate = rd32(fmt + 4);
    w->block_align = rd16(fmt + 12);
    w->bits = rd16(fmt + 14);
    int fmtcode = tag;
    if (tag == WAV_EXTENSIBLE) {
        if (fmt_len < 40 || rd16(fmt + 16) < 22) return WAV_E_BADFMT;
        int valid = rd16(fmt + 18);
        w->channel_mask = rd32(fmt + 20);
        for (int i = 0; i < 14; i++) if (fmt[26 + i] != guid_tail[i]) return WAV_E_FORMAT;
        fmtcode = rd16(fmt + 24);
        if (valid && valid <= w->bits) w->bits = valid;
    }
    if (fmtcode != WAV_PCM && fmtcode != WAV_FLOAT && fmtcode != WAV_ALAW && fmtcode != WAV_MULAW && fmtcode != WAV_IMA_ADPCM) {
        if (tag == WAV_EXTENSIBLE) w->tag = fmtcode;
        return WAV_E_FORMAT;
    }
    w->format = fmtcode;
    if (w->channels < 1 || w->channels > 32 || rate < 100 || rate > 1000000 || w->block_align < 1) return WAV_E_BADFMT;
    w->rate = (int)rate;
    int ch = w->channels;
    if (fmtcode == WAV_IMA_ADPCM) {
        /* blocks: per channel a 4-byte header (first sample, step index),
         * then groups of 4 bytes (8 samples) per channel in turn */
        if (w->bits != 4 || w->block_align < 8 * ch || w->block_align % (4 * ch)) return WAV_E_BADFMT;
        int fpb = (w->block_align - 4 * ch) * 2 / ch + 1;
        if (fmt_len >= 20 && rd16(fmt + 16) >= 2) {
            int stated = rd16(fmt + 18);
            if (stated >= 1 && stated < fpb) fpb = stated;   /* (larger is impossible) */
        }
        w->frames_per_block = fpb;
        w->container = 0;
        uint64_t whole = data_len / (uint64_t)w->block_align;
        uint64_t rest = data_len % (uint64_t)w->block_align;
        uint64_t frames = whole * (uint64_t)fpb;
        if (rest >= (uint64_t)(4 * ch)) {                 /* a cut-off last block: its whole groups */
            uint64_t part = (rest - 4 * (uint64_t)ch) / (4 * (uint64_t)ch) * 8 + 1;
            frames += part < (uint64_t)fpb ? part : (uint64_t)fpb;
        }
        if (have_fact && fact < frames) frames = fact;
        w->frames = frames;
    } else {
        if (w->block_align % ch) return WAV_E_BADFMT;
        int cont = w->block_align / ch;
        w->container = cont;
        if (fmtcode == WAV_PCM && (cont < 1 || cont > 4 || w->bits < 1 || w->bits > cont * 8)) return WAV_E_BADFMT;
        if (fmtcode == WAV_FLOAT && cont != 4 && cont != 8) return WAV_E_BADFMT;
        if ((fmtcode == WAV_ALAW || fmtcode == WAV_MULAW) && cont != 1) return WAV_E_BADFMT;
        w->frames_per_block = 1;
        w->frames = data_len / (uint64_t)w->block_align;
    }
    w->data = data;
    w->data_len = data_len;
    if (!w->frames) return WAV_E_EMPTY;
    return WAV_OK;
}

/* ---------------- sample decoders: one sample to a signed 16-bit value ---------------- */
static int16_t ulaw(uint8_t u) {
    u = (uint8_t)~u;
    int t = ((u & 0x0F) << 3) + 0x84;
    t <<= (u & 0x70) >> 4;
    return (int16_t)((u & 0x80) ? 0x84 - t : t - 0x84);
}

static int16_t alaw(uint8_t a) {
    a ^= 0x55;
    int t = (a & 0x0F) << 4;
    int seg = (a & 0x70) >> 4;
    if (seg == 0) t += 8;
    else if (seg == 1) t += 0x108;
    else { t += 0x108; t <<= seg - 1; }
    return (int16_t)((a & 0x80) ? t : -t);
}

static int16_t from_float(double v) {
    if (!(v == v)) return 0;                    /* NaN */
    if (v >= 1.0) return 32767;
    if (v <= -1.0) return -32768;
    double s = v * 32768.0;
    long r = (long)(s < 0 ? s - 0.5 : s + 0.5);
    if (r > 32767) r = 32767;
    return (int16_t)r;
}

static int16_t sample_at(const wav_t *w, const uint8_t *p) {
    switch (w->format) {
    case WAV_PCM:
        switch (w->container) {
        case 1: return (int16_t)((p[0] - 128) * 256);
        case 2: return (int16_t)rd16(p);
        case 3: return (int16_t)(p[1] | p[2] << 8);
        default: return (int16_t)(p[2] | p[3] << 8);
        }
    case WAV_FLOAT:
        if (w->container == 4) {
            union { uint32_t u; float f; } x;
            x.u = rd32(p);
            return from_float(x.f);
        } else {
            union { uint64_t u; double d; } x;
            x.u = rd32(p) | (uint64_t)rd32(p + 4) << 32;
            return from_float(x.d);
        }
    case WAV_MULAW: return ulaw(p[0]);
    case WAV_ALAW: return alaw(p[0]);
    }
    return 0;
}

/* ---------------- IMA ADPCM ---------------- */
static const int16_t ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
static const int8_t ima_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

typedef struct { int pred, index; } ima_t;

static int16_t ima_nibble(ima_t *s, int n) {
    int step = ima_step[s->index];
    int diff = step >> 3;
    if (n & 1) diff += step >> 2;
    if (n & 2) diff += step >> 1;
    if (n & 4) diff += step;
    s->pred += (n & 8) ? -diff : diff;
    if (s->pred > 32767) s->pred = 32767;
    if (s->pred < -32768) s->pred = -32768;
    s->index += ima_index[n & 15];
    if (s->index < 0) s->index = 0;
    if (s->index > 88) s->index = 88;
    return (int16_t)s->pred;
}

#define MAXCH 32

/* the frames [from, from + n) of one block (n <= frames_per_block - from)
 * into tmp, ch samples per frame. Returns the frames decoded. */
static long ima_block(const wav_t *w, uint64_t block, int from, long n, int16_t *tmp) {
    int ch = w->channels;
    uint64_t off = block * (uint64_t)w->block_align;
    if (off + 4 * (uint64_t)ch > w->data_len) return 0;
    const uint8_t *b = w->data + off;
    uint64_t avail = w->data_len - off;
    if (avail > (uint64_t)w->block_align) avail = (uint64_t)w->block_align;
    ima_t st[MAXCH];
    for (int c = 0; c < ch; c++) {
        st[c].pred = (int16_t)rd16(b + 4 * c);
        st[c].index = b[4 * c + 2] > 88 ? 88 : b[4 * c + 2];
    }
    long done = 0;
    int f = 0;                                         /* the frame within the block */
    if (from == 0 && n > 0) {
        for (int c = 0; c < ch; c++) tmp[c] = (int16_t)st[c].pred;
        done = 1;
    }
    f = 1;
    /* groups of 8 frames: 4 bytes per channel in turn */
    for (uint64_t g = 0; done < n; g++) {
        uint64_t go = 4 * (uint64_t)ch + g * 4 * (uint64_t)ch;
        if (go + 4 * (uint64_t)ch > avail) break;
        int16_t grp[8][MAXCH];
        for (int c = 0; c < ch; c++) {
            const uint8_t *q = b + go + 4 * (uint64_t)c;
            for (int k = 0; k < 4; k++) {
                grp[2 * k][c] = ima_nibble(&st[c], q[k] & 15);
                grp[2 * k + 1][c] = ima_nibble(&st[c], q[k] >> 4);
            }
        }
        for (int k = 0; k < 8 && done < n; k++, f++) {
            if (f >= w->frames_per_block) return done;
            if (f < from) continue;
            for (int c = 0; c < ch; c++) tmp[done * ch + c] = grp[k][c];
            done++;
        }
    }
    return done;
}

/* ---------------- frames out ---------------- */
/* mix one frame of ch channels to out_ch */
static void mixdown(const wav_t *w, const int16_t *in, int16_t *out, int out_ch) {
    int ch = w->channels;
    if (ch == 1) {
        out[0] = in[0];
        if (out_ch == 2) out[1] = in[0];
        return;
    }
    long l, r;
    if (ch == 2) {
        l = in[0];
        r = in[1];
    } else {
        /* front L, R, centre, LFE (dropped), back L, R, side L, R; more: alternately */
        long wl = 0, wr = 0, sl = 0, sr = 0;   /* weights in 1/1000 */
        for (int c = 0; c < ch; c++) {
            int a = 0, b = 0;
            switch (c) {
            case 0: a = 1000; break;
            case 1: b = 1000; break;
            case 2: a = b = 707; break;
            case 3: break;
            default: if (c % 2 == 0) a = 707; else b = 707; break;
            }
            sl += a * (long)in[c]; wl += a;
            sr += b * (long)in[c]; wr += b;
        }
        l = sl / wl;
        r = sr / wr;
    }
    if (out_ch == 1) out[0] = (int16_t)((l + r) / 2);
    else { out[0] = (int16_t)l; out[1] = (int16_t)r; }
}

long wav_read(const wav_t *w, uint64_t first, int16_t *out, long n, int out_ch) {
    if (!w || !w->data || n <= 0 || first >= w->frames || (out_ch != 1 && out_ch != 2)) return 0;
    if ((uint64_t)n > w->frames - first) n = (long)(w->frames - first);
    int ch = w->channels;
    long done = 0;
    if (w->format == WAV_IMA_ADPCM) {
        int16_t tmp[64 * MAXCH];
        while (done < n) {
            uint64_t f = first + (uint64_t)done;
            uint64_t block = f / (uint64_t)w->frames_per_block;
            int from = (int)(f % (uint64_t)w->frames_per_block);
            long want = n - done;
            if (want > 64) want = 64;
            if (want > w->frames_per_block - from) want = w->frames_per_block - from;
            long got = ima_block(w, block, from, want, tmp);
            if (got <= 0) break;
            for (long i = 0; i < got; i++) mixdown(w, tmp + i * ch, out + (done + i) * out_ch, out_ch);
            done += got;
        }
        return done;
    }
    int16_t frame[MAXCH];
    for (; done < n; done++) {
        const uint8_t *p = w->data + (first + (uint64_t)done) * (uint64_t)w->block_align;
        for (int c = 0; c < ch; c++) frame[c] = sample_at(w, p + c * w->container);
        mixdown(w, frame, out + done * out_ch, out_ch);
    }
    return done;
}
