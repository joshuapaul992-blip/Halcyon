/* Halcyon OS - exFAT file system driver
 *
 * Read and write support, written from the Microsoft exFAT specification.
 *
 * Design notes (the architecture follows fat.c)
 *  - Sector numbers inside the driver are file system sectors (64 bit). They
 *    are converted to device LBAs with x->dshift, so a 4096-byte-sector
 *    volume works on 512- or 4096-byte devices.
 *  - FAT, allocation bitmap and directory sectors go through a small
 *    write-back LRU cache. Dirty sectors are flushed at the end of every
 *    mutating operation (FAT first, then the bitmap, then directories), so
 *    the volume is consistent between operations.
 *  - File data bypasses the cache: runs of contiguous clusters are read or
 *    written with one request straight to/from the caller's buffer.
 *  - Open files are a shared node per directory entry set (keyed by the
 *    position of its File entry), so several handles see the same size.
 *  - New files are allocated contiguously with NoFatChain set. A NoFatChain
 *    file or directory that has to grow while the next cluster is taken is
 *    converted to a FAT chain. Freeing clusters only clears bitmap bits (the
 *    bitmap is authoritative for free space; FAT entries of free clusters are
 *    undefined).
 *  - ValidDataLength: bytes between ValidDataLength and DataLength read as
 *    zeros. Growing a file with truncate only allocates (VDL stays); a write
 *    past VDL zero-fills the gap on disk first.
 *  - VolumeDirty is set in the main boot sector before the first change
 *    after mount (or after a sync), and cleared again by sync and unmount,
 *    together with PercentInUse. A volume that was already dirty when it was
 *    mounted keeps the flag (it may need a check by another system).
 *  - Names: UTF-16 on disk, compared with the volume's up-case table (the
 *    built-in uc_upper() if the table is missing or corrupt). Names that are
 *    not usable as one VFS path component (path characters, control
 *    characters, unpaired surrogates, "." / "..", trailing dots or spaces,
 *    more than 255 bytes of UTF-8) are shown under a sanitized "display
 *    name", and lookups match display names, so every listed entry can be
 *    opened. exFAT has no 8.3 names: vfs_dirent_t.short_name is always "".
 *  - Timestamps are stored as local time without a UTC offset (OffsetValid
 *    clear, as the RTC gives local time); read back as stored.
 *  - With two FATs (TexFAT layout) the FAT and bitmap selected by ActiveFat
 *    are used; the inactive copies are not touched.
 */
#include "core/types.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/unicode.h"
#include "fs/block.h"
#include "fs/vfs.h"
#include "fs/exfat.h"

#define CACHE_N         32
#define RUN_N           8           /* readahead / merged writeback, sectors */
#define MAX_IO_BYTES    (1024u * 1024u)
#define ZERO_BUF_MAX    (64u * 1024u)
#define DIR_MAX_BYTES   (256u * 1024u * 1024u)
#define MAX_SET         19          /* File entry + up to 18 secondaries */
#define MAX_UNITS       255
#define FREE_UNKNOWN    0xFFFFFFFFu
#define EOC             0xFFFFFFFFu
#define UPCASE_MAX      (128u * 1024u)

/* entry types */
#define ET_EOD      0x00
#define ET_INUSE    0x80
#define ET_SECOND   0x40
#define ET_BENIGN   0x20
#define ET_BITMAP   0x81
#define ET_UPCASE   0x82
#define ET_LABEL    0x83
#define ET_FILE     0x85
#define ET_STREAM   0xC0
#define ET_NAME     0xC1

/* stream extension GeneralSecondaryFlags */
#define SF_ALLOC    0x01
#define SF_NOFAT    0x02

/* volume flags */
#define VF_ACTIVEFAT 0x01
#define VF_DIRTY     0x02
#define VF_MEDIAFAIL 0x04

#define ATTR_RO     0x01
#define ATTR_DIR    0x10
#define ATTR_ARCH   0x20

enum { K_DIR, K_FAT, K_BMP };

typedef struct {
    u64 sec;
    u32 stamp;
    u8  valid, dirty, kind;
    u8 *buf;
} cent_t;

typedef struct xnode {
    struct xnode *next;
    u32 refs;
    u64 sec[MAX_SET];           /* entry set location */
    u16 off[MAX_SET];
    u8  n;
    u8  nofat;
    u8  bad;                    /* allocation impossible: no writes / truncates */
    u16 attr;
    u32 first;
    u32 alloc;                  /* clusters allocated (> nclus: corrupt size) */
    u64 size, vdl;
    u32 hint_idx, hint_clus;    /* chain position cache (hint_clus 0 = none) */
} xnode_t;

typedef struct xfs {
    volume_t   *v;
    blockdev_t *bd;
    u32  ss, ss_shift, dshift;  /* fs sector size, log2, shift to device LBAs */
    u32  csize, spc_shift;
    u64  fat_start;             /* first sector of the active FAT */
    u32  fat_len;
    u64  heap;                  /* first sector of the cluster heap */
    u32  nclus;                 /* usable clusters (limited to the device) */
    u32  root;
    u32  dir_max_clus;
    u32 *bm_clus;               /* clusters of the allocation bitmap */
    u32  bm_n;
    u16 *upcase;                /* decompressed up-case table, NULL = uc_upper */
    u32  upcase_n;
    u32  free_count, next_free;
    u16  mount_flags;           /* VolumeFlags as found at mount */
    u8   ro;
    u8   dirty_on_disk;         /* we set VolumeDirty */
    cent_t cache[CACHE_N];
    u8  *cache_mem;
    u32  stamp;
    u8  *bounce;                /* one sector */
    u8  *ra_buf, *wb_buf;       /* RUN_N sectors each */
    u8  *zero;
    u32  zero_size;
    xnode_t *nodes;
} xfs_t;

/* a directory: the root (no entry, FAT chain without a size) or a
 * subdirectory described by its stream extension */
typedef struct {
    u32 first;
    u64 size;
    u8  nofat, root;
} xdir_t;

/* a File entry set found by a scan */
typedef struct {
    u8  raw[MAX_SET][32];
    u64 sec[MAX_SET];
    u16 off[MAX_SET];
    u8  n;                      /* entries in the set */
    u8  nname;                  /* File Name entries */
    u8  nunits;
    u16 units[MAX_UNITS];
    char name[VFS_MAX_NAME + 1];   /* display name (UTF-8) */
} xent_t;

static const fs_ops_t exfat_ops;

/* ------------------------------------------------------------------ */
/* little helpers                                                       */

static inline u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static inline u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static inline u64 rd64(const u8 *p) { return rd32(p) | ((u64)rd32(p + 4) << 32); }
static inline void wr16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static inline void wr32(u8 *p, u32 v) { wr16(p, (u16)v); wr16(p + 2, (u16)(v >> 16)); }
static inline void wr64(u8 *p, u64 v) { wr32(p, (u32)v); wr32(p + 4, (u32)(v >> 32)); }

static inline int clus_ok(xfs_t *x, u32 c) { return c >= 2 && (u64)c < (u64)x->nclus + 2; }
static inline u64 clus_sec(xfs_t *x, u32 c) { return x->heap + ((u64)(c - 2) << x->spc_shift); }
static inline u64 clusters_for(xfs_t *x, u64 bytes) { return (bytes + x->csize - 1) >> (x->ss_shift + x->spc_shift); }

/* accessors of a File entry set */
static inline u16 e_attr(const xent_t *e) { return rd16(e->raw[0] + 4); }
static inline int e_isdir(const xent_t *e) { return (e_attr(e) & ATTR_DIR) != 0; }
static inline u8  e_nofat(const xent_t *e) { return (e->raw[1][1] & SF_NOFAT) != 0; }
static inline u32 e_first(const xent_t *e) { return rd32(e->raw[1] + 20); }
static inline u64 e_size(const xent_t *e) { return rd64(e->raw[1] + 24); }
static inline u64 e_vdl(const xent_t *e) { return rd64(e->raw[1] + 8); }

static u32 exfat_now(u8 *ms10) {
    struct rtc_time t;
    rtc_read(&t);
    if (t.year < 1980) t.year = 1980;
    if (t.year > 2107) t.year = 2107;
    if (t.month < 1 || t.month > 12) t.month = 1;
    if (t.day < 1 || t.day > 31) t.day = 1;
    if (t.hour < 0 || t.hour > 23) t.hour = 0;
    if (t.minute < 0 || t.minute > 59) t.minute = 0;
    if (t.second < 0 || t.second > 59) t.second = 0;
    if (ms10) *ms10 = (u8)((t.second & 1) * 100);
    return ((u32)(t.year - 1980) << 25) | ((u32)t.month << 21) | ((u32)t.day << 16) |
           ((u32)t.hour << 11) | ((u32)t.minute << 5) | (u32)(t.second / 2);
}

static void ts_to_vtime(u32 ts, u8 ms10, vfs_time_t *o) {
    o->year = (u16)(1980 + (ts >> 25));
    o->month = (ts >> 21) & 15;
    o->day = (ts >> 16) & 31;
    o->hour = (ts >> 11) & 31;
    o->minute = (ts >> 5) & 63;
    u32 s = (ts & 31) * 2 + (ms10 < 200 ? ms10 / 100 : 0);
    o->second = (u8)(s > 59 ? 59 : s);
}

static u32 popcount8(u8 b) {
    b = (u8)(b - ((b >> 1) & 0x55));
    b = (u8)((b & 0x33) + ((b >> 2) & 0x33));
    return (b + (b >> 4)) & 0x0F;
}

/* ------------------------------------------------------------------ */
/* device I/O                                                           */

static int dev_read(xfs_t *x, u64 sec, u32 n, void *buf) {
    return blk_read(x->bd, sec << x->dshift, n << x->dshift, buf);
}
static int dev_write(xfs_t *x, u64 sec, u32 n, const void *buf) {
    return blk_write(x->bd, sec << x->dshift, n << x->dshift, buf);
}

/* ------------------------------------------------------------------ */
/* metadata sector cache                                                */

/* write back the dirty sectors of one kind, merging runs of consecutive
 * sectors into one request */
static int flush_kind(xfs_t *x, int kind) {
    cent_t *list[CACHE_N];
    int n = 0, err = 0;
    for (int i = 0; i < CACHE_N; i++) {
        cent_t *c = &x->cache[i];
        if (c->valid && c->dirty && c->kind == kind) {
            int k = n++;
            while (k > 0 && list[k - 1]->sec > c->sec) { list[k] = list[k - 1]; k--; }
            list[k] = c;
        }
    }
    for (int i = 0; i < n;) {
        int j = i + 1;
        while (j < n && j - i < RUN_N && list[j]->sec == list[j - 1]->sec + 1) j++;
        const u8 *buf = list[i]->buf;
        if (j - i > 1) {
            for (int k = i; k < j; k++) memcpy(x->wb_buf + (size_t)(k - i) * x->ss, list[k]->buf, x->ss);
            buf = x->wb_buf;
        }
        int r = dev_write(x, list[i]->sec, (u32)(j - i), buf);
        if (r) { if (!err) err = r; }
        else for (int k = i; k < j; k++) list[k]->dirty = 0;
        i = j;
    }
    return err;
}

/* every dirty sector: FAT first, then the bitmap, then directories */
static int flush_dirty(xfs_t *x) {
    int r1 = flush_kind(x, K_FAT);
    int r2 = flush_kind(x, K_BMP);
    int r3 = flush_kind(x, K_DIR);
    return r1 ? r1 : r2 ? r2 : r3;
}

static cent_t *cache_find(xfs_t *x, u64 sec) {
    for (int i = 0; i < CACHE_N; i++)
        if (x->cache[i].valid && x->cache[i].sec == sec) return &x->cache[i];
    return NULL;
}

static cent_t *cache_victim(xfs_t *x, int *err) {
    cent_t *victim = NULL;
    for (int i = 0; i < CACHE_N; i++) {
        cent_t *c = &x->cache[i];
        if (!c->valid) { victim = c; break; }
        if (!victim || c->stamp < victim->stamp) victim = c;
    }
    if (victim->valid && victim->dirty) {
        int r = flush_dirty(x);
        if (r) { *err = r; return NULL; }
    }
    victim->valid = 0;
    return victim;
}

/* Get a cached sector. ra: sectors from sec on that are contiguous metadata
 * of the same kind (readahead limit, 1 = none). With noread the buffer is
 * zero-filled instead of read. */
static cent_t *cget(xfs_t *x, u64 sec, int kind, u32 ra, int noread, int *err) {
    cent_t *c = cache_find(x, sec);
    if (c) {
        c->stamp = ++x->stamp;
        return c;
    }
    u32 n = MIN((u32)RUN_N, ra);
    if (!noread && n > 1) {
        u32 cached = 0;
        for (u32 i = 0; i < n; i++) if (cache_find(x, sec + i)) cached |= 1u << i;
        if (dev_read(x, sec, n, x->ra_buf) == 0) {
            for (u32 i = n; i-- > 0;) {
                if ((cached & (1u << i)) || cache_find(x, sec + i)) continue;
                cent_t *v = cache_victim(x, err);
                if (!v) { if (i == 0) return NULL; continue; }
                memcpy(v->buf, x->ra_buf + (size_t)i * x->ss, x->ss);
                v->sec = sec + i;
                v->valid = 1;
                v->dirty = 0;
                v->kind = (u8)kind;
                v->stamp = ++x->stamp;
            }
            c = cache_find(x, sec);
            if (c) return c;
        }
    }
    c = cache_victim(x, err);
    if (!c) return NULL;
    if (noread) {
        memset(c->buf, 0, x->ss);
    } else {
        int r = dev_read(x, sec, 1, c->buf);
        if (r) { *err = r; return NULL; }
    }
    c->sec = sec;
    c->valid = 1;
    c->dirty = 0;
    c->kind = (u8)kind;
    c->stamp = ++x->stamp;
    return c;
}

static void cache_inval(xfs_t *x, u64 sec, u64 n) {
    for (int i = 0; i < CACHE_N; i++) {
        cent_t *c = &x->cache[i];
        if (c->valid && c->sec >= sec && c->sec - sec < n) c->valid = c->dirty = 0;
    }
}

/* sectors left in the cluster holding sector sec of the heap */
static inline u32 sec_left_in_clus(xfs_t *x, u64 sec) {
    u32 spc = 1u << x->spc_shift;
    return spc - (u32)((sec - x->heap) & (spc - 1));
}

/* ------------------------------------------------------------------ */
/* boot sector flags                                                    */

/* rewrite VolumeFlags / PercentInUse of the main boot sector (both are
 * excluded from the boot checksum) */
static int boot_flags_store(xfs_t *x, u16 flags, int pct_known, u8 pct) {
    cache_inval(x, 0, 1);
    int r = dev_read(x, 0, 1, x->bounce);
    if (r) return r;
    if (memcmp(x->bounce + 3, "EXFAT   ", 8)) return E_IO;
    wr16(x->bounce + 106, flags);
    if (pct_known) x->bounce[112] = pct;
    return dev_write(x, 0, 1, x->bounce);
}

/* set VolumeDirty before the first metadata change */
static int mark_dirty(xfs_t *x) {
    if (x->ro) return E_ROFS;
    if (x->dirty_on_disk || (x->mount_flags & VF_DIRTY)) return 0;
    int r = boot_flags_store(x, (u16)(x->mount_flags | VF_DIRTY), 0, 0);
    if (!r) x->dirty_on_disk = 1;
    return r;
}

static int exfat_flush(xfs_t *x) { return flush_dirty(x); }

/* flush and clear VolumeDirty (unless it was set when we mounted) */
static int flush_clean(xfs_t *x) {
    int r = flush_dirty(x);
    if (r || !x->dirty_on_disk) return r;
    u8 pct = 0xFF;
    if (x->free_count != FREE_UNKNOWN && x->nclus)
        pct = (u8)(((u64)(x->nclus - x->free_count) * 100) / x->nclus);
    r = boot_flags_store(x, x->mount_flags, 1, pct);
    if (!r) x->dirty_on_disk = 0;
    return r;
}

/* ------------------------------------------------------------------ */
/* FAT                                                                  */

static int fat_ptr(xfs_t *x, u32 clus, u8 **p, cent_t **ce) {
    u64 off = (u64)clus * 4;
    u64 sec = x->fat_start + (off >> x->ss_shift);
    int err = 0;
    cent_t *c = cget(x, sec, K_FAT, (u32)MIN((u64)RUN_N, x->fat_start + x->fat_len - sec), 0, &err);
    if (!c) return err ? err : E_IO;
    *p = c->buf + (off & (x->ss - 1));
    *ce = c;
    return 0;
}

static int fat_get(xfs_t *x, u32 clus, u32 *val) {
    if (!clus_ok(x, clus)) return E_IO;
    u8 *p; cent_t *c;
    int r = fat_ptr(x, clus, &p, &c);
    if (r) return r;
    *val = rd32(p);
    return 0;
}

static int fat_set(xfs_t *x, u32 clus, u32 val) {
    if (!clus_ok(x, clus)) return E_IO;
    u8 *p; cent_t *c;
    int r = fat_ptr(x, clus, &p, &c);
    if (r) return r;
    wr32(p, val);
    c->dirty = 1;
    return 0;
}

/* next cluster in a chain: 0 = ok (*next valid), 1 = end of chain */
static int chain_next(xfs_t *x, u32 c, u32 *next) {
    u32 v;
    int r = fat_get(x, c, &v);
    if (r) return r;
    if (v == EOC) return 1;
    if (!clus_ok(x, v)) {
        kprintf("exfat: %s: corrupt chain at cluster %u (-> %x)\n", x->bd->name, c, v);
        return E_IO;
    }
    *next = v;
    return 0;
}

/* write a FAT chain for count contiguous clusters (NoFatChain conversion) */
static int fat_link_run(xfs_t *x, u32 first, u32 count) {
    for (u32 i = 0; i < count; i++) {
        int r = fat_set(x, first + i, i + 1 < count ? first + i + 1 : EOC);
        if (r) return r;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* allocation bitmap                                                    */

/* the cached sector holding bit (cluster - 2); *byte = offset in the sector */
static int bm_ptr(xfs_t *x, u32 bit, cent_t **ce, u32 *byte) {
    u32 boff = bit >> 3;
    u32 ci = boff >> (x->ss_shift + x->spc_shift);
    if (ci >= x->bm_n) return E_IO;
    u32 in = boff & (x->csize - 1);
    u64 sec = clus_sec(x, x->bm_clus[ci]) + (in >> x->ss_shift);
    int err = 0;
    cent_t *c = cget(x, sec, K_BMP, sec_left_in_clus(x, sec), 0, &err);
    if (!c) return err ? err : E_IO;
    *ce = c;
    *byte = in & (x->ss - 1);
    return 0;
}

static int bm_get(xfs_t *x, u32 c, int *used) {
    if (!clus_ok(x, c)) return E_IO;
    cent_t *ce; u32 b;
    int r = bm_ptr(x, c - 2, &ce, &b);
    if (r) return r;
    *used = (ce->buf[b] >> ((c - 2) & 7)) & 1;
    return 0;
}

/* set or clear the bit of cluster c; *was gets the old value */
static int bm_put(xfs_t *x, u32 c, int used, int *was) {
    if (!clus_ok(x, c)) return E_IO;
    cent_t *ce; u32 b;
    int r = bm_ptr(x, c - 2, &ce, &b);
    if (r) return r;
    u8 m = (u8)(1u << ((c - 2) & 7));
    int old = (ce->buf[b] & m) != 0;
    if (was) *was = old;
    if (old == used) return 0;
    if (used) ce->buf[b] |= m; else ce->buf[b] &= (u8)~m;
    ce->dirty = 1;
    if (x->free_count != FREE_UNKNOWN) {
        if (used && x->free_count) x->free_count--;
        else if (!used) x->free_count++;
    }
    return 0;
}

/* allocate one free cluster, searching from near */
static int clus_alloc(xfs_t *x, u32 near, u32 *out) {
    u32 n = x->nclus;
    u32 c = clus_ok(x, near) ? near : clus_ok(x, x->next_free) ? x->next_free : 2;
    u32 left = n;
    while (left) {
        cent_t *ce; u32 b;
        int r = bm_ptr(x, c - 2, &ce, &b);
        if (r) return r;
        u32 bit0 = (c - 2) & 7;
        u32 lim = (x->ss - b) * 8 - bit0;                       /* bits in this sector */
        lim = MIN(lim, n + 2 - c);
        lim = MIN(lim, left);
        const u8 *p = ce->buf + b;
        for (u32 i = 0; i < lim;) {
            u32 k = bit0 + i;
            u8 byte = p[k >> 3];
            if (!(k & 7) && byte == 0xFF && lim - i >= 8) { i += 8; continue; }
            if (!(byte & (1u << (k & 7)))) {
                u32 found = c + i;
                if ((r = bm_put(x, found, 1, NULL))) return r;
                x->next_free = found + 1 < n + 2 ? found + 1 : 2;
                *out = found;
                return 0;
            }
            i++;
        }
        c += lim;
        left -= lim;
        if (c >= n + 2) c = 2;
    }
    x->free_count = 0;
    return E_NOSPC;
}

/* take cluster c if it is free: 1 = taken, 0 = in use */
static int clus_take(xfs_t *x, u32 c) {
    if (!clus_ok(x, c)) return 0;
    int used, r = bm_get(x, c, &used);
    if (r) return r;
    if (used) return 0;
    if ((r = bm_put(x, c, 1, NULL))) return r;
    return 1;
}

/* can an allocation of size bytes at first exist on this volume? (a
 * corrupt one must not be freed: the clusters may belong to others) */
static int alloc_sane(xfs_t *x, u32 first, u64 size, int nofat) {
    if (!size) return 1;
    u64 n = clusters_for(x, size);
    if (n > x->nclus || !clus_ok(x, first)) return 0;
    if (nofat && (u64)first + n > (u64)x->nclus + 2) return 0;
    return 1;
}

/* release an allocation of count clusters (bitmap only) */
static int alloc_free(xfs_t *x, u32 first, u64 count, int nofat) {
    if (!clus_ok(x, first) || !count) return 0;
    if (count > x->nclus) count = x->nclus;
    if (nofat) {
        for (u64 i = 0; i < count && clus_ok(x, (u32)(first + i)); i++) {
            int r = bm_put(x, (u32)(first + i), 0, NULL);
            if (r) return r;
        }
        return 0;
    }
    u32 c = first;
    for (u64 i = 0; i < count; i++) {
        int was, r = bm_put(x, c, 0, &was);
        if (r) return r;
        if (!was) break;                 /* already free: corrupt, stop */
        if (i + 1 == count) break;
        r = chain_next(x, c, &c);
        if (r) return r < 0 ? r : 0;
    }
    return 0;
}

static int zero_buf(xfs_t *x) {
    if (x->zero) return 0;
    u32 sz = MIN(x->csize, ZERO_BUF_MAX);
    if (sz < x->ss) sz = x->ss;
    x->zero = kzalloc(sz);
    if (!x->zero) return E_NOMEM;
    x->zero_size = sz;
    return 0;
}

static int clus_zero(xfs_t *x, u32 c) {
    int r = zero_buf(x);
    if (r) return r;
    u64 sec = clus_sec(x, c);
    u32 spc = 1u << x->spc_shift, per = x->zero_size >> x->ss_shift;
    for (u32 done = 0; done < spc; done += per) {
        u32 n = MIN(per, spc - done);
        if ((r = dev_write(x, sec + done, n, x->zero))) return r;
    }
    cache_inval(x, sec, spc);
    return 0;
}

static int count_free(xfs_t *x) {
    u32 chunk = MIN(x->csize, 64u * 1024);
    u8 *buf = kmalloc(chunk);
    if (!buf) return E_NOMEM;
    int r = flush_dirty(x);
    u64 bits = x->nclus, done = 0;
    u32 used = 0;
    for (u32 ci = 0; !r && ci < x->bm_n && done < bits; ci++) {
        for (u32 off = 0; off < x->csize && done < bits; off += chunk) {
            u64 sec = clus_sec(x, x->bm_clus[ci]) + (off >> x->ss_shift);
            if ((r = dev_read(x, sec, chunk >> x->ss_shift, buf))) break;
            for (u32 i = 0; i < chunk && done < bits; i++) {
                u8 b = buf[i];
                if (bits - done < 8) b &= (u8)((1u << (bits - done)) - 1);
                used += popcount8(b);
                done += MIN((u64)8, bits - done);
            }
        }
    }
    kfree(buf);
    if (r) return r;
    x->free_count = used > x->nclus ? 0 : x->nclus - used;
    return 0;
}

/* ------------------------------------------------------------------ */
/* names                                                                */

static inline u16 up(xfs_t *x, u16 c) {
    if (x->upcase) return c < x->upcase_n ? x->upcase[c] : c;
    return (u16)uc_upper(c);
}

static u32 up_cp(xfs_t *x, u32 cp) { return cp < 0x10000 ? up(x, (u16)cp) : cp; }

static u16 name_hash(xfs_t *x, const u16 *u, int n) {
    u16 h = 0;
    for (int i = 0; i < n; i++) {
        u16 c = up(x, u[i]);
        h = (u16)(((h & 1) ? 0x8000 : 0) + (h >> 1) + (c & 0xFF));
        h = (u16)(((h & 1) ? 0x8000 : 0) + (h >> 1) + (c >> 8));
    }
    return h;
}

/* case-insensitive (up-case table) comparison of two UTF-8 names */
static int name_eq(xfs_t *x, const char *a, const char *b) {
    for (;;) {
        u32 ca = utf8_next(&a), cb = utf8_next(&b);
        if (up_cp(x, ca) != up_cp(x, cb)) return 0;
        if (!ca) return 1;
    }
}

static int set_checksum_ok(const u8 (*raw)[32], int n, u16 *out) {
    u16 c = 0;
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 32; k++) {
            if (i == 0 && (k == 2 || k == 3)) continue;
            c = (u16)(((c & 1) ? 0x8000 : 0) + (c >> 1) + raw[i][k]);
        }
    if (out) *out = c;
    return c == rd16(raw[0] + 2);
}

static void set_checksum_store(u8 (*raw)[32], int n) {
    u16 c;
    set_checksum_ok((const u8 (*)[32])raw, n, &c);
    wr16(raw[0] + 2, c);
}

/* the name handed to the VFS for the on-disk name u[0..n): UTF-8, one
 * usable path component, at most VFS_MAX_NAME bytes */
static void display_name(xfs_t *x, const u16 *u, int n, char *out) {
    int o = 0, cut = 0;
    for (int i = 0; i < n; i++) {
        u32 cp = u[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            i++;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = UC_REPLACEMENT;
        }
        if (cp < 0x20 || cp == '/' || cp == '\\' || cp == ':') cp = '_';
        char tmp[4];
        int l = utf8_put(cp, tmp);
        if (o + l > VFS_MAX_NAME - 6) {
            /* may not fit: decide once we know the total length */
            int total = o;
            for (int k = i; k < n; k++) total += u[k] < 0x80 ? 1 : u[k] < 0x800 ? 2 : 3;
            if (total > VFS_MAX_NAME) { cut = 1; break; }
        }
        memcpy(out + o, tmp, l);
        o += l;
    }
    if (cut) {
        /* too long for the VFS: keep it unique with the name hash */
        o += snprintf(out + o, VFS_MAX_NAME + 1 - o, "~%04X", name_hash(x, u, n));
    }
    if (!o || out[o - 1] == '.' || out[o - 1] == ' ') {
        /* "", ".", "..", trailing dots or spaces (stripped by lookups) */
        if (o >= VFS_MAX_NAME) o--;
        out[o++] = '_';
    }
    out[o] = 0;
}

/* copy one path component, stripping trailing spaces and dots */
static int get_comp(const char *s, size_t len, char *out) {
    while (len && (s[len - 1] == ' ' || s[len - 1] == '.')) len--;
    if (!len) return E_INVAL;
    if (len > VFS_MAX_NAME) return E_NAMETOOLONG;
    memcpy(out, s, len);
    out[len] = 0;
    return 0;
}

/* a name for a new entry: valid UTF-8, no characters exFAT forbids, at
 * most 255 UTF-16 units; the units go to u */
static int name_units(const char *n, u16 *u, int *nu) {
    size_t len = strlen(n);
    if (!len) return E_INVAL;
    if (len > VFS_MAX_NAME) return E_NAMETOOLONG;
    for (size_t i = 0; i < len; i++) {
        u8 c = (u8)n[i];
        if (c < 0x20 || (c < 0x80 && strchr("\"*/:<>?\\|", c))) return E_INVAL;
    }
    if (!utf8_valid(n)) return E_INVAL;
    int k = utf8_to_utf16(n, u, MAX_UNITS);
    if (k < 0) return E_NAMETOOLONG;
    if (!k) return E_INVAL;
    *nu = k;
    return 0;
}

/* ------------------------------------------------------------------ */
/* directory iteration                                                  */

typedef struct {
    u32 clus, cidx, ncl;
    u8  nofat, root;
    u32 idx;                    /* entry index in the directory */
    u64 sec;
} diter_t;

static int di_start(xfs_t *x, diter_t *d, const xdir_t *dir) {
    if (!clus_ok(x, dir->first)) return E_IO;
    d->clus = dir->first;
    d->cidx = 0;
    d->idx = 0;
    d->nofat = dir->nofat;
    d->root = dir->root;
    if (dir->root) {
        d->ncl = x->dir_max_clus;
    } else {
        if (!dir->size || dir->size > DIR_MAX_BYTES) return E_IO;
        d->ncl = (u32)clusters_for(x, dir->size);
    }
    d->sec = clus_sec(x, d->clus);
    return 0;
}

static inline u32 di_off(xfs_t *x, diter_t *d) { return (d->idx * 32) & (x->ss - 1); }

/* 0 = advanced, 1 = end of the directory's clusters (d stays on the last
 * entry), < 0 = error */
static int di_next(xfs_t *x, diter_t *d) {
    u32 nidx = d->idx + 1;
    if (((nidx * 32) & (x->csize - 1)) == 0) {
        if (d->cidx + 1 >= d->ncl) return 1;
        u32 nx;
        if (d->nofat) {
            nx = d->clus + 1;
            if (!clus_ok(x, nx)) return E_IO;
        } else {
            int r = chain_next(x, d->clus, &nx);
            if (r < 0) return r;
            if (r == 1) return d->root ? 1 : E_IO;   /* subdirectory chain shorter than its size */
        }
        d->clus = nx;
        d->cidx++;
        d->sec = clus_sec(x, nx);
    } else if (((nidx * 32) & (x->ss - 1)) == 0) {
        d->sec++;
    }
    d->idx = nidx;
    return 0;
}

static inline const u8 *di_entry(xfs_t *x, diter_t *d, int *err) {
    cent_t *c = cget(x, d->sec, K_DIR, sec_left_in_clus(x, d->sec), 0, err);
    return c ? c->buf + di_off(x, d) : NULL;
}

/* check a collected entry set and decode its name: 0 = valid */
static int set_finish(xfs_t *x, xent_t *e) {
    if (!set_checksum_ok((const u8 (*)[32])e->raw, e->n, NULL)) return E_IO;
    const u8 *st = e->raw[1];
    if (st[0] != ET_STREAM) return E_IO;
    int nl = st[3];
    int nn = (nl + 14) / 15;
    if (!nl || 1 + nn > e->n - 1) return E_IO;
    for (int i = 0; i < nn; i++)
        if (e->raw[2 + i][0] != ET_NAME) return E_IO;
    for (int i = 2 + nn; i < e->n; i++)
        if (!(e->raw[i][0] & ET_BENIGN)) return E_IO;   /* unknown critical secondary */
    for (int i = 0; i < nl; i++) e->units[i] = rd16(e->raw[2 + i / 15] + 2 + (i % 15) * 2);
    e->nunits = (u8)nl;
    e->nname = (u8)nn;
    display_name(x, e->units, nl, e->name);
    return 0;
}

/* scan callback: 0 = continue, 1 = stop (entry in *e), < 0 = error */
typedef int (*scan_cb)(xfs_t *x, xent_t *e, void *ctx);

static int dir_scan(xfs_t *x, const xdir_t *dir, scan_cb cb, void *ctx, xent_t *e) {
    diter_t d;
    int want = 0, r = di_start(x, &d, dir);
    if (r) return r;
    e->n = 0;
    for (;;) {
        int err = 0;
        const u8 *p = di_entry(x, &d, &err);
        if (!p) return err ? err : E_IO;
        u8 t = p[0];
        if (t == ET_EOD) break;
        if (want) {
            if ((t & (ET_INUSE | ET_SECOND)) == (ET_INUSE | ET_SECOND)) {
                memcpy(e->raw[e->n], p, 32);
                e->sec[e->n] = d.sec;
                e->off[e->n] = (u16)di_off(x, &d);
                if (++e->n == want) {
                    want = 0;
                    if (set_finish(x, e)) {
                        kprintf("exfat: %s: corrupt entry set at sector %lu, ignored\n", x->bd->name, e->sec[0]);
                    } else {
                        r = cb(x, e, ctx);
                        if (r) return r;
                    }
                }
                goto next;
            }
            want = 0;           /* set cut short: look at this entry afresh */
        }
        if (t == ET_FILE) {
            int sc = p[1];
            if (sc >= 2 && sc <= MAX_SET - 1) {
                memcpy(e->raw[0], p, 32);
                e->sec[0] = d.sec;
                e->off[0] = (u16)di_off(x, &d);
                e->n = 1;
                want = sc + 1;
            }
        }
    next:
        r = di_next(x, &d);
        if (r == 1) break;
        if (r < 0) return r;
    }
    return 0;
}

static int lookup_cb(xfs_t *x, xent_t *e, void *ctx) { return name_eq(x, e->name, ctx); }

/* find name in dir: 0 = found (in *e), E_NOENT, or error */
static int dir_lookup(xfs_t *x, const xdir_t *dir, const char *name, xent_t *e) {
    int r = dir_scan(x, dir, lookup_cb, (void *)name, e);
    if (r == 1) return 0;
    return r < 0 ? r : E_NOENT;
}

static void root_dir(xfs_t *x, xdir_t *d) {
    d->first = x->root;
    d->size = 0;
    d->nofat = 0;
    d->root = 1;
}

static int dir_of(xfs_t *x, const xent_t *e, xdir_t *d) {
    if (!e_isdir(e)) return E_NOTDIR;
    d->first = e_first(e);
    d->size = e_size(e);
    d->nofat = e_nofat(e);
    d->root = 0;
    if (!clus_ok(x, d->first) || !d->size) return E_IO;
    return 0;
}

static inline int same_set(const xent_t *a, u64 sec, u16 off) { return a->sec[0] == sec && a->off[0] == off; }

/* resolve a path; *isroot set for "" (then *e is untouched). If avoid is
 * given, *hit is set when a directory on the way is that entry set. */
static int resolve(xfs_t *x, const char *path, xent_t *e, int *isroot, const xent_t *avoid, int *hit) {
    *isroot = 0;
    if (!*path) { *isroot = 1; return 0; }
    xdir_t dir;
    root_dir(x, &dir);
    char comp[VFS_MAX_NAME + 1];
    const char *s = path;
    for (;;) {
        const char *sl = strchr(s, '/');
        size_t len = sl ? (size_t)(sl - s) : strlen(s);
        int r = get_comp(s, len, comp);
        if (r) return r;
        if ((r = dir_lookup(x, &dir, comp, e))) return r;
        if (avoid && hit && same_set(e, avoid->sec[0], avoid->off[0])) *hit = 1;
        if (!sl || !sl[1]) return 0;
        if ((r = dir_of(x, e, &dir))) return r;
        s = sl + 1;
    }
}

/* resolve the parent directory of path: *pe / *isroot describe it, the last
 * component goes to name */
static int resolve_parent(xfs_t *x, const char *path, xent_t *pe, int *isroot, xdir_t *dir, char *name,
                          const xent_t *avoid, int *hit) {
    if (!*path) return E_INVAL;
    const char *sl = strrchr(path, '/');
    const char *last = sl ? sl + 1 : path;
    int r = get_comp(last, strlen(last), name);
    if (r) return r;
    *isroot = 1;
    if (!sl) { root_dir(x, dir); return 0; }
    size_t plen = (size_t)(sl - path);
    char *parent = kmalloc(plen + 1);
    if (!parent) return E_NOMEM;
    memcpy(parent, path, plen);
    parent[plen] = 0;
    r = resolve(x, parent, pe, isroot, avoid, hit);
    kfree(parent);
    if (r) return r;
    if (*isroot) root_dir(x, dir);
    else r = dir_of(x, pe, dir);
    return r;
}

/* ------------------------------------------------------------------ */
/* entry sets                                                           */

/* read the n entries of a set, check that it still is the set we expect */
static int set_read(xfs_t *x, const u64 *sec, const u16 *off, int n, u8 (*raw)[32]) {
    for (int i = 0; i < n; i++) {
        int err = 0;
        cent_t *c = cget(x, sec[i], K_DIR, 1, 0, &err);
        if (!c) return err ? err : E_IO;
        memcpy(raw[i], c->buf + off[i], 32);
    }
    if (n < 3 || raw[0][0] != ET_FILE || raw[0][1] != n - 1 || raw[1][0] != ET_STREAM) return E_IO;
    return 0;
}

static int set_write(xfs_t *x, const u64 *sec, const u16 *off, int n, u8 (*raw)[32]) {
    for (int i = 0; i < n; i++) {
        int err = 0;
        cent_t *c = cget(x, sec[i], K_DIR, 1, 0, &err);
        if (!c) return err ? err : E_IO;
        memcpy(c->buf + off[i], raw[i], 32);
        c->dirty = 1;
    }
    return 0;
}

/* mark the entries of a set unused */
static int set_delete(xfs_t *x, const u64 *sec, const u16 *off, int n) {
    for (int i = 0; i < n; i++) {
        int err = 0;
        cent_t *c = cget(x, sec[i], K_DIR, 1, 0, &err);
        if (!c) return err ? err : E_IO;
        c->buf[off[i]] &= (u8)~ET_INUSE;
        c->dirty = 1;
    }
    return 0;
}

/* stream extension contents */
static void stream_set(u8 *st, u32 first, u64 size, u64 vdl, int nofat) {
    st[1] = (u8)(SF_ALLOC | (nofat && first ? SF_NOFAT : 0));
    wr64(st + 8, vdl);
    wr32(st + 20, first);
    wr64(st + 24, size);
}

/* Build a File entry set in raw from the File entry f0, the stream
 * extension st0, a name and benign secondaries to keep. Returns the entry
 * count or E_NAMETOOLONG. */
static int set_build(xfs_t *x, u8 (*raw)[32], const u8 *f0, const u8 *st0, const u16 *u, int nu,
                     const u8 (*benign)[32], int nbenign) {
    int nn = (nu + 14) / 15, n = 2 + nn + nbenign;
    if (n > MAX_SET) return E_NAMETOOLONG;
    memcpy(raw[0], f0, 32);
    memcpy(raw[1], st0, 32);
    raw[0][0] = ET_FILE;
    raw[0][1] = (u8)(n - 1);
    raw[1][0] = ET_STREAM;
    raw[1][3] = (u8)nu;
    wr16(raw[1] + 4, name_hash(x, u, nu));
    for (int i = 0; i < nn; i++) {
        u8 *p = raw[2 + i];
        memset(p, 0, 32);
        p[0] = ET_NAME;
        for (int k = 0; k < 15 && i * 15 + k < nu; k++) wr16(p + 2 + 2 * k, u[i * 15 + k]);
    }
    for (int i = 0; i < nbenign; i++) memcpy(raw[2 + nn + i], benign[i], 32);
    set_checksum_store(raw, n);
    return n;
}

static void file_entry_new(u8 *f0, u16 attr) {
    u8 ms;
    u32 ts = exfat_now(&ms);
    memset(f0, 0, 32);
    wr16(f0 + 4, attr);
    wr32(f0 + 8, ts);
    wr32(f0 + 12, ts);
    wr32(f0 + 16, ts);
    f0[20] = ms;
    f0[21] = ms;
}

/* update the stream extension (and optionally the modification time) of
 * the set at sec/off and its checksum */
static int set_update(xfs_t *x, const u64 *sec, const u16 *off, int n, u32 first, u64 size, u64 vdl,
                      int nofat, int touch) {
    u8 (*raw)[32] = kmalloc(MAX_SET * 32);
    if (!raw) return E_NOMEM;
    int r = n > MAX_SET ? E_IO : set_read(x, sec, off, n, raw);
    if (!r) {
        stream_set(raw[1], first, size, vdl, nofat);
        if (touch) {
            u8 ms;
            u32 ts = exfat_now(&ms);
            wr32(raw[0] + 12, ts);
            wr32(raw[0] + 16, ts);
            raw[0][21] = ms;
            raw[0][23] = 0;
            raw[0][24] = 0;
            wr16(raw[0] + 4, rd16(raw[0] + 4) | ATTR_ARCH);
        }
        set_checksum_store(raw, n);
        r = set_write(x, sec, off, 2, raw);
    }
    kfree(raw);
    return r;
}

/* Add one zeroed cluster to the directory being scanned by d (its entry
 * set is pe, NULL for the root). */
static int dir_grow(xfs_t *x, diter_t *d, xdir_t *dir, xent_t *pe) {
    int r;
    u32 nc = 0;
    if (dir->root) {
        if (d->cidx + 1 >= x->dir_max_clus) return E_NOSPC;
        if ((r = clus_alloc(x, d->clus + 1, &nc))) return r;
        if ((r = fat_set(x, nc, EOC)) || (r = fat_set(x, d->clus, nc))) goto undo;
    } else {
        if (!pe || dir->size + x->csize > DIR_MAX_BYTES) return E_NOSPC;
        int nofat = dir->nofat;
        if (nofat && (r = clus_take(x, d->clus + 1)) != 0) {
            if (r < 0) return r;
            nc = d->clus + 1;
        } else {
            if ((r = clus_alloc(x, d->clus + 1, &nc))) return r;
            if (nofat && (r = fat_link_run(x, dir->first, d->ncl))) goto undo;
            nofat = 0;
            if ((r = fat_set(x, nc, EOC)) || (r = fat_set(x, d->clus, nc))) goto undo;
        }
        u64 nsize = (u64)(d->ncl + 1) * x->csize;
        if ((r = set_update(x, pe->sec, pe->off, pe->n, dir->first, nsize, nsize, nofat, 0))) goto undo;
        stream_set(pe->raw[1], dir->first, nsize, nsize, nofat);
        dir->size = nsize;
        dir->nofat = (u8)nofat;
        d->nofat = (u8)nofat;
        d->ncl++;
    }
    if ((r = clus_zero(x, nc))) return r;
    d->clus = nc;
    d->cidx++;
    d->sec = clus_sec(x, nc);
    d->idx++;
    return 0;
undo:
    bm_put(x, nc, 0, NULL);
    return r;
}

/* Find room for an n-entry set in dir (growing it if needed) and write it.
 * pe: the directory's own entry set (NULL for the root), updated if the
 * directory grows. Positions go to sec/off. */
static int dir_add(xfs_t *x, xdir_t *dir, xent_t *pe, u8 (*raw)[32], int n, u64 *sec, u16 *off) {
    diter_t d;
    int r = di_start(x, &d, dir);
    if (r) return r;
    int run = 0, eod = 0, nskip = 0;
    u64 ksec[16];
    u16 koff[16];
    for (;;) {
        int err = 0;
        const u8 *p = di_entry(x, &d, &err);
        if (!p) return err ? err : E_IO;
        if (p[0] == ET_EOD) eod = 1;
        /* everything after the end-of-directory marker is unused */
        if (eod || !(p[0] & ET_INUSE)) {
            /* a set that fits in one sector is kept in one: it is then
             * written in one piece, and a power cut leaves the old or the
             * new set, never one with a wrong checksum (renames in place
             * rely on this too: core/settings.c). Entries skipped past the
             * end marker stop being the end (below). */
            if (run && n <= (int)(x->ss / 32) && d.sec != sec[0]) {
                for (int i = 0; eod && i < run && nskip < 16; i++) { ksec[nskip] = sec[i]; koff[nskip++] = off[i]; }
                run = 0;
            }
            sec[run] = d.sec;
            off[run] = (u16)di_off(x, &d);
            if (++run == n) break;
        } else {
            run = 0;
        }
        r = di_next(x, &d);
        if (r < 0) return r;
        if (r == 1 && (r = dir_grow(x, &d, dir, pe))) return r;
    }
    if ((r = set_write(x, sec, off, n, raw))) return r;
    for (int i = 0; i < nskip; i++) {
        int err = 0;
        cent_t *c = cget(x, ksec[i], K_DIR, 1, 0, &err);
        if (!c) return err ? err : E_IO;
        if (c->buf[koff[i]] == ET_EOD) { c->buf[koff[i]] = 0x05; c->dirty = 1; }   /* unused, not the end */
    }
    if (eod) {
        /* the set went into the unused tail: keep an end marker after it */
        r = di_next(x, &d);
        if (r < 0) return r;
        if (r == 0) {
            int err = 0;
            cent_t *c = cget(x, d.sec, K_DIR, 1, 0, &err);
            if (!c) return err ? err : E_IO;
            if (c->buf[di_off(x, &d)] != ET_EOD) {
                c->buf[di_off(x, &d)] = ET_EOD;
                c->dirty = 1;
            }
        }
    }
    return 0;
}

/* free the clusters of benign secondaries with an allocation */
static int benign_free(xfs_t *x, const xent_t *e) {
    for (int i = 2 + e->nname; i < e->n; i++) {
        const u8 *p = e->raw[i];
        if (!(p[1] & SF_ALLOC)) continue;
        if (!alloc_sane(x, rd32(p + 20), rd64(p + 24), (p[1] & SF_NOFAT) != 0)) continue;
        int r = alloc_free(x, rd32(p + 20), clusters_for(x, rd64(p + 24)), (p[1] & SF_NOFAT) != 0);
        if (r) return r;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* file nodes and data                                                  */

static xnode_t *node_find(xfs_t *x, u64 sec, u16 off) {
    for (xnode_t *n = x->nodes; n; n = n->next)
        if (n->sec[0] == sec && n->off[0] == off) return n;
    return NULL;
}

static int node_store(xfs_t *x, xnode_t *n, int touch) {
    return set_update(x, n->sec, n->off, n->n, n->first, n->size, n->vdl, n->nofat, touch);
}

static int clus_at(xfs_t *x, xnode_t *n, u32 idx, u32 *out) {
    if (idx >= n->alloc || idx >= x->nclus || !clus_ok(x, n->first)) return E_IO;
    if (n->nofat) {
        u64 c = (u64)n->first + idx;
        if (c >= (u64)x->nclus + 2) return E_IO;
        *out = (u32)c;
        return 0;
    }
    u32 c, i;
    if (n->hint_clus && n->hint_idx <= idx) { c = n->hint_clus; i = n->hint_idx; }
    else { c = n->first; i = 0; }
    while (i < idx) {
        u32 nx;
        int r = chain_next(x, c, &nx);
        if (r) return r < 0 ? r : E_IO;
        c = nx;
        i++;
    }
    n->hint_idx = idx;
    n->hint_clus = c;
    *out = c;
    return 0;
}

/* read (buf != NULL, !write) or write (buf or zeros) file data */
static int data_io(xfs_t *x, xnode_t *n, u64 pos, u8 *buf, u32 len, int write) {
    u32 done = 0;
    while (done < len) {
        u64 ci64 = pos / x->csize;
        if (ci64 >= n->alloc) return E_IO;
        u32 cidx = (u32)ci64, coff = (u32)(pos % x->csize);
        u32 c;
        int r = clus_at(x, n, cidx, &c);
        if (r) return r;
        u32 remaining = len - done;
        u32 maxb = (write && !buf) ? x->zero_size : MAX_IO_BYTES;
        u32 run = 1, last = c;
        while ((u64)run * x->csize - coff < remaining && (u64)run * x->csize - coff < maxb) {
            if (cidx + run >= n->alloc) break;
            u32 nx;
            if (n->nofat) {
                nx = last + 1;
                if (!clus_ok(x, nx)) break;
            } else {
                r = chain_next(x, last, &nx);
                if (r < 0) return r;
                if (r == 1) break;
            }
            if (nx != last + 1) break;
            run++;
            last = nx;
        }
        u64 span64 = (u64)run * x->csize - coff;
        u32 span = (u32)MIN(span64, (u64)remaining);
        if (span > maxb) span = maxb;
        u64 sec = clus_sec(x, c) + (coff >> x->ss_shift);
        u32 soff = coff & (x->ss - 1), nb;
        if (soff || span < x->ss) {
            nb = MIN(x->ss - soff, span);
            if (write) {
                if (nb < x->ss && (r = dev_read(x, sec, 1, x->bounce))) return r;
                if (buf) memcpy(x->bounce + soff, buf + done, nb);
                else memset(x->bounce + soff, 0, nb);
                if ((r = dev_write(x, sec, 1, x->bounce))) return r;
                cache_inval(x, sec, 1);
            } else {
                if ((r = dev_read(x, sec, 1, x->bounce))) return r;
                memcpy(buf + done, x->bounce + soff, nb);
            }
        } else {
            u32 ns = span >> x->ss_shift;
            nb = ns << x->ss_shift;
            if (write) {
                if ((r = dev_write(x, sec, ns, buf ? buf + done : x->zero))) return r;
                cache_inval(x, sec, ns);
            } else if ((r = dev_read(x, sec, ns, buf + done))) {
                return r;
            }
        }
        if (!n->nofat) {
            u32 k = (coff + nb - 1) / x->csize;
            n->hint_idx = cidx + k;
            n->hint_clus = c + k;
        }
        pos += nb;
        done += nb;
    }
    return 0;
}

/* write zeros over [from, to) of the file */
static int data_zero(xfs_t *x, xnode_t *n, u64 from, u64 to) {
    int r = zero_buf(x);
    while (!r && from < to) {
        u32 len = (u32)MIN(to - from, (u64)0x40000000);
        r = data_io(x, n, from, NULL, len, 1);
        from += len;
    }
    return r;
}

/* cut the allocation to keep clusters (does not touch the set on disk) */
static int node_trim(xfs_t *x, xnode_t *n, u32 keep) {
    if (n->hint_clus && n->hint_idx >= keep) n->hint_clus = 0;
    if (keep >= n->alloc) return 0;
    if (!n->first) { n->alloc = 0; return 0; }
    u32 have = MIN(n->alloc, x->nclus);
    int r;
    if (keep == 0) {
        u32 old = n->first;
        int nofat = n->nofat;
        n->first = 0;
        n->nofat = 0;
        n->alloc = 0;
        n->hint_clus = 0;
        return alloc_free(x, old, have, nofat);
    }
    if (n->nofat) {
        r = alloc_free(x, n->first + keep, have - keep, 1);
        n->alloc = keep;
        return r;
    }
    u32 c, nx;
    if ((r = clus_at(x, n, keep - 1, &c))) return r;
    n->alloc = keep;
    int end = chain_next(x, c, &nx);
    if (end < 0) return end;
    if ((r = fat_set(x, c, EOC))) return r;
    if (end == 1) return 0;
    return alloc_free(x, nx, have - keep, 0);
}

/* make the allocation cover end bytes; *cap = bytes actually covered */
static int node_extend(xfs_t *x, xnode_t *n, u64 end, u64 *cap) {
    *cap = end;
    if (!n->alloc && n->first) {
        /* clusters on an empty file: not ours to free (maybe cross-linked) */
        n->first = 0;
        n->nofat = 0;
        n->hint_clus = 0;
    }
    u64 need64 = clusters_for(x, end);
    if (need64 <= n->alloc) return 0;
    int capped = 0;
    if (need64 > x->nclus) {
        need64 = x->nclus;              /* allocate what we can, then stop */
        capped = 1;
    }
    u32 need = (u32)need64;
    u32 last = 0;
    int r;
    if (n->alloc && (r = clus_at(x, n, n->alloc - 1, &last))) return r;
    while (n->alloc < need) {
        u32 nc = 0;
        if (!n->alloc) {
            if ((r = clus_alloc(x, 0, &nc))) break;
            n->first = nc;
            n->nofat = 1;
        } else if (n->nofat && (r = clus_take(x, last + 1)) != 0) {
            if (r < 0) break;
            nc = last + 1;
        } else {
            if ((r = clus_alloc(x, last + 1, &nc))) break;
            if (n->nofat) {
                /* the next cluster is taken: switch to a FAT chain */
                if ((r = fat_link_run(x, n->first, n->alloc))) { bm_put(x, nc, 0, NULL); break; }
                n->nofat = 0;
            }
            if ((r = fat_set(x, nc, EOC)) || (r = fat_set(x, last, nc))) { bm_put(x, nc, 0, NULL); break; }
        }
        last = nc;
        n->alloc++;
        if (!n->nofat) { n->hint_idx = n->alloc - 1; n->hint_clus = nc; }
        r = 0;
    }
    if (n->alloc < need || r) {
        u64 got = (u64)n->alloc * x->csize;
        if (got < *cap) *cap = got;
        return r ? r : E_NOSPC;
    }
    if (capped) {
        *cap = MIN(*cap, (u64)n->alloc * x->csize);
        return E_NOSPC;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* fs_ops                                                               */

#define XFS(v) ((xfs_t *)(v)->fs_priv)

typedef struct {
    xent_t e, pe;
    char name[VFS_MAX_NAME + 1];
    u16 units[MAX_UNITS];
    u8 raw[MAX_SET][32];
    u64 sec[MAX_SET];
    u16 off[MAX_SET];
} opctx_t;

static void fill_dirent(const xent_t *e, vfs_dirent_t *o) {
    memset(o, 0, sizeof *o);
    strlcpy(o->name, e->name, sizeof o->name);
    o->attr = (u8)(e_attr(e) & 0x37);
    o->is_dir = e_isdir(e);
    if (o->is_dir) o->attr |= VFS_ATTR_DIR;
    o->size = o->is_dir ? 0 : e_size(e);
    ts_to_vtime(rd32(e->raw[0] + 12), e->raw[0][21], &o->mtime);
    ts_to_vtime(rd32(e->raw[0] + 8), e->raw[0][20], &o->ctime);
}

static void node_unref(xfs_t *x, xnode_t *n) {
    if (--n->refs) return;
    for (xnode_t **pp = &x->nodes; *pp; pp = &(*pp)->next)
        if (*pp == n) { *pp = n->next; break; }
    kfree(n);
}

static xnode_t *node_new(xfs_t *x, const u64 *sec, const u16 *off, int cnt) {
    xnode_t *n = kzalloc(sizeof *n);
    if (!n) return NULL;
    memcpy(n->sec, sec, sizeof(u64) * cnt);
    memcpy(n->off, off, sizeof(u16) * cnt);
    n->n = (u8)cnt;
    n->refs = 1;
    n->next = x->nodes;
    x->nodes = n;
    return n;
}

static int exfat_open(volume_t *v, const char *path, int flags, void **fh) {
    xfs_t *x = XFS(v);
    if (!*path) return E_ISDIR;
    if (x->ro && (flags & (VFS_O_WRITE | VFS_O_TRUNC))) return E_ROFS;
    opctx_t *o = kmalloc(sizeof *o);
    if (!o) return E_NOMEM;
    xnode_t *n = NULL;
    xdir_t dir;
    int isroot, r = resolve_parent(x, path, &o->pe, &isroot, &dir, o->name, NULL, NULL);
    if (r) goto out;
    r = dir_lookup(x, &dir, o->name, &o->e);
    if (r == E_NOENT && (flags & VFS_O_CREATE)) {
        int nu;
        if (x->ro) { r = E_ROFS; goto out; }
        if ((r = name_units(o->name, o->units, &nu))) goto out;
        if ((r = mark_dirty(x))) goto out;
        u8 f0[32], st0[32];
        file_entry_new(f0, ATTR_ARCH);
        memset(st0, 0, 32);
        stream_set(st0, 0, 0, 0, 0);
        int cnt = set_build(x, o->raw, f0, st0, o->units, nu, NULL, 0);
        if (cnt < 0) { r = cnt; goto out; }
        r = dir_add(x, &dir, isroot ? NULL : &o->pe, o->raw, cnt, o->sec, o->off);
        int r2 = exfat_flush(x);
        if (!r) r = r2;
        if (r) goto out;
        n = node_new(x, o->sec, o->off, cnt);
        if (!n) { r = E_NOMEM; goto out; }
        n->attr = ATTR_ARCH;
    } else if (r) {
        goto out;
    } else {
        xent_t *e = &o->e;
        if ((flags & VFS_O_CREATE) && (flags & VFS_O_EXCL)) { r = E_EXIST; goto out; }
        if (e_isdir(e)) { r = E_ISDIR; goto out; }
        if ((flags & VFS_O_WRITE) && (e_attr(e) & ATTR_RO)) { r = E_ROFS; goto out; }
        n = node_find(x, e->sec[0], e->off[0]);
        if (n) {
            n->refs++;
        } else {
            n = node_new(x, e->sec, e->off, e->n);
            if (!n) { r = E_NOMEM; goto out; }
            n->first = e_first(e);
            n->size = e_size(e);
            n->vdl = MIN(e_vdl(e), n->size);
            n->nofat = e_nofat(e);
            n->attr = e_attr(e);
            u64 a = clusters_for(x, n->size);
            n->alloc = a > x->nclus ? x->nclus + 1 : (u32)a;   /* larger than any allocation: corrupt */
            n->bad = !alloc_sane(x, n->first, n->size, n->nofat);
        }
        if ((flags & VFS_O_TRUNC) && (flags & VFS_O_WRITE) && (n->size || n->first)) {
            if (n->bad) { node_unref(x, n); r = E_IO; goto out; }
            u32 old = n->first, alloc = MIN(n->alloc, x->nclus);
            int nofat = n->nofat;
            if ((r = mark_dirty(x))) { node_unref(x, n); goto out; }
            n->size = n->vdl = 0;
            n->first = 0;
            n->alloc = 0;
            n->nofat = 0;
            n->hint_clus = 0;
            r = node_store(x, n, 1);
            int r2 = exfat_flush(x);
            if (!r) r = r2;
            if (!r && old) {
                r = alloc_free(x, old, alloc, nofat);
                r2 = exfat_flush(x);
                if (!r) r = r2;
            }
            if (r) { node_unref(x, n); goto out; }
        }
    }
    *fh = n;
    r = 0;
out:
    kfree(o);
    return r;
}

static int exfat_close(volume_t *v, void *fh) {
    xfs_t *x = XFS(v);
    int r = exfat_flush(x);
    node_unref(x, fh);
    return r;
}

static int exfat_read(volume_t *v, void *fh, u64 pos, void *buf, u32 len) {
    xfs_t *x = XFS(v);
    xnode_t *n = fh;
    if (pos >= n->size) return 0;
    if (len > n->size - pos) len = (u32)(n->size - pos);
    if (len > 0x7FFFFFFF) len = 0x7FFFFFFF;
    if (!len) return 0;
    u32 disk = pos < n->vdl ? (u32)MIN((u64)len, n->vdl - pos) : 0;
    if (disk) {
        int r = data_io(x, n, pos, buf, disk, 0);
        if (r) return r;
    }
    if (disk < len) {
        /* past ValidDataLength: zeros, if the allocation can exist at all */
        if (n->bad) return disk ? (int)disk : E_IO;
        memset((u8 *)buf + disk, 0, len - disk);
    }
    return (int)len;
}

static int exfat_write(volume_t *v, void *fh, u64 pos, const void *buf, u32 len) {
    xfs_t *x = XFS(v);
    xnode_t *n = fh;
    if (x->ro) return E_ROFS;
    if (!len) return 0;
    if (len > 0x7FFFFFFF) len = 0x7FFFFFFF;
    u64 maxsz = (u64)x->nclus * x->csize;
    if (n->bad) return E_IO;
    if (pos >= maxsz) return E_NOSPC;
    int r = mark_dirty(x);
    if (r) return r;
    u64 end = pos + len, cap;
    r = node_extend(x, n, end, &cap);
    if (r && r != E_NOSPC) goto fail;
    if (cap < end) end = cap;
    if (end <= pos) { r = E_NOSPC; goto fail; }
    if (pos > n->vdl && (r = data_zero(x, n, n->vdl, pos))) goto fail;
    if ((r = data_io(x, n, pos, (u8 *)buf, (u32)(end - pos), 1))) goto fail;
    if (end > n->size) n->size = end;
    if (end > n->vdl) n->vdl = end;
    r = node_store(x, n, 1);
    int r2 = exfat_flush(x);
    if (r || r2) return r ? r : r2;
    return (int)(end - pos);
fail:
    node_trim(x, n, (u32)MIN(clusters_for(x, n->size), (u64)x->nclus + 1));
    exfat_flush(x);
    return r;
}

static int exfat_truncate(volume_t *v, void *fh, u64 size) {
    xfs_t *x = XFS(v);
    xnode_t *n = fh;
    if (x->ro) return E_ROFS;
    if (n->bad) return E_IO;
    int r = mark_dirty(x);
    if (r) return r;
    if (size > n->size) {
        if (size > (u64)x->nclus * x->csize) return E_NOSPC;
        u64 cap;
        r = node_extend(x, n, size, &cap);
        if (r) {
            node_trim(x, n, (u32)MIN(clusters_for(x, n->size), (u64)x->nclus + 1));
            exfat_flush(x);
            return r;
        }
        n->size = size;             /* ValidDataLength stays: the rest reads as zeros */
    } else if (size < n->size) {
        n->size = size;
        if (n->vdl > size) n->vdl = size;
        u32 keep = (u32)clusters_for(x, size);
        u32 old = n->first, alloc = MIN(n->alloc, x->nclus);
        int nofat = n->nofat;
        if (keep == 0) { n->first = 0; n->nofat = 0; n->alloc = 0; n->hint_clus = 0; }
        if ((r = node_store(x, n, 1)) || (r = exfat_flush(x))) return r;
        if (keep == 0) r = alloc_free(x, old, alloc, nofat);
        else r = node_trim(x, n, keep);
        int r2 = exfat_flush(x);
        return r ? r : r2;
    }
    r = node_store(x, n, 1);
    int r2 = exfat_flush(x);
    return r ? r : r2;
}

static u64 exfat_size(volume_t *v, void *fh) { return ((xnode_t *)fh)->size; }

typedef struct {
    int (*cb)(void *ctx, const vfs_dirent_t *e);
    void *ctx;
    vfs_dirent_t de;
} rd_ctx_t;

static int readdir_cb(xfs_t *x, xent_t *e, void *vctx) {
    rd_ctx_t *c = vctx;
    fill_dirent(e, &c->de);
    return c->cb(c->ctx, &c->de) ? 1 : 0;
}

static int exfat_readdir(volume_t *v, const char *path, int (*cb)(void *ctx, const vfs_dirent_t *e), void *ctx) {
    xfs_t *x = XFS(v);
    xent_t *e = kmalloc(sizeof *e);
    rd_ctx_t *c = kmalloc(sizeof *c);
    int r = (!e || !c) ? E_NOMEM : 0, isroot = 0;
    xdir_t dir;
    if (!r) r = resolve(x, path, e, &isroot, NULL, NULL);
    if (!r) {
        if (isroot) root_dir(x, &dir);
        else r = dir_of(x, e, &dir);
    }
    if (!r) {
        c->cb = cb;
        c->ctx = ctx;
        r = dir_scan(x, &dir, readdir_cb, c, e);
        if (r > 0) r = 0;
    }
    kfree(e);
    kfree(c);
    return r;
}

static int exfat_stat(volume_t *v, const char *path, vfs_dirent_t *out) {
    xfs_t *x = XFS(v);
    xent_t *e = kmalloc(sizeof *e);
    if (!e) return E_NOMEM;
    int isroot;
    int r = resolve(x, path, e, &isroot, NULL, NULL);
    if (!r) {
        if (isroot) {
            memset(out, 0, sizeof *out);
            out->attr = VFS_ATTR_DIR;
            out->is_dir = 1;
        } else {
            fill_dirent(e, out);
            xnode_t *n = node_find(x, e->sec[0], e->off[0]);
            if (n) out->size = n->size;
        }
    }
    kfree(e);
    return r;
}

static int exfat_mkdir(volume_t *v, const char *path) {
    xfs_t *x = XFS(v);
    if (x->ro) return E_ROFS;
    opctx_t *o = kmalloc(sizeof *o);
    if (!o) return E_NOMEM;
    xdir_t dir;
    u32 nc = 0;
    int nu, isroot, r = resolve_parent(x, path, &o->pe, &isroot, &dir, o->name, NULL, NULL);
    if (!r) {
        r = dir_lookup(x, &dir, o->name, &o->e);
        if (!r) r = E_EXIST;
        else if (r == E_NOENT) r = name_units(o->name, o->units, &nu);
    }
    if (!r) r = mark_dirty(x);
    if (r) goto out;
    if ((r = clus_alloc(x, 0, &nc))) goto out;
    if ((r = clus_zero(x, nc))) goto undo;
    {
        u8 f0[32], st0[32];
        file_entry_new(f0, ATTR_DIR);
        memset(st0, 0, 32);
        stream_set(st0, nc, x->csize, x->csize, 1);
        int cnt = set_build(x, o->raw, f0, st0, o->units, nu, NULL, 0);
        if (cnt < 0) { r = cnt; goto undo; }
        if ((r = dir_add(x, &dir, isroot ? NULL : &o->pe, o->raw, cnt, o->sec, o->off))) goto undo;
    }
    r = exfat_flush(x);
    goto out;
undo:
    bm_put(x, nc, 0, NULL);
    exfat_flush(x);
out:
    kfree(o);
    return r;
}

static int dir_nonempty_cb(xfs_t *x, xent_t *e, void *ctx) { return 1; }

static int exfat_unlink(volume_t *v, const char *path) {
    xfs_t *x = XFS(v);
    if (x->ro) return E_ROFS;
    if (!*path) return E_INVAL;
    opctx_t *o = kmalloc(sizeof *o);
    if (!o) return E_NOMEM;
    xent_t *e = &o->e;
    int isroot, r = resolve(x, path, e, &isroot, NULL, NULL);
    if (r) goto out;
    if (node_find(x, e->sec[0], e->off[0])) { r = E_BUSY; goto out; }
    if (e_isdir(e)) {
        xdir_t dir;
        if ((r = dir_of(x, e, &dir))) goto out;
        r = dir_scan(x, &dir, dir_nonempty_cb, NULL, &o->pe);
        if (r < 0) goto out;
        if (r == 1) { r = E_NOTEMPTY; goto out; }
    }
    if ((r = mark_dirty(x))) goto out;
    if ((r = set_delete(x, e->sec, e->off, e->n)) || (r = exfat_flush(x))) goto out;
    if (alloc_sane(x, e_first(e), e_size(e), e_nofat(e)))
        r = alloc_free(x, e_first(e), clusters_for(x, e_size(e)), e_nofat(e));
    else
        kprintf("exfat: %s: '%s' has an impossible allocation, its clusters are not freed\n", x->bd->name, e->name);
    if (!r) r = benign_free(x, e);
    int r2 = exfat_flush(x);
    if (!r) r = r2;
out:
    kfree(o);
    return r;
}

static int exfat_rename(volume_t *v, const char *from, const char *to) {
    xfs_t *x = XFS(v);
    if (x->ro) return E_ROFS;
    if (!*from || !*to) return E_INVAL;
    opctx_t *o = kmalloc(sizeof *o);
    xent_t *src = kmalloc(sizeof *src);
    int r = (!o || !src) ? E_NOMEM : 0, isroot, nu, hit = 0;
    xdir_t ddir, sdir;
    /* the source and its directory (o->e is scratch here) */
    if (!r) r = resolve_parent(x, from, &o->e, &isroot, &sdir, o->name, NULL, NULL);
    if (!r) r = dir_lookup(x, &sdir, o->name, src);
    if (r) goto out;
    /* the destination's parent: must not be src itself or inside it */
    r = resolve_parent(x, to, &o->pe, &isroot, &ddir, o->name, e_isdir(src) ? src : NULL, &hit);
    if (r) goto out;
    if (hit) { r = E_INVAL; goto out; }
    r = dir_lookup(x, &ddir, o->name, &o->e);
    if (!r) {
        if (!same_set(&o->e, src->sec[0], src->off[0])) { r = E_EXIST; goto out; }
        if (!strcmp(src->name, o->name)) { r = 0; goto out; }   /* same name: nothing to do */
    } else if (r != E_NOENT) {
        goto out;
    }
    if ((r = name_units(o->name, o->units, &nu))) goto out;
    int cnt = set_build(x, o->raw, src->raw[0], src->raw[1], o->units, nu,
                        (const u8 (*)[32])src->raw + 2 + src->nname, src->n - 2 - src->nname);
    if (cnt < 0) { r = cnt; goto out; }
    if ((r = mark_dirty(x))) goto out;
    /* same directory and the new set is not larger: rewrite it in place */
    if (sdir.first == ddir.first && cnt <= src->n) {
        memcpy(o->sec, src->sec, sizeof(u64) * cnt);
        memcpy(o->off, src->off, sizeof(u16) * cnt);
        if ((r = set_write(x, o->sec, o->off, cnt, o->raw))) goto out;
        if (cnt < src->n && (r = set_delete(x, src->sec + cnt, src->off + cnt, src->n - cnt))) goto out;
    } else {
        if ((r = dir_add(x, &ddir, isroot ? NULL : &o->pe, o->raw, cnt, o->sec, o->off))) { exfat_flush(x); goto out; }
        if ((r = set_delete(x, src->sec, src->off, src->n))) goto out;
    }
    xnode_t *n = node_find(x, src->sec[0], src->off[0]);
    if (n) {
        memcpy(n->sec, o->sec, sizeof(u64) * cnt);
        memcpy(n->off, o->off, sizeof(u16) * cnt);
        n->n = (u8)cnt;
    }
    r = exfat_flush(x);
out:
    kfree(o);
    kfree(src);
    return r;
}

static int exfat_statfs(volume_t *v, u64 *total, u64 *free_bytes) {
    xfs_t *x = XFS(v);
    if (x->free_count == FREE_UNKNOWN) {
        int r = count_free(x);
        if (r) return r;
    }
    if (total) *total = (u64)x->nclus * x->csize;
    if (free_bytes) *free_bytes = (u64)x->free_count * x->csize;
    return 0;
}

static int exfat_sync(volume_t *v) {
    xfs_t *x = XFS(v);
    if (x->ro) return 0;
    if (x->dirty_on_disk && x->free_count == FREE_UNKNOWN) count_free(x);   /* for PercentInUse */
    return flush_clean(x);
}

static void xfs_free(xfs_t *x) {
    while (x->nodes) {
        xnode_t *n = x->nodes;
        x->nodes = n->next;
        kfree(n);
    }
    kfree(x->cache_mem);
    kfree(x->bounce);
    kfree(x->ra_buf);
    kfree(x->wb_buf);
    kfree(x->zero);
    kfree(x->bm_clus);
    kfree(x->upcase);
    kfree(x);
}

static int exfat_unmount(volume_t *v) {
    xfs_t *x = XFS(v);
    if (!x) return 0;
    int r = x->ro ? 0 : exfat_sync(v);
    xfs_free(x);
    v->fs_priv = NULL;
    return r;
}

static const fs_ops_t exfat_ops = {
    .name = "exfat",
    .unmount = exfat_unmount,
    .open = exfat_open,
    .close = exfat_close,
    .read = exfat_read,
    .write = exfat_write,
    .truncate = exfat_truncate,
    .size = exfat_size,
    .readdir = exfat_readdir,
    .stat = exfat_stat,
    .mkdir = exfat_mkdir,
    .unlink = exfat_unlink,
    .rename = exfat_rename,
    .statfs = exfat_statfs,
    .sync = exfat_sync,
};

/* ------------------------------------------------------------------ */
/* mount                                                                */

static u32 boot_checksum(const u8 *region, u32 ss) {
    u32 c = 0;
    for (u32 i = 0; i < 11 * ss; i++) {
        if (i == 106 || i == 107 || i == 112) continue;
        c = ((c & 1) ? 0x80000000u : 0) + (c >> 1) + region[i];
    }
    return c;
}

static int boot_region_ok(const u8 *region, u32 ss) {
    if (!exfat_boot_sane(region, 0) || 1u << region[108] != ss) return 0;
    u32 sum = boot_checksum(region, ss);
    for (u32 i = 0; i < ss; i += 4)
        if (rd32(region + 11 * ss + i) != sum) return 0;
    return 1;
}

int exfat_boot_sane(const u8 *s, u32 dev_ss) {
    if (memcmp(s + 3, "EXFAT   ", 8) || s[510] != 0x55 || s[511] != 0xAA) return 0;
    for (int i = 11; i < 64; i++) if (s[i]) return 0;
    u32 bss = s[108], spc = s[109];
    if (bss < 9 || bss > 12 || spc > 25 - bss) return 0;
    if (dev_ss && (1u << bss) < dev_ss) return 0;
    if (s[110] < 1 || s[110] > 2) return 0;
    if ((rd16(s + 104) >> 8) != 1) return 0;     /* revision 1.x */
    return 1;
}

/* read len bytes of an allocation described by a FAT chain from first */
static int read_chain(xfs_t *x, u32 first, u32 len, u8 *out, u32 *clus_list, u32 maxclus) {
    u32 c = first, n = (u32)clusters_for(x, len);
    if (n > maxclus && clus_list) return E_IO;
    for (u32 i = 0; i < n; i++) {
        if (!clus_ok(x, c)) return E_IO;
        if (clus_list) clus_list[i] = c;
        if (out) {
            u32 part = MIN(x->csize, len - i * x->csize);
            u32 secs = (part + x->ss - 1) >> x->ss_shift;
            for (u32 s = 0; s < secs; s++) {
                int r = dev_read(x, clus_sec(x, c) + s, 1, x->bounce);
                if (r) return r;
                u32 o = i * x->csize + s * x->ss;
                memcpy(out + o, x->bounce, MIN(x->ss, len - o));
            }
        }
        if (i + 1 < n) {
            int r = chain_next(x, c, &c);
            if (r) return r < 0 ? r : E_IO;
        }
    }
    return 0;
}

static u32 table_checksum(const u8 *p, u32 n) {
    u32 c = 0;
    for (u32 i = 0; i < n; i++) c = ((c & 1) ? 0x80000000u : 0) + (c >> 1) + p[i];
    return c;
}

static void load_upcase(xfs_t *x, const u8 *ent) {
    u32 sum = rd32(ent + 4), first = rd32(ent + 20);
    u64 len = rd64(ent + 24);
    const char *why = NULL;
    u8 *raw = NULL;
    if (!len || len > UPCASE_MAX || (len & 1)) why = "bad size";
    else if (!clus_ok(x, first)) why = "bad cluster";
    if (!why && !(raw = kmalloc((size_t)len))) why = "out of memory";
    if (!why && read_chain(x, first, (u32)len, raw, NULL, 0)) why = "unreadable";
    if (!why && table_checksum(raw, (u32)len) != sum) why = "checksum mismatch";
    if (!why) {
        x->upcase = kmalloc(0x10000 * sizeof(u16));
        if (!x->upcase) why = "out of memory";
    }
    if (!why) {
        u32 n = 0, words = (u32)len / 2;
        for (u32 i = 0; i < words && n < 0x10000; i++) {
            u16 w = rd16(raw + 2 * i);
            if (w == 0xFFFF && i + 1 < words) {
                u32 run = rd16(raw + 2 * ++i);
                while (run-- && n < 0x10000) { x->upcase[n] = (u16)n; n++; }
            } else {
                x->upcase[n++] = w;
            }
        }
        x->upcase_n = n;
        /* the mandatory part: a-z map to A-Z */
        for (u32 c = 'a'; c <= 'z'; c++)
            if (c >= n || x->upcase[c] != c - 32) { why = "a-z not mapped"; break; }
    }
    kfree(raw);
    if (why) {
        kprintf("exfat: %s: up-case table %s, using the built-in mapping\n", x->bd->name, why);
        kfree(x->upcase);
        x->upcase = NULL;
        x->upcase_n = 0;
    }
}

/* walk the root directory for the bitmap, up-case table and label */
static int read_root_meta(xfs_t *x, int active, char *label) {
    xdir_t dir;
    diter_t d;
    root_dir(x, &dir);
    int r = di_start(x, &d, &dir);
    if (r) return r;
    u8 bm[32], uc[32];
    int have_bm = 0, have_uc = 0;
    label[0] = 0;
    for (;;) {
        int err = 0;
        const u8 *p = di_entry(x, &d, &err);
        if (!p) return err ? err : E_IO;
        if (p[0] == ET_EOD) break;
        if (p[0] == ET_BITMAP && (p[1] & 1) == active && !have_bm) { memcpy(bm, p, 32); have_bm = 1; }
        else if (p[0] == ET_UPCASE && !have_uc) { memcpy(uc, p, 32); have_uc = 1; }
        else if (p[0] == ET_LABEL && !label[0]) {
            u16 u[11];
            int n = MIN(p[1], 11), o = 0;
            for (int i = 0; i < n; i++) u[i] = rd16(p + 2 + 2 * i);
            for (int i = 0; i < n; i++) {
                u32 cp = u[i];
                if (cp >= 0xD800 && cp <= 0xDFFF) cp = '_';
                if (cp < 0x20) cp = '_';
                char tmp[4];
                int l = utf8_put(cp, tmp);
                if (o + l > 15) break;
                memcpy(label + o, tmp, l);
                o += l;
            }
            while (o && label[o - 1] == ' ') o--;
            label[o] = 0;
        }
        r = di_next(x, &d);
        if (r == 1) break;
        if (r < 0) return r;
    }
    if (!have_bm) {
        kprintf("exfat: %s: no allocation bitmap\n", x->bd->name);
        return E_INVAL;
    }
    u32 bfirst = rd32(bm + 20);
    u64 blen = rd64(bm + 24), need = ((u64)x->nclus + 7) / 8;
    if (!clus_ok(x, bfirst) || blen < need) {
        kprintf("exfat: %s: bad allocation bitmap entry\n", x->bd->name);
        return E_INVAL;
    }
    x->bm_n = (u32)clusters_for(x, need);
    x->bm_clus = kmalloc(sizeof(u32) * x->bm_n);
    if (!x->bm_clus) return E_NOMEM;
    if ((r = read_chain(x, bfirst, (u32)need, NULL, x->bm_clus, x->bm_n))) {
        kprintf("exfat: %s: allocation bitmap outside the volume\n", x->bd->name);
        return r == E_NOMEM ? r : E_INVAL;
    }
    if (have_uc) load_upcase(x, uc);
    else kprintf("exfat: %s: no up-case table, using the built-in mapping\n", x->bd->name);
    /* the bitmap must cover its own clusters and the root: if not, the
     * bitmap cannot be trusted for allocation */
    if (!x->ro) {
        int used = 0;
        for (u32 i = 0; i < x->bm_n && !r; i++) {
            r = bm_get(x, x->bm_clus[i], &used);
            if (!r && !used) break;
        }
        if (!r && used) r = bm_get(x, x->root, &used);
        if (r || !used) {
            kprintf("exfat: %s: allocation bitmap inconsistent, mounting read-only\n", x->bd->name);
            x->ro = 1;
        }
    }
    return 0;
}

int exfat_probe(blockdev_t *bd, volume_t *v) {
    u32 dss = bd->sector_size;
    if (dss < 512 || dss > 4096 || (dss & (dss - 1))) return E_NOTSUP;
    u8 *s = kmalloc(dss);
    if (!s) return E_NOMEM;
    int r = blk_read(bd, 0, 1, s);
    if (r) { kfree(s); return r; }
    if (!exfat_boot_sane(s, dss)) { kfree(s); return E_INVAL; }
    u32 ss_shift = s[108], ss = 1u << ss_shift, dshift = 0;
    kfree(s);
    while ((dss << dshift) < ss) dshift++;
    u64 dev_secs = bd->sector_count >> dshift;
    if (dev_secs < 12) return E_INVAL;

    xfs_t *x = kzalloc(sizeof *x);
    u8 *region = kmalloc((size_t)12 * ss);
    if (!x || !region) { kfree(x); kfree(region); return E_NOMEM; }
    x->v = v;
    x->bd = bd;
    x->ss = ss;
    x->ss_shift = ss_shift;
    x->dshift = dshift;
    x->ro = bd->readonly;
    /* main boot region, or the backup if the main one is damaged */
    if ((r = dev_read(x, 0, 12, region))) goto fail;
    if (!boot_region_ok(region, ss)) {
        kprintf("exfat: %s: main boot region checksum mismatch, trying the backup\n", bd->name);
        if (dev_secs < 24 || dev_read(x, 12, 12, region) || !boot_region_ok(region, ss)) goto bad;
        kprintf("exfat: %s: using the backup boot region, read-only\n", bd->name);
        x->ro = 1;
    }
    {
        const u8 *b = region;
        u64 vol_len = rd64(b + 72);
        u32 fat_off = rd32(b + 80), fat_len = rd32(b + 84), heap = rd32(b + 88);
        u32 nclus = rd32(b + 92), nfats = b[110];
        x->spc_shift = b[109];
        x->csize = ss << x->spc_shift;
        x->root = rd32(b + 96);
        x->mount_flags = rd16(b + 106);
        u32 active = (nfats == 2) ? (x->mount_flags & VF_ACTIVEFAT) : 0;
        if (fat_off < 24 || !fat_len || !nclus || nclus > 0xFFFFFFF5u) goto bad;
        if ((u64)fat_off + (u64)fat_len * nfats > heap) goto bad;
        if ((u64)heap + ((u64)nclus << x->spc_shift) > vol_len) goto bad;
        if ((u64)fat_len * ss < ((u64)nclus + 2) * 4) goto bad;
        x->fat_start = fat_off + (u64)active * fat_len;
        x->fat_len = fat_len;
        x->heap = heap;
        if (x->fat_start + fat_len > dev_secs || heap >= dev_secs) goto bad;
        if ((u64)heap + ((u64)nclus << x->spc_shift) > dev_secs) {
            /* volume larger than the device (truncated image, bad field):
             * clusters past the end of the device don't exist */
            kprintf("exfat: %s: file system exceeds the device, clusters limited\n", bd->name);
            nclus = (u32)((dev_secs - heap) >> x->spc_shift);
            if (!nclus) goto bad;
        }
        x->nclus = nclus;
        if (!clus_ok(x, x->root)) goto bad;
        x->dir_max_clus = (u32)MIN((u64)nclus, (u64)(DIR_MAX_BYTES >> (ss_shift + x->spc_shift)));
        if (!x->dir_max_clus) x->dir_max_clus = 1;
        x->free_count = FREE_UNKNOWN;
        x->next_free = 2;
        x->cache_mem = kmalloc((size_t)CACHE_N * ss);
        x->bounce = kmalloc(ss);
        x->ra_buf = kmalloc((size_t)RUN_N * ss);
        x->wb_buf = kmalloc((size_t)RUN_N * ss);
        if (!x->cache_mem || !x->bounce || !x->ra_buf || !x->wb_buf) { r = E_NOMEM; goto fail; }
        for (int i = 0; i < CACHE_N; i++) x->cache[i].buf = x->cache_mem + (size_t)i * ss;
        if (x->mount_flags & VF_DIRTY)
            kprintf("exfat: %s: volume was not cleanly unmounted (VolumeDirty set)\n", bd->name);
        if ((r = read_root_meta(x, (int)active, v->label))) {
            if (r != E_NOMEM) r = E_INVAL;
            goto fail;
        }
    }
    kfree(region);
    v->ops = &exfat_ops;
    v->fs_priv = x;
    strlcpy(v->fs_name, "exFAT", sizeof v->fs_name);
    kprintf("exfat: %s: %u clusters of %u bytes, label '%s'%s\n", bd->name, x->nclus, x->csize, v->label,
            x->ro ? " (read-only)" : "");
    return 0;
bad:
    r = E_INVAL;
fail:
    kfree(region);
    xfs_free(x);
    return r;
}

/* ------------------------------------------------------------------ */
/* format                                                               */

/* compressed up-case table derived from uc_upper(); returns the byte size
 * (out may be NULL to measure) */
static u32 upcase_build(u8 *out) {
    u32 o = 0;
    u32 c = 0;
    while (c < 0x10000) {
        u32 run = 0;
        while (c + run < 0x10000 && uc_upper(c + run) == c + run && run < 0xFFFF) run++;
        if (run >= 3 || (run && c + run - 1 == 0xFFFF)) {
            if (out) { wr16(out + o, 0xFFFF); wr16(out + o + 2, (u16)run); }
            o += 4;
            c += run;
            continue;
        }
        u32 m = uc_upper(c);
        if (out) wr16(out + o, (u16)m);
        o += 2;
        c++;
    }
    return o;
}

/* write n sectors of buf (or zeros if buf is NULL, from zbuf of zsz bytes) */
static int fmt_write(blockdev_t *bd, u64 sec, u64 n, const u8 *buf, const u8 *zbuf, u32 zsz) {
    u32 ss = bd->sector_size, per = zsz / ss;
    while (n) {
        u32 k = (u32)MIN(n, (u64)per);
        int r = blk_write(bd, sec, k, buf ? buf : zbuf);
        if (r) return r;
        if (buf) buf += (size_t)k * ss;
        sec += k;
        n -= k;
    }
    return 0;
}

int exfat_format(blockdev_t *bd, const char *label, u32 cluster_bytes) {
    if (!bd || !bd->present) return E_NODEV;
    if (bd->readonly) return E_ROFS;
    u32 ss = bd->sector_size;
    if (ss < 512 || ss > 4096 || (ss & (ss - 1))) return E_NOTSUP;
    u32 ss_shift = 0;
    while ((1u << ss_shift) < ss) ss_shift++;
    u64 vol = bd->sector_count, bytes = vol << ss_shift;
    if (bytes < 1024 * 1024) return E_NOSPC;
    /* label */
    u16 lab[12];
    int nlab = 0;
    if (label && *label) {
        if (!utf8_valid(label)) return E_INVAL;
        nlab = utf8_to_utf16(label, lab, 11);
        if (nlab < 0) return E_INVAL;
        for (int i = 0; i < nlab; i++)
            if (lab[i] < 0x20 || (lab[i] < 0x80 && strchr("\"*/:<>?\\|", lab[i]))) return E_INVAL;
    }
    if (!cluster_bytes)
        cluster_bytes = bytes <= 256ull * 1024 * 1024 ? 4096 : bytes <= 32ull * 1024 * 1024 * 1024 ? 32768 : 131072;
    if (cluster_bytes < ss || cluster_bytes > 32u * 1024 * 1024 || (cluster_bytes & (cluster_bytes - 1))) return E_INVAL;
    u32 spc_shift = 0;
    while ((ss << spc_shift) < cluster_bytes) spc_shift++;
    u32 spc = 1u << spc_shift;
    /* layout: boot regions, FAT aligned like the clusters, then the heap */
    u64 align = MAX((u64)spc, (u64)MAX(4096u >> ss_shift, 1u));
    u64 fat_off = ALIGN_UP((u64)24, align);
    if (fat_off >= vol) return E_NOSPC;
    u64 maxclus = (vol - fat_off) >> spc_shift;
    if (maxclus > 0xFFFFFFF5u) maxclus = 0xFFFFFFF5u;
    u64 fat_len = ALIGN_UP(((maxclus + 2) * 4 + ss - 1) >> ss_shift, align);
    u64 heap = ALIGN_UP(fat_off + fat_len, (u64)spc);
    if (heap >= vol) return E_NOSPC;
    u64 nclus64 = (vol - heap) >> spc_shift;
    if (nclus64 > maxclus) nclus64 = maxclus;
    u32 nclus = (u32)nclus64;
    u32 csize = ss << spc_shift;
    u32 bm_bytes = (nclus + 7) / 8, bm_cl = (bm_bytes + csize - 1) / csize;
    u32 up_bytes = upcase_build(NULL), up_cl = (up_bytes + csize - 1) / csize;
    if ((u64)bm_cl + up_cl + 1 + 1 > nclus || fat_off + fat_len > 0xFFFFFFFFull || heap > 0xFFFFFFFFull)
        return E_NOSPC;
    u32 bm_first = 2, up_first = bm_first + bm_cl, root = up_first + up_cl, used = bm_cl + up_cl + 1;

    u32 zsz = MAX(64u * 1024, ss);
    u8 *z = kzalloc(zsz), *buf = kzalloc(12u * ss);
    u8 *upt = kmalloc(up_bytes);
    int r = 0;
    if (!z || !buf || !upt) { r = E_NOMEM; goto out; }
    upcase_build(upt);
    u32 up_sum = table_checksum(upt, up_bytes);

    /* invalidate an old boot sector first: a half-formatted volume must not mount */
    if ((r = blk_write(bd, 0, 1, z))) goto out;
    /* FAT: media + reserved entries, then chains for bitmap, up-case, root */
    {
        u64 done = 0;
        u32 per = zsz >> ss_shift;
        u32 c = 0;
        u32 total_ent = (u32)MIN((u64)(fat_len << ss_shift) / 4, (u64)nclus + 2);
        while (done < fat_len) {
            u32 k = (u32)MIN((u64)per, fat_len - done);
            memset(z, 0, zsz);
            u32 first_ent = (u32)((done << ss_shift) / 4), nent = (k << ss_shift) / 4;
            for (u32 i = 0; i < nent; i++) {
                c = first_ent + i;
                if (c >= total_ent || c >= used + 2) break;
                u32 val;
                if (c == 0) val = 0xFFFFFFF8u;
                else if (c == 1) val = 0xFFFFFFFFu;
                else if (c == up_first - 1 || c == root - 1 || c == root) val = EOC;
                else val = c + 1;
                wr32(z + 4 * i, val);
            }
            if ((r = blk_write(bd, fat_off + done, k, z))) goto out;
            done += k;
        }
        memset(z, 0, zsz);
    }
    /* bitmap: the first `used` clusters */
    {
        u64 secs = (u64)bm_cl << spc_shift;
        u64 sec0 = heap;
        u32 per = zsz >> ss_shift;
        for (u64 done = 0; done < secs;) {
            u32 k = (u32)MIN((u64)per, secs - done);
            memset(z, 0, zsz);
            u64 byte0 = done << ss_shift;
            for (u32 b = 0; b < used; b++) {
                u64 by = b / 8;
                if (by >= byte0 && by < byte0 + ((u64)k << ss_shift)) z[by - byte0] |= (u8)(1u << (b & 7));
            }
            if ((r = blk_write(bd, sec0 + done, k, z))) goto out;
            done += k;
        }
        memset(z, 0, zsz);
    }
    /* up-case table */
    {
        u64 sec0 = heap + ((u64)(up_first - 2) << spc_shift);
        u64 secs = ((u64)up_bytes + ss - 1) >> ss_shift;
        u8 *tmp = kzalloc((size_t)secs << ss_shift);
        if (!tmp) { r = E_NOMEM; goto out; }
        memcpy(tmp, upt, up_bytes);
        r = fmt_write(bd, sec0, secs, tmp, z, zsz);
        kfree(tmp);
        if (r) goto out;
        u64 rest = ((u64)up_cl << spc_shift) - secs;
        if (rest && (r = fmt_write(bd, sec0 + secs, rest, NULL, z, zsz))) goto out;
    }
    /* root directory: label, bitmap, up-case table */
    {
        u8 *d = buf;
        memset(d, 0, ss);
        d[0] = nlab ? ET_LABEL : (ET_LABEL & ~ET_INUSE);
        d[1] = (u8)nlab;
        for (int i = 0; i < nlab; i++) wr16(d + 2 + 2 * i, lab[i]);
        d[32] = ET_BITMAP;
        wr32(d + 32 + 20, bm_first);
        wr64(d + 32 + 24, bm_bytes);
        d[64] = ET_UPCASE;
        wr32(d + 64 + 4, up_sum);
        wr32(d + 64 + 20, up_first);
        wr64(d + 64 + 24, up_bytes);
        u64 rs = heap + ((u64)(root - 2) << spc_shift);
        if ((r = blk_write(bd, rs, 1, d))) goto out;
        if (spc > 1 && (r = fmt_write(bd, rs + 1, spc - 1, NULL, z, zsz))) goto out;
    }
    /* boot regions: backup first, the main boot sector last */
    {
        u8 *b = buf;
        memset(b, 0, 12u * ss);
        b[0] = 0xEB; b[1] = 0x76; b[2] = 0x90;
        memcpy(b + 3, "EXFAT   ", 8);
        wr64(b + 64, bd->type == BDEV_PART ? bd->part_start : 0);
        wr64(b + 72, vol);
        wr32(b + 80, (u32)fat_off);
        wr32(b + 84, (u32)fat_len);
        wr32(b + 88, (u32)heap);
        wr32(b + 92, nclus);
        wr32(b + 96, root);
        struct rtc_time t;
        rtc_read(&t);
        u32 serial = ((u32)t.year << 20) ^ ((u32)t.month << 16) ^ ((u32)t.day << 11) ^ ((u32)t.hour << 6) ^
                     ((u32)t.minute << 12) ^ (u32)t.second ^ (u32)(vol * 2654435761u) ^ (u32)bd->disk_sig;
        wr32(b + 100, serial);
        wr16(b + 104, 0x0100);
        wr16(b + 106, 0);
        b[108] = (u8)ss_shift;
        b[109] = (u8)spc_shift;
        b[110] = 1;
        b[111] = 0x80;
        b[112] = (u8)(((u64)used * 100) / nclus);
        memset(b + 120, 0xF4, 390);             /* boot code: hlt */
        b[510] = 0x55; b[511] = 0xAA;
        for (int s = 1; s <= 8; s++) { b[(s + 1) * ss - 2] = 0x55; b[(s + 1) * ss - 1] = 0xAA; }
        u32 sum = boot_checksum(b, ss);
        for (u32 i = 0; i < ss; i += 4) wr32(b + 11 * ss + i, sum);
        if ((r = blk_write(bd, 12, 12, b))) goto out;
        if ((r = blk_write(bd, 1, 11, b + ss))) goto out;
        if ((r = blk_write(bd, 0, 1, b))) goto out;
    }
    kprintf("exfat: %s: formatted, %u clusters of %u bytes\n", bd->name, nclus, csize);
out:
    kfree(z);
    kfree(buf);
    kfree(upt);
    return r;
}
