/* Halcyon OS - GDT, IDT, exceptions, timing, RTC, power */
#include "kernel.h"
#include "lib.h"
#include "io.h"

struct bootinfo g_bootinfo;
void (*g_idle_hook)(void);

/* ---------------- GDT ---------------- */
static u64 gdt[5] ALIGNED(16) = {
    0,
    0x00AF9A000000FFFFull,   /* 0x08 kernel code 64 */
    0x00CF92000000FFFFull,   /* 0x10 kernel data */
    0x00CFFA000000FFFFull,   /* 0x18 unused */
    0x00CFF2000000FFFFull,   /* 0x20 unused */
};
struct PACKED dtr { u16 limit; u64 base; };

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
extern u64 isr_table[33];

struct regs {
    u64 r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vector, error, rip, cs, rflags, rsp, ss;
};

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

void isr_dispatch(struct regs *r) {
    if (r->vector >= 32) return;    /* spurious */
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
    for (int i = 0; i < 256; i++) idt_set(i, isr_table[i < 32 ? i : 32]);
    struct dtr d = { sizeof(idt) - 1, (u64)(uptr)idt };
    __asm__ volatile("lidt %0" :: "m"(d));

    /* mask the legacy PICs: Halcyon is fully polled */
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

void time_init(void) {
    /* PIT channel 2, mode 0, ~50 ms */
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
        if (t1 - t0 > 20000000000ull) break;
    }
    outb(0x61, p61);
    if (ok && t1 > t0) tsc_per_ms = (t1 - t0) / ms;
    if (tsc_per_ms < 100000) tsc_per_ms = 2000000;
    tsc_base = rdtsc();
    kprintf("time: TSC %lu.%03lu MHz%s\n", tsc_per_ms / 1000, tsc_per_ms % 1000, ok ? "" : " (PIT calibration failed, guessing)");
}

u64 time_ms(void) { return (rdtsc() - tsc_base) / tsc_per_ms; }
u64 time_us(void) { return (rdtsc() - tsc_base) * 1000 / tsc_per_ms; }
void udelay(u64 us) {
    u64 end = rdtsc() + us * tsc_per_ms / 1000;
    while (rdtsc() < end) cpu_relax();
}
void mdelay(u64 ms) {
    u64 end = rdtsc() + ms * tsc_per_ms;
    while (rdtsc() < end) cpu_relax();
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
void reboot(void) {
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
    kprintf("Shutting down...\n");
    if (acpi_poweroff) acpi_poweroff();
    outw(0x604, 0x2000);    /* QEMU q35/piix (newer) */
    outw(0xB004, 0x2000);   /* Bochs / old QEMU */
    outw(0x4004, 0x3400);   /* VirtualBox */
    mdelay(100);
}
