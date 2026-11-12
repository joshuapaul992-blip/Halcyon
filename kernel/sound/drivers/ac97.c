/* Halcyon OS - AC'97 audio on Intel ICH-style controllers (QEMU -device
 * AC97, the 82801AA; the ICH0..ICH7 and 440MX, and the nForce and AMD
 * controllers that copy their register set).
 *
 * Reference: Intel "I/O Controller Hub AC'97 Programmer's Reference Manual"
 * (bus master registers, buffer descriptor list) and the AC'97 2.3
 * specification (codec registers).
 *
 * Playback only, 48 kHz stereo 16-bit (every AC'97 codec runs at 48 kHz; the
 * variable-rate extension is not needed since the mixer resamples). The PCM
 * out box walks a 32-entry buffer descriptor list round robin; each entry is
 * one period of 10 ms, and the driver keeps LEAD periods queued ahead of the
 * hardware's current entry (CIV), moving the last valid index (LVI) as it
 * refills. If the DMA catches up with LVI it halts (an underrun): the box is
 * reset and primed again. The master volume is applied by the mixer; the
 * codec is set to 0 dB. Tested in QEMU (INTx through the I/O APIC and the
 * PIC, and polled); the other controllers are claimed by ID, untested. */
#include "sound/snddev.h"
#include "drivers/pci.h"
#include "core/kernel.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/io.h"
#include "core/irq.h"

/* codec (mixer) registers, NAMBAR */
#define AC_RESET        0x00
#define AC_MASTER       0x02
#define AC_HEADPHONE    0x04
#define AC_PCM_OUT      0x18
#define AC_POWERDOWN    0x26
#define AC_EXT_ID       0x28
#define AC_EXT_CTRL     0x2A
#define AC_DAC_RATE     0x2C
#define AC_VENDOR1      0x7C
#define AC_VENDOR2      0x7E

/* bus master registers, NABMBAR: the PCM out box and global control */
#define PO_BDBAR        0x10
#define PO_CIV          0x14
#define PO_LVI          0x15
#define PO_SR           0x16
#define PO_PICB         0x18
#define PO_CR           0x1B
#define GLOB_CNT        0x2C
#define GLOB_STA        0x30

#define SR_DCH          0x01        /* DMA halted */
#define SR_CELV         0x02        /* current = last valid */
#define SR_LVBCI        0x04        /* last valid buffer completed (write 1 to clear) */
#define SR_BCIS         0x08        /* buffer completed */
#define SR_FIFOE        0x10        /* FIFO error */
#define CR_RPBM         0x01        /* run */
#define CR_RR           0x02        /* reset the box's registers */
#define CR_LVBIE        0x04
#define CR_FEIE         0x08
#define CR_IOCE         0x10
#define GC_COLD         0x02        /* AC'97 cold reset# (1 = out of reset) */
#define GS_PCR          0x100       /* primary codec ready */
#define GS_POINT        0x40        /* PCM out interrupt */

#define NBDL    32
#define PERIOD  480                 /* frames: 10 ms at 48 kHz */
#define LEAD    4                   /* periods queued ahead of the hardware */
#define PBYTES  (PERIOD * 4)

typedef struct bdl_entry { u32 addr; u16 samples; u16 flags; } __attribute__((packed)) bdl_entry_t;
#define BDL_IOC 0x8000

typedef struct ac97 {
    snddev_t sd;
    pci_dev_t *pci;
    u16 nam, nabm;
    bdl_entry_t *bdl;
    u8 *buf;                        /* NBDL periods */
    int fill;                       /* next entry to fill */
    irq_handle_t irq;
    char irq_name[32];
} ac97_t;

static u16 cr16(ac97_t *a, u8 r) { return inw((u16)(a->nam + r)); }
static void cw16(ac97_t *a, u8 r, u16 v) { outw((u16)(a->nam + r), v); }
static u8 br8(ac97_t *a, u8 r) { return inb((u16)(a->nabm + r)); }
static u16 br16(ac97_t *a, u8 r) { return inw((u16)(a->nabm + r)); }
static u32 br32(ac97_t *a, u8 r) { return inl((u16)(a->nabm + r)); }
static void bw8(ac97_t *a, u8 r, u8 v) { outb((u16)(a->nabm + r), v); }
static void bw16(ac97_t *a, u8 r, u16 v) { outw((u16)(a->nabm + r), v); }
static void bw32(ac97_t *a, u8 r, u32 v) { outl((u16)(a->nabm + r), v); }

static int ac_ack(void *ctx) {
    ac97_t *a = ctx;
    u32 gs = br32(a, GLOB_STA);
    if (gs == 0xFFFFFFFF) return 0;
    u16 sr = br16(a, PO_SR);
    if (sr & (SR_LVBCI | SR_BCIS | SR_FIFOE)) bw16(a, PO_SR, sr & (SR_LVBCI | SR_BCIS | SR_FIFOE));
    return (gs & GS_POINT) != 0;
}

static void box_reset(ac97_t *a) {
    bw8(a, PO_CR, 0);
    for (int i = 0; i < 100 && !(br16(a, PO_SR) & SR_DCH); i++) udelay(10);
    bw8(a, PO_CR, CR_RR);
    for (int i = 0; i < 100 && (br8(a, PO_CR) & CR_RR); i++) udelay(10);
    bw16(a, PO_SR, SR_LVBCI | SR_BCIS | SR_FIFOE);
}

static void fill_entry(ac97_t *a, int i) {
    u8 *p = a->buf + i * PBYTES;
    snd_mix(&a->sd, (i16 *)p, PERIOD);
    a->bdl[i].addr = (u32)virt_to_phys(p);
    a->bdl[i].samples = PERIOD * 2;
    a->bdl[i].flags = BDL_IOC;
}

/* reset the box and queue LEAD periods from entry 0 */
static void prime(ac97_t *a) {
    box_reset(a);
    bw32(a, PO_BDBAR, (u32)virt_to_phys(a->bdl));
    for (int i = 0; i < LEAD; i++) fill_entry(a, i);
    a->fill = LEAD;
    bw8(a, PO_LVI, LEAD - 1);
    u8 cr = CR_RPBM;
    if (a->irq.kind != IRQ_NONE) cr |= CR_IOCE | CR_FEIE;
    bw8(a, PO_CR, cr);
}

static int ac_start(snddev_t *sd) {
    prime(sd->priv);
    return 0;
}

static void ac_stop(snddev_t *sd) {
    box_reset(sd->priv);
}

static void ac_poll(snddev_t *sd) {
    ac97_t *a = sd->priv;
    u16 sr = br16(a, PO_SR);
    if (sr == 0xFFFF) return;
    if (sr & (SR_LVBCI | SR_BCIS | SR_FIFOE)) bw16(a, PO_SR, sr & (SR_LVBCI | SR_BCIS | SR_FIFOE));
    if (sr & SR_DCH) {                  /* ran past the last queued period */
        sd->underruns++;
        prime(a);
        irq_note_work(&a->irq);
        sd->irq_live = irq_live(&a->irq);
        return;
    }
    int civ = br8(a, PO_CIV) & (NBDL - 1);
    int queued = (a->fill - civ + NBDL) % NBDL;     /* entries from the current one to fill-1 */
    int work = 0;
    while (queued < LEAD + 1) {         /* the current entry is still playing: LEAD after it */
        fill_entry(a, a->fill);
        bw8(a, PO_LVI, (u8)a->fill);
        a->fill = (a->fill + 1) % NBDL;
        queued++;
        work = 1;
    }
    if (work) irq_note_work(&a->irq);
    sd->irq_live = irq_live(&a->irq);
}

static const struct { u16 vendor, device; const char *name; } ids[] = {
    { 0x8086, 0x2415, "Intel 82801AA AC'97 Audio" }, { 0x8086, 0x2425, "Intel 82801AB AC'97 Audio" },
    { 0x8086, 0x2445, "Intel 82801BA AC'97 Audio" }, { 0x8086, 0x2485, "Intel 82801CA AC'97 Audio" },
    { 0x8086, 0x24C5, "Intel 82801DB AC'97 Audio" }, { 0x8086, 0x24D5, "Intel 82801EB AC'97 Audio" },
    { 0x8086, 0x25A6, "Intel 6300ESB AC'97 Audio" }, { 0x8086, 0x266E, "Intel 82801FB AC'97 Audio" },
    { 0x8086, 0x27DE, "Intel 82801GB AC'97 Audio" }, { 0x8086, 0x7195, "Intel 440MX AC'97 Audio" },
    { 0x10DE, 0x01B1, "NVIDIA nForce AC'97 Audio" }, { 0x10DE, 0x006A, "NVIDIA nForce2 AC'97 Audio" },
    { 0x10DE, 0x00DA, "NVIDIA nForce3 AC'97 Audio" }, { 0x10DE, 0x00EA, "NVIDIA nForce3 AC'97 Audio" },
    { 0x10DE, 0x0059, "NVIDIA CK804 AC'97 Audio" }, { 0x10DE, 0x008A, "NVIDIA CK8 AC'97 Audio" },
    { 0x10DE, 0x003A, "NVIDIA MCP04 AC'97 Audio" }, { 0x10DE, 0x026B, "NVIDIA MCP51 AC'97 Audio" },
    { 0x1022, 0x746D, "AMD-8111 AC'97 Audio" }, { 0x1022, 0x7445, "AMD-768 AC'97 Audio" },
};

static void ac_attach(pci_dev_t *pd, const char *name) {
    ac97_t *a = kzalloc(sizeof *a);
    if (!a) return;
    a->pci = pd;
    /* ICH4 and later decode the legacy I/O BARs only with IOSE set */
    if (pd->vendor == 0x8086 && pd->device >= 0x24C5 && pd->device != 0x7195)
        pci_write8(pd, 0x41, (u8)(pci_read8(pd, 0x41) | 1));
    pci_enable(pd);
    if (!pd->bar[0].is_io || !pd->bar[1].is_io || !pd->bar[0].addr || !pd->bar[1].addr) {
        kprintf("ac97: %02x:%02x.%x: no I/O BARs\n", pd->bus, pd->dev, pd->func);
        kfree(a);
        return;
    }
    a->nam = (u16)pd->bar[0].addr;
    a->nabm = (u16)pd->bar[1].addr;
    a->bdl = dma_alloc(NBDL * sizeof(bdl_entry_t), 8);
    a->buf = dma_alloc(NBDL * PBYTES, 16);
    if (!a->bdl || !a->buf || virt_to_phys(a->buf) + NBDL * PBYTES > 0x100000000ull ||
        virt_to_phys(a->bdl) + NBDL * sizeof(bdl_entry_t) > 0x100000000ull) {
        kprintf("ac97: out of memory\n");
        dma_free(a->bdl); dma_free(a->buf); kfree(a);
        return;
    }
    memset(a->bdl, 0, NBDL * sizeof(bdl_entry_t));
    memset(a->buf, 0, NBDL * PBYTES);
    /* the AC-link out of reset (a cold reset if it was held), then the codec */
    u32 gc = br32(a, GLOB_CNT);
    if (!(gc & GC_COLD)) {
        bw32(a, GLOB_CNT, gc | GC_COLD);
    } else {
        bw32(a, GLOB_CNT, gc & ~(u32)GC_COLD);
        udelay(100);
        bw32(a, GLOB_CNT, gc | GC_COLD);
    }
    int ready = 0;
    for (int i = 0; i < 1000; i++) {
        if (br32(a, GLOB_STA) & GS_PCR) { ready = 1; break; }
        mdelay(1);
    }
    if (!ready) {
        kprintf("ac97: %02x:%02x.%x: no codec answered\n", pd->bus, pd->dev, pd->func);
        dma_free(a->bdl); dma_free(a->buf); kfree(a);
        return;
    }
    cw16(a, AC_RESET, 0);
    for (int i = 0; i < 1000 && (cr16(a, AC_POWERDOWN) & 0xF) != 0xF; i++) mdelay(1);
    cw16(a, AC_MASTER, 0x0000);          /* 0 dB, unmuted */
    cw16(a, AC_HEADPHONE, 0x0000);
    cw16(a, AC_PCM_OUT, 0x0808);         /* 0 dB */
    if (cr16(a, AC_EXT_ID) & 1) {        /* variable rate: pin it to 48 kHz */
        cw16(a, AC_EXT_CTRL, (u16)(cr16(a, AC_EXT_CTRL) | 1));
        cw16(a, AC_DAC_RATE, 48000);
    }
    u32 codec = (u32)cr16(a, AC_VENDOR1) << 16 | cr16(a, AC_VENDOR2);
    box_reset(a);

    snprintf(a->irq_name, sizeof a->irq_name, "ac97 %02x:%02x.%x", pd->bus, pd->dev, pd->func);
    a->irq.name = a->irq_name;
    a->irq.pci = pd;
    a->irq.ack = ac_ack;
    a->irq.ctx = a;
    irq_attach(&a->irq);

    a->sd.driver = "ac97";
    snprintf(a->sd.desc, sizeof a->sd.desc, "%s, codec %08x", name, codec);
    strlcpy(a->sd.output, "Line out", sizeof a->sd.output);
    a->sd.rate = 48000;
    a->sd.channels = 2;
    a->sd.period_frames = PERIOD;
    a->sd.start = ac_start;
    a->sd.stop = ac_stop;
    a->sd.poll = ac_poll;
    a->sd.priv = a;
    a->sd.pci = pd;
    a->sd.irq = &a->irq;
    pd->driver = "ac97";
    if (snddev_register(&a->sd)) kprintf("ac97: too many sound devices\n");
}

void ac97_init(void) {
    for (int i = 0; i < pci_count(); i++) {
        pci_dev_t *pd = pci_get(i);
        if (pd->driver) continue;
        for (unsigned k = 0; k < ARRAY_SIZE(ids); k++)
            if (pd->vendor == ids[k].vendor && pd->device == ids[k].device) { ac_attach(pd, ids[k].name); break; }
    }
}
