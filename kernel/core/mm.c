/* Halcyon OS - memory management: identity paging + first-fit heap */
#include "mm.h"
#include "lib.h"
#include "kernel.h"

extern char _kernel_start[], _bss_end[];

/* ---------------- paging ---------------- */
static u64 pml4[512] ALIGNED(4096);
static u64 pdpt0[512] ALIGNED(4096);
static u64 pd_low[4][512] ALIGNED(4096);

/* small pool for page tables created on demand by mm_map_mmio */
#define PT_POOL 64
static u64 pt_pool[PT_POOL][512] ALIGNED(4096);
static int pt_pool_used;

#define PTE_P   0x001
#define PTE_RW  0x002
#define PTE_PWT 0x008
#define PTE_PCD 0x010
#define PTE_PS  0x080
#define PTE_ADDR 0x000FFFFFFFFFF000ull

static u64 *pt_alloc(void) {
    if (pt_pool_used >= PT_POOL) panic("out of page table pool");
    u64 *t = pt_pool[pt_pool_used++];
    memset(t, 0, 4096);
    return t;
}

static void paging_init(void) {
    memset(pml4, 0, sizeof pml4);
    memset(pdpt0, 0, sizeof pdpt0);
    pml4[0] = (u64)(uptr)pdpt0 | PTE_P | PTE_RW;
    for (int i = 0; i < 4; i++) {
        pdpt0[i] = (u64)(uptr)pd_low[i] | PTE_P | PTE_RW;
        for (int j = 0; j < 512; j++) {
            u64 addr = ((u64)i << 30) | ((u64)j << 21);
            pd_low[i][j] = addr | PTE_P | PTE_RW | PTE_PS;
        }
    }
    __asm__ volatile("mov %0, %%cr3" :: "r"((u64)(uptr)pml4) : "memory");
}

u64 *mm_kernel_pml4(void) { return pml4; }

void *mm_map_mmio(u64 phys, u64 size) {
    if (phys + size <= 0x100000000ull) return (void *)(uptr)phys;
    /* identity mappings must stay below the user half (PML4 entries 64..255
     * belong to programs and are replaced on every address-space switch) */
    if (phys + size < phys || phys + size > 0x0000200000000000ull) {
        kprintf("mm: device memory at %llx is outside the mappable range\n", (unsigned long long)phys);
        return NULL;
    }
    u64 start = phys & ~0x1FFFFFull, end = (phys + size + 0x1FFFFF) & ~0x1FFFFFull;
    for (u64 a = start; a < end; a += 0x200000) {
        u64 i4 = (a >> 39) & 511, i3 = (a >> 30) & 511, i2 = (a >> 21) & 511;
        u64 *p3, *p2;
        if (!(pml4[i4] & PTE_P)) pml4[i4] = (u64)(uptr)pt_alloc() | PTE_P | PTE_RW;
        p3 = (u64 *)(uptr)(pml4[i4] & PTE_ADDR);
        if (!(p3[i3] & PTE_P)) p3[i3] = (u64)(uptr)pt_alloc() | PTE_P | PTE_RW;
        p2 = (u64 *)(uptr)(p3[i3] & PTE_ADDR);
        p2[i2] = a | PTE_P | PTE_RW | PTE_PS | PTE_PCD | PTE_PWT;
        __asm__ volatile("invlpg (%0)" :: "r"(a) : "memory");
    }
    return (void *)(uptr)phys;
}

/* Write-combining for a frame buffer: PAT entry 1 (PWT=1, PCD=0) becomes WC,
 * and the 2 MiB pages of [phys, phys+size) select it. Writes to the screen
 * then go out in bursts instead of one uncached access at a time (reads
 * stay slow; the GUI never reads the frame buffer). Returns 1 if applied. */
int mm_set_write_combining(u64 phys, u64 size) {
    u32 a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!(d & (1u << 16))) return 0;                       /* no PAT */
    static int pat_done;
    if (!pat_done) {
        u32 lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x277));
        lo = (lo & ~0xFF00u) | (0x01u << 8);               /* PA1 = WC */
        __asm__ volatile("wbinvd; wrmsr; wbinvd" :: "a"(lo), "d"(hi), "c"(0x277) : "memory");
        pat_done = 1;
    }
    u64 start = phys & ~0x1FFFFFull, end = (phys + size + 0x1FFFFF) & ~0x1FFFFFull;
    int n = 0;
    for (u64 va = start; va < end; va += 0x200000) {
        u64 i4 = (va >> 39) & 511, i3 = (va >> 30) & 511, i2 = (va >> 21) & 511;
        if (!(pml4[i4] & PTE_P)) continue;
        u64 *p3 = (u64 *)(uptr)(pml4[i4] & PTE_ADDR);
        if (!(p3[i3] & PTE_P) || (p3[i3] & PTE_PS)) continue;
        u64 *p2 = (u64 *)(uptr)(p3[i3] & PTE_ADDR);
        if (!(p2[i2] & PTE_P) || !(p2[i2] & PTE_PS)) continue;
        p2[i2] = (p2[i2] & ~(PTE_PCD | (1ull << 12))) | PTE_PWT;
        __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
        n++;
    }
    __asm__ volatile("wbinvd" ::: "memory");
    return n > 0;
}

/* ---------------- heap ---------------- */
#define HEAP_MAGIC 0x48454150u
typedef struct hdr {
    struct hdr *prev, *next;
    size_t size;        /* payload bytes up to the next header */
    u32 free, magic;
} hdr_t;

static hdr_t *heap_first;
static u64 heap_base, heap_size, heap_used, total_ram;

static void heap_init(u64 base, u64 size) {
    heap_base = base; heap_size = size;
    heap_first = (hdr_t *)(uptr)base;
    heap_first->prev = heap_first->next = NULL;
    heap_first->size = size - sizeof(hdr_t);
    heap_first->free = 1;
    heap_first->magic = HEAP_MAGIC;
}

void *kmalloc_aligned(size_t sz, size_t align) {
    if (!sz) sz = 1;
    sz = ALIGN_UP(sz, 16);
    if (align < 16) align = 16;
    for (hdr_t *h = heap_first; h; h = h->next) {
        if (!h->free || h->size < sz) continue;
        uptr pay = (uptr)(h + 1);
        uptr ap = ALIGN_UP(pay, align);
        while (ap != pay && ap - pay < sizeof(hdr_t) + 16) ap += align;
        uptr end = pay + h->size;
        if (ap + sz > end) continue;
        if (ap != pay) {
            hdr_t *n = (hdr_t *)(ap - sizeof(hdr_t));
            n->prev = h; n->next = h->next;
            if (h->next) h->next->prev = n;
            h->next = n;
            n->size = end - ap;
            h->size = (uptr)n - pay;
            n->free = 1; n->magic = HEAP_MAGIC;
            h = n;
        }
        if (h->size >= sz + sizeof(hdr_t) + 64) {
            hdr_t *t = (hdr_t *)(ap + sz);
            t->prev = h; t->next = h->next;
            if (h->next) h->next->prev = t;
            h->next = t;
            t->size = h->size - sz - sizeof(hdr_t);
            t->free = 1; t->magic = HEAP_MAGIC;
            h->size = sz;
        }
        h->free = 0;
        heap_used += h->size;
        return (void *)ap;
    }
    kprintf("kmalloc: out of memory (%lu bytes, align %lu)\n", (u64)sz, (u64)align);
    return NULL;
}

void *kmalloc(size_t size) { return kmalloc_aligned(size, 16); }
void *kzalloc(size_t size) {
    void *p = kmalloc(size);
    if (p) memset(p, 0, size);
    return p;
}
void *dma_alloc(size_t size, size_t align) {
    void *p = kmalloc_aligned(size, align);
    if (p) memset(p, 0, size);
    return p;
}

void kfree(void *p) {
    if (!p) return;
    hdr_t *h = (hdr_t *)p - 1;
    if (h->magic != HEAP_MAGIC || h->free) {
        kprintf("kfree: bad pointer %p\n", p);
        return;
    }
    h->free = 1;
    heap_used -= h->size;
    hdr_t *n = h->next;
    if (n && n->free) {
        h->size += sizeof(hdr_t) + n->size;
        h->next = n->next;
        if (n->next) n->next->prev = h;
        n->magic = 0;
    }
    hdr_t *pv = h->prev;
    if (pv && pv->free) {
        pv->size += sizeof(hdr_t) + h->size;
        pv->next = h->next;
        if (h->next) h->next->prev = pv;
        h->magic = 0;
    }
}

void *krealloc(void *p, size_t size) {
    if (!p) return kmalloc(size);
    hdr_t *h = (hdr_t *)p - 1;
    if (h->size >= size) return p;
    void *n = kmalloc(size);
    if (!n) return NULL;
    memcpy(n, p, h->size);
    kfree(p);
    return n;
}

u64 mm_heap_free(void) { return heap_size - heap_used; }

void mm_get_stats(struct mm_stats *st) {
    st->total_ram = total_ram;
    st->heap_size = heap_size;
    st->heap_used = heap_used;
    u64 n = 0;
    for (hdr_t *h = heap_first; h; h = h->next) n++;
    st->heap_blocks = n;
}

void mm_init(void) {
    paging_init();
    mm_map_mmio(g_bootinfo.fb_addr, (u64)g_bootinfo.fb_pitch * g_bootinfo.fb_height);

    u64 kend = ALIGN_UP((u64)(uptr)_bss_end, 0x10000);
    u64 best_base = 0, best_len = 0;
    for (u32 i = 0; i < g_bootinfo.mmap_count; i++) {
        struct bootinfo_mmap *m = &g_bootinfo.mmap[i];
        if (m->type != MMAP_USABLE) continue;
        total_ram += m->length;
        u64 b = m->base, e = m->base + m->length;
        if (b < kend) b = kend;
        if (e > 0xFFFFFFFFull) e = 0x100000000ull;
        if (e <= b) continue;
        b = ALIGN_UP(b, 4096);
        e = ALIGN_DOWN(e, 4096);
        if (e > b && e - b > best_len) { best_base = b; best_len = e - b; }
    }
    if (best_len < 16 * 1024 * 1024) panic("not enough memory (need 16 MiB contiguous above the kernel)");
    heap_init(best_base, best_len);
    kprintf("mm: kernel %p-%p, heap at %p, %lu MiB (total RAM %lu MiB)\n",
            _kernel_start, _bss_end, (void *)(uptr)best_base, best_len >> 20, total_ram >> 20);
}
