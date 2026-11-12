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
 * 10d adds, per head: page flips through the base channel (completed when
 * its notifier says so, polled; a flip that does not complete in four
 * frames, or a channel that does not start, ends flips on that head: the
 * core layer shows the buffer, single buffered as in 10b), the hardware
 * cursor (images in VRAM, enabled in the core channel, positions through
 * the cursor channel; any failure: the software cursor), the vertical blank
 * from the scanline counter, and several heads lit at once, each with its
 * own output resource (evochan.c: nv_evo_pick_or) and video PLL. No
 * interrupt path: flips are found by poll, woken at the expected vertical
 * blank (idle_wake_at). See docs/DESIGN-M10-NVIDIA.md, "10d".
 *
 * Switches: nv.disable=1 and F8 Safe Mode (nothing of M10), nv.modeset=0
 * (identification only), nv.scripts=0 (no VBIOS display scripts),
 * nv.flip=0 (single buffered), nv.hwcursor=0 (software cursor), nv.heads=N
 * (at most N heads lit), display.disable=nv (the display core), the start
 * guard (display.nv.guard). */
#include "nv.h"
#include "evo.h"
#include "accel.h"
#include "drivers/display.h"
#include "core/lib.h"
#include "core/io.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/irq.h"
#include "core/settings.h"

#define MiB (1ull << 20)
#define NVD_LOG 32768
/* inside the 2 MiB block of our objects: the instance memory (64 KiB), the
 * core push buffer, the notifiers, the base push buffers, the cursor images */
#define CTL_PUSH    0x010000
#define CTL_SYNC    0x011000
#define CTL_BASE    0x012000                         /* + head * 4 KiB */
#define CTL_CURS    0x020000                         /* + head * 32 KiB + slot * 16 KiB */

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

/* a hardware head */
typedef struct nvd_head {
    int on, dcb, or_type, or_id;
    u32 khz;
    /* the core layer's surface (set_mode) */
    u64 core_off;
    int fb_w, fb_h;
    u32 fb_pitch;
    u32 frame_ms;
    /* 10d: page flips through the base channel */
    int flips;                  /* the base channel runs for this head */
    display_fb_t *pend;         /* the buffer of the queued flip */
    u64 flip_ms;
    u64 vis_off;                /* the buffer on screen (base layer, else the core layer) */
    u32 flips_done;
    /* 10d: the hardware cursor */
    int curs;                   /* the cursor channel runs for this head */
    int curs_on;                /* enabled in the core channel */
    int curs_slot;              /* the image the core channel points at */
    int curs_hx, curs_hy, curs_x, curs_y;
    u32 curs_sum[2];            /* each slot's image (checksum; 0: empty) */
    int curs_busy_run;          /* moves dropped in a row */
    u32 curs_updates;
    int vbl_fail;
} nvd_head_t;

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
    nvd_head_t head[EVO_HEADS];
    int want_flip, want_curs;   /* nv.flip, nv.hwcursor */
    int heads_avail, heads_used;      /* the engine's heads, the ones registered (nv.heads) */
    char flip_why[96], curs_why[96];  /* why page flips / the hardware cursor are off */
    nv_evo_dp_t dp[EVO_ORS];
    u16 pci_cmd;
    int pci_cmd_saved;
    int first_set_done;
    u64 next_hpd;
    nv_accel_t *acc;            /* 10e: the copy engine (accel.c) */
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
static void evo_wmb(void *ctx) { (void)ctx; __asm__ volatile("sfence" ::: "memory"); }
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

/* Every timing of the mode list (each refresh rate separately) at its exact
 * pixel clock: the head's limit, then the output's. DisplayPort: the most
 * the link can carry, the lanes and rate both ends support (the DPCD's,
 * capped by the DCB and the generation, read at detect); a mode set
 * retrains the link for the mode's clock, widest and fastest first
 * (nv_dp_configs), so a timing that fits here trains unless the cable
 * cannot carry it, and then the mode set fails and the core goes back. */
static int nvd_mode_valid(display_adapter_t *a, display_connector_t *dc, const display_mode_t *m) {
    nvd_t *n = a->priv;
    nvd_conn_t *c = &n->conn[dc->index];
    const nv_dcb_output_t *o = dcb(n, c->sel);
    if (!o || (m->flags & DT_INTERLACE) || !m->t.clock_khz || m->t.clock_khz < 25000) return 0;
    if (m->w > n->g->max_w || m->h > n->g->max_h) return 0;
    if (m->t.clock_khz > nv_evo_head_max_khz(n->g)) return 0;
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

/* what every hardware head drives now (nv_evo_pick_or, nv_evo_bw_check) */
static void use_of(nvd_t *n, nv_evo_use_t use[EVO_HEADS]) {
    for (int h = 0; h < EVO_HEADS; h++) {
        use[h].on = (u8)n->head[h].on;
        use[h].dcb = n->head[h].dcb;
        use[h].or_type = n->head[h].or_type;
        use[h].or_id = n->head[h].or_id;
        use[h].khz = n->head[h].khz;
    }
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

/* ---------------- 10d: flips and the cursor given up ---------------- */

/* a core channel update of its own (cursor, core layer); 0 or E_* */
static int core_update(nvd_t *n, nv_evo_stream_t *s) {
    nv_evo_update(n->g, s, 1);
    return nv_evo_submit(&n->e, s, 1);
}

/* point a lit head's core layer at a buffer (the fallback from flips) */
static int core_layer(nvd_t *n, int head, u64 off) {
    nvd_head_t *hd = &n->head[head];
    if (!hd->on || hd->core_off == off) return 0;
    nv_evo_stream_t s;
    memset(&s, 0, sizeof s);
    nv_evo_core_layer_methods(n->g, &s, head, off, hd->fb_w, hd->fb_h, hd->fb_pitch, EVO_HANDLE_VRAM, 1, hd->curs_on,
                              nv_evo_curs_offset(&n->e, head, hd->curs_slot));
    int r = core_update(n, &s);
    if (!r) hd->core_off = off;
    return r;
}

/* page flips end on this head: the base layer off and its channel stopped,
 * the core layer shows `show` (a VRAM offset, ~0: leave it), the display
 * core single-buffers from now on (10b) */
static void flips_off(nvd_t *n, display_head_t *dh, int head, u64 show, const char *why) {
    nv_evo_t *e = &n->e;
    nvd_head_t *hd = &n->head[head];
    nvlog(n, "head %d: page flips given up (%s): single-buffered scanout from the core channel", head, why);
    snprintf(n->flip_why, sizeof n->flip_why, "head %d: %s", head, why);
    if (e->base[head].up && (e->base[head].shown || e->base[head].pending) && nv_evo_base_off(e, head, 50000))
        nvlog(n, "head %d: the base layer stays on (%s): the picture may stop following; nv.flip=0 avoids this", head, e->why);
    nv_evo_base_stop(e, head);
    hd->flips = 0;
    hd->pend = NULL;
    if (dh) dh->caps &= ~HEAD_CAP_FLIP;
    if (show != ~0ull && hd->on) {
        int r = core_layer(n, head, show);
        nvlog(n, "head %d: the core layer shows VRAM %llx: %s", head, (unsigned long long)show, r ? e->why : "done");
        hd->vis_off = show;
    }
}

/* the hardware cursor ends (the display core draws it in software); on
 * every head, since the display core then stops moving it on the others */
static void curs_off(nvd_t *n, display_head_t *dh, int head, const char *why) {
    (void)dh;
    nvlog(n, "head %d: hardware cursor given up (%s): the software cursor is used on every head", head, why);
    snprintf(n->curs_why, sizeof n->curs_why, "head %d: %s", head, why);
    for (int k = 0; k < n->a->nheads; k++) {
        int h = n->hw_head[k];
        nvd_head_t *hd = &n->head[h];
        if (hd->curs_on && hd->on) {
            nv_evo_stream_t s;
            memset(&s, 0, sizeof s);
            nv_evo_curs_methods(n->g, &s, h, 0, 0, 0);
            core_update(n, &s);
        }
        hd->curs_on = 0;
        nv_evo_curs_stop(&n->e, h);
        hd->curs = 0;
        n->a->heads[k].caps &= ~HEAD_CAP_CURSOR;
    }
}

static display_head_t *dhead_of(nvd_t *n, int head) {
    for (int k = 0; k < n->a->nheads; k++) if (n->hw_head[k] == head) return &n->a->heads[k];
    return NULL;
}

/* before the core channel changes a head (mode, off): the base layer off */
static void base_quiet(nvd_t *n, int head) {
    nvd_head_t *hd = &n->head[head];
    nv_evo_t *e = &n->e;
    hd->pend = NULL;
    if (!hd->flips || !(e->base[head].shown || e->base[head].pending)) return;
    if (nv_evo_base_off(e, head, 100000)) flips_off(n, dhead_of(n, head), head, ~0ull, "the base layer did not go off for a mode set");
    else hd->vis_off = hd->core_off;
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
    n->head[head].curs_on = 0;
    n->head[head].khz = 0;
    nvlog(n, "head %d off: %s", head, r ? e->why : "done");
    return r;
}

static int nvd_set_mode(display_head_t *dh, display_connector_t *dc, const display_mode_t *m, display_fb_t *fb) {
    display_adapter_t *a = dh->adapter;
    nvd_t *n = a->priv;
    nv_evo_t *e = &n->e;
    int head = n->hw_head[dh->index];
    if (!n->taken) return E_IO;
    base_quiet(n, head);
    if (!m) return head_off(n, head) ? E_IO : 0;
    nvd_conn_t *c = &n->conn[dc->index];
    int k = c->sel;
    const nv_dcb_output_t *o = dcb(n, k);
    if (!o || !fb || !m->t.clock_khz) { snprintf(a->why, sizeof a->why, "%s: no output or timing", dc->name); return E_INVAL; }
    /* several heads: an output resource no other head uses (pad macros
     * shared between connectors), within the per-head and total pixel clock */
    nv_evo_use_t use[EVO_HEADS];
    use_of(n, use);
    char why[80];
    int or_type = 0, or_id = nv_evo_pick_or(e, use, k, head, &or_type, why, sizeof why);
    if (or_id < 0) {
        snprintf(a->why, sizeof a->why, "%s: no output resource: %s", dc->name, why);
        nvlog(n, "%s", a->why);
        return E_NOSPC;
    }
    if (nv_evo_bw_check(e, use, head, m->t.clock_khz, why, sizeof why)) {
        snprintf(a->why, sizeof a->why, "%s: %s", dc->name, why);
        nvlog(n, "%s", a->why);
        return E_NOSPC;
    }

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
    cfg.base = (u8)n->head[head].flips;             /* 10d: the base channel may show 32 bpp surfaces */
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
    nvd_head_t *hd = &n->head[head];
    hd->on = 1;
    hd->dcb = k;
    hd->or_type = or_type;
    hd->or_id = or_id;
    hd->khz = m->t.clock_khz;
    hd->core_off = hd->vis_off = fb->handle;
    hd->fb_w = m->w;
    hd->fb_h = m->h;
    hd->fb_pitch = fb->pitch;
    hd->frame_ms = m->hz > 0 ? (u32)(1000 / m->hz) + 1 : 17;
    hd->curs_on = 0;                                 /* (the head's methods switched it off) */
    nvlog(n, "%s: on (%u supervisor rounds so far, %u script opcodes, %u writes refused)%s", dc->name, e->supers, e->script_ops,
          e->script_refused, hd->flips ? "; page flips through base channel" : "");
    return 0;
}

/* ---------------- 10d: page flips ---------------- */

static int nvd_flip(display_head_t *dh, display_fb_t *fb) {
    nvd_t *n = dh->adapter->priv;
    int head = n->hw_head[dh->index];
    nvd_head_t *hd = &n->head[head];
    if (!n->taken || !hd->flips || !hd->on || hd->pend) return E_IO;
    int r = nv_evo_base_flip(&n->e, head, fb->handle, fb->w, fb->h, fb->pitch);
    if (r) {
        char w[64];
        snprintf(w, sizeof w, "a flip could not be queued (%s)", strerror(r));
        flips_off(n, dh, head, hd->vis_off, w);       /* (the buffer on screen stays: the core keeps its front) */
        return r;
    }
    hd->pend = fb;
    hd->flip_ms = time_ms();
    if (hd->flips_done == 0) nvlog(n, "head %d: first page flip queued (VRAM %llx)", head, (unsigned long long)fb->handle);
    /* no interrupt: be back for the vertical blank (polled) */
    idle_wake_at(time_ms() + nv_evo_us_to_vblank(&n->e, head) / 1000 + 1);
    return 0;
}

static int nvd_wait_vblank(display_head_t *dh) {
    nvd_t *n = dh->adapter->priv;
    int head = n->hw_head[dh->index];
    nvd_head_t *hd = &n->head[head];
    if (!n->taken || !hd->on) return E_IO;
    int r = nv_evo_wait_vblank(&n->e, head, 50000);
    if (!r) { hd->vbl_fail = 0; return 0; }
    if (++hd->vbl_fail == 3) {
        dh->caps &= ~HEAD_CAP_VBLANK;
        nvlog(n, "head %d: three vertical blank waits failed (%s): no vertical blank any more", head, strerror(r));
    }
    return r;
}

/* ---------------- 10d: the hardware cursor ---------------- */

static int curs_core(nvd_t *n, int head, int on) {
    nvd_head_t *hd = &n->head[head];
    nv_evo_stream_t s;
    memset(&s, 0, sizeof s);
    nv_evo_curs_methods(n->g, &s, head, on, nv_evo_curs_offset(&n->e, head, hd->curs_slot), EVO_HANDLE_VRAM);
    int r = core_update(n, &s);
    if (!r) { hd->curs_on = on; hd->curs_updates++; }
    return r;
}

static u32 curs_sum(const u32 *p, int size) {
    u32 h = 2166136261u ^ (u32)size;
    for (int i = 0; i < size * size; i++) h = (h ^ p[i]) * 16777619u;
    return h ? h : 1;
}

static int nvd_cursor_image(display_head_t *dh, const u32 *argb, int size, int hx, int hy) {
    nvd_t *n = dh->adapter->priv;
    int head = n->hw_head[dh->index];
    nvd_head_t *hd = &n->head[head];
    if (!n->taken || !hd->curs || !argb || size <= 0 || size > EVO_CURS_SIZE) return E_IO;
    u32 sum = curs_sum(argb, size);
    hd->curs_hx = hx;
    hd->curs_hy = hy;
    if (hd->curs_sum[hd->curs_slot] != sum) {
        /* the other slot while the cursor is shown (no tearing), else the current one */
        int slot = hd->curs_on ? !hd->curs_slot : hd->curs_slot;
        if (hd->curs_sum[slot] != sum) {
            int r = nv_evo_curs_upload(&n->e, head, slot, argb, size);
            if (r) { hd->curs_sum[slot] = 0; curs_off(n, dh, head, "the image does not reach VRAM"); return E_IO; }
            hd->curs_sum[slot] = sum;
        }
        int was = hd->curs_slot;
        hd->curs_slot = slot;
        if (hd->curs_on && curs_core(n, head, 1)) {
            hd->curs_slot = was;
            curs_off(n, dh, head, "the core channel did not take the new image");
            return E_IO;
        }
    }
    /* the hot spot may have changed: the image's corner moves */
    if (hd->curs_on) nv_evo_curs_move(&n->e, head, hd->curs_x - hx, hd->curs_y - hy);
    return 0;
}

static void nvd_cursor_move(display_head_t *dh, int x, int y, int visible) {
    nvd_t *n = dh->adapter->priv;
    int head = n->hw_head[dh->index];
    nvd_head_t *hd = &n->head[head];
    if (!n->taken || !hd->curs || !hd->on) return;
    hd->curs_x = x;
    hd->curs_y = y;
    if (!visible) {
        if (hd->curs_on && curs_core(n, head, 0)) curs_off(n, dh, head, "the core channel did not hide it");
        return;
    }
    /* the position of the image's top left (the hot spot subtracted; may be
     * negative: the hardware clips), then shown if it was not */
    int r = nv_evo_curs_move(&n->e, head, x - hd->curs_hx, y - hd->curs_hy);
    if (r == E_BUSY) {
        if (++hd->curs_busy_run >= 100) { curs_off(n, dh, head, "the cursor channel took no position for 100 moves"); return; }
    } else if (r) {
        curs_off(n, dh, head, "the cursor channel's registers are unreadable");
        return;
    } else {
        hd->curs_busy_run = 0;
    }
    if (!hd->curs_on && curs_core(n, head, 1)) curs_off(n, dh, head, "the core channel did not show it");
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
    __asm__ volatile("sfence" ::: "memory");
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

/* 10e: BAR1 maps this VRAM offset 1:1 (a pattern written and read back through PRAMIN) */
static int vram_linear(void *ctx, u64 off) { return pramin_check(ctx, off, 1); }

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

/* the simulated card: a monitor on every connector at once (the first
 * output of each, in connector order), as nv_evo_plan would place them on
 * a card with the generation's resources (no hardware read) */
static void dry_plan(nvd_t *n) {
    static nv_evo_t pe;
    memset(&pe, 0, sizeof pe);
    pe.g = n->g;
    pe.bios = n->d->bios;
    pe.chipset = n->d->chip.chipset;
    pe.nheads = n->g->heads;
    pe.head_mask = (1u << n->g->heads) - 1;
    pe.nsors = n->g->gf119 ? 8 : n->g->sor_state == EVO_SOR_G94 ? 4 : 2;
    pe.sor_mask = n->g->gf119 || n->g->sor_state == EVO_SOR_G94 ? 0xF : 3;
    pe.ndacs = n->g->has_dac ? (n->g->gf119 ? 4 : 3) : 0;
    pe.dac_mask = n->g->has_dac ? 7 : 0;
    for (int i = 0; i < EVO_ORS; i++) pe.sor_outp[i] = pe.dac_outp[i] = pe.sor_asy_outp[i] = pe.dac_asy_outp[i] = -1;
    u32 heads = 0;
    for (int k = 0; k < n->heads_used; k++) heads |= 1u << n->hw_head[k];
    nv_evo_plan_t p[DISPLAY_MAX_CONNECTORS];
    int np = 0;
    for (int i = 0; i < n->nconn && np < DISPLAY_MAX_CONNECTORS; i++) {
        memset(&p[np], 0, sizeof p[np]);
        p[np].dcb = n->conn[i].out[0];
        p[np].khz = 148500;
        np++;
    }
    int lit = nv_evo_plan(&pe, p, np, heads);
    for (int i = 0; i < np; i++) {
        if (p[i].err) nvlog(n, "dry run: a monitor on every connector: %s: %s", n->a->conns[i].name, p[i].why);
        else nvlog(n, "dry run: a monitor on every connector: %s on head %d, %s%d, VPLL%d", n->a->conns[i].name, p[i].head,
                   p[i].or_type ? "DAC" : "SOR", p[i].or_id, p[i].head);
    }
    nvlog(n, "dry run: %d of %d monitors can be lit at once", lit, np);
}

/* the simulated card: what a first mode set would send (no hardware) */
static void dry_run(nvd_t *n) {
    nv_device_t *d = n->d;
    for (int i = 0; i < n->nconn; i++) {
        nv_connector_t *c = &d->conn[n->conn[i].nvc];
        if (c->edid_len <= 0) continue;
        display_mode_t m[8];
        if (edid_timings(c->edid, c->edid_len, m, 8) <= 0 || !m[0].t.clock_khz) continue;
        int k = n->conn[i].out[0];
        const nv_dcb_output_t *o = dcb(n, k);
        if (!o) continue;
        nv_evo_timing_t t = { m[0].t.clock_khz, m[0].t.hactive, m[0].t.hsync_start, m[0].t.hsync_end, m[0].t.htotal,
                              m[0].t.vactive, m[0].t.vsync_start, m[0].t.vsync_end, m[0].t.vtotal,
                              (u8)!(m[0].flags & DT_HSYNC_POS), (u8)!(m[0].flags & DT_VSYNC_POS) };
        nv_evo_head_cfg_t cfg;
        memset(&cfg, 0, sizeof cfg);
        if (nv_evo_raster(&t, &cfg.r)) continue;
        cfg.on = 1;
        cfg.w = (u16)m[0].w; cfg.h = (u16)m[0].h;
        cfg.pitch = ALIGN_UP((u32)m[0].w * 4, 256u);
        cfg.offset = 4 * MiB;
        cfg.vram_handle = EVO_HANDLE_VRAM;
        cfg.or_id = o->or_mask ? __builtin_ctz(o->or_mask) : 0;
        cfg.or_heads = 1;
        cfg.proto = o->type == NV_OUT_DP ? EVO_PROTO_DP_A : EVO_PROTO_TMDS_A;
        cfg.depth = o->type == NV_OUT_DP ? EVO_DEPTH_24 : 0;
        nv_evo_stream_t s;
        memset(&s, 0, sizeof s);
        nv_evo_head_methods(n->g, &s, &cfg);
        nv_evo_update(n->g, &s, 1);
        nvlog(n, "dry run: %dx%d at %u kHz on head 0, SOR%d %s: %d method words (%08x %08x ... %08x)", m[0].w, m[0].h,
              m[0].t.clock_khz, cfg.or_id, nv_out_type_str(o->type), s.n, s.w[0], s.w[1], s.w[s.n - 1]);
        /* 10d: what a page flip and the cursor would send */
        memset(&s, 0, sizeof s);
        nv_evo_base_image_methods(n->g, &s, nv_evo_base_ntfy(0, 1), EVO_HANDLE_SYNC, EVO_HANDLE_VRAM, cfg.offset + 16 * MiB, cfg.w,
                                  cfg.h, cfg.pitch);
        nvlog(n, "dry run: a page flip on head 0 (base channel %04x): %d method words (%08x %08x ... %08x)", nv_evo_base_class(n->g), s.n,
              s.w[0], s.w[1], s.w[s.n - 1]);
        memset(&s, 0, sizeof s);
        nv_evo_curs_methods(n->g, &s, 0, 1, 2 * MiB + CTL_CURS, EVO_HANDLE_VRAM);
        nv_evo_update(n->g, &s, 1);
        int cc, cu;
        nv_evo_chan_ids(n->g, EVO_CHAN_CURS, 0, &cc, &cu);
        nvlog(n, "dry run: the cursor on head 0: %d core method words (%08x %08x ...), position through channel %d (class %04x) at %06x",
              s.n, s.w[0], s.w[1], cc, nv_evo_curs_class(n->g), 0x640000 + cu * 0x1000 + 0x84);
        dry_plan(n);
        return;
    }
    nvlog(n, "dry run: no monitor with a timing on the simulated card");
}

/* 10d: the base and cursor channels of every head we registered. After one
 * that does not come up the others of its kind are not tried: the feature
 * is off (10b behaviour) and BOOT.LOG says why. */
static void chans_start(nvd_t *n) {
    display_adapter_t *a = n->a;
    nv_evo_t *e = &n->e;
    int flip_bad = !n->want_flip, curs_bad = !n->want_curs;
    for (int k = 0; k < a->nheads; k++) {
        display_head_t *dh = &a->heads[k];
        int head = n->hw_head[k];
        nvd_head_t *hd = &n->head[head];
        hd->flips = hd->curs = 0;
        if (!flip_bad && (dh->caps & HEAD_CAP_FLIP)) {
            if (!nv_evo_base_start(e, head)) hd->flips = 1;
            else {
                flip_bad = 1;
                snprintf(n->flip_why, sizeof n->flip_why, "%s", e->why);
                nvlog(n, "page flips off: %s; single-buffered scanout from the core channel (10b)", e->why);
            }
        }
        if (!curs_bad && (dh->caps & HEAD_CAP_CURSOR)) {
            if (!nv_evo_curs_start(e, head)) hd->curs = 1;
            else {
                curs_bad = 1;
                snprintf(n->curs_why, sizeof n->curs_why, "%s", e->why);
                nvlog(n, "hardware cursor off: %s; the software cursor is used", e->why);
            }
        }
    }
    for (int k = 0; k < a->nheads; k++) {
        display_head_t *dh = &a->heads[k];
        nvd_head_t *hd = &n->head[n->hw_head[k]];
        if (!hd->flips) dh->caps &= ~HEAD_CAP_FLIP;
        if (!hd->curs) dh->caps &= ~HEAD_CAP_CURSOR;
    }
    /* one feature failing on one head turns it off on all (a mixed desktop gains nothing) */
    for (int k = 0; k < a->nheads; k++) {
        display_head_t *dh = &a->heads[k];
        int head = n->hw_head[k];
        if (flip_bad && n->head[head].flips) { nv_evo_base_stop(e, head); n->head[head].flips = 0; dh->caps &= ~HEAD_CAP_FLIP; }
        if (curs_bad && n->head[head].curs) { nv_evo_curs_stop(e, head); n->head[head].curs = 0; dh->caps &= ~HEAD_CAP_CURSOR; }
    }
    int nf = 0, nc = 0;
    for (int k = 0; k < a->nheads; k++) { nf += n->head[n->hw_head[k]].flips; nc += n->head[n->hw_head[k]].curs; }
    nvlog(n, "10d: page flips on %d of %d heads%s, hardware cursor on %d%s, vertical blank polled from the scanline counter", nf,
          a->nheads, n->want_flip ? "" : " (nv.flip=0)", nc, n->want_curs ? "" : " (nv.hwcursor=0)");
}

static int nvd_enable(display_adapter_t *a) {
    nvd_t *n = a->priv;
    nv_device_t *d = n->d;
    a->why[0] = 0;
    if (d->fake) {
        dry_run(n);
        nvlog(n, "10d: no base or cursor channel started, no scanline read, %d head%s not lit: the simulated card has no registers",
              n->heads_used, n->heads_used == 1 ? "" : "s");
        return fail(n, E_NODEV, "simulated card (QEMU test hook): no registers, native modesetting not attempted");
    }
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
    nvlog(n, "BAR1 %llx, %llu MiB (window %llu MiB of %llu MiB VRAM); firmware frame buffer at %llx (%u bpp) %s at VRAM %llx, %ux%u, pitch %u",
          (unsigned long long)b1->addr, (unsigned long long)(b1->size >> 20), (unsigned long long)(n->window >> 20),
          (unsigned long long)(d->vram >> 20), (unsigned long long)fb, (unsigned)g_bootinfo.fb_bpp,
          n->fw_ok ? "in BAR1" : "not in BAR1 (or not 32 bpp)", (unsigned long long)n->fw_off,
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
    e->ops = (nv_evo_ops_t){ n, evo_i2c, evo_aux, evo_log, evo_rd8, evo_wr8, evo_wmb };
    e->vram = n->bar1;
    e->vram_base = 0;
    e->vram_size = n->window;
    e->vram_total = d->vram ? d->vram : n->window;
    e->scripts = settings_get_int("nv.scripts", 1);
    e->inst = ctrl;
    e->push = ctrl + CTL_PUSH;
    e->sync = ctrl + CTL_SYNC;
    e->basepush = ctrl + CTL_BASE;
    e->cursimg = ctrl + CTL_CURS;
    e->chan_us = 200000;
    r = nv_evo_takeover(e);
    if (r) {
        snprintf(a->why, sizeof a->why, "takeover: %s", e->why);
        return E_IO;
    }
    n->taken = 1;
    for (int h = 0; h < EVO_HEADS; h++) memset(&n->head[h], 0, sizeof n->head[h]);
    /* 10d: page flips and the hardware cursor; a channel that does not come
     * up only turns its feature off (the 10b behaviour), never the driver */
    chans_start(n);
    /* 10e: the copy engine starts later (nvd_poll), once the start guard is done */
    nv_accel_host_t ah = { n->bar1, n->window, &n->vram, n, evo_log, vram_linear };
    nv_accel_attach(n->acc, &ah);
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
    nv_accel_release(n->acc, "the display goes back to the firmware");     /* 10e first: nothing writes into VRAM any more */
    /* 10d first: the base layers off (they would cover the firmware's
     * surface), the base and cursor channels stopped (bounded) */
    if (n->taken) {
        nv_evo_chans_stop(e);
        for (int h = 0; h < EVO_HEADS; h++) { n->head[h].flips = n->head[h].curs = 0; n->head[h].pend = NULL; }
    }
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
    if (!n->taken) return;
    nv_accel_poll(n->acc);                          /* 10e: the copy engine's bring-up, once */
    /* 10d: flips that completed at a vertical blank (polled: the notifier and GET) */
    for (int k = 0; k < a->nheads; k++) {
        display_head_t *dh = &a->heads[k];
        int head = n->hw_head[k];
        nvd_head_t *hd = &n->head[head];
        if (!hd->pend) continue;
        int r = nv_evo_base_poll(&n->e, head);
        if (r == 1) {
            hd->vis_off = hd->pend->handle;
            hd->pend = NULL;
            if (++hd->flips_done == 1) nvlog(n, "head %d: first page flip completed after %llu ms", head, (unsigned long long)(time_ms() - hd->flip_ms));
            display_flip_done(dh);
            continue;
        }
        /* a flip that does not complete within four frames (at least 100 ms, before
         * the display core's 250 ms): flips end on this head, the core layer shows
         * the buffer the display core will take as the front one */
        u32 lim = MIN(200u, MAX(100u, 4 * hd->frame_ms));
        u64 waited = time_ms() - hd->flip_ms;
        if (r < 0 || waited > lim) {
            display_fb_t *fb = hd->pend;
            char w[64];
            if (r < 0) snprintf(w, sizeof w, "the base channel reported an error");
            else snprintf(w, sizeof w, "a flip did not complete in %llu ms", (unsigned long long)waited);
            flips_off(n, dh, head, fb->handle, w);
            display_flip_done(dh);
            continue;
        }
        idle_wake_at(time_ms() + 1);
    }
    if (time_ms() < n->next_hpd) return;
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
    .flip = nvd_flip,
    .wait_vblank = nvd_wait_vblank,
    .cursor_image = nvd_cursor_image,
    .cursor_move = nvd_cursor_move,
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
    a->rates = 1;                                    /* each mode's own timing: every refresh rate the monitor lists */
    a->max_w = g->max_w;
    a->max_h = g->max_h;
    a->pitch_align = 256;
    a->guarded = !d->fake;
    u64 win = d->bar1_size && d->vram ? MIN(d->bar1_size, d->vram) : d->bar1_size ? d->bar1_size : 256 * MiB;
    a->vram = win > 64 * MiB ? (win - 32 * MiB) / 2 : 0;
    /* 10d: the hardware cursor, page flips (base channel), the vertical blank
     * (polled), several heads; nv.hwcursor=0, nv.flip=0, nv.heads=N turn them
     * off or down for diagnosis */
    n->want_curs = settings_get_int("nv.hwcursor", 1) != 0;
    n->want_flip = settings_get_int("nv.flip", 1) != 0;
    int max_heads = settings_get_int("nv.heads", EVO_HEADS);
    if (max_heads < 1) max_heads = 1;
    u32 caps = HEAD_CAP_VBLANK | (n->want_curs ? HEAD_CAP_CURSOR : 0) | (n->want_flip ? HEAD_CAP_FLIP : 0);
    nv_pll_limits_t pl;
    int pll_table = d->bios && !nv_pll_limits(d->bios, d->chip.chipset, NV_PLL_VPLL0, 27000, &pl);
    int nheads = 0;
    for (u32 h = 0; h < nh && h < EVO_HEADS && nheads < DISPLAY_MAX_HEADS; h++) {
        if (!(hmask & (1u << h))) continue;
        n->heads_avail++;
        if (nheads >= max_heads) continue;
        /* a head whose video PLL the VBIOS table lacks (the table has VPLL0) is not used */
        if (h > 0 && pll_table && nv_pll_limits(d->bios, d->chip.chipset, (u8)(NV_PLL_VPLL0 + h), 27000, &pl)) {
            kprintf("nv%d: head %u not used: the video BIOS has no limits for VPLL%u\n", d->index, h, h);
            continue;
        }
        if (!display_add_head(a, caps, n->want_curs ? EVO_CURS_SIZE : 0)) break;
        n->hw_head[nheads++] = (int)h;
    }
    n->heads_used = nheads;
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
    nvlog(n, "10d: hardware cursor %s (cursor channel %04x, %dx%d), page flips %s (base channel %04x), vertical blank polled; %d of %d heads%s; "
          "a head drives up to %u kHz", n->want_curs ? "on" : "off (nv.hwcursor=0)", nv_evo_curs_class(g), EVO_CURS_SIZE, EVO_CURS_SIZE,
          n->want_flip ? "on" : "off (nv.flip=0)", nv_evo_base_class(g), nheads, n->heads_avail,
          nheads < n->heads_avail && max_heads < n->heads_avail ? " (nv.heads)" : "", nv_evo_head_max_khz(g));
    n->acc = nv_accel_probe(d, a, evo_log, n);     /* 10e (reads nothing) */
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

/* 10e: the copy engine for NVREPORT (bytes) and System Information (one line) */
int nv_display_accel(nv_device_t *d, char *buf, int size) {
    nvd_t *x = nvd_of(d);
    if (size > 0) buf[0] = 0;
    return x ? nv_accel_report(x->acc, buf, size) : 0;
}
const char *nv_display_accel_state(nv_device_t *d, char *buf, int n) {
    nvd_t *x = nvd_of(d);
    if (!x) { snprintf(buf, (size_t)n, "none (no native driver)"); return buf; }
    return nv_accel_state(x->acc, buf, n);
}

const char *nv_display_log(nv_device_t *d) {
    nvd_t *x = nvd_of(d);
    return x && x->log ? x->log : NULL;
}

/* 10d: one line per head the driver registered (NVREPORT) */
int nv_display_heads(nv_device_t *d, char *buf, int size) {
    nvd_t *x = nvd_of(d);
    int o = 0;
    if (size < 1) return 0;
    buf[0] = 0;
    if (!x) return 0;
    o += snprintf(buf + o, (size_t)(size - o), "  settings: nv.flip=%d, nv.hwcursor=%d, nv.heads=%d (%d of %d heads registered)\n",
                  x->want_flip, x->want_curs, settings_get_int("nv.heads", EVO_HEADS), x->heads_used, x->heads_avail);
    for (int k = 0; k < x->a->nheads && o < size - 1; k++) {
        display_head_t *dh = &x->a->heads[k];
        int h = x->hw_head[k];
        nvd_head_t *hd = &x->head[h];
        nv_evo_chan_t *b = &x->e.base[h], *c = &x->e.curs[h];
        o += snprintf(buf + o, (size_t)(size - o),
                      "  head %d: %s%s, %u kHz; caps%s%s%s; base channel %s (%u images, %u errors, %u flips completed); "
                      "cursor channel %s (%s, %u core updates, %u moves dropped); vertical blank waits failed %d\n",
                      h, hd->on ? "on " : "off", hd->on && dh->conn ? dh->conn->name : "", hd->khz,
                      dh->caps & HEAD_CAP_FLIP ? " flip" : "", dh->caps & HEAD_CAP_CURSOR ? " cursor" : "",
                      dh->caps & HEAD_CAP_VBLANK ? " vblank" : "", b->up ? "running" : "off", b->flips, b->errors, hd->flips_done,
                      c->up ? "running" : "off", hd->curs_on ? "shown" : "hidden", hd->curs_updates, c->busy, hd->vbl_fail);
    }
    if (x->flip_why[0] && o < size - 1) o += snprintf(buf + o, (size_t)(size - o), "  page flips off: %s\n", x->flip_why);
    if (x->curs_why[0] && o < size - 1) o += snprintf(buf + o, (size_t)(size - o), "  hardware cursor off: %s\n", x->curs_why);
    return MIN(o, size - 1);
}
