/* Halcyon OS - HD Audio codecs: the widget graph, output paths, jacks.
 *
 * A codec's audio function group (AFG) is a graph of widgets: converters
 * (the DACs), mixers, selectors and pin complexes (the jacks and the
 * internal speakers), each with a connection list naming the widgets that
 * feed it. The BIOS describes every pin in its configuration default
 * register: what it is (line out, speaker, headphones, S/PDIF, HDMI ...),
 * where (rear or front jack, internal), and its default association and
 * sequence, which group pins into one logical device (Microsoft's pin
 * configuration guidelines). This file reads all of that, then for every
 * output pin worth driving searches backwards through the connection lists
 * (breadth first, so the shortest path wins) for a DAC:
 *
 *   - analog pins: every speaker and headphone pin, and the line out with
 *     the lowest sequence of each association (the front pair: the other
 *     line outs of a desktop association are the surround channels, left
 *     off); ordered speakers, line outs, headphones; the first is the main
 *     output. A DAC nobody uses yet is preferred, else one is shared (every
 *     DAC gets the same stream, so sharing is fine). A selector or pin
 *     already switched by another path is not switched to another input.
 *   - digital pins (HDMI, DisplayPort, S/PDIF) only when there is no analog
 *     output at all; HDMI/DP pins with a monitor present (pin sense) first.
 *   - if the BIOS left no usable configuration (every pin "no connection"),
 *     every output capable analog pin is tried as a line out (headphones if
 *     it can drive them).
 *
 * Along each path: mixer inputs that are not on a path are muted (they are
 * the analog loopback of the inputs: mic, line in), the inputs on it
 * unmuted at 0 dB, selectors and pins switched to it, every output amp set
 * to 0 dB (the gain step the codec reports as 0 dB, never above), pins
 * enabled for output (with the headphone amplifier on headphone pins),
 * external amplifiers powered (EAPD), the widgets and the function group put
 * in D0. HDMI/DP pins also get an audio InfoFrame (2 channels, standard
 * layout) and their converters digital output enabled.
 *
 * Volume: when every path has an amplifier with at least 46 dB of range
 * below 0 dB (the first one from the DAC: usually the DAC's own output amp)
 * the master volume is applied there (hw_volume), on the same curve as the
 * mixer's (-40 log10 of the fraction: -12 dB at 50 %); otherwise the mixer
 * scales the samples and the amps stay at 0 dB.
 *
 * Jacks: pins with presence detect get unsolicited responses (tag = path
 * index + 1) where the codec has them; the controller driver also polls
 * them. While headphones are plugged in, speaker paths are muted (their pin
 * output disabled), and so are the other outputs of the headphones'
 * association when the headphones have sequence 15 (the guidelines'
 * "headphone redirection": the usual front-panel jack of a desktop, which
 * then also silences the rear line out). */
#include "sound/drivers/hda.h"
#include "core/lib.h"
#include "core/kmath.h"

#define MIN_HW_RANGE_QDB (46 * 4)    /* hardware volume needs this much range (quarter dB) */

/* ---------------- verbs ---------------- */
static int send(hda_codec_t *c, int nid, u32 verb, u32 payload, u32 *resp) {
    u32 v = verb <= 0xF ? hda_verb4(c->cad, nid, verb, payload) : hda_verb(c->cad, nid, verb, payload);
    u32 r = 0;
    int e = c->cmd(c->ctx, v, &r);
    if (e) { c->verbs_failed++; r = 0; }
    if (resp) *resp = r;
    return e;
}
static u32 get(hda_codec_t *c, int nid, u32 verb, u32 payload) {
    u32 r;
    send(c, nid, verb, payload, &r);
    return r;
}
static u32 param(hda_codec_t *c, int nid, u32 p) { return get(c, nid, HDA_GET_PARAM, p); }
static void set(hda_codec_t *c, int nid, u32 verb, u32 payload) { send(c, nid, verb, payload, NULL); }

static hda_widget_t *W(hda_codec_t *c, int nid) {
    if (nid < c->wstart || nid >= c->wstart + c->nw) return NULL;
    return &c->w[nid - c->wstart];
}
static const hda_widget_t *Wc(const hda_codec_t *c, int nid) {
    if (nid < c->wstart || nid >= c->wstart + c->nw) return NULL;
    return &c->w[nid - c->wstart];
}

/* ---------------- names ---------------- */
const char *hda_out_name(int kind) {
    switch (kind) {
    case HDA_OUT_SPEAKER: return "Speakers";
    case HDA_OUT_LINE: return "Line out";
    case HDA_OUT_HP: return "Headphones";
    case HDA_OUT_HDMI: return "HDMI";
    case HDA_OUT_DP: return "DisplayPort";
    case HDA_OUT_SPDIF: return "S/PDIF";
    }
    return "output";
}

static const char *wtype_name(int t) {
    static const char *n[16] = { "dac", "adc", "mixer", "select", "pin", "power", "volknob", "beep",
                                 "?8", "?9", "?a", "?b", "?c", "?d", "?e", "vendor" };
    return n[t & 15];
}

static const char *device_name(int d) {
    static const char *n[16] = { "Line out", "Speaker", "HP out", "CD", "SPDIF out", "Digital out",
                                 "Modem line", "Modem handset", "Line in", "Aux", "Mic in", "Telephony",
                                 "SPDIF in", "Digital in", "Reserved", "Other" };
    return n[d & 15];
}

static void location_name(u32 cfg, char *buf, int n) {
    static const char *geo[16] = { "", "Rear", "Front", "Left", "Right", "Top", "Bottom", "Special",
                                   "Special", "Special", "?", "?", "?", "?", "?", "?" };
    static const char *gross[4] = { "Ext", "Int", "Sep", "Other" };
    static const char *conn[4] = { "jack", "none", "fixed", "both" };
    int loc = HDA_CFG_LOCATION(cfg);
    snprintf(buf, n, "%s, %s%s%s", conn[HDA_CFG_CONN(cfg)], gross[loc >> 4], geo[loc & 15][0] ? " " : "", geo[loc & 15]);
}

static const char *color_name(int c) {
    static const char *n[16] = { "", "Black", "Grey", "Blue", "Green", "Red", "Orange", "Yellow",
                                 "Purple", "Pink", "?", "?", "?", "?", "White", "Other" };
    return n[c & 15];
}

/* a name for the codec from its vendor/device ID (the log; System Information) */
static void codec_name(hda_codec_t *c) {
    u16 v = (u16)(c->vendor_id >> 16), d = (u16)c->vendor_id;
    static const struct { u32 id; const char *name; } named[] = {
        { 0x10EC0899, "Realtek ALC898" }, { 0x10EC0900, "Realtek ALC1150" }, { 0x10EC1168, "Realtek ALC1220" },
        { 0x10EC0B00, "Realtek ALCS1200A" }, { 0x10EC0283, "Realtek ALC283" }, { 0x11D41984, "Analog Devices AD1984" },
        { 0x11D4198B, "Analog Devices AD1988B" }, { 0x11D41988, "Analog Devices AD1988" },
        { 0x11D41882, "Analog Devices AD1882" }, { 0x11D4194A, "Analog Devices AD1984A" },
        { 0x14F15051, "Conexant CX20561" }, { 0x14F15066, "Conexant CX20582" }, { 0x14F15069, "Conexant CX20585" },
        { 0x14F1506E, "Conexant CX20590" }, { 0x14F150F2, "Conexant CX20722" }, { 0x11020011, "Creative CA0132" },
        { 0x434D4980, "C-Media CMI9880" }, { 0x1AF40012, "QEMU hda-output" }, { 0x1AF40022, "QEMU hda-duplex" },
        { 0x1AF40032, "QEMU hda-micro" },
    };
    for (unsigned i = 0; i < ARRAY_SIZE(named); i++)
        if (named[i].id == c->vendor_id) { snprintf(c->name, sizeof c->name, "%s", named[i].name); return; }
    switch (v) {
    case 0x10EC: snprintf(c->name, sizeof c->name, "Realtek ALC%X", d); return;
    case 0x8086: snprintf(c->name, sizeof c->name, "Intel %s", d >= 0x2800 && d < 0x2900 ? "HDMI/DP" : "codec"); return;
    case 0x10DE: snprintf(c->name, sizeof c->name, "NVIDIA HDMI/DP"); return;
    case 0x1002: snprintf(c->name, sizeof c->name, "AMD HDMI/DP"); return;
    case 0x1106: snprintf(c->name, sizeof c->name, "VIA VT%04X", d); return;
    case 0x11D4: snprintf(c->name, sizeof c->name, "Analog Devices AD%04X", d); return;
    case 0x14F1: snprintf(c->name, sizeof c->name, "Conexant %04X", d); return;
    case 0x111D: case 0x8384: snprintf(c->name, sizeof c->name, "IDT/SigmaTel %04X", d); return;
    case 0x1013: snprintf(c->name, sizeof c->name, "Cirrus Logic CS%04X", d); return;
    case 0x1102: snprintf(c->name, sizeof c->name, "Creative %04X", d); return;
    case 0x434D: snprintf(c->name, sizeof c->name, "C-Media %04X", d); return;
    case 0x1AF4: snprintf(c->name, sizeof c->name, "QEMU %04X", d); return;
    case 0x17E8: snprintf(c->name, sizeof c->name, "Chrontel HDMI"); return;
    case 0x1057: snprintf(c->name, sizeof c->name, "Motorola modem"); return;
    case 0x11C1: snprintf(c->name, sizeof c->name, "LSI/Agere modem"); return;
    }
    snprintf(c->name, sizeof c->name, "codec %08X", c->vendor_id);
}

/* ---------------- probing ---------------- */
static void read_conns(hda_codec_t *c, hda_widget_t *w) {
    u32 cl = param(c, w->nid, HDA_PAR_CONN_LEN);
    int lng = (cl & 0x80) != 0, len = (int)(cl & 0x7F);
    int per = lng ? 2 : 4, bits = lng ? 16 : 8;
    u32 mask = lng ? 0xFFFF : 0xFF, rbit = lng ? 0x8000 : 0x80;
    u32 r = 0;
    int prev = 0;
    w->nconn = 0;
    for (int i = 0; i < len && w->nconn < HDA_MAX_CONN; i++) {
        if (i % per == 0) r = get(c, w->nid, HDA_GET_CONN_LIST, (u32)i);
        u32 e = (r >> ((i % per) * bits)) & mask;
        int v = (int)(e & (rbit - 1));
        if ((e & rbit) && prev && v > prev && v - prev < 128) {
            /* a range: prev+1 .. v */
            for (int k = prev + 1; k <= v && w->nconn < HDA_MAX_CONN; k++) w->conn[w->nconn++] = (u16)k;
        } else {
            w->conn[w->nconn++] = (u16)v;
        }
        prev = v;
    }
}

int hda_codec_probe(hda_codec_t *c) {
    u32 r;
    c->afg = 0; c->nw = 0; c->npaths = 0;
    if (send(c, 0, HDA_GET_PARAM, HDA_PAR_VENDOR_ID, &r) || r == 0 || r == 0xFFFFFFFF) return E_NODEV;
    c->vendor_id = r;
    c->revision = param(c, 0, HDA_PAR_REVISION_ID);
    codec_name(c);
    u32 nc = param(c, 0, HDA_PAR_NODE_COUNT);
    int start = (int)((nc >> 16) & 0xFF), n = (int)(nc & 0xFF);
    for (int i = 0; i < n && i < 16; i++) {
        u32 t = param(c, start + i, HDA_PAR_FG_TYPE);
        if ((t & 0xFF) == HDA_FG_AUDIO && !c->afg) {
            c->afg = (u16)(start + i);
            c->afg_unsol = (t & HDA_FG_UNSOL) != 0;
        } else if ((t & 0xFF) == HDA_FG_MODEM) {
            c->modem = 1;
        }
    }
    if (!c->afg) return E_NODEV;
    c->subsystem = get(c, c->afg, HDA_GET_SUBSYSTEM_ID, 0);
    /* D0 for the function group (it may come out of reset in D3) */
    set(c, c->afg, HDA_SET_POWER_STATE, 0);
    for (int i = 0; i < 100; i++) {
        u32 ps = get(c, c->afg, HDA_GET_POWER_STATE, 0);
        if (((ps >> 4) & 0xF) == 0) break;
        if (c->delay) c->delay(1);
    }
    c->afg_ampin = param(c, c->afg, HDA_PAR_AMP_IN_CAPS);
    c->afg_ampout = param(c, c->afg, HDA_PAR_AMP_OUT_CAPS);
    u32 afg_pcm = param(c, c->afg, HDA_PAR_PCM);
    nc = param(c, c->afg, HDA_PAR_NODE_COUNT);
    c->wstart = (u16)((nc >> 16) & 0xFF);
    n = (int)(nc & 0xFF);
    if (c->wstart == 0 || c->wstart + n > 0x80) n = c->wstart ? 0x80 - c->wstart : 0;
    if (n > HDA_MAX_WIDGETS) {
        kprintf("%s: codec %d: %d widgets, only %d used\n", c->log, c->cad, n, HDA_MAX_WIDGETS);
        n = HDA_MAX_WIDGETS;
    }
    c->nw = n;
    for (int i = 0; i < n; i++) {
        hda_widget_t *w = &c->w[i];
        memset(w, 0, sizeof *w);
        w->nid = (u16)(c->wstart + i);
        w->caps = param(c, w->nid, HDA_PAR_WIDGET_CAPS);
        w->type = (u8)HDA_WCAP_TYPE(w->caps);
        if (w->caps & HDA_WCAP_IN_AMP)
            w->ampin = (w->caps & HDA_WCAP_AMP_OVRD) ? param(c, w->nid, HDA_PAR_AMP_IN_CAPS) : c->afg_ampin;
        if (w->caps & HDA_WCAP_OUT_AMP)
            w->ampout = (w->caps & HDA_WCAP_AMP_OVRD) ? param(c, w->nid, HDA_PAR_AMP_OUT_CAPS) : c->afg_ampout;
        if (w->type == HDA_W_PIN) {
            w->pincap = param(c, w->nid, HDA_PAR_PIN_CAPS);
            w->cfg = get(c, w->nid, HDA_GET_CONFIG_DEFAULT, 0);
        }
        if (w->type == HDA_W_OUTPUT)
            w->pcm = (w->caps & HDA_WCAP_FORMAT_OVRD) ? param(c, w->nid, HDA_PAR_PCM) : afg_pcm;
        if (w->caps & HDA_WCAP_CONN_LIST) read_conns(c, w);
    }
    return 0;
}

/* ---------------- output paths ---------------- */
typedef struct cand { hda_widget_t *w; int kind, digital, order, present; } cand_t;

/* what an output pin is: HDA_OUT_* or -1 (not an output); *digital */
static int pin_kind(const hda_widget_t *w, int any_config, int *digital) {
    *digital = 0;
    if (!(w->pincap & HDA_PINCAP_OUT)) return -1;
    if (w->pincap & (HDA_PINCAP_HDMI | HDA_PINCAP_DP)) {
        *digital = 1;
        if (!any_config && HDA_CFG_CONN(w->cfg) == HDA_CONN_NONE) return -1;
        return (w->pincap & HDA_PINCAP_HDMI) ? HDA_OUT_HDMI : HDA_OUT_DP;
    }
    int dev = HDA_CFG_DEVICE(w->cfg);
    if (any_config) {
        if (w->caps & HDA_WCAP_DIGITAL) return -1;
        return (w->pincap & HDA_PINCAP_HP_DRIVE) ? HDA_OUT_HP : HDA_OUT_LINE;
    }
    if (HDA_CFG_CONN(w->cfg) == HDA_CONN_NONE) return -1;
    if (dev == HDA_DEV_SPDIF_OUT || dev == HDA_DEV_DIG_OUT || (w->caps & HDA_WCAP_DIGITAL)) {
        *digital = 1;
        return HDA_OUT_SPDIF;
    }
    switch (dev) {
    case HDA_DEV_SPEAKER: return HDA_OUT_SPEAKER;
    case HDA_DEV_HP: return HDA_OUT_HP;
    case HDA_DEV_LINE_OUT:
        /* a line out that is built in (no jack, inside the case) drives speakers */
        if (HDA_CFG_CONN(w->cfg) == HDA_CONN_FIXED && (HDA_CFG_LOCATION(w->cfg) >> 4) == 1) return HDA_OUT_SPEAKER;
        return HDA_OUT_LINE;
    }
    return -1;
}

static int assoc_key(u32 cfg) {
    int a = HDA_CFG_ASSOC(cfg);
    return a ? a : 16;          /* 0 is reserved: after the others */
}

/* breadth-first search from the pin back to a converter; fills p */
static int find_path(hda_codec_t *c, hda_widget_t *pin, int digital, int allow_used, hda_path_t *p) {
    short parent[HDA_MAX_WIDGETS], queue[HDA_MAX_WIDGETS];
    u8 pidx[HDA_MAX_WIDGETS], depth[HDA_MAX_WIDGETS], seen[HDA_MAX_WIDGETS];
    memset(seen, 0, sizeof seen);
    int qh = 0, qt = 0, start = pin->nid - c->wstart;
    queue[qt++] = (short)start;
    seen[start] = 1;
    parent[start] = -1;
    depth[start] = 0;
    while (qh < qt) {
        int x = queue[qh++];
        hda_widget_t *wx = &c->w[x];
        if (depth[x] >= HDA_PATH_MAX - 1) continue;
        for (int i = 0; i < wx->nconn; i++) {
            hda_widget_t *wy = W(c, wx->conn[i]);
            if (!wy) continue;
            int y = wy->nid - c->wstart;
            if (seen[y]) continue;
            /* a selector (or pin) another path already switched elsewhere */
            if (wx->nconn > 1 && wx->sel_claimed && wx->sel != i &&
                (wx->type == HDA_W_SELECTOR || wx->type == HDA_W_PIN)) continue;
            if (wy->type == HDA_W_OUTPUT) {
                if (((wy->caps & HDA_WCAP_DIGITAL) != 0) != (digital != 0)) continue;
                if (!allow_used && wy->used) continue;
                /* found: walk back to the pin */
                parent[y] = (short)x;
                pidx[y] = (u8)i;
                int len = 0;
                for (int k = y; k >= 0; k = parent[k]) p->nid[len++] = c->w[k].nid;
                p->idx[0] = 0;
                /* idx[k] = position of nid[k-1] in nid[k]'s connection list */
                int k = y;
                for (int j = 1; j < len; j++) { p->idx[j] = pidx[k]; k = parent[k]; }
                p->len = len;
                return 1;
            }
            if (wy->type != HDA_W_MIXER && wy->type != HDA_W_SELECTOR) continue;
            seen[y] = 1;
            parent[y] = (short)x;
            pidx[y] = (u8)i;
            depth[y] = (u8)(depth[x] + 1);
            queue[qt++] = (short)y;
        }
    }
    return 0;
}

/* amps of a widget on a path: the output amp, or the input amp at index idx */
static int amp_range_qdb(u32 caps) {
    int g0 = MIN((int)HDA_AMPCAP_OFFSET(caps), (int)HDA_AMPCAP_STEPS(caps));
    return g0 * (int)HDA_AMPCAP_STEP_QDB(caps);
}

static void choose_amps(hda_codec_t *c, hda_path_t *p) {
    p->vol_nid = 0; p->mute_nid = 0;
    u16 any_nid = 0; u8 any_out = 0, any_idx = 0; u32 any_caps = 0;
    for (int k = 0; k < p->len; k++) {
        hda_widget_t *w = W(c, p->nid[k]);
        int last = k == p->len - 1;
        /* in path order: the widget's input amp (for this input), then its output amp */
        for (int o = 0; o < 2; o++) {
            u32 caps;
            u8 out = (u8)o, idx = 0;
            if (!o) {
                if (k == 0 || last || !(w->caps & HDA_WCAP_IN_AMP)) continue;
                caps = w->ampin;
                idx = p->idx[k];
            } else {
                if (!(w->caps & HDA_WCAP_OUT_AMP)) continue;
                caps = w->ampout;
            }
            if (caps & HDA_AMPCAP_MUTE) { p->mute_nid = w->nid; p->mute_out = out; p->mute_idx = idx; }
            if (HDA_AMPCAP_STEPS(caps) == 0) continue;
            if (!any_nid) { any_nid = w->nid; any_out = out; any_idx = idx; any_caps = caps; }
            if (!p->vol_nid && amp_range_qdb(caps) >= MIN_HW_RANGE_QDB) {
                p->vol_nid = w->nid; p->vol_out = out; p->vol_idx = idx; p->vol_caps = caps;
            }
        }
    }
    if (!p->vol_nid && any_nid) {       /* not enough range: kept at 0 dB, the mixer does the volume */
        p->vol_nid = any_nid; p->vol_out = any_out; p->vol_idx = any_idx; p->vol_caps = any_caps;
    }
}

static void claim(hda_codec_t *c, hda_path_t *p) {
    for (int k = 0; k < p->len; k++) {
        hda_widget_t *w = W(c, p->nid[k]);
        w->used = 1;
        if (k > 0 && w->nconn > 1 && (w->type == HDA_W_SELECTOR || w->type == HDA_W_PIN)) {
            w->sel_claimed = 1;
            w->sel = p->idx[k];
        }
    }
}

static int pin_present(hda_codec_t *c, hda_widget_t *w) {
    if (!(w->pincap & HDA_PINCAP_PRESENCE)) return 0;
    if (w->pincap & HDA_PINCAP_TRIGGER) set(c, w->nid, HDA_EXEC_PIN_SENSE, 0);
    return (get(c, w->nid, HDA_GET_PIN_SENSE, 0) & HDA_SENSE_PRESENCE) != 0;
}

static int cand_cmp(const cand_t *a, const cand_t *b) {
    if (a->present != b->present) return b->present - a->present;
    return a->order - b->order;
}

static int build(hda_codec_t *c, cand_t *cd, int n, int max) {
    /* sort (insertion: a handful of pins) */
    for (int i = 1; i < n; i++) {
        cand_t t = cd[i];
        int j = i;
        while (j > 0 && cand_cmp(&t, &cd[j - 1]) < 0) { cd[j] = cd[j - 1]; j--; }
        cd[j] = t;
    }
    for (int i = 0; i < n && c->npaths < MIN(max, HDA_MAX_PATHS); i++) {
        hda_path_t *p = &c->path[c->npaths];
        memset(p, 0, sizeof *p);
        if (!find_path(c, cd[i].w, cd[i].digital, 0, p) && !find_path(c, cd[i].w, cd[i].digital, 1, p)) continue;
        p->kind = cd[i].kind;
        p->assoc = (u8)HDA_CFG_ASSOC(cd[i].w->cfg);
        p->seq = (u8)HDA_CFG_SEQ(cd[i].w->cfg);
        p->present = 1;
        claim(c, p);
        choose_amps(c, p);
        c->npaths++;
    }
    return c->npaths;
}

int hda_codec_find_paths(hda_codec_t *c, int allow_digital) {
    cand_t cd[HDA_MAX_WIDGETS];
    c->npaths = 0;
    c->digital = 0;
    c->hw_volume = 0;
    for (int i = 0; i < c->nw; i++) { c->w[i].used = 0; c->w[i].sel_claimed = 0; }
    for (int pass = 0; pass < 2 && !c->npaths; pass++) {
        /* pass 0: the BIOS's pin configuration; pass 1: none usable, every analog output pin */
        int n = 0;
        for (int i = 0; i < c->nw; i++) {
            hda_widget_t *w = &c->w[i];
            if (w->type != HDA_W_PIN) continue;
            int dig, kind = pin_kind(w, pass, &dig);
            if (kind < 0 || dig) continue;
            if (!pass && kind == HDA_OUT_LINE && HDA_CFG_ASSOC(w->cfg) != 0xF) {
                /* surround channels: another line out of the association comes first */
                int skip = 0;
                for (int j = 0; j < c->nw && !skip; j++) {
                    hda_widget_t *o = &c->w[j];
                    int d2;
                    if (o == w || o->type != HDA_W_PIN || pin_kind(o, 0, &d2) != HDA_OUT_LINE || d2) continue;
                    if (HDA_CFG_ASSOC(o->cfg) != HDA_CFG_ASSOC(w->cfg)) continue;
                    if (HDA_CFG_SEQ(o->cfg) < HDA_CFG_SEQ(w->cfg) ||
                        (HDA_CFG_SEQ(o->cfg) == HDA_CFG_SEQ(w->cfg) && o->nid < w->nid)) skip = 1;
                }
                if (skip) continue;
            }
            int rank = kind == HDA_OUT_SPEAKER ? 0 : kind == HDA_OUT_LINE ? 1 : 2;
            cd[n].w = w; cd[n].kind = kind; cd[n].digital = 0; cd[n].present = 0;
            cd[n].order = rank << 16 | assoc_key(w->cfg) << 12 | HDA_CFG_SEQ(w->cfg) << 8 | (w->nid & 0xFF);
            n++;
        }
        build(c, cd, n, HDA_MAX_PATHS);
        if (c->npaths && pass) kprintf("%s: codec %d: no usable pin configuration, every output pin is driven\n", c->log, c->cad);
    }
    if (!c->npaths && allow_digital) {
        int n = 0, any_present = 0;
        for (int i = 0; i < c->nw; i++) {
            hda_widget_t *w = &c->w[i];
            if (w->type != HDA_W_PIN) continue;
            int dig, kind = pin_kind(w, 0, &dig);
            if (kind < 0 || !dig) continue;
            int pres = (kind == HDA_OUT_HDMI || kind == HDA_OUT_DP) && pin_present(c, w);
            any_present |= pres;
            cd[n].w = w; cd[n].kind = kind; cd[n].digital = 1; cd[n].present = pres;
            cd[n].order = (kind == HDA_OUT_SPDIF) << 16 | assoc_key(w->cfg) << 12 | HDA_CFG_SEQ(w->cfg) << 8 | (w->nid & 0xFF);
            n++;
        }
        /* a monitor is connected: just its pins */
        if (any_present) {
            int m = 0;
            for (int i = 0; i < n; i++) if (cd[i].present) cd[m++] = cd[i];
            n = m;
        }
        build(c, cd, n, 4);
        c->digital = c->npaths > 0;
    }
    if (!c->npaths) return 0;
    c->hw_volume = 1;
    for (int i = 0; i < c->npaths; i++) {
        hda_path_t *p = &c->path[i];
        if (!p->vol_nid || amp_range_qdb(p->vol_caps) < MIN_HW_RANGE_QDB ||
            (!(p->vol_caps & HDA_AMPCAP_MUTE) && !p->mute_nid)) c->hw_volume = 0;
    }
    return c->npaths;
}

/* ---------------- programming the codec ---------------- */
static void amp_set(hda_codec_t *c, int nid, int out, int idx, int mute, int gain) {
    u32 v = (out ? HDA_AMP_OUT : HDA_AMP_IN) | HDA_AMP_LEFT | HDA_AMP_RIGHT | HDA_AMP_INDEX(idx & 0xF) |
            (mute ? HDA_AMP_MUTE : 0) | (u32)(gain & 0x7F);
    set(c, nid, HDA_SET_AMP, v);
}

static int gain_0db(u32 caps) {
    return MIN((int)HDA_AMPCAP_OFFSET(caps), (int)HDA_AMPCAP_STEPS(caps));
}

int hda_volume_gain(u32 caps, int percent) {
    int g0 = gain_0db(caps);
    if (percent >= 100) return g0;
    if (percent <= 0) return 0;
    /* -40 log10(percent / 100) dB, in steps */
    double db = -40.0 * k_log(percent / 100.0) / k_log(10.0);
    int steps = (int)(db * 4.0 / HDA_AMPCAP_STEP_QDB(caps) + 0.5);
    return MAX(0, g0 - steps);
}

static void pin_ctl(hda_codec_t *c, const hda_path_t *p, int on) {
    const hda_widget_t *w = W(c, p->nid[p->len - 1]);
    u32 v = 0;
    if (on) {
        v = HDA_PINCTL_OUT;
        if (p->kind == HDA_OUT_HP && (w->pincap & HDA_PINCAP_HP_DRIVE)) v |= HDA_PINCTL_HP;
    }
    set(c, w->nid, HDA_SET_PIN_CTL, v);
}

/* HDMI / DisplayPort: the audio InfoFrame (CEA-861 / DP 1.2 SDP): two
 * channels, the speaker layout and sample format from the stream */
static void infoframe(hda_codec_t *c, hda_widget_t *pin) {
    u8 f[14];
    int n = 0;
    int dp = (pin->pincap & HDA_PINCAP_DP) && !(pin->pincap & HDA_PINCAP_HDMI);
    u32 eld = get(c, pin->nid, HDA_GET_HDMI_ELDD, 5);      /* ELD byte 5: SAD count, connection type */
    if (eld & 0x80000000) dp = ((eld >> 2) & 3) == 1;
    memset(f, 0, sizeof f);
    if (dp) {
        f[0] = 0x84; f[1] = 0x1B; f[2] = 0x11 << 2;          /* type, length - 1, version */
        f[3] = 0x01;                                        /* channel count - 1 */
        n = 8;
    } else {
        f[0] = 0x84; f[1] = 0x01; f[2] = 0x0A;               /* type, version, length */
        f[4] = 0x01;                                        /* channel count - 1 */
        u8 sum = 0;
        for (int i = 0; i < 14; i++) sum = (u8)(sum + f[i]);
        f[3] = (u8)(0x100 - sum);
        n = 14;
    }
    set(c, pin->nid, HDA_SET_HDMI_DIP_XMIT, 0x00);
    set(c, pin->nid, HDA_SET_HDMI_DIP_INDEX, 0x00);         /* packet 0 (audio InfoFrame), byte 0 */
    for (int i = 0; i < n; i++) set(c, pin->nid, HDA_SET_HDMI_DIP_DATA, f[i]);
    set(c, pin->nid, HDA_SET_HDMI_DIP_XMIT, 0xC0);          /* send it, best effort */
}

int hda_codec_setup(hda_codec_t *c, u16 format) {
    if (!c->npaths) return E_NODEV;
    int failed = c->verbs_failed;
    set(c, c->afg, HDA_SET_POWER_STATE, 0);
    /* power, and every input of the mixers on paths muted */
    for (int i = 0; i < c->npaths; i++) {
        hda_path_t *p = &c->path[i];
        for (int k = 0; k < p->len; k++) {
            hda_widget_t *w = W(c, p->nid[k]);
            if (w->caps & HDA_WCAP_POWER) set(c, w->nid, HDA_SET_POWER_STATE, 0);
            if (k > 0 && w->type == HDA_W_MIXER && (w->caps & HDA_WCAP_IN_AMP))
                for (int j = 0; j < w->nconn && j < 16; j++) amp_set(c, w->nid, 0, j, 1, 0);
        }
    }
    for (int i = 0; i < c->npaths; i++) {
        hda_path_t *p = &c->path[i];
        for (int k = 0; k < p->len; k++) {
            hda_widget_t *w = W(c, p->nid[k]);
            int last = k == p->len - 1;
            if (k > 0 && w->nconn > 1 && (w->type == HDA_W_SELECTOR || w->type == HDA_W_PIN))
                set(c, w->nid, HDA_SET_CONN_SELECT, p->idx[k]);
            if (k > 0 && !last && (w->caps & HDA_WCAP_IN_AMP))
                amp_set(c, w->nid, 0, p->idx[k], 0, gain_0db(w->ampin));
            if (w->caps & HDA_WCAP_OUT_AMP) amp_set(c, w->nid, 1, 0, 0, gain_0db(w->ampout));
        }
        hda_widget_t *cv = W(c, p->nid[0]), *pin = W(c, p->nid[p->len - 1]);
        set(c, cv->nid, HDA_SET_FORMAT, format);
        if (cv->caps & HDA_WCAP_DIGITAL) set(c, cv->nid, HDA_SET_DIGI_CONV1, 0x01);     /* DigEn */
        if (p->kind == HDA_OUT_HDMI || p->kind == HDA_OUT_DP) {
            set(c, cv->nid, HDA_SET_CVT_CHAN_COUNT, 1);     /* two channels */
            infoframe(c, pin);
        }
        pin_ctl(c, p, 1);
        if (pin->pincap & HDA_PINCAP_EAPD)
            set(c, pin->nid, HDA_SET_EAPD_BTL, (get(c, pin->nid, HDA_GET_EAPD_BTL, 0) & 0x7) | HDA_EAPD);
        /* jack detection (not for digital outputs: there "present" means a monitor) */
        p->detect = !c->digital && (pin->pincap & HDA_PINCAP_PRESENCE) &&
                    !(HDA_CFG_MISC(pin->cfg) & HDA_CFG_NO_DETECT) &&
                    (HDA_CFG_CONN(pin->cfg) == HDA_CONN_JACK || HDA_CFG_CONN(pin->cfg) == HDA_CONN_BOTH);
        p->unsol = 0;
        if (p->detect && c->afg_unsol && (pin->caps & HDA_WCAP_UNSOL)) {
            set(c, pin->nid, HDA_SET_UNSOL, 0x80 | (u32)(i + 1));
            p->unsol = 1;
        }
        p->present = 1;
        p->jack_muted = 0;
    }
    hda_codec_jacks(c);
    return c->verbs_failed != failed ? E_IO : 0;
}

void hda_codec_stream(hda_codec_t *c, int tag, u16 format) {
    for (int i = 0; i < c->npaths; i++) {
        int cv = c->path[i].nid[0];
        int dup = 0;
        for (int j = 0; j < i; j++) dup |= c->path[j].nid[0] == cv;
        if (dup) continue;
        if (tag) set(c, cv, HDA_SET_FORMAT, format);
        set(c, cv, HDA_SET_STREAM_CHANNEL, (u32)(tag & 0xF) << 4);
    }
}

void hda_codec_set_volume(hda_codec_t *c, int percent, int mute) {
    if (!c->hw_volume) return;
    if (percent <= 0) mute = 1;
    for (int i = 0; i < c->npaths; i++) {
        hda_path_t *p = &c->path[i];
        int g = mute ? 0 : hda_volume_gain(p->vol_caps, percent);
        int vmute = mute && (p->vol_caps & HDA_AMPCAP_MUTE);
        amp_set(c, p->vol_nid, p->vol_out, p->vol_idx, vmute, g);
        if (p->mute_nid && (p->mute_nid != p->vol_nid || p->mute_out != p->vol_out || p->mute_idx != p->vol_idx)) {
            const hda_widget_t *w = W(c, p->mute_nid);
            u32 caps = p->mute_out ? w->ampout : w->ampin;
            amp_set(c, p->mute_nid, p->mute_out, p->mute_idx, mute && !vmute, gain_0db(caps));
        }
    }
}

int hda_codec_jacks(hda_codec_t *c) {
    int changed = 0, hp = 0;
    for (int i = 0; i < c->npaths; i++) {
        hda_path_t *p = &c->path[i];
        if (!p->detect) continue;
        int pres = pin_present(c, W(c, p->nid[p->len - 1]));
        if (pres != p->present) { p->present = (u8)pres; changed = 1; }
    }
    for (int i = 0; i < c->npaths; i++)
        if (c->path[i].kind == HDA_OUT_HP && c->path[i].detect && c->path[i].present) hp = 1;
    c->hp_plugged = hp;
    for (int i = 0; i < c->npaths; i++) {
        hda_path_t *p = &c->path[i];
        int mute = 0;
        if (hp && p->kind != HDA_OUT_HP) {
            if (p->kind == HDA_OUT_SPEAKER) mute = 1;
            for (int j = 0; j < c->npaths && !mute; j++) {
                const hda_path_t *h = &c->path[j];
                if (h->kind == HDA_OUT_HP && h->detect && h->present && h->seq == 0xF &&
                    h->assoc && h->assoc != 0xF && h->assoc == p->assoc) mute = 1;
            }
        }
        if (mute != p->jack_muted) {
            p->jack_muted = (u8)mute;
            pin_ctl(c, p, !mute);
            changed = 1;
        }
    }
    return changed;
}

int hda_codec_unsol(hda_codec_t *c, u32 resp) {
    int tag = (int)(resp >> 26);
    if (tag < 1 || tag > c->npaths || !c->path[tag - 1].unsol) return 0;
    hda_codec_jacks(c);
    return 1;
}

void hda_codec_describe(const hda_codec_t *c, char *buf, int n) {
    int len = 0, seen = 0;
    buf[0] = 0;
    for (int pass = 0; pass < 2 && !len; pass++) {
        for (int i = 0; i < c->npaths; i++) {
            const hda_path_t *p = &c->path[i];
            if (!pass && (p->jack_muted || (p->detect && !p->present))) continue;
            if (seen & (1 << p->kind)) continue;
            seen |= 1 << p->kind;
            int r = snprintf(buf + len, (size_t)(n - len), "%s%s", len ? ", " : "", hda_out_name(p->kind));
            if (r < 0 || len + r >= n) break;
            len += r;
        }
        seen = 0;
    }
}

void hda_codec_dump(const hda_codec_t *c) {
    kprintf("%s: codec %d: %s (%08x rev %08x, subsystem %08x), %d widgets from %#x%s%s\n", c->log, c->cad, c->name,
            c->vendor_id, c->revision, c->subsystem, c->nw, c->wstart, c->afg_unsol ? ", unsolicited responses" : "",
            c->modem ? ", modem" : "");
    for (int i = 0; i < c->nw; i++) {
        const hda_widget_t *w = &c->w[i];
        char line[200], loc[32];
        if (!w->caps) continue;             /* an unused node number */
        int n = snprintf(line, sizeof line, "  %02x %-6s %08x", w->nid, wtype_name(w->type), w->caps);
        if (w->type == HDA_W_PIN) {
            location_name(w->cfg, loc, sizeof loc);
            n += snprintf(line + n, sizeof line - n, " pin %08x cfg %08x %s, %s, %s, a%x s%x", w->pincap, w->cfg,
                          device_name(HDA_CFG_DEVICE(w->cfg)), loc, color_name(HDA_CFG_COLOR(w->cfg)),
                          HDA_CFG_ASSOC(w->cfg), HDA_CFG_SEQ(w->cfg));
        }
        if (w->caps & HDA_WCAP_IN_AMP) n += snprintf(line + n, sizeof line - n, " in %08x", w->ampin);
        if (w->caps & HDA_WCAP_OUT_AMP) n += snprintf(line + n, sizeof line - n, " out %08x", w->ampout);
        if (w->nconn) {
            n += snprintf(line + n, sizeof line - n, " <-");
            for (int j = 0; j < w->nconn && n < (int)sizeof line - 4; j++) n += snprintf(line + n, sizeof line - n, " %02x", w->conn[j]);
        }
        kprintf("%s:%s\n", c->log, line);
    }
    for (int i = 0; i < c->npaths; i++) {
        const hda_path_t *p = &c->path[i];
        const hda_widget_t *pin = Wc(c, p->nid[p->len - 1]);
        char line[160], loc[32];
        int n = 0;
        for (int k = 0; k < p->len; k++)
            n += snprintf(line + n, sizeof line - n, "%s%02x", k ? " -> " : "", p->nid[k]);
        location_name(pin->cfg, loc, sizeof loc);
        if (p->vol_nid)
            n += snprintf(line + n, sizeof line - n, ", volume %02x %s (%d dB range%s)", p->vol_nid,
                          p->vol_out ? "out" : "in", amp_range_qdb(p->vol_caps) / 4,
                          amp_range_qdb(p->vol_caps) < MIN_HW_RANGE_QDB ? ": too little, kept at 0 dB" : "");
        kprintf("%s: codec %d: %s: %s (%s%s%s)\n", c->log, c->cad, hda_out_name(p->kind), line, loc,
                p->detect ? ", presence detect" : "", p->unsol ? " by unsolicited response" : "");
    }
}
