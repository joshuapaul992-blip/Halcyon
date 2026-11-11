/* Halcyon OS - BIOS stage2 loader (gcc -m16, unreal mode)
 *
 *  1. find the FAT32 boot partition in the MBR
 *  2. load \HALCYON\KERNEL.BIN to 1 MiB
 *  3. collect the E820 memory map
 *  4. pick and set a VESA linear-framebuffer mode
 *  5. build identity page tables for 4 GiB and jump to the 64-bit kernel
 */
#include <stdint.h>
#include "bootinfo.h"

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;

struct regs { u32 eax, ebx, ecx, edx, esi, edi; u16 ds, es; u32 eflags; } __attribute__((packed));
struct regs bregs;
extern void bios_int(u32 n);
extern void enter_long(u32 pml4, u32 entry, u32 bootinfo);

#define DISK_BUF   0x60000u   /* 64 KiB real-mode transfer buffer */
#define PT_BASE    0x70000u   /* page tables: PML4, PDPT, 4 x PD */
#define BOOTINFO   0x78000u
#define KERNEL_LOAD 0x100000u

static u32 drive;

/* gcc -m16 pitfalls: string instructions are emitted without an addr32
 * prefix and constant addresses >64K are truncated by the assembler, so all
 * bulk copies use explicit addr32 string ops and constant pointers are
 * laundered through a register. */
#define LAUNDER(p) ({ __typeof__(p) _p = (p); __asm__("" : "+r"(_p)); _p; })

static void *memset(void *d, int c, u32 n) {
    void *r = d;
    __asm__ volatile("cld; addr32 rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}
static void *memcpy(void *d, const void *s, u32 n) {
    void *r = d;
    __asm__ volatile("cld; addr32 rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
    return r;
}
static int memcmp(const void *a, const void *b, u32 n) {
    const volatile u8 *x = a, *y = b;
    for (u32 i = 0; i < n; i++) if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}

static void clear_regs(void) { memset(&bregs, 0, sizeof bregs); }

static void putc(char c) {
    clear_regs();
    bregs.eax = 0x0E00 | (u8)c;
    bregs.ebx = 0x0007;
    bios_int(0x10);
}
static void puts(const char *s) { while (*s) { if (*s == '\n') putc('\r'); putc(*s++); } }
static void puthex(u32 v) {
    puts("0x");
    for (int i = 28; i >= 0; i -= 4) putc("0123456789ABCDEF"[(v >> i) & 15]);
}

static void die(const char *msg) {
    puts("\nHalcyon boot error: ");
    puts(msg);
    puts("\nSystem halted.");
    for (;;) __asm__ volatile("cli; hlt");
}

/* ---------------- disk ---------------- */
struct dap { u8 size, zero; u16 count; u16 off, seg; u32 lba_lo, lba_hi; } __attribute__((packed));
static struct dap dap;

/* read count (<=64) sectors into the low buffer */
static int read_low(u32 lba, u32 count) {
    for (int tries = 0; tries < 4; tries++) {
        dap.size = 16; dap.zero = 0; dap.count = (u16)count;
        dap.off = 0; dap.seg = DISK_BUF >> 4;
        dap.lba_lo = lba; dap.lba_hi = 0;
        clear_regs();
        bregs.eax = 0x4200;
        bregs.edx = drive;
        bregs.esi = (u32)&dap;
        bios_int(0x13);
        if (!(bregs.eflags & 1)) return 0;
        clear_regs();
        bregs.edx = drive;
        bios_int(0x13);  /* reset */
    }
    return -1;
}

static int read_to(u32 lba, u32 count, u32 dest) {
    while (count) {
        u32 n = count > 32 ? 32 : count;
        if (read_low(lba, n)) return -1;
        memcpy((void *)dest, (void *)DISK_BUF, n * 512);
        lba += n; count -= n; dest += n * 512;
    }
    return 0;
}

/* ---------------- FAT32 ---------------- */
static u8 sector[512];
static u8 fatsec[512];
static u32 fatsec_lba = 0xFFFFFFFF;
static u32 part_lba, fat_lba, data_lba, spc, root_clus, disk_sig;

static u32 rd32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }

static u32 fat_next(u32 clus) {
    u32 lba = fat_lba + (clus * 4) / 512;
    if (lba != fatsec_lba) {
        if (read_to(lba, 1, (u32)fatsec)) die("FAT read failed");
        fatsec_lba = lba;
    }
    return rd32(fatsec + (clus * 4) % 512) & 0x0FFFFFFF;
}

static u32 clus_lba(u32 c) { return data_lba + (c - 2) * spc; }

/* find 8.3 name (11 chars) in directory cluster chain, return first cluster
 * (and size); 0 if not found */
static u32 find_entry(u32 dir_clus, const char *name83, u32 *size, int want_dir) {
    static u8 clusbuf[512];
    u32 c = dir_clus;
    while (c >= 2 && c < 0x0FFFFFF8) {
        for (u32 s = 0; s < spc; s++) {
            if (read_to(clus_lba(c) + s, 1, (u32)clusbuf)) die("directory read failed");
            for (int i = 0; i < 512; i += 32) {
                u8 *e = clusbuf + i;
                if (e[0] == 0) return 0;
                if (e[0] == 0xE5 || e[11] == 0x0F || (e[11] & 0x08)) continue;
                if (memcmp(e, name83, 11) == 0 && (!!(e[11] & 0x10)) == want_dir) {
                    if (size) *size = rd32(e + 28);
                    return ((u32)rd16(e + 20) << 16) | rd16(e + 26);
                }
            }
        }
        c = fat_next(c);
    }
    return 0;
}

static u32 load_kernel(void) {
    if (read_to(0, 1, (u32)sector)) die("cannot read MBR");
    if (sector[510] != 0x55 || sector[511] != 0xAA) die("bad MBR signature");
    disk_sig = rd32(sector + 440);
    int best = -1;
    for (int i = 0; i < 4; i++) {
        u8 *pe = sector + 446 + i * 16;
        u8 t = pe[4];
        if (t == 0x0B || t == 0x0C || t == 0xEF || t == 0x0E || t == 0x06) {
            if (best < 0 || (pe[0] & 0x80)) best = i;
        }
    }
    if (best < 0) die("no FAT partition found");
    part_lba = rd32(sector + 446 + best * 16 + 8);

    if (read_to(part_lba, 1, (u32)sector)) die("cannot read boot sector");
    if (rd16(sector + 11) != 512) die("unsupported sector size");
    spc = sector[13];
    u32 rsvd = rd16(sector + 14);
    u32 nfats = sector[16];
    u32 fatsz = rd16(sector + 22);
    if (fatsz != 0) die("boot partition is not FAT32");
    fatsz = rd32(sector + 36);
    root_clus = rd32(sector + 44);
    fat_lba = part_lba + rsvd;
    data_lba = fat_lba + nfats * fatsz;

    u32 dir = find_entry(root_clus, "HALCYON    ", 0, 1);
    if (!dir) die("\\HALCYON directory not found");
    u32 size;
    u32 c = find_entry(dir, "KERNEL  BIN", &size, 0);
    if (!c) die("\\HALCYON\\KERNEL.BIN not found");

    puts("Loading kernel");
    u32 dest = KERNEL_LOAD, remaining = (size + 511) / 512, dots = 0;
    while (c >= 2 && c < 0x0FFFFFF8 && remaining) {
        /* coalesce contiguous clusters */
        u32 first = c, run = 1;
        u32 n;
        while ((n = fat_next(c)) == c + 1 && (run + 1) * spc <= 64) { c = n; run++; }
        u32 secs = run * spc;
        if (secs > remaining) secs = remaining;
        if (read_to(clus_lba(first), secs, dest)) die("kernel read failed");
        dest += secs * 512; remaining -= secs;
        c = n;
        if (++dots % 16 == 0) putc('.');
    }
    if (remaining) die("kernel file truncated");
    puts(" ok\n");
    return size;
}

/* ---------------- memory map ---------------- */
static void get_mmap(struct bootinfo *bi) {
    static u8 ent[24];
    u32 cont = 0;
    bi->mmap_count = 0;
    do {
        memset(ent, 0, 24);
        ent[20] = 1;
        clear_regs();
        bregs.eax = 0xE820;
        bregs.edx = 0x534D4150;
        bregs.ecx = 24;
        bregs.ebx = cont;
        bregs.edi = (u32)ent;
        bios_int(0x15);
        if ((bregs.eflags & 1) || bregs.eax != 0x534D4150) break;
        cont = bregs.ebx;
        if (bregs.ecx >= 24 && !(ent[20] & 1)) continue;
        struct bootinfo_mmap *m = &bi->mmap[bi->mmap_count];
        memcpy(&m->base, ent, 8);
        memcpy(&m->length, ent + 8, 8);
        u32 t = rd32(ent + 16);
        m->type = (t >= 1 && t <= 5) ? t : MMAP_RESERVED;
        if (m->length) bi->mmap_count++;
    } while (cont && bi->mmap_count < BOOTINFO_MAX_MMAP);
    if (bi->mmap_count == 0) die("E820 memory map unavailable");
}

/* ---------------- VESA ---------------- */
struct vbe_info { char sig[4]; u16 ver; u32 oem; u32 caps; u32 modes; u16 mem; u8 rest[492]; } __attribute__((packed));
struct vbe_mode {
    u16 attr; u8 wa, wb; u16 gran, wsize, sega, segb; u32 wfunc; u16 pitch;
    u16 w, h; u8 cw, ch, planes, bpp, banks, model, banksz, pages, r1;
    u8 rmask, rpos, gmask, gpos, bmask, bpos, xmask, xpos, dcinfo;
    u32 fb; u32 off; u16 offsz; u16 lpitch; u8 rest[204];
} __attribute__((packed));
static struct vbe_info vinfo;
static struct vbe_mode vmode;

static int get_mode(u16 m) {
    memset(&vmode, 0, sizeof vmode);
    clear_regs();
    bregs.eax = 0x4F01; bregs.ecx = m; bregs.edi = (u32)&vmode;
    bios_int(0x10);
    return (bregs.eax & 0xFFFF) == 0x004F;
}

static void set_video(struct bootinfo *bi) {
    memset(&vinfo, 0, sizeof vinfo);
    memcpy(vinfo.sig, "VBE2", 4);
    clear_regs();
    bregs.eax = 0x4F00; bregs.edi = (u32)&vinfo;
    bios_int(0x10);
    if ((bregs.eax & 0xFFFF) != 0x004F || memcmp(vinfo.sig, "VESA", 4)) die("VESA BIOS not available");

    u32 lst = ((vinfo.modes >> 16) << 4) + (vinfo.modes & 0xFFFF);
    u16 modes[256]; int nm = 0;
    for (volatile u16 *p = LAUNDER((volatile u16 *)lst); *p != 0xFFFF && nm < 256; p++) modes[nm++] = *p;

    int best = -1; long best_score = -1;
    for (int i = 0; i < nm; i++) {
        if (!get_mode(modes[i])) continue;
        if ((vmode.attr & 0x99) != 0x99) continue;      /* supported, color, graphics, LFB */
        if (vmode.model != 6) continue;                 /* direct color */
        if (vmode.bpp != 32 && vmode.bpp != 24) continue;
        if (vmode.w < 800 || vmode.h < 600 || vmode.w > 1920 || vmode.h > 1200) continue;
        long score = (long)vmode.w * vmode.h / 64 + (vmode.bpp == 32 ? 1 : 0);
        if (vmode.w == 1024 && vmode.h == 768) score += 1000000;   /* classic resolution */
        if ((bi->boot_flags & BOOTF_SAFE_VIDEO) && vmode.w == 800 && vmode.h == 600) score += 4000000;
        if (score > best_score) { best_score = score; best = modes[i]; }
    }
    if (best < 0) die("no usable 24/32-bit VESA mode");
    get_mode((u16)best);
    bi->fb_addr = vmode.fb;
    bi->fb_width = vmode.w;
    bi->fb_height = vmode.h;
    bi->fb_pitch = (vinfo.ver >= 0x300 && vmode.lpitch) ? vmode.lpitch : vmode.pitch;
    bi->fb_bpp = vmode.bpp;
    bi->fb_red_pos = vmode.rpos;   bi->fb_red_size = vmode.rmask;
    bi->fb_green_pos = vmode.gpos; bi->fb_green_size = vmode.gmask;
    bi->fb_blue_pos = vmode.bpos;  bi->fb_blue_size = vmode.bmask;

    clear_regs();
    bregs.eax = 0x4F02; bregs.ebx = (u32)best | 0x4000;
    bios_int(0x10);
    if ((bregs.eax & 0xFFFF) != 0x004F) die("failed to set video mode");
}

/* ---------------- ACPI RSDP ---------------- */
static u32 find_rsdp(void) {
    u32 ebda = (u32)(*LAUNDER((volatile u16 *)0x40E)) << 4;
    for (u32 p = ebda; p && p < ebda + 1024; p += 16)
        if (memcmp(LAUNDER((void *)p), "RSD PTR ", 8) == 0) return p;
    for (u32 p = 0xE0000; p < 0x100000; p += 16)
        if (memcmp(LAUNDER((void *)p), "RSD PTR ", 8) == 0) return p;
    return 0;
}

/* ---------------- page tables ---------------- */
static void build_page_tables(void) {
    u64 *pml4 = LAUNDER((u64 *)PT_BASE);
    u64 *pdpt = LAUNDER((u64 *)(PT_BASE + 0x1000));
    memset(pml4, 0, 0x6000);
    pml4[0] = (PT_BASE + 0x1000) | 3;
    for (u32 i = 0; i < 4; i++) {
        u64 *pd = LAUNDER((u64 *)(PT_BASE + 0x2000 + i * 0x1000));
        pdpt[i] = (PT_BASE + 0x2000 + i * 0x1000) | 3;
        for (u32 j = 0; j < 512; j++)
            pd[j] = (((u64)i << 30) + ((u64)j << 21)) | 0x83;   /* P | RW | PS */
    }
}


/* ---------------- F8 Advanced Startup Options ---------------- */
static u16 ticks(void) { return *LAUNDER((volatile u16 *)0x46C); }

static int key_ready(void) {
    clear_regs();
    bregs.eax = 0x0100;
    bios_int(0x16);
    return !(bregs.eflags & 0x40);    /* ZF clear: key waiting */
}
static u16 key_read(void) {
    clear_regs();
    bios_int(0x16);                   /* AH=0: wait for key */
    return (u16)bregs.eax;            /* AH scancode, AL ascii */
}

static void vga_put(int x, int y, const char *str, u8 attr) {
    volatile u16 *vga = LAUNDER((volatile u16 *)0xB8000);
    for (; *str && x < 80; str++, x++) vga[y * 80 + x] = (u16)((attr << 8) | (u8)*str);
}
static void vga_fill(int y, u8 attr) {
    volatile u16 *vga = LAUNDER((volatile u16 *)0xB8000);
    for (int x = 0; x < 80; x++) vga[y * 80 + x] = (u16)((attr << 8) | ' ');
}

struct opt { const char *label; u32 flag; };
static const struct opt opts[] = {
    { "Start Halcyon normally", 0 },
    { "Safe Mode (no USB, SATA, NVMe or interrupts; verbose)", 0 },
    { "Start with the options below", 0 },
    { "Verbose boot: show kernel messages on screen", BOOTF_VERBOSE },
    { "Disable USB 3.x controllers (xHCI)", BOOTF_NO_XHCI },
    { "Disable USB 2.0 controllers (EHCI)", BOOTF_NO_EHCI },
    { "Disable USB 1.1 controllers (UHCI/OHCI)", BOOTF_NO_USB11 },
    { "Disable SATA controllers (AHCI)", BOOTF_NO_AHCI },
    { "Disable NVMe controllers", BOOTF_NO_NVME },
    { "Disable ACPI", BOOTF_NO_ACPI },
    { "Disable interrupts (poll all devices, no CPU idle)", BOOTF_NO_IRQ },
    { "Disable MSI (PCI devices use INTx)", BOOTF_NO_MSI },
    { "Use the legacy PIC instead of the I/O APIC", BOOTF_NO_IOAPIC },
    { "Safe video mode (800 x 600)", BOOTF_SAFE_VIDEO },
};
#define NOPTS ((int)(sizeof opts / sizeof opts[0]))

static u32 boot_menu(void) {
    u32 flags = 0;
    int sel = 0;
    clear_regs(); bregs.eax = 0x0003; bios_int(0x10);   /* 80x25 text, clears screen */
    clear_regs(); bregs.eax = 0x0100; bregs.ecx = 0x2000; bios_int(0x10);   /* hide cursor */
    for (;;) {
        for (int y = 0; y < 25; y++) vga_fill(y, 0x07);
        vga_put(18, 2, "Halcyon Advanced Startup Options", 0x0F);
        vga_put(4, 4, "Use the arrow keys to highlight an item, Enter to choose or toggle it.", 0x07);
        for (int i = 0; i < NOPTS; i++) {
            int y = 6 + i + (i >= 3 ? 1 : 0);    /* rows 6-8, 10-20 */
            u8 a = i == sel ? 0x70 : 0x07;
            char line[64];
            int n = 0;
            if (opts[i].flag) {
                line[n++] = '['; line[n++] = (flags & opts[i].flag) ? 'X' : ' '; line[n++] = ']'; line[n++] = ' ';
            }
            for (const char *c = opts[i].label; *c && n < 62; c++) line[n++] = *c;
            line[n] = 0;
            vga_fill(y, 0x07);
            for (int x = 6; x < 74; x++) LAUNDER((volatile u16 *)0xB8000)[y * 80 + x] = (u16)((a << 8) | ' ');
            vga_put(8, y, line, a);
        }
        vga_put(4, 22, "The kernel log is saved to \\HALCYON\\BOOT.LOG on the boot drive", 0x08);
        vga_put(4, 23, "once Halcyon reaches the desktop.", 0x08);
        u16 k = key_read();
        u8 sc = k >> 8, ch = (u8)k;
        if (sc == 0x48) sel = (sel + NOPTS - 1) % NOPTS;
        else if (sc == 0x50) sel = (sel + 1) % NOPTS;
        else if (ch == 13 || ch == ' ') {
            if (sel == 0) return 0;
            if (sel == 1) return BOOTF_SAFE_MODE;
            if (sel == 2) return flags;
            flags ^= opts[sel].flag;
        } else if (ch == 27) return 0;
    }
}

static u32 check_f8(void) {
    puts("Press F8 for Advanced Startup Options");
    u16 start = ticks();
    u32 flags = 0;
    int dots = 0;
    while ((u16)(ticks() - start) < 18) {          /* ~1 second */
        if (key_ready()) {
            u16 k = key_read();
            if ((k >> 8) == 0x42) { flags = boot_menu() | 0x80000000u; break; }
        }
        if ((u16)(ticks() - start) / 4 > dots) { putc('.'); dots++; }
    }
    puts("\n");
    return flags;
}

void stage2_main(u32 boot_drive) {
    drive = boot_drive;
    puts("\r\nHalcyon loader\n");

    u32 ksize = load_kernel();
    struct kernel_header *kh = LAUNDER((struct kernel_header *)KERNEL_LOAD);
    if (kh->magic != KERNEL_MAGIC) die("KERNEL.BIN has a bad header");
    if (kh->bss_end > ksize + KERNEL_LOAD)
        memset((void *)(KERNEL_LOAD + ksize), 0, (u32)kh->bss_end - (KERNEL_LOAD + ksize));

    struct bootinfo *bi = LAUNDER((struct bootinfo *)BOOTINFO);
    memset(bi, 0, sizeof *bi);
    bi->magic = BOOTINFO_MAGIC;
    bi->boot_type = BOOT_TYPE_BIOS;
    bi->bios_drive = drive;
    bi->boot_disk_sig = disk_sig;
    bi->boot_part_lba = part_lba;
    bi->rsdp = find_rsdp();
    u32 bf = check_f8();
    bi->boot_flags = bf & ~0x80000000u;
    get_mmap(bi);
    build_page_tables();
    (void)puthex;
    set_video(bi);
    enter_long(PT_BASE, (u32)kh->entry, BOOTINFO);
}
