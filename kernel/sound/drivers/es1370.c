/* Halcyon OS - Ensoniq AudioPCI ES1370 (QEMU -device ES1370; the Creative
 * Sound Blaster PCI64/PCI128 of 1998 are this chip).
 *
 * Reference: Ensoniq ES1370 AudioPCI specification (control/status, serial
 * interface, sample counter and frame registers) and the Asahi Kasei AK4531
 * codec data sheet (the mixer behind it).
 *
 * Playback only, on DAC2 (the "voice" DAC; DAC1 is meant for FM synthesis
 * and only has four fixed rates). DAC2's rate comes from the 1411200 Hz
 * master clock divided by PCLKDIV + 2: 44100 Hz exactly (divider 30), while
 * 48000 Hz is not reachable (1411200 / 29.4), so the device runs at 44.1 kHz
 * and the mixer resamples to it. DAC2 reads a buffer of NPER periods of
 * 10 ms (441 frames) through its frame registers (address and size in
 * longwords, memory page 0x0C) in loop mode; the sample counter raises the
 * DAC2 interrupt after every period. The frame register's upper half is the
 * hardware's current position in the buffer, which drives the ring of
 * periods (loopring.h). The interrupt is acknowledged by clearing and
 * setting P2_INTR_EN in the serial interface control register.
 *
 * The AK4531's master and voice inputs are set to 0 dB and the voice input
 * is routed to the output mixer; the master volume is applied by the mixer.
 * Tested in QEMU (INTx through the I/O APIC and the PIC, and polled). */
#include "sound/snddev.h"
#include "sound/drivers/loopring.h"
#include "drivers/pci.h"
#include "core/kernel.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/io.h"
#include "core/irq.h"

/* registers (I/O BAR 0) */
#define R_CONTROL       0x00
#define R_STATUS        0x04
#define R_MEMPAGE       0x0C
#define R_CODEC         0x10        /* AK4531 write: register << 8 | value */
#define R_SCTRL         0x20        /* serial interface control */
#define R_DAC2_SCOUNT   0x28        /* samples per interrupt - 1 (low), current count (high) */
#define R_DAC2_FRAME    0x38        /* memory page 0x0C: frame address */
#define R_DAC2_SIZE     0x3C        /* memory page 0x0C: longwords - 1 (low), current longword (high) */
#define PAGE_DAC        0x0C

#define CTRL_SERR_DIS   0x00000001
#define CTRL_CDC_EN     0x00000002
#define CTRL_DAC2_EN    0x00000020
#define CTRL_PCLKDIV(d) ((u32)(d) << 16)       /* 13 bits: DAC2 rate = 1411200 / (d + 2) */

#define STAT_INTR       0x80000000u
#define STAT_CSTAT      0x00000400  /* codec busy */
#define STAT_DAC2       0x00000002

#define SCTRL_P2SMB     0x00000004  /* DAC2 stereo */
#define SCTRL_P2SEB     0x00000008  /* DAC2 16-bit */
#define SCTRL_P2INTEN   0x00000200
#define SCTRL_P2PAUSE   0x00001000
#define SCTRL_P2LOOPSEL 0x00004000  /* 1 = stop at the end of the buffer, 0 = loop */
#define SCTRL_P2ENDINC(x) ((u32)(x) << 19)    /* address increment at the end of a sample: 2 for 16-bit */
#define SCTRL_P2STINC(x)  ((u32)(x) << 16)

/* AK4531 */
#define AK_MASTER_L     0x00        /* attenuation, 2 dB steps (0 = 0 dB), bit 7 mute */
#define AK_MASTER_R     0x01
#define AK_VOICE_L      0x02        /* gain +12..-50 dB in 2 dB steps (6 = 0 dB), bit 7 mute */
#define AK_VOICE_R      0x03
#define AK_OUTSW1       0x10        /* output mixer switches */
#define AK_OUTSW2       0x11        /* bit 3 voice L, bit 2 voice R */
#define AK_RESET        0x16        /* bit 0: 0 = reset; bit 1: 0 = power down */
#define AK_CLOCK        0x17

#define RATE    44100
#define PCLKDIV (1411200 / RATE - 2)
#define PERIOD  441                 /* frames: 10 ms */
#define NPER    5                   /* periods in the buffer: 4 ahead of the hardware */

typedef struct es1370 {
    snddev_t sd;
    pci_dev_t *pci;
    u16 io;
    u32 ctrl, sctrl;
    loopring_t ring;
    irq_handle_t irq;
    char irq_name[32];
} es1370_t;

static u32 rd32(es1370_t *e, u8 r) { return inl((u16)(e->io + r)); }
static void wr32(es1370_t *e, u8 r, u32 v) { outl((u16)(e->io + r), v); }

static void codec_write(es1370_t *e, u8 reg, u8 val) {
    for (int i = 0; i < 1000 && (rd32(e, R_STATUS) & STAT_CSTAT); i++) udelay(10);
    outw((u16)(e->io + R_CODEC), (u16)(reg << 8 | val));
}

static int es_ack(void *ctx) {
    es1370_t *e = ctx;
    u32 st = rd32(e, R_STATUS);
    if (st == 0xFFFFFFFF || !(st & STAT_INTR)) return 0;
    if (st & STAT_DAC2) {
        wr32(e, R_SCTRL, e->sctrl & ~(u32)SCTRL_P2INTEN);
        wr32(e, R_SCTRL, e->sctrl);
    }
    return 1;
}

/* the hardware's position in the buffer, in frames (longwords) */
static u32 hw_pos(es1370_t *e) {
    outb((u16)(e->io + R_MEMPAGE), PAGE_DAC);
    return rd32(e, R_DAC2_SIZE) >> 16;
}

static void dac_stop(es1370_t *e) {
    e->ctrl &= ~(u32)CTRL_DAC2_EN;
    wr32(e, R_CONTROL, e->ctrl);
    e->sctrl &= ~(u32)SCTRL_P2INTEN;
    wr32(e, R_SCTRL, e->sctrl);
}

static int es_start(snddev_t *sd) {
    es1370_t *e = sd->priv;
    dac_stop(e);
    lr_prime(sd, &e->ring);
    outb((u16)(e->io + R_MEMPAGE), PAGE_DAC);
    wr32(e, R_DAC2_FRAME, (u32)virt_to_phys(e->ring.buf));
    wr32(e, R_DAC2_SIZE, NPER * PERIOD - 1);          /* longwords - 1; the position restarts at 0 */
    wr32(e, R_DAC2_SCOUNT, PERIOD - 1);
    e->sctrl &= ~(u32)(SCTRL_P2LOOPSEL | SCTRL_P2PAUSE | SCTRL_P2ENDINC(7) | SCTRL_P2STINC(7));
    e->sctrl |= SCTRL_P2SMB | SCTRL_P2SEB | SCTRL_P2ENDINC(2);
    if (e->irq.kind != IRQ_NONE) e->sctrl |= SCTRL_P2INTEN;
    wr32(e, R_SCTRL, e->sctrl);
    e->ctrl |= CTRL_DAC2_EN;
    wr32(e, R_CONTROL, e->ctrl);
    return 0;
}

static void es_stop(snddev_t *sd) {
    dac_stop(sd->priv);
}

static void es_poll(snddev_t *sd) {
    es1370_t *e = sd->priv;
    if (rd32(e, R_STATUS) == 0xFFFFFFFF) return;
    if (lr_update(sd, &e->ring, hw_pos(e))) irq_note_work(&e->irq);
    sd->irq_live = irq_live(&e->irq);
}

static void es_attach(pci_dev_t *pd) {
    if (!pd->bar[0].is_io || !pd->bar[0].addr) {
        kprintf("es1370: %02x:%02x.%x: no I/O BAR\n", pd->bus, pd->dev, pd->func);
        return;
    }
    es1370_t *e = kzalloc(sizeof *e);
    if (!e) return;
    e->pci = pd;
    pci_enable(pd);
    e->io = (u16)pd->bar[0].addr;
    e->ring.nper = NPER;
    e->ring.period = PERIOD;
    e->ring.buf = dma_alloc(NPER * PERIOD * 4, 4096);
    if (!e->ring.buf || virt_to_phys(e->ring.buf) + NPER * PERIOD * 4 > 0x100000000ull) {
        kprintf("es1370: out of memory\n");
        dma_free(e->ring.buf); kfree(e);
        return;
    }
    /* everything off, the codec interface on, DAC2 at 44.1 kHz */
    e->ctrl = CTRL_CDC_EN | CTRL_SERR_DIS | CTRL_PCLKDIV(PCLKDIV);
    wr32(e, R_CONTROL, e->ctrl);
    e->sctrl = 0;
    wr32(e, R_SCTRL, 0);
    /* the AK4531: out of reset and power-down, its clock from the ES1370,
     * master and voice at 0 dB, the voice (DAC2) into the output mixer */
    codec_write(e, AK_RESET, 0x02);
    udelay(100);
    codec_write(e, AK_RESET, 0x03);
    udelay(100);
    codec_write(e, AK_CLOCK, 0x00);
    codec_write(e, AK_MASTER_L, 0x00);
    codec_write(e, AK_MASTER_R, 0x00);
    codec_write(e, AK_VOICE_L, 0x06);
    codec_write(e, AK_VOICE_R, 0x06);
    codec_write(e, AK_OUTSW1, 0x00);
    codec_write(e, AK_OUTSW2, 0x0C);

    snprintf(e->irq_name, sizeof e->irq_name, "es1370 %02x:%02x.%x", pd->bus, pd->dev, pd->func);
    e->irq.name = e->irq_name;
    e->irq.pci = pd;
    e->irq.ack = es_ack;
    e->irq.ctx = e;
    e->irq.intx_only = 1;           /* no MSI on this chip anyway */
    irq_attach(&e->irq);

    e->sd.driver = "es1370";
    snprintf(e->sd.desc, sizeof e->sd.desc, "Ensoniq AudioPCI ES1370 (rev %u)", pd->revision);
    strlcpy(e->sd.output, "Line out", sizeof e->sd.output);
    e->sd.rate = RATE;
    e->sd.channels = 2;
    e->sd.period_frames = PERIOD;
    e->sd.start = es_start;
    e->sd.stop = es_stop;
    e->sd.poll = es_poll;
    e->sd.priv = e;
    e->sd.pci = pd;
    e->sd.irq = &e->irq;
    pd->driver = "es1370";
    if (snddev_register(&e->sd)) kprintf("es1370: too many sound devices\n");
}

void es1370_init(void) {
    for (int i = 0; i < pci_count(); i++) {
        pci_dev_t *pd = pci_get(i);
        if (pd->driver || pd->vendor != 0x1274) continue;
        if (pd->device == 0x5000) es_attach(pd);
    }
}
