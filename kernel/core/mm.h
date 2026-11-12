#ifndef MM_H
#define MM_H
#include "types.h"

/* All kernel memory is identity mapped (virtual == physical) and the heap
 * lives entirely below 4 GiB, so any heap pointer can be handed to a DMA
 * engine (including 32-bit-only controllers such as UHCI/OHCI). */

void mm_init(void);

void *kmalloc(size_t size);
void *kzalloc(size_t size);
void *kmalloc_aligned(size_t size, size_t align);
void *krealloc(void *p, size_t size);
void kfree(void *p);

/* DMA helpers: zeroed, aligned, physically contiguous, below 4 GiB.
 * Free with dma_free (== kfree). */
void *dma_alloc(size_t size, size_t align);
static inline void dma_free(void *p) { kfree(p); }
static inline u64 virt_to_phys(const void *p) { return (u64)(uptr)p; }
static inline void *phys_to_virt(u64 p) { return (void *)(uptr)p; }

/* Make sure a physical MMIO range is identity mapped (uncached). Ranges
 * below 4 GiB are mapped at boot; this is needed for 64-bit BARs. Returns
 * the virtual address (== phys). */
void *mm_map_mmio(u64 phys, u64 size);

struct mm_stats {
    u64 total_ram;        /* usable RAM reported by firmware */
    u64 heap_size;
    u64 heap_used;
    u64 heap_blocks;
};
void mm_get_stats(struct mm_stats *st);
u64  mm_heap_free(void);          /* bytes not allocated (may be fragmented) */
int  mm_set_write_combining(u64 phys, u64 size);   /* frame buffers; 1 if applied */
/* Register BARs below 4 GiB: their 2 MiB pages become uncached in the page
 * tables (PCD | PWT), whatever the MTRRs say. Only whole 2 MiB pages of an
 * aligned range are changed; 1 if applied. */
int  mm_set_uncached(u64 phys, u64 size);
/* the page attribute (PAT) memory type of the page mapping phys: 0 UC,
 * 1 WC, 4 WT, 5 WP, 6 WB, 7 UC-; -1 not mapped (the MTRRs may still make
 * a WB page uncached) */
int  mm_cache_type(u64 phys);
const char *mm_cache_type_str(int type);           /* "write-combining" */

#endif
