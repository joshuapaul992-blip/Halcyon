/* Halcyon OS - GDT, IDT, exceptions, timing, RTC, power */
#include "kernel.h"
#include "lib.h"
#include "io.h"
#include "proc.h"

struct bootinfo g_bootinfo;
void (*g_idle_hook)(void);

/* ---------------- GDT ---------------- */
/* The order is what SYSCALL/SYSRET expect: kernel code, kernel data, then
 * user data and user code (STAR[63:48] = 0x10: SS = 0x18, CS = 0x20). */
static u64 gdt[7] ALIGNED(16) = {
    0,
    0x00AF9A000000FFFFull,   /* 0x08 kernel code 64 */
    0x00CF92000000FFFFull,   /* 0x10 kernel data */
    0x00CFF2000000FFFFull,   /* 0x18 user data (DPL 3) */
    0x00AFFA000000FFFFull,   /* 0x20 user code 64 (DPL 3) */
    0, 0,                    /* 0x28 TSS (16 bytes) */
};
struct PACKED dtr { u16 limit; u64 base; };

/* 64-bit TSS: RSP0 = kernel stack for interrupts from user mode, IST1 for
 * double faults, IST2 for NMI / machine check */
static struct PACKED tss {
    u32 rsvd0;
    u64 rsp[3];
    u64 rsvd1;
    u64 ist[7];
    u64 rsvd2;
    u16 rsvd3, iomap;
} tss ALIGNED(16);
static u8 ist_df[8192] ALIGNED(16), ist_nmi[8192] ALIGNED(16);
u64 g_kstack_top;           /* kernel stack of the current thread (syscall entry) */

void tss_set_rsp0(u64 rsp0) {
    tss.rsp[0] = rsp0;
    g_kstack_top = rsp0;
}

static void tss_init(void) {
    memset(&tss, 0, sizeof tss);
    tss.ist[0] = (u64)(uptr)(ist_df + sizeof ist_df);
    tss.ist[1] = (u64)(uptr)(ist_nmi + sizeof ist_nmi);
    tss.iomap = sizeof tss;           /* no I/O permission bitmap */
    u64 base = (u64)(uptr)&tss, lim = sizeof tss - 1;
    gdt[5] = (lim & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ull << 40) | (((lim >> 16) & 0xF) << 48) |
             (((base >> 24) & 0xFF) << 56);
    gdt[6] = base >> 32;
    __asm__ volatile("ltr %w0" :: "r"(0x28));
}

/* SYSCALL/SYSRET: kernel CS 0x08 / SS 0x10, user base 0x10 (SS 0x1B, CS 0x23) */
extern void syscall_entry(void);   /* entry.asm */
static void syscall_init(void) {
    wrmsr(0xC0000080, rdmsr(0xC0000080) | 1);                 /* EFER.SCE */
    wrmsr(0xC0000081, (0x10ull << 48) | (0x08ull << 32));      /* STAR */
    wrmsr(0xC0000082, (u64)(uptr)syscall_entry);               /* LSTAR */
    wrmsr(0xC0000084, 0x200 | 0x400 | 0x100 | 0x40000 | 0x4000);   /* FMASK: IF, DF, TF, AC, NT */
}

static void gdt_load(void) {
    struct dtr d = { sizeof(gdt) - 1, (u64)(uptr)gdt };
    __asm__ volatile(
        "lgdt %0\n"
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n movw %%ax, %%es\n movw %%ax, %%ss\n"
        "movw %%ax, %%fs\n movw %%ax, %%gs\n"
        :: "m"(d) : "rax", "memory");
}

/* ---------------- IDT ---------------- */
struct PACKED idt_entry { u16 off_lo, sel; u8 ist, type; u16 off_mid; u32 off_hi, zero; };
static struct idt_entry idt[256] ALIGNED(16);
extern u64 isr_table[256];


static void idt_set(int n, u64 handler) {
    idt[n].off_lo = handler & 0xFFFF;
    idt[n].sel = 0x08;
    idt[n].ist = 0;
    idt[n].type = 0x8E;
    idt[n].off_mid = (handler >> 16) & 0xFFFF;
    idt[n].off_hi = handler >> 32;
    idt[n].zero = 0;
}

static const char *exc_names[] = {
    "Divide error", "Debug", "NMI", "Breakpoint", "Overflow", "Bound range", "Invalid opcode",
    "Device not available", "Double fault", "Coprocessor overrun", "Invalid TSS", "Segment not present",
    "Stack fault", "General protection fault", "Page fault", "Reserved", "x87 FP error", "Alignment check",
    "Machine check", "SIMD FP exception", "Virtualization", "Control protection",
};

void panic_screen(const char *title, const char *detail);   /* gui/panic.c */

void irq_dispatch(u64 vector);   /* core/irq.c */
extern int g_panicking;          /* core/thread.c: locks and scheduling off */

void isr_dispatch(struct regs *r) {
    if (r->vector >= 32) {
        irq_dispatch(r->vector);
        if (r->cs & 3) proc_user_return(r);     /* preemption, kill requests */
        return;
    }
    /* an NMI is not the program's doing */
    if ((r->cs & 3) && r->vector == 2) { kprintf("cpu: NMI while a program was running (ignored)\n"); proc_user_return(r); return; }
    /* an exception in a user program ends that program, not the system */
    if ((r->cs & 3) && r->vector != 8 && r->vector != 18) { proc_user_fault(r); return; }
    __asm__ volatile("cli");
    g_panicking = 1;
    u64 cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    char buf[640];
    snprintf(buf, sizeof buf,
             "%s (vector %lu, error %#lx)\n"
             "RIP=%016lx  RSP=%016lx  RFLAGS=%016lx\n"
             "RAX=%016lx  RBX=%016lx  RCX=%016lx\n"
             "RDX=%016lx  RSI=%016lx  RDI=%016lx\n"
             "RBP=%016lx  R8 =%016lx  R9 =%016lx\n"
             "CR2=%016lx",
             r->vector < ARRAY_SIZE(exc_names) ? exc_names[r->vector] : "Exception",
             r->vector, r->error, r->rip, r->rsp, r->rflags, r->rax, r->rbx, r->rcx,
             r->rdx, r->rsi, r->rdi, r->rbp, r->r8, r->r9, cr2);
    kprintf("\n*** CPU EXCEPTION ***\n%s\n", buf);
    /* stack trace via frame pointers */
    u64 *fp = (u64 *)r->rbp;
    kprintf("Backtrace: %lx", r->rip);
    for (int i = 0; i < 12 && fp && (u64)fp > 0x1000 && (u64)fp < 0x100000000ull; i++) {
        kprintf(" %lx", fp[1]);
        fp = (u64 *)fp[0];
    }
    kprintf("\n");
    panic_screen("A fatal CPU exception occurred", buf);
    for (;;) __asm__ volatile("cli; hlt");
}


void panic(const char *fmt, ...) {
    __asm__ volatile("cli");
    g_panicking = 1;
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    kprintf("\n*** KERNEL PANIC: %s\n", buf);
    panic_screen("Kernel panic", buf);
    for (;;) __asm__ volatile("cli; hlt");
}

static char brand[49], vendor[13];
const char *cpu_brand(void) { return brand; }
const char *cpu_vendor(void) { return vendor; }

void cpu_init(void) {
    gdt_load();
    for (int i = 0; i < 256; i++) idt_set(i, isr_table[i]);
    idt[8].ist = 1;                    /* double fault: known-good stack */
    idt[2].ist = 2;                    /* NMI */
    idt[18].ist = 2;                   /* machine check */
    tss_init();
    syscall_init();
    /* protection the kernel can use: CR0.WP; SMEP (no running user pages in
     * ring 0), SMAP (no touching them: the kernel reaches user memory only
     * through the identity map), UMIP (no SGDT/SIDT/SLDT/SMSW/STR in user mode) */
    u64 cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0 | (1ull << 16)));
    u32 ma, mb, mc, md;
    cpuid(0, 0, &ma, &mb, &mc, &md);
    if (ma >= 7) {
        cpuid(7, 0, &ma, &mb, &mc, &md);
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        if (mb & (1u << 7)) cr4 |= 1ull << 20;
        if (mb & (1u << 20)) cr4 |= 1ull << 21;
        if (mc & (1u << 2)) cr4 |= 1ull << 11;
        __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));
        kprintf("cpu: SMEP %s, SMAP %s, UMIP %s\n", (mb & (1u << 7)) ? "on" : "n/a", (mb & (1u << 20)) ? "on" : "n/a",
                (mc & (1u << 2)) ? "on" : "n/a");
    }
    struct dtr d = { sizeof(idt) - 1, (u64)(uptr)idt };
    __asm__ volatile("lidt %0" :: "m"(d));

    /* mask the legacy PICs until irq_init() sets up interrupts */
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    u32 a, b, c, dd;
    cpuid(0, 0, &a, &b, &c, &dd);
    memcpy(vendor, &b, 4); memcpy(vendor + 4, &dd, 4); memcpy(vendor + 8, &c, 4);
    cpuid(0x80000000, 0, &a, &b, &c, &dd);
    if (a >= 0x80000004) {
        u32 *p = (u32 *)brand;
        for (u32 i = 0; i < 3; i++) cpuid(0x80000002 + i, 0, &p[i * 4], &p[i * 4 + 1], &p[i * 4 + 2], &p[i * 4 + 3]);
        char *s = brand; while (*s == ' ') s++;
        memmove(brand, s, strlen(s) + 1);
    } else {
        strlcpy(brand, vendor, sizeof brand);
    }
}

/* ---------------- timing ---------------- */
static u64 tsc_per_ms = 2000000, tsc_base;
u64 tsc_khz(void) { return tsc_per_ms; }

int acpi_pm_timer(u16 *port, int *bits32) __attribute__((weak));

/* TSC ticks per ms measured against PIT channel 2 (0 on failure) */
static u64 cal_pit(void) {
    const u32 ms = 50;
    u32 count = 1193182 * ms / 1000;
    u8 p61 = inb(0x61);
    outb(0x61, (p61 & ~0x02) | 0x01);
    outb(0x43, 0xB0);
    outb(0x42, count & 0xFF);
    outb(0x42, count >> 8);
    u64 t0 = rdtsc(), t1;
    int ok = 0;
    for (;;) {
        t1 = rdtsc();
        if (inb(0x61) & 0x20) { ok = 1; break; }
        if (t1 - t0 > 20000000000ull) break;     /* ~5+ s at any sane clock: PIT gated off */
    }
    outb(0x61, p61);
    return ok && t1 > t0 ? (t1 - t0) / ms : 0;
}

/* TSC ticks per ms measured against the ACPI PM timer (0 if unavailable) */
static u64 cal_pmtimer(void) {
    u16 port; int b32;
    if (!acpi_pm_timer || !acpi_pm_timer(&port, &b32)) return 0;
    u32 mask = b32 ? 0xFFFFFFFFu : 0x00FFFFFFu;
    const u32 want = 3579545 / 20;              /* 50 ms */
    u32 start = inl(port) & mask, last = start;
    u64 t0 = rdtsc(), elapsed = 0;
    for (u64 spins = 0; elapsed < want; spins++) {
        u32 now = inl(port) & mask;
        elapsed += (now - last) & mask;
        last = now;
        if (spins > 50000000) return 0;         /* timer not counting */
    }
    u64 t1 = rdtsc();
    return (t1 - t0) * 3579545 / (elapsed * 1000);
}

/* TSC frequency reported by CPUID (Intel leaf 0x15, else 0x16 base MHz) */
static u64 cal_cpuid(void) {
    u32 a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    u32 max = a;
    if (max >= 0x15) {
        cpuid(0x15, 0, &a, &b, &c, &d);
        if (a && b && c) return (u64)c * b / a / 1000;
    }
    if (max >= 0x16) {
        cpuid(0x16, 0, &a, &b, &c, &d);
        if (a & 0xFFFF) return (u64)(a & 0xFFFF) * 1000;
    }
    return 0;
}

static int plausible(u64 khz) { return khz >= 100000 && khz <= 10000000; }

void time_init(void) {
    u64 pit = cal_pit(), pm = cal_pmtimer(), cid = cal_cpuid();
    const char *src;
    if (plausible(pit)) {
        tsc_per_ms = pit; src = "PIT";
        /* a PIT that is emulated or clock-gated can be off: trust the PM timer if they disagree by >2% */
        if (plausible(pm) && (pit > pm ? pit - pm : pm - pit) * 50 > pm) { tsc_per_ms = pm; src = "ACPI PM timer (PIT disagreed)"; }
    } else if (plausible(pm)) { tsc_per_ms = pm; src = "ACPI PM timer"; }
    else if (plausible(cid)) { tsc_per_ms = cid; src = "CPUID"; }
    else { tsc_per_ms = 2000000; src = "guess"; }
    tsc_base = rdtsc();
    kprintf("time: TSC %lu.%03lu MHz from %s (PIT %lu, PM %lu, CPUID %lu kHz)\n", tsc_per_ms / 1000, tsc_per_ms % 1000,
            src, pit, pm, cid);
}

u64 time_ms_to_tsc(u64 ms) { return tsc_base + ms * tsc_per_ms; }
u64 time_ms(void) { return (rdtsc() - tsc_base) / tsc_per_ms; }
u64 time_us(void) { return (rdtsc() - tsc_base) * 1000 / tsc_per_ms; }
void udelay(u64 us) {
    u64 end = rdtsc() + us * tsc_per_ms / 1000;
    while (rdtsc() < end) cpu_relax();
}
void idle_wait_tsc(u32 gen, u64 deadline_tsc);   /* core/irq.c */
u32  irq_generation(void);
/* longer delays halt the CPU (when the idle timer works) instead of spinning */
void mdelay(u64 ms) {
    u64 end = rdtsc() + ms * tsc_per_ms;
    while (rdtsc() < end) {
        if (ms >= 2) idle_wait_tsc(irq_generation(), end);
        else cpu_relax();
    }
}

/* ---------------- QEMU test hook ---------------- */
/* Under a hypervisor, QEMU's fw_cfg file "opt/halcyon/bootflags" (a number)
 * adds BOOTF_* flags, so automated tests can reach the fallback paths that
 * otherwise need the F8 menu. Never probed on real hardware. */
static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
/* contents of a QEMU fw_cfg file as a string; 0 if there is none */
int qemu_fw_file(const char *name, char *buf, int bufsz) {
    u32 a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d);
    if (!(c & (1u << 31)) || bufsz < 1) return 0;
    u8 sig[4];
    outw(0x510, 0x0000);
    for (int i = 0; i < 4; i++) sig[i] = inb(0x511);
    if (memcmp(sig, "QEMU", 4)) return 0;
    u8 hdr[64];
    outw(0x510, 0x0019);                          /* file directory */
    for (int i = 0; i < 4; i++) hdr[i] = inb(0x511);
    u32 n = be32(hdr);
    for (u32 f = 0; f < n && f < 256; f++) {
        for (int i = 0; i < 64; i++) hdr[i] = inb(0x511);
        if (strcmp((char *)hdr + 8, name)) continue;
        u32 size = be32(hdr);
        memset(buf, 0, bufsz);
        outw(0x510, (u16)(hdr[4] << 8 | hdr[5]));
        for (u32 i = 0; i < size && i < (u32)bufsz - 1; i++) buf[i] = (char)inb(0x511);
        return 1;
    }
    return 0;
}

u32 qemu_boot_flags(void) {
    char buf[24];
    return qemu_fw_file("opt/halcyon/bootflags", buf, sizeof buf) ? (u32)strtol(buf, NULL, 0) : 0;
}

/* ---------------- RTC ---------------- */
static u8 cmos(u8 r) { outb(0x70, r); return inb(0x71); }
static int bcd(int v) { return (v & 15) + (v >> 4) * 10; }

void rtc_read(struct rtc_time *t) {
    for (int i = 0; i < 1000 && (cmos(0x0A) & 0x80); i++) ;
    int s = cmos(0), m = cmos(2), h = cmos(4), d = cmos(7), mo = cmos(8), y = cmos(9), wd = cmos(6);
    int cent = cmos(0x32);
    u8 b = cmos(0x0B);
    if (!(b & 0x04)) {
        s = bcd(s); m = bcd(m); d = bcd(d); mo = bcd(mo); y = bcd(y); wd = bcd(wd);
        h = bcd(h & 0x7F) | (h & 0x80);
        cent = bcd(cent);
    }
    if (!(b & 0x02) && (h & 0x80)) h = ((h & 0x7F) + 12) % 24;
    h &= 0x7F;
    if (cent < 19 || cent > 30) cent = 20;
    t->year = cent * 100 + y; t->month = mo; t->day = d;
    t->hour = h; t->minute = m; t->second = s; t->weekday = wd;
}

/* ---------------- power ---------------- */
/* disk drivers that want to hear about the power going away (weak: optional) */
void nvme_shutdown(void) __attribute__((weak));

void reboot(void) {
    if (nvme_shutdown) nvme_shutdown();
    kprintf("Rebooting...\n");
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) ;
    outb(0x64, 0xFE);                 /* 8042 pulse reset */
    mdelay(50);
    outb(0xCF9, 0x02); outb(0xCF9, 0x06);   /* PCI reset */
    mdelay(50);
    struct dtr d = { 0, 0 };          /* triple fault */
    __asm__ volatile("lidt %0; int3" :: "m"(d));
    for (;;) __asm__ volatile("hlt");
}

int acpi_poweroff(void) __attribute__((weak));
void shutdown(void) {
    if (nvme_shutdown) nvme_shutdown();
    kprintf("Shutting down...\n");
    if (acpi_poweroff) acpi_poweroff();
    outw(0x604, 0x2000);    /* QEMU q35/piix (newer) */
    outw(0xB004, 0x2000);   /* Bochs / old QEMU */
    outw(0x4004, 0x3400);   /* VirtualBox */
    mdelay(100);
}
