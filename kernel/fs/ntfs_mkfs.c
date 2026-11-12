/* Halcyon OS - the NTFS formatter (milestone 7.5).
 *
 * ntfs_format() writes an empty NTFS 3.1 volume the way Windows lays one
 * out (docs/DESIGN-M7.5-FORMAT.md has the details and the reasons):
 *
 *   cluster 0      $Boot: the boot sector (code that says the disk is not
 *                  bootable), 8 KiB; the backup boot sector is the last
 *                  sector of the device, just past the volume
 *   next           $MFTMirr (the first 4 records, or one cluster's worth)
 *   next           $AttrDef, the root directory's index block, $UpCase,
 *                  $Secure:$SDS, $Bitmap, $MFT:$BITMAP, $LogFile
 *   then           $MFT (64 records to start with); on volumes of 12 GiB
 *                  and more it starts 3 GiB into the volume like Windows'.
 *                  Nothing follows it, so the MFT zone (the free space the
 *                  driver keeps for the MFT to grow into, 1/8 of the
 *                  volume) is free.
 *
 * MFT records: 0-11 the system files, 12-15 reserved (in use, empty),
 * 16-23 free but never handed out (they are kept for $MFT's own extension
 * records), 24-26 $Extend's $Quota, $ObjId and $Reparse, the rest free.
 * Every file's security comes from $Secure (security ids 0x100-0x102):
 * there are no $SECURITY_DESCRIPTOR attributes. $LogFile has two clean
 * restart pages and an empty log; $Volume says version 3.1, not dirty.
 *
 * Only metadata is written (quick format). Everything is 64-bit: volumes
 * up to 2^32 - 1 clusters (256 TiB with 64 KiB clusters).
 */
#include "core/types.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/unicode.h"
#include "fs/block.h"
#include "fs/ntfs_layout.h"
#include "fs/ntfs_mkfs.h"

#define INDEX_BLOCK   4096u
#define MFT_RECORDS   64u               /* initial $MFT size (records) */
#define SDS_MIRROR    0x40000u          /* $SDS: every 256 KiB block is followed by its copy */
#define BOOT_BYTES    8192u
#define UPCASE_BYTES  131072u
#define ATTRDEF_BYTES 2560u
#define LOG_PAGE      4096u
#define CHUNK         65536u            /* write buffer */
#define MAX_CLUSTERS  0xFFFFFFFFull

/* security ids of the descriptors written to $Secure */
#define SEC_SYSTEM    0x100             /* metadata files: SYSTEM and Administrators may read */
#define SEC_SYSTEM_RW 0x101             /* $Secure, $Extend and its files: read and write */
#define SEC_ROOT      0x102             /* the root directory: what Windows gives a new volume */

enum { X_BOOT, X_MIRR, X_ATTRDEF, X_ROOTIDX, X_UPCASE, X_SDS, X_BITMAP, X_MFTBMP, X_LOG, X_MFT, X_COUNT };

typedef struct extent { u64 lcn, n; } extent_t;

typedef struct sec_entry { u32 id, hash, off, len; } sec_entry_t;

typedef struct mk {
    blockdev_t *bd;
    u32 ss, cs, rs;                     /* sector, cluster, MFT record bytes */
    u64 nclus;                          /* clusters in the volume */
    u64 total_sectors;                  /* sectors in the volume (the device minus the backup boot sector) */
    i64 now;                            /* NTFS time */
    u64 serial;
    extent_t x[X_COUNT];
    u64 log_bytes, bitmap_bytes, sds_bytes;
    u32 mft_recs, mirr_recs, mftbmp_bytes;
    u8 *mft;                            /* mft_recs records, built in memory */
    u8 *buf;                            /* CHUNK bytes */
    u16 *upcase;
    u8 sds[1024];                       /* the first $SDS block's entries */
    u32 sds_used;
    sec_entry_t sec[3];
    u16 label[NTFS_LABEL_MAX];
    int nlabel;
} mk_t;

/* ------------------------------------------------------------------ */
/* little helpers                                                       */

static void w16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void w32(u8 *p, u32 v) { w16(p, (u16)v); w16(p + 2, (u16)(v >> 16)); }
static void w64(u8 *p, u64 v) { w32(p, (u32)v); w32(p + 4, (u32)(v >> 32)); }
static u32 r32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

static u64 div_up(u64 a, u64 b) { return (a + b - 1) / b; }

static int log2u(u64 v) { int n = 0; while (v > 1) { v >>= 1; n++; } return n; }

/* RTC local time (Halcyon keeps no time zone) as NTFS time */
static i64 ntfs_now(void) {
    struct rtc_time t;
    rtc_read(&t);
    i64 y = t.year - (t.month <= 2), m = t.month;
    i64 era = (y >= 0 ? y : y - 399) / 400;
    i64 yoe = y - era * 400;
    i64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + t.day - 1;
    i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    i64 days = era * 146097 + doe - 719468;     /* since 1970-01-01 */
    i64 secs = days * 86400 + t.hour * 3600 + t.minute * 60 + t.second;
    if (secs < 0) secs = 0;
    return (secs + 11644473600ll) * 10000000ll;
}

/* ASCII -> UTF-16 units; returns the count */
static int ascii16(const char *s, u16 *out) {
    int n = 0;
    while (s[n]) { out[n] = (u8)s[n]; n++; }
    return n;
}

/* ------------------------------------------------------------------ */
/* writing                                                              */

/* write n clusters from lcn: src (srclen bytes), the rest `fill` */
static int put_clusters(mk_t *m, u64 lcn, u64 n, const u8 *src, u64 srclen, u8 fill) {
    u64 spc = m->cs / m->ss, sec = lcn * spc, left = n * m->cs, done = 0;
    while (left) {
        u32 k = (u32)MIN((u64)CHUNK, left);
        if (src && done < srclen) {
            u32 c = (u32)MIN((u64)k, srclen - done);
            memcpy(m->buf, src + done, c);
            memset(m->buf + c, fill, k - c);
        } else {
            memset(m->buf, fill, k);
        }
        int r = blk_write(m->bd, sec, k / m->ss, m->buf);
        if (r) return r;
        sec += k / m->ss;
        done += k;
        left -= k;
    }
    return 0;
}

static int put_extent(mk_t *m, int x, const u8 *src, u64 srclen, u8 fill) {
    return put_clusters(m, m->x[x].lcn, m->x[x].n, src, srclen, fill);
}

/* ------------------------------------------------------------------ */
/* MFT records                                                          */

typedef struct rec { u8 *b; u32 off; u16 inst; } rec_t;

static u8 *mft_rec(mk_t *m, u32 n) { return m->mft + (size_t)n * m->rs; }

/* an empty record: header, update sequence array, end marker */
static void rec_init(mk_t *m, rec_t *r, u32 n, u16 seq, u16 flags, u16 links) {
    u8 *b = mft_rec(m, n);
    memset(b, 0, m->rs);
    ntfs_mft_record_t *h = (ntfs_mft_record_t *)b;
    h->magic = NTFS_MAGIC_FILE;
    h->usa_offset = sizeof(ntfs_mft_record_t);
    h->usa_count = (u16)(m->rs / NTFS_FIXUP_STRIDE + 1);
    h->sequence = seq;
    h->link_count = links;
    h->attrs_offset = (u16)ALIGN_UP(h->usa_offset + 2u * h->usa_count, 8u);
    h->flags = flags;
    h->bytes_allocated = m->rs;
    h->record_number = n;
    r->b = b;
    r->off = h->attrs_offset;
    r->inst = 0;
    w32(b + r->off, NTFS_AT_END);
    h->bytes_in_use = r->off + 8;
}

static void rec_close(rec_t *r) {
    ntfs_mft_record_t *h = (ntfs_mft_record_t *)r->b;
    w32(r->b + r->off, NTFS_AT_END);
    w32(r->b + r->off + 4, 0);
    h->bytes_in_use = r->off + 8;
    h->next_attr_instance = r->inst;
}

/* attribute header common part; returns the attribute */
static ntfs_attr_t *attr_begin(rec_t *r, u32 type, const char *name, int nonres, u32 hdr, u32 *name_end) {
    ntfs_attr_t *a = (ntfs_attr_t *)(r->b + r->off);
    u16 nm[16];
    int nl = name ? ascii16(name, nm) : 0;
    memset(a, 0, hdr);
    a->type = type;
    a->non_resident = (u8)nonres;
    a->name_length = (u8)nl;
    a->name_offset = (u16)hdr;
    a->instance = r->inst++;
    for (int i = 0; i < nl; i++) w16((u8 *)a + hdr + 2 * i, nm[i]);
    *name_end = (u32)ALIGN_UP(hdr + 2u * nl, 8u);
    return a;
}

/* a resident attribute; returns a pointer to its value (len bytes, zeroed) */
static u8 *attr_res(rec_t *r, u32 type, const char *name, u32 len, u8 rflags) {
    u32 vo;
    ntfs_attr_t *a = attr_begin(r, type, name, 0, NTFS_ATTR_RESIDENT_HDR, &vo);
    a->r.value_length = len;
    a->r.value_offset = (u16)vo;
    a->r.resident_flags = rflags;
    a->length = (u32)ALIGN_UP(vo + len, 8u);
    memset((u8 *)a + vo, 0, a->length - vo);
    r->off += a->length;
    return (u8 *)a + vo;
}

/* signed minimal little-endian encoding for run lists; returns the bytes */
static int run_num(u8 *p, i64 v) {
    int n = 0;
    do {
        p[n++] = (u8)v;
        v >>= 8;
    } while (!((v == 0 && !(p[n - 1] & 0x80)) || (v == -1 && (p[n - 1] & 0x80))));
    return n;
}

/* a non-resident attribute with one run of `ncl` clusters at lcn (sparse if
 * lcn == ~0) */
static void attr_nonres(rec_t *r, u32 type, const char *name, u64 lcn, u64 ncl, u64 alloc, u64 data, u64 init) {
    u32 mp;
    ntfs_attr_t *a = attr_begin(r, type, name, 1, NTFS_ATTR_NONRES_HDR, &mp);
    u8 *p = (u8 *)a + mp, len[9], off[9];
    int o = 0;
    int nl = run_num(len, (i64)ncl), no = lcn == ~0ull ? 0 : run_num(off, (i64)lcn);
    p[o++] = (u8)(no << 4 | nl);
    memcpy(p + o, len, nl); o += nl;
    memcpy(p + o, off, no); o += no;
    p[o++] = 0;
    a->nr.lowest_vcn = 0;
    a->nr.highest_vcn = (i64)ncl - 1;
    a->nr.mapping_pairs_offset = (u16)mp;
    a->nr.allocated_size = (i64)alloc;
    a->nr.data_size = (i64)data;
    a->nr.initialized_size = (i64)init;
    a->length = (u32)ALIGN_UP(mp + (u32)o, 8u);
    r->off += a->length;
}

static void attr_nonres_x(mk_t *m, rec_t *r, u32 type, const char *name, int x, u64 data) {
    attr_nonres(r, type, name, m->x[x].lcn, m->x[x].n, m->x[x].n * m->cs, data, data);
}

static void std_info(mk_t *m, rec_t *r, u32 fattr, u32 secid) {
    ntfs_std_info_t *si = (ntfs_std_info_t *)attr_res(r, NTFS_AT_STANDARD_INFORMATION, NULL, sizeof(ntfs_std_info_t), 0);
    si->creation_time = si->data_change_time = si->mft_change_time = si->access_time = m->now;
    si->file_attributes = fattr;
    si->security_id = secid;
}

/* a $FILE_NAME value in out; returns its length */
static u32 fn_build(mk_t *m, u8 *out, u64 parent, const char *name, u32 fattr, u64 alloc, u64 size) {
    ntfs_file_name_t *f = (ntfs_file_name_t *)out;
    memset(f, 0, sizeof *f);
    f->parent = parent;
    f->creation_time = f->data_change_time = f->mft_change_time = f->access_time = m->now;
    f->allocated_size = (i64)alloc;
    f->data_size = (i64)size;
    f->file_attributes = fattr;
    f->name_length = (u8)ascii16(name, f->name);
    f->name_type = NTFS_NS_WIN32_AND_DOS;
    return (u32)sizeof *f + 2u * f->name_length;
}

/* the system files' names as their parents' index entries see them */
typedef struct sysname { u64 mref; u8 fn[sizeof(ntfs_file_name_t) + 2 * 16]; u32 len; } sysname_t;

static void file_name(mk_t *m, rec_t *r, sysname_t *s, u32 recno, u64 parent, const char *name, u32 fattr,
                      u64 alloc, u64 size) {
    s->len = fn_build(m, s->fn, parent, name, fattr, alloc, size);
    s->mref = NTFS_MK_MREF(recno, ((ntfs_mft_record_t *)r->b)->sequence);
    memcpy(attr_res(r, NTFS_AT_FILE_NAME, NULL, s->len, NTFS_RESIDENT_INDEXED), s->fn, s->len);
}

/* ------------------------------------------------------------------ */
/* indexes                                                              */

/* the index root header (32 bytes) for a view index or $I30 */
static void index_root_hdr(mk_t *m, u8 *v, u32 type, u32 coll, u32 entries_len, u8 ihflags) {
    ntfs_index_root_t *ir = (ntfs_index_root_t *)v;
    ir->type = type;
    ir->collation_rule = coll;
    ir->index_block_size = INDEX_BLOCK;
    ir->clusters_per_index_block = (u8)(m->cs <= INDEX_BLOCK ? INDEX_BLOCK / m->cs : INDEX_BLOCK / NTFS_FIXUP_STRIDE);
    ir->ih.entries_offset = sizeof(ntfs_index_header_t);
    ir->ih.index_length = ir->ih.allocated_size = sizeof(ntfs_index_header_t) + entries_len;
    ir->ih.flags = ihflags;
}

/* the last entry of a node (flags: NTFS_IE_END, maybe NTFS_IE_NODE with vcn 0) */
static u32 index_end(u8 *p, int node) {
    ntfs_index_entry_t *e = (ntfs_index_entry_t *)p;
    memset(p, 0, node ? 24 : 16);
    e->length = node ? 24 : 16;
    e->flags = NTFS_IE_END | (node ? NTFS_IE_NODE : 0);
    return e->length;
}

/* a $I30 entry for a system file */
static u32 index_name(u8 *p, const sysname_t *s) {
    ntfs_index_entry_t *e = (ntfs_index_entry_t *)p;
    u32 len = (u32)ALIGN_UP(sizeof *e + s->len, 8u);
    memset(p, 0, len);
    e->file = s->mref;
    e->length = (u16)len;
    e->key_length = (u16)s->len;
    memcpy(e->key, s->fn, s->len);
    return len;
}

/* a view index entry: key, data, then pad bytes of padding */
static u32 index_view(u8 *p, const void *key, u32 klen, const void *data, u32 dlen, const void *pad, u32 padlen) {
    ntfs_index_entry_t *e = (ntfs_index_entry_t *)p;
    u32 len = (u32)ALIGN_UP(sizeof *e + klen + dlen + padlen, 8u);
    memset(p, 0, len);
    e->view.data_offset = (u16)(sizeof *e + klen);
    e->view.data_length = (u16)dlen;
    e->length = (u16)len;
    e->key_length = (u16)klen;
    memcpy(e->key, key, klen);
    memcpy(e->key + klen, data, dlen);
    if (padlen) memcpy(e->key + klen + dlen, pad, padlen);
    return len;
}

/* file name collation: the names upcased through $UpCase, then as they are */
static int fn_cmp(const mk_t *m, const sysname_t *a, const sysname_t *b) {
    const ntfs_file_name_t *fa = (const ntfs_file_name_t *)a->fn, *fb = (const ntfs_file_name_t *)b->fn;
    int n = MIN(fa->name_length, fb->name_length);
    for (int i = 0; i < n; i++) {
        u16 x = m->upcase[fa->name[i]], y = m->upcase[fb->name[i]];
        if (x != y) return x < y ? -1 : 1;
    }
    if (fa->name_length != fb->name_length) return fa->name_length < fb->name_length ? -1 : 1;
    for (int i = 0; i < n; i++)
        if (fa->name[i] != fb->name[i]) return fa->name[i] < fb->name[i] ? -1 : 1;
    return 0;
}

static void sort_names(const mk_t *m, sysname_t **v, int n) {
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && fn_cmp(m, v[j - 1], v[j]) > 0; j--) {
            sysname_t *t = v[j]; v[j] = v[j - 1]; v[j - 1] = t;
        }
}

/* ------------------------------------------------------------------ */
/* security descriptors                                                 */

#define SID_SYSTEM    1     /* S-1-5-18 */
#define SID_ADMINS    2     /* S-1-5-32-544 */
#define SID_USERS     3     /* S-1-5-32-545 */
#define SID_AUTH      4     /* S-1-5-11 Authenticated Users */
#define SID_CREATOR   5     /* S-1-3-0 CREATOR OWNER */

static u32 sid_put(u8 *p, int which) {
    u8 auth = which == SID_CREATOR ? 3 : 5;
    u32 sub[2];
    int n = 1;
    switch (which) {
    case SID_SYSTEM: sub[0] = 18; break;
    case SID_ADMINS: sub[0] = 32; sub[1] = 544; n = 2; break;
    case SID_USERS:  sub[0] = 32; sub[1] = 545; n = 2; break;
    case SID_AUTH:   sub[0] = 11; break;
    default:         sub[0] = 0; break;
    }
    p[0] = 1;
    p[1] = (u8)n;
    memset(p + 2, 0, 5);
    p[7] = auth;                        /* identifier authority: 48-bit big-endian */
    for (int i = 0; i < n; i++) w32(p + 8 + 4 * i, sub[i]);
    return 8u + 4u * (u32)n;
}

typedef struct ace { int sid; u8 flags; u32 mask; } ace_t;

#define ACE_OI 0x01                     /* object inherit */
#define ACE_CI 0x02                     /* container inherit */
#define ACE_IO 0x08                     /* inherit only */

/* a self-relative descriptor: header, DACL, owner, group; returns its length */
static u32 sd_build(u8 *p, int owner, int group, const ace_t *aces, int n) {
    u32 o = 20, acl = o;
    memset(p, 0, 20);
    p[0] = 1;                           /* revision */
    w16(p + 2, 0x8004);                 /* SE_SELF_RELATIVE | SE_DACL_PRESENT */
    o += 8;
    for (int i = 0; i < n; i++) {
        u8 *a = p + o;
        a[0] = 0;                       /* ACCESS_ALLOWED_ACE_TYPE */
        a[1] = aces[i].flags;
        w32(a + 4, aces[i].mask);
        u32 sl = sid_put(a + 8, aces[i].sid);
        w16(a + 2, (u16)(8 + sl));
        o += 8 + sl;
    }
    p[acl] = 2;                         /* ACL revision */
    p[acl + 1] = 0;
    w16(p + acl + 2, (u16)(o - acl));
    w16(p + acl + 4, (u16)n);
    w16(p + acl + 6, 0);
    w32(p + 16, acl);                   /* DACL */
    w32(p + 4, o);                      /* owner */
    o += sid_put(p + o, owner);
    w32(p + 8, o);                      /* group */
    o += sid_put(p + o, group);
    w32(p + 12, 0);                     /* no SACL */
    return o;
}

static u32 sd_hash(const u8 *sd, u32 len) {
    u32 h = 0;
    for (u32 i = 0; i + 4 <= len; i += 4) h = ((h >> 29) | (h << 3)) + r32(sd + i);
    return h;
}

/* the $SDS entries (first block; the mirror copy follows at SDS_MIRROR) */
static void build_secure(mk_t *m) {
    static const ace_t sys_ro[] = { { SID_SYSTEM, 0, 0x00120089 }, { SID_ADMINS, 0, 0x00120089 } };
    static const ace_t sys_rw[] = { { SID_SYSTEM, 0, 0x0012019F }, { SID_ADMINS, 0, 0x0012019F } };
    static const ace_t root[] = {
        { SID_ADMINS, ACE_OI | ACE_CI, 0x001F01FF },            /* full control */
        { SID_SYSTEM, ACE_OI | ACE_CI, 0x001F01FF },
        { SID_AUTH, ACE_OI | ACE_CI, 0x001301BF },              /* modify */
        { SID_USERS, ACE_OI | ACE_CI, 0x001200A9 },             /* read and execute */
        { SID_CREATOR, ACE_OI | ACE_CI | ACE_IO, 0x10000000 },  /* GENERIC_ALL, for what they create */
    };
    struct { u32 id; int owner, group; const ace_t *a; int n; } d[3] = {
        { SEC_SYSTEM, SID_ADMINS, SID_ADMINS, sys_ro, 2 },
        { SEC_SYSTEM_RW, SID_ADMINS, SID_ADMINS, sys_rw, 2 },
        { SEC_ROOT, SID_ADMINS, SID_SYSTEM, root, 5 },
    };
    u32 o = 0;
    memset(m->sds, 0, sizeof m->sds);
    for (int i = 0; i < 3; i++) {
        u8 *e = m->sds + o;
        u32 sl = sd_build(e + 20, d[i].owner, d[i].group, d[i].a, d[i].n);
        u32 h = sd_hash(e + 20, sl);
        w32(e, h);
        w32(e + 4, d[i].id);
        w64(e + 8, o);
        w32(e + 16, 20 + sl);
        m->sec[i] = (sec_entry_t){ d[i].id, h, o, 20 + sl };
        m->sds_used = o + 20 + sl;
        o = (u32)ALIGN_UP(o + 20 + sl, 16u);
    }
    m->sds_bytes = SDS_MIRROR + m->sds_used;
}

/* ------------------------------------------------------------------ */
/* $AttrDef                                                             */

static const struct { const char *name; u32 type, coll, flags; i64 min, max; } attrdefs[] = {
    { "$STANDARD_INFORMATION", 0x10, 0, 0x40, 0x30, 0x48 },
    { "$ATTRIBUTE_LIST", 0x20, 0, 0x80, 0, -1 },
    { "$FILE_NAME", 0x30, 0, 0x42, 0x44, 0x242 },      /* (Windows leaves the collation 0 here too) */
    { "$OBJECT_ID", 0x40, 0, 0x40, 0, 0x100 },
    { "$SECURITY_DESCRIPTOR", 0x50, 0, 0x80, 0, -1 },
    { "$VOLUME_NAME", 0x60, 0, 0x40, 2, 0x100 },
    { "$VOLUME_INFORMATION", 0x70, 0, 0x40, 0x0C, 0x0C },
    { "$DATA", 0x80, 0, 0x00, 0, -1 },
    { "$INDEX_ROOT", 0x90, 0, 0x40, 0, -1 },
    { "$INDEX_ALLOCATION", 0xA0, 0, 0x80, 0, -1 },
    { "$BITMAP", 0xB0, 0, 0x80, 0, -1 },
    { "$REPARSE_POINT", 0xC0, 0, 0x80, 0, 0x4000 },
    { "$EA_INFORMATION", 0xD0, 0, 0x40, 0x08, 0x08 },
    { "$EA", 0xE0, 0, 0x00, 0, 0x10000 },
    { "$LOGGED_UTILITY_STREAM", 0x100, 0, 0x80, 0, 0x10000 },
};

static void build_attrdef(u8 *out) {
    memset(out, 0, ATTRDEF_BYTES);
    for (u32 i = 0; i < ARRAY_SIZE(attrdefs); i++) {
        ntfs_attr_def_t *d = (ntfs_attr_def_t *)(out + i * sizeof(ntfs_attr_def_t));
        ascii16(attrdefs[i].name, d->name);
        d->type = attrdefs[i].type;
        d->collation_rule = attrdefs[i].coll;
        d->flags = attrdefs[i].flags;
        d->min_size = attrdefs[i].min;
        d->max_size = attrdefs[i].max;
    }
}

/* ------------------------------------------------------------------ */
/* $LogFile                                                             */

/* a restart page: an empty log with one (free) client, "NTFS", marked
 * clean; written twice (offsets 0 and 4096) */
static void build_restart_page(mk_t *m, u8 *p) {
    memset(p, 0, LOG_PAGE);
    w32(p, NTFS_MAGIC_RSTR);
    w16(p + 4, 0x1E);                               /* usa_offset */
    w16(p + 6, LOG_PAGE / NTFS_FIXUP_STRIDE + 1);   /* usa_count */
    w64(p + 8, 0);                                  /* chkdsk_lsn */
    w32(p + 0x10, LOG_PAGE);                        /* system page size */
    w32(p + 0x14, LOG_PAGE);                        /* log page size */
    w16(p + 0x18, 0x30);                            /* restart area offset */
    w16(p + 0x1A, 1);                               /* minor version */
    w16(p + 0x1C, 1);                               /* major version: 1.1 */
    u8 *ra = p + 0x30;
    w64(ra, 0);                                     /* current_lsn: nothing logged */
    w16(ra + 8, 1);                                 /* log_clients */
    w16(ra + 10, 0);                                /* client_free_list: client 0 */
    w16(ra + 12, 0xFFFF);                           /* client_in_use_list: none */
    w16(ra + 14, 0x0002);                           /* flags: volume is clean */
    int bits = log2u(m->log_bytes) + 1;             /* bits of the file size */
    w32(ra + 16, (u32)(67 - bits));                 /* seq_number_bits: 64 - (bits - 3) */
    w16(ra + 20, 0x30 + 0xA0);                      /* restart_area_length */
    w16(ra + 22, 0x30);                             /* client_array_offset */
    w64(ra + 24, m->log_bytes);                     /* file_size */
    w32(ra + 32, 0);                                /* last_lsn_data_length */
    w16(ra + 36, 0x30);                             /* log_record_header_length */
    w16(ra + 38, 0x40);                             /* log_page_data_offset */
    w32(ra + 40, random_u32());                     /* restart_log_open_count */
    u8 *c = ra + 0x30;                              /* the client record */
    w64(c, 0);                                      /* oldest_lsn */
    w64(c + 8, 0);                                  /* client_restart_lsn */
    w16(c + 16, 0xFFFF);                            /* prev_client */
    w16(c + 18, 0xFFFF);                            /* next_client */
    w16(c + 20, 0);                                 /* seq_number */
    w32(c + 28, 8);                                 /* client_name_length (bytes) */
    u16 nm[4];
    ascii16("NTFS", nm);
    for (int i = 0; i < 4; i++) w16(c + 32 + 2 * i, nm[i]);
    ntfs_fixup_write(p, LOG_PAGE, NTFS_FIXUP_STRIDE);
}

/* $LogFile size: 1/64 of the volume between 256 KiB and 64 MiB (Windows
 * uses 64 MiB on volumes of a few GiB and more, less on small ones) */
static u64 log_size(u64 vol_bytes, u32 cs) {
    u64 l = vol_bytes / 64;
    l = CLAMP(l, 256ull * 1024, 64ull * 1024 * 1024);
    l = ALIGN_DOWN(l, 65536ull);
    return ALIGN_UP(l, (u64)MAX(cs, LOG_PAGE));
}

/* ------------------------------------------------------------------ */
/* the boot sector                                                      */

/* real-mode code at 0x54 (the boot sector's jump lands there): print the
 * message below with the BIOS, wait for a key, try the next boot device */
static const u8 boot_code[] = {
    0xFA,                   /* cli */
    0x31, 0xC0,             /* xor ax, ax */
    0x8E, 0xD8,             /* mov ds, ax */
    0x8E, 0xD0,             /* mov ss, ax */
    0xBC, 0x00, 0x7C,       /* mov sp, 0x7c00 */
    0xFB,                   /* sti */
    0xFC,                   /* cld */
    0xBE, 0x7A, 0x7C,       /* mov si, 0x7c7a (the message) */
    0xAC,                   /* .next: lodsb */
    0x84, 0xC0,             /* test al, al */
    0x74, 0x09,             /* jz .done */
    0xB4, 0x0E,             /* mov ah, 0x0e */
    0xBB, 0x07, 0x00,       /* mov bx, 7 */
    0xCD, 0x10,             /* int 0x10 */
    0xEB, 0xF2,             /* jmp .next */
    0x31, 0xC0,             /* .done: xor ax, ax */
    0xCD, 0x16,             /* int 0x16 (wait for a key) */
    0xCD, 0x19,             /* int 0x19 (next boot device) */
    0xF4,                   /* hlt */
    0xEB, 0xFD,             /* jmp hlt */
};
static const char boot_msg[] = "\r\nThis is not a bootable disk (an NTFS data volume formatted by Halcyon).\r\n"
                               "Remove it and press any key to try the next boot device.\r\n";

static void build_boot(mk_t *m, u8 *b) {
    memset(b, 0, m->ss);
    ntfs_boot_t *bs = (ntfs_boot_t *)b;
    bs->jump[0] = 0xEB; bs->jump[1] = 0x52; bs->jump[2] = 0x90;
    memcpy(bs->oem, "NTFS    ", 8);
    bs->bytes_per_sector = (u16)m->ss;
    bs->sectors_per_cluster = (u8)(m->cs / m->ss);
    bs->media = 0xF8;
    bs->sectors_per_track = 63;
    bs->heads = 255;
    if (m->bd->type == BDEV_PART && m->bd->part_start <= 0xFFFFFFFFull) bs->hidden_sectors = (u32)m->bd->part_start;
    bs->unused3 = 0x00800080;
    bs->total_sectors = m->total_sectors;
    bs->mft_lcn = m->x[X_MFT].lcn;
    bs->mftmirr_lcn = m->x[X_MIRR].lcn;
    bs->clusters_per_mft_record = (i8)(m->rs >= m->cs ? (int)(m->rs / m->cs) : -log2u(m->rs));
    bs->clusters_per_index_block = (i8)(INDEX_BLOCK >= m->cs ? (int)(INDEX_BLOCK / m->cs) : -log2u(INDEX_BLOCK));
    bs->serial = m->serial;
    _Static_assert(sizeof boot_code + sizeof boot_msg <= sizeof bs->code, "boot code");
    _Static_assert(0x54 + sizeof boot_code == 0x7A, "boot message address");
    memcpy(bs->code, boot_code, sizeof boot_code);
    memcpy(bs->code + sizeof boot_code, boot_msg, sizeof boot_msg);
    bs->signature = 0xAA55;
}

/* ------------------------------------------------------------------ */
/* the system files                                                     */

#define FA_SYS (NTFS_FA_HIDDEN | NTFS_FA_SYSTEM)

/* the MFT records in m->mft (without fixups), the root's index block in blk */
static void build_mft(mk_t *m, u8 *blk) {
    rec_t r;
    sysname_t nm[12], ext[3];
    u64 root = NTFS_MK_MREF(NTFS_FILE_ROOT, NTFS_FILE_ROOT), extend = NTFS_MK_MREF(NTFS_FILE_EXTEND, NTFS_FILE_EXTEND);
    u64 mft_bytes = (u64)m->mft_recs * m->rs, mirr_bytes = (u64)m->mirr_recs * m->rs;
#define ALLOC(e) (m->x[e].n * m->cs)

    for (u32 i = 0; i < m->mft_recs; i++)       /* free records */
        rec_init(m, &r, i, (u16)(i >= 16 && i < 24 ? i : 1), 0, 0);

    /* 0 $MFT */
    rec_init(m, &r, NTFS_FILE_MFT, 1, NTFS_MFT_IN_USE, 1);
    std_info(m, &r, FA_SYS, SEC_SYSTEM);
    file_name(m, &r, &nm[0], 0, root, "$MFT", FA_SYS, ALLOC(X_MFT), mft_bytes);
    attr_nonres_x(m, &r, NTFS_AT_DATA, NULL, X_MFT, mft_bytes);
    attr_nonres_x(m, &r, NTFS_AT_BITMAP, NULL, X_MFTBMP, m->mftbmp_bytes);
    rec_close(&r);
    /* 1 $MFTMirr */
    rec_init(m, &r, NTFS_FILE_MFTMIRR, NTFS_FILE_MFTMIRR, NTFS_MFT_IN_USE, 1);
    std_info(m, &r, FA_SYS, SEC_SYSTEM);
    file_name(m, &r, &nm[1], 1, root, "$MFTMirr", FA_SYS, ALLOC(X_MIRR), mirr_bytes);
    attr_nonres_x(m, &r, NTFS_AT_DATA, NULL, X_MIRR, mirr_bytes);
    rec_close(&r);
    /* 2 $LogFile */
    rec_init(m, &r, NTFS_FILE_LOGFILE, NTFS_FILE_LOGFILE, NTFS_MFT_IN_USE, 1);
    std_info(m, &r, FA_SYS, SEC_SYSTEM);
    file_name(m, &r, &nm[2], 2, root, "$LogFile", FA_SYS, ALLOC(X_LOG), m->log_bytes);
    attr_nonres_x(m, &r, NTFS_AT_DATA, NULL, X_LOG, m->log_bytes);
    rec_close(&r);
    /* 3 $Volume */
    rec_init(m, &r, NTFS_FILE_VOLUME, NTFS_FILE_VOLUME, NTFS_MFT_IN_USE, 1);
    std_info(m, &r, FA_SYS, SEC_SYSTEM);
    file_name(m, &r, &nm[3], 3, root, "$Volume", FA_SYS, 0, 0);
    {
        u8 *v = attr_res(&r, NTFS_AT_VOLUME_NAME, NULL, 2u * (u32)m->nlabel, 0);
        for (int i = 0; i < m->nlabel; i++) w16(v + 2 * i, m->label[i]);
        ntfs_volume_info_t *vi = (ntfs_volume_info_t *)attr_res(&r, NTFS_AT_VOLUME_INFORMATION, NULL, sizeof *vi, 0);
        vi->major = 3;
        vi->minor = 1;
        vi->flags = 0;
        attr_res(&r, NTFS_AT_DATA, NULL, 0, 0);
    }
    rec_close(&r);
    /* 4 $AttrDef */
    rec_init(m, &r, NTFS_FILE_ATTRDEF, NTFS_FILE_ATTRDEF, NTFS_MFT_IN_USE, 1);
    std_info(m, &r, FA_SYS, SEC_SYSTEM);
    file_name(m, &r, &nm[4], 4, root, "$AttrDef", FA_SYS, ALLOC(X_ATTRDEF), ATTRDEF_BYTES);
    attr_nonres_x(m, &r, NTFS_AT_DATA, NULL, X_ATTRDEF, ATTRDEF_BYTES);
    rec_close(&r);
    /* 6 $Bitmap */
    rec_init(m, &r, NTFS_FILE_BITMAP, NTFS_FILE_BITMAP, NTFS_MFT_IN_USE, 1);
    std_info(m, &r, FA_SYS, SEC_SYSTEM);
    file_name(m, &r, &nm[6], 6, root, "$Bitmap", FA_SYS, ALLOC(X_BITMAP), m->bitmap_bytes);
    attr_nonres_x(m, &r, NTFS_AT_DATA, NULL, X_BITMAP, m->bitmap_bytes);
    rec_close(&r);
    /* 7 $Boot */
    rec_init(m, &r, NTFS_FILE_BOOT, NTFS_FILE_BOOT, NTFS_MFT_IN_USE, 1);
    std_info(m, &r, FA_SYS, SEC_SYSTEM);
    file_name(m, &r, &nm[7], 7, root, "$Boot", FA_SYS, ALLOC(X_BOOT), BOOT_BYTES);
    attr_nonres_x(m, &r, NTFS_AT_DATA, NULL, X_BOOT, BOOT_BYTES);
    rec_close(&r);
    /* 8 $BadClus: an empty $DATA and $Bad, a sparse stream as long as the volume */
    rec_init(m, &r, NTFS_FILE_BADCLUS, NTFS_FILE_BADCLUS, NTFS_MFT_IN_USE, 1);
    std_info(m, &r, FA_SYS, SEC_SYSTEM);
    file_name(m, &r, &nm[8], 8, root, "$BadClus", FA_SYS, 0, 0);
    attr_res(&r, NTFS_AT_DATA, NULL, 0, 0);
    attr_nonres(&r, NTFS_AT_DATA, "$Bad", ~0ull, m->nclus, m->nclus * m->cs, m->nclus * m->cs, 0);
    rec_close(&r);
    /* 9 $Secure: $SDS, $SDH, $SII */
    rec_init(m, &r, NTFS_FILE_SECURE, NTFS_FILE_SECURE, NTFS_MFT_IN_USE | NTFS_MFT_VIEW_INDEX, 1);
    std_info(m, &r, FA_SYS | NTFS_FA_DUP_VIEW_INDEX, SEC_SYSTEM_RW);
    file_name(m, &r, &nm[9], 9, root, "$Secure", FA_SYS | NTFS_FA_DUP_VIEW_INDEX, 0, 0);
    attr_nonres_x(m, &r, NTFS_AT_DATA, "$SDS", X_SDS, m->sds_bytes);
    {
        static const u8 pad_ii[4] = { 'I', 0, 'I', 0 };
        u8 tmp[512];
        u32 o = 0;
        /* $SDH: (hash, id) -> the $SDS header, sorted by hash then id */
        int ord[3] = { 0, 1, 2 };
        for (int i = 1; i < 3; i++)
            for (int j = i; j > 0; j--) {
                sec_entry_t *a = &m->sec[ord[j - 1]], *b = &m->sec[ord[j]];
                if (a->hash > b->hash || (a->hash == b->hash && a->id > b->id)) { int t = ord[j]; ord[j] = ord[j - 1]; ord[j - 1] = t; }
            }
        for (int k = 0; k < 3; k++) {
            sec_entry_t *s = &m->sec[ord[k]];
            u8 key[8], data[20];
            w32(key, s->hash); w32(key + 4, s->id);
            memcpy(data, m->sds + s->off, 20);
            o += index_view(tmp + o, key, 8, data, 20, pad_ii, 4);
        }
        o += index_end(tmp + o, 0);
        u8 *v = attr_res(&r, NTFS_AT_INDEX_ROOT, "$SDH", 32 + o, 0);
        index_root_hdr(m, v, 0, NTFS_COLLATION_SECURITY_HASH, o, 0);
        memcpy(v + 32, tmp, o);
        /* $SII: id -> the $SDS header (ids ascending) */
        o = 0;
        for (int k = 0; k < 3; k++) {
            u8 key[4];
            w32(key, m->sec[k].id);
            o += index_view(tmp + o, key, 4, m->sds + m->sec[k].off, 20, NULL, 0);
        }
        o += index_end(tmp + o, 0);
        v = attr_res(&r, NTFS_AT_INDEX_ROOT, "$SII", 32 + o, 0);
        index_root_hdr(m, v, 0, NTFS_COLLATION_ULONG, o, 0);
        memcpy(v + 32, tmp, o);
    }
    rec_close(&r);
    /* 10 $UpCase */
    rec_init(m, &r, NTFS_FILE_UPCASE, NTFS_FILE_UPCASE, NTFS_MFT_IN_USE, 1);
    std_info(m, &r, FA_SYS, SEC_SYSTEM);
    file_name(m, &r, &nm[10], 10, root, "$UpCase", FA_SYS, ALLOC(X_UPCASE), UPCASE_BYTES);
    attr_nonres_x(m, &r, NTFS_AT_DATA, NULL, X_UPCASE, UPCASE_BYTES);
    rec_close(&r);
    /* 12-15: reserved, in use and empty */
    for (u32 i = 12; i < 16; i++) {
        rec_init(m, &r, i, (u16)i, NTFS_MFT_IN_USE, 0);
        std_info(m, &r, FA_SYS, SEC_SYSTEM);
        attr_res(&r, NTFS_AT_DATA, NULL, 0, 0);
        rec_close(&r);
    }
    /* 24 $Quota: $O (owner SID -> owner id) and $Q (owner id -> quota) */
    {
        u8 tmp[512], sid[16], data[64];
        u32 o, sl;
        rec_init(m, &r, 24, 1, NTFS_MFT_IN_USE | NTFS_MFT_IN_EXTEND | NTFS_MFT_VIEW_INDEX, 1);
        std_info(m, &r, FA_SYS | NTFS_FA_ARCHIVE | NTFS_FA_DUP_VIEW_INDEX, SEC_SYSTEM_RW);
        file_name(m, &r, &ext[1], 24, extend, "$Quota", FA_SYS | NTFS_FA_ARCHIVE | NTFS_FA_DUP_VIEW_INDEX, 0, 0);
        sl = sid_put(sid, SID_ADMINS);
        u8 owner[4] = { 0x00, 0x01, 0, 0 };     /* owner id 0x100 */
        static const u8 zero4[4];
        o = index_view(tmp, sid, sl, owner, 4, zero4, 4);
        o += index_end(tmp + o, 0);
        u8 *v = attr_res(&r, NTFS_AT_INDEX_ROOT, "$O", 32 + o, 0);
        index_root_hdr(m, v, 0, NTFS_COLLATION_SID, o, 0);
        memcpy(v + 32, tmp, o);
        /* quota control entries: owner 1 holds the defaults; 0x100 Administrators */
        o = 0;
        for (int k = 0; k < 2; k++) {
            u8 key[4];
            w32(key, k ? 0x100 : 1);
            memset(data, 0, sizeof data);
            w32(data, 2);                       /* version */
            w32(data + 4, 1);                   /* flags: default limits */
            w64(data + 8, 0);                   /* bytes used */
            w64(data + 16, (u64)m->now);        /* change time */
            w64(data + 24, ~0ull);              /* threshold: none */
            w64(data + 32, ~0ull);              /* limit: none */
            w64(data + 40, 0);                  /* exceeded time */
            u32 dl = 48;
            if (k) dl += sid_put(data + 48, SID_ADMINS);
            o += index_view(tmp + o, key, 4, data, dl, NULL, 0);
        }
        o += index_end(tmp + o, 0);
        v = attr_res(&r, NTFS_AT_INDEX_ROOT, "$Q", 32 + o, 0);
        index_root_hdr(m, v, 0, NTFS_COLLATION_ULONG, o, 0);
        memcpy(v + 32, tmp, o);
        rec_close(&r);
    }
    /* 25 $ObjId, 26 $Reparse: empty view indexes */
    {
        static const struct { u32 rec; const char *name, *index; } vx[2] = { { 25, "$ObjId", "$O" }, { 26, "$Reparse", "$R" } };
        for (int k = 0; k < 2; k++) {
            rec_init(m, &r, vx[k].rec, 1, NTFS_MFT_IN_USE | NTFS_MFT_IN_EXTEND | NTFS_MFT_VIEW_INDEX, 1);
            std_info(m, &r, FA_SYS | NTFS_FA_ARCHIVE | NTFS_FA_DUP_VIEW_INDEX, SEC_SYSTEM_RW);
            file_name(m, &r, &ext[k ? 2 : 0], vx[k].rec, extend, vx[k].name,
                      FA_SYS | NTFS_FA_ARCHIVE | NTFS_FA_DUP_VIEW_INDEX, 0, 0);
            u8 *v = attr_res(&r, NTFS_AT_INDEX_ROOT, vx[k].index, 32 + 16, 0);
            index_root_hdr(m, v, 0, NTFS_COLLATION_ULONGS, 16, 0);
            index_end(v + 32, 0);
            rec_close(&r);
        }
    }
    /* 11 $Extend: a small index of its three files */
    rec_init(m, &r, NTFS_FILE_EXTEND, NTFS_FILE_EXTEND, NTFS_MFT_IN_USE | NTFS_MFT_DIRECTORY, 1);
    std_info(m, &r, FA_SYS, SEC_SYSTEM_RW);
    file_name(m, &r, &nm[11], 11, root, "$Extend", FA_SYS | NTFS_FA_DUP_FILE_NAME_INDEX, 0, 0);
    {
        sysname_t *v[3] = { &ext[0], &ext[1], &ext[2] };
        sort_names(m, v, 3);
        u8 tmp[512];
        u32 o = 0;
        for (int k = 0; k < 3; k++) o += index_name(tmp + o, v[k]);
        o += index_end(tmp + o, 0);
        u8 *iv = attr_res(&r, NTFS_AT_INDEX_ROOT, "$I30", 32 + o, 0);
        index_root_hdr(m, iv, NTFS_AT_FILE_NAME, NTFS_COLLATION_FILE_NAME, o, 0);
        memcpy(iv + 32, tmp, o);
    }
    rec_close(&r);
    /* 5 the root directory: its index lives in one index block (written by
     * write_root_index) under a root node that only points to it */
    rec_init(m, &r, NTFS_FILE_ROOT, NTFS_FILE_ROOT, NTFS_MFT_IN_USE | NTFS_MFT_DIRECTORY, 1);
    std_info(m, &r, FA_SYS, SEC_ROOT);
    file_name(m, &r, &nm[5], 5, root, ".", FA_SYS | NTFS_FA_DUP_FILE_NAME_INDEX, 0, 0);
    {
        u8 *iv = attr_res(&r, NTFS_AT_INDEX_ROOT, "$I30", 32 + 24, 0);
        index_root_hdr(m, iv, NTFS_AT_FILE_NAME, NTFS_COLLATION_FILE_NAME, 24, NTFS_INDEX_LARGE);
        index_end(iv + 32, 1);
        attr_nonres_x(m, &r, NTFS_AT_INDEX_ALLOCATION, "$I30", X_ROOTIDX, INDEX_BLOCK);
        u8 *bm = attr_res(&r, NTFS_AT_BITMAP, "$I30", 8, 0);
        bm[0] = 1;
    }
    rec_close(&r);
#undef ALLOC

    /* the root index block: every system file but the root itself */
    sysname_t *v[12];
    int n = 0;
    for (int i = 0; i < 12; i++) v[n++] = &nm[i];
    sort_names(m, v, n);
    memset(blk, 0, INDEX_BLOCK);
    ntfs_index_block_t *ib = (ntfs_index_block_t *)blk;
    ib->magic = NTFS_MAGIC_INDX;
    ib->usa_offset = sizeof *ib;
    ib->usa_count = INDEX_BLOCK / NTFS_FIXUP_STRIDE + 1;
    ib->vcn = 0;
    u32 eo = (u32)ALIGN_UP(sizeof *ib + 2u * ib->usa_count, 8u);
    ib->ih.entries_offset = eo - 0x18;
    u32 o = eo;
    for (int i = 0; i < n; i++) o += index_name(blk + o, v[i]);
    o += index_end(blk + o, 0);
    ib->ih.index_length = o - 0x18;
    ib->ih.allocated_size = INDEX_BLOCK - 0x18;
    ib->ih.flags = 0;
    ntfs_fixup_write(blk, INDEX_BLOCK, NTFS_FIXUP_STRIDE);
}

/* ------------------------------------------------------------------ */
/* layout                                                               */

u32 ntfs_default_cluster(u64 bytes, u32 sector_size) {
    u32 c = MAX(4096u, sector_size);
    while (bytes / c > MAX_CLUSTERS && c < 65536) c *= 2;
    return bytes / c > MAX_CLUSTERS ? 0 : c;
}

static int layout(mk_t *m, u64 vol_bytes) {
    u64 cs = m->cs, next = 0;
#define PLACE(e, bytes) do { m->x[e].lcn = next; m->x[e].n = div_up((bytes), cs); next += m->x[e].n; } while (0)
    m->mirr_recs = MAX(4u, m->cs / m->rs);
    m->mft_recs = (u32)(div_up((u64)MFT_RECORDS * m->rs, cs) * cs / m->rs);
    m->mftbmp_bytes = (u32)ALIGN_UP(div_up(m->mft_recs, 8), 8u);
    m->bitmap_bytes = ALIGN_UP(div_up(m->nclus, 8), 8ull);
    m->log_bytes = log_size(vol_bytes, m->cs);
    PLACE(X_BOOT, BOOT_BYTES);
    PLACE(X_MIRR, (u64)m->mirr_recs * m->rs);
    PLACE(X_ATTRDEF, ATTRDEF_BYTES);
    PLACE(X_ROOTIDX, INDEX_BLOCK);
    PLACE(X_UPCASE, UPCASE_BYTES);
    PLACE(X_SDS, m->sds_bytes);
    PLACE(X_BITMAP, m->bitmap_bytes);
    PLACE(X_MFTBMP, m->mftbmp_bytes);
    PLACE(X_LOG, m->log_bytes);
    /* on a large volume the MFT starts 3 GiB in, as Windows places it */
    u64 at3g = (3ull << 30) / cs;
    if (vol_bytes >= (12ull << 30) && at3g > next) next = at3g;
    PLACE(X_MFT, (u64)m->mft_recs * m->rs);
#undef PLACE
    /* the volume must hold all this and some room for files */
    if (next + 16 > m->nclus) return E_NOSPC;
    return 0;
}

/* $Bitmap: every extent in use, and the bits past the last cluster */
static int write_bitmap(mk_t *m) {
    u64 total = m->x[X_BITMAP].n * m->cs, spc = m->cs / m->ss;
    u64 sec = m->x[X_BITMAP].lcn * spc;
    for (u64 pos = 0; pos < total; pos += CHUNK) {
        u32 k = (u32)MIN((u64)CHUNK, total - pos);
        memset(m->buf, 0, k);
        u64 c0 = pos * 8, c1 = c0 + (u64)k * 8;      /* clusters this chunk covers */
        for (int x = 0; x < X_COUNT; x++) {
            u64 a = MAX(m->x[x].lcn, c0), b = MIN(m->x[x].lcn + m->x[x].n, c1);
            for (u64 c = a; c < b; c++) m->buf[(c - c0) / 8] |= (u8)(1u << (c & 7));
        }
        u64 a = MAX(m->nclus, c0), b = MIN(m->bitmap_bytes * 8, c1);
        for (u64 c = a; c < b; c++) m->buf[(c - c0) / 8] |= (u8)(1u << (c & 7));
        int r = blk_write(m->bd, sec + pos / m->ss, k / m->ss, m->buf);
        if (r) return r;
    }
    return 0;
}

static int write_volume(mk_t *m) {
    int r;
    u8 *tmp = NULL;
    /* invalidate the old boot sector first: a half-written volume must not mount */
    memset(m->buf, 0, m->ss);
    if ((r = blk_write(m->bd, 0, 1, m->buf))) return r;
    if (!(tmp = kzalloc(UPCASE_BYTES))) return E_NOMEM;
    /* the MFT and the root directory's index block */
    build_mft(m, tmp);
    if ((r = put_extent(m, X_ROOTIDX, tmp, INDEX_BLOCK, 0))) goto out;
    for (u32 i = 0; i < m->mft_recs; i++) ntfs_fixup_write(mft_rec(m, i), m->rs, NTFS_FIXUP_STRIDE);
    if ((r = put_extent(m, X_MFT, m->mft, (u64)m->mft_recs * m->rs, 0))) goto out;
    if ((r = put_extent(m, X_MIRR, m->mft, (u64)m->mirr_recs * m->rs, 0))) goto out;
    /* $MFT:$BITMAP: records 0-15 and 24-26 */
    {
        u8 b[8] = { 0xFF, 0xFF, 0x00, 0x07 };
        if ((r = put_extent(m, X_MFTBMP, b, 8, 0))) goto out;
    }
    /* $AttrDef */
    build_attrdef(tmp);
    if ((r = put_extent(m, X_ATTRDEF, tmp, ATTRDEF_BYTES, 0))) goto out;
    /* $UpCase (little-endian units) */
    for (u32 c = 0; c < 65536; c++) w16(tmp + 2 * c, m->upcase[c]);
    if ((r = put_extent(m, X_UPCASE, tmp, UPCASE_BYTES, 0))) goto out;
    /* $Secure:$SDS: the entries, zeros, their copy */
    {
        u64 lcn = m->x[X_SDS].lcn, half = SDS_MIRROR / m->cs, n = m->x[X_SDS].n;
        if (SDS_MIRROR % m->cs == 0) {
            if ((r = put_clusters(m, lcn, half, m->sds, m->sds_used, 0))) goto out;
            if ((r = put_clusters(m, lcn + half, n - half, m->sds, m->sds_used, 0))) goto out;
        } else {
            r = E_INVAL;    /* (clusters are at most 64 KiB) */
            goto out;
        }
    }
    /* $LogFile: two restart pages, then empty (0xFF) log pages */
    build_restart_page(m, tmp);
    memcpy(tmp + LOG_PAGE, tmp, LOG_PAGE);
    if ((r = put_extent(m, X_LOG, tmp, 2 * LOG_PAGE, 0xFF))) goto out;
    /* $Bitmap */
    if ((r = write_bitmap(m))) goto out;
    /* the boot sector: the backup (the device's last sector) first, then $Boot */
    memset(tmp, 0, BOOT_BYTES);
    build_boot(m, tmp);
    if ((r = blk_write(m->bd, m->total_sectors, 1, tmp))) goto out;
    r = put_extent(m, X_BOOT, tmp, BOOT_BYTES, 0);
out:
    kfree(tmp);
    return r;
}

int ntfs_format(blockdev_t *bd, const char *label, u32 cluster_bytes) {
    if (!bd || !bd->present) return E_NODEV;
    if (bd->readonly) return E_ROFS;
    u32 ss = bd->sector_size;
    if (ss < 512 || ss > 4096 || (ss & (ss - 1))) return E_NOTSUP;
    u64 bytes = bd->sector_count * ss;
    if (bd->sector_count < 2 || bytes < NTFS_MIN_VOLUME) return E_NOSPC;
    if (!cluster_bytes) cluster_bytes = ntfs_default_cluster(bytes, ss);
    if (!cluster_bytes) return E_INVAL;
    if (cluster_bytes < 512 || cluster_bytes > 65536 || (cluster_bytes & (cluster_bytes - 1)) || cluster_bytes < ss)
        return E_INVAL;
    mk_t *m = kzalloc(sizeof *m);
    if (!m) return E_NOMEM;
    m->bd = bd;
    m->ss = ss;
    m->cs = cluster_bytes;
    m->rs = MAX(1024u, ss);             /* 4 KiB records on 4 KiB sectors, as Windows */
    m->total_sectors = bd->sector_count - 1;
    m->nclus = m->total_sectors / (cluster_bytes / ss);
    int r = 0;
    if (m->nclus > MAX_CLUSTERS) { r = E_INVAL; goto out; }
    if (label && *label) {
        if (!utf8_valid(label)) { r = E_INVAL; goto out; }
        m->nlabel = utf8_to_utf16(label, m->label, NTFS_LABEL_MAX);
        if (m->nlabel < 0) { r = E_INVAL; goto out; }
        for (int i = 0; i < m->nlabel; i++)
            if (m->label[i] < 0x20 || (m->label[i] < 0x80 && strchr("\"*/:<>?\\|", m->label[i]))) { r = E_INVAL; goto out; }
    }
    m->now = ntfs_now();
    random_bytes(&m->serial, sizeof m->serial);
    build_secure(m);
    if ((r = layout(m, bytes))) goto out;
    m->buf = kmalloc(CHUNK);
    m->mft = kzalloc((size_t)m->mft_recs * m->rs);
    m->upcase = kmalloc(65536 * sizeof(u16));
    if (!m->buf || !m->mft || !m->upcase) { r = E_NOMEM; goto out; }
    ntfs_upcase_table(m->upcase);
    r = write_volume(m);
    kprintf("ntfs: format %s: %llu clusters of %u bytes, MFT at %llu, log %llu KiB, serial %016llX: %s\n", bd->name,
            (unsigned long long)m->nclus, m->cs, (unsigned long long)m->x[X_MFT].lcn,
            (unsigned long long)(m->log_bytes >> 10), (unsigned long long)m->serial, r ? strerror(r) : "done");
out:
    if (m->buf) kfree(m->buf);
    if (m->mft) kfree(m->mft);
    if (m->upcase) kfree(m->upcase);
    kfree(m);
    return r;
}
