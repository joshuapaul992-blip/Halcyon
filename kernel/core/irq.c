/* Halcyon OS - interrupt controllers, vectors, MSI / INTx, timer, idle (see irq.h) */
#include "kernel.h"
#include "lib.h"
#include "io.h"
#include "irq.h"
#include "drivers/acpi.h"
#include "drivers/pci.h"

/* vector map */
#define VEC_PIC_BASE  0x20      /* 8259 IRQ 0-15 (PIC mode) */
#define VEC_ISA_BASE  0x30      /* ISA IRQ 0-15 through the I/O APIC */
#define VEC_DYN_FIRST 0x40      /* MSI and PCI INTx lines */
#define VEC_DYN_LAST  0xEF
#define VEC_TIMER     0xF0
#define VEC_ERROR     0xFE
#define VEC_SPURIOUS  0xFF

enum { V_FREE = 0, V_HANDLE, V_LINE, V_TIMER, V_SYS };

typedef struct line line_t;
struct vec {
    u8 kind;
    void *ptr;              /* irq_handle_t (V_HANDLE) or line_t (V_LINE) */
    u64 count;
};
static struct vec vecs[256];

static int mode = IRQ_MODE_POLL;
static int msi_ok;
static volatile u32 dev_gen;
static volatile u64 total_irqs, spurious_irqs;
static irq_handle_t *handles;

/* ---------------- local APIC ---------------- */
#define LAPIC_ID     0x020
#define LAPIC_VER    0x030
#define LAPIC_TPR    0x080
#define LAPIC_EOI    0x0B0
#define LAPIC_SVR    0x0F0
#define LAPIC_ESR    0x280
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_LVT_THERM 0x330
#define LAPIC_LVT_PERF  0x340
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define LAPIC_LVT_ERR   0x370
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR  0x390
#define LAPIC_TIMER_DIV  0x3E0
#define LVT_MASKED   0x10000

static volatile u8 *lapic_mmio;
static int have_lapic, x2apic;
static u32 lapic_id;
static u32 lapic_esr_last;

static u32 lapic_rd(u32 reg) {
    if (x2apic) return (u32)rdmsr(0x800 + (reg >> 4));
    return *(volatile u32 *)(lapic_mmio + reg);
}
static void lapic_wr(u32 reg, u32 v) {
    if (x2apic) wrmsr(0x800 + (reg >> 4), v);
    else *(volatile u32 *)(lapic_mmio + reg) = v;
}
static void lapic_eoi(void) { lapic_wr(LAPIC_EOI, 0); }

static int lapic_init(const acpi_madt_t *m) {
    u32 a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    if (!(d & (1u << 9))) { kprintf("irq: CPU has no local APIC\n"); return -1; }
    u64 base = rdmsr(0x1B);
    if (!(base & (1u << 11))) {
        wrmsr(0x1B, base | (1u << 11));
        base = rdmsr(0x1B);
        if (!(base & (1u << 11))) { kprintf("irq: local APIC disabled by firmware\n"); return -1; }
    }
    x2apic = (base >> 10) & 1;
    if (!x2apic) {
        u64 phys = base & 0xFFFFF000ull;
        if (m && m->lapic_addr && m->lapic_addr != phys)
            kprintf("irq: MADT LAPIC address %#x differs from MSR %#lx, using the MSR\n", m->lapic_addr, phys);
        if (!phys || phys >= 0x100000000ull) { kprintf("irq: local APIC above 4 GiB\n"); return -1; }
        lapic_mmio = (volatile u8 *)(uptr)phys;
    }
    lapic_id = x2apic ? lapic_rd(LAPIC_ID) : lapic_rd(LAPIC_ID) >> 24;
    u32 ver = lapic_rd(LAPIC_VER);
    int maxlvt = (ver >> 16) & 0xFF;
    lapic_wr(LAPIC_TPR, 0);
    lapic_wr(LAPIC_LVT_TIMER, LVT_MASKED | VEC_TIMER);
    if (maxlvt >= 4) lapic_wr(LAPIC_LVT_PERF, LVT_MASKED);
    if (maxlvt >= 5) lapic_wr(LAPIC_LVT_THERM, LVT_MASKED);
    lapic_wr(LAPIC_LVT_ERR, VEC_ERROR);
    lapic_wr(LAPIC_SVR, 0x100 | VEC_SPURIOUS);
    lapic_wr(LAPIC_ESR, 0);
    lapic_wr(LAPIC_ESR, 0);
    lapic_eoi();
    have_lapic = 1;
    kprintf("irq: local APIC id %u, version %#x, %d LVT entries, %s mode\n", lapic_id, ver & 0xFF, maxlvt + 1,
            x2apic ? "x2APIC" : "xAPIC");
    return 0;
}

/* LINT0 carries the 8259 (ExtINT) in PIC mode and is masked with I/O APICs;
 * LINT1 is the NMI input (MADT says which pin) */
static void lapic_lints(const acpi_madt_t *m, int pic_mode) {
    int nmi = m && m->nmi_lint >= 0 ? m->nmi_lint : 1;
    u32 nmi_lvt = 0x400 | (m && (m->nmi_flags & 3) == 3 ? (1u << 13) : 0);
    u32 l0 = pic_mode ? 0x700 : (LVT_MASKED | 0x700);
    u32 l1 = LVT_MASKED;
    if (nmi == 0) l0 = nmi_lvt; else l1 = nmi_lvt;
    if (nmi == 0 && pic_mode) kprintf("irq: NMI on LINT0, 8259 cascade unavailable\n");
    lapic_wr(LAPIC_LVT_LINT0, l0);
    lapic_wr(LAPIC_LVT_LINT1, l1);
}

/* ---------------- 8259 PIC ---------------- */
static u16 pic_mask = 0xFFFF;

static void pic_write_mask(void) { outb(0x21, (u8)pic_mask); outb(0xA1, (u8)(pic_mask >> 8)); }

static void pic_remap(void) {
    outb(0x20, 0x11); io_wait(); outb(0xA0, 0x11); io_wait();
    outb(0x21, VEC_PIC_BASE); io_wait(); outb(0xA1, VEC_PIC_BASE + 8); io_wait();
    outb(0x21, 4); io_wait(); outb(0xA1, 2); io_wait();
    outb(0x21, 1); io_wait(); outb(0xA1, 1); io_wait();
    pic_mask = 0xFFFF;
    pic_write_mask();
}
static void pic_set(int irq, int masked) {
    u64 f = irq_save();
    if (masked) pic_mask |= (u16)(1u << irq);
    else {
        pic_mask &= (u16)~(1u << irq);
        if (irq >= 8) pic_mask &= (u16)~(1u << 2);
    }
    pic_write_mask();
    irq_restore(f);
}
static void pic_eoi(int irq) {
    if (irq >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}
static int pic_in_service(int irq) {
    u16 port = irq >= 8 ? 0xA0 : 0x20;
    outb(port, 0x0B);
    return (inb(port) >> (irq & 7)) & 1;
}

/* ---------------- I/O APIC ---------------- */
typedef struct ioapic { volatile u32 *base; u32 gsi_base; int n; u8 id; } ioapic_t;
static ioapic_t ioapics[MADT_MAX_IOAPIC];
static int nioapic;
static const acpi_madt_t *madt;

static u32 ioa_rd(ioapic_t *io, u32 reg) { io->base[0] = reg; return io->base[4]; }
static void ioa_wr(ioapic_t *io, u32 reg, u32 v) { io->base[0] = reg; io->base[4] = v; }

static ioapic_t *gsi_ioapic(u32 gsi, int *pin) {
    for (int i = 0; i < nioapic; i++)
        if (gsi >= ioapics[i].gsi_base && gsi < ioapics[i].gsi_base + (u32)ioapics[i].n) {
            *pin = (int)(gsi - ioapics[i].gsi_base);
            return &ioapics[i];
        }
    return NULL;
}

#define RTE_MASKED (1u << 16)
#define RTE_LEVEL  (1u << 15)
#define RTE_LOW    (1u << 13)

static int gsi_program(u32 gsi, int vector, int level, int low, int masked) {
    int pin;
    ioapic_t *io = gsi_ioapic(gsi, &pin);
    if (!io) return -1;
    u32 lo = (u32)vector | (level ? RTE_LEVEL : 0) | (low ? RTE_LOW : 0) | (masked ? RTE_MASKED : 0);
    u64 f = irq_save();
    ioa_wr(io, 0x10 + 2 * pin, RTE_MASKED);
    ioa_wr(io, 0x11 + 2 * pin, lapic_id << 24);
    ioa_wr(io, 0x10 + 2 * pin, lo);
    irq_restore(f);
    return 0;
}
static void gsi_mask(u32 gsi, int masked) {
    int pin;
    ioapic_t *io = gsi_ioapic(gsi, &pin);
    if (!io) return;
    u64 f = irq_save();
    u32 lo = ioa_rd(io, 0x10 + 2 * pin);
    ioa_wr(io, 0x10 + 2 * pin, masked ? (lo | RTE_MASKED) : (lo & ~RTE_MASKED));
    irq_restore(f);
}

static int ioapic_init(void) {
    for (int i = 0; i < madt->nioapic && nioapic < MADT_MAX_IOAPIC; i++) {
        const madt_ioapic_t *m = &madt->ioapic[i];
        ioapic_t *io = &ioapics[nioapic];
        io->base = (volatile u32 *)(uptr)m->addr;
        io->gsi_base = m->gsi_base;
        io->id = m->id;
        u32 ver = ioa_rd(io, 1);
        if (ver == 0xFFFFFFFF) { kprintf("irq: I/O APIC %u at %#x not responding\n", m->id, m->addr); continue; }
        io->n = (int)((ver >> 16) & 0xFF) + 1;
        for (int p = 0; p < io->n; p++) { ioa_wr(io, 0x10 + 2 * p, RTE_MASKED); ioa_wr(io, 0x11 + 2 * p, 0); }
        kprintf("irq: I/O APIC %u: version %#x, GSI %u-%u\n", io->id, ver & 0xFF, io->gsi_base, io->gsi_base + io->n - 1);
        nioapic++;
    }
    return nioapic ? 0 : -1;
}

/* ISA IRQ -> GSI with polarity / trigger (MADT overrides, else edge high) */
static u32 isa_gsi(int irq, int *level, int *low) {
    *level = 0; *low = 0;
    for (int i = 0; madt && i < madt->niso; i++) {
        const madt_iso_t *o = &madt->iso[i];
        if (o->bus != 0 || o->irq != irq) continue;
        int pol = o->flags & 3, trg = (o->flags >> 2) & 3;
        *low = pol == 3;
        *level = trg == 3;
        return o->gsi;
    }
    return (u32)irq;
}
static int gsi_is_isa(u32 gsi) {
    for (int irq = 0; irq < 16; irq++) { int l, p; if (isa_gsi(irq, &l, &p) == gsi) return 1; }
    return 0;
}

/* ---------------- PCI INTx lines ---------------- */
#define MAX_LINES 48
struct line {
    int num;                /* GSI or PIC IRQ */
    int vector;
    u8 used, pending, foreign, dead, level, logged;
    irq_handle_t *clients;
    u32 fires, unhandled, stuck;
};
static line_t lines[MAX_LINES];
static int nlines;
static int learning;         /* candidate I/O APIC inputs are unmasked */

static int alloc_vector(int kind, void *ptr) {
    for (int v = VEC_DYN_FIRST; v <= VEC_DYN_LAST; v++)
        if (vecs[v].kind == V_FREE) { vecs[v].kind = (u8)kind; vecs[v].ptr = ptr; vecs[v].count = 0; return v; }
    return -1;
}

static void line_mask(line_t *l, int masked) {
    if (mode == IRQ_MODE_PIC) pic_set(l->num, masked);
    else gsi_mask((u32)l->num, masked);
}

static line_t *line_get(int num) {
    for (int i = 0; i < nlines; i++) if (lines[i].num == num) return &lines[i];
    if (nlines >= MAX_LINES) return NULL;
    line_t *l = &lines[nlines];
    memset(l, 0, sizeof *l);
    l->num = num;
    if (mode == IRQ_MODE_PIC) {
        l->vector = VEC_PIC_BASE + num;
        l->level = (inb((u16)(0x4D0 + (num >> 3))) >> (num & 7)) & 1;   /* ELCR */
    } else {
        l->vector = alloc_vector(V_LINE, l);
        if (l->vector < 0) return NULL;
        l->level = 1;
        if (gsi_program((u32)num, l->vector, 1, 1, 1) < 0) { vecs[l->vector].kind = V_FREE; return NULL; }
    }
    vecs[l->vector].kind = V_LINE;
    vecs[l->vector].ptr = l;
    l->used = 1;
    nlines++;
    return l;
}

static int is_learner(pci_dev_t *d) {
    for (irq_handle_t *h = handles; h; h = h->next_all)
        if (h->pci == d && h->kind == IRQ_INTX) return 1;
    return 0;
}

/* Unmask every I/O APIC input that nobody claims, to learn PCI INTx routes.
 * Devices without an INTx user are told not to assert INTx first (firmware
 * may have left one interrupting); a line that fired with nobody to claim it
 * is masked, and armed again when another device starts learning. */
static void learning_start(void) {
    if (mode != IRQ_MODE_APIC) return;
    for (int i = 0; i < pci_count(); i++) {
        pci_dev_t *d = pci_get(i);
        if (d->header_type & 0x7F) continue;     /* bridges */
        if (!is_learner(d)) pci_write16(d, 0x04, pci_read16(d, 0x04) | (1u << 10));
    }
    if (learning) {
        for (int i = 0; i < nlines; i++)
            if (lines[i].foreign && !lines[i].dead) { lines[i].foreign = 0; line_mask(&lines[i], 0); }
        return;
    }
    learning = 1;
    int n = 0;
    for (int i = 0; i < nioapic; i++)
        for (int p = 0; p < ioapics[i].n; p++) {
            u32 gsi = ioapics[i].gsi_base + (u32)p;
            if (gsi < 16 || gsi_is_isa(gsi)) continue;
            line_t *l = line_get((int)gsi);
            if (!l || l->foreign || l->dead) continue;
            line_mask(l, 0);
            n++;
        }
    kprintf("irq: learning PCI INTx routes on %d I/O APIC inputs\n", n);
}

static void bind(irq_handle_t *h, line_t *l) {
    h->line = l->num;
    h->vector = l->vector;
    h->next_on_line = l->clients;
    l->clients = h;
    l->foreign = 0;
    kprintf("irq: %s: INTx on %s %d\n", h->name, mode == IRQ_MODE_PIC ? "IRQ" : "GSI", l->num);
}

static int pci_int_status(irq_handle_t *h) {
    return h->pci && (pci_read16(h->pci, 0x06) & (1u << 3));
}

void irq_service(void) {
    if (mode == IRQ_MODE_POLL) return;
    for (int i = 0; i < nlines; i++) {
        line_t *l = &lines[i];
        if (!l->pending) continue;
        u64 f = irq_save();
        l->pending = 0;
        irq_restore(f);
        if (l->dead) continue;
        /* devices still waiting for a route that are asserting right now */
        for (irq_handle_t *h = handles; h; h = h->next_all)
            if (h->kind == IRQ_INTX && h->line < 0 && pci_int_status(h)) bind(h, l);
        if (!l->clients) {
            if (!l->logged) kprintf("irq: %s %d asserted by a device without a driver, masked\n",
                                    mode == IRQ_MODE_PIC ? "IRQ" : "GSI", l->num);
            l->logged = 1;
            l->foreign = 1;
            continue;
        }
        int handled = 0;
        for (irq_handle_t *h = l->clients; h; h = h->next_on_line)
            if (h->ack && h->ack(h->ctx)) { h->events++; handled = 1; }
        line_mask(l, 0);
        if (handled) { l->stuck = 0; continue; }
        /* Nobody was asserting: normal when a driver already cleared its
         * status. A line held by some other device re-fires the moment it
         * is unmasked; after a few such rounds it is given up on. */
        l->unhandled++;
        udelay(20);
        int asserted = 0;
        for (irq_handle_t *h = l->clients; h; h = h->next_on_line) asserted |= pci_int_status(h);
        if (!l->pending || asserted) { l->stuck = 0; continue; }
        if (++l->stuck >= 3) {
            line_mask(l, 1);
            l->dead = 1;
            kprintf("irq: %s %d is held asserted by another device: disabled, its devices are polled\n",
                    mode == IRQ_MODE_PIC ? "IRQ" : "GSI", l->num);
            for (irq_handle_t *h = l->clients; h; h = h->next_on_line) h->broken = 1;
        }
    }
}

/* ---------------- dispatch ---------------- */
static volatile u64 timer_fires;

void irq_dispatch(u64 vector) {
    struct vec *v = &vecs[vector & 0xFF];
    total_irqs++;
    if (vector == VEC_SPURIOUS) { spurious_irqs++; return; }
    if (mode == IRQ_MODE_PIC && vector >= VEC_PIC_BASE && vector < VEC_PIC_BASE + 16) {
        int irq = (int)vector - VEC_PIC_BASE;
        if ((irq == 7 || irq == 15) && !pic_in_service(irq)) {
            spurious_irqs++;
            if (irq == 15) outb(0x20, 0x20);
            return;
        }
        v->count++;
        if (v->kind == V_LINE) { line_t *l = v->ptr; pic_set(irq, 1); l->pending = 1; l->fires++; dev_gen++; }
        else if (v->kind == V_HANDLE) { ((irq_handle_t *)v->ptr)->events++; dev_gen++; }
        else if (v->kind == V_TIMER) timer_fires++;
        pic_eoi(irq);
        return;
    }
    v->count++;
    switch (v->kind) {
    case V_HANDLE: ((irq_handle_t *)v->ptr)->events++; dev_gen++; break;
    case V_LINE: {
        line_t *l = v->ptr;
        gsi_mask((u32)l->num, 1);
        l->pending = 1; l->fires++;
        dev_gen++;
        break;
    }
    case V_TIMER: timer_fires++; break;
    default:
        if (vector == VEC_ERROR) { lapic_wr(LAPIC_ESR, 0); lapic_esr_last = lapic_rd(LAPIC_ESR); }
        break;
    }
    if (have_lapic) lapic_eoi();
}

u32 irq_generation(void) { return dev_gen; }
u64 irq_total(void) { return total_irqs; }
int irq_mode(void) { return mode; }
int irq_msi_allowed(void) { return msi_ok; }
const char *irq_mode_name(void) {
    switch (mode) {
    case IRQ_MODE_APIC: return x2apic ? "I/O APIC + x2APIC" : "I/O APIC + local APIC";
    case IRQ_MODE_PIC: return have_lapic ? "8259 PIC + local APIC" : "8259 PIC";
    }
    return "polling (interrupts off)";
}

/* ---------------- handles ---------------- */
static void handle_link(irq_handle_t *h) {
    for (irq_handle_t *p = handles; p; p = p->next_all) if (p == h) return;
    h->next_all = handles;
    handles = h;
}

static int msi_attach(irq_handle_t *h) {
    pci_dev_t *d = h->pci;
    int vec = alloc_vector(V_HANDLE, h);
    if (vec < 0) return -1;
    u64 addr = 0xFEE00000ull | ((u64)lapic_id << 12);
    u32 data = (u32)vec;                         /* fixed delivery, edge */
    int r = pci_enable_msix(d, addr, data);
    if (r == 0) { h->kind = IRQ_MSIX; h->vector = vec; return 0; }
    r = pci_enable_msi(d, addr, data);
    if (r == 0) { h->kind = IRQ_MSI; h->vector = vec; return 0; }
    vecs[vec].kind = V_FREE;
    return -1;
}

int irq_attach(irq_handle_t *h) {
    h->kind = IRQ_NONE; h->line = -1; h->vector = -1; h->broken = 0;
    if (mode == IRQ_MODE_POLL || !h->pci) return IRQ_NONE;
    handle_link(h);
    if (msi_ok && msi_attach(h) == 0) {
        kprintf("irq: %s: %s, vector %#x\n", h->name, h->kind == IRQ_MSIX ? "MSI-X" : "MSI", h->vector);
        return h->kind;
    }
    if (!h->ack) return IRQ_NONE;
    u8 pin = pci_read8(h->pci, 0x3D);
    if (pin == 0 || pin > 4) return IRQ_NONE;
    pci_write16(h->pci, 0x04, pci_read16(h->pci, 0x04) & ~(1u << 10));   /* allow INTx */
    h->kind = IRQ_INTX;
    if (mode == IRQ_MODE_PIC) {
        int irq = h->pci->irq_line;
        /* not a usable PIC line, or one taken by the PIT / PS/2 (edge) */
        int taken = irq > 0 && irq < 16 && (vecs[VEC_PIC_BASE + irq].kind == V_HANDLE ||
                                            vecs[VEC_PIC_BASE + irq].kind == V_TIMER || irq == 1 || irq == 12);
        if (irq <= 0 || irq >= 16 || irq == 2 || taken) {
            kprintf("irq: %s: no usable INTx line (Interrupt Line %d), polled\n", h->name, irq);
            h->kind = IRQ_NONE;
            return IRQ_NONE;
        }
        u64 f = irq_save();
        line_t *l = line_get(irq);
        if (l) bind(h, l);
        irq_restore(f);
        if (!l) { h->kind = IRQ_NONE; return IRQ_NONE; }
        h->ack(h->ctx);
        line_mask(l, 0);
    } else {
        learning_start();
    }
    return h->kind;
}

int irq_attach_isa(irq_handle_t *h, int irq) {
    h->kind = IRQ_NONE; h->line = irq; h->vector = -1;
    if (mode == IRQ_MODE_POLL || irq < 0 || irq >= 16) return IRQ_NONE;
    handle_link(h);
    if (mode == IRQ_MODE_PIC) {
        h->vector = VEC_PIC_BASE + irq;
        vecs[h->vector].kind = V_HANDLE; vecs[h->vector].ptr = h;
        pic_set(irq, 0);
    } else {
        int level, low;
        u32 gsi = isa_gsi(irq, &level, &low);
        h->vector = VEC_ISA_BASE + irq;
        vecs[h->vector].kind = V_HANDLE; vecs[h->vector].ptr = h;
        if (gsi_program(gsi, h->vector, level, low, 0) < 0) { vecs[h->vector].kind = V_FREE; return IRQ_NONE; }
    }
    h->kind = IRQ_ISA;
    return IRQ_ISA;
}

void irq_detach(irq_handle_t *h) {
    if (!h || h->kind == IRQ_NONE) return;
    u64 f = irq_save();
    if ((h->kind == IRQ_MSI || h->kind == IRQ_MSIX) && h->pci) {
        pci_disable_msi(h->pci);
        if (h->vector > 0) vecs[h->vector].kind = V_FREE;
    } else if (h->kind == IRQ_INTX && h->line >= 0) {
        for (int i = 0; i < nlines; i++) {
            if (lines[i].num != h->line) continue;
            irq_handle_t **pp = &lines[i].clients;
            while (*pp && *pp != h) pp = &(*pp)->next_on_line;
            if (*pp) *pp = h->next_on_line;
        }
    } else if (h->kind == IRQ_ISA && h->vector > 0) {
        if (mode == IRQ_MODE_PIC) pic_set(h->line, 1);
        else { int l, p; gsi_mask(isa_gsi(h->line, &l, &p), 1); }
        vecs[h->vector].kind = V_FREE;
    }
    h->kind = IRQ_NONE;
    irq_handle_t **pp = &handles;
    while (*pp && *pp != h) pp = &(*pp)->next_all;
    if (*pp) *pp = h->next_all;
    irq_restore(f);
}

int irq_live(const irq_handle_t *h) {
    return h && h->kind != IRQ_NONE && !h->broken && h->events > 0 && (h->kind != IRQ_INTX || h->line >= 0);
}

int irq_fired(irq_handle_t *h) {
    if (!h) return 0;
    u32 e = h->events;
    if (e == h->seen) return 0;
    h->seen = e;
    return 1;
}

void irq_note_work(irq_handle_t *h) {
    if (!irq_live(h)) return;
    u64 now = time_ms();
    if (now - h->wd_start > 2000) { h->wd_start = now; h->wd_work = 0; h->wd_events = h->events; return; }
    if (++h->wd_work >= 50 && h->events == h->wd_events) {
        h->broken = 1;
        kprintf("irq: %s: its interrupts stopped arriving, polling it instead\n", h->name);
    }
}

const char *irq_kind_name(const irq_handle_t *h) {
    if (!h) return "none";
    if (h->broken) return "polled (interrupts lost)";
    switch (h->kind) {
    case IRQ_MSI: return "MSI";
    case IRQ_MSIX: return "MSI-X";
    case IRQ_INTX: return h->line >= 0 ? "INTx" : "INTx (route not learned yet, polled)";
    case IRQ_ISA: return "ISA IRQ";
    }
    return "polled";
}

void irq_describe(const irq_handle_t *h, char *buf, int n) {
    if (!h || h->kind == IRQ_NONE) { snprintf(buf, n, "polled"); return; }
    if (h->kind == IRQ_INTX && h->line >= 0)
        snprintf(buf, n, "INTx %s %d%s", mode == IRQ_MODE_PIC ? "IRQ" : "GSI", h->line, h->broken ? ", lost: polled" : "");
    else if (h->kind == IRQ_ISA)
        snprintf(buf, n, "IRQ %d%s", h->line, h->events ? "" : " (none seen yet)");
    else
        snprintf(buf, n, "%s%s", irq_kind_name(h), h->events || h->kind == IRQ_INTX ? "" : " (none seen yet)");
}

/* ---------------- timer ---------------- */
enum { TMR_NONE = 0, TMR_LAPIC, TMR_PIT };
static int timer_kind;
static u64 lapic_ticks_per_ms;
static int idle_ok;

const char *irq_timer_name(void) {
    switch (timer_kind) {
    case TMR_LAPIC: return "local APIC timer (one-shot)";
    case TMR_PIT: return "PIT channel 0 (one-shot)";
    }
    return "none";
}

static void timer_arm_tsc(u64 deadline) {
    u64 now = rdtsc(), d = deadline > now ? deadline - now : 1;
    u64 khz = tsc_khz();
    if (d > khz * 10000) d = khz * 10000;        /* 10 s */
    if (timer_kind == TMR_LAPIC) {
        u64 t = d * lapic_ticks_per_ms / khz;
        if (t < 1) t = 1;
        if (t > 0xFFFFFFFFull) t = 0xFFFFFFFFull;
        lapic_wr(LAPIC_TIMER_INIT, (u32)t);
    } else if (timer_kind == TMR_PIT) {
        u64 c = d * 1193182 / (khz * 1000);
        if (c < 2) c = 2;
        if (c > 0xFFFF) c = 0xFFFF;              /* ~55 ms: the caller simply waits again */
        outb(0x43, 0x30);                        /* channel 0, lo/hi, mode 0 (one-shot) */
        outb(0x40, (u8)c);
        outb(0x40, (u8)(c >> 8));
    }
}
static void timer_disarm(void) {
    if (timer_kind == TMR_LAPIC) lapic_wr(LAPIC_TIMER_INIT, 0);
}

/* does an armed timer interrupt actually arrive? (interrupts are on) */
static int timer_test(void) {
    for (int round = 0; round < 3; round++) {
        u64 before = timer_fires;
        timer_arm_tsc(rdtsc() + tsc_khz() * 2);
        u64 end = rdtsc() + tsc_khz() * 100;
        while (rdtsc() < end && timer_fires == before) cpu_relax();
        if (timer_fires == before) return 0;
    }
    return 1;
}

static int lapic_timer_setup(void) {
    lapic_wr(LAPIC_TIMER_DIV, 0x3);              /* divide by 16 */
    lapic_wr(LAPIC_LVT_TIMER, LVT_MASKED | VEC_TIMER);
    lapic_wr(LAPIC_TIMER_INIT, 0xFFFFFFFF);
    u64 t0 = rdtsc();
    while (rdtsc() - t0 < tsc_khz() * 10) cpu_relax();
    u32 cur = lapic_rd(LAPIC_TIMER_CUR);
    lapic_wr(LAPIC_TIMER_INIT, 0);
    u64 ticks = 0xFFFFFFFFull - cur;
    lapic_ticks_per_ms = ticks / 10;
    if (lapic_ticks_per_ms < 100) { kprintf("irq: local APIC timer not counting\n"); return -1; }
    kprintf("irq: local APIC timer %lu.%03lu MHz (bus clock / 16)\n", lapic_ticks_per_ms / 1000, lapic_ticks_per_ms % 1000);
    vecs[VEC_TIMER].kind = V_TIMER;
    lapic_wr(LAPIC_LVT_TIMER, VEC_TIMER);        /* one-shot */
    timer_kind = TMR_LAPIC;
    return 0;
}

static int pit_timer_setup(void) {
    timer_kind = TMR_PIT;
    if (mode == IRQ_MODE_PIC) {
        vecs[VEC_PIC_BASE].kind = V_TIMER;
        pic_set(0, 0);
    } else {
        int level, low;
        u32 gsi = isa_gsi(0, &level, &low);
        vecs[VEC_ISA_BASE].kind = V_TIMER;
        if (gsi_program(gsi, VEC_ISA_BASE, level, low, 0) < 0) { timer_kind = TMR_NONE; return -1; }
    }
    return 0;
}
static void pit_timer_off(void) {
    if (mode == IRQ_MODE_PIC) pic_set(0, 1);
    else { int l, p; gsi_mask(isa_gsi(0, &l, &p), 1); }
    vecs[mode == IRQ_MODE_PIC ? VEC_PIC_BASE : VEC_ISA_BASE].kind = V_FREE;
    timer_kind = TMR_NONE;
}

/* ---------------- idle ---------------- */
static u64 idle_tsc, wakeups;
static u64 wake_deadline = ~0ull;

int idle_enabled(void) { return idle_ok; }
u64 idle_tsc_total(void) { return idle_tsc; }
u64 idle_wakeups(void) { return wakeups; }

void idle_wake_at(u64 ms) { if (ms < wake_deadline) wake_deadline = ms; }
u64 idle_take_deadline(void) { u64 d = wake_deadline; wake_deadline = ~0ull; return d; }

void idle_wait_tsc(u32 gen, u64 deadline) {
    if (!idle_ok || !irq_enabled_now()) { cpu_relax(); return; }
    irq_service();
    __asm__ volatile("cli" ::: "memory");
    if (dev_gen != gen || rdtsc() + tsc_khz() / 20 >= deadline) {   /* < 50 us left: not worth it */
        __asm__ volatile("sti" ::: "memory");
        return;
    }
    timer_arm_tsc(deadline);
    u64 t0 = rdtsc();
    __asm__ volatile("sti; hlt" ::: "memory");   /* sti takes effect after hlt: no lost wake-up */
    __asm__ volatile("cli" ::: "memory");
    idle_tsc += rdtsc() - t0;
    wakeups++;
    timer_disarm();
    __asm__ volatile("sti" ::: "memory");
}

void idle_wait(u32 gen, u64 deadline_ms) { idle_wait_tsc(gen, time_ms_to_tsc(deadline_ms)); }

void irq_wait(irq_handle_t *h, u32 gen, u64 deadline_ms) {
    if (!irq_live(h)) { cpu_relax(); return; }
    u64 cap = time_ms() + 10;                    /* bounded: a lost interrupt costs at most 10 ms */
    idle_wait(gen, deadline_ms < cap ? deadline_ms : cap);
}

static u64 load_tsc, load_idle;
static int load_pct;
int cpu_load_percent(void) {
    u64 now = rdtsc();
    if (!load_tsc) { load_tsc = now; load_idle = idle_tsc; return 100; }
    u64 dt = now - load_tsc;
    if (dt < tsc_khz() * 250) return load_pct;
    u64 di = idle_tsc - load_idle;
    load_pct = di >= dt ? 0 : (int)(100 - di * 100 / dt);
    load_tsc = now; load_idle = idle_tsc;
    return load_pct;
}

/* ---------------- diagnostics ---------------- */
int irq_list(char *buf, int n) {
    int o = 0;
#define OUT(...) do { if (o < n) o += snprintf(buf + o, n - o, __VA_ARGS__); } while (0)
    OUT("Interrupt mode: %s\n", irq_mode_name());
    OUT("Timer: %s, CPU idle (HLT) %s\n", irq_timer_name(), idle_ok ? "on" : "off");
    OUT("MSI: %s\n", msi_ok ? "allowed" : "not used");
    u64 up = time_ms();
    OUT("Interrupts: %lu total, %lu spurious, %lu idle wake-ups, idle %lu%% of uptime\n", total_irqs, spurious_irqs,
        wakeups, up ? idle_tsc / tsc_khz() * 100 / up : 0);
    OUT("Vector  Count       Source\n");
    if (vecs[VEC_TIMER].count || timer_kind == TMR_LAPIC) OUT("  %02x    %-10lu  local APIC timer\n", VEC_TIMER, vecs[VEC_TIMER].count);
    for (int v = VEC_PIC_BASE; v <= VEC_DYN_LAST; v++) {
        struct vec *e = &vecs[v];
        if (e->kind == V_FREE) continue;
        if (e->kind == V_TIMER) { OUT("  %02x    %-10lu  PIT (IRQ 0)\n", v, e->count); continue; }
        if (e->kind == V_HANDLE) {
            irq_handle_t *h = e->ptr;
            char d[48]; irq_describe(h, d, sizeof d);
            OUT("  %02x    %-10lu  %s [%s]\n", v, e->count, h->name, d);
            continue;
        }
        line_t *l = e->ptr;
        if (!l->clients && !l->foreign && !l->dead && !e->count) continue;
        OUT("  %02x    %-10lu  %s %d level:", v, e->count, mode == IRQ_MODE_PIC ? "IRQ" : "GSI", l->num);
        if (l->dead) OUT(" (disabled: stuck)");
        else if (l->foreign) OUT(" (device without a driver, masked)");
        for (irq_handle_t *h = l->clients; h; h = h->next_on_line) OUT(" %s (%u)", h->name, h->events);
        OUT("\n");
    }
    for (irq_handle_t *h = handles; h; h = h->next_all)
        if (h->kind == IRQ_INTX && h->line < 0) OUT("  --    %-10s  %s [INTx route not learned yet: polled]\n", "-", h->name);
        else if (h->broken && h->kind != IRQ_INTX) OUT("  --    %-10s  %s [interrupts lost: polled]\n", "-", h->name);
    if (lapic_esr_last) OUT("Last local APIC error: %#x\n", lapic_esr_last);
#undef OUT
    return o;
}

/* ---------------- init ---------------- */
void irq_init(void) {
    u32 bf = g_bootinfo.boot_flags;
    vecs[VEC_SPURIOUS].kind = V_SYS;
    vecs[VEC_ERROR].kind = V_SYS;
    pic_remap();                                  /* PIC vectors off the exception range, all masked */
    if (bf & BOOTF_NO_IRQ) {
        kprintf("irq: interrupts disabled by boot option: polling\n");
        return;
    }
    madt = acpi_madt();
    lapic_init(madt);
    int apic = have_lapic && madt && madt->nioapic && !(bf & BOOTF_NO_IOAPIC) && ioapic_init() == 0;
    if (have_lapic && madt && madt->nioapic && (bf & BOOTF_NO_IOAPIC)) kprintf("irq: I/O APIC disabled by boot option\n");
    mode = apic ? IRQ_MODE_APIC : IRQ_MODE_PIC;
    if (have_lapic) lapic_lints(madt, !apic);
    msi_ok = have_lapic && lapic_id < 256 && !(bf & BOOTF_NO_MSI);
    if (bf & BOOTF_NO_MSI) kprintf("irq: MSI disabled by boot option\n");
    __asm__ volatile("sti" ::: "memory");

    /* timer for idle: the local APIC timer keeps running in HLT (C1) on Intel
     * and with ARAT; on older AMD parts C1E may stop it, so use the PIT there */
    u32 a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    int intel = b == 0x756E6547;                 /* "Genu" */
    int arat = 0;
    if (a >= 6) { cpuid(6, 0, &a, &b, &c, &d); arat = (a >> 2) & 1; }
    cpuid(1, 0, &a, &b, &c, &d);
    int hv = (c >> 31) & 1;
    int ok = 0;
    if (have_lapic && (arat || intel || hv) && lapic_timer_setup() == 0) {
        ok = timer_test();
        if (!ok) { kprintf("irq: local APIC timer interrupt does not arrive\n"); lapic_wr(LAPIC_LVT_TIMER, LVT_MASKED | VEC_TIMER); timer_kind = TMR_NONE; }
    }
    if (!ok && pit_timer_setup() == 0) {
        ok = timer_test();
        if (!ok) { kprintf("irq: PIT interrupt does not arrive\n"); pit_timer_off(); }
    }
    idle_ok = ok;
    load_tsc = rdtsc(); load_idle = idle_tsc;     /* first CPU usage figure: since here */
    kprintf("irq: mode %s, timer %s%s, MSI %s\n", irq_mode_name(), irq_timer_name(),
            ok ? "" : " (no working timer interrupt: CPU idle off)", msi_ok ? "on" : "off");
}
