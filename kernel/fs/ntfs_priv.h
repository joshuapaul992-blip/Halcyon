/* Halcyon OS - NTFS driver internals (milestone 7.5).
 *
 * Shared by ntfs.c (mount, safety rules, VFS operations), ntfs_mft.c (I/O,
 * caches, MFT records, attributes, run lists), ntfs_index.c (B+ tree
 * indexes), ntfs_alloc.c (cluster and MFT record allocation) and
 * ntfs_lznt1.c (decompression). See docs/DESIGN-M7.5.md. */
#ifndef FS_NTFS_PRIV_H
#define FS_NTFS_PRIV_H
#include "core/types.h"
#include "fs/vfs.h"
#include "fs/ntfs_layout.h"

#define NT_FB_N         40          /* cached MFT records / index blocks */
#define NT_PG_N         48          /* cached bitmap pages */
#define NT_PG_SIZE      4096u
#define NT_MAX_PIECES   16          /* volume pieces of one record / index block */
#define NT_MAX_DEPTH    24          /* index tree depth limit */
#define NT_MAX_EXTENTS  512         /* extents of one attribute */
#define NT_MAX_RUNS     (1u << 20)  /* runs of one attribute */
#define NT_MAX_ALIST    (256u * 1024u)
#define NT_MAX_NAME     255         /* UTF-16 units */
#define NT_FN_MAX       (sizeof(ntfs_file_name_t) + 2 * NT_MAX_NAME)
#define NT_IE_MAX       (ALIGN_UP(sizeof(ntfs_index_entry_t) + NT_FN_MAX, 8) + 8)
#define NT_LCN_HOLE     (-1)        /* a sparse run */
#define NT_KEY_REC      (1ull << 63)    /* cache key of an MFT record (else: an index block's volume offset) */
#define NT_FIRST_FREE   24          /* new files get MFT records from here on */
#define NT_SEC_NONE     0

/* where a read-only volume's reason comes from */
#define NT_RO_NONE      0
#define NT_RO_DEVICE    1
#define NT_RO_CHECK     2           /* dirty / inconsistent: chkdsk */
#define NT_RO_HIBER     3           /* hibernated / Fast Startup / log not clean */
#define NT_RO_INTERNAL  4           /* internal Windows disk, not allowed in the settings */
#define NT_RO_LAYOUT    5           /* a version / layout the writer does not handle */

typedef struct { i64 vcn, lcn; u64 len; } nrun_t;
typedef struct { nrun_t *r; u32 n, cap; } nrl_t;

/* a cached fixup-protected block: an MFT record or an index block, kept with
 * the fixups applied; written back with them reapplied in a copy */
typedef struct {
    u64 key;
    u32 size, stamp;
    u16 pins;
    u8  valid, dirty, mirror;       /* mirror: also write to $MFTMirr */
    u8  np;
    u64 poff[NT_MAX_PIECES];        /* volume byte offsets */
    u32 plen[NT_MAX_PIECES];
    u8 *buf;
} fblk_t;

/* a cached 4 KiB page of the volume (bitmaps) */
typedef struct {
    u64 off;
    u32 stamp;
    u8  valid, dirty;
    u8 *buf;
} npage_t;

typedef struct nnode nnode_t;

typedef struct ntfs {
    volume_t   *v;
    blockdev_t *bd;
    u32 dss;                        /* device sector size */
    u32 ss;                         /* NTFS sector size */
    u32 csize, cshift;
    u64 nclus;                      /* clusters in the volume */
    u64 vol_bytes;
    u32 rec_size, rec_shift;
    u32 ib_size;                    /* index block size for new directories */
    u64 mft_lcn, mirr_lcn;
    u64 serial;
    nrl_t mft_runs;                 /* $MFT:$DATA */
    u64 mft_size, mft_init, mft_alloc;
    u64 mft_nrec;                   /* records in the initialized part */
    nrl_t mftbmp_runs;              /* $MFT:$BITMAP (non-resident) */
    u64 mftbmp_size, mftbmp_alloc;
    u8  mftbmp_resident;
    u32 mirr_n;                     /* records mirrored in $MFTMirr */
    nrl_t mirr_runs;
    nrl_t bmp_runs;                 /* $Bitmap:$DATA */
    u64 bmp_size;
    u16 *upcase;
    u32 upcase_n;
    u64 free_clus;                  /* ~0: not counted yet */
    u64 alloc_hint, mft_zone_start, mft_zone_end;
    u64 mft_hint;
    u16 vol_flags;                  /* $VOLUME_INFORMATION flags as found at mount */
    u8  ver_major, ver_minor;
    u8  ro;                         /* NT_RO_*: why writes are refused (0 = writable) */
    u8  dirty_set;                  /* we set the dirty flag on disk */
    u8  log_reset;                  /* $LogFile was emptied in this session */
    u8  corrupt;                    /* corruption found: no more writes, the dirty flag stays */
    u32 sec_id;                     /* security id of the root, for new files without a parent id */
    fblk_t fb[NT_FB_N];
    u8 *fb_mem;
    u32 fb_bufsize;
    u32 stamp;
    npage_t pg[NT_PG_N];
    u8 *pg_mem;
    u8 *bounce;                     /* NT_PG_SIZE (>= the device sector size) */
    u8 *scan;                       /* bitmap scans: 4 KiB */
    nnode_t *nodes;
} ntfs_t;

/* an inode for the duration of an operation: the pinned base record and a
 * copy of the attribute list */
typedef struct {
    ntfs_t *fs;
    u64 rec;
    u16 seq;
    fblk_t *base;
    u8 *al;                         /* $ATTRIBUTE_LIST value (NULL: none) */
    u32 al_len;
    u8  al_nonres;                  /* the list is non-resident (read only) */
} nino_t;

/* attribute search: the attribute at rec->buf + off */
typedef struct {
    nino_t *ino;
    fblk_t *rec;                    /* pinned */
    u32 off;
    u32 pos;                        /* next attribute offset (record walk) or list position */
    u32 type;
    const u16 *name;
    u8  nlen, any_name;
} actx_t;

/* sizes and flags of an attribute (all its extents) */
typedef struct {
    u8  resident;
    u16 flags;                      /* NTFS_ATTR_* */
    u8  cu;                         /* compression unit (log2 clusters) */
    u64 size, alloc, init, comp;
    u32 value_off;                  /* resident: value offset in the attribute record */
} ainfo_t;

/* an open file or directory (shared by every handle of the same record) */
struct nnode {
    nnode_t *next;
    u32 refs;
    u64 rec;
    u16 seq;
    u8  is_dir;
    u8  nowrite;                    /* E_NOTSUP for writes (compressed, sparse, ...) */
    u8  runs_ok;
    ainfo_t ai;                     /* unnamed $DATA */
    nrl_t runs;
    u8 *cu_buf;                     /* the last decompressed compression unit */
    i64 cu_vcn;
};

/* a directory entry found by a lookup */
typedef struct {
    u64 mref;                       /* the file */
    u64 dir;                        /* its directory (MFT reference) */
    u32 fn_len;
    u8  fn[NT_FN_MAX];              /* the $FILE_NAME key of the entry */
    char name[VFS_MAX_NAME + 1];    /* display name */
} nent_t;

static inline ntfs_file_name_t *ent_fn(nent_t *e) { return (ntfs_file_name_t *)e->fn; }
static inline ntfs_attr_t *ac_attr(actx_t *c) { return (ntfs_attr_t *)(c->rec->buf + c->off); }
static inline ntfs_mft_record_t *fb_rec(fblk_t *b) { return (ntfs_mft_record_t *)b->buf; }
static inline u8 *attr_value(ntfs_attr_t *a) { return (u8 *)a + a->r.value_offset; }
static inline u16 *attr_name(ntfs_attr_t *a) { return (u16 *)((u8 *)a + a->name_offset); }
static inline u64 nt_rec_of(u64 mref) { return NTFS_MREF(mref); }

extern const u16 nt_I30[4];         /* "$I30" */

/* ---- ntfs_mft.c: I/O and caches ---- */
int  nt_dev_rw(ntfs_t *fs, u64 off, u32 len, void *buf, int write);
int  nt_runs_rw(ntfs_t *fs, const nrl_t *rl, u64 pos, u32 len, void *buf, int write);
int  nt_meta_rw(ntfs_t *fs, const nrl_t *rl, u64 pos, u32 len, void *buf, int write);
int  nt_flush(ntfs_t *fs);
void nt_inval_range(ntfs_t *fs, u64 off, u64 len);
void nt_corrupt(ntfs_t *fs, const char *what, u64 rec);
int  nt_fb_get(ntfs_t *fs, u64 key, u32 size, const u64 *poff, const u32 *plen, int np, int fresh, fblk_t **out);
void nt_fb_put(ntfs_t *fs, fblk_t *b);
static inline void nt_fb_dirty(fblk_t *b) { b->dirty = 1; }

/* run lists */
void nrl_free(nrl_t *rl);
int  nrl_add(nrl_t *rl, i64 vcn, i64 lcn, u64 len);
i64  nrl_lcn(const nrl_t *rl, i64 vcn, u64 *contig);     /* NT_LCN_HOLE for a hole, -2 outside */
u64  nrl_vcns(const nrl_t *rl);
int  nrl_decode(ntfs_t *fs, const ntfs_attr_t *a, nrl_t *rl);
int  nrl_encode(const nrl_t *rl, i64 from_vcn, i64 to_vcn, u8 *out, u32 cap);  /* bytes, or -1 */
int  nrl_truncate(nrl_t *rl, u64 vcns);

/* records */
int  nt_rec_get(ntfs_t *fs, u64 rec, fblk_t **out);
int  nt_rec_get_fresh(ntfs_t *fs, u64 rec, fblk_t **out);
int  nt_rec_valid(ntfs_t *fs, const u8 *buf, u64 rec);
int  nt_rec_walk(ntfs_t *fs, fblk_t *b, u32 *pos, u32 *off, ntfs_attr_t **out);
int  nt_rec_attr_resize(ntfs_t *fs, fblk_t *b, u32 off, u32 newlen);
int  nt_rec_attr_insert(ntfs_t *fs, fblk_t *b, const ntfs_attr_t *a, u32 *off_out);
void nt_rec_attr_remove(ntfs_t *fs, fblk_t *b, u32 off);
u32  nt_rec_free_space(fblk_t *b);
int  nt_rec_init(ntfs_t *fs, fblk_t *b, u64 rec, u16 seq, u16 flags);

/* inodes and attributes */
int  nt_ino_open(ntfs_t *fs, u64 mref, int check_seq, nino_t *ino);
void nt_ino_close(nino_t *ino);
int  nt_attr_first(nino_t *ino, actx_t *c, u32 type, const u16 *name, u8 nlen, int any_name);
int  nt_attr_next(actx_t *c);
void nt_actx_done(actx_t *c);
int  nt_attr_find(nino_t *ino, actx_t *c, u32 type, const u16 *name, u8 nlen);
int  nt_attr_info(nino_t *ino, u32 type, const u16 *name, u8 nlen, ainfo_t *ai, nrl_t *rl);
int  nt_attr_read(nino_t *ino, u32 type, const u16 *name, u8 nlen, u64 pos, u32 len, void *buf);
int  nt_attr_write_meta(nino_t *ino, u32 type, const u16 *name, u8 nlen, u64 pos, u32 len, const void *buf);
int  nt_attr_set_resident(nino_t *ino, u32 type, const u16 *name, u8 nlen, const void *val, u32 len);
int  nt_attr_add_resident(nino_t *ino, u32 type, const u16 *name, u8 nlen, const void *val, u32 len, u8 rflags);
int  nt_attr_make_nonres(nino_t *ino, u32 type, const u16 *name, u8 nlen, u64 min_alloc);
int  nt_attr_store_runs(nino_t *ino, u32 type, const u16 *name, u8 nlen, const nrl_t *rl, const ainfo_t *ai);
int  nt_attr_set_sizes(nino_t *ino, u32 type, const u16 *name, u8 nlen, const ainfo_t *ai);
int  nt_attr_remove(nino_t *ino, actx_t *c);
int  nt_attr_resize_nr(nino_t *ino, u32 type, const u16 *name, u8 nlen, u64 new_size, int zero_new);
int  nt_ino_free_all(nino_t *ino);

/* ---- ntfs_alloc.c ---- */
int  nt_bmp_count_free(ntfs_t *fs);
int  nt_clus_alloc(ntfs_t *fs, u64 n, u64 hint, int mft_zone, nrl_t *rl, i64 vcn);
int  nt_clus_free_runs(ntfs_t *fs, const nrl_t *rl, u64 from_vcn);
int  nt_clus_free(ntfs_t *fs, i64 lcn, u64 n);
int  nt_mft_alloc(ntfs_t *fs, u64 *rec);
int  nt_mft_free(ntfs_t *fs, u64 rec);
int  nt_mft_bit(ntfs_t *fs, u64 rec, int *used);

/* ---- ntfs_index.c ---- */
typedef int (*ncoll_fn)(ntfs_t *fs, const u8 *a, u32 al, const u8 *b, u32 bl);
int  nt_coll_filename(ntfs_t *fs, const u8 *a, u32 al, const u8 *b, u32 bl);
int  nt_coll_ulongs(ntfs_t *fs, const u8 *a, u32 al, const u8 *b, u32 bl);
int  nt_name_cmp(ntfs_t *fs, const u16 *a, u32 al, const u16 *b, u32 bl, int ic);
u16  nt_up(ntfs_t *fs, u16 c);

typedef struct {
    nino_t *ino;
    const u16 *name;
    u8 nlen;
    ncoll_fn coll;
    u32 bsize;                      /* index block size */
    u32 vunit_shift;                /* log2 bytes per index VCN */
    u8  cpb;                        /* clusters_per_index_block field */
    u8  have_alloc;
    nrl_t runs;                     /* $INDEX_ALLOCATION */
    ainfo_t aa;
} nidx_t;

int  nt_idx_open(nino_t *ino, const u16 *name, u8 nlen, ncoll_fn coll, nidx_t *ix);
void nt_idx_close(nidx_t *ix);
/* find an entry by key: exact (collation equal) or, for $I30 with ic, case
 * insensitive; copies the entry (cap bytes) */
int  nt_idx_lookup(nidx_t *ix, const u8 *key, u32 klen, int ic, u8 *out, u32 cap);
int  nt_idx_walk(nidx_t *ix, int (*cb)(void *ctx, const ntfs_index_entry_t *e), void *ctx);
int  nt_idx_insert(nidx_t *ix, const ntfs_index_entry_t *e);
int  nt_idx_delete(nidx_t *ix, const u8 *key, u32 klen, u64 file);
int  nt_idx_update(nidx_t *ix, const ntfs_index_entry_t *e);   /* same key and length: rewrite in place */
int  nt_idx_empty(nidx_t *ix);                                  /* 1 if no entries */

/* ---- ntfs_lznt1.c ---- */
int  nt_lznt1_decompress(const u8 *in, u32 inlen, u8 *out, u32 outlen);

/* ---- ntfs.c ---- */
i64  nt_now(void);
int  nt_writable(ntfs_t *fs);       /* first write: mark dirty, empty $LogFile */

#endif
