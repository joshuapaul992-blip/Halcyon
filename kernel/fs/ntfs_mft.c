/* Halcyon OS - NTFS: device I/O, caches, run lists, MFT records and
 * attributes (see ntfs_priv.h and docs/DESIGN-M7.5.md).
 *
 * Everything read from the disk is untrusted: record and attribute headers
 * are checked before use (nt_rec_valid, rec_attr_ok), run lists must stay
 * inside the volume, attribute lists must point to records that belong to
 * the file, and loops are bounded by sizes known in advance. */
#include "core/types.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "fs/block.h"
#include "fs/ntfs_priv.h"

#define MAX_IO (1024u * 1024u)

const u16 nt_I30[4] = { '$', 'I', '3', '0' };

void nt_corrupt(ntfs_t *fs, const char *what, u64 rec) {
    if (!fs->corrupt)
        kprintf("ntfs: %s: %s (MFT record %lu); the volume needs a check in Windows (chkdsk)\n",
                fs->bd->name, what, (unsigned long)rec);
    fs->corrupt = 1;
    if (!fs->ro) {
        fs->ro = NT_RO_CHECK;
        if (fs->v) {
            fs->v->readonly = 1;
            strlcpy(fs->v->ro_reason, "The volume needs a check in Windows (chkdsk).", sizeof fs->v->ro_reason);
        }
    }
}

/* ------------------------------------------------------------------ */
/* device I/O (volume byte offsets)                                     */

int nt_dev_rw(ntfs_t *fs, u64 off, u32 len, void *vbuf, int write) {
    u8 *buf = vbuf;
    u32 dss = fs->dss;
    if (!len) return 0;
    if (off > fs->vol_bytes || len > fs->vol_bytes - off) return E_IO;
    while (len) {
        u64 lba = off / dss;
        u32 in = (u32)(off % dss);
        if (in || len < dss) {
            /* a partial device sector: through the bounce buffer */
            u32 n = MIN(len, dss - in);
            int r = blk_read(fs->bd, lba, 1, fs->bounce);
            if (r) return r;
            if (write) {
                memcpy(fs->bounce + in, buf, n);
                if ((r = blk_write(fs->bd, lba, 1, fs->bounce))) return r;
            } else {
                memcpy(buf, fs->bounce + in, n);
            }
            off += n; buf += n; len -= n;
            continue;
        }
        u32 n = MIN(len - len % dss, MAX_IO);
        int r = write ? blk_write(fs->bd, lba, n / dss, buf) : blk_read(fs->bd, lba, n / dss, buf);
        if (r) return r;
        off += n; buf += n; len -= n;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* run lists                                                            */

void nrl_free(nrl_t *rl) {
    kfree(rl->r);
    rl->r = NULL;
    rl->n = rl->cap = 0;
}

int nrl_add(nrl_t *rl, i64 vcn, i64 lcn, u64 len) {
    if (!len) return 0;
    if (rl->n) {
        nrun_t *l = &rl->r[rl->n - 1];
        if (l->vcn + (i64)l->len == vcn &&
            ((lcn == NT_LCN_HOLE && l->lcn == NT_LCN_HOLE) ||
             (lcn >= 0 && l->lcn >= 0 && l->lcn + (i64)l->len == lcn))) {
            l->len += len;
            return 0;
        }
    }
    if (rl->n >= NT_MAX_RUNS) return E_IO;
    if (rl->n == rl->cap) {
        u32 nc = rl->cap ? rl->cap * 2 : 8;
        nrun_t *nr = krealloc(rl->r, (size_t)nc * sizeof *nr);
        if (!nr) return E_NOMEM;
        rl->r = nr;
        rl->cap = nc;
    }
    rl->r[rl->n].vcn = vcn;
    rl->r[rl->n].lcn = lcn;
    rl->r[rl->n].len = len;
    rl->n++;
    return 0;
}

u64 nrl_vcns(const nrl_t *rl) {
    return rl->n ? (u64)rl->r[rl->n - 1].vcn + rl->r[rl->n - 1].len : 0;
}

/* the LCN of vcn and the clusters from there to the end of its run */
i64 nrl_lcn(const nrl_t *rl, i64 vcn, u64 *contig) {
    u32 lo = 0, hi = rl->n;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        const nrun_t *r = &rl->r[mid];
        if (vcn < r->vcn) hi = mid;
        else if (vcn >= r->vcn + (i64)r->len) lo = mid + 1;
        else {
            if (contig) *contig = r->len - (u64)(vcn - r->vcn);
            return r->lcn == NT_LCN_HOLE ? NT_LCN_HOLE : r->lcn + (vcn - r->vcn);
        }
    }
    return -2;
}

/* keep only the first vcns clusters */
int nrl_truncate(nrl_t *rl, u64 vcns) {
    while (rl->n && (u64)rl->r[rl->n - 1].vcn >= vcns) rl->n--;
    if (rl->n) {
        nrun_t *l = &rl->r[rl->n - 1];
        if ((u64)l->vcn + l->len > vcns) l->len = vcns - (u64)l->vcn;
    }
    return 0;
}

/* append the runs of a non-resident attribute extent */
int nrl_decode(ntfs_t *fs, const ntfs_attr_t *a, nrl_t *rl) {
    const u8 *p = (const u8 *)a + a->nr.mapping_pairs_offset;
    const u8 *end = (const u8 *)a + a->length;
    i64 vcn = a->nr.lowest_vcn, lcn = 0, last = a->nr.highest_vcn;
    while (p < end && *p) {
        u32 lb = *p & 15, ob = *p >> 4;
        if (!lb || lb > 8 || ob > 8 || p + 1 + lb + ob > end) return E_IO;
        p++;
        u64 len = 0;
        for (u32 i = 0; i < lb; i++) len |= (u64)p[i] << (8 * i);
        if (p[lb - 1] & 0x80) return E_IO;          /* a negative length */
        p += lb;
        i64 run_lcn = NT_LCN_HOLE;
        if (ob) {
            u64 d = 0;
            for (u32 i = 0; i < ob; i++) d |= (u64)p[i] << (8 * i);
            if (ob < 8 && (p[ob - 1] & 0x80)) d |= ~0ull << (8 * ob);
            p += ob;
            lcn += (i64)d;
            if (lcn < 0 || (u64)lcn >= fs->nclus || len > fs->nclus - (u64)lcn) return E_IO;
            run_lcn = lcn;
        }
        if (!len || len > (1ull << 40) || vcn > last) return E_IO;
        if ((u64)(last - vcn + 1) < len) len = (u64)(last - vcn + 1);   /* clamp to highest_vcn */
        int r = nrl_add(rl, vcn, run_lcn, len);
        if (r) return r;
        vcn += (i64)len;
    }
    if (vcn != last + 1) return E_IO;
    return 0;
}

static u32 sbytes(i64 v) {
    u32 n = 1;
    while (n < 8 && !(v >= -(1ll << (8 * n - 1)) && v < (1ll << (8 * n - 1)))) n++;
    return n;
}

/* encode the runs of [from_vcn, to_vcn) as mapping pairs (with the
 * terminating 0); returns the byte count, or -1 if cap is too small */
int nrl_encode(const nrl_t *rl, i64 from_vcn, i64 to_vcn, u8 *out, u32 cap) {
    u32 o = 0;
    i64 prev = 0;
    for (u32 i = 0; i < rl->n; i++) {
        const nrun_t *r = &rl->r[i];
        i64 s = MAX(r->vcn, from_vcn), e = MIN(r->vcn + (i64)r->len, to_vcn);
        if (s >= e) continue;
        i64 len = e - s;
        u32 lb = sbytes(len), ob = 0;
        i64 d = 0;
        if (r->lcn != NT_LCN_HOLE) {
            i64 lcn = r->lcn + (s - r->vcn);
            d = lcn - prev;
            ob = sbytes(d);
            prev = lcn;
        }
        if (o + 1 + lb + ob > cap) return -1;
        if (out) {
            out[o] = (u8)(lb | (ob << 4));
            for (u32 k = 0; k < lb; k++) out[o + 1 + k] = (u8)((u64)len >> (8 * k));
            for (u32 k = 0; k < ob; k++) out[o + 1 + lb + k] = (u8)((u64)d >> (8 * k));
        }
        o += 1 + lb + ob;
    }
    if (o + 1 > cap) return -1;
    if (out) out[o] = 0;
    return (int)(o + 1);
}

/* direct (uncached) I/O of attribute data through its run list; holes read
 * as zeros (and must not be written) */
int nt_runs_rw(ntfs_t *fs, const nrl_t *rl, u64 pos, u32 len, void *vbuf, int write) {
    u8 *buf = vbuf;
    while (len) {
        u64 contig = 0;
        i64 vcn = (i64)(pos >> fs->cshift);
        i64 lcn = nrl_lcn(rl, vcn, &contig);
        if (lcn == -2) return E_IO;
        u32 in = (u32)(pos & (fs->csize - 1));
        u64 avail = contig * fs->csize - in;
        u32 n = (u32)MIN((u64)len, avail);
        if (lcn == NT_LCN_HOLE) {
            if (write) return E_IO;
            memset(buf, 0, n);
        } else {
            int r = nt_dev_rw(fs, ((u64)lcn << fs->cshift) + in, n, buf, write);
            if (r) return r;
        }
        pos += n; buf += n; len -= n;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* page cache (bitmaps)                                                 */

static int pg_write(ntfs_t *fs, npage_t *p) {
    u32 n = (u32)MIN((u64)NT_PG_SIZE, fs->vol_bytes - p->off);
    int r = nt_dev_rw(fs, p->off, n, p->buf, 1);
    if (!r) p->dirty = 0;
    return r;
}

static npage_t *pg_get(ntfs_t *fs, u64 off, int *err) {
    npage_t *victim = NULL;
    for (int i = 0; i < NT_PG_N; i++) {
        npage_t *p = &fs->pg[i];
        if (p->valid && p->off == off) { p->stamp = ++fs->stamp; return p; }
        if (!victim || !p->valid || (victim->valid && p->stamp < victim->stamp)) {
            if (!victim || victim->valid) victim = p;
        }
    }
    if (victim->valid && victim->dirty) {
        int r = pg_write(fs, victim);
        if (r) { *err = r; return NULL; }
    }
    victim->valid = 0;
    u32 n = (u32)MIN((u64)NT_PG_SIZE, fs->vol_bytes - off);
    memset(victim->buf, 0, NT_PG_SIZE);
    int r = nt_dev_rw(fs, off, n, victim->buf, 0);
    if (r) { *err = r; return NULL; }
    victim->off = off;
    victim->valid = 1;
    victim->dirty = 0;
    victim->stamp = ++fs->stamp;
    return victim;
}

/* cached I/O of metadata through a run list (no holes allowed) */
int nt_meta_rw(ntfs_t *fs, const nrl_t *rl, u64 pos, u32 len, void *vbuf, int write) {
    u8 *buf = vbuf;
    while (len) {
        u64 contig = 0;
        i64 lcn = nrl_lcn(rl, (i64)(pos >> fs->cshift), &contig);
        if (lcn < 0) return E_IO;
        u32 in = (u32)(pos & (fs->csize - 1));
        u64 voff = ((u64)lcn << fs->cshift) + in;
        u32 n = (u32)MIN((u64)len, contig * fs->csize - in);
        while (n) {
            u64 po = voff & ~(u64)(NT_PG_SIZE - 1);
            u32 pin = (u32)(voff - po), k = MIN(n, NT_PG_SIZE - pin);
            if (po >= fs->vol_bytes || pin + k > fs->vol_bytes - po) return E_IO;
            int err = 0;
            npage_t *p = pg_get(fs, po, &err);
            if (!p) return err ? err : E_IO;
            if (write) { memcpy(p->buf + pin, buf, k); p->dirty = 1; }
            else memcpy(buf, p->buf + pin, k);
            voff += k; pos += k; buf += k; len -= k; n -= k;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* fixup block cache (MFT records, index blocks)                        */

static int fb_write(ntfs_t *fs, fblk_t *b) {
    u8 *tmp = kmalloc(b->size);
    if (!tmp) return E_NOMEM;
    memcpy(tmp, b->buf, b->size);
    ntfs_fixup_write(tmp, b->size, NTFS_FIXUP_STRIDE);
    /* keep the update sequence array (with the new number) in the cached copy */
    ntfs_record_hdr_t *h = (ntfs_record_hdr_t *)tmp;
    memcpy(b->buf + h->usa_offset, tmp + h->usa_offset, 2u * h->usa_count);
    int r = 0;
    u32 o = 0;
    for (int i = 0; i < b->np && !r; i++) {
        r = nt_dev_rw(fs, b->poff[i], b->plen[i], tmp + o, 1);
        o += b->plen[i];
    }
    if (!r && b->mirror && (b->key & NT_KEY_REC)) {
        u64 pos = (b->key & ~NT_KEY_REC) << fs->rec_shift;
        r = nt_runs_rw(fs, &fs->mirr_runs, pos, b->size, tmp, 1);
    }
    kfree(tmp);
    if (!r) b->dirty = 0;
    return r;
}

int nt_flush(ntfs_t *fs) {
    int err = 0;
    for (int i = 0; i < NT_PG_N; i++) {
        npage_t *p = &fs->pg[i];
        if (p->valid && p->dirty) { int r = pg_write(fs, p); if (r && !err) err = r; }
    }
    for (int i = 0; i < NT_FB_N; i++) {
        fblk_t *b = &fs->fb[i];
        if (b->valid && b->dirty) { int r = fb_write(fs, b); if (r && !err) err = r; }
    }
    return err;
}

/* forget cached copies of [off, off+len) (freed clusters) */
void nt_inval_range(ntfs_t *fs, u64 off, u64 len) {
    for (int i = 0; i < NT_FB_N; i++) {
        fblk_t *b = &fs->fb[i];
        if (!b->valid || (b->key & NT_KEY_REC) || b->pins) continue;
        for (int k = 0; k < b->np; k++)
            if (b->poff[k] < off + len && off < b->poff[k] + b->plen[k]) { b->valid = b->dirty = 0; break; }
    }
    for (int i = 0; i < NT_PG_N; i++) {
        npage_t *p = &fs->pg[i];
        if (p->valid && p->off < off + len && off < p->off + NT_PG_SIZE) p->valid = p->dirty = 0;
    }
}

int nt_fb_get(ntfs_t *fs, u64 key, u32 size, const u64 *poff, const u32 *plen, int np, int fresh, fblk_t **out) {
    fblk_t *victim = NULL;
    for (int i = 0; i < NT_FB_N; i++) {
        fblk_t *b = &fs->fb[i];
        if (b->valid && b->key == key) {
            if (b->size != size) return E_IO;
            b->pins++;
            b->stamp = ++fs->stamp;
            if (fresh) memset(b->buf, 0, size);
            *out = b;
            return 0;
        }
        if (b->pins) continue;
        if (!victim || !b->valid || (victim->valid && b->stamp < victim->stamp))
            if (!victim || victim->valid) victim = b;
    }
    if (!victim) return E_NOMEM;
    if (size > fs->fb_bufsize || np < 1 || np > NT_MAX_PIECES) return E_IO;
    if (victim->valid && victim->dirty) {
        int r = fb_write(fs, victim);
        if (r) return r;
    }
    victim->valid = 0;
    u32 o = 0;
    for (int i = 0; i < np; i++) {
        victim->poff[i] = poff[i];
        victim->plen[i] = plen[i];
        if (!fresh) {
            int r = nt_dev_rw(fs, poff[i], plen[i], victim->buf + o, 0);
            if (r) return r;
        }
        o += plen[i];
    }
    if (o != size) return E_IO;
    if (fresh) {
        memset(victim->buf, 0, size);
    } else if (ntfs_fixup_read(victim->buf, size, NTFS_FIXUP_STRIDE)) {
        return E_IO;                    /* torn or not a protected block */
    }
    victim->np = (u8)np;
    victim->key = key;
    victim->size = size;
    victim->valid = 1;
    victim->dirty = 0;
    victim->mirror = 0;
    victim->pins = 1;
    victim->stamp = ++fs->stamp;
    *out = victim;
    return 0;
}

void nt_fb_put(ntfs_t *fs, fblk_t *b) {
    if (b && b->pins) b->pins--;
}

/* ------------------------------------------------------------------ */
/* MFT records                                                          */

static int rec_pieces(ntfs_t *fs, u64 rec, u64 *poff, u32 *plen, int *np) {
    u64 pos = rec << fs->rec_shift;
    u32 left = fs->rec_size;
    int n = 0;
    while (left) {
        u64 contig = 0;
        i64 lcn = nrl_lcn(&fs->mft_runs, (i64)(pos >> fs->cshift), &contig);
        if (lcn < 0 || n >= NT_MAX_PIECES) return E_IO;
        u32 in = (u32)(pos & (fs->csize - 1));
        u32 k = (u32)MIN((u64)left, contig * fs->csize - in);
        poff[n] = ((u64)lcn << fs->cshift) + in;
        plen[n] = k;
        if (n && poff[n - 1] + plen[n - 1] == poff[n]) plen[n - 1] += k;
        else n++;
        pos += k;
        left -= k;
    }
    *np = n;
    return 0;
}

int nt_rec_valid(ntfs_t *fs, const u8 *buf, u64 rec) {
    const ntfs_mft_record_t *m = (const ntfs_mft_record_t *)buf;
    if (m->magic != NTFS_MAGIC_FILE) return 0;
    if (m->bytes_allocated != fs->rec_size) return 0;
    if (m->bytes_in_use > fs->rec_size || m->bytes_in_use < (u32)m->attrs_offset + 8) return 0;
    if ((m->attrs_offset & 7) || m->attrs_offset < m->usa_offset + 2u * m->usa_count) return 0;
    if (m->usa_offset >= 0x30 && m->record_number != (u32)rec) return 0;
    return 1;
}

static int rec_get(ntfs_t *fs, u64 rec, int fresh, fblk_t **out) {
    if ((rec + 1) << fs->rec_shift > (fresh ? fs->mft_size : fs->mft_init) || rec >= (1ull << 32)) return E_IO;
    u64 poff[NT_MAX_PIECES];
    u32 plen[NT_MAX_PIECES];
    int np;
    int r = rec_pieces(fs, rec, poff, plen, &np);
    if (r) return r;
    fblk_t *b;
    r = nt_fb_get(fs, NT_KEY_REC | rec, fs->rec_size, poff, plen, np, fresh, &b);
    if (r) {
        if (r == E_IO && !fresh) kprintf("ntfs: %s: MFT record %lu unreadable or torn\n", fs->bd->name, (unsigned long)rec);
        return r;
    }
    b->mirror = rec < fs->mirr_n;
    if (!fresh && !nt_rec_valid(fs, b->buf, rec)) {
        if (!--b->pins && !b->dirty) b->valid = 0;
        kprintf("ntfs: %s: MFT record %lu is not a valid record\n", fs->bd->name, (unsigned long)rec);
        return E_IO;
    }
    *out = b;
    return 0;
}

int nt_rec_get(ntfs_t *fs, u64 rec, fblk_t **out) { return rec_get(fs, rec, 0, out); }
int nt_rec_get_fresh(ntfs_t *fs, u64 rec, fblk_t **out) { return rec_get(fs, rec, 1, out); }

/* an empty record (no attributes) in b */
int nt_rec_init(ntfs_t *fs, fblk_t *b, u64 rec, u16 seq, u16 flags) {
    u8 *p = b->buf;
    memset(p, 0, fs->rec_size);
    ntfs_mft_record_t *m = (ntfs_mft_record_t *)p;
    m->magic = NTFS_MAGIC_FILE;
    m->usa_offset = 0x30;
    m->usa_count = (u16)(fs->rec_size / NTFS_FIXUP_STRIDE + 1);
    m->sequence = seq;
    m->link_count = 0;
    m->attrs_offset = (u16)ALIGN_UP(0x30u + 2u * m->usa_count, 8u);
    m->flags = flags;
    m->bytes_allocated = fs->rec_size;
    m->record_number = (u32)rec;
    *(u16 *)(p + 0x30) = 1;             /* update sequence number */
    *(u32 *)(p + m->attrs_offset) = NTFS_AT_END;
    m->bytes_in_use = m->attrs_offset + 8u;
    b->dirty = 1;
    return 0;
}

static int rec_attr_ok(ntfs_t *fs, const u8 *rec, u32 pos, u32 used) {
    const ntfs_attr_t *a = (const ntfs_attr_t *)(rec + pos);
    if (pos + 16 > used) return 0;
    u32 len = a->length;
    if (len < NTFS_ATTR_RESIDENT_HDR || (len & 7) || pos + len + 4 > used) return 0;
    if (a->non_resident > 1) return 0;
    if (a->name_length && (u32)a->name_offset + 2u * a->name_length > len) return 0;
    if (!a->non_resident) {
        if ((u32)a->r.value_offset + a->r.value_length > len) return 0;
        if (a->r.value_length && a->r.value_offset < NTFS_ATTR_RESIDENT_HDR) return 0;
    } else {
        if (len < NTFS_ATTR_NONRES_HDR) return 0;
        if (a->nr.mapping_pairs_offset < NTFS_ATTR_NONRES_HDR || a->nr.mapping_pairs_offset >= len) return 0;
        if (a->name_length && (u32)a->name_offset + 2u * a->name_length > a->nr.mapping_pairs_offset) return 0;
        if (a->nr.lowest_vcn < 0 || a->nr.highest_vcn < a->nr.lowest_vcn - 1) return 0;
        if (a->nr.highest_vcn >= (i64)(1ull << 48)) return 0;
        if (a->nr.lowest_vcn == 0 &&
            (a->nr.allocated_size < 0 || a->nr.data_size < 0 || a->nr.initialized_size < 0))
            return 0;
    }
    return 1;
}

/* walk the attributes of a record: 1 = *out is the attribute at *off, 0 =
 * end, <0 = corrupt. pos starts at 0. */
int nt_rec_walk(ntfs_t *fs, fblk_t *b, u32 *pos, u32 *off, ntfs_attr_t **out) {
    ntfs_mft_record_t *m = fb_rec(b);
    u32 p = *pos ? *pos : m->attrs_offset;
    if (p + 4 > m->bytes_in_use) return E_IO;
    if (*(u32 *)(b->buf + p) == NTFS_AT_END) return 0;
    if (!rec_attr_ok(fs, b->buf, p, m->bytes_in_use)) return E_IO;
    ntfs_attr_t *a = (ntfs_attr_t *)(b->buf + p);
    *off = p;
    *pos = p + a->length;
    *out = a;
    return 1;
}

u32 nt_rec_free_space(fblk_t *b) {
    ntfs_mft_record_t *m = fb_rec(b);
    return m->bytes_allocated - m->bytes_in_use;
}

int nt_rec_attr_resize(ntfs_t *fs, fblk_t *b, u32 off, u32 newlen) {
    ntfs_mft_record_t *m = fb_rec(b);
    ntfs_attr_t *a = (ntfs_attr_t *)(b->buf + off);
    u32 old = a->length;
    newlen = ALIGN_UP(newlen, 8u);
    if (newlen == old) return 0;
    if (newlen > old && newlen - old > m->bytes_allocated - m->bytes_in_use) return E_NOSPC;
    u32 tail = m->bytes_in_use - (off + old);
    memmove(b->buf + off + newlen, b->buf + off + old, tail);
    if (newlen > old) memset(b->buf + off + old, 0, newlen - old);
    else memset(b->buf + m->bytes_in_use - (old - newlen), 0, old - newlen);
    m->bytes_in_use = m->bytes_in_use + newlen - old;
    a->length = newlen;
    b->dirty = 1;
    return 0;
}

/* the sort order of attributes in a record: type, name, then (for resident
 * indexed attributes such as $FILE_NAME) the value */
static int attr_order(ntfs_t *fs, const ntfs_attr_t *a, const ntfs_attr_t *b) {
    if (a->type != b->type) return a->type < b->type ? -1 : 1;
    int c = nt_name_cmp(fs, (const u16 *)((const u8 *)a + a->name_offset), a->name_length,
                        (const u16 *)((const u8 *)b + b->name_offset), b->name_length, 1);
    if (!c) c = nt_name_cmp(fs, (const u16 *)((const u8 *)a + a->name_offset), a->name_length,
                            (const u16 *)((const u8 *)b + b->name_offset), b->name_length, 0);
    if (c) return c;
    if (!a->non_resident && !b->non_resident && a->type == NTFS_AT_FILE_NAME) {
        u32 la = a->r.value_length, lb = b->r.value_length;
        int d = memcmp((const u8 *)a + a->r.value_offset, (const u8 *)b + b->r.value_offset, MIN(la, lb));
        if (d) return d;
        return la < lb ? -1 : la > lb;
    }
    if (a->non_resident && b->non_resident)
        return a->nr.lowest_vcn < b->nr.lowest_vcn ? -1 : a->nr.lowest_vcn > b->nr.lowest_vcn;
    return 0;
}

/* insert attribute a (a->length bytes; the instance is assigned here) */
int nt_rec_attr_insert(ntfs_t *fs, fblk_t *b, const ntfs_attr_t *a, u32 *off_out) {
    ntfs_mft_record_t *m = fb_rec(b);
    u32 len = a->length;
    if (len > m->bytes_allocated - m->bytes_in_use) return E_NOSPC;
    u32 pos = 0, off = 0, at = 0;
    ntfs_attr_t *x;
    int r;
    while ((r = nt_rec_walk(fs, b, &pos, &off, &x)) == 1) {
        if (attr_order(fs, a, x) < 0) { at = off; break; }
    }
    if (r < 0) return r;
    if (!at) at = pos ? pos : m->attrs_offset;
    memmove(b->buf + at + len, b->buf + at, m->bytes_in_use - at);
    memcpy(b->buf + at, a, len);
    ntfs_attr_t *n = (ntfs_attr_t *)(b->buf + at);
    n->instance = m->next_attr_instance;
    m->next_attr_instance = (u16)(m->next_attr_instance + 1);
    m->bytes_in_use += len;
    b->dirty = 1;
    if (off_out) *off_out = at;
    return 0;
}

void nt_rec_attr_remove(ntfs_t *fs, fblk_t *b, u32 off) {
    ntfs_mft_record_t *m = fb_rec(b);
    u32 len = ((ntfs_attr_t *)(b->buf + off))->length;
    memmove(b->buf + off, b->buf + off + len, m->bytes_in_use - off - len);
    m->bytes_in_use -= len;
    memset(b->buf + m->bytes_in_use, 0, len);
    b->dirty = 1;
}

/* ------------------------------------------------------------------ */
/* inodes                                                               */

static int alist_ok(const u8 *al, u32 len) {
    u32 o = 0;
    while (o < len) {
        if (len - o < sizeof(ntfs_attr_list_entry_t)) return 0;
        const ntfs_attr_list_entry_t *e = (const ntfs_attr_list_entry_t *)(al + o);
        if (e->length < sizeof(ntfs_attr_list_entry_t) || e->length > len - o) return 0;
        if (e->name_length && (u32)e->name_offset + 2u * e->name_length > e->length) return 0;
        o += e->length;
    }
    return 1;
}

int nt_ino_open(ntfs_t *fs, u64 mref, int check_seq, nino_t *ino) {
    memset(ino, 0, sizeof *ino);
    ino->fs = fs;
    ino->rec = NTFS_MREF(mref);
    int r = nt_rec_get(fs, ino->rec, &ino->base);
    if (r) return r;
    ntfs_mft_record_t *m = fb_rec(ino->base);
    ino->seq = m->sequence;
    if (!(m->flags & NTFS_MFT_IN_USE) || m->base_record) { r = E_NOENT; goto fail; }
    if (check_seq && NTFS_MSEQ(mref) && NTFS_MSEQ(mref) != m->sequence) { r = E_NOENT; goto fail; }
    /* the attribute list, if any */
    u32 pos = 0, off;
    ntfs_attr_t *a;
    while ((r = nt_rec_walk(fs, ino->base, &pos, &off, &a)) == 1) {
        if (a->type != NTFS_AT_ATTRIBUTE_LIST) continue;
        if (!a->non_resident) {
            u32 len = a->r.value_length;
            ino->al = kmalloc(len ? len : 1);
            if (!ino->al) { r = E_NOMEM; goto fail; }
            memcpy(ino->al, attr_value(a), len);
            ino->al_len = len;
        } else {
            nrl_t rl = { 0 };
            u64 len = (u64)a->nr.data_size;
            if (a->nr.lowest_vcn || len > NT_MAX_ALIST || (u64)a->nr.initialized_size < len) { r = E_IO; goto fail; }
            r = nrl_decode(fs, a, &rl);
            if (!r && nrl_vcns(&rl) << fs->cshift < len) r = E_IO;
            if (!r && !(ino->al = kmalloc(len ? len : 1))) r = E_NOMEM;
            if (!r) r = nt_runs_rw(fs, &rl, 0, (u32)len, ino->al, 0);
            nrl_free(&rl);
            if (r) goto fail;
            ino->al_len = (u32)len;
            ino->al_nonres = 1;
        }
        if (!alist_ok(ino->al, ino->al_len)) { r = E_IO; goto fail; }
        break;
    }
    if (r < 0) goto fail;
    return 0;
fail:
    if (r == E_IO) nt_corrupt(fs, "damaged MFT record or attribute list", ino->rec);
    nt_ino_close(ino);
    return r;
}

void nt_ino_close(nino_t *ino) {
    if (ino->base) nt_fb_put(ino->fs, ino->base);
    ino->base = NULL;
    kfree(ino->al);
    ino->al = NULL;
}

/* ------------------------------------------------------------------ */
/* attribute search                                                     */

static int name_match(actx_t *c, const u16 *n, u8 nlen) {
    if (c->any_name) return 1;
    if (nlen != c->nlen) return 0;
    return !nlen || !memcmp(n, c->name, 2u * nlen);
}

static void ac_drop(actx_t *c) {
    if (c->rec) nt_fb_put(c->ino->fs, c->rec);
    c->rec = NULL;
}

/* the next matching attribute: 0 found, E_NOENT at the end, <0 corrupt */
static int ac_step(actx_t *c) {
    nino_t *ino = c->ino;
    ntfs_t *fs = ino->fs;
    if (!ino->al) {
        ntfs_attr_t *a;
        u32 off;
        int r;
        if (!c->rec) { c->rec = ino->base; c->rec->pins++; }
        while ((r = nt_rec_walk(fs, c->rec, &c->pos, &off, &a)) == 1) {
            if ((!c->type || a->type == c->type) && name_match(c, attr_name(a), a->name_length)) {
                c->off = off;
                return 0;
            }
        }
        if (r < 0) { nt_corrupt(fs, "damaged attribute", ino->rec); return E_IO; }
        return E_NOENT;
    }
    while (c->pos < ino->al_len) {
        ntfs_attr_list_entry_t *e = (ntfs_attr_list_entry_t *)(ino->al + c->pos);
        c->pos += e->length;
        if (c->type && e->type != c->type) continue;
        if (!name_match(c, (const u16 *)((u8 *)e + e->name_offset), e->name_length)) continue;
        u64 rec = NTFS_MREF(e->mft_reference);
        ac_drop(c);
        if (rec == ino->rec) {
            c->rec = ino->base;
            c->rec->pins++;
        } else {
            int r = nt_rec_get(fs, rec, &c->rec);
            if (r) { c->rec = NULL; if (r == E_IO) nt_corrupt(fs, "unreadable extension record", rec); return r; }
            ntfs_mft_record_t *m = fb_rec(c->rec);
            if (!(m->flags & NTFS_MFT_IN_USE) || NTFS_MREF(m->base_record) != ino->rec ||
                (NTFS_MSEQ(e->mft_reference) && NTFS_MSEQ(e->mft_reference) != m->sequence)) {
                nt_corrupt(fs, "attribute list points to a foreign record", ino->rec);
                return E_IO;
            }
        }
        u32 pos = 0, off;
        ntfs_attr_t *a;
        int r;
        while ((r = nt_rec_walk(fs, c->rec, &pos, &off, &a)) == 1)
            if (a->type == e->type && a->instance == e->instance) break;
        if (r != 1) {
            nt_corrupt(fs, "attribute list entry without its attribute", ino->rec);
            return E_IO;
        }
        if (a->name_length != e->name_length ||
            memcmp(attr_name(a), (u8 *)e + e->name_offset, 2u * e->name_length) ||
            (a->non_resident ? a->nr.lowest_vcn != e->lowest_vcn : e->lowest_vcn != 0)) {
            nt_corrupt(fs, "attribute list does not match the attribute", ino->rec);
            return E_IO;
        }
        c->off = off;
        return 0;
    }
    return E_NOENT;
}

int nt_attr_first(nino_t *ino, actx_t *c, u32 type, const u16 *name, u8 nlen, int any_name) {
    memset(c, 0, sizeof *c);
    c->ino = ino;
    c->type = type;
    c->name = name;
    c->nlen = nlen;
    c->any_name = (u8)any_name;
    int r = ac_step(c);
    if (r) ac_drop(c);
    return r;
}

int nt_attr_next(actx_t *c) {
    int r = ac_step(c);
    if (r) ac_drop(c);
    return r;
}

void nt_actx_done(actx_t *c) { ac_drop(c); }

/* the first extent (lowest_vcn 0) of an attribute */
int nt_attr_find(nino_t *ino, actx_t *c, u32 type, const u16 *name, u8 nlen) {
    int r = nt_attr_first(ino, c, type, name, nlen, 0);
    while (!r) {
        ntfs_attr_t *a = ac_attr(c);
        if (!a->non_resident || a->nr.lowest_vcn == 0) return 0;
        r = nt_attr_next(c);
    }
    return r;
}

/* sizes of an attribute and (rl != NULL) its whole run list */
int nt_attr_info(nino_t *ino, u32 type, const u16 *name, u8 nlen, ainfo_t *ai, nrl_t *rl) {
    ntfs_t *fs = ino->fs;
    actx_t c;
    memset(ai, 0, sizeof *ai);
    int r = nt_attr_find(ino, &c, type, name, nlen);
    if (r) return r;
    ntfs_attr_t *a = ac_attr(&c);
    ai->flags = a->flags;
    if (!a->non_resident) {
        ai->resident = 1;
        ai->size = ai->init = a->r.value_length;
        ai->alloc = ALIGN_UP((u64)a->r.value_length, 8u);
        ai->value_off = a->r.value_offset;
        nt_actx_done(&c);
        return 0;
    }
    ai->size = (u64)a->nr.data_size;
    ai->alloc = (u64)a->nr.allocated_size;
    ai->init = MIN((u64)a->nr.initialized_size, ai->size);
    ai->cu = a->nr.compression_unit;
    ai->comp = (a->flags & (NTFS_ATTR_COMPRESSED | NTFS_ATTR_SPARSE)) && a->nr.mapping_pairs_offset >= 0x48
               ? (u64)a->nr.compressed_size : ai->alloc;
    nt_actx_done(&c);
    if (!rl) return 0;
    /* every extent, in VCN order: an attribute list keeps them sorted */
    rl->n = 0;
    r = nt_attr_first(ino, &c, type, name, nlen, 0);
    u32 ext = 0;
    while (!r) {
        a = ac_attr(&c);
        if (!a->non_resident || ++ext > NT_MAX_EXTENTS || (u64)a->nr.lowest_vcn != nrl_vcns(rl)) {
            nt_actx_done(&c);
            nt_corrupt(fs, "attribute extents out of order", ino->rec);
            return E_IO;
        }
        if ((r = nrl_decode(fs, a, rl))) {
            nt_actx_done(&c);
            if (r == E_IO) nt_corrupt(fs, "damaged run list", ino->rec);
            return r;
        }
        r = nt_attr_next(&c);
    }
    if (r != E_NOENT) return r;
    if ((nrl_vcns(rl) << fs->cshift) < ai->init || ai->size > (1ull << 52)) {
        nt_corrupt(fs, "run list shorter than the attribute", ino->rec);
        return E_IO;
    }
    return 0;
}

/* read part of an (uncompressed) attribute's value: returns bytes read */
int nt_attr_read(nino_t *ino, u32 type, const u16 *name, u8 nlen, u64 pos, u32 len, void *buf) {
    ntfs_t *fs = ino->fs;
    ainfo_t ai;
    nrl_t rl = { 0 };
    int r = nt_attr_info(ino, type, name, nlen, &ai, &rl);
    if (r) { nrl_free(&rl); return r; }
    if (pos >= ai.size) { nrl_free(&rl); return 0; }
    if (len > ai.size - pos) len = (u32)(ai.size - pos);
    if (len > 0x7FFFFFFF) len = 0x7FFFFFFF;
    if (ai.resident) {
        actx_t c;
        if (!(r = nt_attr_find(ino, &c, type, name, nlen))) {
            memcpy(buf, attr_value(ac_attr(&c)) + pos, len);
            nt_actx_done(&c);
        }
        return r ? r : (int)len;
    }
    if (ai.flags & (NTFS_ATTR_COMPRESSED | NTFS_ATTR_ENCRYPTED)) { nrl_free(&rl); return E_NOTSUP; }
    u32 disk = pos < ai.init ? (u32)MIN((u64)len, ai.init - pos) : 0;
    if (disk) r = nt_runs_rw(fs, &rl, pos, disk, buf, 0);
    if (!r && disk < len) memset((u8 *)buf + disk, 0, len - disk);
    nrl_free(&rl);
    return r ? r : (int)len;
}

/* ------------------------------------------------------------------ */
/* the attribute list (writes)                                          */

/* write the attribute list (ino->al) back: it is always in the base record
 * (and never listed in itself); a non-resident list gets or gives back
 * clusters as its length changes */
static int al_store(nino_t *ino) {
    ntfs_t *fs = ino->fs;
    u32 pos = 0, off;
    ntfs_attr_t *a;
    int r;
    while ((r = nt_rec_walk(fs, ino->base, &pos, &off, &a)) == 1)
        if (a->type == NTFS_AT_ATTRIBUTE_LIST) break;
    if (r != 1) return E_IO;
    if (!a->non_resident) {
        r = nt_rec_attr_resize(fs, ino->base, off, a->r.value_offset + ino->al_len);
        if (r) return r;
        a = (ntfs_attr_t *)(ino->base->buf + off);
        a->r.value_length = ino->al_len;
        memcpy(attr_value(a), ino->al, ino->al_len);
        ino->base->dirty = 1;
        return 0;
    }
    nrl_t rl = { 0 }, tail = { 0 };
    if (a->nr.lowest_vcn || (r = nrl_decode(fs, a, &rl))) { nrl_free(&rl); return r ? r : E_IO; }
    u64 have = nrl_vcns(&rl), need = ((u64)ino->al_len + fs->csize - 1) >> fs->cshift;
    if (!need) need = 1;
    if (need > have && (r = nt_clus_alloc(fs, need - have, fs->alloc_hint, 0, &rl, (i64)have))) { nrl_free(&rl); return r; }
    for (u32 i = 0; i < rl.n && !r; i++) {
        u64 s = (u64)rl.r[i].vcn, e = s + rl.r[i].len, lim = MIN(have, need);
        if (e <= lim || rl.r[i].lcn < 0) continue;
        u64 f = MAX(s, lim);
        r = nrl_add(&tail, (i64)f, rl.r[i].lcn + (i64)(f - s), e - f);
    }
    if (need < have) nrl_truncate(&rl, need);
    if (!r) r = nt_runs_rw(fs, &rl, 0, ino->al_len, ino->al, 1);
    int mplen = nrl_encode(&rl, 0, (i64)need, NULL, 0x7FFFFFFF);
    u32 mpoff = a->nr.mapping_pairs_offset;
    if (!r) r = nt_rec_attr_resize(fs, ino->base, off, mpoff + (u32)mplen);
    if (!r) {
        a = (ntfs_attr_t *)(ino->base->buf + off);
        nrl_encode(&rl, 0, (i64)need, (u8 *)a + mpoff, a->length - mpoff);
        a->nr.highest_vcn = (i64)need - 1;
        a->nr.allocated_size = (i64)(need << fs->cshift);
        a->nr.data_size = a->nr.initialized_size = ino->al_len;
        ino->base->dirty = 1;
        if (need < have) nt_clus_free_runs(fs, &tail, 0);
    } else if (need > have) {
        nt_clus_free_runs(fs, &tail, 0);
    }
    nrl_free(&rl);
    nrl_free(&tail);
    return r;
}

static int al_cmp(ntfs_t *fs, const ntfs_attr_list_entry_t *a, const ntfs_attr_list_entry_t *b) {
    if (a->type != b->type) return a->type < b->type ? -1 : 1;
    int c = nt_name_cmp(fs, (const u16 *)((const u8 *)a + a->name_offset), a->name_length,
                        (const u16 *)((const u8 *)b + b->name_offset), b->name_length, 0);
    if (c) return c;
    return a->lowest_vcn < b->lowest_vcn ? -1 : a->lowest_vcn > b->lowest_vcn;
}

/* add an entry for attribute a (in record rec) to the list */
static int al_add(nino_t *ino, fblk_t *rec, const ntfs_attr_t *a) {
    ntfs_t *fs = ino->fs;
    u32 elen = ALIGN_UP((u32)sizeof(ntfs_attr_list_entry_t) + 2u * a->name_length, 8u);
    u8 *nal = kmalloc(ino->al_len + elen);
    if (!nal) return E_NOMEM;
    u8 ebuf[sizeof(ntfs_attr_list_entry_t) + 2 * 255 + 8];
    memset(ebuf, 0, sizeof ebuf);
    ntfs_attr_list_entry_t *e = (ntfs_attr_list_entry_t *)ebuf;
    e->type = a->type;
    e->length = (u16)elen;
    e->name_length = a->name_length;
    e->name_offset = (u8)sizeof(ntfs_attr_list_entry_t);
    e->lowest_vcn = a->non_resident ? a->nr.lowest_vcn : 0;
    u64 recno = NTFS_MREF(rec->key & ~NT_KEY_REC);
    e->mft_reference = NTFS_MK_MREF(recno, fb_rec(rec)->sequence);
    e->instance = a->instance;
    memcpy(ebuf + e->name_offset, (const u8 *)a + a->name_offset, 2u * a->name_length);
    u32 o = 0;
    while (o < ino->al_len) {
        ntfs_attr_list_entry_t *x = (ntfs_attr_list_entry_t *)(ino->al + o);
        if (al_cmp(fs, e, x) < 0) break;
        o += x->length;
    }
    memcpy(nal, ino->al, o);
    memcpy(nal + o, ebuf, elen);
    memcpy(nal + o + elen, ino->al + o, ino->al_len - o);
    u8 *old = ino->al;
    u32 oldlen = ino->al_len;
    ino->al = nal;
    ino->al_len += elen;
    int r = al_store(ino);
    if (r) { ino->al = old; ino->al_len = oldlen; kfree(nal); return r; }
    kfree(old);
    return 0;
}

static int al_remove(nino_t *ino, u64 recno, u32 type, u16 instance) {
    u32 o = 0;
    while (o < ino->al_len) {
        ntfs_attr_list_entry_t *x = (ntfs_attr_list_entry_t *)(ino->al + o);
        if (x->type == type && x->instance == instance && NTFS_MREF(x->mft_reference) == recno) {
            u32 l = x->length;
            memmove(ino->al + o, ino->al + o + l, ino->al_len - o - l);
            ino->al_len -= l;
            return al_store(ino);
        }
        o += x->length;
    }
    return E_IO;
}

/* ------------------------------------------------------------------ */
/* attribute changes                                                    */

/* remove the attribute at c (c is released) */
int nt_attr_remove(nino_t *ino, actx_t *c) {
    ntfs_t *fs = ino->fs;
    ntfs_attr_t *a = ac_attr(c);
    u32 type = a->type;
    u16 inst = a->instance;
    u64 recno = c->rec->key & ~NT_KEY_REC;
    fblk_t *rec = c->rec;
    rec->pins++;
    nt_actx_done(c);
    nt_rec_attr_remove(fs, rec, c->off);
    int r = 0;
    if (ino->al) {
        r = al_remove(ino, recno, type, inst);
        /* an extension record without attributes is freed */
        ntfs_mft_record_t *m = fb_rec(rec);
        if (!r && rec != ino->base && m->bytes_in_use <= (u32)m->attrs_offset + 8) {
            m->flags = 0;
            m->sequence = (u16)(m->sequence + 1 ? m->sequence + 1 : 1);
            rec->dirty = 1;
            r = nt_mft_free(fs, recno);
        }
    }
    nt_fb_put(fs, rec);
    return r;
}

/* the value of a resident attribute: replace it (the attribute must exist) */
int nt_attr_set_resident(nino_t *ino, u32 type, const u16 *name, u8 nlen, const void *val, u32 len) {
    actx_t c;
    int r = nt_attr_find(ino, &c, type, name, nlen);
    if (r) return r;
    ntfs_attr_t *a = ac_attr(&c);
    if (a->non_resident) { nt_actx_done(&c); return E_INVAL; }
    u32 voff = a->r.value_offset ? a->r.value_offset : ALIGN_UP(NTFS_ATTR_RESIDENT_HDR + 2u * a->name_length, 8u);
    r = nt_rec_attr_resize(ino->fs, c.rec, c.off, voff + len);
    if (!r) {
        a = ac_attr(&c);
        a->r.value_offset = (u16)voff;
        a->r.value_length = len;
        memcpy((u8 *)a + voff, val, len);
        c.rec->dirty = 1;
    }
    nt_actx_done(&c);
    return r;
}

/* put attribute a into an extension record with room for it (one the file
 * has, or a new one) and list it */
static int ext_insert(nino_t *ino, const ntfs_attr_t *a) {
    ntfs_t *fs = ino->fs;
    fblk_t *b = NULL;
    u32 off;
    int r;
    for (u32 o = 0; o < ino->al_len && !b;) {
        ntfs_attr_list_entry_t *e = (ntfs_attr_list_entry_t *)(ino->al + o);
        o += e->length;
        u64 rec = NTFS_MREF(e->mft_reference);
        if (rec == ino->rec || nt_rec_get(fs, rec, &b)) continue;
        ntfs_mft_record_t *m = fb_rec(b);
        if (!(m->flags & NTFS_MFT_IN_USE) || NTFS_MREF(m->base_record) != ino->rec ||
            nt_rec_free_space(b) < a->length + 8) {
            nt_fb_put(fs, b);
            b = NULL;
        }
    }
    u64 newrec = 0;
    if (!b) {
        if ((r = nt_mft_alloc(fs, &newrec))) return r;
        u16 seq = 1;
        if (!nt_rec_get(fs, newrec, &b)) {
            if (fb_rec(b)->flags & NTFS_MFT_IN_USE) { nt_fb_put(fs, b); nt_corrupt(fs, "a free MFT record is in use", newrec); return E_IO; }
            seq = fb_rec(b)->sequence ? fb_rec(b)->sequence : 1;
            nt_fb_put(fs, b);
        }
        if ((r = nt_rec_get_fresh(fs, newrec, &b))) { nt_mft_free(fs, newrec); return r; }
        nt_rec_init(fs, b, newrec, seq, NTFS_MFT_IN_USE);
        fb_rec(b)->base_record = NTFS_MK_MREF(ino->rec, ino->seq);
    }
    r = nt_rec_attr_insert(fs, b, a, &off);
    if (!r) {
        r = al_add(ino, b, (ntfs_attr_t *)(b->buf + off));
        if (r) nt_rec_attr_remove(fs, b, off);
    }
    if (r && newrec) {
        fb_rec(b)->flags = 0;
        b->dirty = 1;
        nt_mft_free(fs, newrec);
    }
    nt_fb_put(fs, b);
    return r;
}

/* add a resident attribute to the base record (an extension record when
 * the file has an attribute list and the base record is full) */
int nt_attr_add_resident(nino_t *ino, u32 type, const u16 *name, u8 nlen, const void *val, u32 len, u8 rflags) {
    u32 noff = NTFS_ATTR_RESIDENT_HDR, voff = ALIGN_UP(noff + 2u * nlen, 8u);
    u32 total = ALIGN_UP(voff + len, 8u);
    u8 *buf = kzalloc(total);
    if (!buf) return E_NOMEM;
    ntfs_attr_t *a = (ntfs_attr_t *)buf;
    a->type = type;
    a->length = total;
    a->name_length = nlen;
    a->name_offset = (u16)noff;
    a->r.value_length = len;
    a->r.value_offset = (u16)voff;
    a->r.resident_flags = rflags;
    if (nlen) memcpy(buf + noff, name, 2u * nlen);
    memcpy(buf + voff, val, len);
    u32 off;
    int r = nt_rec_attr_insert(ino->fs, ino->base, a, &off);
    if (r == E_NOSPC && ino->al && type != NTFS_AT_STANDARD_INFORMATION) {
        /* the base record is full: an extension record of the file */
        r = ext_insert(ino, a);
        kfree(buf);
        return r;
    }
    kfree(buf);
    if (r) return r;
    if (ino->al) {
        r = al_add(ino, ino->base, (ntfs_attr_t *)(ino->base->buf + off));
        if (r) nt_rec_attr_remove(ino->fs, ino->base, off);
    }
    return r;
}

/* rewrite the first (and only) extent of a non-resident attribute with
 * the run list rl and the sizes in ai */
int nt_attr_store_runs(nino_t *ino, u32 type, const u16 *name, u8 nlen, const nrl_t *rl, const ainfo_t *ai) {
    ntfs_t *fs = ino->fs;
    actx_t c;
    int r = nt_attr_find(ino, &c, type, name, nlen);
    if (r) return r;
    ntfs_attr_t *a = ac_attr(&c);
    if (!a->non_resident) { nt_actx_done(&c); return E_INVAL; }
    /* only single-extent attributes are rewritten */
    actx_t c2;
    int more = nt_attr_first(ino, &c2, type, name, nlen, 0);
    int extents = 0;
    while (!more) { extents++; more = nt_attr_next(&c2); }
    if (extents != 1) { nt_actx_done(&c); return E_NOTSUP; }
    u64 vcns = nrl_vcns(rl);
    int mplen = nrl_encode(rl, 0, (i64)vcns, NULL, 0x7FFFFFFF);
    u32 mpoff = a->nr.mapping_pairs_offset;
    r = nt_rec_attr_resize(fs, c.rec, c.off, mpoff + (u32)mplen);
    if (!r) {
        a = ac_attr(&c);
        nrl_encode(rl, 0, (i64)vcns, (u8 *)a + mpoff, a->length - mpoff);
        a->nr.lowest_vcn = 0;
        a->nr.highest_vcn = (i64)vcns - 1;
        a->nr.allocated_size = (i64)ai->alloc;
        a->nr.data_size = (i64)ai->size;
        a->nr.initialized_size = (i64)ai->init;
        if ((a->flags & (NTFS_ATTR_COMPRESSED | NTFS_ATTR_SPARSE)) && mpoff >= 0x48) a->nr.compressed_size = (i64)ai->comp;
        c.rec->dirty = 1;
    }
    nt_actx_done(&c);
    return r;
}

int nt_attr_set_sizes(nino_t *ino, u32 type, const u16 *name, u8 nlen, const ainfo_t *ai) {
    actx_t c;
    int r = nt_attr_find(ino, &c, type, name, nlen);
    if (r) return r;
    ntfs_attr_t *a = ac_attr(&c);
    if (!a->non_resident) { nt_actx_done(&c); return E_INVAL; }
    a->nr.data_size = (i64)ai->size;
    a->nr.initialized_size = (i64)ai->init;
    a->nr.allocated_size = (i64)ai->alloc;
    c.rec->dirty = 1;
    nt_actx_done(&c);
    return 0;
}

/* make a resident attribute non-resident, with at least min_alloc bytes
 * allocated (the value is written to the new clusters) */
int nt_attr_make_nonres(nino_t *ino, u32 type, const u16 *name, u8 nlen, u64 min_alloc) {
    ntfs_t *fs = ino->fs;
    actx_t c;
    int r = nt_attr_find(ino, &c, type, name, nlen);
    if (r) return r;
    ntfs_attr_t *a = ac_attr(&c);
    if (a->non_resident) { nt_actx_done(&c); return 0; }
    u32 vlen = a->r.value_length;
    u64 bytes = MAX((u64)vlen, min_alloc);
    u64 nclus = (bytes + fs->csize - 1) >> fs->cshift;
    nrl_t rl = { 0 };
    u8 *val = kmalloc(vlen ? vlen : 1);
    if (!val) { nt_actx_done(&c); return E_NOMEM; }
    memcpy(val, attr_value(a), vlen);
    u16 aflags = a->flags;
    nt_actx_done(&c);
    if (nclus && (r = nt_clus_alloc(fs, nclus, fs->alloc_hint, 0, &rl, 0))) goto out;
    if (vlen) {
        /* the value, and zeros to the end of its last cluster */
        u32 tail = (u32)(((u64)vlen + fs->csize - 1) / fs->csize * fs->csize);
        u8 *tmp = kzalloc(MIN(tail, 65536u));
        if (!tmp) { r = E_NOMEM; goto undo; }
        for (u32 o = 0; o < tail && !r; o += 65536) {
            u32 k = MIN(65536u, tail - o);
            memset(tmp, 0, k);
            if (o < vlen) memcpy(tmp, val + o, MIN(k, vlen - o));
            r = nt_runs_rw(fs, &rl, o, k, tmp, 1);
        }
        kfree(tmp);
        if (r) goto undo;
    }
    /* the new attribute record, in place of the old one (same instance) */
    {
        int mplen = nrl_encode(&rl, 0, (i64)nclus, NULL, 0x7FFFFFFF);
        u32 noff = NTFS_ATTR_NONRES_HDR, mpoff = ALIGN_UP(noff + 2u * nlen, 8u);
        u32 total = ALIGN_UP(mpoff + (u32)mplen, 8u);
        if ((r = nt_attr_find(ino, &c, type, name, nlen))) goto undo;
        a = ac_attr(&c);
        u16 inst = a->instance;
        r = nt_rec_attr_resize(fs, c.rec, c.off, total);
        if (r) { nt_actx_done(&c); goto undo; }
        a = ac_attr(&c);
        memset((u8 *)a + 8, 0, total - 8);
        a->length = total;
        a->non_resident = 1;
        a->name_length = nlen;
        a->name_offset = (u16)noff;
        a->flags = aflags;
        a->instance = inst;
        if (nlen) memcpy((u8 *)a + noff, name, 2u * nlen);
        a->nr.lowest_vcn = 0;
        a->nr.highest_vcn = (i64)nclus - 1;
        a->nr.mapping_pairs_offset = (u16)mpoff;
        a->nr.allocated_size = (i64)(nclus << fs->cshift);
        a->nr.data_size = vlen;
        a->nr.initialized_size = vlen;
        nrl_encode(&rl, 0, (i64)nclus, (u8 *)a + mpoff, total - mpoff);
        c.rec->dirty = 1;
        nt_actx_done(&c);
    }
    goto out;
undo:
    nt_clus_free_runs(fs, &rl, 0);
out:
    nrl_free(&rl);
    kfree(val);
    return r;
}

/* Resize a single-extent, uncompressed, non-sparse non-resident attribute
 * (metadata: index allocations and bitmaps) to new_size bytes, allocating
 * or freeing clusters; zero_new zeroes newly allocated clusters. The
 * initialized size follows the data size. */
int nt_attr_resize_nr(nino_t *ino, u32 type, const u16 *name, u8 nlen, u64 new_size, int zero_new) {
    ntfs_t *fs = ino->fs;
    ainfo_t ai;
    nrl_t rl = { 0 }, extra = { 0 };
    int r = nt_attr_info(ino, type, name, nlen, &ai, &rl);
    if (r) { nrl_free(&rl); return r; }
    if (ai.resident || (ai.flags & (NTFS_ATTR_COMPRESSED | NTFS_ATTR_SPARSE | NTFS_ATTR_ENCRYPTED))) {
        nrl_free(&rl);
        return E_NOTSUP;
    }
    u64 have = nrl_vcns(&rl), need = (new_size + fs->csize - 1) >> fs->cshift;
    if (need > have) {
        i64 hint = rl.n && rl.r[rl.n - 1].lcn >= 0 ? rl.r[rl.n - 1].lcn + (i64)rl.r[rl.n - 1].len : (i64)fs->alloc_hint;
        r = nt_clus_alloc(fs, need - have, (u64)hint, 0, &rl, (i64)have);
        if (r) { nrl_free(&rl); return r; }
    }
    /* extra: the clusters past min(have, need) (new ones, or ones to free) */
    for (u32 i = 0; i < rl.n && !r; i++) {
        u64 s = (u64)rl.r[i].vcn, e = s + rl.r[i].len, lim = MIN(have, need);
        if (e <= lim || rl.r[i].lcn < 0) continue;
        u64 from = MAX(s, lim);
        r = nrl_add(&extra, (i64)from, rl.r[i].lcn + (i64)(from - s), e - from);
    }
    if (!r && need > have && zero_new) {
        u8 *z = kzalloc(65536);
        if (!z) r = E_NOMEM;
        for (u64 o = have << fs->cshift; !r && o < need << fs->cshift; o += 65536)
            r = nt_runs_rw(fs, &rl, o, (u32)MIN(65536ull, (need << fs->cshift) - o), z, 1);
        kfree(z);
        for (u32 i = 0; !r && i < extra.n; i++)      /* stale cached copies (the clusters were free) */
            nt_inval_range(fs, (u64)extra.r[i].lcn << fs->cshift, extra.r[i].len << fs->cshift);
    }
    if (need < have) nrl_truncate(&rl, need);
    ainfo_t na = ai;
    na.size = na.init = new_size;
    na.alloc = need << fs->cshift;
    if (!r) r = nt_attr_store_runs(ino, type, name, nlen, &rl, &na);
    if (r) {
        if (need > have) nt_clus_free_runs(fs, &extra, 0);
    } else if (need < have) {
        r = nt_clus_free_runs(fs, &extra, 0);
    }
    nrl_free(&rl);
    nrl_free(&extra);
    return r;
}

/* free everything a file owns: clusters of every non-resident attribute,
 * extension records, then the base record (deletion) */
int nt_ino_free_all(nino_t *ino) {
    ntfs_t *fs = ino->fs;
    actx_t c;
    int r = nt_attr_first(ino, &c, 0, NULL, 0, 1), err = 0;
    while (!r) {
        ntfs_attr_t *a = ac_attr(&c);
        if (a->non_resident) {
            nrl_t rl = { 0 };
            int k = nrl_decode(fs, a, &rl);
            if (!k) k = nt_clus_free_runs(fs, &rl, 0);
            else nt_corrupt(fs, "damaged run list (clusters not freed)", ino->rec);
            if (k && !err) err = k;
            nrl_free(&rl);
        }
        r = nt_attr_next(&c);
    }
    if (r != E_NOENT && !err) err = r;
    /* a non-resident attribute list's own clusters */
    u32 pos = 0, off;
    ntfs_attr_t *a;
    while (nt_rec_walk(fs, ino->base, &pos, &off, &a) == 1) {
        if (a->type == NTFS_AT_ATTRIBUTE_LIST && a->non_resident) {
            nrl_t rl = { 0 };
            if (!nrl_decode(fs, a, &rl)) nt_clus_free_runs(fs, &rl, 0);
            nrl_free(&rl);
        }
    }
    /* extension records */
    if (ino->al) {
        for (u32 o = 0; o < ino->al_len;) {
            ntfs_attr_list_entry_t *e = (ntfs_attr_list_entry_t *)(ino->al + o);
            o += e->length;
            u64 rec = NTFS_MREF(e->mft_reference);
            if (rec == ino->rec) continue;
            fblk_t *b;
            if (nt_rec_get(fs, rec, &b)) continue;
            ntfs_mft_record_t *m = fb_rec(b);
            if ((m->flags & NTFS_MFT_IN_USE) && NTFS_MREF(m->base_record) == ino->rec) {
                m->flags = 0;
                m->sequence = (u16)(m->sequence + 1 ? m->sequence + 1 : 1);
                b->dirty = 1;
                int k = nt_mft_free(fs, rec);
                if (k && !err) err = k;
            }
            nt_fb_put(fs, b);
        }
    }
    ntfs_mft_record_t *m = fb_rec(ino->base);
    m->flags = 0;
    m->link_count = 0;
    m->sequence = (u16)(m->sequence + 1 ? m->sequence + 1 : 1);
    ino->base->dirty = 1;
    int k = nt_mft_free(fs, ino->rec);
    return err ? err : k;
}
