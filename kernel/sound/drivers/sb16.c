/* Halcyon OS - Creative Sound Blaster 16 (ISA; QEMU -device sb16).
 *
 * Reference: Creative "Sound Blaster Series Hardware Programming Guide"
 * (DSP reset and version, DSP 4.xx commands, the SB16 mixer's interrupt and
 * DMA setup registers) and the Intel 8237A DMA controller data sheet (with
 * the PC/AT page registers and the second, 16-bit controller).
 *
 * Probing: only when no PCI sound card was found (an SB16 is ISA hardware
 * from the 1990s; PCI cards and chipsets with "SB compatible" emulation would
 * otherwise be claimed twice). The DSP is looked for at 0x220 and 0x240: a
 * reset must be answered with 0xAA within 100 us and the version (command
 * 0xE1) must be 4.xx or later. Ports where nothing answers read 0xFF, which is
 * never mistaken for either. The IRQ (2/9, 5, 7 or 10) and the 16-bit DMA
 * channel (5, 6 or 7) are read from the mixer's setup registers 0x80/0x81,
 * which the card's jumpers or its Plug and Play configuration set.
 *
 * Playback: 16-bit signed stereo at 44100 Hz (DSP command 0x41; the SB16
 * DSP tops out at 44.1 kHz, so the device runs at that rate and the mixer
 * resamples). The DMA buffer holds NPER periods of 10 ms (441 frames) and
 * the 8237 channel loops over it in auto-initialize mode; the DSP runs an
 * auto-init 16-bit output (0xB6) with one period per block, so it
 * interrupts after every period. The 8237's current count is the hardware
 * position that drives the ring of periods (loopring.h). Interrupts are
 * acknowledged by reading port 0x22F; poll does that whenever the mixer's
 * interrupt status (0x82) shows one, so the card works polled too. ISA
 * interrupts are edge-triggered and never shared, but the firmware may
 * declare the input level-triggered (QEMU's q35 does for IRQ 5, which it
 * keeps for PCI): the line is then masked when it fires until irq.c has
 * called the acknowledge (sb_irq_ack).
 *
 * The DMA buffer must be below 16 MiB (the 8237 has 24 address bits) and
 * must not cross a 128 KiB boundary (a 16-bit channel's address counter
 * covers 64 K words; the page register supplies the bits above). The heap
 * gives no such guarantee (it is the largest free region below 4 GiB, which
 * on a real PC can start anywhere), so the buffer is a static array in the
 * kernel's .bss: the kernel is loaded at 1 MiB and is a few MiB long, so
 * the array is below 16 MiB, and it is aligned to its own (power of two)
 * size, so it cannot cross a 64 or 128 KiB boundary. Both are checked.
 *
 * The mixer's master and voice levels are set to 0 dB; the master volume is
 * applied by the mixer. Tested in QEMU on the q35 machine (its ICH9 LPC
 * bridge has the ISA bus and both 8237s): ISA IRQ 5 through the I/O APIC and
 * the PIC, and polled. */
#include "sound/snddev.h"
#include "sound/drivers/loopring.h"
#include "core/kernel.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/io.h"
#include "core/irq.h"

/* DSP and mixer ports (base + ...) */
#define P_MIXER_ADDR    0x4
#define P_MIXER_DATA    0x5
#define P_RESET         0x6
#define P_READ          0xA
#define P_WRITE         0xC         /* write data / command; bit 7 on read: busy */
#define P_RSTATUS       0xE         /* bit 7: data to read; reading acks an 8-bit interrupt */
#define P_ACK16         0xF         /* reading acks a 16-bit interrupt */

/* DSP commands */
#define DSP_OUT_RATE    0x41
#define DSP_16_AUTO_OUT 0xB6        /* 16-bit, D/A, auto-init, FIFO on */
#define DSP_MODE_STEREO_SIGNED 0x30
#define DSP_SPEAKER_ON  0xD1
#define DSP_PAUSE_16    0xD5
#define DSP_EXIT_AUTO16 0xD9
#define DSP_VERSION     0xE1

/* mixer registers */
#define MX_MASTER_L     0x30        /* bits 7-3: 0..31, 31 = 0 dB */
#define MX_MASTER_R     0x31
#define MX_VOICE_L      0x32
#define MX_VOICE_R      0x33
#define MX_OUTGAIN_L    0x41        /* bits 7-6: x1, x2, x4, x8 */
#define MX_OUTGAIN_R    0x42
#define MX_IRQ_SELECT   0x80
#define MX_DMA_SELECT   0x81
#define MX_IRQ_STATUS   0x82        /* bit 0: 8-bit DMA, bit 1: 16-bit DMA, bit 2: MPU-401 */

#define RATE    44100
#define PERIOD  441
#define NPER    10                  /* 100 ms: QEMU's SB16 reads the DMA buffer in bursts of ~40 ms */
#define BUFSZ   (NPER * PERIOD * 4)             /* 17640 bytes */
#define BUFALIGN 32768                          /* power of two >= BUFSZ */

/* 8237 #2 (channels 4-7, word transfers) */
#define DMA2_MASK       0xD4
#define DMA2_MODE       0xD6
#define DMA2_FLIPFLOP   0xD8
#define DMA_MODE_READ   0x08        /* memory -> device */
#define DMA_MODE_AUTO   0x10
#define DMA_MODE_SINGLE 0x40

static u8 dmabuf[BUFALIGN] __attribute__((aligned(BUFALIGN)));

typedef struct sb16 {
    snddev_t sd;
    u16 base;
    int irqn, dma;                  /* ISA IRQ, 16-bit DMA channel (5..7) */
    int ver_major, ver_minor;
    loopring_t ring;
    irq_handle_t irq;
    char irq_name[24];
} sb16_t;

static sb16_t sb;

/* ---------------- DSP, mixer ---------------- */
static int dsp_write(u16 base, u8 v) {
    for (int i = 0; i < 10000; i++) {           /* ~10 ms */
        if (!(inb((u16)(base + P_WRITE)) & 0x80)) { outb((u16)(base + P_WRITE), v); return 0; }
        udelay(1);
    }
    return E_TIMEOUT;
}

static int dsp_read(u16 base) {
    for (int i = 0; i < 10000; i++) {
        if (inb((u16)(base + P_RSTATUS)) & 0x80) return inb((u16)(base + P_READ));
        udelay(1);
    }
    return -1;
}

/* reset the DSP: it must answer 0xAA (bytes left over from before the
 * reset are skipped; where nothing answers every port reads 0xFF) */
static int dsp_reset(u16 base) {
    outb((u16)(base + P_RESET), 1);
    udelay(5);                                  /* >= 3 us */
    outb((u16)(base + P_RESET), 0);
    for (int i = 0; i < 200; i++) {             /* the guide allows 100 us */
        if ((inb((u16)(base + P_RSTATUS)) & 0x80) && inb((u16)(base + P_READ)) == 0xAA) return 0;
        udelay(1);
    }
    return E_NODEV;
}

static u8 mixer_read(u16 base, u8 r) { outb((u16)(base + P_MIXER_ADDR), r); udelay(1); return inb((u16)(base + P_MIXER_DATA)); }
static void mixer_write(u16 base, u8 r, u8 v) { outb((u16)(base + P_MIXER_ADDR), r); udelay(1); outb((u16)(base + P_MIXER_DATA), v); }

/* ---------------- 8237 ---------------- */
static u16 dma_addr_port(int ch) { return (u16)(0xC0 + (ch - 4) * 4); }
static u16 dma_count_port(int ch) { return (u16)(0xC2 + (ch - 4) * 4); }
static u16 dma_page_port(int ch) {
    static const u8 page[4] = { 0x8F, 0x8B, 0x89, 0x8A };
    return page[ch - 4];
}

static void dma_mask(int ch, int on) { outb(DMA2_MASK, (u8)((ch & 3) | (on ? 4 : 0))); }

static void dma_program(int ch, u64 phys, u32 bytes) {
    u32 words = bytes / 2;
    u64 f = irq_save();                         /* one sequence through the byte flip-flop */
    dma_mask(ch, 1);
    outb(DMA2_FLIPFLOP, 0);
    outb(DMA2_MODE, (u8)((ch & 3) | DMA_MODE_READ | DMA_MODE_AUTO | DMA_MODE_SINGLE));
    u32 a = (u32)(phys >> 1);                   /* word address: A1-A16 */
    outb(dma_addr_port(ch), (u8)a);
    outb(dma_addr_port(ch), (u8)(a >> 8));
    outb(dma_page_port(ch), (u8)((phys >> 16) & 0xFE));  /* A17-A23 (bit 0 unused) */
    outb(DMA2_FLIPFLOP, 0);
    outb(dma_count_port(ch), (u8)(words - 1));
    outb(dma_count_port(ch), (u8)((words - 1) >> 8));
    dma_mask(ch, 0);
    irq_restore(f);
}

/* words still to transfer before the 8237 reloads (1..total) */
static u32 dma_remaining(int ch) {
    u16 c = dma_count_port(ch);
    u32 a, b;
    u64 f = irq_save();
    for (int i = 0; i < 4; i++) {
        outb(DMA2_FLIPFLOP, 0);
        a = inb(c); a |= (u32)inb(c) << 8;
        outb(DMA2_FLIPFLOP, 0);
        b = inb(c); b |= (u32)inb(c) << 8;
        if (a - b < 4 || b - a < 4) break;      /* the two reads agree (no carry in between) */
    }
    irq_restore(f);
    return ((b + 1) & 0xFFFF);
}

/* ---------------- snddev ---------------- */
/* acknowledge what the card is interrupting for; 1 if it was */
static int sb_ack(sb16_t *s) {
    u8 st = mixer_read(s->base, MX_IRQ_STATUS);
    if (st == 0xFF) return 0;
    if (st & 2) inb((u16)(s->base + P_ACK16));
    if (st & 1) inb((u16)(s->base + P_RSTATUS));
    return (st & 3) != 0;
}
static int sb_irq_ack(void *ctx) { return sb_ack(ctx); }

static int sb_start(snddev_t *sd) {
    sb16_t *s = sd->priv;
    if (dsp_reset(s->base)) return E_IO;
    lr_prime(sd, &s->ring);
    dma_program(s->dma, virt_to_phys(s->ring.buf), BUFSZ);
    int r = dsp_write(s->base, DSP_SPEAKER_ON);
    r |= dsp_write(s->base, DSP_OUT_RATE);
    r |= dsp_write(s->base, (u8)(RATE >> 8));
    r |= dsp_write(s->base, (u8)RATE);
    u32 n = PERIOD * 2 - 1;                     /* samples per block - 1 (both channels count) */
    r |= dsp_write(s->base, DSP_16_AUTO_OUT);
    r |= dsp_write(s->base, DSP_MODE_STEREO_SIGNED);
    r |= dsp_write(s->base, (u8)n);
    r |= dsp_write(s->base, (u8)(n >> 8));
    if (r) {
        dma_mask(s->dma, 1);
        dsp_reset(s->base);
        return E_IO;
    }
    return 0;
}

static void sb_stop(snddev_t *sd) {
    sb16_t *s = sd->priv;
    dsp_write(s->base, DSP_PAUSE_16);
    dsp_write(s->base, DSP_EXIT_AUTO16);
    dma_mask(s->dma, 1);
    sb_ack(s);
    dsp_reset(s->base);
}

static void sb_poll(snddev_t *sd) {
    sb16_t *s = sd->priv;
    sb_ack(s);
    u32 total = BUFSZ / 2;
    u32 words = total - dma_remaining(s->dma);  /* words transferred in this pass */
    if (lr_update(sd, &s->ring, words / 2)) irq_note_work(&s->irq);
    sd->irq_live = irq_live(&s->irq);
}

/* ---------------- probe ---------------- */
static int probe(u16 base) {
    if (dsp_reset(base)) return 0;
    if (dsp_write(base, DSP_VERSION)) return 0;
    int maj = dsp_read(base), min = dsp_read(base);
    if (maj < 0 || min < 0) return 0;
    if (maj < 4) {
        kprintf("sb16: DSP version %d.%02d at 0x%x: not a Sound Blaster 16, ignored\n", maj, min, base);
        return 0;
    }
    sb.base = base;
    sb.ver_major = maj;
    sb.ver_minor = min;
    return 1;
}

void sb16_init(void) {
    if (snddev_count()) return;                 /* a PCI card is there: never probe ISA ports */
    if (!probe(0x220) && !probe(0x240)) return;
    u8 irqsel = mixer_read(sb.base, MX_IRQ_SELECT), dmasel = mixer_read(sb.base, MX_DMA_SELECT);
    sb.irqn = irqsel & 2 ? 5 : irqsel & 4 ? 7 : irqsel & 8 ? 10 : irqsel & 1 ? 9 : -1;
    sb.dma = dmasel & 0x20 ? 5 : dmasel & 0x40 ? 6 : dmasel & 0x80 ? 7 : -1;
    if (sb.dma < 0) {
        kprintf("sb16: DSP %d.%02d at 0x%x: no 16-bit DMA channel (mixer 0x81 = %02x), not used\n",
                sb.ver_major, sb.ver_minor, sb.base, dmasel);
        return;
    }
    u64 phys = virt_to_phys(dmabuf);
    if (phys + BUFSZ > 0x1000000 || (phys >> 17) != ((phys + BUFSZ - 1) >> 17)) {
        kprintf("sb16: the DMA buffer at %p is not usable for ISA DMA\n", dmabuf);
        return;
    }
    memset(dmabuf, 0, sizeof dmabuf);
    sb.ring.buf = dmabuf;
    sb.ring.nper = NPER;
    sb.ring.period = PERIOD;
    dma_mask(sb.dma, 1);
    mixer_write(sb.base, MX_MASTER_L, 0xF8);    /* 0 dB */
    mixer_write(sb.base, MX_MASTER_R, 0xF8);
    mixer_write(sb.base, MX_VOICE_L, 0xF8);
    mixer_write(sb.base, MX_VOICE_R, 0xF8);
    mixer_write(sb.base, MX_OUTGAIN_L, 0x00);
    mixer_write(sb.base, MX_OUTGAIN_R, 0x00);
    sb_ack(&sb);

    snprintf(sb.irq_name, sizeof sb.irq_name, "sb16 0x%x", sb.base);
    sb.irq.name = sb.irq_name;
    sb.irq.ack = sb_irq_ack;            /* for a level-triggered input (see irq.c) */
    sb.irq.ctx = &sb;
    if (sb.irqn < 0) kprintf("sb16: no IRQ selected (mixer 0x80 = %02x), polled\n", irqsel);
    else if (irq_attach_isa(&sb.irq, sb.irqn) == IRQ_ISA)
        kprintf("irq: %s: ISA IRQ %d\n", sb.irq_name, sb.irqn);

    sb.sd.driver = "sb16";
    snprintf(sb.sd.desc, sizeof sb.sd.desc, "Creative Sound Blaster 16 (DSP %d.%02d) at 0x%x, IRQ %d, DMA %d",
             sb.ver_major, sb.ver_minor, sb.base, sb.irqn, sb.dma);
    strlcpy(sb.sd.output, "Line out", sizeof sb.sd.output);
    sb.sd.rate = RATE;
    sb.sd.channels = 2;
    sb.sd.period_frames = PERIOD;
    sb.sd.start = sb_start;
    sb.sd.stop = sb_stop;
    sb.sd.poll = sb_poll;
    sb.sd.priv = &sb;
    sb.sd.irq = &sb.irq;
    if (snddev_register(&sb.sd)) kprintf("sb16: too many sound devices\n");
}
