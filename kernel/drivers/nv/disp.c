/* Halcyon OS - NVIDIA GPUs (M10b): the modesetting driver for the EVO
 * display engine (Tesla to Pascal) on the display core (display.h).
 *
 * The hardware logic is in evo.c, evodp.c, evopll.c and evoscript.c
 * (host-tested); this file is the kernel side: registration from nv_init,
 * the BAR1 window and the VRAM we use, the start guard, the takeover from
 * the firmware, detect / mode_valid / buffers / mode sets, restore and the
 * hot-plug poll. See docs/DESIGN-M10-NVIDIA.md, "10b".
 *
 * Safety: nothing is written before enable (display_start, on the GUI
 * thread, after the start marker is saved); every wait is bounded; any
 * failure returns an E_* with the reason in the adapter's why, and the
 * display core gives the device back (restore: the firmware's mode is
 * programmed again, the saved registers put back) and uses the firmware
 * frame buffer. Interrupts: the GPU's are masked (PMC) and INTx is off
 * while the driver owns the display; the supervisor is polled.
 *
 * Switches: nv.disable=1 and F8 Safe Mode (nothing of M10), nv.modeset=0
 * (identification only), nv.scripts=0 (no VBIOS display scripts),
 * display.disable=nv (the display core), the start guard
 * (display.nv.guard). */
#include "nv.h"
#include "evo.h"
#include "drivers/display.h"
#include "core/lib.h"
#include "core/io.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/settings.h"

#define MiB (1ull << 20)
#define NVD_LOG 8192

typedef struct nvd_conn {
    int nvc;                    /* d->conn[] */
    int type;                   /* CONN_* */
    int nout;
    int out[6];                 /* DCB outputs */
    int sel;                    /* the output detect chose (DCB index), -1 */
    int dp_ok;                  /* a DisplayPort sink answered */
    nv_evo_dp_t dp;             /* its capabilities */
    int hdmi, digital;          /* from the EDID */
    int hpd;                    /* last hot-plug sense */
} nvd_conn_t;

typedef struct nvd {
    nv_device_t *d;
    display_adapter_t *a;
    const nv_evo_gen_t *g;
    nv_evo_t e;
    u8 *bar1;
    u64 bar1_phys, bar1_size, window;
    nv_evo_vram_t vram;
    u64 fw_off;
    u32 fw_pitch, fw_w, fw_h;
    int fw_ok;                  /* the firmware frame buffer is in BAR1 and 32 bpp */
    int armed;                  /* we wrote to the hardware */
    int taken;                  /* takeover done, the channel runs */
    int nconn;
    nvd_conn_t conn[DISPLAY_MAX_CONNECTORS];
    int hw_head[DISPLAY_MAX_HEADS];   /* display head index -> hardware head */
    struct { int on, dcb, or_type, or_id; } head[EVO_HEADS];
    nv_evo_dp_t dp[EVO_ORS];
    u16 pci_cmd;
    int pci_cmd_saved;
    int first_set_done;
    u64 next_hpd;
    char state[96];
    char *log;
    int loglen;
} nvd_t;

static nvd_t *nvds[NV_MAX_GPUS];
static int pramin_check(nvd_t *n, u64 off, int write);

static nvd_t *nvd_of(nv_device_t *d) {
    for (int i = 0; i < NV_MAX_GPUS; i++) if (nvds[i] && nvds[i]->d == d) return nvds[i];
    return NULL;
}

/* ---------------- log: BOOT.LOG and NVREPORT ---------------- */

static void nvlog(nvd_t *n, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void nvlog(nvd_t *n, const char *fmt, ...) {
    char line[220];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    kprintf("nv%d: %s\n", n->d->index, line);
    if (!n->log) n->log = kzalloc(NVD_LOG);
    if (n->log && n->loglen < NVD_LOG - 2) {
        int l = (int)strlen(line);
        if (n->loglen + l + 2 >= NVD_LOG) { l = NVD_LOG - 2 - n->loglen; }
        memcpy(n->log + n->loglen, line, (size_t)l);
        n->loglen += l;
        n->log[n->loglen++] = '\n';
        n->log[n->loglen] = 0;
    }
}

static void evo_log(void *ctx, const char *line) { nvlog(ctx, "%s", line); }
static int evo_i2c(void *ctx, int ccb, nv_i2c_msg_t *m, int k) { return nv_i2c_xfer(((nvd_t *)ctx)->d, ccb, m, k); }
static int evo_aux(void *ctx, int ccb, u8 req, u32 addr, u8 *buf, u8 *len) {
    return nv_aux_xfer(((nvd_t *)ctx)->d, ccb, req, addr, buf, len);
}
static u8 evo_rd8(void *ctx, u32 reg) { return nv_rd08(((nvd_t *)ctx)->d, reg); }
static void evo_wr8(void *ctx, u32 reg, u8 v) { nv_wr08(((nvd_t *)ctx)->d, reg, v); }

static int fail(nvd_t *n, int err, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int fail(nvd_t *n, int err, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(n->a->why, sizeof n->a->why, fmt, ap);
    va_end(ap);
    nvlog(n, "native modesetting stops: %s", n->a->why);
    return err;
}

/* ---------------- connectors ---------------- */

static int conn_type(u8 t, const char **name) {
    switch (t) {
    case 0x00: case 0x02: case 0x50: case 0x51: *name = "VGA"; return CONN_VGA;
    case 0x01: *name = "DVI-A"; return CONN_DVI;
    case 0x20: case 0x21: case 0x22: case 0x30: case 0x38: case 0x39: case 0x52: case 0x53: *name = "DVI-I"; return CONN_DVI;
    case 0x31: case 0x45: case 0x54: case 0x55: *name = "DVI-D"; return CONN_DVI;
    case 0x46: case 0x48: case 0x56: case 0x57: case 0x58: case 0x59: case 0x64: case 0x65: case 0x71: *name = "DP"; return CONN_DP;
    case 0x60: case 0x61: case 0x63: *name = "HDMI"; return CONN_HDMI;
    }
    *name = NULL;
    return CONN_UNKNOWN;
}

static const nv_dcb_output_t *dcb(nvd_t *n, int k) { return nv_output(n->d, k); }

/* the output to drive on a connector: DisplayPort if a sink answered,
 * else TMDS (unless the EDID says analog), else the DAC */
static int pick_output(nvd_t *n, nvd_conn_t *c) {
    int dp = -1, tmds = -1, crt = -1;
    for (int i = 0; i < c->nout; i++) {
        const nv_dcb_output_t *o = dcb(n, c->out[i]);
        if (!o || o->location) continue;
        if (o->type == NV_OUT_DP && dp < 0) dp = c->out[i];
        if (o->type == NV_OUT_TMDS && tmds < 0) tmds = c->out[i];
        if (o->type == NV_OUT_CRT && crt < 0 && n->g->has_dac) crt = c->out[i];
    }
    if (dp >= 0 && c->dp_ok) return dp;
    if (tmds >= 0 && (c->digital || crt < 0)) return tmds;
    if (crt >= 0) return crt;
    return tmds >= 0 ? tmds : dp;
}

/* ---------------- detect ---------------- */

static void nvd_detect(display_adapter_t *a) {
    nvd_t *n = a->priv;
    nv_device_t *d = n->d;
    u8 *edid = kzalloc(NV_EDID_MAX);
    for (int k = 0; k < n->nconn && k < a->nconns; k++) {
        nvd_conn_t *c = &n->conn[k];
        int status = CONN_DISCONNECTED, len = 0;
        char via[48] = "";
        c->dp_ok = 0;
        c->hpd = nv_hotplug(d, d->conn[c->nvc].index);
        for (int i = 0; i < c->nout && edid; i++) {
            const nv_dcb_output_t *o = dcb(n, c->out[i]);
            if (!o || o->type != NV_OUT_DP) continue;
            memset(&c->dp, 0, sizeof c->dp);
            c->dp.dcb = c->out[i];
            c->dp.ccb = o->i2c;
            if (!nv_dp_detect(&n->e, &c->dp)) {
                c->dp_ok = 1;
                status = CONN_CONNECTED;
                nvlog(n, "%s: DisplayPort sink, DPCD %d.%d, %d lanes, rate %02x (DCB lanes %d)", a->conns[k].name, c->dp.dpcd[0] >> 4,
                      c->dp.dpcd[0] & 15, c->dp.lanes_max, c->dp.bw_max, o->dp_max_lanes);
                len = nv_read_edid(d, o, edid, NV_EDID_MAX, via, sizeof via);
            }
            break;
        }
        for (int i = 0; i < c->nout && edid && len <= 0; i++) {
            const nv_dcb_output_t *o = dcb(n, c->out[i]);
            if (!o || o->type == NV_OUT_DP || o->type == NV_OUT_TV) continue;
            int dup = 0;
            for (int j = 0; j < i; j++) { const nv_dcb_output_t *q = dcb(n, c->out[j]); if (q && q->i2c == o->i2c && q->type != NV_OUT_DP) dup = 1; }
            if (dup) continue;
            len = nv_read_edid(d, o, edid, NV_EDID_MAX, via, sizeof via);
        }
        if (len <= 0 && d->conn[c->nvc].edid_len > 0) {
            /* (10a's copy: read before the takeover) */
            len = MIN(d->conn[c->nvc].edid_len, NV_EDID_MAX);
            memcpy(edid, d->conn[c->nvc].edid, (size_t)len);
            strlcpy(via, "read at boot", sizeof via);
        }
        if (len > 0) status = CONN_CONNECTED;
        else if (c->hpd == 1 && status == CONN_DISCONNECTED) status = CONN_MAYBE;
        c->hdmi = len > 0 && nv_edid_hdmi(edid, len);
        c->digital = len > 0 && nv_edid_digital(edid, len);
        c->sel = status == CONN_DISCONNECTED ? -1 : pick_output(n, c);
        const nv_dcb_output_t *so = dcb(n, c->sel);
        nvlog(n, "%s: %s%s%s, hot plug %s, EDID %d bytes%s%s, output %d (%s)", a->conns[k].name,
              status == CONN_CONNECTED ? "connected" : status == CONN_MAYBE ? "maybe connected" : "nothing connected",
              c->hdmi ? ", HDMI sink" : "", c->digital ? ", digital" : "", c->hpd == 1 ? "on" : c->hpd == 0 ? "off" : "unknown", len > 0 ? len : 0,
              via[0] ? " via " : "", via, c->sel, so ? nv_out_type_str(so->type) : "none");
        display_connector_update(&a->conns[k], status, len > 0 ? edid : NULL, len > 0 ? len : 0, 0, 0);
    }
    kfree(edid);
}

/* ---------------- mode_valid ---------------- */

static int dual_link_ok(nvd_t *n, const nv_dcb_output_t *o) {
    if (n->d->bios && n->d->bios->dcb_ver >= 0x40) return o->link == 3;
    return o->or_mask && ((1u << __builtin_ctz(o->or_mask)) * 3u) == o->or_mask;
}

static u32 max_clock(nvd_t *n, nvd_conn_t *c, const nv_dcb_output_t *o) {
    switch (o->type) {
    case NV_OUT_TMDS:
        if (c->hdmi && o->hdmi) return n->g->hdmi_khz;
        return n->g->tmds_khz * (dual_link_ok(n, o) ? 2u : 1u);
    case NV_OUT_CRT: return n->g->dac_khz ? n->g->dac_khz : 350000;
    }
    return 0;
}

static int nvd_mode_valid(display_adapter_t *a, display_connector_t *dc, const display_mode_t *m) {
    nvd_t *n = a->priv;
    nvd_conn_t *c = &n->conn[dc->index];
    const nv_dcb_output_t *o = dcb(n, c->sel);
    if (!o || (m->flags & DT_INTERLACE) || !m->t.clock_khz || m->t.clock_khz < 25000) return 0;
    if (m->w > n->g->max_w || m->h > n->g->max_h) return 0;
    if (o->type == NV_OUT_DP) return c->dp_ok && nv_dp_fits(c->dp.lanes_max, c->dp.bw_max, m->t.clock_khz, 24);
    return m->t.clock_khz <= max_clock(n, c, o);
}

/* ---------------- buffers ---------------- */

static int nvd_fb_alloc(display_adapter_t *a, int w, int h, display_fb_t *fb) {
    nvd_t *n = a->priv;
    if (!n->taken || w <= 0 || h <= 0) return E_IO;
    u32 pitch = ALIGN_UP((u32)w * 4, 256u);
    u64 size = ALIGN_UP((u64)pitch * (u64)h, 2 * MiB);   /* whole 2 MiB pages: write-combining is set per page */
    u64 at = nv_evo_vram_alloc(&n->vram, size, 2 * MiB);
    if (at == ~0ull) return E_NOMEM;
    /* (BAR1 is checked to map this block 1:1, at both ends) */
    if (!pramin_check(n, at, 1) || !pramin_check(n, at + size - 4096, 1)) {
        nv_evo_vram_free(&n->vram, at);
        nvlog(n, "buffer at VRAM %llx: BAR1 does not map it linearly", (unsigned long long)at);
        return E_NOMEM;
    }
    memset(fb, 0, sizeof *fb);
    fb->px = n->bar1 + at;
    fb->phys = n->bar1_phys + at;
    fb->w = w;
    fb->h = h;
    fb->pitch = pitch;
    fb->bpp = 32;
    fb->rpos = 16; fb->gpos = 8; fb->bpos = 0;
    fb->handle = (u32)at;                                /* (the window is below 4 GiB of VRAM) */
    return 0;
}

static void nvd_fb_free(display_adapter_t *a, display_fb_t *fb) {
    nvd_t *n = a->priv;
    if (fb->px) nv_evo_vram_free(&n->vram, fb->handle);
}

/* ---------------- the output resource for an output ---------------- */

static int or_used_by_other(nvd_t *n, int head, int type, int id) {
    for (int h = 0; h < EVO_HEADS; h++)
        if (h != head && n->head[h].on && n->head[h].or_type == type && n->head[h].or_id == id) return 1;
    return 0;
}

/* nvkm_outp_acquire: the OR already driving it, else a free one */
static int pick_or(nvd_t *n, int k, int head, int *type) {
    const nv_dcb_output_t *o = dcb(n, k);
    nv_evo_t *e = &n->e;
    if (!o) return -1;
    if (o->type == NV_OUT_CRT) {
        *type = 1;
        int id = o->or_mask ? __builtin_ctz(o->or_mask) : -1;
        return id >= 0 && id < e->ndacs && (e->dac_mask & (1u << id)) && !or_used_by_other(n, head, 1, id) ? id : -1;
    }
    *type = 0;
    if (!n->g->route) {
        int id = o->or_mask ? __builtin_ctz(o->or_mask) : -1;
        return id >= 0 && id < e->nsors && (e->sor_mask & (1u << id)) && !or_used_by_other(n, head, 0, id) ? id : -1;
    }
    for (int i = 0; i < e->nsors; i++)
        if ((e->sor_mask & (1u << i)) && e->sor_outp[i] == k && !or_used_by_other(n, head, 0, i)) return i;
    for (int i = 0; i < e->nsors; i++)
        if ((e->sor_mask & (1u << i)) && e->sor_outp[i] < 0 && !e->sor_arm[i].heads && !or_used_by_other(n, head, 0, i)) return i;
    for (int i = 0; i < e->nsors; i++)
        if ((e->sor_mask & (1u << i)) && !or_used_by_other(n, head, 0, i)) return i;
    return -1;
}

static u32 or_heads_of(nvd_t *n, int type, int id, int except) {
    u32 m = 0;
    for (int h = 0; h < EVO_HEADS; h++)
        if (h != except && n->head[h].on && n->head[h].or_type == type && n->head[h].or_id == id) m |= 1u << h;
    return m;
}

/* the firmware's head and OR, at the first mode set, unless we take them over */
static void fw_off(nvd_t *n, nv_evo_stream_t *s, int head, int or_type, int or_id) {
    nv_evo_t *e = &n->e;
    if (n->first_set_done || !e->fw.valid) return;
    nv_evo_head_cfg_t c;
    if (e->fw.head != head && !n->head[e->fw.head].on) {
        memset(&c, 0, sizeof c);
        c.head = e->fw.head;
        c.or_id = -1;
        nv_evo_head_methods(n->g, s, &c);
        nvlog(n, "the firmware's head %d goes off", e->fw.head);
    }
    if (e->fw.or_type != or_type || e->fw.or_id != or_id) {
        nv_evo_or_off(n->g, s, e->fw.or_type, e->fw.or_id);
        nvlog(n, "the firmware's %s%d is detached", e->fw.or_type ? "DAC" : "SOR", e->fw.or_id);
    }
}

/* ---------------- set_mode ---------------- */

static int head_off(nvd_t *n, int head) {
    nv_evo_t *e = &n->e;
    nv_evo_stream_t s;
    memset(&s, 0, sizeof s);
    nv_evo_head_cfg_t c;
    memset(&c, 0, sizeof c);
    c.head = head;
    c.or_id = n->head[head].on ? n->head[head].or_id : -1;
    c.or_type = n->head[head].or_type;
    if (c.or_id >= 0) c.or_heads = or_heads_of(n, c.or_type, c.or_id, head);
    nv_evo_head_methods(n->g, &s, &c);
    nv_evo_update(n->g, &s, 1);
    if (c.or_id >= 0 && !c.or_heads) {
        if (c.or_type) e->dac_asy_outp[c.or_id] = -1;
        else e->sor_asy_outp[c.or_id] = -1;
    }
    int r = nv_evo_submit(e, &s, 1);
    n->head[head].on = 0;
    nvlog(n, "head %d off: %s", head, r ? e->why : "done");
    return r;
}

static int nvd_set_mode(display_head_t *dh, display_connector_t *dc, const display_mode_t *m, display_fb_t *fb) {
    display_adapter_t *a = dh->adapter;
    nvd_t *n = a->priv;
    nv_evo_t *e = &n->e;
    int head = n->hw_head[dh->index];
    if (!n->taken) return E_IO;
    if (!m) return head_off(n, head) ? E_IO : 0;
    nvd_conn_t *c = &n->conn[dc->index];
    int k = c->sel;
    const nv_dcb_output_t *o = dcb(n, k);
    if (!o || !fb || !m->t.clock_khz) { snprintf(a->why, sizeof a->why, "%s: no output or timing", dc->name); return E_INVAL; }
    int or_type = 0, or_id = pick_or(n, k, head, &or_type);
    if (or_id < 0) { snprintf(a->why, sizeof a->why, "%s: no free output resource", dc->name); nvlog(n, "%s", a->why); return E_NOSPC; }

    nv_evo_timing_t t = { m->t.clock_khz, m->t.hactive, m->t.hsync_start, m->t.hsync_end, m->t.htotal,
                          m->t.vactive, m->t.vsync_start, m->t.vsync_end, m->t.vtotal,
                          (u8)!(m->flags & DT_HSYNC_POS), (u8)!(m->flags & DT_VSYNC_POS) };
    nv_evo_head_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    if (nv_evo_raster(&t, &cfg.r)) { snprintf(a->why, sizeof a->why, "%s: timing out of range", dc->name); return E_INVAL; }
    cfg.head = head;
    cfg.on = 1;
    cfg.w = (u16)m->w;
    cfg.h = (u16)m->h;
    cfg.offset = fb->handle;
    cfg.pitch = fb->pitch;
    cfg.vram_handle = EVO_HANDLE_VRAM;
    cfg.or_type = or_type;
    cfg.or_id = or_id;
    cfg.or_heads = or_heads_of(n, or_type, or_id, head) | (1u << head);
    int dual = 0;
    switch (o->type) {
    case NV_OUT_CRT: cfg.proto = EVO_PROTO_CRT; cfg.depth = EVO_DEPTH_DEFAULT; break;
    case NV_OUT_DP: cfg.proto = (o->link & 1) ? EVO_PROTO_DP_A : EVO_PROTO_DP_B; cfg.depth = EVO_DEPTH_24; break;
    default:
        dual = (o->link & 1) && m->t.clock_khz >= 165000 && dual_link_ok(n, o) && !c->hdmi;
        cfg.proto = dual ? EVO_PROTO_DUAL_TMDS : (o->link & 1) ? EVO_PROTO_TMDS_A : EVO_PROTO_TMDS_B;
        cfg.depth = EVO_DEPTH_DEFAULT;
        break;
    }
    char md[128];
    mode_describe(m, md, sizeof md);
    nvlog(n, "%s: %s on head %d, %s%d %s%s, surface %llx pitch %u", dc->name, md, head, or_type ? "DAC" : "SOR", or_id,
          nv_out_type_str(o->type), dual ? " dual link" : "", (unsigned long long)cfg.offset, cfg.pitch);

    nv_evo_stream_t s;
    memset(&s, 0, sizeof s);
    fw_off(n, &s, head, or_type, or_id);
    /* the head's previous OR, if it changes */
    if (n->head[head].on && (n->head[head].or_type != or_type || n->head[head].or_id != or_id)) {
        int pt = n->head[head].or_type, pid = n->head[head].or_id;
        u32 rest = or_heads_of(n, pt, pid, head);
        if (!rest) { nv_evo_or_off(n->g, &s, pt, pid); if (pt) e->dac_asy_outp[pid] = -1; else e->sor_asy_outp[pid] = -1; }
    }
    nv_evo_head_methods(n->g, &s, &cfg);
    nv_evo_update(n->g, &s, 1);
    if (or_type) e->dac_asy_outp[or_id] = k;
    else e->sor_asy_outp[or_id] = k;
    if (o->type == NV_OUT_DP) {
        n->dp[or_id] = c->dp;
        nv_pad_hold_aux(n->d, o->i2c);
        e->dp[or_id] = &n->dp[or_id];
    }
    if (!n->first_set_done) display_guard_checkpoint(a, "display driver: first mode set");
    u32 errs = e->errors;
    int r = nv_evo_submit(e, &s, 1);
    e->dp[or_id] = NULL;
    if (!r && o->type == NV_OUT_DP && !n->dp[or_id].trained) {
        snprintf(e->why, sizeof e->why, "DisplayPort: %s", n->dp[or_id].why[0] ? n->dp[or_id].why : "the link did not train");
        r = E_IO;
    }
    if (!r && e->errors != errs) r = E_IO;
    if (r) {
        snprintf(a->why, sizeof a->why, "%s: %s", dc->name, e->why);
        nvlog(n, "mode set failed: %s", a->why);
        return E_IO;
    }
    n->first_set_done = 1;
    n->head[head].on = 1;
    n->head[head].dcb = k;
    n->head[head].or_type = or_type;
    n->head[head].or_id = or_id;
    nvlog(n, "%s: on (%u supervisor rounds so far, %u script opcodes, %u writes refused)", dc->name, e->supers, e->script_ops,
          e->script_refused);
    return 0;
}

/* ---------------- enable: the takeover ---------------- */

/* writes through BAR1 must read back through PRAMIN (VRAM directly) at the
 * same offset: BAR1 maps VRAM linearly from 0, as the firmware left it */
static int pramin_check(nvd_t *n, u64 off, int write) {
    nv_device_t *d = n->d;
    volatile u32 *p = (volatile u32 *)(n->bar1 + off);
    u32 want[4];
    for (int i = 0; i < 4; i++) {
        want[i] = write ? (0x4E564842u ^ (u32)(off >> 12) ^ (u32)i * 0x01010101u) : p[i];
        if (write) p[i] = want[i];
    }
    nv_wr32(d, d->chip.chipset == 0x50 ? 0x00330c : 0x070000, 1);
    for (u64 end = time_us() + 2000; (nv_rd32(d, d->chip.chipset == 0x50 ? 0x00330c : 0x070000) & 2) && time_us() < end;) cpu_relax();
    u32 save = nv_rd32(d, 0x001700);
    nv_wr32(d, 0x001700, (u32)(off >> 16));
    int ok = nv_rd32(d, 0x001700) == (u32)(off >> 16);
    for (int i = 0; i < 4 && ok; i++)
        if (nv_rd32(d, 0x700000 + (u32)(off & 0xFFFF) + (u32)i * 4) != want[i]) ok = 0;
    nv_wr32(d, 0x001700, save);
    return ok;
}

static void layout(nvd_t *n) {
    nv_device_t *d = n->d;
    nv_evo_vram_init(&n->vram, 0, n->window);
    nv_evo_vram_hole(&n->vram, 0, 2 * MiB, "start of VRAM");
    if (n->fw_ok) {
        u64 a = ALIGN_DOWN(n->fw_off, 2 * MiB), b = ALIGN_UP(n->fw_off + (u64)n->fw_pitch * n->fw_h, 2 * MiB);
        nv_evo_vram_hole(&n->vram, a, b - a, "firmware frame buffer");
    }
    u32 v = nv_rd32(d, 0x619f04);
    if (!nv_bad_read(v) && (v & 8) && (v & 3) == 1) nv_evo_vram_hole(&n->vram, ALIGN_DOWN((u64)(v & ~0xFFu) << 8, MiB), 2 * MiB, "VBIOS shadow");
    v = nv_rd32(d, 0x610010);
    if (!nv_bad_read(v) && (v & ~0xFFu)) nv_evo_vram_hole(&n->vram, ALIGN_DOWN((u64)(v & ~0xFFu) << 8, MiB), 2 * MiB, "firmware's display objects");
    if (d->chip.family >= NV_FAM_FERMI) {
        u32 b1 = nv_rd32(d, 0x001704), b2 = nv_rd32(d, 0x001714);
        if (!nv_bad_read(b1) && (b1 & 0x80000000u)) nv_evo_vram_hole(&n->vram, ALIGN_DOWN((u64)(b1 & 0x0FFFFFFF) << 12, MiB), 2 * MiB, "BAR1 page tables");
        if (!nv_bad_read(b2) && (b2 & 0x80000000u)) nv_evo_vram_hole(&n->vram, ALIGN_DOWN((u64)(b2 & 0x0FFFFFFF) << 12, MiB), 2 * MiB, "BAR2 page tables");
    }
    if (d->vram && d->vram <= n->window + 16 * MiB) nv_evo_vram_hole(&n->vram, d->vram - 16 * MiB, 16 * MiB, "end of VRAM");
}

static int nvd_enable(display_adapter_t *a) {
    nvd_t *n = a->priv;
    nv_device_t *d = n->d;
    a->why[0] = 0;
    if (d->fake) return fail(n, E_NODEV, "simulated card (QEMU test hook): no registers, native modesetting not attempted");
    if (!d->mmio || !d->bios) return fail(n, E_NODEV, "no registers or no video BIOS");
    if (d->chip.family >= NV_FAM_FERMI) {
        u32 fuse = nv_rd32(d, d->chip.chipset >= 0x110 ? 0x021C04 : 0x022500);
        if (!nv_bad_read(fuse) && (fuse & 1)) return fail(n, E_NODEV, "the display engine is fused off");
    }
    pci_bar_t *b1 = &d->pci->bar[1];
    if (b1->is_io || !b1->addr || b1->size < 64 * MiB) return fail(n, E_NODEV, "no usable frame buffer BAR (BAR1: %llu MiB)", (unsigned long long)(b1->size >> 20));
    n->bar1 = pci_map_bar(d->pci, 1);
    if (!n->bar1) return fail(n, E_NODEV, "BAR1 at %llx cannot be mapped", (unsigned long long)b1->addr);
    n->bar1_phys = b1->addr;
    n->bar1_size = b1->size;
    n->window = d->vram && d->vram < b1->size ? d->vram : b1->size;
    if (n->window > 4096 * MiB) n->window = 4096 * MiB;
    /* the firmware frame buffer */
    u64 fb = g_bootinfo.fb_addr;
    n->fw_w = g_bootinfo.fb_width; n->fw_h = g_bootinfo.fb_height; n->fw_pitch = g_bootinfo.fb_pitch;
    n->fw_ok = fb >= b1->addr && fb + (u64)n->fw_pitch * n->fw_h <= b1->addr + n->window && g_bootinfo.fb_bpp == 32;
    n->fw_off = n->fw_ok ? fb - b1->addr : 0;
    nvlog(n, "BAR1 %llx, %llu MiB (window %llu MiB of %llu MiB VRAM); firmware frame buffer %s at VRAM %llx, %ux%u, pitch %u",
          (unsigned long long)b1->addr, (unsigned long long)(b1->size >> 20), (unsigned long long)(n->window >> 20),
          (unsigned long long)(d->vram >> 20), n->fw_ok ? "in BAR1" : "not in BAR1 (or not 32 bpp)", (unsigned long long)n->fw_off,
          n->fw_w, n->fw_h, n->fw_pitch);
    nvlog(n, "BAR1 mode %08x (0x001704), BAR2 %08x, PRAMIN %08x, PMC_ENABLE %08x, interrupts %08x", nv_rd32(d, 0x001704),
          nv_rd32(d, 0x001714), nv_rd32(d, 0x001700), nv_rd32(d, 0x000200), nv_rd32(d, 0x000140));
    layout(n);
    u64 ctrl = nv_evo_vram_alloc(&n->vram, 2 * MiB, 2 * MiB);
    if (ctrl == ~0ull) return fail(n, E_NOMEM, "no free VRAM inside BAR1 for the display objects");
    for (int i = 0; i < n->vram.nholes; i++)
        nvlog(n, "VRAM kept: %llx-%llx %s", (unsigned long long)n->vram.hole[i].at,
              (unsigned long long)(n->vram.hole[i].at + n->vram.hole[i].size - 1), n->vram.hole[i].what);
    /* BAR1 must map VRAM 1:1 (checked through PRAMIN, a write and a read only) */
    if (!pramin_check(n, ctrl, 1) || !pramin_check(n, ctrl + MiB, 1) || (n->fw_ok && !pramin_check(n, n->fw_off, 0)))
        return fail(n, E_NOTSUP, "BAR1 does not map VRAM linearly (0x001704 = %08x)", nv_rd32(d, 0x001704));
    nvlog(n, "BAR1 maps VRAM linearly (checked through PRAMIN)");

    /* from here the hardware changes: the start guard first */
    int r = display_guard_begin(a);
    if (r) return fail(n, r, "the start marker cannot be saved in SETTINGS.INI (%s): not starting", strerror(r));
    n->armed = 1;
    /* no interrupts from the GPU while the driver owns the display */
    n->pci_cmd = pci_read16(d->pci, 0x04);
    n->pci_cmd_saved = 1;
    pci_write16(d->pci, 0x04, n->pci_cmd | 0x0400);

    nv_evo_t *e = &n->e;
    memset(e, 0, sizeof *e);
    e->g = n->g;
    e->r = &d->regs;
    e->bios = d->bios;
    e->chipset = d->chip.chipset;
    e->ops = (nv_evo_ops_t){ n, evo_i2c, evo_aux, evo_log, evo_rd8, evo_wr8 };
    e->vram = n->bar1;
    e->vram_base = 0;
    e->vram_size = n->window;
    e->vram_total = d->vram ? d->vram : n->window;
    e->scripts = settings_get_int("nv.scripts", 1);
    e->inst = ctrl;
    e->push = ctrl + EVO_INST_BYTES;
    e->sync = ctrl + EVO_INST_BYTES + EVO_PUSH_BYTES;
    r = nv_evo_takeover(e);
    if (r) {
        snprintf(a->why, sizeof a->why, "takeover: %s", e->why);
        return E_IO;
    }
    n->taken = 1;
    for (int h = 0; h < EVO_HEADS; h++) n->head[h].on = 0;
    if (e->fw.valid && !n->fw_ok) nvlog(n, "the firmware's surface is not in BAR1: restore can only switch the heads off");
    if (e->fw.valid && n->fw_ok && ((u64)e->fw.hs.offset << 8) != n->fw_off)
        nvlog(n, "note: the firmware head scans out %llx, its frame buffer is at %llx", (unsigned long long)e->fw.hs.offset << 8,
              (unsigned long long)n->fw_off);
    snprintf(n->state, sizeof n->state, "native (%s display engine), started", n->g->name);
    return 0;
}

/* ---------------- restore ---------------- */

static void nvd_restore(display_adapter_t *a) {
    nvd_t *n = a->priv;
    nv_evo_t *e = &n->e;
    if (!n->armed) return;
    nvlog(n, "giving the display back to the firmware frame buffer");
    if (n->taken && e->chan_up) {
        nv_evo_stream_t s;
        memset(&s, 0, sizeof s);
        nv_evo_head_cfg_t c;
        int fw = n->fw_ok && !nv_evo_fw_cfg(e, n->fw_off, n->fw_pitch, (int)n->fw_w, (int)n->fw_h, &c);
        for (int h = 0; h < EVO_HEADS; h++) {
            if (!n->head[h].on) continue;
            nv_evo_head_cfg_t off;
            memset(&off, 0, sizeof off);
            off.head = h;
            off.or_type = n->head[h].or_type;
            off.or_id = fw && n->head[h].or_type == c.or_type && n->head[h].or_id == c.or_id ? -1 : n->head[h].or_id;
            nv_evo_head_methods(n->g, &s, &off);
            if (off.or_id >= 0) { if (off.or_type) e->dac_asy_outp[off.or_id] = -1; else e->sor_asy_outp[off.or_id] = -1; }
            n->head[h].on = 0;
        }
        if (fw) {
            nv_evo_head_methods(n->g, &s, &c);
            if (c.or_type) e->dac_asy_outp[c.or_id] = e->fw.dcb;
            else e->sor_asy_outp[c.or_id] = e->fw.dcb;
            if (!c.or_type && e->fw.os.proto == NV_OUT_DP && e->fw.dcb >= 0) {
                /* the firmware's DP link: train it again */
                const nv_dcb_output_t *o = dcb(n, e->fw.dcb);
                memset(&n->dp[c.or_id], 0, sizeof n->dp[0]);
                n->dp[c.or_id].dcb = e->fw.dcb;
                n->dp[c.or_id].ccb = o ? o->i2c : 0;
                if (o && !nv_dp_detect(e, &n->dp[c.or_id])) e->dp[c.or_id] = &n->dp[c.or_id];
            }
        }
        nv_evo_update(n->g, &s, 1);
        int r = nv_evo_submit(e, &s, 1);
        if (!fw) nvlog(n, "restore: the firmware's mode cannot be programmed again (%s); heads switched off: %s",
                       !e->fw.valid ? "no firmware head was found" : "its frame buffer is not in BAR1", r ? e->why : "done");
        else nvlog(n, "restore: firmware mode on head %d (%s%d), %ux%u from VRAM %llx: %s", c.head, c.or_type ? "DAC" : "SOR", c.or_id,
                   n->fw_w, n->fw_h, (unsigned long long)n->fw_off, r ? e->why : "done");
        for (int i = 0; i < EVO_ORS; i++) e->dp[i] = NULL;
    } else {
        nvlog(n, "restore: the core channel is not running; the registers are put back only");
    }
    nv_evo_release(e);
    /* (INTx stays off: the GPU's interrupts stay masked, nobody would answer them) */
    n->taken = 0;
    snprintf(n->state, sizeof n->state, "firmware frame buffer (the native driver failed: %.60s)", a->why[0] ? a->why : "see BOOT.LOG");
}

/* ---------------- poll: hot plug ---------------- */

static void nvd_poll(display_adapter_t *a) {
    nvd_t *n = a->priv;
    if (!n->taken || time_ms() < n->next_hpd) return;
    n->next_hpd = time_ms() + 1000;
    for (int k = 0; k < n->nconn; k++) {
        int h = nv_hotplug(n->d, n->d->conn[n->conn[k].nvc].index);
        if (h >= 0 && n->conn[k].hpd >= 0 && h != n->conn[k].hpd) {
            nvlog(n, "%s: hot plug %s", a->conns[k].name, h ? "on" : "off");
            display_hotplug(a);
        }
        if (h >= 0) n->conn[k].hpd = h;
    }
}

static const display_ops_t nvd_ops = {
    .enable = nvd_enable,
    .restore = nvd_restore,
    .detect = nvd_detect,
    .mode_valid = nvd_mode_valid,
    .fb_alloc = nvd_fb_alloc,
    .fb_free = nvd_fb_free,
    .set_mode = nvd_set_mode,
    .poll = nvd_poll,
};

/* ---------------- registration (from nv_init) ---------------- */

static void probe_one(nv_device_t *d) {
    if (nvd_of(d)) return;
    if (!d->chip.in_range) return;
    const nv_evo_gen_t *g = nv_evo_gen_for(d->chip.chipset);
    if (!g) {
        kprintf("nv%d: native modesetting: not for %s (%s); the firmware frame buffer is used\n", d->index, d->chip.codename,
                d->chip.disp == NV_DISP_NV04 ? "VGA-style CRTCs: M10c" : "no EVO display engine");
        return;
    }
    if (g_bootinfo.boot_flags & (BOOTF_SAFE_VIDEO | BOOTF_NO_DISPLAY)) {
        kprintf("nv%d: native modesetting off (boot option)\n", d->index);
        return;
    }
    if (!settings_get_int("nv.modeset", 1)) {
        kprintf("nv%d: native modesetting off (nv.modeset=0 in SETTINGS.INI): the firmware frame buffer is used\n", d->index);
        return;
    }
    if (!d->bios || !d->bios->noutputs) {
        kprintf("nv%d: native modesetting not possible: no video BIOS output table (DCB)\n", d->index);
        return;
    }
    int slot = -1;
    for (int i = 0; i < NV_MAX_GPUS; i++) if (!nvds[i]) { slot = i; break; }
    if (slot < 0) return;
    nvd_t *n = kzalloc(sizeof *n);
    if (!n) return;
    n->d = d;
    n->g = g;
    /* heads: the capability registers (read only), else the generation's count */
    u32 hmask = 0, nh = 0;
    if (!d->fake && g->gf119) {
        u32 caps = nv_rd32(d, 0x612004), cnt = nv_rd32(d, 0x022448);
        if (!nv_bad_read(caps) && !nv_bad_read(cnt)) { hmask = caps & 0xF; nh = MIN(cnt, 4u); }
    }
    if (!hmask) { hmask = (1u << g->heads) - 1; nh = g->heads; }
    display_adapter_t *a = display_register("nv", d->name, &nvd_ops, n, d->pci);
    if (!a) { kfree(n); return; }
    nvds[slot] = n;
    n->a = a;
    a->live = 1;
    a->max_w = g->max_w;
    a->max_h = g->max_h;
    a->pitch_align = 256;
    a->guarded = !d->fake;
    u64 win = d->bar1_size && d->vram ? MIN(d->bar1_size, d->vram) : d->bar1_size ? d->bar1_size : 256 * MiB;
    a->vram = win > 64 * MiB ? (win - 32 * MiB) / 2 : 0;
    int nheads = 0;
    for (u32 h = 0; h < nh && h < EVO_HEADS && nheads < DISPLAY_MAX_HEADS; h++) {
        if (!(hmask & (1u << h))) continue;
        if (!display_add_head(a, 0, 0)) break;
        n->hw_head[nheads++] = (int)h;
    }
    int count[16] = { 0 };
    for (int i = 0; i < d->nconn && n->nconn < DISPLAY_MAX_CONNECTORS; i++) {
        const char *nm;
        int type = conn_type(d->conn[i].type, &nm);
        if (!nm) continue;                               /* TV, panels, unknown: not driven */
        nvd_conn_t *c = &n->conn[n->nconn];
        memset(c, 0, sizeof *c);
        c->nvc = i;
        c->type = type;
        c->sel = -1;
        c->hpd = d->conn[i].hpd;
        u32 heads = 0;
        for (int k = 0; k < d->conn[i].nout && c->nout < 6; k++) {
            const nv_dcb_output_t *o = nv_output(d, d->conn[i].out[k]);
            if (!o || o->location || (o->type != NV_OUT_TMDS && o->type != NV_OUT_DP && o->type != NV_OUT_CRT)) continue;
            c->out[c->nout++] = d->conn[i].out[k];
            for (int dh = 0; dh < nheads; dh++) if (o->heads & (1u << n->hw_head[dh])) heads |= 1u << dh;
        }
        if (!c->nout) continue;
        char name[16];
        snprintf(name, sizeof name, "%s-%d", nm, ++count[type & 15]);
        if (!display_add_connector(a, name, type, heads ? heads : (1u << nheads) - 1)) break;
        n->nconn++;
    }
    snprintf(n->state, sizeof n->state, d->fake ? "native (simulated card: not started)" : "native (%s display engine), not started yet",
             g->name);
    nvlog(n, "native modesetting: %s display engine (core class %04x), %d head%s, %d connector%s, %s", g->name, g->core_class, nheads,
          nheads == 1 ? "" : "s", n->nconn, n->nconn == 1 ? "" : "s", d->fake ? "simulated card" : "starts with the desktop");
    for (int k = 0; k < n->nconn; k++) {
        char outs[48] = "";
        for (int i = 0; i < n->conn[k].nout; i++) {
            const nv_dcb_output_t *o = nv_output(d, n->conn[k].out[i]);
            size_t l = strlen(outs);
            snprintf(outs + l, sizeof outs - l, "%s%d %s", i ? ", " : "", n->conn[k].out[i], o ? nv_out_type_str(o->type) : "?");
        }
        nvlog(n, "  %s: DCB connector %d (%s), outputs %s, heads %x", a->conns[k].name, d->conn[n->conn[k].nvc].index,
              nv_conn_type_str(d->conn[n->conn[k].nvc].type), outs, a->conns[k].possible_heads);
    }
}

void nv_display_probe(void) {
    for (int i = 0; i < nv_count(); i++) probe_one(nv_get(i));
}

const char *nv_display_state(nv_device_t *d, char *buf, int n) {
    nvd_t *x = nvd_of(d);
    if (!x) {
        const nv_evo_gen_t *g = nv_evo_gen_for(d->chip.chipset);
        if (!g) snprintf(buf, (size_t)n, "firmware frame buffer (no native modesetting for this generation)");
        else if (g_bootinfo.boot_flags & (BOOTF_SAFE_VIDEO | BOOTF_NO_DISPLAY)) snprintf(buf, (size_t)n, "firmware frame buffer (Safe Mode)");
        else if (!settings_get_int("nv.modeset", 1)) snprintf(buf, (size_t)n, "firmware frame buffer (nv.modeset=0)");
        else snprintf(buf, (size_t)n, "firmware frame buffer (native modesetting not registered)");
        return buf;
    }
    display_adapter_t *a = x->a;
    switch (a->state) {
    case ADAPTER_ACTIVE: {
        char outs[48] = "";
        for (int k = 0; k < a->nconns; k++)
            if (a->conns[k].head) { if (outs[0]) strlcat(outs, ", ", sizeof outs); strlcat(outs, a->conns[k].name, sizeof outs); }
        snprintf(buf, (size_t)n, "native (%s display engine), in use on %s", x->g->name, outs[0] ? outs : "no output");
        break;
    }
    case ADAPTER_DISABLED: case ADAPTER_FAILED:
        snprintf(buf, (size_t)n, "firmware frame buffer; native driver %s: %s", a->state == ADAPTER_DISABLED ? "off" : "failed", a->why);
        break;
    default:
        strlcpy(buf, x->state, (size_t)n);
        break;
    }
    return buf;
}

const char *nv_display_log(nv_device_t *d) {
    nvd_t *x = nvd_of(d);
    return x && x->log ? x->log : NULL;
}
