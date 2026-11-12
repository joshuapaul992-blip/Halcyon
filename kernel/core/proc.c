/* Halcyon OS - user processes: address spaces, ELF loading, lifecycle */
#include "kernel.h"
#include "lib.h"
#include "io.h"
#include "mm.h"
#include "irq.h"
#include "thread.h"
#include "proc.h"
#include "fs/vfs.h"

#define PTE_P   0x001ull
#define PTE_RW  0x002ull
#define PTE_US  0x004ull
#define PTE_NX  (1ull << 63)
#define PTE_SHARED (1ull << 9)   /* available bit: the frame is not the process's own (shared mapping) */
#define PTE_ADDR 0x000FFFFFFFFFF000ull
#define VM_W 1
#define VM_X 2
static u64 nx_bit;               /* PTE_NX when the CPU supports it (EFER.NXE set) */
static u64 slice_end;            /* TSC when the current user thread's slice ends */
#define PML4_USER_FIRST 64
#define PML4_USER_LAST  255
#define KERNEL_RESERVE  (32ull << 20)       /* heap kept free for the kernel */

u64 *mm_kernel_pml4(void);                         /* mm.c */
void irq_timeslice(u64 ms);                        /* irq.c */
void irq_timeslice_tsc(u64 ticks);
extern void user_enter(u64 rip, u64 rsp, u64 arg); /* entry.asm */
extern void (*thread_switch_hook)(thread_t *next); /* thread.c */

static proc_t *procs;
static u32 next_pid = 1;
volatile int g_need_resched;
void (*proc_release_hook)(proc_t *p);
void (*proc_crash_hook)(proc_t *p);

proc_t *proc_first(void) { return procs; }
proc_t *proc_current(void) { thread_t *t = thread_current(); return t ? (proc_t *)t->proc : NULL; }
proc_t *proc_by_pid(u32 pid) {
    for (proc_t *p = procs; p; p = p->next) if (p->pid == pid) return p;
    return NULL;
}
int proc_alive(u32 pid) { proc_t *p = proc_by_pid(pid); return p && p->state == P_RUNNING; }
int proc_exit_code(u32 pid) { proc_t *p = proc_by_pid(pid); return p ? p->exit_code : E_NOENT; }
u64 proc_cpu_ms(proc_t *p) {
    u64 t = p->cpu_tsc;
    for (thread_t *x = thread_first(); x; x = x->next)
        if (x->proc == p && x->state != T_DEAD) t += x->cpu_tsc + (x == thread_current() ? rdtsc() - x->run_start : 0);
    return t / tsc_khz();
}

/* ---------------- page pool ---------------- */
/* User pages and page tables come from 1 MiB chunks of the kernel heap
 * (the heap's first-fit list would be far too slow for single pages); a chunk
 * goes back to the heap when all of its pages are free again. */
#define CHUNK_PAGES 256
typedef struct chunk {
    u8 *base;
    u32 nfree;
    u64 used[CHUNK_PAGES / 64];
    struct chunk *next;
} chunk_t;
static chunk_t *chunks;

static void *page_alloc(void) {
    chunk_t *c = chunks;
    while (c && !c->nfree) c = c->next;
    if (!c) {
        if (mm_heap_free() < KERNEL_RESERVE + CHUNK_PAGES * 4096) return NULL;
        c = kzalloc(sizeof *c);
        u8 *b = c ? kmalloc_aligned(CHUNK_PAGES * 4096, 4096) : NULL;
        if (!b) { kfree(c); return NULL; }
        c->base = b;
        c->nfree = CHUNK_PAGES;
        c->next = chunks;
        chunks = c;
    }
    for (int w = 0; w < CHUNK_PAGES / 64; w++) {
        if (c->used[w] == ~0ull) continue;
        int bit = __builtin_ctzll(~c->used[w]);
        c->used[w] |= 1ull << bit;
        c->nfree--;
        u8 *pg = c->base + (u64)(w * 64 + bit) * 4096;
        memset(pg, 0, 4096);
        return pg;
    }
    return NULL;
}

static chunk_t *free_hint;
static void page_free(void *pg) {
    chunk_t *h = free_hint;
    if (h && (u8 *)pg >= h->base && (u8 *)pg < h->base + CHUNK_PAGES * 4096 && h->nfree + 1 < CHUNK_PAGES) {
        u64 i = (u64)((u8 *)pg - h->base) / 4096;      /* fast path: same chunk as last time, not emptied */
        h->used[i / 64] &= ~(1ull << (i % 64));
        h->nfree++;
        return;
    }
    chunk_t **pp = &chunks;
    for (; *pp; pp = &(*pp)->next) {
        chunk_t *c = *pp;
        if ((u8 *)pg < c->base || (u8 *)pg >= c->base + CHUNK_PAGES * 4096) continue;
        u64 i = (u64)((u8 *)pg - c->base) / 4096;
        c->used[i / 64] &= ~(1ull << (i % 64));
        free_hint = c;
        if (++c->nfree == CHUNK_PAGES) {
            *pp = c->next;
            if (free_hint == c) free_hint = NULL;
            kfree(c->base);
            kfree(c);
        }
        return;
    }
    kprintf("proc: page_free: %p is not a user page\n", pg);
}

/* ---------------- address spaces ---------------- */

static u64 *table_new(void) { return page_alloc(); }

static int user_range(u64 va, u64 len) {
    return va >= USER_BASE && va < USER_END && len <= USER_END - va;
}

/* the level-1 entry for va, creating tables on the way if asked */
static u64 *walk(proc_t *p, u64 va, int create) {
    u64 *t = p->pml4;
    for (int shift = 39; shift > 12; shift -= 9) {
        u64 *e = &t[(va >> shift) & 511];
        if (!(*e & PTE_P)) {
            if (!create) return NULL;
            u64 *n = table_new();
            if (!n) return NULL;
            *e = (u64)(uptr)n | PTE_P | PTE_RW | PTE_US;
        }
        t = (u64 *)(uptr)(*e & PTE_ADDR);
    }
    return &t[(va >> 12) & 511];
}

/* map zeroed pages over [va, va+len); pages already mapped get the union of
 * the permissions (two segments can share a page) */
static int vm_alloc(proc_t *p, u64 va, u64 len, int prot) {
    if (!user_range(va, len)) return E_INVAL;
    u64 flags = PTE_P | PTE_US | ((prot & VM_W) ? PTE_RW : 0) | ((prot & VM_X) ? 0 : nx_bit);
    for (u64 a = va & ~0xFFFull; a < va + len; a += 4096) {
        u64 *pte = walk(p, a, 1);
        if (!pte) return E_NOMEM;
        if (*pte & PTE_P) {
            if (prot & VM_W) *pte |= PTE_RW;
            if (prot & VM_X) *pte &= ~PTE_NX;
            continue;
        }
        if (p->pages >= USER_MAX_PAGES) return E_NOMEM;
        void *frame = page_alloc();          /* keeps the kernel's reserve */
        if (!frame) return E_NOMEM;
        *pte = (u64)(uptr)frame | flags;
        p->pages++;
    }
    return 0;
}

int vm_grow(proc_t *p, u64 va, u64 len) { return vm_alloc(p, va, len, VM_W); }

static void vm_destroy(proc_t *p) {
    if (!p->pml4) return;
    for (int i4 = PML4_USER_FIRST; i4 <= PML4_USER_LAST; i4++) {
        if (!(p->pml4[i4] & PTE_P)) continue;
        u64 *l3 = (u64 *)(uptr)(p->pml4[i4] & PTE_ADDR);
        for (int i3 = 0; i3 < 512; i3++) {
            if (!(l3[i3] & PTE_P)) continue;
            u64 *l2 = (u64 *)(uptr)(l3[i3] & PTE_ADDR);
            for (int i2 = 0; i2 < 512; i2++) {
                if (!(l2[i2] & PTE_P)) continue;
                u64 *l1 = (u64 *)(uptr)(l2[i2] & PTE_ADDR);
                for (int i1 = 0; i1 < 512; i1++)
                    if ((l1[i1] & PTE_P) && !(l1[i1] & PTE_SHARED)) page_free((void *)(uptr)(l1[i1] & PTE_ADDR));
                page_free(l1);
            }
            page_free(l2);
        }
        page_free(l3);
    }
    page_free(p->pml4);
    p->pml4 = NULL;
    p->pages = 0;
}

/* unmap [va, va+len): the process's own frames go back to the pool */
static void vm_free_range(proc_t *p, u64 va, u64 len) {
    int live = 0;
    u64 cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    live = (u64 *)(uptr)(cr3 & PTE_ADDR) == p->pml4;
    for (u64 a = va & ~0xFFFull; a < va + len; a += 4096) {
        u64 *pte = walk(p, a, 0);
        if (!pte || !(*pte & PTE_P)) continue;
        if (!(*pte & PTE_SHARED)) { page_free((void *)(uptr)(*pte & PTE_ADDR)); p->pages--; }
        *pte = 0;
        if (live) __asm__ volatile("invlpg (%0)" :: "r"(a) : "memory");
    }
}

u64 vm_map_shared(proc_t *p, void *mem, u64 size, int writable) {
    if (!p || !p->pml4 || !mem || ((uptr)mem & 0xFFF) || !size || (size & 0xFFF) || size > MAP_SLOT) return 0;
    int slot = -1;
    for (int i = 0; i < MAP_SLOTS && slot < 0; i++) if (!(p->map_used[i / 64] & (1ull << (i % 64)))) slot = i;
    if (slot < 0) return 0;
    u64 va = MAP_BASE + (u64)slot * MAP_SLOT;
    u64 flags = PTE_P | PTE_US | PTE_SHARED | (writable ? PTE_RW : 0) | nx_bit;
    for (u64 o = 0; o < size; o += 4096) {
        u64 *pte = walk(p, va + o, 1);
        if (!pte) { vm_free_range(p, va, o); return 0; }
        *pte = ((u64)(uptr)mem + o) | flags;
    }
    p->map_used[slot / 64] |= 1ull << (slot % 64);
    p->map_pages[slot] = (u32)(size / 4096);
    return va;
}

int vm_unmap_shared(proc_t *p, u64 va) {
    if (!p || !p->pml4 || va < MAP_BASE || (va - MAP_BASE) % MAP_SLOT) return E_INVAL;
    u64 slot = (va - MAP_BASE) / MAP_SLOT;
    if (slot >= MAP_SLOTS || !(p->map_used[slot / 64] & (1ull << (slot % 64)))) return E_INVAL;
    vm_free_range(p, va, (u64)p->map_pages[slot] * 4096);
    p->map_used[slot / 64] &= ~(1ull << (slot % 64));
    p->map_pages[slot] = 0;
    return 0;
}

static u64 *cr3_now(void) { u64 v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return (u64 *)(uptr)(v & PTE_ADDR); }
static void cr3_load(u64 *pml4) { if (cr3_now() != pml4) __asm__ volatile("mov %0, %%cr3" :: "r"((u64)(uptr)pml4) : "memory"); }

/* thread switch: the next thread's address space (kernel entries refreshed,
 * in case the kernel mapped something since); user threads get a time slice */
static void switch_hook(thread_t *n) {
    proc_t *p = n->proc;
    if (p && p->pml4) {
        u64 *k = mm_kernel_pml4();
        for (int i = 0; i < 512; i++) if (i < PML4_USER_FIRST || i > PML4_USER_LAST) p->pml4[i] = k[i];
        cr3_load(p->pml4);
        wrmsr(0xC0000100, n->fs_base);             /* FS base: the thread's TLS */
        slice_end = rdtsc() + 10 * tsc_khz();
        irq_timeslice(10);
    } else {
        cr3_load(mm_kernel_pml4());
    }
}

/* translate a user address of the current process: kernel pointer or NULL */
static u8 *uaddr(u64 va, int write) {
    proc_t *p = proc_current();
    if (!p || !p->pml4 || va < USER_BASE || va >= USER_END) return NULL;
    u64 *pte = walk(p, va, 0);
    if (!pte || !(*pte & PTE_P) || !(*pte & PTE_US) || (write && !(*pte & PTE_RW))) return NULL;
    return (u8 *)(uptr)((*pte & PTE_ADDR) + (va & 0xFFF));
}

int user_writable(u64 uva, u64 len) {
    if (!len) return 1;
    if (!user_range(uva, len)) return 0;
    for (u64 a = uva & ~0xFFFull; a < uva + len; a += 4096) if (!uaddr(a < uva ? uva : a, 1)) return 0;
    return 1;
}

int copyin(void *dst, u64 uva, u64 len) {
    if (!len) return 0;
    if (!user_range(uva, len)) return E_INVAL;
    u8 *d = dst;
    while (len) {
        u8 *k = uaddr(uva, 0);
        if (!k) return E_INVAL;
        u64 n = MIN(len, 4096 - (uva & 0xFFF));
        memcpy(d, k, n);
        d += n; uva += n; len -= n;
    }
    return 0;
}

int copyout(u64 uva, const void *src, u64 len) {
    if (!len) return 0;
    if (!user_range(uva, len)) return E_INVAL;
    const u8 *s = src;
    while (len) {
        u8 *k = uaddr(uva, 1);
        if (!k) return E_INVAL;
        u64 n = MIN(len, 4096 - (uva & 0xFFF));
        memcpy(k, s, n);
        s += n; uva += n; len -= n;
    }
    return 0;
}

int copyin_str(char *dst, u64 uva, u64 max) {
    for (u64 i = 0; i < max; i++) {
        u8 *k = uaddr(uva + i, 0);
        if (!k) return E_INVAL;
        dst[i] = (char)*k;
        if (!*k) return 0;
    }
    if (max) dst[max - 1] = 0;
    return E_NAMETOOLONG;
}

/* copy into the address space of p (which is not current: loading) */
static int load_copy(proc_t *p, u64 va, const void *src, u64 len) {
    const u8 *s = src;
    while (len) {
        u64 *pte = walk(p, va, 0);
        if (!pte || !(*pte & PTE_P)) return E_INVAL;
        u8 *k = (u8 *)(uptr)((*pte & PTE_ADDR) + (va & 0xFFF));
        u64 n = MIN(len, 4096 - (va & 0xFFF));
        if (s) { memcpy(k, s, n); s += n; } else memset(k, 0, n);
        va += n; len -= n;
    }
    return 0;
}
static int load_read(proc_t *p, u64 va, void *dst, u64 len) {
    u8 *d = dst;
    while (len) {
        u64 *pte = walk(p, va, 0);
        if (!pte || !(*pte & PTE_P)) return E_INVAL;
        u8 *k = (u8 *)(uptr)((*pte & PTE_ADDR) + (va & 0xFFF));
        u64 n = MIN(len, 4096 - (va & 0xFFF));
        memcpy(d, k, n);
        d += n; va += n; len -= n;
    }
    return 0;
}

/* ---------------- ELF64 ---------------- */
typedef struct PACKED {
    u8 ident[16]; u16 type, machine; u32 version; u64 entry, phoff, shoff;
    u32 flags; u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} elf_ehdr_t;
typedef struct PACKED { u32 type, flags; u64 offset, vaddr, paddr, filesz, memsz, align; } elf_phdr_t;
typedef struct PACKED { i64 tag; u64 val; } elf_dyn_t;
typedef struct PACKED { u64 offset, info; i64 addend; } elf_rela_t;

#define MAX_IMAGE (64u * 1024 * 1024)

static int elf_load(proc_t *p, const u8 *f, u32 size, char *why, int whysz) {
#define FAIL(...) do { snprintf(why, whysz, __VA_ARGS__); return E_INVAL; } while (0)
    const elf_ehdr_t *h = (const elf_ehdr_t *)f;
    if (size < sizeof *h || memcmp(h->ident, "\x7F" "ELF", 4)) FAIL("not a Halcyon program (no ELF header)");
    if (h->ident[4] != 2 || h->ident[5] != 1 || h->machine != 0x3E) FAIL("not a 64-bit x86 program");
    if (h->type != 2 && h->type != 3) FAIL("not an executable (ELF type %u)", h->type);
    if (h->phentsize != sizeof(elf_phdr_t) || !h->phnum || h->phnum > 64 ||
        h->phoff > size || (u64)h->phnum * sizeof(elf_phdr_t) > size - h->phoff) FAIL("bad program headers");
    u64 base = h->type == 3 ? USER_BASE : 0;
    const elf_phdr_t *ph = (const elf_phdr_t *)(f + h->phoff);
    u64 lo = ~0ull, hi = 0, dyn = 0, dynsz = 0;
    for (int i = 0; i < h->phnum; i++) {
        const elf_phdr_t *s = &ph[i];
        if (s->type == 2) { dyn = s->vaddr; dynsz = s->memsz; }
        if (s->type != 1) continue;                     /* PT_LOAD */
        if (s->filesz > s->memsz || s->offset > size || s->filesz > size - s->offset || s->memsz > MAX_IMAGE)
            FAIL("bad segment %d", i);
        u64 va = base + s->vaddr;
        if (!user_range(va, s->memsz)) FAIL("segment %d outside user memory", i);
        if (vm_alloc(p, va, s->memsz, ((s->flags & 2) ? VM_W : 0) | ((s->flags & 1) ? VM_X : 0))) FAIL("out of memory");
        if (load_copy(p, va, f + s->offset, s->filesz)) FAIL("cannot load segment %d", i);
        if (va < lo) lo = va;
        if (va + s->memsz > hi) hi = va + s->memsz;
    }
    if (!hi) FAIL("no loadable segments");
    /* position independent: apply R_X86_64_RELATIVE relocations */
    if (dyn && base) {
        u64 rela = 0, relasz = 0, relaent = sizeof(elf_rela_t);
        for (u64 off = 0; off + sizeof(elf_dyn_t) <= dynsz && off < 64 * 1024; off += sizeof(elf_dyn_t)) {
            elf_dyn_t d;
            if (load_read(p, base + dyn + off, &d, sizeof d)) FAIL("bad dynamic section");
            if (d.tag == 0) break;
            if (d.tag == 7) rela = d.val;
            else if (d.tag == 8) relasz = d.val;
            else if (d.tag == 9) relaent = d.val;
        }
        if (relaent != sizeof(elf_rela_t)) FAIL("bad relocation entries");
        if (relasz > hi - lo || relasz > (16u << 20)) FAIL("relocation table too large");
        for (u64 off = 0; off < relasz; off += sizeof(elf_rela_t)) {
            elf_rela_t r;
            if (load_read(p, base + rela + off, &r, sizeof r)) FAIL("bad relocation table");
            u32 type = (u32)r.info;
            if (type == 0) continue;
            if (type != 8) FAIL("unsupported relocation type %u (link with -static-pie)", type);
            u64 v = base + (u64)r.addend;
            if (load_copy(p, base + r.offset, &v, 8)) FAIL("relocation outside the program");
        }
    }
    p->image_base = lo;
    p->entry = base + h->entry;
    if (p->entry < lo || p->entry >= hi) FAIL("entry point outside the program");
    p->brk_start = p->brk = ALIGN_UP(hi, 4096) + 0x100000;   /* heap after a 1 MiB gap */
    return 0;
#undef FAIL
}

/* ---------------- lifecycle ---------------- */

typedef struct start { proc_t *p; u64 sp, argp; } start_t;

static void user_main(void *arg) {
    start_t st = *(start_t *)arg;
    kfree(arg);
    user_enter(st.p->entry, st.sp, st.argp);
}

static void proc_reap(void) {
    proc_t **pp = &procs;
    while (*pp) {
        proc_t *p = *pp;
        if (p->state == P_ZOMBIE && !p->nthreads && !p->main && (!p->parent || !proc_alive(p->parent))) {
            *pp = p->next;
            kfree(p);
            continue;
        }
        pp = &p->next;
    }
}

int proc_spawn(const char *path, const char *args, const console_ops_t *con, void *con_ctx) {
    proc_reap();
    /* without a timer interrupt programs could not be preempted: one that
     * loops would hang the desktop */
    if (!idle_enabled()) {
        kprintf("proc: %s not started: programs need a working timer interrupt\n", path);
        return E_NOTSUP;
    }
    int running = 0;
    for (proc_t *q = procs; q; q = q->next) if (q->state == P_RUNNING) running++;
    if (running >= MAX_PROCS) return E_BUSY;
    char *data = NULL;
    u32 size = 0;
    int r = vfs_read_file(path, &data, &size);
    if (r < 0) return r;
    if (size > MAX_IMAGE) { kfree(data); return E_NOMEM; }
    proc_t *p = kzalloc(sizeof *p);
    if (!p) { kfree(data); return E_NOMEM; }
    p->pid = next_pid++;
    strlcpy(p->path, path, sizeof p->path);
    strlcpy(p->name, vfs_basename(path), sizeof p->name);
    for (char *c = p->name; *c; c++) *c = (char)toupper((u8)*c);
    p->pml4 = table_new();
    p->con = con;
    p->con_ctx = con_ctx;
    p->start_ms = time_ms();
    proc_t *parent = proc_current();
    p->parent = parent ? parent->pid : 0;
    char why[96] = "out of memory";
    r = p->pml4 ? elf_load(p, (const u8 *)data, size, why, sizeof why) : E_NOMEM;
    kfree(data);
    /* stack, with the argument string at its top */
    u64 argp = 0, sp = USER_STACK_TOP;
    if (!r) r = vm_alloc(p, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_SIZE, VM_W);
    if (!r) {
        char a[512];
        snprintf(a, sizeof a, "%s%s%s", p->name, args && *args ? " " : "", args ? args : "");
        u64 n = strlen(a) + 1;
        sp = (sp - n) & ~15ull;
        argp = sp;
        r = load_copy(p, sp, a, n);
        sp -= 8;                     /* as if a call pushed a return address */
    }
    start_t *st = r ? NULL : kmalloc(sizeof *st);
    thread_t *t = st ? thread_create(p->name, user_main, st) : NULL;
    if (!t) {
        kprintf("proc: cannot start %s: %s\n", path, r ? why : "out of memory");
        kfree(st);
        vm_destroy(p);
        kfree(p);
        return r ? r : E_NOMEM;
    }
    st->p = p; st->sp = sp; st->argp = argp;
    t->proc = p;
    t->utid = 1;
    p->main = t;
    p->nthreads = 1;
    p->uthreads[0].t = t;
    p->uthreads[0].tid = 1;
    p->next_tid = 2;
    if (con && con->attach) con->attach(con_ctx, p->pid);
    p->state = P_RUNNING;
    p->next = procs;
    procs = p;
    kprintf("proc: %u %s started (%lu KiB mapped, entry %lx)\n", p->pid, p->name, p->pages * 4, p->entry);
    return (int)p->pid;
}

void proc_detach_console(void *ctx) {
    for (proc_t *p = procs; p; p = p->next) if (p->con_ctx == ctx) { p->con = NULL; p->con_ctx = NULL; }
}

void snd_release_pid(u32 pid) __attribute__((weak));   /* sound/: the program's streams */

/* the last thread of p is ending: give back everything it owned */
static void proc_finish(proc_t *p) {
    if (proc_release_hook) proc_release_hook(p);
    if (socket_release_all) socket_release_all(p);
    if (snd_release_pid) snd_release_pid(p->pid);
    shm_release_all(p);
    for (int i = 0; i < PROC_HANDLES; i++) {
        if (p->handles[i].type == H_FILE) vfs_close(p->handles[i].obj);
        else if (p->handles[i].type == H_DIR) kfree(p->handles[i].obj);
        p->handles[i].type = H_NONE;
    }
    cr3_load(mm_kernel_pml4());
    u64 pages = p->pages;
    vm_destroy(p);
    p->state = P_ZOMBIE;
    kprintf("proc: %u %s ended (code %d, %lu KiB freed)%s%s\n", p->pid, p->name, p->exit_code, pages * 4,
            p->fault[0] ? ": " : "", p->fault);
    if (p->con && p->con->exited) p->con->exited(p->con_ctx, p->pid, p->exit_code);
    p->con = NULL;
    if (p->crashed && proc_crash_hook) proc_crash_hook(p);
    proc_reap();
}

static uthread_t *uthread_of(proc_t *p, thread_t *t, int *slot) {
    for (int i = 0; i < MAX_UTHREADS; i++)
        if (p->uthreads[i].tid && p->uthreads[i].t == t) { if (slot) *slot = i; return &p->uthreads[i]; }
    return NULL;
}

/* the current thread of p ends; the last one ends the process */
static NORETURN void end_thread(proc_t *p, int code) {
    thread_t *t = thread_current();
    int slot = -1;
    uthread_t *u = uthread_of(p, t, &slot);
    if (u) {
        if (slot > 0) vm_free_range(p, USER_STACK_TOP - (u64)slot * THREAD_SLOT - UTHREAD_STACK, UTHREAD_STACK);
        u->t = NULL;
        u->done = 1;
        u->code = code;
        if (u->detached) u->tid = 0;
    }
    p->cpu_tsc += t->cpu_tsc + (rdtsc() - t->run_start);
    if (p->main == t) p->main = NULL;   /* the thread is freed after it ends */
    if (--p->nthreads == 0) proc_finish(p);
    t->proc = NULL;                     /* this thread now only finishes in the kernel */
    thread_exit();
}

/* the whole process ends (SYS_EXIT, a fault, being ended): this thread now,
 * the others at their next return to user mode or blocking wait */
NORETURN void proc_exit(int code) {
    proc_t *p = proc_current();
    if (!p) panic("proc_exit outside a process");
    if (!p->killed) p->exit_code = code;
    p->killed = 1;
    end_thread(p, code);
}

/* ---------------- threads ---------------- */
typedef struct ustart { proc_t *p; u64 rip, sp, arg; } ustart_t;

static void uthread_main(void *a) {
    ustart_t st = *(ustart_t *)a;
    kfree(a);
    user_enter(st.rip, st.sp, st.arg);
}

int uthread_create(proc_t *p, u64 rip, u64 arg) {
    if (p->killed) return E_CANCELED;
    if (rip < USER_BASE || rip >= USER_END) return E_INVAL;
    int slot = -1;
    for (int i = 1; i < MAX_UTHREADS && slot < 0; i++) if (!p->uthreads[i].tid) slot = i;
    if (slot < 0) return E_BUSY;
    u64 top = USER_STACK_TOP - (u64)slot * THREAD_SLOT;
    int r = vm_alloc(p, top - UTHREAD_STACK, UTHREAD_STACK, VM_W);
    if (r) { vm_free_range(p, top - UTHREAD_STACK, UTHREAD_STACK); return r; }
    ustart_t *st = kmalloc(sizeof *st);
    thread_t *t = st ? thread_create(p->name, uthread_main, st) : NULL;
    if (!t) { kfree(st); vm_free_range(p, top - UTHREAD_STACK, UTHREAD_STACK); return E_NOMEM; }
    st->p = p; st->rip = rip; st->sp = top - 8; st->arg = arg;
    uthread_t *u = &p->uthreads[slot];
    memset(u, 0, sizeof *u);
    u->t = t;
    u->tid = p->next_tid++;
    t->proc = p;                        /* before it first runs: it starts in p's address space */
    t->utid = u->tid;
    t->fs_base = 0;
    p->nthreads++;
    return (int)u->tid;
}

NORETURN void uthread_exit(int code) {
    proc_t *p = proc_current();
    if (!p) panic("uthread_exit outside a process");
    end_thread(p, code);
}

static uthread_t *uthread_by_tid(proc_t *p, u32 tid) {
    for (int i = 0; i < MAX_UTHREADS; i++) if (tid && p->uthreads[i].tid == tid) return &p->uthreads[i];
    return NULL;
}

static int uthread_done(void *a) { return ((uthread_t *)a)->done; }

int uthread_join(proc_t *p, u32 tid) {
    uthread_t *u = uthread_by_tid(p, tid);
    if (!u || u->detached || u->t == thread_current()) return E_INVAL;
    int r = proc_wait(uthread_done, u, ~0ull);
    if (r < 0) return r;
    int code = u->code;
    u->tid = 0;                          /* the slot is free again */
    return code;
}

int uthread_detach(proc_t *p, u32 tid) {
    uthread_t *u = uthread_by_tid(p, tid);
    if (!u || u->detached) return E_INVAL;
    if (u->done) u->tid = 0;
    else u->detached = 1;
    return 0;
}

void uthread_set_tls(u64 base) {
    thread_t *t = thread_current();
    t->fs_base = base;
    wrmsr(0xC0000100, base);
}

/* futexes: wait while the 32-bit word at addr still holds expected */
static int fwait_woken(void *a) { return ((fwait_t *)a)->woken; }

int futex_wait(proc_t *p, u64 addr, u32 expected, u64 deadline_tsc) {
    u32 v;
    if (addr & 3) return E_INVAL;
    if (copyin(&v, addr, 4)) return E_INVAL;
    if (v != expected) return E_AGAIN;
    fwait_t w = { addr, 0, p->futexq };
    p->futexq = &w;
    int r = proc_wait(fwait_woken, &w, deadline_tsc);
    for (fwait_t **pp = &p->futexq; *pp; pp = &(*pp)->next) if (*pp == &w) { *pp = w.next; break; }
    if (w.woken) return 0;
    return r < 0 ? r : E_TIMEOUT;
}

int futex_wake(proc_t *p, u64 addr, int n) {
    int woke = 0;
    for (fwait_t *w = p->futexq; w && woke < n; w = w->next)
        if (w->addr == addr && !w->woken) { w->woken = 1; woke++; }
    return woke;
}

int proc_is_ancestor(u32 ancestor, u32 pid) {
    for (int depth = 0; depth < MAX_PROCS + 8; depth++) {
        proc_t *q = proc_by_pid(pid);
        if (!q || !q->parent) return 0;
        if (q->parent == ancestor) return 1;
        pid = q->parent;
    }
    return 0;
}

int proc_kill(u32 pid, const char *why) {
    proc_t *p = proc_by_pid(pid);
    if (!p || p->state != P_RUNNING) return E_NOENT;
    if (!p->killed) {
        p->exit_code = -1;
        if (why && !p->fault[0]) strlcpy(p->fault, why, sizeof p->fault);
    }
    p->killed = 1;
    kprintf("proc: %u %s: end requested%s%s\n", pid, p->name, why ? ": " : "", why ? why : "");
    return 0;
}

static const char *const exc_names[] = {
    "a divide error", "a debug trap", "an NMI", "a breakpoint", "an overflow", "a bound range error",
    "an invalid instruction", "a floating-point instruction without FPU", "a double fault", "a coprocessor error",
    "an invalid TSS", "a segment not present", "a stack fault", "a general protection fault", "a page fault",
    "a reserved exception", "a floating-point error", "an alignment check", "a machine check", "a SIMD floating-point error",
};

void proc_user_fault(struct regs *r) {
    proc_t *p = proc_current();
    if (!p) return;
    u64 cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    const char *what = r->vector < ARRAY_SIZE(exc_names) ? exc_names[r->vector] : "an exception";
    if (r->vector == 14)
        snprintf(p->fault, sizeof p->fault, "%s: %s address %#lx%s (at %#lx)", what, (r->error & 2) ? "writing" : "reading",
                 cr2, cr2 < 0x10000 ? " (a null pointer)" : cr2 < USER_BASE || cr2 >= USER_END ? " (kernel memory)" : "",
                 r->rip);
    else
        snprintf(p->fault, sizeof p->fault, "%s at %#lx", what, r->rip);
    p->exit_code = -(int)(1000 + r->vector);
    p->crashed = 1;
    proc_exit(p->exit_code);
}

void proc_slice_check(void) {
    proc_t *p = proc_current();
    if (!p) return;
    for (;;) {
        if (p->killed) proc_exit(p->exit_code);
        if (!g_need_resched && rdtsc() < slice_end) break;
        g_need_resched = 0;
        thread_yield();                /* a new slice when it runs again */
    }
    /* the one-shot timer may have fired (and been used up) while this thread
     * was in the kernel: arm it again for the rest of the slice */
    u64 left = slice_end - rdtsc();
    irq_timeslice_tsc(left);
}

void proc_user_return(struct regs *r) { (void)r; proc_slice_check(); }

/* ---------------- handles ---------------- */
int handle_alloc(proc_t *p, int type, void *obj) {
    for (int i = 3; i < PROC_HANDLES; i++)       /* 0-2: stdin / stdout / stderr */
        if (p->handles[i].type == H_NONE) { p->handles[i].type = type; p->handles[i].obj = obj; return i; }
    return E_NOMEM;
}
void *handle_get(proc_t *p, int h, int type) {
    if (h < 0 || h >= PROC_HANDLES || p->handles[h].type != type) return NULL;
    return p->handles[h].obj;
}
void handle_free(proc_t *p, int h) {
    if (h >= 0 && h < PROC_HANDLES) { p->handles[h].type = H_NONE; p->handles[h].obj = NULL; }
}

void proc_init(void) {
    thread_switch_hook = switch_hook;
    u32 a, b, c, d;
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a >= 0x80000001) {
        cpuid(0x80000001, 0, &a, &b, &c, &d);
        if (d & (1u << 20)) { wrmsr(0xC0000080, rdmsr(0xC0000080) | (1u << 11)); nx_bit = PTE_NX; }   /* EFER.NXE */
    }
}
