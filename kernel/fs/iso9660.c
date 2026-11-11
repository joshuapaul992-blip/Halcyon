/* Halcyon OS - ISO 9660 file system driver (read-only)
 *
 * CD/DVD data discs and ISO images (also hybrid images on USB sticks / disks).
 *
 * Supported:
 *   - primary volume descriptor (ISO 9660 levels 1-3), logical block sizes
 *     512 / 1024 / 2048, on devices with any sector size (all addressing is
 *     done in bytes: the volume descriptors are at byte 32768 on a 2048-byte
 *     CD as well as on a 512-byte USB stick holding a dd'ed ISO image)
 *   - Joliet supplementary descriptor (escape sequences %/@ %/C %/E, names in
 *     UCS-2 / UTF-16 big endian, converted to UTF-8 incl. surrogate pairs);
 *     preferred over the primary tree when present
 *   - Rock Ridge NM (POSIX names, with CE continuation areas) and RE (hidden
 *     relocated directories) on the primary tree when there is no Joliet tree
 *   - directory records spanning several blocks, multi-extent files (flag
 *     bit 7, up to ISO_MAX_EXTENTS extents; more are listed but not opened),
 *     ";1" version suffixes and trailing '.' stripped, case-insensitive lookup
 *   - volume label from the volume identifier (Joliet one if present)
 * Not supported: interleaved files (listed, open fails with E_NOTSUP), Rock
 * Ridge CL/PL deep-directory relocation, UDF, multisession (the first session's
 * descriptors are used, which is what the drive returns at byte 32768 for
 * most discs).
 *
 * Everything on the disc is untrusted: every length field is checked against
 * the record / block / device it lives in, directory scans are bounded by the
 * directory size (capped at ISO_DIR_MAX), CE chains by ISO_RR_MAX_CE hops, and
 * a path that descends into one of its own ancestors fails with E_IO, so a
 * directory loop cannot make a recursive walk run forever.
 *
 * All write operations return E_ROFS.
 */
#include "core/types.h"
#include "core/lib.h"
#include "core/unicode.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "fs/block.h"
#include "fs/vfs.h"
#include "fs/iso9660.h"

#define ISO_VD_START    32768u           /* byte offset of the descriptor set */
#define ISO_VD_SIZE     2048u
#define ISO_MAX_VDS     32
#define ISO_CACHE_N     16               /* directory block cache */
#define ISO_MAX_IO      (1024u * 1024u)
#define ISO_DIR_MAX     (16u << 20)      /* largest directory accepted */
#define ISO_MAX_EXTENTS 64
#define ISO_MAX_DEPTH   (VFS_MAX_PATH / 2 + 2)
#define ISO_RR_MAX_CE   8

/* directory record flags */
#define DR_HIDDEN     0x01
#define DR_DIR        0x02
#define DR_ASSOC      0x04
#define DR_MULTI      0x80

enum { NAMES_PLAIN, NAMES_RR, NAMES_JOLIET };

typedef struct {
    u32 blk;
    u32 stamp;
    u8  valid;
    u8 *buf;
} cent_t;

/* an open file */
typedef struct inode {
    struct inode *next;
    u64 size;
    u32 next_cnt;
    struct { u64 off; u32 len; } ext[];   /* device byte offset, length */
} inode_t;

typedef struct iso {
    blockdev_t *bd;
    u32  ss;                    /* device sector size */
    u32  bsize;                 /* logical block size */
    u64  dev_bytes;
    u64  vol_bytes;
    u32  root_lba, root_len;    /* root of the tree in use */
    u8   root_date[7];
    int  names;                 /* NAMES_* */
    int  joliet_level;
    u32  rr_skip;               /* SUSP LEN_SKP */
    cent_t cache[ISO_CACHE_N];
    u8  *cache_mem;
    u32  stamp;
    u8  *bounce;                /* one device sector */
    u8  *ce_buf;                /* one logical block (Rock Ridge continuation areas) */
    inode_t *files;
} iso_t;

/* one directory entry (multi-extent records merged) */
typedef struct {
    char name[VFS_MAX_NAME + 1];
    u8   flags;
    u8   bad;                   /* interleaved, broken multi-extent chain, too many extents */
    u8   date[7];
    u8   rawlen;
    u8   raw[255];              /* file identifier as recorded (multi-extent matching) */
    u64  size;
    u32  next;
    u32  ext_blk[ISO_MAX_EXTENTS];
    u32  ext_len[ISO_MAX_EXTENTS];
} ient_t;

typedef struct { u32 lba, len, pos; } diter_t;

static const fs_ops_t iso_ops;

/* ------------------------------------------------------------------ */
/* helpers                                                              */

static inline u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static inline u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static inline u16 rd16be(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static inline int is_pow2(u32 v) { return v && !(v & (v - 1)); }

/* VFS names are compared case-insensitively. One helper so the comparison
 * can be swapped for Unicode case folding in one place. */
static int iso_name_eq(const char *a, const char *b) { return !utf8_casecmp(a, b); }   /* Unicode case folding */

/* bounded UTF-8 writer (whole code points only) */
typedef struct { char *p; size_t n, cap; } ustr_t;

static void u_put(ustr_t *u, u32 cp) {
    char b[4];
    size_t k;
    if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;
    if (cp > 0x10FFFF) cp = 0xFFFD;
    if (cp < 0x80) { b[0] = (char)cp; k = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); k = 2; }
    else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F)); k = 3;
    } else {
        b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); k = 4;
    }
    if (u->n + k + 1 > u->cap) { u->cap = u->n + 1; return; }   /* full: stop here */
    memcpy(u->p + u->n, b, k);
    u->n += k;
    u->p[u->n] = 0;
}

static void u_init(ustr_t *u, char *out, size_t cap) { u->p = out; u->n = 0; u->cap = cap; out[0] = 0; }

/* UCS-2 / UTF-16 big endian -> UTF-8 */
static void utf16be_to_utf8(const u8 *s, u32 bytes, char *out, size_t cap) {
    ustr_t u;
    u_init(&u, out, cap);
    u32 n = bytes / 2;
    for (u32 i = 0; i < n; i++) {
        u32 c = rd16be(s + 2 * i);
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) {
            u32 lo = rd16be(s + 2 * i + 2);
            if (lo >= 0xDC00 && lo < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        u_put(&u, c);
    }
}

/* length of a valid UTF-8 sequence at s (n bytes left), 0 if invalid */
static int utf8_seq(const u8 *s, u32 n) {
    u8 c = s[0];
    int k;
    u32 cp;
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) { k = 2; cp = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { k = 3; cp = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { k = 4; cp = c & 0x07; }
    else return 0;
    if ((u32)k > n) return 0;
    for (int i = 1; i < k; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    if ((k == 3 && cp < 0x800) || (k == 4 && (cp < 0x10000 || cp > 0x10FFFF))) return 0;
    if (cp >= 0xD800 && cp < 0xE000) return 0;
    return k;
}

/* 8-bit name: kept as is if it is valid UTF-8 (Rock Ridge names usually
 * are), else taken as Latin-1 */
static void bytes_to_utf8(const u8 *s, u32 n, char *out, size_t cap) {
    ustr_t u;
    u_init(&u, out, cap);
    int valid = 1;
    for (u32 i = 0; i < n;) {
        int k = utf8_seq(s + i, n - i);
        if (!k) { valid = 0; break; }
        i += (u32)k;
    }
    for (u32 i = 0; i < n;) {
        if (valid) {
            int k = utf8_seq(s + i, n - i);
            if (u.n + (u32)k + 1 > u.cap) break;
            memcpy(u.p + u.n, s + i, (size_t)k);
            u.n += (u32)k;
            u.p[u.n] = 0;
            i += (u32)k;
        } else {
            u_put(&u, s[i++]);
        }
    }
}

/* "NAME.EXT;1" -> "NAME.EXT", "README." -> "README" */
static void strip_version(char *s) {
    char *semi = strrchr(s, ';');
    if (semi) {
        const char *d = semi + 1;
        while (*d >= '0' && *d <= '9') d++;
        if (!*d) *semi = 0;
    }
    size_t n = strlen(s);
    while (n > 1 && s[n - 1] == '.') s[--n] = 0;
}

/* make the name usable as one VFS path component */
static void sanitize(char *s) {
    for (u8 *p = (u8 *)s; *p; p++)
        if (*p < 0x20 || *p == 0x7F || *p == '/' || *p == '\\' || *p == ':') *p = '_';
    if (!s[0] || !strcmp(s, ".")) strlcpy(s, "_", 2);
    else if (!strcmp(s, "..")) strlcpy(s, "__", 3);
}

static void iso_time(const u8 *d, vfs_time_t *t) {
    memset(t, 0, sizeof *t);
    if (d[1] < 1 || d[1] > 12 || d[2] < 1 || d[2] > 31 || d[3] > 23 || d[4] > 59 || d[5] > 59) return;
    t->year = (u16)(1900 + d[0]);
    t->month = d[1];
    t->day = d[2];
    t->hour = d[3];
    t->minute = d[4];
    t->second = d[5];
}

/* ------------------------------------------------------------------ */
/* device access                                                        */

/* read len bytes at device byte offset off (any alignment) */
static int dev_read(iso_t *s, u64 off, u32 len, void *buf) {
    u8 *b = buf;
    if (off > s->dev_bytes || len > s->dev_bytes - off) return E_IO;
    while (len) {
        u64 lba = off / s->ss;
        u32 in = (u32)(off % s->ss);
        u32 n;
        if (in == 0 && len >= s->ss) {
            u32 cnt = MIN(len, ISO_MAX_IO) / s->ss;
            if (!cnt) cnt = 1;
            int r = blk_read(s->bd, lba, cnt, b);
            if (r) return r == E_NODEV ? r : E_IO;
            n = cnt * s->ss;
        } else {
            int r = blk_read(s->bd, lba, 1, s->bounce);
            if (r) return r == E_NODEV ? r : E_IO;
            n = MIN(len, s->ss - in);
            memcpy(b, s->bounce + in, n);
        }
        b += n; off += n; len -= n;
    }
    return 0;
}

/* logical block blk through the cache; *out valid until the next cache_get */
static int cache_get(iso_t *s, u32 blk, const u8 **out) {
    cent_t *victim = NULL;
    for (int i = 0; i < ISO_CACHE_N; i++) {
        cent_t *c = &s->cache[i];
        if (c->valid && c->blk == blk) {
            c->stamp = ++s->stamp;
            *out = c->buf;
            return 0;
        }
        if (!victim || !c->valid || (victim->valid && c->stamp < victim->stamp)) victim = c;
    }
    victim->valid = 0;
    int r = dev_read(s, (u64)blk * s->bsize, s->bsize, victim->buf);
    if (r) return r;
    victim->blk = blk;
    victim->valid = 1;
    victim->stamp = ++s->stamp;
    *out = victim->buf;
    return 0;
}

/* ------------------------------------------------------------------ */
/* directories                                                          */

/* next raw directory record, copied to rec (256 bytes). Records never
 * cross a block boundary; a zero length byte pads to the next block. A
 * malformed record makes the scan skip the rest of its block.
 * Returns 1, 0 at the end, or a negative error. */
static int di_next(iso_t *s, diter_t *d, u8 *rec) {
    while (d->pos < d->len) {
        u32 in = d->pos & (s->bsize - 1);
        u32 room = s->bsize - in;
        if (room < 34) { d->pos += room; continue; }
        const u8 *b;
        int r = cache_get(s, d->lba + d->pos / s->bsize, &b);
        if (r) return r;
        const u8 *p = b + in;
        u32 rl = p[0];
        if (rl == 0) { d->pos += room; continue; }
        if (rl < 34 || rl > room || rl > d->len - d->pos || 33u + p[32] > rl) {
            d->pos += room;
            continue;
        }
        memcpy(rec, p, rl);
        d->pos += rl;
        return 1;
    }
    return 0;
}

static void add_extent(iso_t *s, ient_t *e, const u8 *rec) {
    u64 blk = (u64)rd32(rec + 2) + rec[1];       /* extended attribute record first */
    u32 len = rd32(rec + 10);
    if (rec[26] || rec[27]) e->bad = 1;          /* interleaved */
    if (blk > 0xFFFFFFFFull) e->bad = 1;
    if (e->next >= ISO_MAX_EXTENTS) {
        e->bad = 1;
    } else {
        e->ext_blk[e->next] = (u32)blk;
        e->ext_len[e->next] = len;
        e->next++;
    }
    e->size += len;
}

/* Rock Ridge: parse the System Use area of a record (and its CE
 * continuation areas). NM -> name (raw bytes, *nmlen), RE -> *relocated,
 * NM with CURRENT/PARENT -> *dot. */
static void rr_parse(iso_t *s, const u8 *rec, u8 *nm, u32 *nmlen, int *have_nm, int *relocated, int *dot) {
    u32 rl = rec[0], nl = rec[32];
    u32 start = 33 + nl + ((nl & 1) ? 0 : 1) + s->rr_skip;
    const u8 *su = rec;
    u32 pos = start, end = rl;
    int hops = 0;
    *nmlen = 0;
    *have_nm = *relocated = *dot = 0;
    for (;;) {
        u32 ce_blk = 0, ce_off = 0, ce_len = 0;
        int ce = 0, stop = 0;
        while (pos + 4 <= end) {
            const u8 *e = su + pos;
            u32 len = e[2];
            if (len < 4 || len > end - pos) break;
            if (e[0] == 'N' && e[1] == 'M' && len >= 5) {
                if (e[4] & 0x06) *dot = 1;
                u32 n = len - 5;
                if (n > 255 - *nmlen) n = 255 - *nmlen;
                memcpy(nm + *nmlen, e + 5, n);
                *nmlen += n;
                *have_nm = 1;
            } else if (e[0] == 'R' && e[1] == 'E') {
                *relocated = 1;
            } else if (e[0] == 'C' && e[1] == 'E' && len >= 28) {
                ce = 1;
                ce_blk = rd32(e + 4);
                ce_off = rd32(e + 12);
                ce_len = rd32(e + 20);
            } else if (e[0] == 'S' && e[1] == 'T') {
                stop = 1;
                break;
            }
            pos += len;
        }
        if (stop || !ce || ++hops > ISO_RR_MAX_CE) return;
        if (ce_off >= s->bsize || ce_len > s->bsize - ce_off || ce_len < 4) return;
        const u8 *b;
        if (cache_get(s, ce_blk, &b)) return;
        memcpy(s->ce_buf, b + ce_off, ce_len);
        su = s->ce_buf;
        pos = 0;
        end = ce_len;
    }
}

/* name of a record; returns 0 if the record is to be hidden */
static int rec_name(iso_t *s, const u8 *rec, char *out) {
    u32 nl = rec[32];
    const u8 *id = rec + 33;
    if (s->names == NAMES_JOLIET) {
        utf16be_to_utf8(id, nl & ~1u, out, VFS_MAX_NAME + 1);
        strip_version(out);
    } else {
        int done = 0;
        if (s->names == NAMES_RR) {
            u8 nm[256];
            u32 nmlen;
            int have, reloc, dot;
            rr_parse(s, rec, nm, &nmlen, &have, &reloc, &dot);
            if (reloc || dot) return 0;
            if (have && nmlen) {
                bytes_to_utf8(nm, nmlen, out, VFS_MAX_NAME + 1);
                done = 1;
            }
        }
        if (!done) {
            bytes_to_utf8(id, nl, out, VFS_MAX_NAME + 1);
            strip_version(out);
        }
    }
    sanitize(out);
    return 1;
}

/* next directory entry ('.', '..', associated files and Rock Ridge
 * relocated directories skipped; multi-extent records merged).
 * Returns 1, 0 at the end, or a negative error. */
static int dir_next(iso_t *s, diter_t *d, ient_t *e, u8 *rec) {
    for (;;) {
        int r = di_next(s, d, rec);
        if (r <= 0) return r;
        u8 flags = rec[25], nl = rec[32];
        if (nl == 1 && (rec[33] == 0 || rec[33] == 1)) continue;
        if (flags & DR_ASSOC) continue;
        if (!rec_name(s, rec, e->name)) continue;
        e->flags = flags;
        e->bad = 0;
        memcpy(e->date, rec + 18, 7);
        e->rawlen = nl;
        memcpy(e->raw, rec + 33, nl);
        e->size = 0;
        e->next = 0;
        add_extent(s, e, rec);
        if (flags & DR_DIR) {
            if (flags & DR_MULTI) e->bad = 1;    /* directories have one extent */
            return 1;
        }
        /* multi-extent file: the following records with the same identifier
         * hold the rest; the last one has the flag clear */
        while (flags & DR_MULTI) {
            diter_t save = *d;
            r = di_next(s, d, rec);
            if (r < 0) return r;
            if (r == 0 || rec[32] != e->rawlen || memcmp(rec + 33, e->raw, e->rawlen) || (rec[25] & DR_DIR)) {
                *d = save;
                e->bad = 1;
                break;
            }
            flags = rec[25];
            add_extent(s, e, rec);
        }
        return 1;
    }
}

static int dir_find(iso_t *s, u32 lba, u32 len, const char *name, ient_t *e, u8 *rec) {
    diter_t d = { lba, len, 0 };
    for (;;) {
        int r = dir_next(s, &d, e, rec);
        if (r < 0) return r;
        if (r == 0) return E_NOENT;
        if (iso_name_eq(e->name, name)) return 0;
    }
}

/* a directory entry's extent as a directory to scan */
static int dir_extent(iso_t *s, const ient_t *e, u32 *lba, u32 *len) {
    if (!(e->flags & DR_DIR) || e->bad || !e->next) return E_IO;
    if (e->ext_len[0] > ISO_DIR_MAX) return E_IO;
    if ((u64)e->ext_blk[0] * s->bsize >= s->dev_bytes) return E_IO;
    *lba = e->ext_blk[0];
    *len = e->ext_len[0];
    return 0;
}

/* Walk a path ("" = root). On success *isroot tells whether it named the
 * root, else e holds the entry. Directories along the path (including a
 * final one) must not repeat an ancestor: a directory loop fails with E_IO. */
static int resolve(iso_t *s, const char *path, ient_t *e, u8 *rec, int *isroot) {
    u32 anc[ISO_MAX_DEPTH];
    int depth = 0;
    u32 dlba = s->root_lba, dlen = s->root_len;
    char comp[VFS_MAX_NAME + 1];
    anc[depth++] = dlba;
    *isroot = 1;
    const char *p = path;
    while (*p) {
        const char *sl = strchr(p, '/');
        size_t n = sl ? (size_t)(sl - p) : strlen(p);
        if (n == 0) { p++; continue; }
        if (n > VFS_MAX_NAME) return E_NOENT;
        if (!*isroot) {
            /* descend into the previous component */
            if (!(e->flags & DR_DIR)) return E_NOTDIR;
            int r = dir_extent(s, e, &dlba, &dlen);
            if (r) return r;
        }
        memcpy(comp, p, n);
        comp[n] = 0;
        int r = dir_find(s, dlba, dlen, comp, e, rec);
        if (r) return r;
        *isroot = 0;
        if (e->flags & DR_DIR) {
            for (int i = 0; i < depth; i++) {
                if (e->next && anc[i] == e->ext_blk[0]) {
                    kprintf("iso9660: %s: directory loop at '%s'\n", s->bd->name, e->name);
                    return E_IO;
                }
            }
            if (depth >= ISO_MAX_DEPTH) return E_IO;
            if (e->next) anc[depth++] = e->ext_blk[0];
        }
        p += n;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* fs_ops                                                               */

#define ISO(v) ((iso_t *)(v)->fs_priv)

static void fill_dirent(const ient_t *e, vfs_dirent_t *o) {
    memset(o, 0, sizeof *o);
    strlcpy(o->name, e->name, sizeof o->name);
    size_t n = strlen(e->name);
    int ascii = 1;
    for (size_t i = 0; i < n; i++) if ((u8)e->name[i] >= 0x80) ascii = 0;
    if (ascii && n < sizeof o->short_name) strlcpy(o->short_name, e->name, sizeof o->short_name);
    o->is_dir = (e->flags & DR_DIR) != 0;
    o->attr = o->is_dir ? VFS_ATTR_DIR : VFS_ATTR_READONLY;
    if (e->flags & DR_HIDDEN) o->attr |= VFS_ATTR_HIDDEN;
    o->size = o->is_dir ? 0 : e->size;
    iso_time(e->date, &o->mtime);
    o->ctime = o->mtime;
}

typedef struct { ient_t e; u8 rec[256]; } work_t;

static int iso_open(volume_t *v, const char *path, int flags, void **fh) {
    iso_t *s = ISO(v);
    if (flags & (VFS_O_WRITE | VFS_O_TRUNC)) return E_ROFS;
    if (!*path) return E_ISDIR;
    work_t *w = kmalloc(sizeof *w);
    if (!w) return E_NOMEM;
    int isroot;
    int r = resolve(s, path, &w->e, w->rec, &isroot);
    if (r == E_NOENT && (flags & VFS_O_CREATE)) r = E_ROFS;
    if (!r && isroot) r = E_ISDIR;
    if (!r && (w->e.flags & DR_DIR)) r = E_ISDIR;
    if (!r && (flags & VFS_O_CREATE) && (flags & VFS_O_EXCL)) r = E_EXIST;
    if (!r && w->e.bad) {
        kprintf("iso9660: %s: '%s': interleaved or too fragmented file, not supported\n", s->bd->name, w->e.name);
        r = E_NOTSUP;
    }
    if (!r && w->e.size > s->dev_bytes) r = E_IO;
    inode_t *n = NULL;
    if (!r) {
        n = kzalloc(sizeof *n + (size_t)w->e.next * sizeof n->ext[0]);
        if (!n) r = E_NOMEM;
    }
    if (!r) {
        n->size = w->e.size;
        n->next_cnt = w->e.next;
        for (u32 i = 0; i < w->e.next; i++) {
            n->ext[i].off = (u64)w->e.ext_blk[i] * s->bsize;
            n->ext[i].len = w->e.ext_len[i];
        }
        n->next = s->files;
        s->files = n;
        *fh = n;
    }
    kfree(w);
    return r;
}

static int iso_close(volume_t *v, void *fh) {
    iso_t *s = ISO(v);
    for (inode_t **pp = &s->files; *pp; pp = &(*pp)->next)
        if (*pp == fh) { *pp = ((inode_t *)fh)->next; break; }
    kfree(fh);
    return 0;
}

static int iso_read(volume_t *v, void *fh, u64 pos, void *buf, u32 len) {
    iso_t *s = ISO(v);
    inode_t *n = fh;
    if (pos >= n->size) return 0;
    if (len > n->size - pos) len = (u32)(n->size - pos);
    if (len > 0x7FFFFFFF) len = 0x7FFFFFFF;
    u8 *b = buf;
    u32 done = 0;
    u64 base = 0;           /* file offset of extent i */
    for (u32 i = 0; i < n->next_cnt && done < len; i++) {
        u64 elen = n->ext[i].len;
        u64 cur = pos + done;
        if (cur >= base + elen) { base += elen; continue; }
        u32 chunk = (u32)MIN((u64)(len - done), base + elen - cur);
        int r = dev_read(s, n->ext[i].off + (cur - base), chunk, b + done);
        if (r) return done ? (int)done : r;
        done += chunk;
        base += elen;
    }
    return (int)done;
}

static int iso_write(volume_t *v, void *fh, u64 pos, const void *buf, u32 len) { return E_ROFS; }
static int iso_truncate(volume_t *v, void *fh, u64 size) { return E_ROFS; }
static u64 iso_size(volume_t *v, void *fh) { return ((inode_t *)fh)->size; }

static int iso_readdir(volume_t *v, const char *path, int (*cb)(void *ctx, const vfs_dirent_t *e), void *ctx) {
    iso_t *s = ISO(v);
    work_t *w = kmalloc(sizeof *w);
    vfs_dirent_t *de = kmalloc(sizeof *de);
    int r = (!w || !de) ? E_NOMEM : 0, isroot = 1;
    u32 lba = s->root_lba, len = s->root_len;
    if (!r) r = resolve(s, path, &w->e, w->rec, &isroot);
    if (!r && !isroot) {
        if (!(w->e.flags & DR_DIR)) r = E_NOTDIR;
        else r = dir_extent(s, &w->e, &lba, &len);
    }
    if (!r) {
        diter_t d = { lba, len, 0 };
        for (;;) {
            int k = dir_next(s, &d, &w->e, w->rec);
            if (k < 0) { r = k; break; }
            if (k == 0) break;
            fill_dirent(&w->e, de);
            if (cb(ctx, de)) break;
        }
    }
    kfree(w);
    kfree(de);
    return r;
}

static int iso_stat(volume_t *v, const char *path, vfs_dirent_t *out) {
    iso_t *s = ISO(v);
    work_t *w = kmalloc(sizeof *w);
    if (!w) return E_NOMEM;
    int isroot;
    int r = resolve(s, path, &w->e, w->rec, &isroot);
    if (!r) {
        if (isroot) {
            memset(out, 0, sizeof *out);
            out->attr = VFS_ATTR_DIR;
            out->is_dir = 1;
            iso_time(s->root_date, &out->mtime);
            out->ctime = out->mtime;
        } else {
            fill_dirent(&w->e, out);
        }
    }
    kfree(w);
    return r;
}

static int iso_mkdir(volume_t *v, const char *path) { return E_ROFS; }
static int iso_unlink(volume_t *v, const char *path) { return E_ROFS; }
static int iso_rename(volume_t *v, const char *from, const char *to) { return E_ROFS; }

static int iso_statfs(volume_t *v, u64 *total, u64 *free_bytes) {
    iso_t *s = ISO(v);
    if (total) *total = MIN(s->vol_bytes, s->dev_bytes);
    if (free_bytes) *free_bytes = 0;
    return 0;
}

static int iso_sync(volume_t *v) { return 0; }

static void iso_free(iso_t *s) {
    while (s->files) {
        inode_t *n = s->files;
        s->files = n->next;
        kfree(n);
    }
    kfree(s->cache_mem);
    kfree(s->bounce);
    kfree(s->ce_buf);
    kfree(s);
}

static int iso_unmount(volume_t *v) {
    if (v->fs_priv) iso_free(ISO(v));
    v->fs_priv = NULL;
    return 0;
}

static const fs_ops_t iso_ops = {
    .name = "iso9660",
    .unmount = iso_unmount,
    .open = iso_open,
    .close = iso_close,
    .read = iso_read,
    .write = iso_write,
    .truncate = iso_truncate,
    .size = iso_size,
    .readdir = iso_readdir,
    .stat = iso_stat,
    .mkdir = iso_mkdir,
    .unlink = iso_unlink,
    .rename = iso_rename,
    .statfs = iso_statfs,
    .sync = iso_sync,
};

/* ------------------------------------------------------------------ */
/* mount                                                                */

int iso9660_detect(blockdev_t *bd) {
    u32 ss = bd->sector_size;
    if (ss < 512 || !is_pow2(ss) || bd->sector_count * ss < ISO_VD_START + ISO_VD_SIZE) return 0;
    u8 *b = kmalloc(ss);
    if (!b) return 0;
    u32 in = ISO_VD_START % ss;
    int ok = !blk_read(bd, ISO_VD_START / ss, 1, b) && !memcmp(b + in + 1, "CD001", 5) &&
             (b[in] <= 3 || b[in] == 255);
    kfree(b);
    return ok;
}

typedef struct {
    u32 bsize, vol_blocks, root_lba, root_len;
    u8 root_date[7];
    u8 ok;
} vdinfo_t;

/* common PVD / SVD fields; root directory record at 156 */
static int vd_parse(iso_t *s, const u8 *vd, vdinfo_t *o) {
    const u8 *root = vd + 156;
    memset(o, 0, sizeof *o);
    o->bsize = rd16(vd + 128);
    if (o->bsize != 512 && o->bsize != 1024 && o->bsize != 2048) return 0;
    o->vol_blocks = rd32(vd + 80);
    if (root[0] < 34) return 0;
    u64 lba = (u64)rd32(root + 2) + root[1];
    if (lba > 0xFFFFFFFFull) return 0;
    o->root_lba = (u32)lba;
    o->root_len = rd32(root + 10);
    if (!o->root_len || o->root_len > ISO_DIR_MAX) return 0;
    if ((u64)o->root_lba * o->bsize + o->bsize > s->dev_bytes) return 0;
    memcpy(o->root_date, root + 18, 7);
    o->ok = 1;
    return 1;
}

static void trim_label(char *l) {
    size_t n = strlen(l);
    while (n && l[n - 1] == ' ') l[--n] = 0;
    for (u8 *p = (u8 *)l; *p; p++) if (*p < 0x20 || *p == 0x7F) *p = '_';
}

/* copy a UTF-8 string into a small buffer without splitting a character */
static void copy_utf8(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    while (src[n]) {
        size_t k = 1;
        while ((src[n + k] & 0xC0) == 0x80) k++;
        if (n + k + 1 > cap) break;
        n += k;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

int iso9660_probe(blockdev_t *bd, volume_t *v) {
    u32 ss = bd->sector_size;
    if (ss < 512 || ss > 65536 || !is_pow2(ss)) return E_NOTSUP;
    u64 dev_bytes = bd->sector_count * ss;
    if (dev_bytes < ISO_VD_START + 2 * ISO_VD_SIZE) return E_INVAL;
    iso_t *s = kzalloc(sizeof *s);
    u8 *vd = kmalloc(ISO_VD_SIZE);
    char *lab = kmalloc(VFS_MAX_NAME + 1);
    int r = E_INVAL;
    if (!s || !vd || !lab) { r = E_NOMEM; goto fail; }
    s->bd = bd;
    s->ss = ss;
    s->dev_bytes = dev_bytes;
    s->bounce = kmalloc(ss);
    if (!s->bounce) { r = E_NOMEM; goto fail; }

    vdinfo_t pvd = { 0 }, jol = { 0 };
    char plabel[33] = "", jlabel[64] = "";
    int jlevel = 0;
    for (u32 i = 0; i < ISO_MAX_VDS; i++) {
        if (dev_read(s, ISO_VD_START + (u64)i * ISO_VD_SIZE, ISO_VD_SIZE, vd)) {
            if (i == 0) { r = E_IO; goto fail; }
            break;
        }
        if (memcmp(vd + 1, "CD001", 5)) {
            if (i == 0) goto fail;
            break;
        }
        if (vd[0] == 255) break;
        if (vd[0] == 1 && !pvd.ok) {
            if (vd_parse(s, vd, &pvd)) {
                memcpy(plabel, vd + 40, 32);
                plabel[32] = 0;
            }
        } else if (vd[0] == 2 && !jol.ok && !(vd[7] & 1) && vd[88] == '%' && vd[89] == '/' &&
                   (vd[90] == '@' || vd[90] == 'C' || vd[90] == 'E')) {
            if (vd_parse(s, vd, &jol)) {
                jlevel = vd[90] == '@' ? 1 : vd[90] == 'C' ? 2 : 3;
                utf16be_to_utf8(vd + 40, 32, jlabel, sizeof jlabel);
            }
        }
    }
    if (!pvd.ok) {
        kprintf("iso9660: %s: no usable primary volume descriptor\n", bd->name);
        goto fail;
    }
    if (jol.ok && jol.bsize != pvd.bsize) jol.ok = 0;
    s->bsize = pvd.bsize;
    s->vol_bytes = (u64)pvd.vol_blocks * s->bsize;
    s->cache_mem = kmalloc((size_t)ISO_CACHE_N * s->bsize);
    s->ce_buf = kmalloc(s->bsize);
    if (!s->cache_mem || !s->ce_buf) { r = E_NOMEM; goto fail; }
    for (int i = 0; i < ISO_CACHE_N; i++) s->cache[i].buf = s->cache_mem + (size_t)i * s->bsize;

    const u8 *b;
    if (jol.ok && cache_get(s, jol.root_lba, &b)) {
        kprintf("iso9660: %s: Joliet root directory unreadable, using ISO 9660 names\n", bd->name);
        jol.ok = 0;
    }
    if (jol.ok) {
        s->names = NAMES_JOLIET;
        s->joliet_level = jlevel;
        s->root_lba = jol.root_lba;
        s->root_len = jol.root_len;
        memcpy(s->root_date, jol.root_date, 7);
    } else {
        s->names = NAMES_PLAIN;
        s->root_lba = pvd.root_lba;
        s->root_len = pvd.root_len;
        memcpy(s->root_date, pvd.root_date, 7);
        r = cache_get(s, pvd.root_lba, &b);
        if (r) {
            kprintf("iso9660: %s: root directory unreadable (%d)\n", bd->name, r);
            r = r == E_NODEV ? r : E_IO;
            goto fail;
        }
        /* SUSP "SP" entry in the root's "." record: Rock Ridge */
        u32 rl = b[0];
        if (rl >= 34 + 7 && rl <= s->bsize && b[32] == 1 && b[33] == 0 && b[34] == 'S' && b[35] == 'P' &&
            b[36] >= 7 && b[38] == 0xBE && b[39] == 0xEF) {
            s->names = NAMES_RR;
            s->rr_skip = b[40];
        }
    }
    if (s->vol_bytes > s->dev_bytes)
        kprintf("iso9660: %s: volume (%lu bytes) exceeds the device (%lu bytes)\n", bd->name, s->vol_bytes,
                s->dev_bytes);

    trim_label(plabel);
    bytes_to_utf8((const u8 *)plabel, (u32)strlen(plabel), lab, VFS_MAX_NAME + 1);
    if (s->names == NAMES_JOLIET) {
        trim_label(jlabel);
        if (jlabel[0]) strlcpy(lab, jlabel, VFS_MAX_NAME + 1);
    }
    copy_utf8(v->label, sizeof v->label, lab);
    v->ops = &iso_ops;
    v->fs_priv = s;
    strlcpy(v->fs_name, "ISO9660", sizeof v->fs_name);
    kprintf("iso9660: %s: %lu blocks of %u bytes, %s%s, label '%s'\n", bd->name, (u64)pvd.vol_blocks, s->bsize,
            s->names == NAMES_JOLIET ? "Joliet level " : s->names == NAMES_RR ? "Rock Ridge" : "ISO 9660 names",
            s->names == NAMES_JOLIET ? (jlevel == 1 ? "1" : jlevel == 2 ? "2" : "3") : "", v->label);
    kfree(vd);
    kfree(lab);
    return 0;
fail:
    if (s) iso_free(s);
    kfree(vd);
    kfree(lab);
    return r;
}
