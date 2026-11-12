/* Halcyon OS - NTFS: indexes ($I30 directories and the view indexes of
 * $Extend), B+ trees of $INDEX_ROOT and $INDEX_ALLOCATION blocks.
 *
 * Every entry of a node has a sub-node (NTFS_IE_NODE, the VCN in its last
 * 8 bytes) if the node is not a leaf, and each node ends with an END entry
 * (no key; in an inner node it points to the last sub-tree). An entry's
 * sub-node holds the keys smaller than the entry.
 *
 * Changes work on a copy of a node's entries (nbuf_t) and are written back
 * with nb_store. Insertion: into the leaf; a node that no longer fits is
 * split at its middle entry, which moves up into the parent (the left half
 * goes to a new block, the right half stays); a root that no longer fits
 * in its MFT record is pushed down into a new block. The blocks a split
 * can need are reserved before anything changes, so a full volume fails
 * the operation cleanly. Deletion: an entry in an inner node is replaced
 * by its predecessor (the last key of the leftmost-greater subtree's
 * rightmost leaf); a non-root node left without keys borrows a key
 * through its parent from a sibling (rotation) or is merged with it, up
 * the tree as needed; a root left without keys absorbs its only child if
 * that fits, and a directory whose entries all fit in the root again
 * drops its $INDEX_ALLOCATION. So every non-root node keeps at least one
 * key and all leaves stay at the same depth. */
#include "core/types.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/unicode.h"
#include "fs/ntfs_priv.h"

#define IE_HDR      ((u32)sizeof(ntfs_index_entry_t))
#define BLK_HDR     0x18u           /* the index header's offset in a block */
#define ROOT_HDR    0x10u           /* ... in the index root value */
#define EIO_AT() (kprintf("ntfs: index check failed (ntfs_index.c:%d)\n", __LINE__), E_IO)

/* ------------------------------------------------------------------ */
/* collation                                                            */

u16 nt_up(ntfs_t *fs, u16 c) {
    if (fs->upcase) return c < fs->upcase_n ? fs->upcase[c] : c;
    return (u16)uc_upper(c);
}

int nt_name_cmp(ntfs_t *fs, const u16 *a, u32 al, const u16 *b, u32 bl, int ic) {
    u32 n = MIN(al, bl);
    for (u32 i = 0; i < n; i++) {
        u16 x = a[i], y = b[i];
        if (ic) { x = nt_up(fs, x); y = nt_up(fs, y); }
        if (x != y) return x < y ? -1 : 1;
    }
    return al < bl ? -1 : al > bl;
}

static int fn_cmp(ntfs_t *fs, const u8 *a, u32 al, const u8 *b, u32 bl, int ic_only) {
    const ntfs_file_name_t *x = (const ntfs_file_name_t *)a, *y = (const ntfs_file_name_t *)b;
    if (al < sizeof *x || bl < sizeof *y) return al < bl ? -1 : al > bl;
    u32 xl = MIN((u32)x->name_length, (al - (u32)sizeof *x) / 2);
    u32 yl = MIN((u32)y->name_length, (bl - (u32)sizeof *y) / 2);
    int c = nt_name_cmp(fs, x->name, xl, y->name, yl, 1);
    if (c || ic_only) return c;
    return nt_name_cmp(fs, x->name, xl, y->name, yl, 0);
}

int nt_coll_filename(ntfs_t *fs, const u8 *a, u32 al, const u8 *b, u32 bl) { return fn_cmp(fs, a, al, b, bl, 0); }

int nt_coll_ulongs(ntfs_t *fs, const u8 *a, u32 al, const u8 *b, u32 bl) {
    u32 n = MIN(al, bl) / 4;
    for (u32 i = 0; i < n; i++) {
        u32 x, y;
        memcpy(&x, a + 4 * i, 4);
        memcpy(&y, b + 4 * i, 4);
        if (x != y) return x < y ? -1 : 1;
    }
    return al < bl ? -1 : al > bl;
}

/* ------------------------------------------------------------------ */
/* entries                                                              */

static inline ntfs_index_entry_t *IE(const u8 *p) { return (ntfs_index_entry_t *)p; }
static inline i64 ie_child(const ntfs_index_entry_t *e) { return *(const i64 *)((const u8 *)e + e->length - 8); }
static inline void ie_set_child(ntfs_index_entry_t *e, i64 v) { *(i64 *)((u8 *)e + e->length - 8) = v; }

/* the entries of a node, checked: every entry fits, keys fit, the last one
 * is END, NODE flags match large */
static int area_ok(nidx_t *ix, const u8 *p, u32 len, int large, u32 *used) {
    u32 o = 0;
    u64 nblk = ix->have_alloc ? ix->aa.size / ix->bsize : 0;
    while (o + IE_HDR <= len) {
        const ntfs_index_entry_t *e = IE(p + o);
        u32 l = e->length;
        if (l < IE_HDR || (l & 7) || l > len - o) return 0;
        int node = (e->flags & NTFS_IE_NODE) != 0;
        if (node != large) return 0;
        u32 room = l - IE_HDR - (node ? 8u : 0u);
        if (node && l < IE_HDR + 8) return 0;
        if (node) {
            i64 c = ie_child(e);
            if (c < 0 || ((u64)c << ix->vunit_shift) / ix->bsize >= nblk) return 0;
        }
        if (e->flags & NTFS_IE_END) { *used = o + l; return 1; }
        if (e->key_length > room || !e->key_length) return 0;
        if (ix->coll == nt_coll_filename) {
            const ntfs_file_name_t *fn = (const ntfs_file_name_t *)e->key;
            if (e->key_length < sizeof *fn || !fn->name_length ||
                sizeof *fn + 2u * fn->name_length > e->key_length) return 0;
        } else if ((u32)e->view.data_offset + e->view.data_length > l) {
            return 0;
        }
        o += l;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* open / close                                                         */

static int root_get(nidx_t *ix, actx_t *c, ntfs_index_root_t **ir) {
    int r = nt_attr_find(ix->ino, c, NTFS_AT_INDEX_ROOT, ix->name, ix->nlen);
    if (r) return r == E_NOENT ? E_IO : r;
    ntfs_attr_t *a = ac_attr(c);
    if (a->non_resident || a->r.value_length < sizeof(ntfs_index_root_t)) { nt_actx_done(c); return E_IO; }
    *ir = (ntfs_index_root_t *)attr_value(a);
    ntfs_index_header_t *ih = &(*ir)->ih;
    u32 vlen = a->r.value_length;
    if (ih->entries_offset < sizeof *ih || ih->entries_offset > ih->index_length ||
        ROOT_HDR + (u64)ih->index_length > vlen) {
        nt_actx_done(c);
        return E_IO;
    }
    return 0;
}

static int ix_load_alloc(nidx_t *ix) {
    nrl_free(&ix->runs);
    ix->have_alloc = 0;
    int r = nt_attr_info(ix->ino, NTFS_AT_INDEX_ALLOCATION, ix->name, ix->nlen, &ix->aa, &ix->runs);
    if (r == E_NOENT) return 0;
    if (r) return r;
    if (ix->aa.resident || (ix->aa.flags & (NTFS_ATTR_COMPRESSED | NTFS_ATTR_ENCRYPTED))) return E_IO;
    if (ix->aa.size > nrl_vcns(&ix->runs) << ix->ino->fs->cshift) return E_IO;
    ix->have_alloc = 1;
    return 0;
}

int nt_idx_open(nino_t *ino, const u16 *name, u8 nlen, ncoll_fn coll, nidx_t *ix) {
    ntfs_t *fs = ino->fs;
    memset(ix, 0, sizeof *ix);
    ix->ino = ino;
    ix->name = name;
    ix->nlen = nlen;
    ix->coll = coll;
    actx_t c;
    ntfs_index_root_t *ir;
    int r = root_get(ix, &c, &ir);
    if (r) { if (r == E_IO) nt_corrupt(fs, "damaged index root", ino->rec); return r; }
    u32 want = coll == nt_coll_filename ? NTFS_COLLATION_FILE_NAME : NTFS_COLLATION_ULONGS;
    u32 bs = ir->index_block_size;
    u8 cpb = ir->clusters_per_index_block;
    int bad = ir->collation_rule != want || bs < 512 || bs > fs->fb_bufsize || (bs & (bs - 1));
    nt_actx_done(&c);
    if (bad) {
        kprintf("ntfs: %s: record %lu: unsupported index (collation or block size)\n", fs->bd->name, (unsigned long)ino->rec);
        return E_NOTSUP;
    }
    ix->bsize = bs;
    ix->cpb = cpb;
    if (bs >= fs->csize) {
        ix->vunit_shift = fs->cshift;
    } else {
        u32 unit = cpb ? bs / cpb : 512;
        if (!unit || (unit & (unit - 1)) || unit < 256 || unit * cpb != bs) unit = 512;
        u32 sh = 0;
        while ((1u << sh) < unit) sh++;
        ix->vunit_shift = sh;
    }
    if ((r = ix_load_alloc(ix))) {
        nrl_free(&ix->runs);
        if (r == E_IO) nt_corrupt(fs, "damaged index allocation", ino->rec);
        return r;
    }
    return 0;
}

void nt_idx_close(nidx_t *ix) { nrl_free(&ix->runs); }

/* ------------------------------------------------------------------ */
/* blocks                                                               */

static int blk_get(nidx_t *ix, i64 vcn, int fresh, fblk_t **out) {
    ntfs_t *fs = ix->ino->fs;
    if (!ix->have_alloc || vcn < 0) return E_IO;
    u64 pos = (u64)vcn << ix->vunit_shift;
    if (pos % ix->bsize || pos + ix->bsize > ix->aa.size) return E_IO;
    u64 poff[NT_MAX_PIECES];
    u32 plen[NT_MAX_PIECES], left = ix->bsize;
    int np = 0;
    while (left) {
        u64 contig = 0;
        i64 lcn = nrl_lcn(&ix->runs, (i64)(pos >> fs->cshift), &contig);
        if (lcn < 0 || np >= NT_MAX_PIECES) return E_IO;
        u32 in = (u32)(pos & (fs->csize - 1));
        u32 k = (u32)MIN((u64)left, contig * fs->csize - in);
        poff[np] = ((u64)lcn << fs->cshift) + in;
        plen[np] = k;
        if (np && poff[np - 1] + plen[np - 1] == poff[np]) plen[np - 1] += k;
        else np++;
        pos += k;
        left -= k;
    }
    fblk_t *b;
    int r = nt_fb_get(fs, poff[0], ix->bsize, poff, plen, np, fresh, &b);
    if (r) return r;
    if (!fresh) {
        ntfs_index_block_t *h = (ntfs_index_block_t *)b->buf;
        ntfs_index_header_t *ih = &h->ih;
        if (h->magic != NTFS_MAGIC_INDX || h->vcn != vcn || ih->entries_offset < sizeof *ih ||
            ih->entries_offset > ih->index_length || BLK_HDR + (u64)ih->allocated_size > ix->bsize ||
            ih->index_length > ih->allocated_size) {
            if (!--b->pins && !b->dirty) b->valid = 0;
            return E_IO;
        }
    }
    *out = b;
    return 0;
}

/* ------------------------------------------------------------------ */
/* node buffers                                                         */

typedef struct {
    u8 *e;
    u32 len, cap;
    i64 vcn;                        /* -1: the root */
    int large;
} nbuf_t;

static void nb_free(nbuf_t *nb) { kfree(nb->e); nb->e = NULL; nb->len = nb->cap = 0; }

static int nb_reserve(nbuf_t *nb, u32 need) {
    if (need <= nb->cap) return 0;
    u32 nc = MAX(need, nb->cap * 2);
    u8 *n = krealloc(nb->e, nc);
    if (!n) return E_NOMEM;
    nb->e = n;
    nb->cap = nc;
    return 0;
}

static int nb_load(nidx_t *ix, i64 vcn, nbuf_t *nb) {
    ntfs_t *fs = ix->ino->fs;
    nb->vcn = vcn;
    nb->len = 0;
    const u8 *p;
    u32 len;
    int large, r;
    actx_t c;
    fblk_t *b = NULL;
    if (vcn < 0) {
        ntfs_index_root_t *ir;
        if ((r = root_get(ix, &c, &ir))) return r;
        p = (const u8 *)&ir->ih + ir->ih.entries_offset;
        len = ir->ih.index_length - ir->ih.entries_offset;
        large = (ir->ih.flags & NTFS_INDEX_LARGE) != 0;
    } else {
        if ((r = blk_get(ix, vcn, 0, &b))) {
            if (r == E_IO) nt_corrupt(fs, "damaged index block", ix->ino->rec);
            return r;
        }
        ntfs_index_header_t *ih = &((ntfs_index_block_t *)b->buf)->ih;
        p = (const u8 *)ih + ih->entries_offset;
        len = ih->index_length - ih->entries_offset;
        large = (ih->flags & NTFS_INDEX_LARGE) != 0;
    }
    u32 used = 0;
    if (!area_ok(ix, p, len, large, &used)) r = E_IO;
    else if (!(r = nb_reserve(nb, used + 64))) {
        memcpy(nb->e, p, used);
        nb->len = used;
        nb->large = large;
    }
    if (vcn < 0) nt_actx_done(&c);
    else nt_fb_put(fs, b);
    if (r == E_IO) nt_corrupt(fs, "damaged index node", ix->ino->rec);
    return r;
}

/* entries of nb: offsets */
static u32 nb_count(const nbuf_t *nb) {
    u32 n = 0;
    for (u32 o = 0; o < nb->len; o += IE(nb->e + o)->length) {
        if (IE(nb->e + o)->flags & NTFS_IE_END) break;
        n++;
    }
    return n;
}

static u32 nb_end_off(const nbuf_t *nb) {
    u32 o = 0;
    while (o < nb->len && !(IE(nb->e + o)->flags & NTFS_IE_END)) o += IE(nb->e + o)->length;
    return o;
}

static u32 nb_last_real(const nbuf_t *nb) {
    u32 o = 0, last = ~0u;
    while (o < nb->len && !(IE(nb->e + o)->flags & NTFS_IE_END)) { last = o; o += IE(nb->e + o)->length; }
    return last;
}

static u32 nb_next_off(const nbuf_t *nb, u32 o) { return o + IE(nb->e + o)->length; }

static int nb_insert(nbuf_t *nb, u32 off, const u8 *ent, u32 len) {
    int r = nb_reserve(nb, nb->len + len);
    if (r) return r;
    memmove(nb->e + off + len, nb->e + off, nb->len - off);
    memcpy(nb->e + off, ent, len);
    nb->len += len;
    return 0;
}

static void nb_remove(nbuf_t *nb, u32 off) {
    u32 len = IE(nb->e + off)->length;
    memmove(nb->e + off, nb->e + off + len, nb->len - off - len);
    nb->len -= len;
}

/* copy an entry into buf as a leaf entry (node = 0) or with sub-node vcn */
static u32 ie_copy(const ntfs_index_entry_t *e, u8 *buf, int node, i64 vcn) {
    u32 base = e->length - ((e->flags & NTFS_IE_NODE) ? 8u : 0u);
    memcpy(buf, e, base);
    ntfs_index_entry_t *n = IE(buf);
    if (node) {
        n->length = (u16)(base + 8);
        n->flags |= NTFS_IE_NODE;
        ie_set_child(n, vcn);
    } else {
        n->length = (u16)base;
        n->flags &= (u16)~NTFS_IE_NODE;
    }
    return n->length;
}

static void nb_set_end(nbuf_t *nb, int large, i64 child) {
    u8 end[IE_HDR + 8];
    memset(end, 0, sizeof end);
    ntfs_index_entry_t *e = IE(end);
    e->flags = NTFS_IE_END | (large ? NTFS_IE_NODE : 0);
    e->length = (u16)(IE_HDR + (large ? 8 : 0));
    if (large) ie_set_child(e, child);
    nb->len = 0;
    nb_reserve(nb, 64);
    memcpy(nb->e, end, e->length);
    nb->len = e->length;
    nb->large = large;
}

/* ------------------------------------------------------------------ */
/* the block bitmap                                                     */

static int ibmp(nidx_t *ix, u64 blk, int set, int val, int *out) {
    ntfs_t *fs = ix->ino->fs;
    ainfo_t ai;
    nrl_t rl = { 0 };
    int r = nt_attr_info(ix->ino, NTFS_AT_BITMAP, ix->name, ix->nlen, &ai, &rl);
    if (r) { nrl_free(&rl); return r == E_NOENT ? E_IO : r; }
    if (blk >= ai.size * 8) { nrl_free(&rl); return E_IO; }
    u8 b = 0;
    if (ai.resident) {
        actx_t c;
        if (!(r = nt_attr_find(ix->ino, &c, NTFS_AT_BITMAP, ix->name, ix->nlen))) {
            u8 *v = attr_value(ac_attr(&c)) + blk / 8;
            if (set) {
                if (val) *v |= (u8)(1u << (blk & 7)); else *v &= (u8)~(1u << (blk & 7));
                c.rec->dirty = 1;
            }
            b = *v;
            nt_actx_done(&c);
        }
    } else if (blk / 8 >= ai.init) {
        if (set) r = E_IO;
    } else {
        r = nt_meta_rw(fs, &rl, blk / 8, 1, &b, 0);
        if (!r && set) {
            if (val) b |= (u8)(1u << (blk & 7)); else b &= (u8)~(1u << (blk & 7));
            r = nt_meta_rw(fs, &rl, blk / 8, 1, &b, 1);
        }
    }
    nrl_free(&rl);
    if (!r && out) *out = (b >> (blk & 7)) & 1;
    return r;
}

/* free blocks in the allocation (up to want); the bitmap is read at once */
static int ibmp_free_count(nidx_t *ix, u32 want, u32 *n, i64 *first) {
    u64 nblk = ix->have_alloc ? ix->aa.size / ix->bsize : 0;
    *n = 0;
    if (first) *first = -1;
    if (!nblk) return 0;
    ainfo_t ai;
    int r = nt_attr_info(ix->ino, NTFS_AT_BITMAP, ix->name, ix->nlen, &ai, NULL);
    if (r) return r == E_NOENT ? E_IO : r;
    u32 bytes = (u32)MIN((nblk + 7) / 8, (u64)NT_MAX_ALIST);
    u8 *bm = kzalloc(bytes);
    if (!bm) return E_NOMEM;
    r = nt_attr_read(ix->ino, NTFS_AT_BITMAP, ix->name, ix->nlen, 0, bytes, bm);
    if (r >= 0) {
        r = 0;
        for (u64 i = 0; i < nblk && i < 8ull * bytes && *n < want; i++) {
            if (bm[i / 8] == 0xFF && !(i & 7)) { i += 7; continue; }
            if (!(bm[i / 8] & (1u << (i & 7)))) {
                if (first && *first < 0) *first = (i64)i;
                (*n)++;
            }
        }
    }
    kfree(bm);
    return r;
}

/* create $INDEX_ALLOCATION and $BITMAP (empty) */
static int ix_create_alloc(nidx_t *ix) {
    nino_t *ino = ix->ino;
    u8 buf[NTFS_ATTR_NONRES_HDR + 16];
    memset(buf, 0, sizeof buf);
    ntfs_attr_t *a = (ntfs_attr_t *)buf;
    u32 mpoff = ALIGN_UP(NTFS_ATTR_NONRES_HDR + 2u * ix->nlen, 8u);
    a->type = NTFS_AT_INDEX_ALLOCATION;
    a->length = ALIGN_UP(mpoff + 1, 8u);
    a->non_resident = 1;
    a->name_length = ix->nlen;
    a->name_offset = NTFS_ATTR_NONRES_HDR;
    a->nr.lowest_vcn = 0;
    a->nr.highest_vcn = -1;
    a->nr.mapping_pairs_offset = (u16)mpoff;
    memcpy(buf + NTFS_ATTR_NONRES_HDR, ix->name, 2u * ix->nlen);
    if (a->length > sizeof buf || ino->al) return E_NOTSUP;
    u8 zero[8] = { 0 };
    int r = nt_attr_add_resident(ino, NTFS_AT_BITMAP, ix->name, ix->nlen, zero, 8, 0);
    if (r) return r;
    u32 off;
    r = nt_rec_attr_insert(ino->fs, ino->base, a, &off);
    if (r) {
        actx_t c;
        if (!nt_attr_find(ino, &c, NTFS_AT_BITMAP, ix->name, ix->nlen)) nt_attr_remove(ino, &c);
        return r;
    }
    return ix_load_alloc(ix);
}

/* grow the allocation by k blocks (clusters ahead: an eighth, at least 4 blocks) */
static int ix_add_blocks(nidx_t *ix, u32 k) {
    nino_t *ino = ix->ino;
    ntfs_t *fs = ino->fs;
    int r;
    if (!ix->have_alloc && (r = ix_create_alloc(ix))) return r;
    u64 new_size = ix->aa.size + (u64)k * ix->bsize;
    u64 old_vcns = nrl_vcns(&ix->runs);
    ainfo_t na = ix->aa;
    if (new_size > ix->aa.alloc || new_size > old_vcns << fs->cshift) {
        u64 want = MAX(new_size, ix->aa.alloc + MAX(4ull * ix->bsize, ix->aa.alloc / 8));
        u64 need = (want + fs->csize - 1) >> fs->cshift;
        i64 hint = ix->runs.n && ix->runs.r[ix->runs.n - 1].lcn >= 0
                   ? ix->runs.r[ix->runs.n - 1].lcn + (i64)ix->runs.r[ix->runs.n - 1].len : (i64)fs->alloc_hint;
        r = nt_clus_alloc(fs, need - old_vcns, (u64)hint, 0, &ix->runs, (i64)old_vcns);
        if (r == E_NOSPC && need - old_vcns > ((new_size + fs->csize - 1) >> fs->cshift) - old_vcns) {
            need = (new_size + fs->csize - 1) >> fs->cshift;
            r = nt_clus_alloc(fs, need - old_vcns, (u64)hint, 0, &ix->runs, (i64)old_vcns);
        }
        if (r) return r;
        na.alloc = need << fs->cshift;
    }
    na.size = na.init = new_size;
    r = nt_attr_store_runs(ino, NTFS_AT_INDEX_ALLOCATION, ix->name, ix->nlen, &ix->runs, &na);
    if (r) {
        nt_clus_free_runs(fs, &ix->runs, old_vcns);
        ix_load_alloc(ix);
        return r;
    }
    /* the bitmap: one bit per block, 8-byte multiples */
    ainfo_t bi;
    if ((r = nt_attr_info(ino, NTFS_AT_BITMAP, ix->name, ix->nlen, &bi, NULL))) goto fail;
    u64 bbytes = ALIGN_UP((new_size / ix->bsize + 7) / 8, 8ull);
    if (bbytes > bi.size) {
        if (bi.resident) {
            u8 *v = kzalloc((size_t)bbytes);
            if (!v) { r = E_NOMEM; goto fail; }
            r = nt_attr_read(ino, NTFS_AT_BITMAP, ix->name, ix->nlen, 0, (u32)bi.size, v);
            if (r >= 0) r = nt_attr_set_resident(ino, NTFS_AT_BITMAP, ix->name, ix->nlen, v, (u32)bbytes);
            kfree(v);
            if (r == E_NOSPC) {
                r = nt_attr_make_nonres(ino, NTFS_AT_BITMAP, ix->name, ix->nlen, 0);
                if (!r) r = nt_attr_info(ino, NTFS_AT_BITMAP, ix->name, ix->nlen, &bi, NULL);
                if (!r) goto nonres;
            }
            if (r) goto fail;
        } else {
        nonres:;
            u64 old = bi.size;
            r = nt_attr_resize_nr(ino, NTFS_AT_BITMAP, ix->name, ix->nlen, bbytes, 1);
            if (r) goto fail;
            nrl_t brl = { 0 };
            if (!(r = nt_attr_info(ino, NTFS_AT_BITMAP, ix->name, ix->nlen, &bi, &brl))) {
                u8 z[64];
                memset(z, 0, sizeof z);
                for (u64 o = old; o < bbytes && !r; o += sizeof z)
                    r = nt_meta_rw(fs, &brl, o, (u32)MIN((u64)sizeof z, bbytes - o), z, 1);
            }
            nrl_free(&brl);
            if (r) goto fail;
        }
    }
    return ix_load_alloc(ix);
fail:
    /* keep the larger allocation (harmless: its blocks are unused) */
    ix_load_alloc(ix);
    return r;
}

/* make sure k unused blocks exist */
static int ix_reserve(nidx_t *ix, u32 k) {
    u32 n;
    int r = ibmp_free_count(ix, k, &n, NULL);
    if (r) return r;
    if (n >= k) return 0;
    return ix_add_blocks(ix, k - n);
}

static int blk_alloc(nidx_t *ix, i64 *vcn) {
    u32 n;
    i64 first;
    int r = ibmp_free_count(ix, 1, &n, &first);
    if (r) return r;
    if (!n) return E_NOSPC;
    if ((r = ibmp(ix, (u64)first, 1, 1, NULL))) return r;
    *vcn = (i64)(((u64)first * ix->bsize) >> ix->vunit_shift);
    return 0;
}

static int blk_release(nidx_t *ix, i64 vcn) {
    return ibmp(ix, ((u64)vcn << ix->vunit_shift) / ix->bsize, 1, 0, NULL);
}

/* ------------------------------------------------------------------ */
/* storing nodes                                                        */

static u32 blk_entries_off(nidx_t *ix) {
    u32 usa = ix->bsize / NTFS_FIXUP_STRIDE + 1;
    return ALIGN_UP(0x28u + 2u * usa, 8u) - BLK_HDR;
}

static u32 blk_cap(nidx_t *ix) { return ix->bsize - BLK_HDR - blk_entries_off(ix); }

/* the root's room: its current value plus the free space of its record,
 * less a reserve for the attributes a push-down needs ($INDEX_ALLOCATION,
 * $BITMAP, their growth); an END entry always fits */
#define ROOT_HEADROOM 256u
static int root_cap(nidx_t *ix, u32 *cap) {
    actx_t c;
    ntfs_index_root_t *ir;
    int r = root_get(ix, &c, &ir);
    if (r) return r;
    ntfs_attr_t *a = ac_attr(&c);
    u32 room = a->length - a->r.value_offset + nt_rec_free_space(c.rec);
    nt_actx_done(&c);
    u32 hdr = (u32)sizeof(ntfs_index_root_t) + ROOT_HEADROOM;
    *cap = room > hdr ? room - hdr : 0;
    if (*cap < IE_HDR + 8) *cap = IE_HDR + 8;
    return 0;
}

static int nb_fits(nidx_t *ix, const nbuf_t *nb, int *fits) {
    if (nb->vcn >= 0) { *fits = nb->len <= blk_cap(ix); return 0; }
    u32 cap;
    int r = root_cap(ix, &cap);
    if (!r) *fits = nb->len <= (cap & ~7u);
    return r;
}

static int nb_store(nidx_t *ix, const nbuf_t *nb, int fresh) {
    ntfs_t *fs = ix->ino->fs;
    int r;
    if (nb->vcn < 0) {
        actx_t c;
        ntfs_index_root_t *ir;
        if ((r = root_get(ix, &c, &ir))) return r;
        u32 vlen = (u32)sizeof(ntfs_index_root_t) + nb->len;
        u8 *v = kmalloc(vlen);
        if (!v) { nt_actx_done(&c); return E_NOMEM; }
        memcpy(v, ir, sizeof(ntfs_index_root_t));
        nt_actx_done(&c);
        ntfs_index_root_t *nr = (ntfs_index_root_t *)v;
        nr->ih.entries_offset = ROOT_HDR;
        nr->ih.index_length = nr->ih.allocated_size = ROOT_HDR + nb->len;
        nr->ih.flags = nb->large ? NTFS_INDEX_LARGE : 0;
        memcpy(v + sizeof(ntfs_index_root_t), nb->e, nb->len);
        r = nt_attr_set_resident(ix->ino, NTFS_AT_INDEX_ROOT, ix->name, ix->nlen, v, vlen);
        kfree(v);
        return r;
    }
    if (nb->len > blk_cap(ix)) return E_NOSPC;
    fblk_t *b;
    if ((r = blk_get(ix, nb->vcn, fresh, &b))) {
        if (r == E_IO) nt_corrupt(fs, "damaged index block", ix->ino->rec);
        return r;
    }
    ntfs_index_block_t *h = (ntfs_index_block_t *)b->buf;
    u32 eo = blk_entries_off(ix);
    if (fresh) {
        memset(b->buf, 0, ix->bsize);
        h->magic = NTFS_MAGIC_INDX;
        h->usa_offset = 0x28;
        h->usa_count = (u16)(ix->bsize / NTFS_FIXUP_STRIDE + 1);
        *(u16 *)(b->buf + 0x28) = 1;
        h->vcn = nb->vcn;
        h->ih.entries_offset = eo;
        h->ih.allocated_size = ix->bsize - BLK_HDR;
    }
    eo = h->ih.entries_offset;
    if (eo + nb->len > h->ih.allocated_size) {
        /* an existing block with a larger header: rewrite it as ours */
        eo = blk_entries_off(ix);
        h->ih.entries_offset = eo;
        h->ih.allocated_size = ix->bsize - BLK_HDR;
    }
    u8 *ent = (u8 *)&h->ih + eo;
    memcpy(ent, nb->e, nb->len);
    u32 old_end = h->ih.index_length;
    h->ih.index_length = eo + nb->len;
    if (old_end > h->ih.index_length && old_end <= h->ih.allocated_size)
        memset((u8 *)&h->ih + h->ih.index_length, 0, old_end - h->ih.index_length);
    h->ih.flags = nb->large ? NTFS_INDEX_LARGE : 0;
    b->dirty = 1;
    nt_fb_put(fs, b);
    return 0;
}

/* ------------------------------------------------------------------ */
/* lookups and walks                                                    */

typedef struct { i64 vcn; u32 pos; } plev_t;

/* descend to key: path[0..*depth] (path[d].pos: the entry the key is equal
 * to or would be inserted before). Returns 1 if found (at path[*depth]). */
static int descend(nidx_t *ix, const u8 *key, u32 klen, int ic, plev_t *path, int *depth, nbuf_t *nb) {
    i64 vcn = -1;
    for (int d = 0; d < NT_MAX_DEPTH; d++) {
        int r = nb_load(ix, vcn, nb);
        if (r) return r;
        path[d].vcn = vcn;
        u32 o = 0;
        for (;;) {
            ntfs_index_entry_t *e = IE(nb->e + o);
            if (e->flags & NTFS_IE_END) break;
            int c = ix->coll == nt_coll_filename ? fn_cmp(ix->ino->fs, key, klen, e->key, e->key_length, ic)
                                                 : ix->coll(ix->ino->fs, key, klen, e->key, e->key_length);
            if (!c) { path[d].pos = o; *depth = d; return 1; }
            if (c < 0) break;
            o += e->length;
        }
        path[d].pos = o;
        *depth = d;
        ntfs_index_entry_t *e = IE(nb->e + o);
        if (!(e->flags & NTFS_IE_NODE)) return 0;
        vcn = ie_child(e);
    }
    nt_corrupt(ix->ino->fs, "index tree too deep (a loop?)", ix->ino->rec);
    return E_IO;
}

int nt_idx_lookup(nidx_t *ix, const u8 *key, u32 klen, int ic, u8 *out, u32 cap) {
    plev_t path[NT_MAX_DEPTH];
    nbuf_t nb = { 0 };
    int depth, r = descend(ix, key, klen, 0, path, &depth, &nb);
    if (r == 0 && ic && ix->coll == nt_coll_filename) r = descend(ix, key, klen, 1, path, &depth, &nb);
    if (r == 1) {
        ntfs_index_entry_t *e = IE(nb.e + path[depth].pos);
        u32 l = MIN((u32)e->length, cap);
        memcpy(out, e, l);
        r = 0;
    } else if (r == 0) {
        r = E_NOENT;
    }
    nb_free(&nb);
    return r;
}

typedef struct {
    nidx_t *ix;
    int (*cb)(void *ctx, const ntfs_index_entry_t *e);
    void *ctx;
    u64 visits, limit;
} walk_t;

static int walk_node(walk_t *w, i64 vcn, int depth) {
    if (depth >= NT_MAX_DEPTH || ++w->visits > w->limit) {
        nt_corrupt(w->ix->ino->fs, "index tree loops", w->ix->ino->rec);
        return E_IO;
    }
    nbuf_t nb = { 0 };
    int r = nb_load(w->ix, vcn, &nb);
    for (u32 o = 0; !r && o < nb.len;) {
        ntfs_index_entry_t *e = IE(nb.e + o);
        if (e->flags & NTFS_IE_NODE) r = walk_node(w, ie_child(e), depth + 1);
        if (r) break;
        if (e->flags & NTFS_IE_END) break;
        r = w->cb(w->ctx, e);
        o += e->length;
    }
    nb_free(&nb);
    return r;
}

int nt_idx_walk(nidx_t *ix, int (*cb)(void *ctx, const ntfs_index_entry_t *e), void *ctx) {
    walk_t w = { ix, cb, ctx, 0, 1 + (ix->have_alloc ? ix->aa.size / ix->bsize : 0) };
    return walk_node(&w, -1, 0);
}

static int any_cb(void *ctx, const ntfs_index_entry_t *e) { return 1; }

/* reserve the blocks n later deletions (or insertions) may need */
int nt_idx_reserve(nidx_t *ix, int n) {
    if (!ix->have_alloc) return 0;
    int depth = 0;
    nbuf_t nb = { 0 };
    i64 v = -1;
    int r;
    while (!(r = nb_load(ix, v, &nb)) && nb.large && depth < NT_MAX_DEPTH) {
        v = ie_child(IE(nb.e));
        depth++;
    }
    nb_free(&nb);
    if (r) return r;
    return ix_reserve(ix, (u32)(n * (depth + 3)));
}

int nt_idx_empty(nidx_t *ix) {
    int r = nt_idx_walk(ix, any_cb, NULL);
    return r < 0 ? r : r == 0;
}

/* rewrite an entry in place (dup info); same key and length */
int nt_idx_update(nidx_t *ix, const ntfs_index_entry_t *ne) {
    plev_t path[NT_MAX_DEPTH];
    nbuf_t nb = { 0 };
    int depth, r = descend(ix, ne->key, ne->key_length, 0, path, &depth, &nb);
    if (r == 1) {
        ntfs_index_entry_t *e = IE(nb.e + path[depth].pos);
        u32 base = e->length - ((e->flags & NTFS_IE_NODE) ? 8u : 0u);
        u32 nbase = ne->length - ((ne->flags & NTFS_IE_NODE) ? 8u : 0u);
        if (e->file != ne->file || base != nbase || e->key_length != ne->key_length) {
            r = E_NOENT;
        } else {
            memcpy(e->key, ne->key, ne->key_length);
            r = nb_store(ix, &nb, 0);
        }
    } else if (r == 0) {
        r = E_NOENT;
    }
    nb_free(&nb);
    return r;
}

/* ------------------------------------------------------------------ */
/* insertion                                                            */

/* store node nb at depth d of path, splitting / pushing down as needed */
static int fix_overflow(nidx_t *ix, plev_t *path, int d, nbuf_t *nb) {
    u8 *med = kmalloc(NT_IE_MAX + 16);
    if (!med) return E_NOMEM;
    nbuf_t left = { 0 }, parent = { 0 };
    int r = 0;
    r = E_NOSPC;
    for (int guard = 0; guard < 2 * NT_MAX_DEPTH + 4; guard++) {
        int fits;
        if ((r = nb_fits(ix, nb, &fits))) break;
        if (fits) { r = nb_store(ix, nb, 0); break; }
        if (nb->vcn < 0) {
            /* push the root down into a new block */
            i64 nv;
            if ((r = blk_alloc(ix, &nv))) break;
            nbuf_t blk = { 0 };
            nb_set_end(&blk, 0, 0);
            blk.vcn = nv;
            r = nb_store(ix, &blk, 1);          /* a valid empty block first */
            nb_free(&blk);
            if (r) break;
            nbuf_t root = { 0 };
            nb_set_end(&root, 1, nv);
            root.vcn = -1;
            if ((r = nb_store(ix, &root, 0))) { nb_free(&root); break; }
            nb_free(&root);
            /* continue with the new block one level down */
            if (d + 1 >= NT_MAX_DEPTH) { r = E_IO; break; }
            memmove(path + 1, path, sizeof(plev_t) * (u32)(d + 1));
            path[0].vcn = -1;
            path[0].pos = 0;
            path[1].vcn = nv;
            nb->vcn = nv;
            d = 1;
            continue;
        }
        /* split: the entries before the middle one go to a new block */
        u32 cnt = nb_count(nb), half = nb->len / 2, o = 0, idx = 0;
        if (cnt < 3) { r = E_IO; break; }
        while (idx + 2 < cnt && o + IE(nb->e + o)->length <= half) { o += IE(nb->e + o)->length; idx++; }
        if (idx == 0) { o = nb_next_off(nb, 0); idx = 1; }
        ntfs_index_entry_t *m = IE(nb->e + o);
        i64 nv;
        if ((r = blk_alloc(ix, &nv))) break;
        left.vcn = nv;
        left.len = 0;
        if ((r = nb_reserve(&left, o + IE_HDR + 8))) break;
        memcpy(left.e, nb->e, o);
        left.len = o;
        left.large = nb->large;
        /* the left half's END: the middle entry's sub-node */
        {
            u8 end[IE_HDR + 8];
            memset(end, 0, sizeof end);
            ntfs_index_entry_t *e = IE(end);
            e->flags = NTFS_IE_END | (nb->large ? NTFS_IE_NODE : 0);
            e->length = (u16)(IE_HDR + (nb->large ? 8 : 0));
            if (nb->large) ie_set_child(e, ie_child(m));
            memcpy(left.e + left.len, end, e->length);
            left.len += e->length;
        }
        u32 mlen = ie_copy(m, med, 1, nv);
        /* the right half stays in this block */
        u32 mend = o + m->length;
        memmove(nb->e, nb->e + mend, nb->len - mend);
        nb->len -= mend;
        if ((r = nb_store(ix, &left, 1)) || (r = nb_store(ix, nb, 0))) break;
        /* the middle entry goes into the parent, before the entry that
         * points to this block */
        if ((r = nb_load(ix, path[d - 1].vcn, &parent))) break;
        if ((r = nb_insert(&parent, path[d - 1].pos, med, mlen))) break;
        nb_free(nb);
        *nb = parent;
        parent.e = NULL;
        parent.len = parent.cap = 0;
        d--;
    }
    kfree(med);
    nb_free(&left);
    nb_free(&parent);
    return r;
}

int nt_idx_insert(nidx_t *ix, const ntfs_index_entry_t *e) {
    plev_t path[NT_MAX_DEPTH];
    nbuf_t nb = { 0 };
    int depth;
    int r = descend(ix, e->key, e->key_length, 0, path, &depth, &nb);
    if (r == 1) { nb_free(&nb); return E_EXIST; }
    if (r < 0) { nb_free(&nb); return r; }
    if (ix->coll == nt_coll_filename) {
        /* names equal ignoring case are one name for Halcyon */
        plev_t p2[NT_MAX_DEPTH];
        nbuf_t nb2 = { 0 };
        int d2, k = descend(ix, e->key, e->key_length, 1, p2, &d2, &nb2);
        nb_free(&nb2);
        if (k == 1) { nb_free(&nb); return E_EXIST; }
        if (k < 0) { nb_free(&nb); return k; }
    }
    u8 *ent = kmalloc(NT_IE_MAX + 16);
    if (!ent) { nb_free(&nb); return E_NOMEM; }
    u32 len = ie_copy(e, ent, 0, 0);
    r = nb_insert(&nb, path[depth].pos, ent, len);
    kfree(ent);
    int fits = 0;
    if (!r) r = nb_fits(ix, &nb, &fits);
    if (!r && !fits) r = ix_reserve(ix, (u32)depth + 3);
    if (!r) r = fix_overflow(ix, path, depth, &nb);
    nb_free(&nb);
    return r;
}

/* ------------------------------------------------------------------ */
/* deletion                                                             */

/* the root: absorb a single child, drop the allocation when unused */
static int root_tidy(nidx_t *ix) {
    nbuf_t root = { 0 }, child = { 0 };
    int r = nb_load(ix, -1, &root);
    if (r) goto out;
    if (root.large && nb_count(&root) == 0) {
        i64 cv = ie_child(IE(root.e));
        if ((r = nb_load(ix, cv, &child))) goto out;
        u32 cap;
        if ((r = root_cap(ix, &cap))) goto out;
        if (child.len <= (cap & ~7u)) {
            child.vcn = -1;
            if ((r = nb_store(ix, &child, 0))) goto out;
            if ((r = blk_release(ix, cv))) goto out;
            root.large = child.large;
        }
    }
    if (!root.large && ix->have_alloc) {
        u32 n;
        u64 nblk = ix->aa.size / ix->bsize;
        if ((r = ibmp_free_count(ix, (u32)MIN(nblk, 0xFFFFFFFFull), &n, NULL))) goto out;
        if (n == nblk) {
            /* nothing in use: back to a small index (as a new directory) */
            actx_t c;
            if (!(r = nt_attr_find(ix->ino, &c, NTFS_AT_INDEX_ALLOCATION, ix->name, ix->nlen))) {
                nrl_t rl = ix->runs;
                ix->runs.r = NULL;
                ix->runs.n = ix->runs.cap = 0;
                if (!(r = nt_attr_remove(ix->ino, &c))) {
                    nt_clus_free_runs(ix->ino->fs, &rl, 0);
                    nrl_t brl = { 0 };
                    ainfo_t bi;
                    if (!nt_attr_info(ix->ino, NTFS_AT_BITMAP, ix->name, ix->nlen, &bi, &brl)) {
                        if (!nt_attr_find(ix->ino, &c, NTFS_AT_BITMAP, ix->name, ix->nlen)) {
                            r = nt_attr_remove(ix->ino, &c);
                            if (!r && !bi.resident) nt_clus_free_runs(ix->ino->fs, &brl, 0);
                        }
                    }
                    nrl_free(&brl);
                }
                nrl_free(&rl);
                if (!r) { ix->have_alloc = 0; memset(&ix->aa, 0, sizeof ix->aa); }
            }
        }
    }
out:
    nb_free(&root);
    nb_free(&child);
    return r;
}

/* node u (depth d of path, no keys) is not the root: rotate or merge */
static int fix_underflow(nidx_t *ix, plev_t *path, int d, nbuf_t *u) {
    u8 *t1 = kmalloc(NT_IE_MAX + 16), *t2 = kmalloc(NT_IE_MAX + 16);
    nbuf_t p = { 0 }, s = { 0 };
    int r = (!t1 || !t2) ? E_NOMEM : 0;
    while (!r && d > 0) {
        if (nb_count(u) > 0) { r = nb_store(ix, u, 0); break; }
        if ((r = nb_load(ix, path[d - 1].vcn, &p))) break;
        u32 xo = path[d - 1].pos;
        if (xo >= p.len) { r = EIO_AT(); break; }
        ntfs_index_entry_t *x = IE(p.e + xo);
        if (!(x->flags & NTFS_IE_NODE) || ie_child(x) != u->vcn) { r = EIO_AT(); break; }
        i64 uchild = u->large ? ie_child(IE(u->e)) : 0;     /* u: only its END */
        if (!(x->flags & NTFS_IE_END)) {
            /* right sibling: the sub-node of the entry after x */
            u32 yo = nb_next_off(&p, xo);
            ntfs_index_entry_t *y = IE(p.e + yo);
            if (!(y->flags & NTFS_IE_NODE)) { r = EIO_AT(); break; }
            if ((r = nb_load(ix, ie_child(y), &s))) break;
            if (s.large != u->large) { r = EIO_AT(); break; }
            u32 xl = ie_copy(x, t1, u->large, uchild);  /* x moves down into u / s */
            if (nb_count(&s) >= 2) {
                /* rotate left: x down into u, s's first key up into x's place */
                ntfs_index_entry_t *s1 = IE(s.e);
                i64 s1c = s.large ? ie_child(s1) : 0;
                u32 sl = ie_copy(s1, t2, 1, u->vcn);
                nb_set_end(u, u->large, s1c);
                if ((r = nb_insert(u, 0, t1, xl))) break;
                nb_remove(&s, 0);
                nb_remove(&p, xo);
                if ((r = nb_insert(&p, xo, t2, sl))) break;
                if ((r = nb_store(ix, u, 0)) || (r = nb_store(ix, &s, 0))) break;
                nb_free(u);
                *u = p; p.e = NULL; p.len = p.cap = 0;
                r = fix_overflow(ix, path, d - 1, u);
                break;
            }
            /* merge: x and s's keys in s; u is freed */
            if ((r = nb_insert(&s, 0, t1, xl))) break;
            nb_remove(&p, xo);
            if ((r = nb_store(ix, &s, 0)) || (r = blk_release(ix, u->vcn))) break;
        } else {
            /* u is the last sub-node: left sibling under the last key w */
            u32 wo = nb_last_real(&p);
            if (wo == ~0u && d - 1 == 0) {
                /* a keyless root over u alone: the tree loses a level */
                nb_set_end(&p, u->large, uchild);
                if ((r = nb_store(ix, &p, 0)) || (r = blk_release(ix, u->vcn))) break;
                break;
            }
            if (wo == ~0u) { r = EIO_AT(); break; }      /* only the root can be keyless */
            ntfs_index_entry_t *w = IE(p.e + wo);
            if ((r = nb_load(ix, ie_child(w), &s))) break;
            if (s.large != u->large) { r = EIO_AT(); break; }
            u32 so = nb_end_off(&s);
            i64 send = s.large ? ie_child(IE(s.e + so)) : 0;
            u32 wl = ie_copy(w, t1, u->large, send);    /* w moves down */
            if (nb_count(&s) >= 2) {
                /* rotate right: w down into u, s's last key up into w's place */
                u32 ko = nb_last_real(&s);
                ntfs_index_entry_t *k = IE(s.e + ko);
                i64 kc = s.large ? ie_child(k) : 0;
                u32 kl = ie_copy(k, t2, 1, s.vcn);
                nb_set_end(u, u->large, uchild);
                if ((r = nb_insert(u, 0, t1, wl))) break;
                nb_remove(&s, ko);
                if (s.large) ie_set_child(IE(s.e + nb_end_off(&s)), kc);
                nb_remove(&p, wo);
                if ((r = nb_insert(&p, wo, t2, kl))) break;
                if ((r = nb_store(ix, u, 0)) || (r = nb_store(ix, &s, 0))) break;
                nb_free(u);
                *u = p; p.e = NULL; p.len = p.cap = 0;
                r = fix_overflow(ix, path, d - 1, u);
                break;
            }
            /* merge: s's keys, w, and u's sub-tree in s; u is freed */
            if ((r = nb_insert(&s, so, t1, wl))) break;
            if (s.large) ie_set_child(IE(s.e + nb_end_off(&s)), uchild);
            i64 sv = s.vcn;
            nb_remove(&p, wo);
            ie_set_child(IE(p.e + nb_end_off(&p)), sv);
            if ((r = nb_store(ix, &s, 0)) || (r = blk_release(ix, u->vcn))) break;
        }
        /* the parent lost a key */
        nb_free(u);
        *u = p; p.e = NULL; p.len = p.cap = 0;
        nb_free(&s);
        d--;
        if (d == 0) { r = nb_store(ix, u, 0); break; }
    }
    kfree(t1);
    kfree(t2);
    nb_free(&p);
    nb_free(&s);
    return r;
}

int nt_idx_delete(nidx_t *ix, const u8 *key, u32 klen, u64 file) {
    plev_t path[NT_MAX_DEPTH];
    nbuf_t nb = { 0 }, leaf = { 0 };
    u8 *pd = NULL;
    int depth, r = descend(ix, key, klen, 0, path, &depth, &nb);
    if (r == 0) { r = E_NOENT; goto out; }
    if (r < 0) goto out;
    ntfs_index_entry_t *x = IE(nb.e + path[depth].pos);
    if (ix->coll == nt_coll_filename && x->file != file) { r = E_NOENT; goto out; }
    /* rotations and splits may need new blocks (reserved first, so a full
     * volume fails here, unchanged); removing a key from a leaf that keeps
     * others needs none */
    int simple = !nb.large && (depth == 0 || nb_count(&nb) >= 2);
    if (ix->have_alloc && !simple && (r = ix_reserve(ix, (u32)depth + 3))) goto out;
    if (!nb.large) {
        nb_remove(&nb, path[depth].pos);
        if (depth == 0) r = nb_store(ix, &nb, 0);
        else r = fix_underflow(ix, path, depth, &nb);
        if (!r) r = root_tidy(ix);
        goto out;
    }
    /* an inner entry: take the predecessor from the rightmost leaf of its
     * sub-tree */
    int d = depth;
    i64 v = ie_child(x);
    for (;;) {
        if (++d >= NT_MAX_DEPTH) { r = EIO_AT(); goto out; }
        if ((r = nb_load(ix, v, &leaf))) goto out;
        path[d].vcn = v;
        path[d].pos = nb_end_off(&leaf);
        if (!leaf.large) break;
        v = ie_child(IE(leaf.e + path[d].pos));
    }
    u32 lo = nb_last_real(&leaf);
    if (lo == ~0u) { r = EIO_AT(); goto out; }
    pd = kmalloc(NT_IE_MAX + 16);
    if (!pd) { r = E_NOMEM; goto out; }
    u32 pl = ie_copy(IE(leaf.e + lo), pd, 1, ie_child(x));
    nb_remove(&leaf, lo);
    i64 leaf_vcn = leaf.vcn;
    int leaf_empty = nb_count(&leaf) == 0;
    if ((r = nb_store(ix, &leaf, 0))) goto out;
    /* the predecessor replaces x */
    nb_remove(&nb, path[depth].pos);
    if ((r = nb_insert(&nb, path[depth].pos, pd, pl))) goto out;
    if ((r = fix_overflow(ix, path, depth, &nb))) goto out;
    if (leaf_empty) {
        /* find the leaf again (splits may have moved the path) */
        int d2;
        nb_free(&nb);
        r = descend(ix, pd + IE_HDR, IE(pd)->key_length, 0, path, &d2, &nb);
        if (r != 1) { r = r < 0 ? r : E_IO; goto out; }
        v = ie_child(IE(nb.e + path[d2].pos));
        for (;;) {
            if (++d2 >= NT_MAX_DEPTH) { r = EIO_AT(); goto out; }
            if ((r = nb_load(ix, v, &leaf))) goto out;
            path[d2].vcn = v;
            path[d2].pos = nb_end_off(&leaf);
            if (!leaf.large) break;
            v = ie_child(IE(leaf.e + path[d2].pos));
        }
        if (leaf.vcn != leaf_vcn) { r = EIO_AT(); goto out; }
        r = fix_underflow(ix, path, d2, &leaf);
    }
    if (!r) r = root_tidy(ix);
out:
    if (r == E_IO) nt_corrupt(ix->ino->fs, "index damaged during a deletion", ix->ino->rec);
    kfree(pd);
    nb_free(&nb);
    nb_free(&leaf);
    return r;
}
