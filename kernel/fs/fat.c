/* Halcyon OS - FAT12 / FAT16 / FAT32 file system driver
 *
 * Read and write support including long file names (VFAT).
 *
 * Design notes
 *  - All sector numbers inside the driver are file system sectors (BPB
 *    bytes/sector). They are converted to device LBAs with f->shift, so a
 *    4096-byte-sector file system works on 512- or 4096-byte devices.
 *  - FAT and directory sectors go through a small write-back LRU cache.
 *    Dirty sectors are flushed at the end of every mutating operation
 *    (FAT sectors first, mirrored to every FAT copy, then directory sectors
 *    and FSInfo), so the volume is consistent between operations.
 *  - File data bypasses the cache: runs of contiguous clusters are read or
 *    written with one request straight to/from the caller's buffer; only
 *    partial sectors use a bounce buffer. Direct writes invalidate cache
 *    entries for the sectors they overwrite.
 *  - Open files are represented by a shared node per directory entry, so
 *    several handles on the same file see the same size / cluster chain.
 */
#include "core/types.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/unicode.h"
#include "fs/block.h"
#include "fs/vfs.h"
#include "fs/fat.h"

#define CACHE_N         32
#define RUN_N           8           /* FAT readahead / merged writeback, sectors */
#define MAX_IO_BYTES    (1024u * 1024u)
#define ZERO_BUF_MAX    (64u * 1024u)
#define DIR_MAX_ENTRIES 65536u
#define LFN_MAX_SLOTS   20          /* 20 * 13 = 260 >= 255 chars */
#define FREE_UNKNOWN    0xFFFFFFFFu

#define ATTR_RO     0x01
#define ATTR_HIDDEN 0x02
#define ATTR_SYSTEM 0x04
#define ATTR_VOLUME 0x08
#define ATTR_DIR    0x10
#define ATTR_ARCH   0x20
#define ATTR_LFN    0x0F

typedef struct {
    u32 sec;
    u32 stamp;
    u8  valid, dirty;
    u8 *buf;
} cent_t;

typedef struct fnode {
    struct fnode *next;
    u32 refs;
    u32 ent_sec;            /* location of the short directory entry */
    u16 ent_off;
    u32 dir;                /* containing directory id */
    u32 first;              /* first cluster (0 = empty file) */
    u32 size;
    u8  attr;
    u32 hint_idx, hint_clus;   /* chain position cache (hint_clus 0 = none) */
} fnode_t;

typedef struct fat {
    volume_t   *v;
    blockdev_t *bd;
    int  type;                  /* 12, 16, 32 */
    u32  bps, spc, csize, shift;
    u32  nfats, fat_base, fat_start, fat_size;
    u8   mirror;
    u8   ro;
    u8   fsinfo_dirty;
    u32  root_start, root_sectors, root_entries, root_clus;
    u32  data_start, nclusters, total_sectors;
    u32  fsinfo_sec;
    u32  free_count, next_free;
    u32  eoc_min, eoc_mark;
    cent_t cache[CACHE_N];
    u8  *cache_mem;
    u32  stamp;
    u8  *bounce;                /* one sector */
    u8  *ra_buf, *wb_buf;       /* RUN_N sectors each */
    u8  *zero;                  /* zero_size bytes of zeros (lazily) */
    u32  zero_size;
    fnode_t *nodes;
} fat_t;

/* a directory entry found by a scan */
typedef struct {
    u8  de[32];                 /* copy of the short entry */
    u32 dir;                    /* containing directory id */
    u32 sec;                    /* short entry location */
    u16 off;
    u8  nslots;                 /* LFN slots + short entry */
    u8  has_lfn;
    u8  is_dot;
    u32 slot_sec[LFN_MAX_SLOTS + 1];
    u16 slot_off[LFN_MAX_SLOTS + 1];
    char name[VFS_MAX_NAME + 1];   /* long name, or 8.3 display name */
    char sname[13];                /* 8.3 name as stored ("README.TXT") */
} fent_t;

static const fs_ops_t fat_ops;

/* ------------------------------------------------------------------ */
/* little helpers                                                       */

static inline u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static inline u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static inline void wr16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static inline void wr32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static inline int is_pow2(u32 v) { return v && !(v & (v - 1)); }

static inline u32 root_id(fat_t *f) { return f->type == 32 ? f->root_clus : 0; }
static inline int is_fixed_root(fat_t *f, u32 dir) { return f->type != 32 && dir == 0; }
static inline int clus_ok(fat_t *f, u32 c) { return c >= 2 && c < f->nclusters + 2; }
static inline u32 clus_sec(fat_t *f, u32 c) { return f->data_start + (c - 2) * f->spc; }

static u32 ent_clus(fat_t *f, const u8 *de) {
    u32 c = rd16(de + 26);
    if (f->type == 32) c |= (u32)rd16(de + 20) << 16;
    return c;
}
static void ent_set_clus(u8 *de, u32 c) { wr16(de + 26, (u16)c); wr16(de + 20, (u16)(c >> 16)); }

/* directory id of a subdirectory entry / ".." value */
static u32 dir_id(fat_t *f, u32 clus) { return clus == 0 ? root_id(f) : clus; }

static void fat_now(u16 *time, u16 *date, u8 *tenths) {
    struct rtc_time t;
    rtc_read(&t);
    if (t.year < 1980) t.year = 1980;
    if (t.year > 2107) t.year = 2107;
    if (t.month < 1 || t.month > 12) t.month = 1;
    if (t.day < 1 || t.day > 31) t.day = 1;
    if (t.hour < 0 || t.hour > 23) t.hour = 0;
    if (t.minute < 0 || t.minute > 59) t.minute = 0;
    if (t.second < 0 || t.second > 59) t.second = 0;
    *time = (u16)((t.hour << 11) | (t.minute << 5) | (t.second / 2));
    *date = (u16)(((t.year - 1980) << 9) | (t.month << 5) | t.day);
    if (tenths) *tenths = (u8)((t.second & 1) * 100);
}

static void fat_to_vtime(u16 time, u16 date, vfs_time_t *o) {
    o->year = (u16)(1980 + (date >> 9));
    o->month = (date >> 5) & 15;
    o->day = date & 31;
    o->hour = time >> 11;
    o->minute = (time >> 5) & 63;
    o->second = (u8)((time & 31) * 2);
}

/* ------------------------------------------------------------------ */
/* device I/O                                                           */

static int dev_read(fat_t *f, u32 sec, u32 n, void *buf) {
    return blk_read(f->bd, (u64)sec << f->shift, n << f->shift, buf);
}
static int dev_write(fat_t *f, u32 sec, u32 n, const void *buf) {
    return blk_write(f->bd, (u64)sec << f->shift, n << f->shift, buf);
}

/* ------------------------------------------------------------------ */
/* metadata sector cache                                                */

static inline int sec_is_fat(fat_t *f, u32 sec) {
    return sec >= f->fat_start && sec < f->fat_start + f->fat_size;
}

/* write a run of n consecutive sectors starting at sec (FAT sectors go to
 * every FAT copy) */
static int write_run(fat_t *f, u32 sec, u32 n, const u8 *buf) {
    if (sec_is_fat(f, sec) && f->mirror) {
        int err = 0;
        u32 rel = sec - f->fat_start;
        for (u32 i = 0; i < f->nfats; i++) {
            int r = dev_write(f, f->fat_base + i * f->fat_size + rel, n, buf);
            if (r && !err) err = r;
        }
        return err;
    }
    return dev_write(f, sec, n, buf);
}

/* write back the dirty FAT (fat_pass) or non-FAT sectors, merging runs of
 * consecutive sectors into one request */
static int flush_pass(fat_t *f, int fat_pass) {
    cent_t *list[CACHE_N];
    int n = 0, err = 0;
    for (int i = 0; i < CACHE_N; i++) {
        cent_t *c = &f->cache[i];
        if (c->valid && c->dirty && (sec_is_fat(f, c->sec) != 0) == fat_pass) {
            int k = n++;
            while (k > 0 && list[k - 1]->sec > c->sec) { list[k] = list[k - 1]; k--; }
            list[k] = c;
        }
    }
    for (int i = 0; i < n;) {
        int j = i + 1;
        while (j < n && j - i < RUN_N && list[j]->sec == list[j - 1]->sec + 1 &&
               sec_is_fat(f, list[j]->sec) == sec_is_fat(f, list[i]->sec)) j++;
        const u8 *buf = list[i]->buf;
        if (j - i > 1) {
            for (int k = i; k < j; k++) memcpy(f->wb_buf + (size_t)(k - i) * f->bps, list[k]->buf, f->bps);
            buf = f->wb_buf;
        }
        int r = write_run(f, list[i]->sec, (u32)(j - i), buf);
        if (r) { if (!err) err = r; }
        else for (int k = i; k < j; k++) list[k]->dirty = 0;
        i = j;
    }
    return err;
}

/* all dirty sectors: FAT first, then directories / FSInfo */
static int flush_dirty(fat_t *f) {
    int r1 = flush_pass(f, 1);
    int r2 = flush_pass(f, 0);
    return r1 ? r1 : r2;
}

static cent_t *cache_find(fat_t *f, u32 sec) {
    for (int i = 0; i < CACHE_N; i++)
        if (f->cache[i].valid && f->cache[i].sec == sec) return &f->cache[i];
    return NULL;
}

/* least recently used entry, made clean (writing back if needed) */
static cent_t *cache_victim(fat_t *f, int *err) {
    cent_t *victim = NULL;
    for (int i = 0; i < CACHE_N; i++) {
        cent_t *c = &f->cache[i];
        if (!c->valid) { victim = c; break; }
        if (!victim || c->stamp < victim->stamp) victim = c;
    }
    if (victim->valid && victim->dirty) {
        /* write back everything at once: merged requests, FAT before dirs */
        int r = flush_dirty(f);
        if (r) { *err = r; return NULL; }
    }
    victim->valid = 0;
    return victim;
}

/* Get a cached sector. With noread the buffer is zero-filled instead of
 * read (caller is about to overwrite all of it). FAT misses read ahead a
 * few sectors in one request. */
static cent_t *cget(fat_t *f, u32 sec, int noread, int *err) {
    cent_t *c = cache_find(f, sec);
    if (c) {
        c->stamp = ++f->stamp;
        return c;
    }
    if (!noread && sec_is_fat(f, sec)) {
        u32 n = MIN((u32)RUN_N, f->fat_start + f->fat_size - sec);
        /* sectors cached now may be dirty: the disk copy we read is stale
         * for them, even if they get written back and evicted below */
        u32 cached = 0;
        for (u32 i = 0; i < n; i++) if (cache_find(f, sec + i)) cached |= 1u << i;
        if (n > 1 && dev_read(f, sec, n, f->ra_buf) == 0) {
            /* insert backwards so that sec ends up most recently used */
            for (u32 i = n; i-- > 0;) {
                if ((cached & (1u << i)) || cache_find(f, sec + i)) continue;
                cent_t *v = cache_victim(f, err);
                if (!v) { if (i == 0) return NULL; continue; }
                memcpy(v->buf, f->ra_buf + (size_t)i * f->bps, f->bps);
                v->sec = sec + i;
                v->valid = 1;
                v->dirty = 0;
                v->stamp = ++f->stamp;
            }
            c = cache_find(f, sec);
            if (c) return c;
        }
    }
    c = cache_victim(f, err);
    if (!c) return NULL;
    if (noread) {
        memset(c->buf, 0, f->bps);
    } else {
        int r = dev_read(f, sec, 1, c->buf);
        if (r) { *err = r; return NULL; }
    }
    c->sec = sec;
    c->valid = 1;
    c->dirty = 0;
    c->stamp = ++f->stamp;
    return c;
}

static void cache_inval(fat_t *f, u32 sec, u32 n) {
    for (int i = 0; i < CACHE_N; i++) {
        cent_t *c = &f->cache[i];
        if (c->valid && c->sec >= sec && c->sec - sec < n) c->valid = c->dirty = 0;
    }
}

static int fsinfo_store(fat_t *f) {
    if (f->type != 32 || !f->fsinfo_sec || !f->fsinfo_dirty || f->ro) return 0;
    int err = 0;
    cent_t *c = cget(f, f->fsinfo_sec, 0, &err);
    if (!c) return err;
    u8 *b = c->buf;
    if (rd32(b) == 0x41615252 && rd32(b + 484) == 0x61417272) {
        wr32(b + 488, f->free_count);
        wr32(b + 492, clus_ok(f, f->next_free) ? f->next_free : FREE_UNKNOWN);
        c->dirty = 1;
    }
    f->fsinfo_dirty = 0;
    return 0;
}

/* write every dirty sector: FAT first, then directories and FSInfo */
static int fat_flush(fat_t *f) {
    int err = fsinfo_store(f);
    int r = flush_dirty(f);
    return err ? err : r;
}

/* ------------------------------------------------------------------ */
/* FAT table                                                            */

static int fat_byte_ptr(fat_t *f, u32 off, u8 **p, cent_t **ce) {
    int err = 0;
    cent_t *c = cget(f, f->fat_start + off / f->bps, 0, &err);
    if (!c) return err ? err : E_IO;
    *p = c->buf + off % f->bps;
    *ce = c;
    return 0;
}

static int fat_get(fat_t *f, u32 clus, u32 *val) {
    if (!clus_ok(f, clus)) return E_IO;
    u8 *p = NULL; cent_t *c = NULL; int r;
    switch (f->type) {
    case 12: {
        u32 off = clus + clus / 2;
        if ((r = fat_byte_ptr(f, off, &p, &c))) return r;
        u32 v = *p;
        if ((r = fat_byte_ptr(f, off + 1, &p, &c))) return r;
        v |= (u32)*p << 8;
        *val = (clus & 1) ? v >> 4 : v & 0xFFF;
        return 0;
    }
    case 16:
        if ((r = fat_byte_ptr(f, clus * 2, &p, &c))) return r;
        *val = rd16(p);
        return 0;
    default:
        if ((r = fat_byte_ptr(f, clus * 4, &p, &c))) return r;
        *val = rd32(p) & 0x0FFFFFFF;
        return 0;
    }
}

static int fat_set(fat_t *f, u32 clus, u32 val) {
    if (!clus_ok(f, clus)) return E_IO;
    u8 *p = NULL; cent_t *c = NULL; int r;
    switch (f->type) {
    case 12: {
        u32 off = clus + clus / 2;
        if ((r = fat_byte_ptr(f, off, &p, &c))) return r;
        if (clus & 1) *p = (u8)((*p & 0x0F) | ((val << 4) & 0xF0));
        else *p = (u8)val;
        c->dirty = 1;
        if ((r = fat_byte_ptr(f, off + 1, &p, &c))) return r;
        if (clus & 1) *p = (u8)(val >> 4);
        else *p = (u8)((*p & 0xF0) | ((val >> 8) & 0x0F));
        c->dirty = 1;
        return 0;
    }
    case 16:
        if ((r = fat_byte_ptr(f, clus * 2, &p, &c))) return r;
        wr16(p, (u16)val);
        c->dirty = 1;
        return 0;
    default:
        if ((r = fat_byte_ptr(f, clus * 4, &p, &c))) return r;
        wr32(p, (rd32(p) & 0xF0000000) | (val & 0x0FFFFFFF));
        c->dirty = 1;
        return 0;
    }
}

static inline int is_eoc(fat_t *f, u32 v) { return v >= f->eoc_min; }

/* next cluster in a chain: 0 = ok (*next valid), 1 = end of chain */
static int chain_next(fat_t *f, u32 c, u32 *next) {
    u32 v;
    int r = fat_get(f, c, &v);
    if (r) return r;
    if (is_eoc(f, v)) return 1;
    if (!clus_ok(f, v)) {
        kprintf("fat: %s: corrupt chain at cluster %u (-> %x)\n", f->bd->name, c, v);
        return E_IO;
    }
    *next = v;
    return 0;
}

static int clus_alloc(fat_t *f, u32 near, u32 *out) {
    u32 n = f->nclusters;
    u32 start = clus_ok(f, near) ? near : (clus_ok(f, f->next_free) ? f->next_free : 2);
    for (u32 i = 0; i < n; i++) {
        u32 c = start + i;
        if (c >= n + 2) c -= n;
        u32 v;
        int r = fat_get(f, c, &v);
        if (r) return r;
        if (v) continue;
        if ((r = fat_set(f, c, f->eoc_mark))) return r;
        if (f->free_count != FREE_UNKNOWN && f->free_count) f->free_count--;
        f->next_free = c + 1 < n + 2 ? c + 1 : 2;
        f->fsinfo_dirty = 1;
        *out = c;
        return 0;
    }
    f->free_count = 0;
    f->fsinfo_dirty = 1;
    return E_NOSPC;
}

static int chain_free(fat_t *f, u32 c) {
    for (u32 steps = 0; clus_ok(f, c) && steps <= f->nclusters; steps++) {
        u32 v;
        int r = fat_get(f, c, &v);
        if (r) return r;
        if (!v) break;                   /* already free: corrupt, stop */
        if ((r = fat_set(f, c, 0))) return r;
        if (f->free_count != FREE_UNKNOWN) f->free_count++;
        f->fsinfo_dirty = 1;
        if (is_eoc(f, v)) break;
        c = v;
    }
    return 0;
}

static int zero_buf(fat_t *f) {
    if (f->zero) return 0;
    u32 sz = f->csize < ZERO_BUF_MAX ? f->csize : ZERO_BUF_MAX;
    if (sz < f->bps) sz = f->bps;
    f->zero = kzalloc(sz);
    if (!f->zero) return E_NOMEM;
    f->zero_size = sz;
    return 0;
}

static int clus_zero(fat_t *f, u32 c) {
    int r = zero_buf(f);
    if (r) return r;
    u32 sec = clus_sec(f, c), per = f->zero_size / f->bps;
    for (u32 done = 0; done < f->spc; done += per) {
        u32 n = MIN(per, f->spc - done);
        if ((r = dev_write(f, sec + done, n, f->zero))) return r;
    }
    cache_inval(f, sec, f->spc);
    return 0;
}

static int count_free(fat_t *f) {
    u32 bytes = f->type == 12 ? ((f->nclusters + 2) * 3 + 1) / 2 : (f->nclusters + 2) * (f->type / 8);
    u32 chunk = f->type == 12 ? ALIGN_UP(bytes, f->bps) : 64 * 1024;
    if (chunk < f->bps) chunk = f->bps;
    u8 *buf = kmalloc(chunk);
    if (!buf) return E_NOMEM;
    int r = fat_flush(f);
    u32 free = 0;
    u32 esz = f->type / 8;   /* FAT16/32 */
    for (u32 off = 0; !r && off < bytes; off += chunk) {
        u32 len = MIN(chunk, ALIGN_UP(bytes - off, f->bps));
        if ((r = dev_read(f, f->fat_start + off / f->bps, len / f->bps, buf))) break;
        if (f->type == 12) {
            for (u32 c = 2; c < f->nclusters + 2; c++) {
                u32 o = c + c / 2, v = buf[o] | (buf[o + 1] << 8);
                v = (c & 1) ? v >> 4 : v & 0xFFF;
                if (!v) free++;
            }
        } else {
            u32 first = off / esz, cnt = len / esz;
            for (u32 i = 0; i < cnt; i++) {
                u32 c = first + i;
                if (c < 2 || c >= f->nclusters + 2) continue;
                u32 v = f->type == 16 ? rd16(buf + i * 2) : (rd32(buf + i * 4) & 0x0FFFFFFF);
                if (!v) free++;
            }
        }
    }
    kfree(buf);
    if (r) return r;
    f->free_count = free;
    f->fsinfo_dirty = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* directory iteration                                                  */

typedef struct { u32 start, clus, idx, sec; } diter_t;

static int di_start(fat_t *f, diter_t *d, u32 dir) {
    d->start = dir;
    d->idx = 0;
    if (is_fixed_root(f, dir)) {
        d->clus = 0;
        d->sec = f->root_start;
        return f->root_entries ? 0 : 1;
    }
    if (!clus_ok(f, dir)) return E_IO;
    d->clus = dir;
    d->sec = clus_sec(f, dir);
    return 0;
}

static inline u32 di_off(fat_t *f, diter_t *d) { return (d->idx * 32) % f->bps; }

/* 0 = advanced, 1 = end of directory (d->clus stays the last cluster) */
static int di_next(fat_t *f, diter_t *d) {
    u32 nidx = d->idx + 1;
    if (is_fixed_root(f, d->start)) {
        if (nidx >= f->root_entries) return 1;
        d->idx = nidx;
        if ((nidx * 32) % f->bps == 0) d->sec++;
        return 0;
    }
    if (nidx >= DIR_MAX_ENTRIES) return 1;
    if ((nidx * 32) % f->csize == 0) {
        u32 nx;
        int r = chain_next(f, d->clus, &nx);
        if (r) return r;
        d->clus = nx;
        d->sec = clus_sec(f, nx);
    } else if ((nidx * 32) % f->bps == 0) {
        d->sec++;
    }
    d->idx = nidx;
    return 0;
}

static u8 lfn_checksum(const u8 *name11) {
    u8 s = 0;
    for (int i = 0; i < 11; i++) s = (u8)(((s & 1) << 7) + (s >> 1) + name11[i]);
    return s;
}

static const u8 lfn_pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

/* bytes that must not appear in a name we hand out: they would make it more
 * than one path component (or name a drive) */
static int name_char_bad(u32 c) { return c < 0x20 || c == '/' || c == '\\' || c == ':'; }

/* one short-name byte: sname gets it as stored ('_' for OEM characters, so
 * it stays ASCII), the display name gets it as UTF-8 (code page 437) */
static void short_char_out(u8 c, int lower, char *sname, int *n, char *disp, int *m) {
    if (name_char_bad(c)) c = '_';
    sname[(*n)++] = c >= 0x80 ? '_' : (char)c;
    if (c >= 0x80) *m += utf8_put(cp437_to_uc(c), disp + *m);
    else disp[(*m)++] = (char)(lower ? tolower(c) : c);
}

static void short_display(const u8 *de, char *sname, char *disp) {
    int n = 0, m = 0;
    u8 nt = de[12];
    for (int i = 0; i < 8 && de[i] != ' '; i++)
        short_char_out(i == 0 && de[0] == 0x05 ? 0xE5 : de[i], nt & 0x08, sname, &n, disp, &m);
    if (!n) { sname[n++] = '_'; disp[m++] = '_'; }     /* corrupt: blank base name */
    if (de[8] != ' ') {
        sname[n++] = '.'; disp[m++] = '.';
        for (int i = 8; i < 11 && de[i] != ' '; i++) short_char_out(de[i], nt & 0x10, sname, &n, disp, &m);
    }
    sname[n] = 0;
    disp[m] = 0;
}

/* scan callback: 0 = continue, 1 = stop (entry in *e), <0 = error */
typedef int (*scan_cb)(fat_t *f, fent_t *e, void *ctx);

static int dir_scan(fat_t *f, u32 dir, scan_cb cb, void *ctx, fent_t *e) {
    u16 lfn[LFN_MAX_SLOTS * 13 + 1];
    int lfn_ok = 0, lfn_expect = 0, nslots = 0;
    u8 lfn_chk = 0;
    diter_t d;
    int r = di_start(f, &d, dir);
    if (r) return r < 0 ? r : 0;
    for (;;) {
        int err = 0;
        cent_t *c = cget(f, d.sec, 0, &err);
        if (!c) return err;
        const u8 *p = c->buf + di_off(f, &d);
        if (p[0] == 0) break;
        if (p[0] == 0xE5) {
            lfn_ok = 0;
        } else if ((p[11] & 0x3F) == ATTR_LFN) {
            u8 ord = p[0];
            if (ord & 0x40) {
                int n = ord & 0x1F;
                if (n < 1 || n > LFN_MAX_SLOTS) { lfn_ok = 0; goto next; }
                lfn_ok = 1;
                lfn_expect = n;
                lfn_chk = p[13];
                nslots = 0;
                for (int i = 0; i <= LFN_MAX_SLOTS * 13; i++) lfn[i] = 0;
            } else if (!lfn_ok || lfn_expect < 2 || (ord & 0x1F) != lfn_expect - 1 || p[13] != lfn_chk) {
                lfn_ok = 0;
                goto next;
            } else {
                lfn_expect--;
            }
            int base = (lfn_expect - 1) * 13;
            for (int i = 0; i < 13; i++) lfn[base + i] = rd16(p + lfn_pos[i]);
            e->slot_sec[nslots] = d.sec;
            e->slot_off[nslots] = (u16)di_off(f, &d);
            nslots++;
        } else if (p[11] & ATTR_VOLUME) {
            lfn_ok = 0;
        } else {
            memcpy(e->de, p, 32);
            e->dir = dir;
            e->sec = d.sec;
            e->off = (u16)di_off(f, &d);
            e->is_dot = p[0] == '.';
            short_display(p, e->sname, e->name);
            e->has_lfn = 0;
            if (lfn_ok && lfn_expect == 1 && lfn_checksum(p) == lfn_chk && !e->is_dot) {
                /* UTF-16 -> UTF-8; names that do not convert (unpaired
                 * surrogates, control characters, > 255 bytes) or that hold
                 * path characters fall back to the short name */
                char tmp[VFS_MAX_NAME + 1];
                int units = 0, bad = 0, dots = 0;
                while (units < LFN_MAX_SLOTS * 13 && lfn[units] && lfn[units] != 0xFFFF) units++;
                int n = utf16_to_utf8(lfn, units, tmp, sizeof tmp);
                if (n < 0) { bad = 1; n = 0; }
                for (int i = 0; i < n; i++) {
                    if (name_char_bad((u8)tmp[i])) bad = 1;
                    dots += tmp[i] == '.';
                }
                if (dots == n) bad = 1;          /* "", ".", "..": not a usable name */
                if (!bad && n) {
                    memcpy(e->name, tmp, n + 1);
                    e->has_lfn = 1;
                }
            }
            if (lfn_ok && lfn_expect == 1 && lfn_checksum(p) == lfn_chk && !e->is_dot) {
                /* slots belong to this entry even if the name was not representable */
                e->nslots = (u8)(nslots + 1);
            } else {
                e->nslots = 1;
                nslots = 0;
            }
            e->slot_sec[e->nslots - 1] = d.sec;
            e->slot_off[e->nslots - 1] = e->off;
            lfn_ok = 0;
            r = cb(f, e, ctx);
            if (r) return r;
        }
    next:
        r = di_next(f, &d);
        if (r == 1) break;
        if (r < 0) return r;
    }
    return 0;
}

static int lookup_cb(fat_t *f, fent_t *e, void *ctx) {
    const char *name = ctx;
    if (e->is_dot) return 0;
    return !utf8_casecmp(e->name, name) || !strcasecmp(e->sname, name);
}

/* find name in dir: 0 = found (in *e), E_NOENT, or error */
static int dir_lookup(fat_t *f, u32 dir, const char *name, fent_t *e) {
    int r = dir_scan(f, dir, lookup_cb, (void *)name, e);
    if (r == 1) return 0;
    return r < 0 ? r : E_NOENT;
}

/* ------------------------------------------------------------------ */
/* names                                                                */

/* copy one path component, stripping trailing spaces and dots */
static int get_comp(const char *s, size_t len, char *out) {
    while (len && (s[len - 1] == ' ' || s[len - 1] == '.')) len--;
    if (!len) return E_INVAL;
    if (len > VFS_MAX_NAME) return E_NAMETOOLONG;
    memcpy(out, s, len);
    out[len] = 0;
    return 0;
}

static int name_valid(const char *n) {
    size_t len = strlen(n);
    if (!len) return E_INVAL;
    if (len > VFS_MAX_NAME) return E_NAMETOOLONG;
    for (size_t i = 0; i < len; i++) {
        u8 c = (u8)n[i];
        if (c < 0x20 || c == 0x7F || (c < 0x80 && strchr("\"*/:<>?\\|", c))) return E_INVAL;
    }
    /* UTF-8, at most 255 UTF-16 units in the long name */
    u16 tmp[VFS_MAX_NAME + 1];
    if (!utf8_valid(n)) return E_INVAL;
    if (utf8_to_utf16(n, tmp, VFS_MAX_NAME) < 0) return E_NAMETOOLONG;
    return 0;
}

static int short_char(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && strchr("!#$%&'()-@^_`{}~", c));
}

/* is n already a valid uppercase 8.3 name? fills out[11] */
static int exact_83(const char *n, u8 *out) {
    memset(out, ' ', 11);
    int i = 0, bi = 0;
    for (; n[i] && n[i] != '.'; i++) {
        if (bi >= 8 || !short_char((u8)n[i])) return 0;
        out[bi++] = (u8)n[i];
    }
    if (!bi) return 0;
    if (n[i] == '.') {
        i++;
        int ei = 0;
        for (; n[i]; i++) {
            if (ei >= 3 || !short_char((u8)n[i])) return 0;
            out[8 + ei++] = (u8)n[i];
        }
        if (!ei) return 0;
    }
    if (out[0] == 0xE5) out[0] = 0x05;
    return 1;
}

/* Microsoft "basis name" generation; returns lossy flag, base length in *blen */
static int basis_name(const char *n, u8 *out, int *blen) {
    int lossy = 0, bi = 0, ei = 0;
    memset(out, ' ', 11);
    const char *s = n;
    while (*s == '.' || *s == ' ') { s++; lossy = 1; }
    const char *dot = strrchr(s, '.');
    /* one '_' per non-ASCII character (no OEM code page conversion) */
    for (const char *p = s; *p && p != dot;) {
        u32 u = utf8_next(&p);
        int c = u < 0x80 ? toupper((int)u) : '_';
        if (c == ' ' || c == '.') { lossy = 1; continue; }
        if (!short_char(c)) { c = '_'; lossy = 1; }
        if (bi < 8) out[bi++] = (u8)c; else lossy = 1;
    }
    if (dot) {
        for (const char *p = dot + 1; *p;) {
            u32 u = utf8_next(&p);
            int c = u < 0x80 ? toupper((int)u) : '_';
            if (c == ' ') { lossy = 1; continue; }
            if (!short_char(c)) { c = '_'; lossy = 1; }
            if (ei < 3) out[8 + ei++] = (u8)c; else lossy = 1;
        }
    }
    if (!bi) { out[0] = '_'; bi = 1; lossy = 1; }
    *blen = bi;
    return lossy;
}

typedef struct {
    const u8 *basis;
    int blen;
    int basis_used;
    u8 *used;           /* bitmap of numeric tails in use */
    u32 used_max;
} alias_ctx_t;

static int ndigits(u32 k) { int d = 1; while (k >= 10) { k /= 10; d++; } return d; }

static int alias_cb(fat_t *f, fent_t *e, void *vctx) {
    alias_ctx_t *a = vctx;
    const u8 *nm = e->de;
    if (e->is_dot) return 0;
    if (!memcmp(nm, a->basis, 11)) a->basis_used = 1;
    if (memcmp(nm + 8, a->basis + 8, 3)) return 0;
    /* parse "PREFIX~NNN" */
    int t = -1;
    for (int i = 0; i < 8; i++) if (nm[i] == '~') { t = i; break; }
    if (t < 1) return 0;
    u32 k = 0; int i = t + 1, nd = 0;
    for (; i < 8 && nm[i] >= '0' && nm[i] <= '9'; i++, nd++) k = k * 10 + (nm[i] - '0');
    if (!nd || (i < 8 && nm[i] != ' ') || k == 0 || k >= a->used_max) return 0;
    if (ndigits(k) != nd) return 0;
    int keep = MIN(a->blen, 7 - nd);
    if (t != keep || memcmp(nm, a->basis, t)) return 0;
    a->used[k >> 3] |= (u8)(1 << (k & 7));
    return 0;
}

/* choose the short name for a new entry; *need_lfn set if a LFN is required */
static int make_short(fat_t *f, u32 dir, const char *name, u8 *out, int *need_lfn) {
    u8 basis[11];
    int blen, lossy;
    int exact = exact_83(name, basis);
    if (exact) {
        lossy = 0;
        for (blen = 8; blen > 0 && basis[blen - 1] == ' '; blen--) ;
    } else {
        lossy = basis_name(name, basis, &blen);
    }
    alias_ctx_t a = { basis, blen, 0, NULL, 8192 };
    a.used = kzalloc(a.used_max / 8);
    fent_t *e = kmalloc(sizeof *e);
    int r = (!a.used || !e) ? E_NOMEM : dir_scan(f, dir, alias_cb, &a, e);
    kfree(e);
    if (r < 0) { kfree(a.used); return r; }
    r = 0;
    *need_lfn = !exact || a.basis_used;
    if (!lossy && !a.basis_used) {
        memcpy(out, basis, 11);
        if (out[0] == 0xE5) out[0] = 0x05;
    } else {
        r = E_NOSPC;
        for (u32 k = 1; k < a.used_max; k++) {
            if (a.used[k >> 3] & (1 << (k & 7))) continue;
            int nd = ndigits(k), keep = MIN(blen, 7 - nd);
            char num[12];
            memcpy(out, basis, 11);
            memset(out + keep, ' ', 8 - keep);
            out[keep] = '~';
            snprintf(num, sizeof num, "%u", k);
            memcpy(out + keep + 1, num, nd);
            r = 0;
            break;
        }
    }
    kfree(a.used);
    return r;
}

/* ------------------------------------------------------------------ */
/* path resolution                                                      */

static int dir_is_empty_cb(fat_t *f, fent_t *e, void *ctx) { return e->is_dot ? 0 : 1; }

/* resolve a path; *isroot set for "" (then *e is untouched) */
static int resolve(fat_t *f, const char *path, fent_t *e, int *isroot) {
    *isroot = 0;
    if (!*path) { *isroot = 1; return 0; }
    u32 cur = root_id(f);
    char comp[VFS_MAX_NAME + 1];
    const char *s = path;
    for (;;) {
        const char *sl = strchr(s, '/');
        size_t len = sl ? (size_t)(sl - s) : strlen(s);
        int r = get_comp(s, len, comp);
        if (r) return r;
        if ((r = dir_lookup(f, cur, comp, e))) return r;
        if (!sl || !sl[1]) return 0;
        if (!(e->de[11] & ATTR_DIR)) return E_NOTDIR;
        u32 c = ent_clus(f, e->de);
        if (!c) return E_IO;
        cur = c;
        s = sl + 1;
    }
}

/* resolve the parent directory of path, return the last component in name */
static int resolve_parent(fat_t *f, const char *path, u32 *dir, char *name) {
    if (!*path) return E_INVAL;
    const char *sl = strrchr(path, '/');
    const char *last = sl ? sl + 1 : path;
    int r = get_comp(last, strlen(last), name);
    if (r) return r;
    if (!sl) { *dir = root_id(f); return 0; }
    size_t plen = (size_t)(sl - path);
    char *parent = kmalloc(plen + 1);
    if (!parent) return E_NOMEM;
    memcpy(parent, path, plen);
    parent[plen] = 0;
    fent_t *e = kmalloc(sizeof *e);
    if (!e) { kfree(parent); return E_NOMEM; }
    int isroot;
    r = resolve(f, parent, e, &isroot);
    if (!r) {
        if (isroot) *dir = root_id(f);
        else if (!(e->de[11] & ATTR_DIR)) r = E_NOTDIR;
        else if (!(*dir = ent_clus(f, e->de))) r = E_IO;
    }
    kfree(e);
    kfree(parent);
    return r;
}

/* ------------------------------------------------------------------ */
/* directory modification                                               */

/* Add an entry named name to dir. tmpl holds the short entry (everything
 * except the name). Returns the location of the short entry. */
static int dir_add(fat_t *f, u32 dir, const char *name, const u8 *tmpl, u32 *osec, u16 *ooff) {
    u8 sn[11];
    int need_lfn;
    int r = make_short(f, dir, name, sn, &need_lfn);
    if (r) return r;
    u16 uname[VFS_MAX_NAME + 1];
    int len = utf8_to_utf16(name, uname, VFS_MAX_NAME);
    if (len < 0) return E_NAMETOOLONG;
    int nlfn = need_lfn ? (len + 12) / 13 : 0;
    int need = nlfn + 1;
    u32 ssec[LFN_MAX_SLOTS + 1];
    u16 soff[LFN_MAX_SLOTS + 1];
    int run = 0;

    diter_t d;
    r = di_start(f, &d, dir);
    if (r < 0) return r;
    if (r == 1) return E_NOSPC;
    for (;;) {
        int err = 0;
        cent_t *c = cget(f, d.sec, 0, &err);
        if (!c) return err;
        u8 b0 = c->buf[di_off(f, &d)];
        if (b0 == 0 || b0 == 0xE5) {
            ssec[run] = d.sec;
            soff[run] = (u16)di_off(f, &d);
            if (++run == need) break;
        } else {
            run = 0;
        }
        r = di_next(f, &d);
        if (r < 0) return r;
        if (r == 1) {
            /* end of directory: grow it */
            if (is_fixed_root(f, dir) || d.idx + 1 >= DIR_MAX_ENTRIES) return E_NOSPC;
            u32 nc;
            if ((r = clus_alloc(f, d.clus + 1, &nc))) return r;
            if ((r = clus_zero(f, nc)) || (r = fat_set(f, d.clus, nc))) {
                chain_free(f, nc);
                return r;
            }
            d.clus = nc;
            d.sec = clus_sec(f, nc);
            d.idx++;
        }
    }

    u8 chk = lfn_checksum(sn);
    for (int j = 0; j < nlfn; j++) {
        int ord = nlfn - j;
        int err = 0;
        cent_t *c = cget(f, ssec[j], 0, &err);
        if (!c) return err;
        u8 *p = c->buf + soff[j];
        memset(p, 0, 32);
        p[0] = (u8)(ord | (j == 0 ? 0x40 : 0));
        p[11] = ATTR_LFN;
        p[13] = chk;
        for (int i = 0; i < 13; i++) {
            int ci = (ord - 1) * 13 + i;
            u16 ch = ci < len ? uname[ci] : ci == len ? 0 : 0xFFFF;
            wr16(p + lfn_pos[i], ch);
        }
        c->dirty = 1;
    }
    int err = 0;
    cent_t *c = cget(f, ssec[nlfn], 0, &err);
    if (!c) return err;
    u8 *p = c->buf + soff[nlfn];
    memcpy(p, tmpl, 32);
    memcpy(p, sn, 11);
    p[12] = 0;
    c->dirty = 1;
    *osec = ssec[nlfn];
    *ooff = soff[nlfn];
    return 0;
}

static int ent_delete(fat_t *f, const fent_t *e) {
    for (int i = 0; i < e->nslots; i++) {
        int err = 0;
        cent_t *c = cget(f, e->slot_sec[i], 0, &err);
        if (!c) return err;
        c->buf[e->slot_off[i]] = 0xE5;
        c->dirty = 1;
    }
    return 0;
}

static void tmpl_new(u8 *de, u8 attr, u32 clus) {
    u16 t, dt; u8 tn;
    memset(de, 0, 32);
    fat_now(&t, &dt, &tn);
    de[11] = attr;
    de[13] = tn;
    wr16(de + 14, t); wr16(de + 16, dt);
    wr16(de + 18, dt);
    wr16(de + 22, t); wr16(de + 24, dt);
    ent_set_clus(de, clus);
}

/* parent directory id of directory cluster c (via its ".." entry) */
static int dir_parent(fat_t *f, u32 c, u32 *parent) {
    int err = 0;
    if (!clus_ok(f, c)) return E_IO;
    cent_t *ce = cget(f, clus_sec(f, c), 0, &err);
    if (!ce) return err;
    const u8 *p = ce->buf + 32;
    if (p[0] != '.' || p[1] != '.') return E_IO;
    *parent = dir_id(f, ent_clus(f, p));
    return 0;
}

/* ------------------------------------------------------------------ */
/* file nodes and data                                                  */

static fnode_t *node_find(fat_t *f, u32 sec, u16 off) {
    for (fnode_t *n = f->nodes; n; n = n->next)
        if (n->ent_sec == sec && n->ent_off == off) return n;
    return NULL;
}

static int node_store(fat_t *f, fnode_t *n, int touch) {
    int err = 0;
    cent_t *c = cget(f, n->ent_sec, 0, &err);
    if (!c) return err;
    u8 *de = c->buf + n->ent_off;
    ent_set_clus(de, n->first);
    wr32(de + 28, n->size);
    if (touch) {
        u16 t, dt;
        fat_now(&t, &dt, NULL);
        wr16(de + 22, t); wr16(de + 24, dt);
        wr16(de + 18, dt);
        de[11] |= ATTR_ARCH;
    }
    c->dirty = 1;
    return 0;
}

static int clus_at(fat_t *f, fnode_t *n, u32 idx, u32 *out) {
    u32 c, i;
    if (n->hint_clus && n->hint_idx <= idx) { c = n->hint_clus; i = n->hint_idx; }
    else { c = n->first; i = 0; }
    if (!clus_ok(f, c) || idx >= f->nclusters) return E_IO;   /* longer than any chain: loop */
    while (i < idx) {
        u32 nx;
        int r = chain_next(f, c, &nx);
        if (r) return r < 0 ? r : E_IO;
        c = nx;
        i++;
    }
    n->hint_idx = idx;
    n->hint_clus = c;
    *out = c;
    return 0;
}

static inline u32 clusters_for(fat_t *f, u64 bytes) { return (u32)((bytes + f->csize - 1) / f->csize); }

/* read (buf != NULL, !write) or write (buf or zeros) file data */
static int data_io(fat_t *f, fnode_t *n, u32 pos, u8 *buf, u32 len, int write) {
    u32 done = 0;
    while (done < len) {
        u32 cidx = pos / f->csize, coff = pos % f->csize;
        u32 c;
        int r = clus_at(f, n, cidx, &c);
        if (r) return r;
        u32 remaining = len - done;
        u32 maxb = (write && !buf) ? f->zero_size : MAX_IO_BYTES;
        u32 run = 1, last = c;
        while ((u64)run * f->csize - coff < remaining && (u64)run * f->csize - coff < maxb) {
            u32 nx;
            r = chain_next(f, last, &nx);
            if (r < 0) return r;
            if (r == 1 || nx != last + 1) break;
            run++;
            last = nx;
        }
        u64 span64 = (u64)run * f->csize - coff;
        u32 span = (u32)MIN(span64, (u64)remaining);
        if (span > maxb) span = maxb;
        u32 sec = clus_sec(f, c) + coff / f->bps, soff = coff % f->bps, nb;
        if (soff || span < f->bps) {
            nb = MIN(f->bps - soff, span);
            if (write) {
                if (nb < f->bps && (r = dev_read(f, sec, 1, f->bounce))) return r;
                if (buf) memcpy(f->bounce + soff, buf + done, nb);
                else memset(f->bounce + soff, 0, nb);
                if ((r = dev_write(f, sec, 1, f->bounce))) return r;
                cache_inval(f, sec, 1);
            } else {
                if ((r = dev_read(f, sec, 1, f->bounce))) return r;
                memcpy(buf + done, f->bounce + soff, nb);
            }
        } else {
            u32 ns = span / f->bps;
            nb = ns * f->bps;
            if (write) {
                if ((r = dev_write(f, sec, ns, buf ? buf + done : f->zero))) return r;
                cache_inval(f, sec, ns);
            } else if ((r = dev_read(f, sec, ns, buf + done))) {
                return r;
            }
        }
        u32 k = (coff + nb - 1) / f->csize;
        n->hint_idx = cidx + k;
        n->hint_clus = c + k;
        pos += nb;
        done += nb;
    }
    return 0;
}

/* cut the chain to keep clusters (does not touch size/dir entry) */
static int chain_trim(fat_t *f, fnode_t *n, u32 keep) {
    int r;
    if (n->hint_clus && n->hint_idx >= keep) n->hint_clus = 0;
    if (!n->first) return 0;
    if (keep == 0) {
        u32 old = n->first;
        n->first = 0;
        n->hint_clus = 0;
        return chain_free(f, old);
    }
    u32 c, nx;
    if ((r = clus_at(f, n, keep - 1, &c))) return r;
    r = chain_next(f, c, &nx);
    if (r < 0) return r;
    if (r == 1) return 0;
    if ((r = fat_set(f, c, f->eoc_mark))) return r;
    return chain_free(f, nx);
}

/* make the chain cover end bytes; *cap = bytes actually covered */
static int node_extend(fat_t *f, fnode_t *n, u64 end, u64 *cap) {
    u32 have = clusters_for(f, n->size);
    u32 need = clusters_for(f, end);
    int r;
    *cap = end;
    if (n->size == 0 && n->first) {
        /* stale chain on an empty file: drop it */
        if ((r = chain_trim(f, n, 0))) return r;
    }
    if (need <= have) return 0;
    u32 last = 0;
    if (have && (r = clus_at(f, n, have - 1, &last))) return r;
    for (u32 i = have; i < need; i++) {
        u32 nc;
        r = clus_alloc(f, last ? last + 1 : 0, &nc);
        if (r) {
            *cap = (u64)i * f->csize;
            return r;
        }
        if (last) r = fat_set(f, last, nc);
        else n->first = nc;
        if (r) { fat_set(f, nc, 0); *cap = (u64)i * f->csize; return r; }
        last = nc;
        n->hint_idx = i;
        n->hint_clus = nc;
    }
    return 0;
}

/* grow file n to new_size with zeros */
static int node_grow(fat_t *f, fnode_t *n, u32 new_size) {
    u64 cap;
    int r = node_extend(f, n, new_size, &cap);
    if (r) {
        chain_trim(f, n, clusters_for(f, n->size));
        return r;
    }
    if ((r = zero_buf(f))) return r;
    if ((r = data_io(f, n, n->size, NULL, new_size - n->size, 1))) return r;
    n->size = new_size;
    return 0;
}

/* ------------------------------------------------------------------ */
/* fs_ops                                                               */

#define FAT(v) ((fat_t *)(v)->fs_priv)

static void fill_dirent(fat_t *f, const fent_t *e, vfs_dirent_t *o) {
    memset(o, 0, sizeof *o);
    strlcpy(o->name, e->name, sizeof o->name);
    strlcpy(o->short_name, e->sname, sizeof o->short_name);
    o->attr = e->de[11] & 0x3F;
    o->is_dir = (e->de[11] & ATTR_DIR) != 0;
    o->size = o->is_dir ? 0 : rd32(e->de + 28);
    fat_to_vtime(rd16(e->de + 22), rd16(e->de + 24), &o->mtime);
    fat_to_vtime(rd16(e->de + 14), rd16(e->de + 16), &o->ctime);
    if (e->de[13] >= 100 && o->ctime.second < 59) o->ctime.second++;
}

static void node_link(fat_t *f, fnode_t *n) {
    n->next = f->nodes;
    f->nodes = n;
}

static void node_unref(fat_t *f, fnode_t *n) {
    if (--n->refs) return;
    for (fnode_t **pp = &f->nodes; *pp; pp = &(*pp)->next)
        if (*pp == n) { *pp = n->next; break; }
    kfree(n);
}

static int fat_open(volume_t *v, const char *path, int flags, void **fh) {
    fat_t *f = FAT(v);
    if (!*path) return E_ISDIR;
    if (f->ro && (flags & (VFS_O_WRITE | VFS_O_TRUNC))) return E_ROFS;
    fent_t *e = kmalloc(sizeof *e);
    char *name = kmalloc(VFS_MAX_NAME + 1);
    fnode_t *n = NULL;
    u32 dir = 0;
    int r = (!e || !name) ? E_NOMEM : 0;
    if (!r) r = resolve_parent(f, path, &dir, name);
    if (r) goto out;
    r = dir_lookup(f, dir, name, e);
    if (r == E_NOENT && (flags & VFS_O_CREATE)) {
        u8 tmpl[32];
        u32 sec; u16 off;
        if (f->ro) { r = E_ROFS; goto out; }
        if ((r = name_valid(name))) goto out;
        tmpl_new(tmpl, ATTR_ARCH, 0);
        r = dir_add(f, dir, name, tmpl, &sec, &off);
        int r2 = fat_flush(f);
        if (!r) r = r2;
        if (r) goto out;
        n = kzalloc(sizeof *n);
        if (!n) { r = E_NOMEM; goto out; }
        n->ent_sec = sec; n->ent_off = off; n->dir = dir;
        n->attr = ATTR_ARCH;
        n->refs = 1;
        node_link(f, n);
    } else if (r) {
        goto out;
    } else {
        if ((flags & VFS_O_CREATE) && (flags & VFS_O_EXCL)) { r = E_EXIST; goto out; }
        if (e->de[11] & ATTR_DIR) { r = E_ISDIR; goto out; }
        if ((flags & VFS_O_WRITE) && (e->de[11] & ATTR_RO)) { r = E_ROFS; goto out; }
        n = node_find(f, e->sec, e->off);
        if (n) {
            n->refs++;
        } else {
            n = kzalloc(sizeof *n);
            if (!n) { r = E_NOMEM; goto out; }
            n->ent_sec = e->sec; n->ent_off = e->off; n->dir = dir;
            n->first = ent_clus(f, e->de);
            n->size = rd32(e->de + 28);
            n->attr = e->de[11];
            n->refs = 1;
            node_link(f, n);
        }
        if ((flags & VFS_O_TRUNC) && (flags & VFS_O_WRITE) && (n->size || n->first)) {
            u32 old = n->first;
            n->size = 0;
            n->first = 0;
            n->hint_clus = 0;
            r = node_store(f, n, 1);
            int r2 = fat_flush(f);
            if (!r) r = r2;
            if (!r && old) {
                r = chain_free(f, old);
                r2 = fat_flush(f);
                if (!r) r = r2;
            }
            if (r) { node_unref(f, n); goto out; }
        }
    }
    *fh = n;
    r = 0;
out:
    kfree(e);
    kfree(name);
    return r;
}

static int fat_close(volume_t *v, void *fh) {
    fat_t *f = FAT(v);
    int r = fat_flush(f);
    node_unref(f, fh);
    return r;
}

static int fat_read(volume_t *v, void *fh, u64 pos, void *buf, u32 len) {
    fat_t *f = FAT(v);
    fnode_t *n = fh;
    if (pos >= n->size) return 0;
    if (len > n->size - pos) len = (u32)(n->size - pos);
    if (len > 0x7FFFFFFF) len = 0x7FFFFFFF;
    if (!len) return 0;
    int r = data_io(f, n, (u32)pos, buf, len, 0);
    return r ? r : (int)len;
}

static int fat_write(volume_t *v, void *fh, u64 pos, const void *buf, u32 len) {
    fat_t *f = FAT(v);
    fnode_t *n = fh;
    if (f->ro) return E_ROFS;
    if (!len) return 0;
    if (len > 0x7FFFFFFF) len = 0x7FFFFFFF;
    if (pos >= 0xFFFFFFFFull) return E_NOSPC;
    u64 end = pos + len;
    if (end > 0xFFFFFFFFull) end = 0xFFFFFFFFull;
    u64 cap;
    int r = node_extend(f, n, end, &cap);
    if (r && r != E_NOSPC) { chain_trim(f, n, clusters_for(f, n->size)); fat_flush(f); return r; }
    if (cap < end) end = cap;
    if (end <= pos) {
        chain_trim(f, n, clusters_for(f, n->size));
        fat_flush(f);
        return E_NOSPC;
    }
    if (pos > n->size) {
        if ((r = zero_buf(f)) || (r = data_io(f, n, n->size, NULL, (u32)pos - n->size, 1))) goto fail;
    }
    if ((r = data_io(f, n, (u32)pos, (u8 *)buf, (u32)(end - pos), 1))) goto fail;
    if (end > n->size) n->size = (u32)end;
    r = node_store(f, n, 1);
    int r2 = fat_flush(f);
    if (r || r2) return r ? r : r2;
    return (int)(end - pos);
fail:
    chain_trim(f, n, clusters_for(f, n->size));
    fat_flush(f);
    return r;
}

static int fat_truncate(volume_t *v, void *fh, u64 size) {
    fat_t *f = FAT(v);
    fnode_t *n = fh;
    int r;
    if (f->ro) return E_ROFS;
    if (size > 0xFFFFFFFFull) return E_NOSPC;
    if (size == n->size) {
        if ((r = node_store(f, n, 1))) return r;
        return fat_flush(f);
    }
    if (size > n->size) {
        r = node_grow(f, n, (u32)size);
        int r1 = node_store(f, n, 1);
        int r2 = fat_flush(f);
        return r ? r : r1 ? r1 : r2;
    }
    n->size = (u32)size;
    u32 keep = clusters_for(f, size);
    u32 old_first = n->first;
    if (keep == 0) n->first = 0;
    if ((r = node_store(f, n, 1)) || (r = fat_flush(f))) return r;
    if (keep == 0) {
        n->hint_clus = 0;
        r = chain_free(f, old_first);
    } else {
        r = chain_trim(f, n, keep);
    }
    int r2 = fat_flush(f);
    return r ? r : r2;
}

static u64 fat_size(volume_t *v, void *fh) { return ((fnode_t *)fh)->size; }

typedef struct {
    int (*cb)(void *ctx, const vfs_dirent_t *e);
    void *ctx;
    vfs_dirent_t de;
} rd_ctx_t;

static int readdir_cb(fat_t *f, fent_t *e, void *vctx) {
    rd_ctx_t *c = vctx;
    if (e->is_dot) return 0;
    fill_dirent(f, e, &c->de);
    return c->cb(c->ctx, &c->de) ? 1 : 0;
}

static int fat_readdir(volume_t *v, const char *path, int (*cb)(void *ctx, const vfs_dirent_t *e), void *ctx) {
    fat_t *f = FAT(v);
    fent_t *e = kmalloc(sizeof *e);
    rd_ctx_t *c = kmalloc(sizeof *c);
    int r = (!e || !c) ? E_NOMEM : 0, isroot = 0;
    u32 dir = root_id(f);
    if (!r) r = resolve(f, path, e, &isroot);
    if (!r && !isroot) {
        if (!(e->de[11] & ATTR_DIR)) r = E_NOTDIR;
        else if (!(dir = ent_clus(f, e->de))) r = E_IO;
    }
    if (!r) {
        c->cb = cb;
        c->ctx = ctx;
        r = dir_scan(f, dir, readdir_cb, c, e);
        if (r > 0) r = 0;
    }
    kfree(e);
    kfree(c);
    return r;
}

static int fat_stat(volume_t *v, const char *path, vfs_dirent_t *out) {
    fat_t *f = FAT(v);
    fent_t *e = kmalloc(sizeof *e);
    if (!e) return E_NOMEM;
    int isroot;
    int r = resolve(f, path, e, &isroot);
    if (!r) {
        if (isroot) {
            memset(out, 0, sizeof *out);
            out->attr = VFS_ATTR_DIR;
            out->is_dir = 1;
        } else {
            fill_dirent(f, e, out);
            fnode_t *n = node_find(f, e->sec, e->off);
            if (n) out->size = n->size;
        }
    }
    kfree(e);
    return r;
}

static int fat_mkdir(volume_t *v, const char *path) {
    fat_t *f = FAT(v);
    if (f->ro) return E_ROFS;
    fent_t *e = kmalloc(sizeof *e);
    char *name = kmalloc(VFS_MAX_NAME + 1);
    int r = (!e || !name) ? E_NOMEM : 0;
    u32 dir = 0, nc = 0;
    if (!r) r = resolve_parent(f, path, &dir, name);
    if (!r) {
        r = dir_lookup(f, dir, name, e);
        if (!r) r = E_EXIST;
        else if (r == E_NOENT) r = name_valid(name);
    }
    if (r) goto out;
    if ((r = clus_alloc(f, 0, &nc))) goto out;
    if ((r = clus_zero(f, nc))) goto undo;
    {
        int err = 0;
        cent_t *c = cget(f, clus_sec(f, nc), 1, &err);
        if (!c) { r = err; goto undo; }
        u8 *p = c->buf;
        tmpl_new(p, ATTR_DIR, nc);
        memset(p, ' ', 11);
        p[0] = '.';
        memcpy(p + 32, p, 32);
        p[33] = '.';
        ent_set_clus(p + 32, dir == root_id(f) ? 0 : dir);
        c->dirty = 1;
        u8 tmpl[32];
        u32 sec; u16 off;
        tmpl_new(tmpl, ATTR_DIR, nc);
        if ((r = dir_add(f, dir, name, tmpl, &sec, &off))) {
            cache_inval(f, clus_sec(f, nc), 1);
            goto undo;
        }
    }
    r = fat_flush(f);
    goto out;
undo:
    chain_free(f, nc);
    fat_flush(f);
out:
    kfree(e);
    kfree(name);
    return r;
}

static int fat_unlink(volume_t *v, const char *path) {
    fat_t *f = FAT(v);
    if (f->ro) return E_ROFS;
    if (!*path) return E_INVAL;
    fent_t *e = kmalloc(sizeof *e);
    fent_t *tmp = kmalloc(sizeof *tmp);
    int r = (!e || !tmp) ? E_NOMEM : 0, isroot;
    if (!r) r = resolve(f, path, e, &isroot);
    if (r) goto out;
    if (node_find(f, e->sec, e->off)) { r = E_BUSY; goto out; }
    u32 c = ent_clus(f, e->de);
    if (e->de[11] & ATTR_DIR) {
        if (!c) { r = E_IO; goto out; }
        r = dir_scan(f, c, dir_is_empty_cb, NULL, tmp);
        if (r < 0) goto out;
        if (r == 1) { r = E_NOTEMPTY; goto out; }
    }
    if ((r = ent_delete(f, e)) || (r = fat_flush(f))) goto out;
    if (c) {
        r = chain_free(f, c);
        int r2 = fat_flush(f);
        if (!r) r = r2;
    }
out:
    kfree(e);
    kfree(tmp);
    return r;
}

static int fat_rename(volume_t *v, const char *from, const char *to) {
    fat_t *f = FAT(v);
    if (f->ro) return E_ROFS;
    if (!*from || !*to) return E_INVAL;
    fent_t *src = kmalloc(sizeof *src), *dst = kmalloc(sizeof *dst);
    char *name = kmalloc(VFS_MAX_NAME + 1);
    int r = (!src || !dst || !name) ? E_NOMEM : 0, isroot;
    u32 ddir = 0;
    if (!r) r = resolve(f, from, src, &isroot);
    if (!r) r = resolve_parent(f, to, &ddir, name);
    if (r) goto out;
    r = dir_lookup(f, ddir, name, dst);
    if (!r) {
        if (dst->sec != src->sec || dst->off != src->off) { r = E_EXIST; goto out; }
        if (!strcmp(src->name, name)) { r = 0; goto out; }   /* same name: nothing to do */
    } else if (r != E_NOENT) {
        goto out;
    }
    if ((r = name_valid(name))) goto out;
    int is_dir = (src->de[11] & ATTR_DIR) != 0;
    u32 sclus = ent_clus(f, src->de);
    if (is_dir && ddir != src->dir) {
        if (!clus_ok(f, sclus)) { r = E_IO; goto out; }
        u32 cur = ddir;
        for (int depth = 0; ; depth++) {
            if (cur == sclus) { r = E_INVAL; goto out; }
            if (cur == root_id(f) || depth > 4096) break;
            if ((r = dir_parent(f, cur, &cur))) goto out;
        }
    }
    u32 nsec; u16 noff;
    u8 tmpl[32];
    memcpy(tmpl, src->de, 32);
    if ((r = dir_add(f, ddir, name, tmpl, &nsec, &noff))) { fat_flush(f); goto out; }
    if ((r = ent_delete(f, src))) goto out;
    if (is_dir && ddir != src->dir) {
        int err = 0;
        cent_t *c = cget(f, clus_sec(f, sclus), 0, &err);
        if (!c) { r = err; goto out; }
        u8 *p = c->buf + 32;
        if (p[0] == '.' && p[1] == '.') {
            ent_set_clus(p, ddir == root_id(f) ? 0 : ddir);
            c->dirty = 1;
        }
    }
    fnode_t *n = node_find(f, src->sec, src->off);
    if (n) { n->ent_sec = nsec; n->ent_off = noff; n->dir = ddir; }
    r = fat_flush(f);
out:
    kfree(src);
    kfree(dst);
    kfree(name);
    return r;
}

static int fat_statfs(volume_t *v, u64 *total, u64 *free_bytes) {
    fat_t *f = FAT(v);
    if (f->free_count == FREE_UNKNOWN) {
        int r = count_free(f);
        if (r) return r;
        if (!f->ro) fat_flush(f);
    }
    if (total) *total = (u64)f->nclusters * f->csize;
    if (free_bytes) *free_bytes = (u64)f->free_count * f->csize;
    return 0;
}

static int fat_sync(volume_t *v) { return fat_flush(FAT(v)); }

static void fat_free(fat_t *f) {
    while (f->nodes) {
        fnode_t *n = f->nodes;
        f->nodes = n->next;
        kfree(n);
    }
    kfree(f->cache_mem);
    kfree(f->bounce);
    kfree(f->ra_buf);
    kfree(f->wb_buf);
    kfree(f->zero);
    kfree(f);
}

static int fat_unmount(volume_t *v) {
    fat_t *f = FAT(v);
    if (!f) return 0;
    int r = f->ro ? 0 : fat_flush(f);
    fat_free(f);
    v->fs_priv = NULL;
    return r;
}

static const fs_ops_t fat_ops = {
    .name = "fat",
    .unmount = fat_unmount,
    .open = fat_open,
    .close = fat_close,
    .read = fat_read,
    .write = fat_write,
    .truncate = fat_truncate,
    .size = fat_size,
    .readdir = fat_readdir,
    .stat = fat_stat,
    .mkdir = fat_mkdir,
    .unlink = fat_unlink,
    .rename = fat_rename,
    .statfs = fat_statfs,
    .sync = fat_sync,
};

/* ------------------------------------------------------------------ */
/* mount                                                                */

int fat_bpb_sane(const u8 *s, u32 dev_ss) {
    if (!((s[0] == 0xEB && s[2] == 0x90) || s[0] == 0xE9 || (s[510] == 0x55 && s[511] == 0xAA))) return 0;
    u32 bps = rd16(s + 11), spc = s[13], rsv = rd16(s + 14), nfats = s[16];
    u32 tot = rd16(s + 19) ? rd16(s + 19) : rd32(s + 32);
    u32 fsz = rd16(s + 22) ? rd16(s + 22) : rd32(s + 36);
    u8 media = s[21];
    if (!is_pow2(bps) || bps < 512 || bps > 4096) return 0;
    if (dev_ss && (bps < dev_ss || bps % dev_ss)) return 0;
    if (!is_pow2(spc) || spc > 128) return 0;
    if (!rsv || nfats < 1 || nfats > 2) return 0;
    if (media != 0xF0 && media < 0xF8) return 0;
    if (!tot || !fsz) return 0;
    if (rd16(s + 22) == 0 && rd16(s + 17) != 0) return 0;   /* FAT32 BPB with root entries */
    return 1;
}

static void trim_label(char *dst, const u8 *src) {
    int n = 11;
    while (n > 0 && (src[n - 1] == ' ' || src[n - 1] == 0)) n--;
    for (int i = 0; i < n; i++) dst[i] = (char)(src[i] < 0x20 ? '_' : src[i]);
    dst[n] = 0;
}

static void read_label(fat_t *f, const u8 *bpb, char *label) {
    diter_t d;
    label[0] = 0;
    if (di_start(f, &d, root_id(f)) == 0) {
        for (u32 i = 0; i < DIR_MAX_ENTRIES; i++) {
            int err = 0;
            cent_t *c = cget(f, d.sec, 0, &err);
            if (!c) break;
            const u8 *p = c->buf + di_off(f, &d);
            if (p[0] == 0) break;
            if (p[0] != 0xE5 && (p[11] & 0x3F) != ATTR_LFN && (p[11] & ATTR_VOLUME)) {
                trim_label(label, p);
                return;
            }
            if (di_next(f, &d)) break;
        }
    }
    const u8 *bl = bpb + (f->type == 32 ? 71 : 43);
    u8 sig = bpb[f->type == 32 ? 66 : 38];
    if (sig == 0x29 && memcmp(bl, "NO NAME    ", 11)) trim_label(label, bl);
}

int fat_probe(blockdev_t *bd, volume_t *v) {
    u32 dss = bd->sector_size;
    if (dss < 512 || dss > 4096 || !is_pow2(dss)) return E_NOTSUP;
    u8 *s = kmalloc(dss);
    if (!s) return E_NOMEM;
    int r = blk_read(bd, 0, 1, s);
    if (r) { kfree(s); return r; }
    if (!fat_bpb_sane(s, dss)) { kfree(s); return E_INVAL; }

    fat_t *f = kzalloc(sizeof *f);
    if (!f) { kfree(s); return E_NOMEM; }
    f->v = v;
    f->bd = bd;
    f->bps = rd16(s + 11);
    f->spc = s[13];
    f->csize = f->bps * f->spc;
    f->shift = 0;
    while ((dss << f->shift) < f->bps) f->shift++;
    u32 rsv = rd16(s + 14);
    f->nfats = s[16];
    f->root_entries = rd16(s + 17);
    f->total_sectors = rd16(s + 19) ? rd16(s + 19) : rd32(s + 32);
    f->fat_size = rd16(s + 22) ? rd16(s + 22) : rd32(s + 36);
    f->root_sectors = (f->root_entries * 32 + f->bps - 1) / f->bps;
    f->fat_base = rsv;
    u64 meta = (u64)rsv + (u64)f->nfats * f->fat_size + f->root_sectors;
    if (f->total_sectors <= meta) goto bad;     /* also keeps all sums below in 32 bits */
    u32 nclus = (u32)((f->total_sectors - meta) / f->spc);
    if (nclus < 4085) f->type = 12;
    else if (nclus < 65525) f->type = 16;
    else f->type = 32;
    if ((f->type == 32) != (rd16(s + 22) == 0)) goto bad;
    if (f->type == 32 && f->root_entries) goto bad;
    if (f->type != 32 && !f->root_entries) goto bad;
    /* the FAT must be able to describe every cluster */
    u64 fat_entries = (u64)f->fat_size * f->bps * 8 / (f->type == 12 ? 12 : f->type);
    if (fat_entries < 3) goto bad;
    if ((u64)nclus + 2 > fat_entries) nclus = (u32)(fat_entries - 2);
    if (f->type == 32 && nclus > 0x0FFFFFF5) nclus = 0x0FFFFFF5;
    f->nclusters = nclus;
    f->mirror = 1;
    f->fat_start = rsv;
    if (f->type == 32) {
        u16 ext = rd16(s + 40);
        if (ext & 0x80) {
            f->mirror = 0;
            u32 act = ext & 0x0F;
            if (act < f->nfats) f->fat_start = rsv + act * f->fat_size;
        }
    }
    f->root_start = rsv + f->nfats * f->fat_size;
    f->data_start = f->root_start + f->root_sectors;
    u64 dev_secs = bd->sector_count >> f->shift;
    if (f->data_start >= dev_secs) goto bad;
    if ((u64)f->data_start + (u64)f->nclusters * f->spc > dev_secs) {
        /* file system larger than the device (truncated image, bad BPB):
         * clusters past the end of the device don't exist */
        kprintf("fat: %s: file system exceeds the device, clusters limited\n", bd->name);
        f->nclusters = (u32)((dev_secs - f->data_start) / f->spc);
    }
    switch (f->type) {
    case 12: f->eoc_min = 0xFF8; f->eoc_mark = 0xFFF; break;
    case 16: f->eoc_min = 0xFFF8; f->eoc_mark = 0xFFFF; break;
    default: f->eoc_min = 0x0FFFFFF8; f->eoc_mark = 0x0FFFFFFF; break;
    }
    f->free_count = FREE_UNKNOWN;
    f->next_free = 2;
    if (f->type == 32) {
        f->root_clus = rd32(s + 44);
        if (!clus_ok(f, f->root_clus)) goto bad;
        u32 fsi = rd16(s + 48);
        if (fsi && fsi != 0xFFFF && fsi < rsv) f->fsinfo_sec = fsi;
    }
    f->ro = bd->readonly;
    f->cache_mem = kmalloc((size_t)CACHE_N * f->bps);
    f->bounce = kmalloc(f->bps);
    f->ra_buf = kmalloc((size_t)RUN_N * f->bps);
    f->wb_buf = kmalloc((size_t)RUN_N * f->bps);
    if (!f->cache_mem || !f->bounce || !f->ra_buf || !f->wb_buf) { r = E_NOMEM; goto fail; }
    for (int i = 0; i < CACHE_N; i++) f->cache[i].buf = f->cache_mem + (size_t)i * f->bps;

    if (f->fsinfo_sec) {
        int err = 0;
        cent_t *c = cget(f, f->fsinfo_sec, 0, &err);
        if (c && rd32(c->buf) == 0x41615252 && rd32(c->buf + 484) == 0x61417272) {
            u32 fc = rd32(c->buf + 488), nf = rd32(c->buf + 492);
            if (fc <= f->nclusters) f->free_count = fc;
            if (clus_ok(f, nf)) f->next_free = nf;
        } else {
            f->fsinfo_sec = 0;
        }
    }

    {
        /* a broken root directory start is a strong hint of a bogus BPB */
        diter_t d;
        if (di_start(f, &d, root_id(f)) < 0) goto bad;
    }
    v->ops = &fat_ops;
    v->fs_priv = f;
    snprintf(v->fs_name, sizeof v->fs_name, "FAT%d", f->type);
    read_label(f, s, v->label);
    kprintf("fat: %s: FAT%d, %u clusters of %u bytes, label '%s'%s\n", bd->name, f->type,
            f->nclusters, f->csize, v->label, f->ro ? " (read-only)" : "");
    kfree(s);
    return 0;
bad:
    r = E_INVAL;
fail:
    fat_free(f);
    kfree(s);
    return r;
}
