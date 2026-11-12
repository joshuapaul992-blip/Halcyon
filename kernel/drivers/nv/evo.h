/* Halcyon OS - NVIDIA GPUs (M10b): the NV50 "EVO" display engine, Tesla to
 * Pascal, without the kernel.
 *
 * Everything here works on register callbacks (nv_regs_t), a byte view of
 * VRAM (the push buffer, the display engine's instance memory, the
 * notifier) and a few more callbacks (I2C, AUX, log), so it is compiled for
 * the host too and tested there against a simulated register file
 * (tests/host/evotest.c). The kernel side, the display driver, is disp.c.
 *
 * Ported from nouveau (MIT; Copyright 2012-2016 Red Hat Inc., authors Ben
 * Skeggs and others, the notice is in THIRD_PARTY_NOTICES.md):
 *   - nvkm/engine/disp: nv50.c, g84.c, g94.c, gt200.c, gt215.c, mcp77.c,
 *     mcp89.c, gf119.c, gk104.c, gm107.c, gm200.c, gp100.c, gp102.c (the
 *     display engine's init, the core channel, the supervisor, SOR / DAC
 *     control), dp.c (link training), outp.c (choosing an output resource)
 *   - nvkm/engine/dma/usernv50.c, usergf119.c (DMA objects),
 *     nvkm/core/ramht.c (the object table)
 *   - nvkm/subdev/devinit/nv50.c, gt215.c, gf100.c and nvkm/subdev/clk/
 *     pllnv04.c, pllgt215.c (the video PLLs), nvkm/subdev/bios/pll.c,
 *     disp.c, dp.c, init.c (VBIOS tables and the init script interpreter)
 *   - dispnv50/core507d.c, core907d.c, head507d.c, head827d.c,
 *     head907d.c, head917d.c, sor507d.c, sor907d.c, dac507d.c, dac907d.c
 *     and include/nvhw/class/cl507d.h ... cl917d.h (the method streams)
 * See docs/DESIGN-M10-NVIDIA.md, "10b". */
#ifndef EVO_H
#define EVO_H
#include "core/types.h"
#include "drivers/nv/nvhw.h"

/* ---------------- generations ---------------- */

enum { EVO_NONE, EVO_NV50, EVO_G84, EVO_G94, EVO_GT200, EVO_GT215, EVO_MCP77, EVO_MCP89, EVO_GF119, EVO_GK104,
       EVO_GK110, EVO_GM107, EVO_GM200, EVO_GP100, EVO_GP102 };
/* core channel method layouts (dispnv50 core507d / core827d / core907d / core917d) */
enum { MTHD_507D, MTHD_827D, MTHD_907D, MTHD_917D };
/* where the SOR control state is mirrored (armed / assembly) */
enum { EVO_SOR_NV50, EVO_SOR_G94, EVO_SOR_GF119 };
/* DisplayPort register sets (nvkm ior_func_dp) */
enum { EVO_DP_NONE, EVO_DP_G94, EVO_DP_MCP89, EVO_DP_GF119, EVO_DP_GM107, EVO_DP_GM200 };
/* video PLL programming (nvkm devinit pll_set) */
enum { EVO_PLL_NV50, EVO_PLL_GT215, EVO_PLL_GF100 };

typedef struct nv_evo_gen {
    u8  kind;                   /* EVO_* */
    const char *name;           /* "GP102" (nouveau's display implementation) */
    u16 core_class;             /* 0x987d: names the method set, the hardware never sees it */
    u8  mthd;                   /* MTHD_* */
    u8  gf119;                  /* GF119-style registers (0x6100ac, 0x610490, 0x640000 + 0x20000 assembly) */
    u8  push_gp102;             /* push buffer registers at 0x611494 (else 0x610494 / 0x610204) */
    u8  sor_state;              /* EVO_SOR_* */
    u8  dp;                     /* EVO_DP_* */
    u8  pll;                    /* EVO_PLL_* */
    u8  route;                  /* GM200+: SORs are routed to pad macros (0x612308) */
    u8  war_g94;                /* g94_sor_war_2 / 3 */
    u8  has_dac;
    u8  heads;                  /* at most (NV50 family 2; GF119+ from 0x022448, up to 4) */
    u8  sors;                   /* SOR slots */
    u8  dp_max_bw;              /* highest link rate code: 0x0a 2.7, 0x14 5.4, 0x1e 8.1 GHz */
    u32 tmds_khz;               /* TMDS single link */
    u32 hdmi_khz;               /* HDMI (no scrambling) */
    u32 dac_khz;
    u16 max_w, max_h;           /* raster limits used for the mode list */
} nv_evo_gen_t;

/* NULL if the chipset has no EVO display engine (pre-NV50, Volta and later) */
const nv_evo_gen_t *nv_evo_gen_for(u16 chipset);

/* ---------------- the push buffer and method streams ---------------- */

#define EVO_PUSH_BYTES  0x1000                  /* one 4 KiB page, as nouveau */
#define EVO_PUSH_MAX    (EVO_PUSH_BYTES / 4 - 1 - 12)   /* the last 12 words are unusable (EVO bug) */

/* a method stream being built (host tests compare these word for word) */
typedef struct nv_evo_stream {
    u32 w[512];
    int n;
    int overflow;
} nv_evo_stream_t;

void nv_evo_mthd(nv_evo_stream_t *s, u32 mthd, int count, ...);   /* header + count data words */
static inline u32 nv_evo_hdr(u32 mthd, int count) { return ((u32)count << 18) | (mthd & 0xFFC); }
#define NV_EVO_JUMP(off) (0x20000000u | ((u32)(off) & 0xFFC))

/* EVO raster values of a display_timing_t-like timing (nouveau's
 * nv50_head_atomic_check_mode): the hardware counts from one unit into the
 * sync pulse */
typedef struct nv_evo_raster {
    u32 clock_khz;
    u16 htotal, hsynce, hblanke, hblanks;
    u16 vtotal, vsynce, vblanke, vblanks;
    u16 vblank2s, vblank2e;
    u32 blankus;
    u8  nhsync, nvsync, interlace;
} nv_evo_raster_t;
typedef struct nv_evo_timing {
    u32 clock_khz;
    u16 hactive, hsync_start, hsync_end, htotal;
    u16 vactive, vsync_start, vsync_end, vtotal;
    u8  nhsync, nvsync;         /* negative polarities */
} nv_evo_timing_t;
int  nv_evo_raster(const nv_evo_timing_t *t, nv_evo_raster_t *r);     /* 0 or E_INVAL */

/* protocol codes of SOR_SET_CONTROL (and the head's pixel depth) */
enum { EVO_PROTO_LVDS = 0, EVO_PROTO_TMDS_A = 1, EVO_PROTO_TMDS_B = 2, EVO_PROTO_DUAL_TMDS = 5,
       EVO_PROTO_DP_A = 8, EVO_PROTO_DP_B = 9, EVO_PROTO_CRT = 0x100 /* DAC RGB_CRT */ };
enum { EVO_DEPTH_DEFAULT = 0, EVO_DEPTH_18 = 2, EVO_DEPTH_24 = 5, EVO_DEPTH_30 = 6 };

/* what one head shows: a pitch-linear A8R8G8B8 surface in VRAM through the
 * core channel's own layer (no base channel: single buffered) */
typedef struct nv_evo_head_cfg {
    int head;
    int on;                     /* 0: switch the head off (and detach its OR) */
    nv_evo_raster_t r;
    u16 w, h;                   /* the surface (and the viewport's input) */
    u16 out_w, out_h;           /* the viewport's output (the raster's active area); 0: w x h */
    u64 offset;                 /* surface, VRAM offset (256-byte aligned) */
    u32 pitch;                  /* bytes, multiple of 256 */
    u32 storage_raw;            /* SET_STORAGE as is (restoring a firmware surface); 0: from pitch */
    u32 vram_handle;            /* the VRAM context DMA */
    int or_type;                /* 0 SOR, 1 DAC */
    int or_id;
    u32 proto;                  /* EVO_PROTO_* */
    u8  depth;                  /* EVO_DEPTH_* */
    u32 or_heads;               /* every head the OR drives afterwards (owner mask) */
} nv_evo_head_cfg_t;

/* the methods for one head (and its OR) in nouveau's order: view, mode,
 * core layer, base / overlay bounds, dither, procamp, output resource,
 * cursor and LUT off, OR control. Appended to s. */
void nv_evo_head_methods(const nv_evo_gen_t *g, nv_evo_stream_t *s, const nv_evo_head_cfg_t *c);
/* detach an OR (owner none) */
void nv_evo_or_off(const nv_evo_gen_t *g, nv_evo_stream_t *s, int or_type, int or_id);
/* SET_NOTIFIER_CONTROL (offset 0, notify) + UPDATE + notifier off */
void nv_evo_update(const nv_evo_gen_t *g, nv_evo_stream_t *s, int notify);
/* the first methods after the channel starts: SET_CONTEXT_DMA_NOTIFIER */
void nv_evo_core_init_methods(const nv_evo_gen_t *g, nv_evo_stream_t *s, u32 sync_handle);

/* ---------------- objects in the display engine's instance memory ---------------- */

#define EVO_INST_BYTES   0x10000                /* nouveau: 64 KiB, the object table at its start */
#define EVO_RAMHT_BYTES  0x1000
#define EVO_HANDLE_SYNC  0xF0000000u            /* the notifier's context DMA */
#define EVO_HANDLE_VRAM  0xF0000001u            /* all of VRAM (scanout) */

u32  nv_evo_ramht_hash(u32 handle, int chid);    /* 9 bits (512 entries) */
/* write a VRAM context DMA (24 bytes) at inst[obj] and its object table
 * entry; 0 or E_NOSPC. inst: a CPU view of the 64 KiB instance memory. */
int  nv_evo_dmaobj(const nv_evo_gen_t *g, u8 *inst, u32 obj, u32 handle, u64 start, u64 limit);

/* ---------------- VRAM for our own use ---------------- */

#define EVO_VRAM_HOLES 12
typedef struct nv_evo_vram {
    u64 lo, hi;                 /* the window we may use (inside BAR1, below the end of VRAM) */
    int nholes;
    struct { u64 at, size; char what[24]; } hole[EVO_VRAM_HOLES];   /* never handed out */
    int nused;
    struct { u64 at, size; } used[16];
} nv_evo_vram_t;
void nv_evo_vram_init(nv_evo_vram_t *v, u64 lo, u64 hi);
void nv_evo_vram_hole(nv_evo_vram_t *v, u64 at, u64 size, const char *what);
/* first fit, aligned; returns the offset or ~0 */
u64  nv_evo_vram_alloc(nv_evo_vram_t *v, u64 size, u64 align);
void nv_evo_vram_free(nv_evo_vram_t *v, u64 at);

/* ---------------- VBIOS: PLL limits, output and DP tables, init scripts ---------------- */

enum { NV_PLL_VPLL0 = 0x80 };
typedef struct nv_pll_limits {
    u8  ver;                    /* table version (0x20 ... 0x50) */
    u8  type;
    u32 reg;                    /* 0: the table names none (version 0x50) */
    struct { u32 min_freq, max_freq, min_inputfreq, max_inputfreq; u8 min_m, max_m, min_n, max_n; } vco1, vco2;
    u8  min_p, max_p, max_p_usable, bias_p;
    u32 refclk;                 /* kHz */
} nv_pll_limits_t;
/* the limits of a PLL by type (NV_PLL_VPLL0 + head) from the BIT 'C' table;
 * chipset selects the NV50 / G84 fixed register maps, crystal (kHz) is the
 * reference when the table has none; 0 or E_NOENT / E_NOTSUP */
int  nv_pll_limits(const nv_bios_t *b, u16 chipset, u8 type, u32 crystal, nv_pll_limits_t *out);
/* nouveau's calculators: the achieved kHz, or 0 / E_INVAL */
int  nv_pll_calc_gt215(const nv_pll_limits_t *p, u32 khz, int *N, int *fN, int *M, int *P);
int  nv_pll_calc_nv04(const nv_pll_limits_t *p, u16 chipset, u32 khz, int *N1, int *M1, int *N2, int *M2, int *log2P);

/* IED (BIT 'U') output tables and the DP (BIT 'd') tables */
typedef struct nv_ied {
    u32 outp;                   /* the output entry (0: none) */
    u8  ver, hdr, cnt, len;
    u16 type, mask;
    u16 script[3];              /* OffInt1, OffInt2 (and a third) */
} nv_ied_t;
u16  nv_dcb_hasht(const nv_dcb_output_t *o);
u16  nv_dcb_hashm(const nv_dcb_output_t *o);
int  nv_ied_match(const nv_bios_t *b, u16 type, u16 mask, nv_ied_t *out);   /* 1 found */
/* the clock-compare script pointers for protocol / flags; 1 found */
int  nv_ied_ocfg(const nv_bios_t *b, const nv_ied_t *ied, u8 proto, u8 flags, u16 clkcmp[2]);
u16  nv_ied_oclk(const nv_bios_t *b, u16 cmp, u32 khz);      /* the script for this clock, 0 none */

typedef struct nv_dpout {
    u32 outp;
    u8  ver, hdr, cnt, len;
    u16 type, mask;
    u8  flags;
    u16 script[5];              /* BeforeLinkTraining, AfterLinkTraining, EnableSpread, DisableSpread, DisableLT */
    u16 lnkcmp;
} nv_dpout_t;
int  nv_dpout_match(const nv_bios_t *b, u16 type, u16 mask, nv_dpout_t *out);   /* 1 found */
/* the drive values for a (post cursor 2, voltage swing, pre-emphasis) level */
typedef struct nv_dpcfg { u8 pc, dc, pe, tx_pu; } nv_dpcfg_t;
int  nv_dpcfg_match(const nv_bios_t *b, const nv_dpout_t *o, u8 pc, u8 vs, u8 pe, nv_dpcfg_t *out);   /* 1 found */
/* the link-rate script of lnkcmp for a link rate code */
u16  nv_dp_lnkcmp(const nv_bios_t *b, const nv_dpout_t *o, u8 bw);

/* ---------------- the core: one display engine ---------------- */

#define EVO_HEADS 4
#define EVO_ORS   8

typedef struct nv_evo nv_evo_t;

/* what the kernel (or a host test) provides besides the registers */
typedef struct nv_evo_ops {
    void *ctx;
    /* I2C on a CCB index (VBIOS script opcodes): 0 or E_* */
    int  (*i2c)(void *ctx, int ccb, nv_i2c_msg_t *msgs, int n);
    /* one AUX transaction on a CCB index: the reply nibble or E_* */
    int  (*aux)(void *ctx, int ccb, u8 req, u32 addr, u8 *buf, u8 *len);
    /* one line for the log (no newline) */
    void (*log)(void *ctx, const char *line);
    /* 8-bit register access (VGA CRTC index / data for script opcodes); NULL: none */
    u8   (*rd8)(void *ctx, u32 reg);
    void (*wr8)(void *ctx, u32 reg, u8 v);
} nv_evo_ops_t;

/* an output resource as the hardware has it */
typedef struct nv_evo_or_state {
    u32 ctrl;                   /* the raw control word */
    u8  heads;                  /* owner mask */
    u8  proto_evo;              /* raw protocol field */
    u8  proto;                  /* NV_OUT_* (CRT, TMDS, LVDS, DP), 0xFF unknown */
    u8  link;                   /* 1 A, 2 B, 3 both */
    u8  depth;
} nv_evo_or_state_t;
typedef struct nv_evo_head_state {
    u32 hz;
    u16 htotal, hsynce, hblanke, hblanks, vtotal, vsynce, vblanke, vblanks;
    u8  depth;                  /* bits per pixel the OR sends (18, 24, 30) */
    u32 orctrl;                 /* GF119+: HEAD_SET_CONTROL_OUTPUT_RESOURCE (polarities, depth) */
    u32 offset, size, storage, params, ctxdma;   /* the core layer's surface (armed) */
} nv_evo_head_state_t;

/* the firmware's display as found (what restore programs again) */
typedef struct nv_evo_fw {
    int valid;
    int head;                   /* the lit head, -1 none */
    nv_evo_head_state_t hs;
    int or_type, or_id;         /* its output resource */
    nv_evo_or_state_t os;
    int dcb;                    /* the DCB output it drives, -1 unknown */
    u64 fb_offset;              /* the surface (VRAM) */
    u32 fb_pitch, fb_w, fb_h;
} nv_evo_fw_t;

/* registers saved at takeover and put back by nv_evo_release */
#define EVO_SAVE_MAX 48
typedef struct nv_evo_saved { int n; struct { u32 reg, val; } r[EVO_SAVE_MAX]; } nv_evo_saved_t;

struct nv_evo {
    /* set by the caller */
    const nv_evo_gen_t *g;
    const nv_regs_t *r;
    const nv_bios_t *bios;
    u16 chipset;
    nv_evo_ops_t ops;
    u8  *vram;                  /* CPU view of [vram_base, vram_base + vram_size) */
    u64 vram_base, vram_size;
    u64 vram_total;             /* the card's VRAM (context DMA limit) */
    int scripts;                /* run the VBIOS display scripts (nv.scripts) */
    u32 timeout_us;             /* channel waits (2 s like nouveau) */
    u32 update_us;              /* an update with its supervisor rounds (3 s) */
    /* layout (nv_evo_takeover) */
    u64 inst, push, sync;       /* VRAM offsets */
    int nheads, nsors, ndacs;
    u32 head_mask, sor_mask, dac_mask;
    /* state */
    int chan_up;                /* the core channel is ours and running */
    u32 put;                    /* bytes */
    nv_evo_fw_t fw;
    nv_evo_saved_t saved;
    nv_evo_or_state_t sor_arm[EVO_ORS], sor_asy[EVO_ORS], dac_arm[EVO_ORS], dac_asy[EVO_ORS];
    nv_evo_head_state_t head_arm[EVO_HEADS], head_asy[EVO_HEADS];
    int sor_outp[EVO_ORS];      /* DCB output an armed SOR drives (-1) */
    int dac_outp[EVO_ORS];
    int sor_asy_outp[EVO_ORS];  /* the DCB output we attach (-1) */
    int dac_asy_outp[EVO_ORS];
    struct nv_evo_dp *dp[EVO_ORS];   /* DP state per SOR being set up (evodp.c) */
    u32 supers, errors, timeouts, script_ops, script_refused;
    char why[96];               /* the last failure */
};

/* bring up: read the state (fw), the caps copies, take the display engine
 * from the VBIOS, the object table and context DMAs, interrupts masked,
 * the core channel. The caller has checked VRAM (BAR1 linear) and filled
 * the layout fields. 0 or E_*; e->why says what failed. */
int  nv_evo_takeover(nv_evo_t *e);
/* read the armed / assembly state of every head and OR */
void nv_evo_read_state(nv_evo_t *e, int asy);
/* find the firmware's head and surface (called by takeover; for the log) */
void nv_evo_find_fw(nv_evo_t *e);
/* submit a stream to the core channel and wait for the update it ends with
 * (supervisor rounds polled and answered); 0 or E_* (E_TIMEOUT, E_IO) */
int  nv_evo_submit(nv_evo_t *e, const nv_evo_stream_t *s, int wait_update);
/* give back: interrupts and the registers saved at takeover */
void nv_evo_release(nv_evo_t *e);
/* format into the log */
void nv_evo_log(nv_evo_t *e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* EDID helpers: an HDMI sink (CEA-861 vendor block with the HDMI OUI) /
 * a digital input */
int  nv_edid_hdmi(const u8 *edid, int len);
int  nv_edid_digital(const u8 *edid, int len);
/* the firmware's mode as a head configuration (restore); 0 or E_NOTSUP */
int  nv_evo_fw_cfg(const nv_evo_t *e, u64 fb_offset, u32 fb_pitch, int fb_w, int fb_h, nv_evo_head_cfg_t *c);

/* register layout helpers (also used by report.c) */
u32  nv_evo_core_ctrl_reg(const nv_evo_gen_t *g);       /* 0x610200 / 0x610490 */
u32  nv_evo_mthd_arm(const nv_evo_gen_t *g, u32 mthd);  /* armed copy of a core method */

/* ---------------- DisplayPort (evodp.c) ---------------- */

typedef struct nv_evo_dp {
    int dcb;                    /* DCB output */
    int ccb;                    /* its AUX channel's CCB index */
    u8  dpcd[16];               /* receiver capabilities */
    int lanes_max;              /* sink and DCB */
    u8  bw_max;                 /* 0x06 / 0x0a / 0x14 / 0x1e */
    int nr;                     /* trained lanes */
    u8  bw;                     /* trained rate code */
    u8  ef;                     /* enhanced framing */
    int link;                   /* sub-link (1 A, 2 B) */
    nv_dpout_t info;
    int have_info;
    int trained;
    char why[64];
} nv_evo_dp_t;

/* link configurations to try, most lanes and the highest rate first (as
 * nouveau's nvkm_dp_train): fills nr[] / bw[]; returns the count */
int  nv_dp_configs(int lanes_max, u8 bw_max, u32 data_kBps, int nr[], u8 bw[], int max);
/* bandwidth check for mode_valid: 1 if clock_khz at bpp fits lanes x rate */
int  nv_dp_fits(int lanes, u8 bw, u32 clock_khz, int bpp);
/* the transfer unit / watermark values (nv50_disp_super_2_2_dp) */
typedef struct nv_dp_tu { int TU, VTUa, VTUf, VTUi; u32 watermark; u16 hsym; u32 vsym; } nv_dp_tu_t;
int  nv_dp_tu(const nv_evo_head_state_t *h, int nr, u8 bw, u8 ef, int depth, int activesym, nv_dp_tu_t *out);
/* read the sink's capabilities (DPCD 0x000..0x00f); 0 or E_* */
int  nv_dp_detect(nv_evo_t *e, nv_evo_dp_t *dp);
/* train the link on SOR or_id for data_kBps (source and sink); 0 or E_* */
int  nv_dp_train(nv_evo_t *e, nv_evo_dp_t *dp, int or_id, u32 data_kBps);
/* native AUX read / write with DEFER retries */
int  nv_dp_rd(nv_evo_t *e, int ccb, u32 addr, u8 *buf, int len);
int  nv_dp_wr(nv_evo_t *e, int ccb, u32 addr, const u8 *buf, int len);

/* ---------------- scripts (evoscript.c) ---------------- */

typedef struct nv_init_ctx {
    int outp;                   /* DCB output index, -1 none */
    int or_id;                  /* -1 none */
    int link;                   /* 0 none, 1 A, 2 B */
    int head;                   /* -1 none */
} nv_init_ctx_t;
/* run the init script at offset (one of the display scripts); 0, or E_*
 * when it had to stop (unknown opcode, too long, a refused register) */
int  nv_evo_script(nv_evo_t *e, u16 offset, const nv_init_ctx_t *c, const char *what);
/* the registers display scripts may write (everything else is refused and logged) */
int  nv_evo_script_reg_ok(u32 reg);

/* ---------------- the supervisor's steps (evo.c) ---------------- */
/* the video PLL of a head (evopll.c): 0 or E_* */
int  nv_evo_vpll_set(nv_evo_t *e, int head, u32 khz);

#endif
