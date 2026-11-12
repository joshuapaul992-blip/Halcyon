/* Host tests for the HD Audio codec layer (kernel/sound/drivers/hda_codec.c,
 * compiled unmodified; ASan + UBSan): widget graph parsing, output path
 * search, amplifier and pin programming, volume, jack sensing.
 *
 * The codecs are synthetic: a table of widgets (capabilities, pin
 * capabilities, configuration defaults, amplifier capabilities, connection
 * lists) answered verb by verb by a small codec model that also records
 * every control the driver sets. The layouts follow published codec
 * descriptions and typical BIOS pin configurations:
 *   ALC892  desktop: DACs 02-05 (+25), mixers 0c-0f (+26) each fed by a DAC
 *           and the analog loopback mixer 0b, rear line out 14 / surround
 *           15 16 17, front headphones 1b in the same association with
 *           sequence 15 (headphone redirection), S/PDIF 06 -> 1e, inputs
 *  ALC269   laptop: DACs 02 03, mixers 0c 0d, internal speaker 14, headphone
 *           jack 15 (unsolicited responses), digital mic, mic jack
 *  ALC1220  desktop with the front headphones in their own association
 *           (no redirection), the loopback mixer's list as a range
 *  Intel HDMI/DP (Haswell style): converters 02-04, pins 05-07, one monitor
 *           on DisplayPort
 *  NVIDIA HDMI/DP (GeForce style): pins 04-07, converters 08-0b
 *  QEMU's hda-duplex and hda-micro (as they answer in QEMU 8.2)
 * and constructed ones: a BIOS that left every pin "no connection", a
 * shared selector, long-form connection lists, loops and dangling
 * connections, a codec without an audio function group, small amplifiers.
 *
 * usage: hdatest [-v]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "core/lib.h"
#include "sound/drivers/hda.h"

static int g_fail, g_checks, g_verbose;
static char g_log[65536];
static int g_loglen;

#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; \
    printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define EXPECT(got, want) do { long _g = (long)(got), _w = (long)(want); \
    CHECK(_g == _w, "%s = %ld (%#lx), want %ld (%#lx)", #got, _g, _g, _w, _w); } while (0)
#define EXPECT_STR(got, want) do { const char *_g = (got), *_w = (want); \
    CHECK(!strcmp(_g, _w), "got \"%s\", want \"%s\"", _g, _w); } while (0)

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(g_log + g_loglen, sizeof g_log - g_loglen, fmt, ap);
    va_end(ap);
    if (g_verbose) fputs(g_log + g_loglen, stdout);
    if (n > 0 && g_loglen + n < (int)sizeof g_log - 1) g_loglen += n;
}

/* ---------------- the codec model ---------------- */
#define MAXW 64
typedef struct mw {
    int nid;
    u32 caps, pincap, cfg, ampin, ampout;
    int nconn;
    u16 conn[40];       /* raw entries (with range bits) */
    int lng;            /* long form */
    u32 sense;          /* pin sense */
    u32 eld5;           /* ELD byte 5 response */
} mw_t;

typedef struct model {
    const char *name;
    u32 vendor, subsystem;
    int fg_type;        /* 1 audio, 2 modem */
    int unsol;
    u32 afg_ampin, afg_ampout, afg_pcm;
    int wstart, nw;
    mw_t w[MAXW];
} model_t;

/* what the driver set */
typedef struct state {
    model_t *m;
    int pinctl[128], connsel[128], eapd[128], power[128], stream[128], fmt[128], unsol[128], digi[128];
    int chcount[128], dipxmit[128], dipn[128];
    u8 dip[128][32];
    int amp_out[128];               /* last value written (gain | mute << 7) */
    int amp_in[128][16];
    int above_0db;                  /* an amp was set above 0 dB */
    int verbs, bad_nid;
    int fail_after;                 /* verbs fail (no response) after this many (0: never) */
} state_t;

static mw_t *mfind(model_t *m, int nid) {
    for (int i = 0; i < m->nw; i++) if (m->w[i].nid == nid) return &m->w[i];
    return NULL;
}

static u32 conn_len(mw_t *w) { return (u32)w->nconn | (w->lng ? 0x80 : 0); }

static int model_cmd(void *ctx, u32 v, u32 *resp) {
    state_t *s = ctx;
    model_t *m = s->m;
    int nid = (int)(v >> 20) & 0x7F, top = (int)(v >> 16) & 0xF;
    u32 verb, pl;
    if (top == 7 || top == 0xF) { verb = (v >> 8) & 0xFFF; pl = v & 0xFF; }
    else { verb = (u32)top; pl = v & 0xFFFF; }
    s->verbs++;
    if (s->fail_after && s->verbs > s->fail_after) return E_TIMEOUT;
    *resp = 0;
    mw_t *w = mfind(m, nid);
    int afg = nid == 1;
    if (!w && !afg && nid != 0) { s->bad_nid++; return 0; }
    switch (verb) {
    case HDA_GET_PARAM:
        if (nid == 0) {
            if (pl == HDA_PAR_VENDOR_ID) *resp = m->vendor;
            else if (pl == HDA_PAR_REVISION_ID) *resp = 0x00100101;
            else if (pl == HDA_PAR_NODE_COUNT) *resp = 1 << 16 | 1;
        } else if (afg) {
            if (pl == HDA_PAR_FG_TYPE) *resp = (u32)m->fg_type | (m->unsol ? 0x100 : 0);
            else if (pl == HDA_PAR_NODE_COUNT) *resp = (u32)m->wstart << 16 | (u32)m->nw;
            else if (pl == HDA_PAR_AMP_IN_CAPS) *resp = m->afg_ampin;
            else if (pl == HDA_PAR_AMP_OUT_CAPS) *resp = m->afg_ampout;
            else if (pl == HDA_PAR_PCM) *resp = m->afg_pcm;
        } else {
            switch (pl) {
            case HDA_PAR_WIDGET_CAPS: *resp = w->caps; break;
            case HDA_PAR_PIN_CAPS: *resp = w->pincap; break;
            case HDA_PAR_AMP_IN_CAPS: *resp = w->ampin; break;
            case HDA_PAR_AMP_OUT_CAPS: *resp = w->ampout; break;
            case HDA_PAR_CONN_LEN: *resp = conn_len(w); break;
            case HDA_PAR_PCM: *resp = 0x000E07F0; break;
            }
        }
        return 0;
    case HDA_GET_CONN_LIST: {
        if (!w) return 0;
        int per = w->lng ? 2 : 4, bits = w->lng ? 16 : 8;
        int base = (int)pl / per * per;
        for (int i = 0; i < per && base + i < w->nconn; i++) *resp |= (u32)w->conn[base + i] << (i * bits);
        return 0;
    }
    case HDA_GET_CONFIG_DEFAULT: if (w) *resp = w->cfg; return 0;
    case HDA_GET_SUBSYSTEM_ID: *resp = m->subsystem; return 0;
    case HDA_GET_PIN_SENSE: if (w) *resp = w->sense; return 0;
    case HDA_GET_HDMI_ELDD: if (w && pl == 5) *resp = w->eld5; return 0;
    case HDA_GET_EAPD_BTL: *resp = (u32)(s->eapd[nid] < 0 ? 0 : s->eapd[nid]); return 0;
    case HDA_GET_POWER_STATE: { int p = s->power[nid] < 0 ? 0 : s->power[nid]; *resp = (u32)(p << 4 | p); return 0; }
    case HDA_SET_POWER_STATE: s->power[nid] = (int)pl; return 0;
    case HDA_SET_PIN_CTL: s->pinctl[nid] = (int)pl; return 0;
    case HDA_SET_CONN_SELECT: s->connsel[nid] = (int)pl; return 0;
    case HDA_SET_EAPD_BTL: s->eapd[nid] = (int)pl; return 0;
    case HDA_SET_STREAM_CHANNEL: s->stream[nid] = (int)pl; return 0;
    case HDA_SET_FORMAT: s->fmt[nid] = (int)pl; return 0;
    case HDA_SET_UNSOL: s->unsol[nid] = (int)pl; return 0;
    case HDA_SET_DIGI_CONV1: s->digi[nid] = (int)pl; return 0;
    case HDA_SET_CVT_CHAN_COUNT: s->chcount[nid] = (int)pl; return 0;
    case HDA_SET_HDMI_DIP_INDEX: s->dipn[nid] = 0; return 0;
    case HDA_SET_HDMI_DIP_DATA: if (s->dipn[nid] < 32) s->dip[nid][s->dipn[nid]++] = (u8)pl; return 0;
    case HDA_SET_HDMI_DIP_XMIT: s->dipxmit[nid] = (int)pl; return 0;
    case HDA_EXEC_PIN_SENSE: return 0;
    case HDA_SET_AMP: {
        if (!w) return 0;
        int out = (pl & HDA_AMP_OUT) != 0, idx = (int)(pl >> 8) & 0xF, gain = (int)(pl & 0x7F);
        u32 caps = out ? w->ampout : w->ampin;
        if (!(w->caps & HDA_WCAP_AMP_OVRD)) caps = out ? m->afg_ampout : m->afg_ampin;
        int g0 = MIN((int)HDA_AMPCAP_OFFSET(caps), (int)HDA_AMPCAP_STEPS(caps));
        if (gain > g0) s->above_0db++;
        int val = gain | ((pl & HDA_AMP_MUTE) ? 0x80 : 0);
        if (out) s->amp_out[nid] = val; else s->amp_in[nid][idx] = val;
        return 0;
    }
    }
    return 0;
}

static void state_init(state_t *s, model_t *m) {
    memset(s, 0, sizeof *s);
    s->m = m;
    for (int i = 0; i < 128; i++) {
        s->pinctl[i] = s->connsel[i] = s->eapd[i] = s->power[i] = s->stream[i] = s->fmt[i] = -1;
        s->unsol[i] = s->digi[i] = s->chcount[i] = s->dipxmit[i] = s->amp_out[i] = -1;
        for (int j = 0; j < 16; j++) s->amp_in[i][j] = -1;
    }
}

static hda_codec_t *codec_new(state_t *s, model_t *m) {
    state_init(s, m);
    hda_codec_t *c = calloc(1, sizeof *c);
    c->cad = 0;
    c->cmd = model_cmd;
    c->ctx = s;
    c->log = "hdatest";
    return c;
}

/* "02 0c 14" for a path */
static const char *path_str(const hda_codec_t *c, int i) {
    static char buf[64];
    int n = 0;
    buf[0] = 0;
    if (i >= c->npaths) return "(none)";
    for (int k = 0; k < c->path[i].len; k++) n += snprintf(buf + n, sizeof buf - n, "%s%02x", k ? " " : "", c->path[i].nid[k]);
    return buf;
}

static void dump(hda_codec_t *c) { if (g_verbose) hda_codec_dump(c); }

/* ---------------- widget helpers ---------------- */
#define DAC(n, amp) { .nid = n, .caps = 0x00041D, .ampout = amp }
#define SPDIF_DAC(n) { .nid = n, .caps = 0x000611 }
#define MIXER2(n, a, b) { .nid = n, .caps = 0x20010B, .ampin = 0x80000000, .nconn = 2, .conn = { a, b } }
#define REALTEK_DAC_AMP 0x00034040u     /* ofs 0x40, 64 steps of 1 dB, no mute */
#define ALC269_DAC_AMP  0x00025757u     /* ofs 0x57, 87 steps of 0.75 dB */
#define PIN_OUT_AMP     0x80000000u     /* mute only */

static model_t alc892 = {
    .name = "ALC892", .vendor = 0x10EC0892, .subsystem = 0x1458A002, .fg_type = 1, .unsol = 1,
    .afg_pcm = 0x000E07F0, .wstart = 2, .nw = 37,
    .w = {
        DAC(0x02, REALTEK_DAC_AMP), DAC(0x03, REALTEK_DAC_AMP), DAC(0x04, REALTEK_DAC_AMP), DAC(0x05, REALTEK_DAC_AMP),
        SPDIF_DAC(0x06),
        { .nid = 0x07, .caps = 0x10011B, .ampin = 0x80051F17, .nconn = 1, .conn = { 0x24 } },
        { .nid = 0x08, .caps = 0x10011B, .ampin = 0x80051F17, .nconn = 1, .conn = { 0x23 } },
        { .nid = 0x09, .caps = 0x10011B, .ampin = 0x80051F17, .nconn = 1, .conn = { 0x22 } },
        { .nid = 0x0A, .caps = 0x100391, .nconn = 1, .conn = { 0x1F } },           /* S/PDIF in */
        { .nid = 0x0B, .caps = 0x20010B, .ampin = 0x80051F17, .nconn = 10,
          .conn = { 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x14, 0x15, 0x16, 0x17 } },
        MIXER2(0x0C, 0x02, 0x0B), MIXER2(0x0D, 0x03, 0x0B), MIXER2(0x0E, 0x04, 0x0B), MIXER2(0x0F, 0x05, 0x0B),
        { .nid = 0x10 }, { .nid = 0x11 }, { .nid = 0x12 }, { .nid = 0x13 },      /* absent (caps 0) */
        { .nid = 0x14, .caps = 0x40058D, .pincap = 0x0001003E, .cfg = 0x01014010, .ampout = PIN_OUT_AMP, .nconn = 5,
          .conn = { 0x0C, 0x0D, 0x0E, 0x0F, 0x26 } },
        { .nid = 0x15, .caps = 0x40058D, .pincap = 0x0001003E, .cfg = 0x01011012, .ampout = PIN_OUT_AMP, .nconn = 5,
          .conn = { 0x0C, 0x0D, 0x0E, 0x0F, 0x26 } },
        { .nid = 0x16, .caps = 0x40058D, .pincap = 0x0001003E, .cfg = 0x01016011, .ampout = PIN_OUT_AMP, .nconn = 5,
          .conn = { 0x0C, 0x0D, 0x0E, 0x0F, 0x26 } },
        { .nid = 0x17, .caps = 0x40058D, .pincap = 0x0001003E, .cfg = 0x01012014, .ampout = PIN_OUT_AMP, .nconn = 5,
          .conn = { 0x0C, 0x0D, 0x0E, 0x0F, 0x26 } },
        { .nid = 0x18, .caps = 0x40058F, .pincap = 0x0000373E, .cfg = 0x01A19030, .ampin = 0x00270300, .ampout = PIN_OUT_AMP,
          .nconn = 5, .conn = { 0x0C, 0x0D, 0x0E, 0x0F, 0x26 } },
        { .nid = 0x19, .caps = 0x40058F, .pincap = 0x0000373E, .cfg = 0x02A19040, .ampin = 0x00270300, .ampout = PIN_OUT_AMP,
          .nconn = 5, .conn = { 0x0C, 0x0D, 0x0E, 0x0F, 0x26 } },
        { .nid = 0x1A, .caps = 0x40058F, .pincap = 0x0000373E, .cfg = 0x0181304F, .ampin = 0x00270300, .ampout = PIN_OUT_AMP,
          .nconn = 5, .conn = { 0x0C, 0x0D, 0x0E, 0x0F, 0x26 } },
        { .nid = 0x1B, .caps = 0x40058F, .pincap = 0x0001373E, .cfg = 0x0221401F, .ampin = 0x00270300, .ampout = PIN_OUT_AMP,
          .nconn = 5, .conn = { 0x0C, 0x0D, 0x0E, 0x0F, 0x26 } },
        { .nid = 0x1C, .caps = 0x400001, .pincap = 0x00000020, .cfg = 0x411111F0 },
        { .nid = 0x1D, .caps = 0x400000, .pincap = 0x00000020, .cfg = 0x4005E601 },
        { .nid = 0x1E, .caps = 0x400300, .pincap = 0x00000010, .cfg = 0x01441130, .nconn = 1, .conn = { 0x06 } },
        { .nid = 0x1F, .caps = 0x400200, .pincap = 0x00000020, .cfg = 0x411111F0 },
        { .nid = 0x20, .caps = 0xF00040 }, { .nid = 0x21 },
        { .nid = 0x22, .caps = 0x30010B, .ampin = 0x80000000, .nconn = 11,
          .conn = { 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x14, 0x15, 0x16, 0x17, 0x0B } },
        { .nid = 0x23, .caps = 0x30010B, .ampin = 0x80000000, .nconn = 11,
          .conn = { 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x14, 0x15, 0x16, 0x17, 0x0B } },
        { .nid = 0x24, .caps = 0x30010B, .ampin = 0x80000000, .nconn = 11,
          .conn = { 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x14, 0x15, 0x16, 0x17, 0x0B } },
        DAC(0x25, REALTEK_DAC_AMP), MIXER2(0x26, 0x25, 0x0B),
    },
};

static model_t alc269 = {
    .name = "ALC269", .vendor = 0x10EC0269, .subsystem = 0x17AA21F6, .fg_type = 1, .unsol = 1,
    .afg_pcm = 0x000E07F0, .wstart = 2, .nw = 0x1E,
    .w = {
        DAC(0x02, ALC269_DAC_AMP), DAC(0x03, ALC269_DAC_AMP), { .nid = 0x04 }, { .nid = 0x05 }, { .nid = 0x06 },
        { .nid = 0x07, .caps = 0x10051B, .ampin = 0x80051F17, .nconn = 1, .conn = { 0x24 } },
        { .nid = 0x08, .caps = 0x10051B, .ampin = 0x80051F17, .nconn = 1, .conn = { 0x23 } },
        { .nid = 0x09 }, { .nid = 0x0A },
        { .nid = 0x0B, .caps = 0x20010B, .ampin = 0x80051F17, .nconn = 5, .conn = { 0x18, 0x19, 0x1A, 0x1B, 0x1D } },
        MIXER2(0x0C, 0x02, 0x0B), MIXER2(0x0D, 0x03, 0x0B),
        { .nid = 0x0E }, { .nid = 0x0F, .caps = 0x20010A, .nconn = 2, .conn = { 0x02, 0x0B } },
        { .nid = 0x10 }, { .nid = 0x11 },
        { .nid = 0x12, .caps = 0x40040B, .pincap = 0x00000020, .cfg = 0x90A60130, .ampin = 0x00270300 },
        { .nid = 0x13 },
        { .nid = 0x14, .caps = 0x40058D, .pincap = 0x00010014, .cfg = 0x90170110, .ampout = PIN_OUT_AMP, .nconn = 2,
          .conn = { 0x0C, 0x0D } },
        { .nid = 0x15, .caps = 0x40058D, .pincap = 0x0001001C, .cfg = 0x0221101F, .ampout = PIN_OUT_AMP, .nconn = 2,
          .conn = { 0x0C, 0x0D } },
        { .nid = 0x16 },
        { .nid = 0x17, .caps = 0x40050C, .pincap = 0x00000010, .cfg = 0x411111F0, .ampout = PIN_OUT_AMP, .nconn = 1,
          .conn = { 0x0F } },
        { .nid = 0x18, .caps = 0x40058F, .pincap = 0x00003724, .cfg = 0x02A11030, .ampin = 0x00270300, .ampout = PIN_OUT_AMP,
          .nconn = 1, .conn = { 0x0D } },
        { .nid = 0x19, .caps = 0x40048B, .pincap = 0x00003724, .cfg = 0x99A3092F, .ampin = 0x00270300 },
        { .nid = 0x1A, .caps = 0x40058F, .pincap = 0x0000373C, .cfg = 0x411111F0, .ampin = 0x00270300, .ampout = PIN_OUT_AMP,
          .nconn = 1, .conn = { 0x0C } },
        { .nid = 0x1B, .caps = 0x40058F, .pincap = 0x0000373C, .cfg = 0x411111F0, .ampin = 0x00270300, .ampout = PIN_OUT_AMP,
          .nconn = 2, .conn = { 0x0C, 0x0D } },
        { .nid = 0x1C }, { .nid = 0x1D, .caps = 0x400400, .pincap = 0x00000020, .cfg = 0x40079A2D },
        { .nid = 0x1E, .caps = 0x400781, .pincap = 0x00000014, .cfg = 0x411111F0, .nconn = 1, .conn = { 0x06 } },
        { .nid = 0x1F },
    },
};

static model_t alc1220 = {
    .name = "ALC1220", .vendor = 0x10EC1220, .subsystem = 0x1043872E, .fg_type = 1, .unsol = 1,
    .afg_pcm = 0x000E07F0, .wstart = 2, .nw = 37,
    .w = {
        DAC(0x02, REALTEK_DAC_AMP), DAC(0x03, REALTEK_DAC_AMP), DAC(0x04, REALTEK_DAC_AMP), DAC(0x05, REALTEK_DAC_AMP),
        SPDIF_DAC(0x06),
        { .nid = 0x07, .caps = 0x10011B, .ampin = 0x80051F17, .nconn = 1, .conn = { 0x24 } },
        { .nid = 0x08, .caps = 0x10011B, .ampin = 0x80051F17, .nconn = 1, .conn = { 0x23 } },
        { .nid = 0x09, .caps = 0x10011B, .ampin = 0x80051F17, .nconn = 1, .conn = { 0x22 } },
        { .nid = 0x0A },
        /* the loopback mixer's list with a range: 18, 19 - 1d (encoded 0x18, 0x9d), 14 - 17 */
        { .nid = 0x0B, .caps = 0x20010B, .ampin = 0x80051F17, .nconn = 4, .conn = { 0x18, 0x9D, 0x14, 0x97 } },
        MIXER2(0x0C, 0x02, 0x0B), MIXER2(0x0D, 0x03, 0x0B), MIXER2(0x0E, 0x04, 0x0B), MIXER2(0x0F, 0x05, 0x0B),
        { .nid = 0x10 }, { .nid = 0x11 }, { .nid = 0x12 }, { .nid = 0x13 },
        { .nid = 0x14, .caps = 0x40058D, .pincap = 0x0001003E, .cfg = 0x01014010, .ampout = PIN_OUT_AMP, .nconn = 5,
          .conn = { 0x0C, 0x0D, 0x0E, 0x0F, 0x26 } },
        { .nid = 0x15, .caps = 0x40058D, .pincap = 0x0001003E, .cfg = 0x01011012, .ampout = PIN_OUT_AMP, .nconn = 5,
          .conn = { 0x0C, 0x0D, 0x0E, 0x0F, 0x26 } },
        { .nid = 0x16, .caps = 0x40058D, .pincap = 0x0001003E, .cfg = 0x01016011, .ampout = PIN_OUT_AMP, .nconn = 5,
          .conn = { 0x0C, 0x0D, 0x0E, 0x0F, 0x26 } },
        { .nid = 0x17, .caps = 0x40058D, .pincap = 0x0001003E, .cfg = 0x01012014, .ampout = PIN_OUT_AMP, .nconn = 5,
          .conn = { 0x0C, 0x0D, 0x0E, 0x0F, 0x26 } },
        { .nid = 0x18, .caps = 0x40058F, .pincap = 0x0000373E, .cfg = 0x01A19030, .ampin = 0x00270300, .ampout = PIN_OUT_AMP,
          .nconn = 5, .conn = { 0x0C, 0x0D, 0x0E, 0x0F, 0x26 } },
        { .nid = 0x19, .caps = 0x40058F, .pincap = 0x0000373E, .cfg = 0x02A19040, .ampin = 0x00270300, .ampout = PIN_OUT_AMP,
          .nconn = 5, .conn = { 0x0C, 0x0D, 0x0E, 0x0F, 0x26 } },
        { .nid = 0x1A, .caps = 0x40058F, .pincap = 0x0000373E, .cfg = 0x0181303F, .ampin = 0x00270300, .ampout = PIN_OUT_AMP,
          .nconn = 5, .conn = { 0x0C, 0x0D, 0x0E, 0x0F, 0x26 } },
        { .nid = 0x1B, .caps = 0x40058F, .pincap = 0x0001373E, .cfg = 0x02214020, .ampin = 0x00270300, .ampout = PIN_OUT_AMP,
          .nconn = 5, .conn = { 0x0C, 0x0D, 0x0E, 0x0F, 0x26 } },
        { .nid = 0x1C }, { .nid = 0x1D, .caps = 0x400000, .pincap = 0x00000020, .cfg = 0x4026C629 },
        { .nid = 0x1E, .caps = 0x400300, .pincap = 0x00000010, .cfg = 0x01451120, .nconn = 1, .conn = { 0x06 } },
        { .nid = 0x1F }, { .nid = 0x20, .caps = 0xF00040 }, { .nid = 0x21 },
        { .nid = 0x22, .caps = 0x30010B, .ampin = 0x80000000, .nconn = 2, .conn = { 0x18, 0x0B } },
        { .nid = 0x23, .caps = 0x30010B, .ampin = 0x80000000, .nconn = 2, .conn = { 0x19, 0x0B } },
        { .nid = 0x24, .caps = 0x30010B, .ampin = 0x80000000, .nconn = 2, .conn = { 0x1A, 0x0B } },
        DAC(0x25, REALTEK_DAC_AMP), MIXER2(0x26, 0x25, 0x0B),
    },
};

#define HDMI_CVT(n) { .nid = n, .caps = 0x00006211 }
#define HDMI_PIN(n, cfg_) { .nid = n, .caps = 0x0040778D, .pincap = 0x09000094, .cfg = cfg_, .ampout = PIN_OUT_AMP, \
                           .nconn = 3, .conn = { 0x02, 0x03, 0x04 } }
static model_t intel_hdmi = {
    .name = "Intel HDMI", .vendor = 0x80862807, .subsystem = 0x80860101, .fg_type = 1, .unsol = 1,
    .afg_pcm = 0x001E07F0, .wstart = 2, .nw = 7,
    .w = { HDMI_CVT(0x02), HDMI_CVT(0x03), HDMI_CVT(0x04),
           HDMI_PIN(0x05, 0x18560010), HDMI_PIN(0x06, 0x18560020), HDMI_PIN(0x07, 0x18560030),
           { .nid = 0x08, .caps = 0xF00000 } },
};

/* NVIDIA GPU style: four HDMI/DP pins 04-07 and converters 08-0b, plus
 * (constructed) an S/PDIF pin and a fifth HDMI pin */
#define NV_PIN(n) { .nid = n, .caps = 0x0040778D, .pincap = 0x09000094, .cfg = 0x185600F0, .ampout = PIN_OUT_AMP, \
                    .nconn = 4, .conn = { 0x08, 0x09, 0x0A, 0x0B } }
static model_t nv_hdmi = {
    .name = "NVIDIA HDMI", .vendor = 0x10DE0083, .subsystem = 0x10DE119E, .fg_type = 1, .unsol = 1,
    .afg_pcm = 0x001E07F0, .wstart = 4, .nw = 10,
    .w = { NV_PIN(0x04), NV_PIN(0x05), NV_PIN(0x06), NV_PIN(0x07),
           HDMI_CVT(0x08), HDMI_CVT(0x09), HDMI_CVT(0x0A), HDMI_CVT(0x0B),
           { .nid = 0x0C, .caps = 0x400300, .pincap = 0x00000010, .cfg = 0x01441130, .nconn = 1, .conn = { 0x08 } },
           NV_PIN(0x0D) },
};

/* QEMU 8.2's codecs, as hda-codec.c describes them (mixer on: an amp on the DAC) */
static model_t qemu_duplex = {
    .name = "QEMU hda-duplex", .vendor = 0x1AF40022, .subsystem = 0x1AF40022, .fg_type = 1,
    .afg_pcm = 0x000E07F0, .wstart = 2, .nw = 4,
    .w = {
        { .nid = 0x02, .caps = 0x0000001D, .ampout = 0x80034A4A },
        { .nid = 0x03, .caps = 0x00400101, .pincap = 0x00000010, .cfg = 0x00004010, .nconn = 1, .conn = { 0x02 } },
        { .nid = 0x04, .caps = 0x0010011B, .ampin = 0x80034A4A, .nconn = 1, .conn = { 0x05 } },
        { .nid = 0x05, .caps = 0x00400001, .pincap = 0x00000020, .cfg = 0x00805020 },
    },
};
static model_t qemu_micro = {
    .name = "QEMU hda-micro", .vendor = 0x1AF40032, .subsystem = 0x1AF40032, .fg_type = 1,
    .afg_pcm = 0x000E07F0, .wstart = 2, .nw = 4,
    .w = {
        { .nid = 0x02, .caps = 0x0000001D, .ampout = 0x80034A4A },
        { .nid = 0x03, .caps = 0x00400101, .pincap = 0x00000010, .cfg = 0x00104010, .nconn = 1, .conn = { 0x02 } },
        { .nid = 0x04, .caps = 0x0010011B, .ampin = 0x80034A4A, .nconn = 1, .conn = { 0x05 } },
        { .nid = 0x05, .caps = 0x00400001, .pincap = 0x00000020, .cfg = 0x00A05020 },
    },
};

/* ---------------- the tests ---------------- */
static void test_alc892(void) {
    printf("ALC892 desktop: line out + front headphones (redirection)\n");
    state_t s;
    hda_codec_t *c = codec_new(&s, &alc892);
    EXPECT(hda_codec_probe(c), 0);
    EXPECT_STR(c->name, "Realtek ALC892");
    EXPECT(c->nw, 37);
    EXPECT(c->w[0x14 - 2].nconn, 5);
    EXPECT(c->w[0x0B - 2].nconn, 10);
    EXPECT(hda_codec_find_paths(c, 0), 2);
    dump(c);
    EXPECT(c->path[0].kind, HDA_OUT_LINE);
    EXPECT_STR(path_str(c, 0), "02 0c 14");
    EXPECT(c->path[1].kind, HDA_OUT_HP);
    EXPECT_STR(path_str(c, 1), "03 0d 1b");
    EXPECT(c->path[1].idx[2], 1);           /* 0d is entry 1 of 1b's list */
    EXPECT(c->digital, 0);
    EXPECT(c->hw_volume, 1);
    EXPECT(c->path[0].vol_nid, 0x02);
    EXPECT(c->path[0].mute_nid, 0x14);      /* the last amp that can mute: the pin's */
    /* rear speakers plugged in, headphones not */
    alc892.w[0x14 - 2].sense = HDA_SENSE_PRESENCE;
    alc892.w[0x1B - 2].sense = 0;
    EXPECT(hda_codec_setup(c, HDA_FMT_48K_16_2), 0);
    EXPECT(s.pinctl[0x14], HDA_PINCTL_OUT);
    EXPECT(s.pinctl[0x1B], HDA_PINCTL_OUT | HDA_PINCTL_HP);
    for (int n = 0x15; n <= 0x17; n++) EXPECT(s.pinctl[n], -1);  /* surround pins left alone */
    EXPECT(s.pinctl[0x1E], -1);             /* S/PDIF: not with analog outputs */
    EXPECT(s.pinctl[0x18], -1);             /* mic jack */
    EXPECT(s.connsel[0x14], 0);
    EXPECT(s.connsel[0x1B], 1);
    EXPECT(s.amp_in[0x0C][0], 0x00);        /* the DAC: unmuted, 0 dB (mute-only amp) */
    EXPECT(s.amp_in[0x0C][1], 0x80);        /* the loopback: muted */
    EXPECT(s.amp_in[0x0D][0], 0x00);
    EXPECT(s.amp_in[0x0D][1], 0x80);
    EXPECT(s.amp_out[0x02], 0x40);          /* 0 dB */
    EXPECT(s.amp_out[0x14], 0x00);          /* unmuted */
    EXPECT(s.eapd[0x14] & HDA_EAPD, HDA_EAPD);
    EXPECT(s.eapd[0x1B] & HDA_EAPD, HDA_EAPD);
    EXPECT(s.fmt[0x02], HDA_FMT_48K_16_2);
    EXPECT(s.fmt[0x03], HDA_FMT_48K_16_2);
    EXPECT(s.power[1], 0);
    EXPECT(s.unsol[0x14], 0x81);
    EXPECT(s.unsol[0x1B], 0x82);
    EXPECT(s.above_0db, 0);
    char out[32];
    hda_codec_describe(c, out, sizeof out);
    EXPECT_STR(out, "Line out");
    /* stream binding */
    hda_codec_stream(c, 1, HDA_FMT_48K_16_2);
    EXPECT(s.stream[0x02], 0x10);
    EXPECT(s.stream[0x03], 0x10);
    hda_codec_stream(c, 0, HDA_FMT_48K_16_2);
    EXPECT(s.stream[0x02], 0);
    /* headphones in: redirection mutes the line out of the same association */
    alc892.w[0x1B - 2].sense = HDA_SENSE_PRESENCE;
    EXPECT(hda_codec_unsol(c, 2u << 26), 1);
    EXPECT(c->hp_plugged, 1);
    EXPECT(s.pinctl[0x14], 0);
    EXPECT(s.pinctl[0x1B], HDA_PINCTL_OUT | HDA_PINCTL_HP);
    hda_codec_describe(c, out, sizeof out);
    EXPECT_STR(out, "Headphones");
    EXPECT(hda_codec_unsol(c, 9u << 26), 0);    /* not one of ours */
    EXPECT(hda_codec_jacks(c), 0);              /* nothing changed */
    alc892.w[0x1B - 2].sense = 0;
    EXPECT(hda_codec_jacks(c), 1);
    EXPECT(s.pinctl[0x14], HDA_PINCTL_OUT);
    hda_codec_describe(c, out, sizeof out);
    EXPECT_STR(out, "Line out");
    /* nothing plugged in anywhere: name everything that plays */
    alc892.w[0x14 - 2].sense = 0;
    hda_codec_jacks(c);
    hda_codec_describe(c, out, sizeof out);
    EXPECT_STR(out, "Line out, Headphones");
    /* volume: on the DAC amps, -40 log10(p) dB in 1 dB steps; mute through the pin */
    hda_codec_set_volume(c, 100, 0);
    EXPECT(s.amp_out[0x02], 0x40);
    EXPECT(s.amp_out[0x03], 0x40);
    hda_codec_set_volume(c, 50, 0);
    EXPECT(s.amp_out[0x02], 0x40 - 12);
    hda_codec_set_volume(c, 10, 0);
    EXPECT(s.amp_out[0x02], 0x40 - 40);
    hda_codec_set_volume(c, 1, 0);
    EXPECT(s.amp_out[0x02], 0);             /* -80 dB: the bottom of the range */
    hda_codec_set_volume(c, 80, 1);
    EXPECT(s.amp_out[0x14], 0x80);
    EXPECT(s.amp_out[0x1B], 0x80);
    hda_codec_set_volume(c, 80, 0);
    EXPECT(s.amp_out[0x14], 0x00);
    EXPECT(s.amp_out[0x02], 0x40 - 4);      /* -3.9 dB */
    EXPECT(s.above_0db, 0);
    EXPECT(s.bad_nid, 0);
    free(c);
}

static void test_alc269(void) {
    printf("ALC269 laptop: speakers + headphone jack\n");
    state_t s;
    hda_codec_t *c = codec_new(&s, &alc269);
    alc269.w[0x15 - 2].sense = 0;
    EXPECT(hda_codec_probe(c), 0);
    EXPECT_STR(c->name, "Realtek ALC269");
    EXPECT(hda_codec_find_paths(c, 1), 2);
    dump(c);
    EXPECT(c->path[0].kind, HDA_OUT_SPEAKER);
    EXPECT_STR(path_str(c, 0), "02 0c 14");
    EXPECT(c->path[1].kind, HDA_OUT_HP);
    EXPECT_STR(path_str(c, 1), "03 0d 15");
    EXPECT(c->hw_volume, 1);
    EXPECT(hda_codec_setup(c, HDA_FMT_48K_16_2), 0);
    EXPECT(c->path[0].detect, 0);           /* fixed: no jack */
    EXPECT(c->path[1].detect, 1);
    EXPECT(c->path[1].unsol, 1);
    EXPECT(s.pinctl[0x14], HDA_PINCTL_OUT);
    EXPECT(s.pinctl[0x15], HDA_PINCTL_OUT | HDA_PINCTL_HP);
    EXPECT(s.pinctl[0x17], -1);             /* mono out: no connection */
    EXPECT(s.pinctl[0x1B], -1);
    EXPECT(s.connsel[0x14], 0);
    EXPECT(s.connsel[0x15], 1);
    EXPECT(s.eapd[0x14] & HDA_EAPD, HDA_EAPD);
    EXPECT(s.eapd[0x15] & HDA_EAPD, HDA_EAPD);
    EXPECT(s.amp_out[0x02], 0x57);
    char out[32];
    hda_codec_describe(c, out, sizeof out);
    EXPECT_STR(out, "Speakers");
    alc269.w[0x15 - 2].sense = HDA_SENSE_PRESENCE;
    EXPECT(hda_codec_unsol(c, 2u << 26 | 0x123), 1);
    EXPECT(s.pinctl[0x14], 0);              /* speakers off */
    hda_codec_describe(c, out, sizeof out);
    EXPECT_STR(out, "Headphones");
    alc269.w[0x15 - 2].sense = 0;
    EXPECT(hda_codec_unsol(c, 2u << 26), 1);
    EXPECT(s.pinctl[0x14], HDA_PINCTL_OUT);
    hda_codec_set_volume(c, 50, 0);
    EXPECT(s.amp_out[0x02], 0x57 - 16);     /* 12.04 dB / 0.75 */
    EXPECT(s.above_0db, 0);
    free(c);

    printf("ALC269 laptop, BIOS without a pin configuration\n");
    model_t bare = alc269;
    for (int i = 0; i < bare.nw; i++) if (bare.w[i].cfg) bare.w[i].cfg = 0x411111F0;
    c = codec_new(&s, &bare);
    g_loglen = 0;
    EXPECT(hda_codec_probe(c), 0);
    int n = hda_codec_find_paths(c, 0);
    CHECK(n >= 2, "%d paths", n);
    CHECK(strstr(g_log, "no usable pin configuration") != NULL, "not logged");
    int hp = 0, line = 0;
    for (int i = 0; i < n; i++) {
        hp += c->path[i].kind == HDA_OUT_HP;
        line += c->path[i].kind == HDA_OUT_LINE;
        CHECK(c->path[i].nid[c->path[i].len - 1] != 0x1E, "S/PDIF pin used");
    }
    CHECK(hp >= 1 && line >= 1, "hp %d line %d", hp, line);
    EXPECT(hda_codec_setup(c, HDA_FMT_48K_16_2), 0);
    EXPECT(s.pinctl[0x14], HDA_PINCTL_OUT);
    EXPECT(s.pinctl[0x15], HDA_PINCTL_OUT | HDA_PINCTL_HP);
    free(c);
}

static void test_alc1220(void) {
    printf("ALC1220 desktop: headphones in their own association\n");
    state_t s;
    hda_codec_t *c = codec_new(&s, &alc1220);
    alc1220.w[0x14 - 2].sense = HDA_SENSE_PRESENCE;
    alc1220.w[0x1B - 2].sense = HDA_SENSE_PRESENCE;
    EXPECT(hda_codec_probe(c), 0);
    EXPECT_STR(c->name, "Realtek ALC1220");
    /* the range: 18, 19..1d, 14, 15..17 */
    EXPECT(c->w[0x0B - 2].nconn, 10);
    EXPECT(c->w[0x0B - 2].conn[5], 0x1D);
    EXPECT(c->w[0x0B - 2].conn[9], 0x17);
    EXPECT(hda_codec_find_paths(c, 1), 2);
    dump(c);
    EXPECT_STR(path_str(c, 0), "02 0c 14");
    EXPECT_STR(path_str(c, 1), "03 0d 1b");
    EXPECT(c->digital, 0);
    EXPECT(hda_codec_setup(c, HDA_FMT_48K_16_2), 0);
    EXPECT(c->hp_plugged, 1);
    EXPECT(s.pinctl[0x14], HDA_PINCTL_OUT);     /* not muted: another association */
    EXPECT(s.pinctl[0x1E], -1);
    EXPECT(s.digi[0x06], -1);
    char out[32];
    hda_codec_describe(c, out, sizeof out);
    EXPECT_STR(out, "Line out, Headphones");
    EXPECT(s.above_0db, 0);
    free(c);
}

static void test_hdmi(void) {
    printf("Intel HDMI/DP codec\n");
    state_t s;
    hda_codec_t *c = codec_new(&s, &intel_hdmi);
    intel_hdmi.w[0x06 - 2].sense = HDA_SENSE_PRESENCE | HDA_SENSE_ELDV;
    intel_hdmi.w[0x06 - 2].eld5 = 0x80000000 | 1 << 2 | 1 << 4;   /* DisplayPort, one SAD */
    EXPECT(hda_codec_probe(c), 0);
    EXPECT_STR(c->name, "Intel HDMI/DP");
    EXPECT(hda_codec_find_paths(c, 0), 0);      /* no analog output */
    EXPECT(hda_codec_find_paths(c, 1), 1);      /* the pin with the monitor */
    dump(c);
    EXPECT(c->digital, 1);
    EXPECT(c->hw_volume, 0);
    EXPECT(c->path[0].kind, HDA_OUT_HDMI);
    EXPECT_STR(path_str(c, 0), "02 06");
    EXPECT(hda_codec_setup(c, HDA_FMT_48K_16_2), 0);
    EXPECT(c->path[0].detect, 0);               /* digital: presence is not a jack */
    EXPECT(s.pinctl[0x06], HDA_PINCTL_OUT);
    EXPECT(s.connsel[0x06], 0);
    EXPECT(s.digi[0x02], 1);
    EXPECT(s.chcount[0x02], 1);
    EXPECT(s.dipxmit[0x06], 0xC0);
    EXPECT(s.dipn[0x06], 8);                    /* DisplayPort audio InfoFrame SDP */
    EXPECT(s.dip[0x06][0], 0x84);
    EXPECT(s.dip[0x06][1], 0x1B);
    EXPECT(s.dip[0x06][3], 0x01);
    EXPECT(s.pinctl[0x05], -1);
    char out[32];
    hda_codec_describe(c, out, sizeof out);
    EXPECT_STR(out, "HDMI");
    free(c);

    printf("Intel HDMI/DP codec, no monitor; HDMI InfoFrame\n");
    intel_hdmi.w[0x06 - 2].sense = 0;
    c = codec_new(&s, &intel_hdmi);
    EXPECT(hda_codec_probe(c), 0);
    EXPECT(hda_codec_find_paths(c, 1), 3);      /* every pin, each its own converter */
    EXPECT_STR(path_str(c, 0), "02 05");
    EXPECT_STR(path_str(c, 1), "03 06");
    EXPECT_STR(path_str(c, 2), "04 07");
    EXPECT(hda_codec_setup(c, HDA_FMT_48K_16_2), 0);
    EXPECT(s.connsel[0x07], 2);
    EXPECT(s.dipn[0x05], 14);                   /* HDMI: header, checksum, 10 bytes */
    u8 sum = 0;
    for (int i = 0; i < 14; i++) sum = (u8)(sum + s.dip[0x05][i]);
    EXPECT(sum, 0);
    EXPECT(s.dip[0x05][4], 0x01);
    free(c);
}

static void test_nvidia(void) {
    printf("NVIDIA HDMI/DP codec: at most four pins, HDMI before S/PDIF\n");
    state_t s;
    hda_codec_t *c = codec_new(&s, &nv_hdmi);
    EXPECT(hda_codec_probe(c), 0);
    EXPECT_STR(c->name, "NVIDIA HDMI/DP");
    EXPECT(hda_codec_find_paths(c, 0), 0);
    EXPECT(hda_codec_find_paths(c, 1), 4);
    dump(c);
    EXPECT_STR(path_str(c, 0), "08 04");
    EXPECT_STR(path_str(c, 1), "09 05");
    EXPECT_STR(path_str(c, 2), "0a 06");
    EXPECT_STR(path_str(c, 3), "0b 07");
    EXPECT(hda_codec_setup(c, HDA_FMT_48K_16_2), 0);
    EXPECT(s.pinctl[0x0C], -1);
    EXPECT(s.pinctl[0x0D], -1);
    hda_codec_stream(c, 1, HDA_FMT_48K_16_2);
    for (int n = 0x08; n <= 0x0B; n++) EXPECT(s.stream[n], 0x10);
    free(c);
}

static void test_qemu(void) {
    printf("QEMU hda-duplex / hda-micro\n");
    state_t s;
    hda_codec_t *c = codec_new(&s, &qemu_duplex);
    EXPECT(hda_codec_probe(c), 0);
    EXPECT_STR(c->name, "QEMU hda-duplex");
    EXPECT(hda_codec_find_paths(c, 1), 1);
    EXPECT_STR(path_str(c, 0), "02 03");
    EXPECT(c->path[0].kind, HDA_OUT_LINE);
    EXPECT(c->hw_volume, 1);
    EXPECT(hda_codec_setup(c, HDA_FMT_48K_16_2), 0);
    EXPECT(s.amp_out[0x02], 0x4A);
    EXPECT(s.pinctl[0x03], HDA_PINCTL_OUT);
    EXPECT(s.pinctl[0x05], -1);
    hda_codec_set_volume(c, 80, 0);
    EXPECT(s.amp_out[0x02], 0x4A - 4);
    hda_codec_set_volume(c, 80, 1);
    EXPECT(s.amp_out[0x02], 0x80);              /* the DAC amp mutes itself */
    hda_codec_set_volume(c, 0, 0);
    EXPECT(s.amp_out[0x02], 0x80);
    free(c);
    c = codec_new(&s, &qemu_micro);
    EXPECT(hda_codec_probe(c), 0);
    EXPECT(hda_codec_find_paths(c, 1), 1);
    EXPECT(c->path[0].kind, HDA_OUT_SPEAKER);
    char out[32];
    EXPECT(hda_codec_setup(c, HDA_FMT_48K_16_2), 0);
    hda_codec_describe(c, out, sizeof out);
    EXPECT_STR(out, "Speakers");
    free(c);
}

static void test_constructed(void) {
    printf("constructed: shared selector, long form, loops, small amps, no AFG, dead codec\n");
    state_t s;
    /* two pins behind one selector with two DACs: the second path may not
     * switch the selector away from the first; it shares the first DAC */
    model_t sel = {
        .name = "selector", .vendor = 0x11112222, .fg_type = 1, .wstart = 2, .nw = 5,
        .afg_ampout = 0x00051F1F,               /* the AFG's amp caps: 31 steps of 1.5 dB */
        .w = {
            { .nid = 0x02, .caps = 0x000005 },  /* stereo, out amp (the AFG's caps) */
            { .nid = 0x03, .caps = 0x000005 },
            { .nid = 0x04, .caps = 0x300101, .nconn = 2, .conn = { 0x02, 0x03 } },
            { .nid = 0x05, .caps = 0x400101, .pincap = 0x10, .cfg = 0x01014010, .nconn = 1, .conn = { 0x04 } },
            { .nid = 0x06, .caps = 0x400101, .pincap = 0x18, .cfg = 0x0221401F, .nconn = 1, .conn = { 0x04 } },
        },
    };
    hda_codec_t *c = codec_new(&s, &sel);
    EXPECT(hda_codec_probe(c), 0);
    EXPECT(c->w[0].ampout, 0x00051F1F);
    EXPECT(hda_codec_find_paths(c, 0), 2);
    EXPECT_STR(path_str(c, 0), "02 04 05");
    EXPECT_STR(path_str(c, 1), "02 04 06");
    EXPECT(c->hw_volume, 0);                    /* 46.5 dB, but nothing on the path can mute */
    EXPECT(hda_codec_setup(c, HDA_FMT_48K_16_2), 0);
    EXPECT(s.connsel[0x04], 0);
    EXPECT(s.amp_out[0x02], 0x1F);
    EXPECT(s.above_0db, 0);
    free(c);

    /* long-form connection lists, a loop between two mixers, dangling and
     * zero entries, a list longer than the driver keeps */
    model_t loop = {
        .name = "loop", .vendor = 0x33334444, .fg_type = 1, .wstart = 2, .nw = 5,
        .w = {
            { .nid = 0x02, .caps = 0x00000D, .ampout = 0x00017F7F },        /* 127 steps of 0.5 dB, 0 dB at the top */
            { .nid = 0x03, .caps = 0x20010B, .ampin = 0x80000000, .nconn = 2, .lng = 1, .conn = { 0x04, 0x7F } },
            { .nid = 0x04, .caps = 0x20010B, .ampin = 0x80000000, .nconn = 4, .lng = 1, .conn = { 0x03, 0x0000, 0x0002, 0x0004 } },
            { .nid = 0x05, .caps = 0x400101, .pincap = 0x10, .cfg = 0x01014010, .nconn = 40,
              .conn = { 0x7E, 0x7D, 0x00, 0x03 } },
            { .nid = 0x06, .caps = 0x400101, .pincap = 0x10, .cfg = 0x01014020, .nconn = 2, .lng = 1,
              .conn = { 0x03, 0x8006 } },       /* long form with a range: 03, 04 05 06 (itself) */
        },
    };
    c = codec_new(&s, &loop);
    EXPECT(hda_codec_probe(c), 0);
    EXPECT(c->w[0x05 - 2].nconn, HDA_MAX_CONN);
    EXPECT(c->w[0x04 - 2].nconn, 4);
    EXPECT(c->w[0x04 - 2].conn[2], 0x02);
    EXPECT(c->w[0x06 - 2].nconn, 4);
    EXPECT(c->w[0x06 - 2].conn[3], 0x06);
    EXPECT(hda_codec_find_paths(c, 0), 2);
    EXPECT_STR(path_str(c, 0), "02 04 03 05");
    EXPECT_STR(path_str(c, 1), "02 04 06");       /* shortest: 04 is in its list too */
    EXPECT(c->path[0].idx[1], 2);
    EXPECT(c->path[0].idx[2], 0);
    EXPECT(c->path[0].idx[3], 3);
    EXPECT(hda_codec_setup(c, HDA_FMT_48K_16_2), 0);
    EXPECT(s.connsel[0x05], 3);
    EXPECT(s.amp_in[0x04][2], 0);
    EXPECT(s.amp_in[0x04][0], 0x80);
    EXPECT(s.amp_in[0x03][0], 0);
    EXPECT(s.amp_in[0x03][1], 0x80);
    EXPECT(s.amp_out[0x02], 0x7F);
    EXPECT(hda_volume_gain(0x00017F7F, 50), 0x7F - 24);
    EXPECT(s.above_0db, 0);
    free(c);

    /* an amp whose 0 dB point is above its range: the top is used, never more */
    EXPECT(hda_volume_gain(0x00032040, 100), 0x20);
    EXPECT(hda_volume_gain(0x00032040, 0), 0);
    /* too little range for the volume: the mixer does it, the amp stays at 0 dB */
    model_t small = qemu_duplex;
    small.w[0].ampout = 0x80031010;              /* 16 dB */
    c = codec_new(&s, &small);
    EXPECT(hda_codec_probe(c), 0);
    EXPECT(hda_codec_find_paths(c, 0), 1);
    EXPECT(c->hw_volume, 0);
    EXPECT(hda_codec_setup(c, HDA_FMT_48K_16_2), 0);
    EXPECT(s.amp_out[0x02], 0x10);
    hda_codec_set_volume(c, 10, 0);
    EXPECT(s.amp_out[0x02], 0x10);              /* untouched */
    free(c);

    /* a modem codec: no audio function group */
    model_t modem = qemu_duplex;
    modem.fg_type = 2;
    modem.vendor = 0x11C11040;
    c = codec_new(&s, &modem);
    EXPECT(hda_codec_probe(c), E_NODEV);
    EXPECT(c->modem, 1);
    free(c);

    /* a codec that stops answering half way through */
    for (int k = 1; k < 60; k += 3) {
        c = codec_new(&s, &alc892);
        s.fail_after = k;
        int r = hda_codec_probe(c);
        if (!r && hda_codec_find_paths(c, 1)) hda_codec_setup(c, HDA_FMT_48K_16_2);
        CHECK(c->verbs_failed > 0, "k %d", k);
        free(c);
    }
    /* no answer at all */
    model_t none = alc892;
    none.vendor = 0;
    c = codec_new(&s, &none);
    EXPECT(hda_codec_probe(c), E_NODEV);
    free(c);
}

int main(int argc, char **argv) {
    g_verbose = argc > 1 && !strcmp(argv[1], "-v");
    test_alc892();
    test_alc269();
    test_alc1220();
    test_hdmi();
    test_nvidia();
    test_qemu();
    test_constructed();
    printf("hdatest: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail != 0;
}
