/* Halcyon OS - UEFI runtime services: firmware variables (see efirt.h) */
#include "efirt.h"
#include "kernel.h"
#include "lib.h"
#include "mm.h"

const u8 EFI_GLOBAL_GUID[16] = { 0x61, 0xDF, 0xE4, 0x8B, 0xCA, 0x93, 0xD2, 0x11,
                                 0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C };

#define RT_SIGNATURE 0x56524553544E5552ull      /* "RUNTSERV" */
#define RT_GET_VARIABLE 72                      /* offsets in EFI_RUNTIME_SERVICES */
#define RT_SET_VARIABLE 88
#define EFI_ERR(n) (0x8000000000000000ull | (n))
#define EFI_BUFFER_TOO_SMALL EFI_ERR(5)
#define EFI_NOT_FOUND EFI_ERR(14)
#define EFI_DEVICE_ERROR EFI_ERR(7)

/* ---------------- calling the firmware ----------------
 * efi_thunk(fn, a1..a5): the SysV arguments moved to the Microsoft x64
 * convention (rcx, rdx, r8, r9, then the stack above 32 bytes of shadow
 * space), on efi_stack, interrupts off. efi_recover is where a CPU exception
 * inside the firmware continues (efi_fault_fixup): the call returns
 * EFI_DEVICE_ERROR with the caller's registers and stack restored. */
#define EFI_STACK (64 * 1024)
u8 efi_stack[EFI_STACK] ALIGNED(16) __attribute__((used));   /* (used by the thunk only) */
u64 efi_saved_rsp;
static volatile int efi_in_call, efi_broken;
u64 efi_thunk(u64 fn, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5);
extern char efi_recover[];

__asm__(
    "    .text\n"
    "    .globl efi_thunk\n"
    "efi_thunk:\n"
    "    push %rbp\n"
    "    mov %rsp, %rbp\n"
    "    push %rbx\n"
    "    push %r12\n"
    "    push %r13\n"
    "    push %r14\n"
    "    push %r15\n"
    "    pushfq\n"
    "    cli\n"
    "    cld\n"
    "    mov %rsp, efi_saved_rsp(%rip)\n"
    "    lea efi_stack+65536(%rip), %rsp\n"
    "    sub $48, %rsp\n"
    "    mov %r9, 32(%rsp)\n"
    "    mov %r8, %r9\n"
    "    mov %rcx, %r8\n"
    "    mov %rsi, %rcx\n"
    "    call *%rdi\n"
    "1:  mov efi_saved_rsp(%rip), %rsp\n"
    "    popfq\n"
    "    pop %r15\n"
    "    pop %r14\n"
    "    pop %r13\n"
    "    pop %r12\n"
    "    pop %rbx\n"
    "    pop %rbp\n"
    "    ret\n"
    "    .globl efi_recover\n"
    "efi_recover:\n"
    "    movabs $0x8000000000000007, %rax\n"
    "    jmp 1b\n"
);

/* called by the CPU exception handler for exceptions in kernel mode */
int efi_fault_fixup(struct regs *r) {
    if (!efi_in_call) return 0;
    kprintf("efi: CPU exception %lu in the firmware's runtime services at %lx: they are not used again\n",
            r->vector, r->rip);
    efi_broken = 1;
    efi_in_call = 0;
    r->rip = (u64)(uptr)efi_recover;
    r->rsp = efi_saved_rsp;
    r->rflags &= ~0x200ull;                     /* interrupts stay off until the thunk's popfq */
    return 1;
}

static u64 rt_fn(int off) {
    return *(volatile u64 *)(uptr)(g_bootinfo.efi_runtime + (u64)off);
}

int efi_rt_available(void) {
    static int checked, ok;
    if (checked) return ok && !efi_broken;
    checked = 1;
    if (g_bootinfo.boot_type != BOOT_TYPE_UEFI || !g_bootinfo.efi_runtime || g_bootinfo.efi_runtime >= 0x100000000ull)
        return 0;
    if (*(volatile u64 *)(uptr)g_bootinfo.efi_runtime != RT_SIGNATURE) {
        kprintf("efi: the runtime services table has no signature\n");
        return 0;
    }
    /* what the firmware needs above the identity-mapped first 4 GiB */
    for (u32 i = 0; i < g_bootinfo.efi_rt_count && i < BOOTINFO_MAX_RT; i++) {
        const struct bootinfo_rtmem *m = &g_bootinfo.efi_rt[i];
        if (m->base + m->pages * 4096 > 0x100000000ull && !mm_map_mmio(m->base, m->pages * 4096)) {
            kprintf("efi: cannot map the runtime range at %llx\n", (unsigned long long)m->base);
            return 0;
        }
    }
    u64 g = rt_fn(RT_GET_VARIABLE), s = rt_fn(RT_SET_VARIABLE);
    ok = g && s;
    kprintf("efi: runtime services at %llx, %u ranges%s\n", (unsigned long long)g_bootinfo.efi_runtime,
            g_bootinfo.efi_rt_count, ok ? "" : ", no variable services");
    return ok;
}

static u64 call(int off, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    if (!efi_rt_available()) return EFI_DEVICE_ERROR;
    efi_in_call = 1;
    u64 st = efi_thunk(rt_fn(off), a1, a2, a3, a4, a5);
    efi_in_call = 0;
    return st;
}

static void ucs2(const char *s, u16 *out, int max) {
    int i = 0;
    while (s[i] && i < max - 1) { out[i] = (u8)s[i]; i++; }
    out[i] = 0;
}

static int status(u64 st) {
    if (!st) return 0;
    if (st == EFI_NOT_FOUND) return E_NOENT;
    if (st == EFI_BUFFER_TOO_SMALL) return E_NOSPC;
    return E_IO;
}

int efi_get_variable(const char *name, const u8 guid[16], u32 *attrs, void *buf, u32 *size) {
    u16 n[64];
    u8 g[16];
    u32 a = 0;
    u64 sz = *size;
    ucs2(name, n, 64);
    memcpy(g, guid, 16);
    int r = status(call(RT_GET_VARIABLE, (u64)(uptr)n, (u64)(uptr)g, (u64)(uptr)&a, (u64)(uptr)&sz, (u64)(uptr)buf));
    if (attrs) *attrs = a;
    *size = (u32)sz;
    return r;
}

int efi_set_variable(const char *name, const u8 guid[16], u32 attrs, const void *buf, u32 size) {
    u16 n[64];
    u8 g[16];
    ucs2(name, n, 64);
    memcpy(g, guid, 16);
    return status(call(RT_SET_VARIABLE, (u64)(uptr)n, (u64)(uptr)g, attrs, size, (u64)(uptr)buf));
}

/* ---------------- boot entries ---------------- */

static void wr16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void wr32(u8 *p, u32 v) { wr16(p, (u16)v); wr16(p + 2, (u16)(v >> 16)); }
static void wr64(u8 *p, u64 v) { wr32(p, (u32)v); wr32(p + 4, (u32)(v >> 32)); }
static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }

/* EFI_LOAD_OPTION: attributes, path length, description, HD()/File()/End */
static u32 build_option(u8 *o, const char *desc, const efi_hd_t *hd, const char *path) {
    u32 n = 0;
    wr32(o, 1);                                  /* LOAD_OPTION_ACTIVE */
    n = 6;
    for (const char *c = desc; *c; c++) { wr16(o + n, (u8)*c); n += 2; }
    wr16(o + n, 0); n += 2;
    u32 dp = n;
    u8 *h = o + n;                               /* hard drive media node: 42 bytes */
    memset(h, 0, 42);
    h[0] = 4; h[1] = 1; wr16(h + 2, 42);
    wr32(h + 4, hd->part_num);
    wr64(h + 8, hd->start);
    wr64(h + 16, hd->count);
    memcpy(h + 24, hd->sig, hd->gpt ? 16 : 4);
    h[40] = hd->gpt ? 2 : 1;                     /* partition format: GPT / MBR */
    h[41] = hd->gpt ? 2 : 1;                     /* signature type: GUID / 32-bit MBR signature */
    n += 42;
    u8 *f = o + n;                               /* file path media node */
    u32 len = 4;
    f[0] = 4; f[1] = 4;
    for (const char *c = path; *c; c++) { wr16(f + len, (u8)*c); len += 2; }
    wr16(f + len, 0); len += 2;
    wr16(f + 2, (u16)len);
    n += len;
    o[n] = 0x7F; o[n + 1] = 0xFF; wr16(o + n + 2, 4);   /* end of the device path */
    n += 4;
    wr16(o + 4, (u16)(n - dp));
    return n;
}

static void boot_name(char *out, u32 num) { snprintf(out, 16, "Boot%04X", num); }

/* the option at o (size n) has description desc and the same partition */
static int same_entry(const u8 *o, u32 n, const char *desc, const efi_hd_t *hd) {
    u32 i = 6;
    for (const char *c = desc; *c; c++, i += 2) if (i + 1 >= n || rd16(o + i) != (u8)*c) return 0;
    if (i + 1 >= n || rd16(o + i)) return 0;
    i += 2;
    if (i + 42 > n || o[i] != 4 || o[i + 1] != 1) return 0;
    return !memcmp(o + i + 24, hd->sig, hd->gpt ? 16 : 4);
}

int efi_add_boot_entry(const char *desc, const efi_hd_t *hd, const char *path) {
    if (!efi_rt_available()) return E_NOTSUP;
    static u8 opt[512], cur[512];
    static u16 order[256], check[256];
    u32 n = build_option(opt, desc, hd, path);
    u32 osz = sizeof order, attrs;
    int r = efi_get_variable("BootOrder", EFI_GLOBAL_GUID, &attrs, order, &osz);
    u32 count = r ? 0 : osz / 2;
    if (r && r != E_NOENT) { kprintf("efi: BootOrder: %s\n", strerror(r)); return r; }
    /* reuse our own entry for this partition, else take the first free number */
    int num = -1;
    char name[16];
    for (u32 i = 0; i < count && num < 0; i++) {
        u32 sz = sizeof cur;
        boot_name(name, order[i]);
        if (!efi_get_variable(name, EFI_GLOBAL_GUID, NULL, cur, &sz) && same_entry(cur, sz, desc, hd)) num = order[i];
    }
    for (u32 k = 0; k < 0x1000 && num < 0; k++) {
        u32 sz = sizeof cur;
        boot_name(name, k);
        r = efi_get_variable(name, EFI_GLOBAL_GUID, NULL, cur, &sz);
        if (r == E_NOENT) num = (int)k;
        else if (r && r != E_NOSPC) return r;
    }
    if (num < 0) return E_NOSPC;
    boot_name(name, (u32)num);
    u32 va = EFI_VAR_NV | EFI_VAR_BS | EFI_VAR_RT;
    r = efi_set_variable(name, EFI_GLOBAL_GUID, va, opt, n);
    if (r) { kprintf("efi: writing %s: %s\n", name, strerror(r)); return r; }
    /* first in BootOrder */
    u32 m = 0;
    check[m++] = (u16)num;
    for (u32 i = 0; i < count && m < 256; i++) if (order[i] != num) check[m++] = order[i];
    memcpy(order, check, m * 2);
    r = efi_set_variable("BootOrder", EFI_GLOBAL_GUID, va, order, m * 2);
    if (r) { kprintf("efi: writing BootOrder: %s\n", strerror(r)); return r; }
    /* read both back */
    u32 sz = sizeof cur;
    r = efi_get_variable(name, EFI_GLOBAL_GUID, NULL, cur, &sz);
    if (!r && (sz != n || memcmp(cur, opt, n))) r = E_IO;
    sz = sizeof check;
    if (!r) r = efi_get_variable("BootOrder", EFI_GLOBAL_GUID, NULL, check, &sz);
    if (!r && (sz != m * 2 || memcmp(check, order, sz))) r = E_IO;
    if (r) { kprintf("efi: %s / BootOrder did not read back as written\n", name); return r; }
    kprintf("efi: boot entry %s '%s' added, first of %u in BootOrder\n", name, desc, m);
    return num;
}

void efi_log_boot_entry(void) {
    if (g_bootinfo.boot_type != BOOT_TYPE_UEFI || g_bootinfo.efi_boot_current > 0xFFFF) return;
    char d[sizeof g_bootinfo.efi_boot_desc + 1];
    memcpy(d, g_bootinfo.efi_boot_desc, sizeof g_bootinfo.efi_boot_desc);
    d[sizeof d - 1] = 0;
    kprintf("efi: started from firmware boot entry Boot%04X '%s'\n", g_bootinfo.efi_boot_current, d);
}
