/* Halcyon OS - Intel High Definition Audio controllers (QEMU -device
 * intel-hda (ICH6) and ich9-intel-hda; the Intel ICH6..ICH10 and PCH
 * chipsets, Atom SoCs, AMD/ATI SB4x0..SB9x0, FCH and Zen, NVIDIA MCP, VIA,
 * SiS and ULi controllers, and the HDMI/DP audio functions of graphics
 * cards). The codecs are handled by hda_codec.c.
 *
 * Reference: Intel "High Definition Audio Specification", Revision 1.0a:
 * chapter 3 (controller registers, CORB/RIRB, immediate command interface,
 * stream descriptors, buffer descriptor lists, the DMA position buffer) and
 * chapter 4 (programming model: reset, codec discovery, stream setup). The
 * chipset-specific PCI configuration bits below are from the Intel ICH/PCH
 * datasheets and the behaviour documented by the BSD drivers (FreeBSD
 * snd_hda, OpenBSD azalia); see "Quirks".
 *
 * Controller: BAR0 is mapped, the controller reset (GCTL.CRST low then high)
 * and the codecs that answer (STATESTS) probed. Verbs go through the CORB
 * and RIRB rings (256 entries where the controller has them, else 16 or 2;
 * RINTCNT 1); a verb that gets no response within 20 ms makes the driver
 * reset the rings and try once more, then switch to the immediate command
 * interface (ICW/IRR/ICS) for good. Unsolicited responses (jack events) are
 * accepted (GCTL.UNSOL) and collected from the RIRB.
 *
 * Codecs: an analog codec is preferred over HDMI-only ones, per controller
 * and across controllers: a controller whose codecs only have digital
 * outputs (a graphics card's HDMI function, e.g. the GeForce's, or the Intel
 * iGPU's HDMI codec when it is alone) registers its device as a fallback
 * (snddev_t.fallback), which never becomes the default while another device
 * exists, and is registered after the analog ones.
 *
 * Playback: the first output stream descriptor (a bidirectional one if there
 * is none), stream tag 1, 48 kHz 16-bit stereo. The buffer descriptor list
 * has 32 entries of one period each (480 frames, 10 ms), interrupt on
 * completion for every entry; the DMA engine loops round the 320 ms ring on
 * its own, so the driver keeps LEAD periods (about 40 ms, as ac97.c) filled
 * ahead of the hardware's position and silences the period after the last
 * one it filled. The position comes from the DMA position buffer when the
 * controller writes it, else from SDnLPIB. Positions that jump ahead faster
 * than the clock allows (a stale or glitching LPIB, which several
 * controllers have) are ignored. An underrun is the hardware's position
 * passing the last period filled (or the thread being away for most of the
 * ring, or the position standing still for 300 ms, or a descriptor error):
 * counted, and the stream is reset and primed again.
 *
 * Interrupts: MSI where allowed (not on the controllers known to have broken
 * MSI), else INTx (learned), else polled. They only wake the audio thread:
 * the acknowledge clears the stream's status and the controller's RIRB
 * status, nothing else. The stream interrupt is enabled only while playing,
 * so an idle device raises no interrupts at all.
 *
 * Quirks (each written to the documentation, untested except where noted):
 *   - every controller with a PCI Express capability: Device Control "Enable
 *     No Snoop" cleared, so the controller's DMA is always cache coherent
 *     (Halcyon's buffers are ordinary write-back memory). Tested in QEMU
 *     (ich9-intel-hda), where it has no effect.
 *   - Intel: TCSEL (PCI 0x44, bits 2:0) set to TC0. Some BIOSes leave a
 *     traffic class there that the chipset's virtual channels do not carry,
 *     which loses or delays the controller's DMA (crackling, stalls).
 *     Tested in QEMU (no effect there).
 *   - Intel SCH (Poulsbo) / Oaktrail: no PCIe capability; DEVC (PCI 0x78)
 *     bit 11 (no-snoop enable) cleared directly.
 *   - Intel Skylake and later PCHs and SoCs: CGCTL (PCI 0x48) bit 6
 *     (MISCBDCGE, dynamic clock gating of the misc backbone) cleared while the
 *     controller is reset and the codecs enumerate, and restored after:
 *     with it set the codecs may not be seen after the reset.
 *   - AMD/ATI SB450..SB9x0 and FCH/Zen chipset controllers: PCI 0x42 bits 2:0
 *     set to 010b (snoop enable): otherwise the DMA is not coherent.
 *   - NVIDIA MCP chipset controllers: PCI 0x4E bits 3:0 set (the coherence
 *     bits of the transaction register), and bit 0 of 0x4C / 0x4D (input /
 *     output stream coherence). Not done for the GPU functions.
 *   - MSI is not used on ATI SB450/SB600, NVIDIA MCP, VIA and SiS controllers
 *     and on the NVIDIA and AMD/ATI graphics cards' HDMI functions (INTx
 *     instead): several are known to deliver MSI unreliably.
 *   - NVIDIA and ATI/AMD: the RIRB write pointer can move before the entry
 *     is in memory: a 10 us pause before entries are read.
 *   - 64-bit addressing (GCAP.64OK): not needed: every buffer comes from
 *     dma_alloc, which is below 4 GiB (checked), and the upper base
 *     registers are written with 0, which also suits controllers that claim
 *     64OK wrongly.
 *   - CORB/RIRB size: the largest size in CORBSIZE/RIRBSIZE's capability
 *     bits (the field is read-only where only one is supported).
 *   - CORB read pointer reset: some controllers (NVIDIA, QEMU) never read
 *     the reset bit back as 1; the driver waits for it at most 1 ms and
 *     then only requires it to read 0 after clearing it.
 *   - codec reset: no codec after the controller reset is answered by a
 *     second, longer reset (some codecs miss the first after the BIOS left
 *     the link running); the function groups are not reset again (the
 *     link reset reset them, and the BIOS's pin configuration survives it).
 *   - stale LPIB: see Playback. The DMA position buffer is used when the
 *     controller writes it; if it stays 0 while LPIB moves, LPIB is used.
 *     VIA and the AMD/ATI HDMI functions use LPIB from the start (their
 *     position buffers are reported unreliable).
 *
 * Not done: AMD's newer controllers' extra workarounds for slow RIRB
 * responses beyond the pause above, VIA's FIFO size setting, Intel's
 * display codec enable for Haswell and later (hidden HDMI pins), GPU HDA
 * functions hidden by the BIOS, codec GPIO/vendor fix-ups for laptop
 * amplifiers, power management (the codec stays in D0, the link up).
 *
 * Tested in QEMU (intel-hda / ich9-intel-hda with hda-duplex, hda-output
 * and hda-micro: MSI, INTx through the I/O APIC and the PIC, polled; the
 * immediate command interface and LPIB through a test hook). Everything
 * specific to real chipsets and codecs is written to the specification,
 * untested. QEMU's codecs have no presence detect, so jack sensing is
 * exercised only by tests/host/hdatest.c (synthetic codecs).
 *
 * Test hook: QEMU fw_cfg "opt/halcyon/hda" = words "immediate" (no
 * CORB/RIRB), "corbstall" (verbs written to the CORB are never handed to
 * the controller: the timeout, ring reset and fallback to the immediate
 * interface), "lpib" (no DMA position buffer). */
#include "sound/snddev.h"
#include "sound/drivers/hda.h"
#include "drivers/pci.h"
#include "core/kernel.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/io.h"
#include "core/irq.h"

/* controller registers */
#define GCAP        0x00
#define VMIN        0x02
#define VMAJ        0x03
#define GCTL        0x08
#define WAKEEN      0x0C
#define STATESTS    0x0E
#define INTCTL      0x20
#define INTSTS      0x24
#define CORBLBASE   0x40
#define CORBUBASE   0x44
#define CORBWP      0x48
#define CORBRP      0x4A
#define CORBCTL     0x4C
#define CORBSTS     0x4D
#define CORBSIZE    0x4E
#define RIRBLBASE   0x50
#define RIRBUBASE   0x54
#define RIRBWP      0x58
#define RINTCNT     0x5A
#define RIRBCTL     0x5C
#define RIRBSTS     0x5D
#define RIRBSIZE    0x5E
#define ICW         0x60
#define IRR         0x64
#define ICS         0x68
#define DPLBASE     0x70
#define DPUBASE     0x74
#define SD_BASE     0x80
#define SD_SIZE     0x20
/* stream descriptor registers */
#define SD_CTL      0x00        /* bits 0-7 */
#define SD_CTL2     0x02        /* bits 16-23: stripe, priority, direction, stream number */
#define SD_STS      0x03
#define SD_LPIB     0x04
#define SD_CBL      0x08
#define SD_LVI      0x0C
#define SD_FMT      0x12
#define SD_BDPL     0x18
#define SD_BDPU     0x1C

#define GCTL_CRST   (1u << 0)
#define GCTL_UNSOL  (1u << 8)
#define INT_GIE     (1u << 31)
#define INT_CIE     (1u << 30)
#define CORBRP_RST  0x8000
#define CORBCTL_RUN 0x02
#define RIRBWP_RST  0x8000
#define RIRBCTL_INT 0x01
#define RIRBCTL_DMA 0x02
#define RIRBSTS_INT 0x01
#define RIRBSTS_OVR 0x04
#define ICS_BUSY    0x01
#define ICS_VALID   0x02
#define DPL_ENABLE  0x01
#define CTL_SRST    0x01
#define CTL_RUN     0x02
#define CTL_IOCE    0x04
#define CTL_FEIE    0x08
#define CTL_DEIE    0x10
#define CTL2_DIR    0x08        /* bidirectional descriptors: 1 = output */
#define STS_BCIS    0x04
#define STS_FIFOE   0x08
#define STS_DESE    0x10
#define STS_ALL     (STS_BCIS | STS_FIFOE | STS_DESE)

#define NBDL        32
#define PERIOD      480                 /* frames: 10 ms at 48 kHz */
#define PERIOD_MS   10
#define LEAD        4                   /* periods queued ahead of the hardware */
#define PBYTES      (PERIOD * 4)
#define RING        (NBDL * PBYTES)
#define RING_MS     (NBDL * PERIOD_MS)
#define BYTES_MS    (48 * 4)
#define STREAM_TAG  1
#define VERB_TIMEOUT_US 20000
#define MAX_UNSOL   16

/* quirks */
#define Q_INTEL     0x0001      /* TCSEL */
#define Q_SCH       0x0002      /* DEVC no-snoop */
#define Q_CGCTL     0x0004      /* Skylake+: clock gating off during reset */
#define Q_ATI       0x0008      /* snoop enable at 0x42 */
#define Q_NVIDIA    0x0010      /* MCP coherence bits */
#define Q_NOMSI     0x0020
#define Q_RIRBDLY   0x0040
#define Q_HDMI      0x0080      /* a graphics card's HDMI/DP audio function */
#define Q_LPIB      0x0100      /* positions from LPIB: the DMA position buffer is not trusted */

typedef struct bdl { u32 lo, hi, len, flags; } bdl_t;

typedef struct hda {
    snddev_t sd;
    pci_dev_t *pci;
    volatile u8 *mmio;
    char log[24];               /* "hda 00:1b.0" */
    const char *cname;
    u32 quirks;
    int iss, oss, bss, sidx, bidir;
    u32 sd_off;
    u32 *corb;
    volatile u32 *rirb;
    int corb_n, rirb_n;
    int corb_wp, rirb_rp;
    int immediate;
    volatile u32 *posbuf;
    int posbuf_on, posbuf_seen, lpib_warned;
    bdl_t *bdl;
    u8 *buf;
    u32 unsol[MAX_UNSOL];
    int nunsol;
    hda_codec_t *codec;
    u16 codecs;
    /* playback */
    u64 hw_total, filled;       /* bytes played (position); periods filled */
    u32 last_pos;
    u64 pos_ms, moved_ms, jack_ms;  /* when last_pos was read; when it last moved */
    int hp_plugged;
    int stall_logged;
    int jacks_off;              /* the codec stopped answering: no more jack polling */
    volatile int dese;          /* the acknowledge saw a descriptor error */
    irq_handle_t irq;
    char irq_name[32];
} hda_t;

static int opt_immediate, opt_lpib, opt_corbstall;

static u8 r8(hda_t *h, u32 r) { return *(volatile u8 *)(h->mmio + r); }
static u16 r16(hda_t *h, u32 r) { return *(volatile u16 *)(h->mmio + r); }
static u32 r32(hda_t *h, u32 r) { return *(volatile u32 *)(h->mmio + r); }
static void w8(hda_t *h, u32 r, u8 v) { *(volatile u8 *)(h->mmio + r) = v; }
static void w16(hda_t *h, u32 r, u16 v) { *(volatile u16 *)(h->mmio + r) = v; }
static void w32(hda_t *h, u32 r, u32 v) { *(volatile u32 *)(h->mmio + r) = v; }
static u8 s8(hda_t *h, u32 r) { return r8(h, h->sd_off + r); }
static u32 s32(hda_t *h, u32 r) { return r32(h, h->sd_off + r); }
static void sw8(hda_t *h, u32 r, u8 v) { w8(h, h->sd_off + r, v); }
static void sw16(hda_t *h, u32 r, u16 v) { w16(h, h->sd_off + r, v); }
static void sw32(hda_t *h, u32 r, u32 v) { w32(h, h->sd_off + r, v); }

/* wait (at most us microseconds) until (reg & mask) == want; 1 if it did */
static int wait8(hda_t *h, u32 r, u8 mask, u8 want, int us) {
    for (int i = 0; i <= us / 10; i++) {
        if ((r8(h, r) & mask) == want) return 1;
        udelay(10);
    }
    return 0;
}
static int wait16(hda_t *h, u32 r, u16 mask, u16 want, int us) {
    for (int i = 0; i <= us / 10; i++) {
        if ((r16(h, r) & mask) == want) return 1;
        udelay(10);
    }
    return 0;
}
static int wait32(hda_t *h, u32 r, u32 mask, u32 want, int us) {
    for (int i = 0; i <= us / 10; i++) {
        if ((r32(h, r) & mask) == want) return 1;
        udelay(10);
    }
    return 0;
}

/* ---------------- interrupts ---------------- */
static int hda_ack(void *ctx) {
    hda_t *h = ctx;
    u32 is = r32(h, INTSTS);
    if (is == 0xFFFFFFFF) return 0;
    u8 sts = s8(h, SD_STS);
    if (sts != 0xFF && (sts & STS_ALL)) {
        sw8(h, SD_STS, sts & STS_ALL);
        if (sts & STS_DESE) h->dese = 1;
    }
    if (is & INT_CIE) w8(h, RIRBSTS, RIRBSTS_INT | RIRBSTS_OVR);
    return (is & (INT_GIE | 1u << h->sidx)) != 0;
}

/* ---------------- verbs: CORB/RIRB and the immediate interface ---------------- */
/* take what is new in the RIRB; the response for codec cad (if want) */
static int rirb_take(hda_t *h, int want, int cad, u32 *resp) {
    int got = 0;
    int wp = r16(h, RIRBWP) & 0xFF;
    if (wp >= h->rirb_n) wp %= h->rirb_n;
    if (wp != h->rirb_rp && (h->quirks & Q_RIRBDLY)) udelay(10);
    while (h->rirb_rp != wp) {
        h->rirb_rp = (h->rirb_rp + 1) % h->rirb_n;
        u32 r = h->rirb[2 * h->rirb_rp], ex = h->rirb[2 * h->rirb_rp + 1];
        if (ex & 0x10) {                /* unsolicited */
            if (h->nunsol < MAX_UNSOL) h->unsol[h->nunsol++] = r;
            continue;
        }
        if (want && !got && (int)(ex & 0xF) == cad) { *resp = r; got = 1; }
    }
    w8(h, RIRBSTS, RIRBSTS_INT | RIRBSTS_OVR);
    return got;
}

static int corb_cmd(hda_t *h, u32 verb, u32 *resp) {
    int cad = (int)(verb >> 28);
    rirb_take(h, 0, 0, NULL);           /* anything left over (unsolicited) */
    int wp = (h->corb_wp + 1) % h->corb_n;
    h->corb[wp] = verb;
    barrier();
    if (!opt_corbstall) w16(h, CORBWP, (u16)wp);
    h->corb_wp = wp;
    u64 end = time_us() + VERB_TIMEOUT_US;
    for (;;) {
        if (rirb_take(h, 1, cad, resp)) return 0;
        if (time_us() > end) return E_TIMEOUT;
        udelay(2);
    }
}

static int imm_cmd(hda_t *h, u32 verb, u32 *resp) {
    if (!wait16(h, ICS, ICS_BUSY, 0, VERB_TIMEOUT_US)) return E_TIMEOUT;
    w16(h, ICS, ICS_VALID);             /* clear an old result */
    w32(h, ICW, verb);
    w16(h, ICS, ICS_BUSY);
    if (!wait16(h, ICS, ICS_VALID, ICS_VALID, VERB_TIMEOUT_US)) return E_TIMEOUT;
    *resp = r32(h, IRR);
    w16(h, ICS, ICS_VALID);
    return 0;
}

static void corb_stop(hda_t *h) {
    w8(h, CORBCTL, 0);
    w8(h, RIRBCTL, 0);
    wait8(h, CORBCTL, CORBCTL_RUN, 0, 1000);
    wait8(h, RIRBCTL, RIRBCTL_DMA, 0, 1000);
}

static int ring_size(u8 reg, int *n) {
    if (reg & 0x40) { *n = 256; return 2; }
    if (reg & 0x20) { *n = 16; return 1; }
    if (reg & 0x10) { *n = 2; return 0; }
    /* no capability bits: keep what is selected */
    *n = (reg & 3) == 2 ? 256 : (reg & 3) == 1 ? 16 : 2;
    return reg & 3;
}

static int corb_setup(hda_t *h) {
    corb_stop(h);
    u8 cs = r8(h, CORBSIZE), rs = r8(h, RIRBSIZE);
    int csel = ring_size(cs, &h->corb_n), rsel = ring_size(rs, &h->rirb_n);
    w8(h, CORBSIZE, (u8)((cs & ~3) | csel));
    w8(h, RIRBSIZE, (u8)((rs & ~3) | rsel));
    w32(h, CORBLBASE, (u32)virt_to_phys(h->corb));
    w32(h, CORBUBASE, 0);
    w32(h, RIRBLBASE, (u32)virt_to_phys((const void *)h->rirb));
    w32(h, RIRBUBASE, 0);
    memset(h->corb, 0, 1024);
    memset((void *)h->rirb, 0, 2048);
    /* read pointer reset: set, (wait for it to read 1: not every controller
     * shows that), clear, and it must read 0 */
    w16(h, CORBRP, CORBRP_RST);
    wait16(h, CORBRP, CORBRP_RST, CORBRP_RST, 1000);
    w16(h, CORBRP, 0);
    if (!wait16(h, CORBRP, CORBRP_RST, 0, 1000)) return E_IO;
    w16(h, CORBWP, 0);
    w16(h, RIRBWP, RIRBWP_RST);
    w16(h, RINTCNT, 1);
    w8(h, RIRBSTS, RIRBSTS_INT | RIRBSTS_OVR);
    w8(h, RIRBCTL, RIRBCTL_DMA | RIRBCTL_INT);
    w8(h, CORBCTL, CORBCTL_RUN);
    if (!wait8(h, CORBCTL, CORBCTL_RUN, CORBCTL_RUN, 1000)) return E_IO;
    h->corb_wp = 0;
    h->rirb_rp = r16(h, RIRBWP) & 0xFF;
    if (h->rirb_rp >= h->rirb_n) h->rirb_rp = 0;
    return 0;
}

static void use_immediate(hda_t *h, const char *why) {
    corb_stop(h);
    h->immediate = 1;
    kprintf("%s: %s: using the immediate command interface\n", h->log, why);
}

static int hda_cmd(void *ctx, u32 verb, u32 *resp) {
    hda_t *h = ctx;
    if (h->immediate) return imm_cmd(h, verb, resp);
    int e = corb_cmd(h, verb, resp);
    if (!e) return 0;
    /* no response: start the rings over (that also drops a late response) and try again */
    kprintf("%s: no response to verb %08x, resetting the CORB/RIRB\n", h->log, verb);
    if (corb_setup(h) == 0 && corb_cmd(h, verb, resp) == 0) return 0;
    use_immediate(h, "the CORB/RIRB do not work");
    return imm_cmd(h, verb, resp);
}

static void delay_ms(int ms) { mdelay((u64)ms); }

/* ---------------- controller reset ---------------- */
static int ctrl_reset(hda_t *h, int settle_ms) {
    corb_stop(h);
    int nsd = h->iss + h->oss + h->bss;
    for (int i = 0; i < nsd; i++) w8(h, SD_BASE + i * SD_SIZE + SD_CTL, 0);
    w32(h, INTCTL, 0);
    w32(h, DPLBASE, 0);
    w32(h, DPUBASE, 0);
    w16(h, STATESTS, 0x7FFF);
    w32(h, GCTL, r32(h, GCTL) & ~GCTL_CRST);
    if (!wait32(h, GCTL, GCTL_CRST, 0, 100000)) return E_IO;
    udelay(500);
    w32(h, GCTL, r32(h, GCTL) | GCTL_CRST);
    if (!wait32(h, GCTL, GCTL_CRST, GCTL_CRST, 100000)) return E_IO;
    /* codecs ask for an address within 521 us of the reset ending */
    for (int i = 0; i < settle_ms && !(r16(h, STATESTS) & 0x7FFF); i++) mdelay(1);
    mdelay(1);
    return 0;
}

/* ---------------- the stream ---------------- */
static void stream_stop(hda_t *h) {
    sw8(h, SD_CTL, (u8)(s8(h, SD_CTL) & ~(CTL_RUN | CTL_IOCE | CTL_FEIE | CTL_DEIE)));
    wait8(h, h->sd_off + SD_CTL, CTL_RUN, 0, 1000);
    sw8(h, SD_CTL, CTL_SRST);
    wait8(h, h->sd_off + SD_CTL, CTL_SRST, CTL_SRST, 1000);
    sw8(h, SD_CTL, 0);
    wait8(h, h->sd_off + SD_CTL, CTL_SRST, 0, 1000);
    sw8(h, SD_STS, STS_ALL);
}

static void fill_period(hda_t *h, u64 n) {
    u8 *p = h->buf + (n % NBDL) * PBYTES;
    snd_mix(&h->sd, (i16 *)p, PERIOD);
    /* the period after it: silence, should the hardware get there first */
    memset(h->buf + ((n + 1) % NBDL) * PBYTES, 0, PBYTES);
}

/* reset the stream, queue LEAD periods from the start of the ring and run */
static void stream_prime(hda_t *h) {
    stream_stop(h);
    memset(h->buf, 0, RING);
    for (int i = 0; i < NBDL; i++) {
        u64 a = virt_to_phys(h->buf + i * PBYTES);
        h->bdl[i].lo = (u32)a;
        h->bdl[i].hi = 0;
        h->bdl[i].len = PBYTES;
        h->bdl[i].flags = 1;            /* interrupt on completion */
    }
    for (int i = 0; i < LEAD; i++) fill_period(h, (u64)i);
    h->filled = LEAD;
    h->hw_total = 0;
    h->last_pos = 0;
    h->pos_ms = h->moved_ms = time_ms();
    barrier();
    sw32(h, SD_BDPL, (u32)virt_to_phys(h->bdl));
    sw32(h, SD_BDPU, 0);
    sw32(h, SD_CBL, RING);
    sw16(h, SD_LVI, NBDL - 1);
    sw16(h, SD_FMT, HDA_FMT_48K_16_2);
    sw8(h, SD_CTL2, (u8)(STREAM_TAG << 4 | (h->bidir ? CTL2_DIR : 0)));
    if (h->posbuf_on) {
        memset((void *)h->posbuf, 0, 256);
        w32(h, DPUBASE, 0);
        w32(h, DPLBASE, (u32)virt_to_phys((const void *)h->posbuf) | DPL_ENABLE);
    }
    int irq = h->irq.kind != IRQ_NONE;
    w32(h, INTCTL, irq ? INT_GIE | 1u << h->sidx : 0);
    sw8(h, SD_CTL, (u8)(CTL_RUN | (irq ? CTL_IOCE | CTL_DEIE : 0)));
}

static u32 hw_pos(hda_t *h) {
    u32 lpib = s32(h, SD_LPIB), pos = lpib;
    if (h->posbuf_on) {
        u32 pb = h->posbuf[2 * h->sidx];
        if (pb) h->posbuf_seen = 1;
        if (h->posbuf_seen) pos = pb;
        else if (lpib >= 4 * PBYTES) {
            /* the controller does not write it */
            h->posbuf_on = 0;
            w32(h, DPLBASE, 0);
            kprintf("%s: the DMA position buffer stays empty, using LPIB\n", h->log);
        }
    }
    return pos % RING;
}

static void jack_changed(hda_t *h) {
    hda_codec_describe(h->codec, h->sd.output, sizeof h->sd.output);
    if (h->codec->hp_plugged != h->hp_plugged) {
        h->hp_plugged = h->codec->hp_plugged;
        kprintf("%s: headphones %s: playing on %s\n", h->log, h->hp_plugged ? "plugged in" : "unplugged", h->sd.output);
    }
}

static void codec_events(hda_t *h, u64 now) {
    if (!h->immediate) rirb_take(h, 0, 0, NULL);
    int changed = 0;
    for (int i = 0; i < h->nunsol; i++) changed |= hda_codec_unsol(h->codec, h->unsol[i]);
    h->nunsol = 0;
    if (now >= h->jack_ms && !h->jacks_off) {   /* poll the jacks too (no unsolicited responses on some codecs) */
        int failed = h->codec->verbs_failed;
        h->jack_ms = now + 500;
        changed |= hda_codec_jacks(h->codec);
        if (h->codec->verbs_failed > failed) {
            h->jacks_off = 1;           /* (each failed verb costs a timeout in the audio thread) */
            kprintf("%s: the codec does not answer, jack sensing off\n", h->log);
        }
    }
    if (changed) jack_changed(h);
}

static int hda_start(snddev_t *sd) {
    hda_t *h = sd->priv;
    h->jack_ms = 0;
    codec_events(h, time_ms());
    hda_codec_stream(h->codec, STREAM_TAG, HDA_FMT_48K_16_2);
    stream_prime(h);
    return 0;
}

static void hda_stop(snddev_t *sd) {
    hda_t *h = sd->priv;
    w32(h, INTCTL, 0);
    stream_stop(h);
    w32(h, DPLBASE, 0);
    hda_codec_stream(h->codec, 0, HDA_FMT_48K_16_2);
}

static void underrun(hda_t *h) {
    h->sd.underruns++;
    stream_prime(h);
    irq_note_work(&h->irq);
}

static void hda_poll(snddev_t *sd) {
    hda_t *h = sd->priv;
    u8 sts = s8(h, SD_STS);
    if (sts == 0xFF) return;            /* the device went away */
    if (sts & STS_ALL) sw8(h, SD_STS, sts & STS_ALL);
    u64 now = time_ms();
    codec_events(h, now);
    sd->irq_live = irq_live(&h->irq);
    if ((sts & STS_DESE) || h->dese) {
        h->dese = 0;
        kprintf("%s: stream descriptor error, restarting\n", h->log);
        underrun(h);
        return;
    }
    u32 pos = hw_pos(h);
    u32 delta = (pos + RING - h->last_pos) % RING;
    u64 el = now - h->pos_ms;
    if (el >= RING_MS - 2 * PERIOD_MS) {    /* away too long: the hardware may have gone round */
        underrun(h);
        return;
    }
    if (delta / BYTES_MS <= el + 60) {      /* (else faster than real time: a stale or bad reading) */
        h->hw_total += delta;
        h->last_pos = pos;
        h->pos_ms = now;
        if (delta) h->moved_ms = now;
    }
    if (now - h->moved_ms > 300) {
        if (!h->stall_logged++) kprintf("%s: the stream does not move, restarting it\n", h->log);
        underrun(h);
        return;
    }
    u64 cur = h->hw_total / PBYTES;     /* the period playing now */
    if (h->filled <= cur) {             /* it played past what was filled */
        underrun(h);
        return;
    }
    int work = 0;
    while (h->filled < cur + 1 + LEAD) {
        fill_period(h, h->filled);
        h->filled++;
        work = 1;
    }
    if (work) irq_note_work(&h->irq);
    sd->irq_live = irq_live(&h->irq);
}

static int hda_set_volume(snddev_t *sd, int percent, int mute) {
    hda_t *h = sd->priv;
    hda_codec_set_volume(h->codec, percent, mute);
    return 0;
}

/* ---------------- controllers ---------------- */
#define I_PCH  (Q_INTEL)
#define I_SKL  (Q_INTEL | Q_CGCTL)
static const struct hda_id { u16 vendor, device; const char *name; u32 quirks; } ids[] = {
    /* Intel chipsets */
    { 0x8086, 0x2668, "Intel ICH6 HD Audio", I_PCH }, { 0x8086, 0x27D8, "Intel ICH7 HD Audio", I_PCH },
    { 0x8086, 0x269A, "Intel ESB2 HD Audio", I_PCH }, { 0x8086, 0x284B, "Intel ICH8 HD Audio", I_PCH },
    { 0x8086, 0x293E, "Intel ICH9 HD Audio", I_PCH }, { 0x8086, 0x293F, "Intel ICH9 HD Audio", I_PCH },
    { 0x8086, 0x3A3E, "Intel ICH10 HD Audio", I_PCH }, { 0x8086, 0x3A6E, "Intel ICH10 HD Audio", I_PCH },
    { 0x8086, 0x3B56, "Intel 5 Series HD Audio", I_PCH }, { 0x8086, 0x3B57, "Intel 5 Series HD Audio", I_PCH },
    { 0x8086, 0x1C20, "Intel 6 Series HD Audio", I_PCH }, { 0x8086, 0x1D20, "Intel C600/X79 HD Audio", I_PCH },
    { 0x8086, 0x1E20, "Intel 7 Series HD Audio", I_PCH }, { 0x8086, 0x8C20, "Intel 8 Series HD Audio", I_PCH },
    { 0x8086, 0x8C21, "Intel 8 Series HD Audio", I_PCH }, { 0x8086, 0x8CA0, "Intel 9 Series HD Audio", I_PCH },
    { 0x8086, 0x8D20, "Intel C610/X99 HD Audio", I_PCH }, { 0x8086, 0x8D21, "Intel C610/X99 HD Audio", I_PCH },
    { 0x8086, 0x9C20, "Intel 8 Series (LP) HD Audio", I_PCH }, { 0x8086, 0x9C21, "Intel 8 Series (LP) HD Audio", I_PCH },
    { 0x8086, 0x9CA0, "Intel 9 Series (LP) HD Audio", I_PCH },
    { 0x8086, 0xA170, "Intel 100 Series HD Audio", I_SKL }, { 0x8086, 0x9D70, "Intel Sunrise Point-LP HD Audio", I_SKL },
    { 0x8086, 0x9D71, "Intel Kaby Lake (LP) HD Audio", I_SKL }, { 0x8086, 0xA171, "Intel CM238 HD Audio", I_SKL },
    { 0x8086, 0xA2F0, "Intel 200 Series HD Audio", I_SKL }, { 0x8086, 0xA348, "Intel 300 Series HD Audio", I_SKL },
    { 0x8086, 0x9DC8, "Intel Cannon Point-LP HD Audio", I_SKL }, { 0x8086, 0xA3F0, "Intel 400 Series HD Audio", I_SKL },
    { 0x8086, 0x06C8, "Intel 400 Series HD Audio", I_SKL }, { 0x8086, 0x02C8, "Intel Comet Lake (LP) HD Audio", I_SKL },
    { 0x8086, 0x34C8, "Intel Ice Lake (LP) HD Audio", I_SKL }, { 0x8086, 0x38C8, "Intel Ice Lake (N) HD Audio", I_SKL },
    { 0x8086, 0xA0C8, "Intel Tiger Lake (LP) HD Audio", I_SKL }, { 0x8086, 0x43C8, "Intel 500 Series HD Audio", I_SKL },
    { 0x8086, 0x4DC8, "Intel Jasper Lake HD Audio", I_SKL }, { 0x8086, 0x4B55, "Intel Elkhart Lake HD Audio", I_SKL },
    { 0x8086, 0x4B58, "Intel Elkhart Lake HD Audio", I_SKL }, { 0x8086, 0x7AD0, "Intel 600 Series HD Audio", I_SKL },
    { 0x8086, 0x51C8, "Intel Alder Lake (P) HD Audio", I_SKL }, { 0x8086, 0x51C9, "Intel Alder Lake (P) HD Audio", I_SKL },
    { 0x8086, 0x51CC, "Intel Alder Lake (M) HD Audio", I_SKL }, { 0x8086, 0x51CD, "Intel Alder Lake (M) HD Audio", I_SKL },
    { 0x8086, 0x54C8, "Intel Alder Lake (N) HD Audio", I_SKL }, { 0x8086, 0x7A50, "Intel 700 Series HD Audio", I_SKL },
    { 0x8086, 0x51CA, "Intel Raptor Lake (P) HD Audio", I_SKL }, { 0x8086, 0x51CB, "Intel Raptor Lake (P) HD Audio", I_SKL },
    { 0x8086, 0x51CE, "Intel Raptor Lake (P) HD Audio", I_SKL }, { 0x8086, 0x51CF, "Intel Raptor Lake (P) HD Audio", I_SKL },
    { 0x8086, 0x7E28, "Intel Meteor Lake HD Audio", I_SKL }, { 0x8086, 0x7F50, "Intel 800 Series HD Audio", I_SKL },
    /* Intel SoCs */
    { 0x8086, 0x811B, "Intel SCH HD Audio", Q_INTEL | Q_SCH }, { 0x8086, 0x080A, "Intel Oaktrail HD Audio", Q_INTEL | Q_SCH },
    { 0x8086, 0x0F04, "Intel Bay Trail HD Audio", I_PCH }, { 0x8086, 0x2284, "Intel Braswell HD Audio", I_PCH },
    { 0x8086, 0x5A98, "Intel Apollo Lake HD Audio", I_SKL }, { 0x8086, 0x1A98, "Intel Broxton HD Audio", I_SKL },
    { 0x8086, 0x3198, "Intel Gemini Lake HD Audio", I_SKL },
    /* Intel display audio (HDMI/DP of the CPU's graphics) */
    { 0x8086, 0x0A0C, "Intel Haswell HDMI audio", Q_INTEL | Q_HDMI }, { 0x8086, 0x0C0C, "Intel Haswell HDMI audio", Q_INTEL | Q_HDMI },
    { 0x8086, 0x0D0C, "Intel Haswell HDMI audio", Q_INTEL | Q_HDMI }, { 0x8086, 0x160C, "Intel Broadwell HDMI audio", Q_INTEL | Q_HDMI },
    /* AMD / ATI chipsets */
    { 0x1002, 0x437B, "ATI SB450 HD Audio", Q_ATI | Q_NOMSI | Q_RIRBDLY },
    { 0x1002, 0x4383, "ATI SB600 HD Audio", Q_ATI | Q_NOMSI | Q_RIRBDLY },
    { 0x1022, 0x780D, "AMD FCH HD Audio", Q_ATI | Q_RIRBDLY }, { 0x1022, 0x1457, "AMD Zen HD Audio", Q_ATI | Q_RIRBDLY },
    { 0x1022, 0x1487, "AMD Zen 2 HD Audio", Q_ATI | Q_RIRBDLY }, { 0x1022, 0x15E3, "AMD Raven/Renoir HD Audio", Q_ATI | Q_RIRBDLY },
    /* NVIDIA MCP chipsets */
    { 0x10DE, 0x026C, "NVIDIA MCP51 HD Audio", Q_NVIDIA | Q_NOMSI | Q_RIRBDLY },
    { 0x10DE, 0x0371, "NVIDIA MCP55 HD Audio", Q_NVIDIA | Q_NOMSI | Q_RIRBDLY },
    { 0x10DE, 0x03E4, "NVIDIA MCP61 HD Audio", Q_NVIDIA | Q_NOMSI | Q_RIRBDLY },
    { 0x10DE, 0x03F0, "NVIDIA MCP61 HD Audio", Q_NVIDIA | Q_NOMSI | Q_RIRBDLY },
    { 0x10DE, 0x044A, "NVIDIA MCP65 HD Audio", Q_NVIDIA | Q_NOMSI | Q_RIRBDLY },
    { 0x10DE, 0x044B, "NVIDIA MCP65 HD Audio", Q_NVIDIA | Q_NOMSI | Q_RIRBDLY },
    { 0x10DE, 0x055C, "NVIDIA MCP67 HD Audio", Q_NVIDIA | Q_NOMSI | Q_RIRBDLY },
    { 0x10DE, 0x055D, "NVIDIA MCP67 HD Audio", Q_NVIDIA | Q_NOMSI | Q_RIRBDLY },
    { 0x10DE, 0x07FC, "NVIDIA MCP73 HD Audio", Q_NVIDIA | Q_NOMSI | Q_RIRBDLY },
    { 0x10DE, 0x07FD, "NVIDIA MCP73 HD Audio", Q_NVIDIA | Q_NOMSI | Q_RIRBDLY },
    { 0x10DE, 0x0774, "NVIDIA MCP77 HD Audio", Q_NVIDIA | Q_NOMSI | Q_RIRBDLY },
    { 0x10DE, 0x0775, "NVIDIA MCP77 HD Audio", Q_NVIDIA | Q_NOMSI | Q_RIRBDLY },
    { 0x10DE, 0x0776, "NVIDIA MCP77 HD Audio", Q_NVIDIA | Q_NOMSI | Q_RIRBDLY },
    { 0x10DE, 0x0777, "NVIDIA MCP77 HD Audio", Q_NVIDIA | Q_NOMSI | Q_RIRBDLY },
    { 0x10DE, 0x0AC0, "NVIDIA MCP79 HD Audio", Q_NVIDIA | Q_NOMSI | Q_RIRBDLY },
    { 0x10DE, 0x0AC1, "NVIDIA MCP79 HD Audio", Q_NVIDIA | Q_NOMSI | Q_RIRBDLY },
    { 0x10DE, 0x0AC2, "NVIDIA MCP79 HD Audio", Q_NVIDIA | Q_NOMSI | Q_RIRBDLY },
    { 0x10DE, 0x0AC3, "NVIDIA MCP79 HD Audio", Q_NVIDIA | Q_NOMSI | Q_RIRBDLY },
    { 0x10DE, 0x0D94, "NVIDIA MCP89 HD Audio", Q_NVIDIA | Q_NOMSI | Q_RIRBDLY },
    { 0x10DE, 0x0D95, "NVIDIA MCP89 HD Audio", Q_NVIDIA | Q_NOMSI | Q_RIRBDLY },
    { 0x10DE, 0x0D96, "NVIDIA MCP89 HD Audio", Q_NVIDIA | Q_NOMSI | Q_RIRBDLY },
    { 0x10DE, 0x0D97, "NVIDIA MCP89 HD Audio", Q_NVIDIA | Q_NOMSI | Q_RIRBDLY },
    /* others */
    { 0x1106, 0x3288, "VIA VT8251/VT8237A HD Audio", Q_NOMSI | Q_LPIB },
    { 0x1039, 0x7502, "SiS 966 HD Audio", Q_NOMSI },
    { 0x10B9, 0x5461, "ULi M5461 HD Audio", Q_NOMSI },
};

/* a controller found by class: name and quirks by vendor */
static void by_vendor(pci_dev_t *pd, struct hda_id *out) {
    out->vendor = pd->vendor; out->device = pd->device; out->quirks = 0;
    switch (pd->vendor) {
    case 0x8086: out->name = "Intel HD Audio"; out->quirks = Q_INTEL; break;
    case 0x10DE: out->name = "NVIDIA HDMI audio"; out->quirks = Q_HDMI | Q_NOMSI | Q_RIRBDLY; break;  /* the MCPs are in the list */
    case 0x1002: out->name = "AMD/ATI HDMI audio"; out->quirks = Q_HDMI | Q_NOMSI | Q_LPIB | Q_RIRBDLY; break;
    case 0x1022: out->name = "AMD HD Audio"; out->quirks = Q_RIRBDLY; break;
    case 0x1106: out->name = "VIA HD Audio"; out->quirks = Q_NOMSI | Q_LPIB; break;
    case 0x1039: out->name = "SiS HD Audio"; out->quirks = Q_NOMSI; break;
    default: out->name = "HD Audio controller"; break;
    }
}

static void pci_quirks(hda_t *h) {
    pci_dev_t *pd = h->pci;
    int pcie = pci_find_cap(pd, 0x10);
    if (pcie) {                         /* Device Control: Enable No Snoop off */
        u16 dc = pci_read16(pd, (u8)(pcie + 8));
        if (dc & (1u << 11)) pci_write16(pd, (u8)(pcie + 8), (u16)(dc & ~(1u << 11)));
    }
    if (h->quirks & Q_INTEL) pci_write8(pd, 0x44, (u8)(pci_read8(pd, 0x44) & ~0x07));
    if ((h->quirks & Q_SCH) && !pcie) pci_write16(pd, 0x78, (u16)(pci_read16(pd, 0x78) & ~0x0800));
    if (h->quirks & Q_ATI) pci_write8(pd, 0x42, (u8)((pci_read8(pd, 0x42) & ~0x07) | 0x02));
    if (h->quirks & Q_NVIDIA) {
        pci_write8(pd, 0x4E, (u8)((pci_read8(pd, 0x4E) & 0xF0) | 0x0F));
        pci_write8(pd, 0x4C, (u8)(pci_read8(pd, 0x4C) | 0x01));
        pci_write8(pd, 0x4D, (u8)(pci_read8(pd, 0x4D) | 0x01));
    }
}

static void hda_free(hda_t *h) {
    dma_free((void *)h->corb);
    dma_free(h->bdl);
    dma_free(h->buf);
    kfree(h->codec);
    kfree(h);
}

static hda_t *hda_attach(pci_dev_t *pd, const struct hda_id *id) {
    hda_t *h = kzalloc(sizeof *h);
    if (!h) return NULL;
    h->pci = pd;
    h->cname = id->name;
    h->quirks = id->quirks;
    snprintf(h->log, sizeof h->log, "hda %02x:%02x.%x", pd->bus, pd->dev, pd->func);
    pci_quirks(h);
    pci_enable(pd);
    if (pd->bar[0].is_io || !pd->bar[0].addr || !(h->mmio = pci_map_bar(pd, 0))) {
        kprintf("%s: no memory BAR\n", h->log);
        kfree(h);
        return NULL;
    }
    u16 gcap = r16(h, GCAP);
    if (gcap == 0xFFFF) {
        kprintf("%s: not responding\n", h->log);
        kfree(h);
        return NULL;
    }
    h->oss = (gcap >> 12) & 0xF;
    h->iss = (gcap >> 8) & 0xF;
    h->bss = (gcap >> 3) & 0x1F;
    if (h->oss) {
        h->sidx = h->iss;
    } else if (h->bss) {
        h->sidx = h->iss;           /* the first bidirectional descriptor, set to output */
        h->bidir = 1;
    } else {
        kprintf("%s: no output stream\n", h->log);
        kfree(h);
        return NULL;
    }
    h->sd_off = SD_BASE + (u32)h->sidx * SD_SIZE;
    /* CORB (1 KB), RIRB (2 KB) and the DMA position buffer in one block */
    u8 *ring = dma_alloc(4096, 128);
    h->bdl = dma_alloc(NBDL * sizeof(bdl_t), 128);
    h->buf = dma_alloc(RING, 128);
    h->corb = (u32 *)ring;
    if (!ring || !h->bdl || !h->buf || virt_to_phys(ring) + 4096 > 0x100000000ull ||
        virt_to_phys(h->bdl) + NBDL * sizeof(bdl_t) > 0x100000000ull || virt_to_phys(h->buf) + RING > 0x100000000ull) {
        kprintf("%s: out of memory\n", h->log);
        hda_free(h);
        return NULL;
    }
    h->rirb = (volatile u32 *)(ring + 1024);
    h->posbuf = (volatile u32 *)(ring + 3072);
    h->posbuf_on = !opt_lpib && !(h->quirks & Q_LPIB);

    /* reset; with clock gating off on Skylake and later while the codecs enumerate */
    u32 cgctl = 0;
    if (h->quirks & Q_CGCTL) {
        cgctl = pci_read32(pd, 0x48);
        pci_write32(pd, 0x48, cgctl & ~(1u << 6));
    }
    int err = ctrl_reset(h, 50);
    h->codecs = r16(h, STATESTS) & 0x7FFF;
    if (!err && !h->codecs) {
        err = ctrl_reset(h, 200);
        h->codecs = r16(h, STATESTS) & 0x7FFF;
    }
    if (h->quirks & Q_CGCTL) pci_write32(pd, 0x48, cgctl);
    if (err) {
        kprintf("%s: the controller does not come out of reset\n", h->log);
        hda_free(h);
        return NULL;
    }
    w16(h, STATESTS, h->codecs);
    w16(h, WAKEEN, 0);
    if (opt_immediate) use_immediate(h, "test hook");
    else if (corb_setup(h)) use_immediate(h, "the CORB/RIRB do not start");
    w32(h, GCTL, r32(h, GCTL) | GCTL_UNSOL);
    char cmdif[32];
    if (h->immediate) snprintf(cmdif, sizeof cmdif, "immediate commands");
    else snprintf(cmdif, sizeof cmdif, "CORB %d / RIRB %d", h->corb_n, h->rirb_n);
    kprintf("%s: %s (%04x:%04x), HDA %d.%d, streams %d in, %d out, %d bidirectional%s, %s, position from %s, codecs %#x\n",
            h->log, h->cname, pd->vendor, pd->device, r8(h, VMAJ), r8(h, VMIN), h->iss, h->oss, h->bss,
            (gcap & 1) ? ", 64-bit" : "", cmdif, h->posbuf_on ? "the DMA position buffer" : "LPIB", h->codecs);
    if (!h->codecs) {
        kprintf("%s: no codec\n", h->log);
        hda_free(h);
        return NULL;
    }

    /* the codecs: an analog one first, else one with HDMI/DP (or S/PDIF) */
    hda_codec_t *cs[15] = { 0 };
    for (int a = 0; a < 15; a++) {
        if (!(h->codecs & (1u << a))) continue;
        hda_codec_t *c = kzalloc(sizeof *c);
        if (!c) break;
        c->cad = a;
        c->cmd = hda_cmd;
        c->ctx = h;
        c->delay = delay_ms;
        c->log = h->log;
        if (hda_codec_probe(c)) {
            kprintf("%s: codec %d: %s\n", h->log, a, c->vendor_id ? "no audio function group" : "does not answer");
            if (c->vendor_id) kprintf("%s: codec %d: %s (%08x)\n", h->log, a, c->name, c->vendor_id);
            kfree(c);
            continue;
        }
        cs[a] = c;
    }
    int pick = -1;
    for (int a = 0; a < 15 && pick < 0; a++) if (cs[a] && hda_codec_find_paths(cs[a], 0)) pick = a;
    for (int a = 0; a < 15 && pick < 0; a++) if (cs[a] && hda_codec_find_paths(cs[a], 1)) pick = a;
    for (int a = 0; a < 15; a++) {
        if (!cs[a]) continue;
        hda_codec_dump(cs[a]);
        if (a != pick) {
            kprintf("%s: codec %d: %s, not used\n", h->log, a, cs[a]->npaths ? "another codec plays" : "no output");
            kfree(cs[a]);
        }
    }
    if (pick < 0) {
        kprintf("%s: no codec with an output\n", h->log);
        hda_free(h);
        return NULL;
    }
    h->codec = cs[pick];
    if (hda_codec_setup(h->codec, HDA_FMT_48K_16_2))
        kprintf("%s: codec %d: some verbs failed during setup\n", h->log, pick);
    h->hp_plugged = h->codec->hp_plugged;
    stream_stop(h);

    snprintf(h->irq_name, sizeof h->irq_name, "hda %02x:%02x.%x", pd->bus, pd->dev, pd->func);
    h->irq.name = h->irq_name;
    h->irq.pci = pd;
    h->irq.ack = hda_ack;
    h->irq.ctx = h;
    h->irq.intx_only = (h->quirks & Q_NOMSI) != 0;
    irq_attach(&h->irq);

    h->sd.driver = "hda";
    snprintf(h->sd.desc, sizeof h->sd.desc, "%s, %s", h->cname, h->codec->name);
    hda_codec_describe(h->codec, h->sd.output, sizeof h->sd.output);
    h->sd.rate = 48000;
    h->sd.channels = 2;
    h->sd.period_frames = PERIOD;
    h->sd.start = hda_start;
    h->sd.stop = hda_stop;
    h->sd.poll = hda_poll;
    if (h->codec->hw_volume) {
        h->sd.set_volume = hda_set_volume;
        h->sd.hw_volume = 1;
    }
    h->sd.fallback = h->codec->digital;
    h->sd.priv = h;
    h->sd.pci = pd;
    h->sd.irq = &h->irq;
    pd->driver = "hda";
    int sense = 0;
    for (int i = 0; i < h->codec->npaths; i++) sense += h->codec->path[i].detect;
    kprintf("%s: codec %d (%s) plays on %s%s; volume %s; %s\n", h->log, pick, h->codec->name, h->sd.output,
            h->codec->digital ? " (digital only: a fallback device)" : "",
            h->codec->hw_volume ? "in the codec" : "in the mixer",
            !sense ? "no jack sensing" : h->hp_plugged ? "headphones plugged in" : "headphones not plugged in");
    return h;
}

void hda_init(void) {
    char opt[64];
    if (qemu_fw_file("opt/halcyon/hda", opt, sizeof opt)) {
        opt_immediate = strstr(opt, "immediate") != NULL;
        opt_lpib = strstr(opt, "lpib") != NULL;
        opt_corbstall = strstr(opt, "corbstall") != NULL;
    }
    hda_t *found[SNDDEV_MAX];
    int n = 0;
    for (int i = 0; i < pci_count() && n < SNDDEV_MAX; i++) {
        pci_dev_t *pd = pci_get(i);
        if (pd->driver) continue;
        struct hda_id gen;
        const struct hda_id *id = NULL;
        for (unsigned k = 0; k < ARRAY_SIZE(ids) && !id; k++)
            if (pd->vendor == ids[k].vendor && pd->device == ids[k].device) id = &ids[k];
        if (!id) {
            if (pd->class_code != 0x04 || pd->subclass != 0x03) continue;
            by_vendor(pd, &gen);
            id = &gen;
        }
        hda_t *h = hda_attach(pd, id);
        if (h) found[n++] = h;
    }
    /* analog devices first: the default is the first device registered */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < n; i++)
            if (found[i]->sd.fallback == pass && snddev_register(&found[i]->sd))
                kprintf("hda: too many sound devices\n");
}
