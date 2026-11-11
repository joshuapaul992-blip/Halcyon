/* Halcyon OS - named shared memory for programs (SYS_SHM_*).
 *
 * An object is page-aligned kernel memory with a name ("halcyon.demo"); every
 * program that opens the name gets a handle and can map the same pages. The
 * object lives while a handle or a mapping refers to it. */
#include "kernel.h"
#include "lib.h"
#include "mm.h"
#include "proc.h"
#include "sysabi.h"

#define SHM_NAME 32
#define SHM_MAX_SIZE (64ull << 20)         /* one mapping slot */
#define SHM_TOTAL (128ull << 20)           /* all objects together */
#define SHM_RESERVE (64ull << 20)          /* kernel heap they never touch */
#define MAX_MAPS 512

typedef struct shm {
    char name[SHM_NAME];
    u8 *mem;
    u64 size;
    int refs;
    struct shm *next;
} shm_t;

typedef struct smap { u32 pid; u64 va; shm_t *s; } smap_t;

static shm_t *objs;
static u64 total;
static smap_t maps[MAX_MAPS];

static void unref(shm_t *s) {
    if (--s->refs > 0) return;
    for (shm_t **pp = &objs; *pp; pp = &(*pp)->next) if (*pp == s) { *pp = s->next; break; }
    total -= s->size;
    kfree(s->mem);
    kfree(s);
}

static int valid_name(const char *n) {
    if (!n[0]) return 0;
    for (const char *c = n; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '.' || *c == '-' || *c == '_'))
            return 0;
    return 1;
}

long shm_open(proc_t *p, u64 uname, u64 size, u64 flags) {
    char name[SHM_NAME];
    if (copyin_str(name, uname, sizeof name) < 0 || !valid_name(name)) return E_INVAL;
    shm_t *s = NULL;
    for (shm_t *x = objs; x; x = x->next) if (!strcmp(x->name, name)) s = x;
    if (s) {
        if ((flags & SHM_EXCL) && (flags & SHM_CREATE)) return E_EXIST;
        if (size > s->size) return E_INVAL;
    } else {
        if (!(flags & SHM_CREATE)) return E_NOENT;
        size = (size + 4095) & ~4095ull;
        if (!size || size > SHM_MAX_SIZE) return E_INVAL;
        if (total + size > SHM_TOTAL || mm_heap_free() < SHM_RESERVE + size) return E_NOMEM;
        s = kzalloc(sizeof *s);
        u8 *mem = s ? kmalloc_aligned(size, 4096) : NULL;
        if (!mem) { kfree(s); return E_NOMEM; }
        memset(mem, 0, size);
        strlcpy(s->name, name, sizeof s->name);
        s->mem = mem;
        s->size = size;
        s->next = objs;
        objs = s;
        total += size;
    }
    int h = handle_alloc(p, H_SHM, s);
    if (h < 0) {
        if (!s->refs) { s->refs = 1; unref(s); }
        return h;
    }
    s->refs++;
    return h;
}

/* map the object of handle h; *size gets its size */
long shm_map(proc_t *p, int h, u64 usize) {
    shm_t *s = handle_get(p, h, H_SHM);
    if (!s) return E_INVAL;
    int slot = -1;
    for (int i = 0; i < MAX_MAPS && slot < 0; i++) if (!maps[i].s) slot = i;
    if (slot < 0) return E_NOMEM;
    u64 va = vm_map_shared(p, s->mem, s->size, 1);
    if (!va) return E_NOMEM;
    if (usize && copyout(usize, &s->size, 8)) { vm_unmap_shared(p, va); return E_INVAL; }
    maps[slot].pid = p->pid; maps[slot].va = va; maps[slot].s = s;
    s->refs++;
    return (long)va;
}

long shm_unmap(proc_t *p, u64 va) {
    for (int i = 0; i < MAX_MAPS; i++) {
        if (maps[i].s && maps[i].pid == p->pid && maps[i].va == va) {
            vm_unmap_shared(p, va);
            shm_t *s = maps[i].s;
            maps[i].s = NULL;
            unref(s);
            return 0;
        }
    }
    return E_INVAL;
}

/* SYS_CLOSE of an H_SHM handle */
int shm_close(proc_t *p, int h) {
    shm_t *s = handle_get(p, h, H_SHM);
    if (!s) return E_INVAL;
    handle_free(p, h);
    unref(s);
    return 0;
}

/* a process ended: its handles and mappings (the page tables go away with it) */
void shm_release_all(proc_t *p) {
    for (int i = 0; i < MAX_MAPS; i++)
        if (maps[i].s && maps[i].pid == p->pid) { shm_t *s = maps[i].s; maps[i].s = NULL; unref(s); }
    for (int h = 0; h < PROC_HANDLES; h++)
        if (p->handles[h].type == H_SHM) { shm_t *s = p->handles[h].obj; p->handles[h].type = H_NONE; unref(s); }
}
