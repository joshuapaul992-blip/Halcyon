/* Halcyon OS - UEFI boot loader (\EFI\BOOT\BOOTX64.EFI)
 *
 *  1. load \HALCYON\KERNEL.BIN from the partition this loader came from
 *  2. pick a GOP framebuffer mode, find the ACPI RSDP and the boot partition
 *  3. build identity page tables, ExitBootServices, convert the memory map
 *  4. jump to the kernel through a small position-independent trampoline
 *     that lives in its own page (it copies the kernel into place if
 *     [load_addr, bss_end) was still owned by the firmware before EBS)
 *
 * Fills struct bootinfo exactly like boot/stage2.c does (see bootinfo.h).
 */
#include "efi.h"
#include "bootinfo.h"

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;

#define PAGE_SIZE   4096ull
#define PAGES(n)    (((u64)(n) + PAGE_SIZE - 1) / PAGE_SIZE)
#define FOUR_GIB    0x100000000ull
#define TWO_MIB     0x200000ull
#define PT_POOL_PAGES 32

static EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES *BS;
static EFI_HANDLE image_handle;
static int ebs_called;                 /* no console output after the first EBS call */

/* kernel destination [kdest, kdest_end) - nothing of ours may live there */
static u64 kdest, kdest_end;

/* ---------------- freestanding helpers ---------------- */
/* clang may emit calls to these for struct copies / zeroing */
void *memset(void *d, int c, size_t n) {
    void *r = d;
    __asm__ volatile("cld; rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}
void *memcpy(void *d, const void *s, size_t n) {
    void *r = d;
    __asm__ volatile("cld; rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
    return r;
}
static int memeq(const void *a, const void *b, size_t n) {
    const u8 *x = a, *y = b;
    for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return 0;
    return 1;
}
static u64 rd_u64(const u8 *p) { u64 v; memcpy(&v, p, 8); return v; }
static u32 rd_u32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }

/* ---------------- console ---------------- */
static void out16(CHAR16 *s) {
    if (!ebs_called && ST->ConOut) ST->ConOut->OutputString(ST->ConOut, s);
}
static void con_puts(const char *s) {
    CHAR16 buf[130];
    int n = 0;
    while (*s) {
        if (*s == '\n') buf[n++] = '\r';
        buf[n++] = (CHAR16)(u8)*s++;
        if (n >= 126) { buf[n] = 0; out16(buf); n = 0; }
    }
    buf[n] = 0;
    out16(buf);
}
static void con_hex(u64 v) {
    char b[19];
    int n = 0;
    b[n++] = '0'; b[n++] = 'x';
    int started = 0;
    for (int i = 60; i >= 0; i -= 4) {
        int d = (v >> i) & 15;
        if (d || started || i == 0) { b[n++] = "0123456789ABCDEF"[d]; started = 1; }
    }
    b[n] = 0;
    con_puts(b);
}
static void con_dec(u64 v) {
    char b[21];
    int n = 20;
    b[n] = 0;
    do { b[--n] = (char)('0' + v % 10); v /= 10; } while (v);
    con_puts(b + n);
}

static __attribute__((noreturn)) void fail(const char *msg, EFI_STATUS st) {
    if (!ebs_called) {
        con_puts("\nHalcyon boot error: ");
        con_puts(msg);
        if (st) { con_puts(" (status "); con_hex(st); con_puts(")"); }
        con_puts("\nSystem halted.\n");
        for (;;) BS->Stall(1000000);
    }
    for (;;) __asm__ volatile("cli; hlt");
}

/* ---------------- memory ---------------- */
static int overlaps_kernel(u64 base, u64 len) {
    return base < kdest_end && base + len > kdest;
}

/* pages below 4 GiB that never overlap the kernel destination */
static void *alloc_low(u64 pages, EFI_MEMORY_TYPE type, const char *what) {
    for (int tries = 0; tries < 64; tries++) {
        EFI_PHYSICAL_ADDRESS a = FOUR_GIB - 1;
        EFI_STATUS st = BS->AllocatePages(AllocateMaxAddress, type, pages, &a);
        if (EFI_ERROR(st)) {
            con_puts("cannot allocate memory for ");
            fail(what, st);
        }
        if (!overlaps_kernel(a, pages * PAGE_SIZE)) {
            memset((void *)(uintptr_t)a, 0, pages * PAGE_SIZE);
            return (void *)(uintptr_t)a;
        }
        /* inside the kernel destination: keep it allocated so the firmware
         * does not hand it out again, and try once more */
    }
    con_puts("cannot allocate memory outside the kernel area for ");
    fail(what, 0);
}

/* ---------------- boot partition (device path) ---------------- */
static void find_boot_partition(EFI_LOADED_IMAGE_PROTOCOL *li, struct bootinfo *bi) {
    EFI_GUID dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_DEVICE_PATH_PROTOCOL *dp = 0;
    if (EFI_ERROR(BS->HandleProtocol(li->DeviceHandle, &dp_guid, (void **)&dp)) || !dp) return;
    for (int i = 0; i < 128; i++) {
        u32 len = dp->Length[0] | ((u32)dp->Length[1] << 8);
        if (dp->Type == DEVICE_PATH_END && dp->SubType == DEVICE_PATH_END_ENTIRE) break;
        if (len < 4) break;
        if (dp->Type == DEVICE_PATH_MEDIA && dp->SubType == MEDIA_HARDDRIVE_DP && len >= 42) {
            const u8 *n = (const u8 *)dp;
            u64 start = rd_u64(n + HD_NODE_PART_START);
            if (start <= 0xFFFFFFFFull) bi->boot_part_lba = (u32)start;
            if (n[HD_NODE_SIG_TYPE] == HD_SIG_TYPE_MBR) bi->boot_disk_sig = rd_u32(n + HD_NODE_SIGNATURE);
            return;
        }
        dp = (EFI_DEVICE_PATH_PROTOCOL *)((u8 *)dp + len);
    }
}

/* ---------------- kernel ---------------- */
struct kernel_load {
    struct kernel_header hdr;
    u64 size;          /* bytes read from the file */
    u64 tmp;           /* temporary copy (0 = loaded in place) */
};

static void read_all(EFI_FILE_PROTOCOL *f, void *dst, u64 size) {
    u8 *p = dst;
    while (size) {
        UINTN n = size > 0x100000 ? 0x100000 : size;
        EFI_STATUS st = f->Read(f, &n, p);
        if (EFI_ERROR(st)) fail("error reading \\HALCYON\\KERNEL.BIN", st);
        if (n == 0) fail("\\HALCYON\\KERNEL.BIN is truncated", 0);
        p += n; size -= n;
    }
}

static void load_kernel(EFI_LOADED_IMAGE_PROTOCOL *li, struct kernel_load *kl) {
    static CHAR16 kpath[] = u"\\HALCYON\\KERNEL.BIN";
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
    EFI_FILE_PROTOCOL *root = 0, *f = 0;
    EFI_STATUS st;

    st = BS->HandleProtocol(li->DeviceHandle, &fs_guid, (void **)&fs);
    if (EFI_ERROR(st) || !fs) fail("boot device has no file system", st);
    st = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(st)) fail("cannot open the boot volume", st);
    st = root->Open(root, &f, kpath, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st)) fail("\\HALCYON\\KERNEL.BIN not found", st);

    /* file size: seek to the end */
    u64 fsize = 0;
    if (EFI_ERROR(f->SetPosition(f, ~0ull)) || EFI_ERROR(f->GetPosition(f, &fsize))) fsize = 0;
    if (EFI_ERROR(f->SetPosition(f, 0))) fail("cannot seek in \\HALCYON\\KERNEL.BIN", 0);

    read_all(f, &kl->hdr, sizeof kl->hdr);
    struct kernel_header *h = &kl->hdr;
    if (h->magic != KERNEL_MAGIC) fail("KERNEL.BIN has a bad header (magic)", 0);
    if (!fsize) fsize = h->file_size;
    if (fsize < h->file_size) fail("KERNEL.BIN is shorter than its header says", 0);
    if ((h->load_addr & (PAGE_SIZE - 1)) || h->bss_end <= h->load_addr || h->bss_end > FOUR_GIB ||
        h->bss_end - h->load_addr > 0x40000000ull || fsize > h->bss_end - h->load_addr ||
        h->entry < h->load_addr || h->entry >= h->load_addr + fsize)
        fail("KERNEL.BIN header has invalid addresses", 0);
    kl->size = fsize;
    kdest = h->load_addr;
    kdest_end = (h->bss_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    u64 pages = (kdest_end - kdest) / PAGE_SIZE;
    EFI_PHYSICAL_ADDRESS a = kdest;
    st = BS->AllocatePages(AllocateAddress, EfiLoaderCode, pages, &a);
    void *dst;
    if (!EFI_ERROR(st) && a == kdest) {
        kl->tmp = 0;
        dst = (void *)(uintptr_t)kdest;
    } else {
        /* still owned by the firmware: stage it elsewhere, copy after EBS */
        kl->tmp = (u64)(uintptr_t)alloc_low(PAGES(fsize), EfiLoaderData, "the kernel image");
        dst = (void *)(uintptr_t)kl->tmp;
    }
    if (EFI_ERROR(f->SetPosition(f, 0))) fail("cannot seek in \\HALCYON\\KERNEL.BIN", 0);
    read_all(f, dst, fsize);
    f->Close(f);
    root->Close(root);

    con_puts("kernel: "); con_dec(fsize); con_puts(" bytes, ");
    con_hex(kdest); con_puts("-"); con_hex(h->bss_end);
    con_puts(kl->tmp ? " (staged, moved after ExitBootServices)\n" : "\n");
}

/* ---------------- graphics ---------------- */
static int top_bit(u32 m) { return m ? 31 - __builtin_clz(m) : -1; }

static int fmt_usable(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *i) {
    if (i->PixelFormat == PixelRedGreenBlueReserved8BitPerColor ||
        i->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) return 1;
    if (i->PixelFormat == PixelBitMask) {
        const EFI_PIXEL_BITMASK *m = &i->PixelInformation;
        if (!m->RedMask || !m->GreenMask || !m->BlueMask) return 0;
        int hb = top_bit(m->RedMask | m->GreenMask | m->BlueMask | m->ReservedMask);
        return (hb + 8) / 8 * 8 == 32;      /* pixel size = highest mask bit, in bytes */
    }
    return 0;
}

static void mask_field(u32 m, u8 *pos, u8 *size) {
    *pos = (u8)__builtin_ctz(m);
    *size = (u8)__builtin_popcount(m);
}

static void setup_gop(struct bootinfo *bi) {
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
    UINTN isz;
    EFI_STATUS st;

    st = BS->HandleProtocol(ST->ConsoleOutHandle, &gop_guid, (void **)&gop);
    if (EFI_ERROR(st) || !gop) st = BS->LocateProtocol(&gop_guid, 0, (void **)&gop);
    if (EFI_ERROR(st) || !gop) fail("no graphics output (GOP) available", st);

    /* some firmware reports NOT_STARTED until a mode has been set */
    st = gop->QueryMode(gop, gop->Mode ? gop->Mode->Mode : 0, &isz, &info);
    if (!EFI_ERROR(st)) BS->FreePool(info);
    if (st == EFI_NOT_STARTED || !gop->Mode) gop->SetMode(gop, 0);
    if (!gop->Mode) fail("GOP has no mode information", 0);

    u32 cur = gop->Mode->Mode, max = gop->Mode->MaxMode;
    int m1024 = -1, m800 = -1, mbig = -1, cur_ok = 0, blt_only = 0;
    u64 big_area = 0;
    for (u32 m = 0; m < max; m++) {
        if (EFI_ERROR(gop->QueryMode(gop, m, &isz, &info))) continue;
        u32 w = info->HorizontalResolution, h = info->VerticalResolution;
        int ok = fmt_usable(info);
        if (info->PixelFormat == PixelBltOnly) blt_only = 1;
        if (ok && w == 1024 && h == 768 && m1024 < 0) m1024 = (int)m;
        if (ok && w == 800 && h == 600 && m800 < 0) m800 = (int)m;
        if (ok && w <= 1920 && h <= 1200 && (u64)w * h > big_area) { big_area = (u64)w * h; mbig = (int)m; }
        if (m == cur) cur_ok = ok && w >= 800 && h >= 600;
        BS->FreePool(info);
    }
    int pick = m1024 >= 0 ? m1024 : cur_ok ? (int)cur : mbig;
    if ((bi->boot_flags & BOOTF_SAFE_VIDEO) && m800 >= 0) pick = m800;
    if (pick < 0) {
        if (blt_only) fail("graphics adapter only offers PixelBltOnly modes (no linear framebuffer)", 0);
        fail("no usable 32-bit graphics mode", 0);
    }
    if ((u32)pick != cur) {
        st = gop->SetMode(gop, (u32)pick);
        if (EFI_ERROR(st)) {
            if (!cur_ok) fail("cannot set the graphics mode", st);
            gop->SetMode(gop, cur);   /* restore whatever the firmware had */
        }
    }

    info = gop->Mode->Info;
    if (!info || !fmt_usable(info)) {
        if (info && info->PixelFormat == PixelBltOnly)
            fail("graphics mode is PixelBltOnly (no linear framebuffer)", 0);
        fail("graphics mode has an unsupported pixel format", 0);
    }
    if (!gop->Mode->FrameBufferBase) fail("graphics mode has no framebuffer address", 0);

    bi->fb_addr = gop->Mode->FrameBufferBase;
    bi->fb_width = info->HorizontalResolution;
    bi->fb_height = info->VerticalResolution;
    bi->fb_pitch = info->PixelsPerScanLine * 4;
    bi->fb_bpp = 32;
    switch (info->PixelFormat) {
    case PixelRedGreenBlueReserved8BitPerColor:   /* byte 0 = R */
        bi->fb_red_pos = 0;  bi->fb_green_pos = 8; bi->fb_blue_pos = 16;
        bi->fb_red_size = bi->fb_green_size = bi->fb_blue_size = 8;
        break;
    case PixelBlueGreenRedReserved8BitPerColor:   /* byte 0 = B */
        bi->fb_red_pos = 16; bi->fb_green_pos = 8; bi->fb_blue_pos = 0;
        bi->fb_red_size = bi->fb_green_size = bi->fb_blue_size = 8;
        break;
    default:
        mask_field(info->PixelInformation.RedMask, &bi->fb_red_pos, &bi->fb_red_size);
        mask_field(info->PixelInformation.GreenMask, &bi->fb_green_pos, &bi->fb_green_size);
        mask_field(info->PixelInformation.BlueMask, &bi->fb_blue_pos, &bi->fb_blue_size);
        break;
    }

    con_puts("video: "); con_dec(bi->fb_width); con_puts("x"); con_dec(bi->fb_height);
    con_puts(" pitch "); con_dec(bi->fb_pitch); con_puts(" fb "); con_hex(bi->fb_addr); con_puts("\n");
}

/* ---------------- ACPI ---------------- */
static u64 find_rsdp(void) {
    EFI_GUID acpi20 = EFI_ACPI_20_TABLE_GUID, acpi10 = EFI_ACPI_10_TABLE_GUID;
    u64 r10 = 0;
    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *t = &ST->ConfigurationTable[i];
        if (memeq(&t->VendorGuid, &acpi20, sizeof(EFI_GUID))) return (u64)(uintptr_t)t->VendorTable;
        if (!r10 && memeq(&t->VendorGuid, &acpi10, sizeof(EFI_GUID))) r10 = (u64)(uintptr_t)t->VendorTable;
    }
    return r10;
}

/* ---------------- page tables ---------------- */
static u64 *pt_pool;
static int pt_used;

static u64 *pt_new(void) {
    if (pt_used >= PT_POOL_PAGES) fail("out of page-table memory", 0);
    return pt_pool + 512 * pt_used++;        /* pool is zeroed */
}

static void map_2m(u64 *pml4, u64 start, u64 end) {
    for (u64 a = start & ~(TWO_MIB - 1); a < end; a += TWO_MIB) {
        u64 i4 = (a >> 39) & 511, i3 = (a >> 30) & 511, i2 = (a >> 21) & 511;
        if (!(pml4[i4] & 1)) pml4[i4] = (u64)(uintptr_t)pt_new() | 3;
        u64 *p3 = (u64 *)(uintptr_t)(pml4[i4] & ~0xFFFull);
        if (!(p3[i3] & 1)) p3[i3] = (u64)(uintptr_t)pt_new() | 3;
        u64 *p2 = (u64 *)(uintptr_t)(p3[i3] & ~0xFFFull);
        p2[i2] = a | 0x83;                    /* P | RW | PS */
    }
}

static u64 build_page_tables(const struct bootinfo *bi) {
    pt_pool = alloc_low(PT_POOL_PAGES, EfiLoaderData, "page tables");
    u64 *pml4 = pt_new();
    map_2m(pml4, 0, FOUR_GIB);
    u64 fb_end = bi->fb_addr + (u64)bi->fb_pitch * bi->fb_height;
    if (fb_end > FOUR_GIB) map_2m(pml4, bi->fb_addr < FOUR_GIB ? FOUR_GIB : bi->fb_addr, fb_end);
    u64 cr3 = (u64)(uintptr_t)pml4, cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (cr4 & (1ull << 12)) {                 /* firmware runs with 5-level paging */
        u64 *pml5 = pt_new();
        pml5[0] = cr3 | 3;
        cr3 = (u64)(uintptr_t)pml5;
    }
    return cr3;
}

/* ---------------- memory map ---------------- */
static u32 mmap_type(u32 t) {
    switch (t) {
    case EfiConventionalMemory: case EfiBootServicesCode: case EfiBootServicesData:
    case EfiLoaderCode: case EfiLoaderData:
        return MMAP_USABLE;
    case EfiACPIReclaimMemory: return MMAP_ACPI;
    case EfiACPIMemoryNVS:     return MMAP_NVS;
    case EfiUnusableMemory:    return MMAP_BAD;
    default:                   return MMAP_RESERVED;
    }
}

#define DESC(map, i, dsz) ((EFI_MEMORY_DESCRIPTOR *)((u8 *)(map) + (i) * (dsz)))

/* the kernel destination must be entirely RAM that is ours after EBS */
static int check_kernel_area(EFI_MEMORY_DESCRIPTOR *map, UINTN n, UINTN dsz, u32 *bad_type) {
    u64 covered = 0;
    for (UINTN i = 0; i < n; i++) {
        EFI_MEMORY_DESCRIPTOR *d = DESC(map, i, dsz);
        u64 b = d->PhysicalStart, e = b + d->NumberOfPages * PAGE_SIZE;
        if (e <= kdest || b >= kdest_end) continue;
        if (mmap_type(d->Type) != MMAP_USABLE) { *bad_type = d->Type; return 0; }
        covered += (e < kdest_end ? e : kdest_end) - (b > kdest ? b : kdest);
    }
    *bad_type = 0xFFFFFFFF;                   /* hole: not RAM */
    return covered == kdest_end - kdest;
}

static void convert_mmap(EFI_MEMORY_DESCRIPTOR *map, UINTN n, UINTN dsz,
                         struct bootinfo_mmap *work, struct bootinfo *bi) {
    UINTN cnt = 0;
    for (UINTN i = 0; i < n; i++) {
        EFI_MEMORY_DESCRIPTOR *d = DESC(map, i, dsz);
        if (!d->NumberOfPages) continue;
        work[cnt].base = d->PhysicalStart;
        work[cnt].length = d->NumberOfPages * PAGE_SIZE;
        work[cnt].type = mmap_type(d->Type);
        work[cnt].reserved = 0;
        cnt++;
    }
    /* insertion sort by base (the UEFI map is not guaranteed to be sorted) */
    for (UINTN i = 1; i < cnt; i++) {
        struct bootinfo_mmap t = work[i];
        UINTN j = i;
        while (j > 0 && work[j - 1].base > t.base) { work[j] = work[j - 1]; j--; }
        work[j] = t;
    }
    /* merge adjacent entries of the same type */
    UINTN m = 0;
    for (UINTN i = 0; i < cnt; i++) {
        if (m && work[m - 1].type == work[i].type && work[m - 1].base + work[m - 1].length == work[i].base)
            work[m - 1].length += work[i].length;
        else
            work[m++] = work[i];
    }
    /* too many entries: drop plain reserved ranges first (the kernel only
     * allocates from usable memory), then truncate */
    int drop_reserved = m > BOOTINFO_MAX_MMAP;
    bi->mmap_count = 0;
    for (UINTN i = 0; i < m && bi->mmap_count < BOOTINFO_MAX_MMAP; i++) {
        if (drop_reserved && work[i].type == MMAP_RESERVED) continue;
        bi->mmap[bi->mmap_count++] = work[i];
    }
}

/* ---------------- hand-off trampoline ----------------
 * Position independent; copied into its own page below 4 GiB and entered
 * with interrupts off:
 *   rbx = bootinfo, rsi = staged kernel (0 = already in place), rdi = load
 *   address, rcx = bytes to copy, rdx = start of the area to zero, r8 = its
 *   length, r9 = new CR3, r10 = kernel entry
 * Uses a stack at the end of its page and its own GDT, so nothing of the
 * loader or the firmware is needed once it runs. */
__asm__(
    "    .text\n"
    "    .p2align 4\n"
    "    .globl halcyon_tramp_start\n"
    "halcyon_tramp_start:\n"
    "    cli\n"
    "    lea halcyon_tramp_start(%rip), %rsp\n"
    "    add $4096, %rsp\n"
    "    mov %r9, %cr3\n"
    "    lea tramp_gdt(%rip), %rax\n"
    "    mov %rax, tramp_gdtr+2(%rip)\n"
    "    lgdt tramp_gdtr(%rip)\n"
    "    lea 1f(%rip), %rax\n"
    "    pushq $0x08\n"
    "    pushq %rax\n"
    "    lretq\n"
    "1:  mov $0x10, %eax\n"
    "    mov %eax, %ds\n"
    "    mov %eax, %es\n"
    "    mov %eax, %ss\n"
    "    mov %eax, %fs\n"
    "    mov %eax, %gs\n"
    "    cld\n"
    "    test %rsi, %rsi\n"
    "    jz 2f\n"
    "    rep movsb\n"
    "2:  mov %rdx, %rdi\n"
    "    mov %r8, %rcx\n"
    "    xor %eax, %eax\n"
    "    rep stosb\n"
    "    mov %rbx, %rdi\n"
    "    xor %ebp, %ebp\n"
    "    jmp *%r10\n"
    "    .p2align 3\n"
    "tramp_gdt:\n"
    "    .quad 0\n"
    "    .quad 0x00AF9A000000FFFF\n"       /* 0x08: 64-bit code */
    "    .quad 0x00CF92000000FFFF\n"       /* 0x10: data */
    "tramp_gdtr:\n"
    "    .word 23\n"
    "    .quad 0\n"
    "    .globl halcyon_tramp_end\n"
    "halcyon_tramp_end:\n"
);
extern const u8 halcyon_tramp_start[], halcyon_tramp_end[];

/* ---------------- main ---------------- */

/* ---------------- F8 Advanced Startup Options ---------------- */
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

static int read_key(EFI_INPUT_KEY *k) {
    if (!ST->ConIn) return 0;
    return !EFI_ERROR(ST->ConIn->ReadKeyStroke(ST->ConIn, k));
}

static void at(int x, int y, const char *s, UINTN attr) {
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *o = ST->ConOut;
    o->SetCursorPosition(o, (UINTN)x, (UINTN)y);
    o->SetAttribute(o, attr);
    con_puts(s);
}

static u32 boot_menu(void) {
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *o = ST->ConOut;
    u32 flags = 0;
    int sel = 0;
    o->EnableCursor(o, 0);
    for (;;) {
        o->SetAttribute(o, 0x07);
        o->ClearScreen(o);
        at(18, 1, "Halcyon Advanced Startup Options", 0x0F);
        at(4, 3, "Use the arrow keys to highlight an item, Enter to choose or toggle it.", 0x07);
        for (int i = 0; i < NOPTS; i++) {
            char line[72];
            int n = 0;
            for (int k = 0; k < 2; k++) line[n++] = ' ';
            if (opts[i].flag) { line[n++] = '['; line[n++] = (flags & opts[i].flag) ? 'X' : ' '; line[n++] = ']'; line[n++] = ' '; }
            for (const char *c = opts[i].label; *c && n < 64; c++) line[n++] = *c;
            while (n < 66) line[n++] = ' ';
            line[n] = 0;
            at(6, 6 + i + (i >= 3 ? 1 : 0), line, i == sel ? 0x70 : 0x07);
        }
        /* options use rows 6-8 and 10-20 */
        at(4, 22, "The kernel log is saved to \\HALCYON\\BOOT.LOG on the boot drive", 0x08);
        at(4, 23, "once Halcyon reaches the desktop.", 0x08);
        o->SetAttribute(o, 0x07);
        EFI_INPUT_KEY k;
        while (!read_key(&k)) BS->Stall(10000);
        if (k.ScanCode == EFI_SCAN_UP) sel = (sel + NOPTS - 1) % NOPTS;
        else if (k.ScanCode == EFI_SCAN_DOWN) sel = (sel + 1) % NOPTS;
        else if (k.ScanCode == EFI_SCAN_ESC) { flags = 0; break; }
        else if (k.UnicodeChar == '\r' || k.UnicodeChar == ' ') {
            if (sel == 0) { flags = 0; break; }
            if (sel == 1) { flags = BOOTF_SAFE_MODE; break; }
            if (sel == 2) break;
            flags ^= opts[sel].flag;
        }
    }
    o->SetAttribute(o, 0x07);
    o->ClearScreen(o);
    return flags;
}

static u32 check_f8(void) {
    con_puts("Press F8 for Advanced Startup Options");
    if (ST->ConIn) ST->ConIn->Reset(ST->ConIn, 0);
    for (int i = 0; i < 100; i++) {                 /* ~1 second */
        EFI_INPUT_KEY k;
        if (read_key(&k) && k.ScanCode == EFI_SCAN_F8) { con_puts("\n"); return boot_menu(); }
        if (i % 25 == 24) con_puts(".");
        BS->Stall(10000);
    }
    con_puts("\n");
    return 0;
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab) {
    ST = systab;
    BS = systab->BootServices;
    image_handle = image;
    EFI_STATUS st;

    BS->SetWatchdogTimer(0, 0, 0, 0);
    con_puts("\nHalcyon UEFI loader\n");

    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *li = 0;
    st = BS->HandleProtocol(image, &li_guid, (void **)&li);
    if (EFI_ERROR(st) || !li) fail("cannot get LoadedImage protocol", st);

    struct kernel_load kl;
    load_kernel(li, &kl);

    /* everything the hand-off needs is allocated now, below 4 GiB */
    struct bootinfo *bi = alloc_low(PAGES(sizeof *bi), EfiLoaderData, "boot information");
    u8 *tramp = alloc_low(1, EfiLoaderCode, "the hand-off trampoline");
    u64 tramp_len = (u64)(halcyon_tramp_end - halcyon_tramp_start);
    if (tramp_len > 2048) fail("trampoline too large", 0);
    memcpy(tramp, halcyon_tramp_start, tramp_len);

    bi->magic = BOOTINFO_MAGIC;
    bi->boot_type = BOOT_TYPE_UEFI;
    bi->boot_flags = check_f8();
    bi->bios_drive = 0;
    find_boot_partition(li, bi);
    bi->rsdp = find_rsdp();
    setup_gop(bi);
    u64 cr3 = build_page_tables(bi);
    if (!bi->rsdp) con_puts("warning: no ACPI RSDP in the configuration table\n");

    /* memory map buffers, with slack for allocations made meanwhile */
    UINTN msize = 0, key = 0, dsz = 0;
    UINT32 dver = 0;
    st = BS->GetMemoryMap(&msize, 0, &key, &dsz, &dver);
    if (st != EFI_BUFFER_TOO_SMALL || dsz < sizeof(EFI_MEMORY_DESCRIPTOR))
        fail("GetMemoryMap failed", st);
    UINTN cap = 0;
    EFI_MEMORY_DESCRIPTOR *map = 0;
    struct bootinfo_mmap *work = 0;

    for (int attempt = 0;; attempt++) {
        if (!map || (st == EFI_BUFFER_TOO_SMALL && !ebs_called)) {
            if (map) { BS->FreePool(map); BS->FreePool(work); }
            cap = msize + 64 * dsz;
            st = BS->AllocatePool(EfiLoaderData, cap, (void **)&map);
            if (EFI_ERROR(st)) fail("cannot allocate the memory map buffer", st);
            st = BS->AllocatePool(EfiLoaderData, (cap / dsz + 1) * sizeof *work, (void **)&work);
            if (EFI_ERROR(st)) fail("cannot allocate the memory map buffer", st);
        }
        if (attempt > 16) fail("ExitBootServices keeps failing", st);
        msize = cap;
        st = BS->GetMemoryMap(&msize, map, &key, &dsz, &dver);
        if (st == EFI_BUFFER_TOO_SMALL && !ebs_called) continue;
        if (EFI_ERROR(st)) fail("GetMemoryMap failed", st);

        u32 bad;
        if (!check_kernel_area(map, msize / dsz, dsz, &bad)) {
            con_puts("kernel area "); con_hex(kdest); con_puts("-"); con_hex(kdest_end);
            if (bad == 0xFFFFFFFF) con_puts(" is not fully backed by RAM\n");
            else { con_puts(" is in use by the firmware (memory type "); con_dec(bad); con_puts(")\n"); }
            fail("cannot place the kernel at its load address", 0);
        }

        st = BS->ExitBootServices(image_handle, key);
        if (!EFI_ERROR(st)) break;
        ebs_called = 1;          /* map key was stale: only GetMemoryMap+EBS from here on */
    }
    ebs_called = 1;

    /* boot services are gone: no more console output, no allocations */
    convert_mmap(map, msize / dsz, dsz, work, bi);

    u64 zero_start = kdest + kl.size;
    u64 zero_len = kl.hdr.bss_end > zero_start ? kl.hdr.bss_end - zero_start : 0;
    register u64 r8 __asm__("r8") = zero_len;
    register u64 r9 __asm__("r9") = cr3;
    register u64 r10 __asm__("r10") = kl.hdr.entry;
    __asm__ volatile(
        "cli\n\t"
        "jmp *%%rax"
        :: "a"(tramp), "b"(bi), "S"(kl.tmp), "D"(kdest), "c"(kl.tmp ? kl.size : 0),
           "d"(zero_start), "r"(r8), "r"(r9), "r"(r10)
        : "memory");
    __builtin_unreachable();
}
