/* Halcyon OS - NTFS: cluster allocation ($Bitmap) and MFT record
 * allocation ($MFT:$BITMAP, growing $MFT). See docs/DESIGN-M7.5.md.
 *
 * Clusters: contiguous first (one free extent large enough, searched from
 * the hint), otherwise the free extents in order. File data avoids the MFT
 * zone (the clusters after the MFT, reserved for its growth, as Windows
 * does) until the rest of the volume is full. */
#include "core/types.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "fs/ntfs_priv.h"

#define SCAN 4096u

static inline u32 pop8(u8 b) {
    b = (u8)(b - ((b >> 1) & 0x55));
    b = (u8)((b & 0x33) + ((b >> 2) & 0x33));
    return (u32)((b + (b >> 4)) & 0x0F);
}

/* ------------------------------------------------------------------ */
/* bits                                                                 */

/* set or clear n bits from bit first of the bitmap behind rl; *changed:
 * bits that actually changed */
static int bits_put(ntfs_t *fs, const nrl_t *rl, u64 first, u64 n, int val, u64 *changed) {
    u8 *buf = fs->scan;
    u64 ch = 0;
    while (n) {
        u64 byte = first >> 3;
        u64 chunk_start = byte & ~(u64)(SCAN - 1);
        u32 o = (u32)(byte - chunk_start);
        u64 lim = nrl_vcns(rl) << fs->cshift;
        if (byte >= lim) return E_IO;
        u32 len = (u32)MIN((u64)SCAN, lim - chunk_start);
        int r = nt_meta_rw(fs, rl, chunk_start, len, buf, 0);
        if (r) return r;
        while (n && o < len) {
            u32 bit = (u32)(first & 7);
            if (!bit && n >= 8) {
                u32 k = (u32)MIN(n / 8, (u64)(len - o));
                for (u32 i = 0; i < k; i++) {
                    u8 old = buf[o + i];
                    u8 nv = val ? 0xFF : 0x00;
                    ch += pop8((u8)(old ^ nv));
                    buf[o + i] = nv;
                }
                o += k;
                first += 8ull * k;
                n -= 8ull * k;
                continue;
            }
            u8 m = (u8)(1u << bit);
            if (!!(buf[o] & m) != !!val) ch++;
            if (val) buf[o] |= m; else buf[o] &= (u8)~m;
            first++;
            n--;
            if (!(first & 7)) o++;
        }
        if ((r = nt_meta_rw(fs, rl, chunk_start, len, buf, 1))) return r;
    }
    if (changed) *changed = ch;
    return 0;
}

static int bit_get(ntfs_t *fs, const nrl_t *rl, u64 bit, int *val) {
    u8 b;
    int r = nt_meta_rw(fs, rl, bit >> 3, 1, &b, 0);
    if (!r) *val = (b >> (bit & 7)) & 1;
    return r;
}

/* the first run of clear bits in [from, to) of at least minlen bits (or the
 * first run at all, minlen 1); *len is its length (capped at to) */
static int scan_free(ntfs_t *fs, const nrl_t *rl, u64 from, u64 to, u64 minlen, u64 *start, u64 *len) {
    u8 *buf = fs->scan;
    u64 run_s = 0, run_n = 0;
    u64 bit = from, lim = nrl_vcns(rl) << fs->cshift;
    if (to > lim * 8) to = lim * 8;
    while (bit < to) {
        u64 cs = (bit >> 3) & ~(u64)(SCAN - 1);
        u32 rl_len = (u32)MIN((u64)SCAN, lim - cs);
        int r = nt_meta_rw(fs, rl, cs, rl_len, buf, 0);
        if (r) return r;
        u64 cend = MIN(to, (cs + rl_len) * 8);
        while (bit < cend) {
            u32 o = (u32)((bit >> 3) - cs);
            if (!(bit & 7) && bit + 8 <= cend && (buf[o] == 0xFF || buf[o] == 0)) {
                if (buf[o] == 0xFF) {
                    if (run_n >= minlen) goto found;
                    run_n = 0;
                } else {
                    if (!run_n) run_s = bit;
                    run_n += 8;
                    if (minlen > 1 && run_n >= minlen) goto found;
                }
                bit += 8;
                continue;
            }
            if (buf[o] & (1u << (bit & 7))) {
                if (run_n >= minlen) goto found;
                run_n = 0;
            } else {
                if (!run_n) run_s = bit;
                run_n++;
                if (minlen > 1 && run_n >= minlen) goto found;
            }
            bit++;
        }
    }
    if (run_n >= minlen && run_n) goto found;
    return E_NOSPC;
found:
    *start = run_s;
    *len = run_n;
    return 0;
}

/* ------------------------------------------------------------------ */
/* clusters                                                             */

int nt_bmp_count_free(ntfs_t *fs) {
    u8 *buf = fs->scan;
    u64 used = 0;
    for (u64 byte = 0; byte < (fs->nclus + 7) / 8; byte += SCAN) {
        u32 n = (u32)MIN((u64)SCAN, (fs->nclus + 7) / 8 - byte);
        int r = nt_meta_rw(fs, &fs->bmp_runs, byte, n, buf, 0);
        if (r) return r;
        for (u32 i = 0; i < n; i++) {
            u8 b = buf[i];
            u64 bit0 = (byte + i) * 8;
            if (bit0 + 8 > fs->nclus) b &= (u8)((1u << (fs->nclus - bit0)) - 1);
            used += pop8(b);
        }
    }
    fs->free_clus = used > fs->nclus ? 0 : fs->nclus - used;
    return 0;
}

static int take(ntfs_t *fs, u64 s, u64 n, nrl_t *rl, i64 *vcn) {
    u64 ch;
    int r = bits_put(fs, &fs->bmp_runs, s, n, 1, &ch);
    if (r) return r;
    if (ch != n) {
        nt_corrupt(fs, "allocated clusters were marked in use", 6);
        return E_IO;
    }
    if (fs->free_clus != ~0ull) fs->free_clus -= MIN(n, fs->free_clus);
    if ((r = nrl_add(rl, *vcn, (i64)s, n))) { bits_put(fs, &fs->bmp_runs, s, n, 0, NULL); return r; }
    *vcn += (i64)n;
    return 0;
}

/* search ranges in order: data skips the MFT zone until the end */
static int ranges(ntfs_t *fs, u64 hint, int mft_zone, u64 rs[4], u64 re[4]) {
    u64 zs = fs->mft_zone_start, ze = fs->mft_zone_end, N = fs->nclus;
    if (hint >= N) hint = 0;
    int k = 0;
    if (mft_zone) {
        rs[k] = hint; re[k++] = N;
        rs[k] = 0; re[k++] = hint;
        return k;
    }
    if (hint >= zs && hint < ze) hint = ze;
    if (hint < zs) {
        rs[k] = hint; re[k++] = zs;
        rs[k] = ze; re[k++] = N;
        rs[k] = 0; re[k++] = hint;
    } else {
        rs[k] = hint; re[k++] = N;
        rs[k] = 0; re[k++] = zs;
        rs[k] = ze; re[k++] = hint;
    }
    rs[k] = zs; re[k++] = ze;
    return k;
}

int nt_clus_alloc(ntfs_t *fs, u64 n, u64 hint, int mft_zone, nrl_t *rl, i64 vcn) {
    if (!n) return 0;
    if (fs->free_clus == ~0ull) {
        int r = nt_bmp_count_free(fs);
        if (r) return r;
    }
    if (fs->free_clus < n) return E_NOSPC;
    u64 rs[4], re[4], s, len;
    int k = ranges(fs, hint, mft_zone, rs, re);
    /* one extent large enough */
    for (int i = 0; i < k; i++) {
        if (re[i] <= rs[i]) continue;
        int r = scan_free(fs, &fs->bmp_runs, rs[i], re[i], n, &s, &len);
        if (!r) {
            if ((r = take(fs, s, n, rl, &vcn))) return r;
            fs->alloc_hint = s + n;
            return 0;
        }
        if (r != E_NOSPC) return r;
    }
    /* the free extents in order */
    u32 n0 = rl->n;
    u64 left = n;
    for (int i = 0; i < k && left; i++) {
        u64 from = rs[i];
        while (left && from < re[i]) {
            int r = scan_free(fs, &fs->bmp_runs, from, re[i], 1, &s, &len);
            if (r == E_NOSPC) break;
            if (!r) r = take(fs, s, MIN(len, left), rl, &vcn);
            if (r) {
                for (u32 j = n0; j < rl->n; j++) nt_clus_free(fs, rl->r[j].lcn, rl->r[j].len);
                rl->n = n0;
                return r;
            }
            fs->alloc_hint = s + MIN(len, left);
            left -= MIN(len, left);
            from = s + len;
        }
    }
    if (left) {
        /* the free count was wrong */
        for (u32 j = n0; j < rl->n; j++) nt_clus_free(fs, rl->r[j].lcn, rl->r[j].len);
        rl->n = n0;
        fs->free_clus = ~0ull;
        return E_NOSPC;
    }
    return 0;
}

int nt_clus_free(ntfs_t *fs, i64 lcn, u64 n) {
    if (lcn < 0 || !n) return 0;
    if ((u64)lcn >= fs->nclus || n > fs->nclus - (u64)lcn) return E_IO;
    u64 ch;
    int r = bits_put(fs, &fs->bmp_runs, (u64)lcn, n, 0, &ch);
    if (r) return r;
    nt_inval_range(fs, (u64)lcn << fs->cshift, n << fs->cshift);
    if (ch != n) kprintf("ntfs: %s: freed %lu clusters that were already free\n", fs->bd->name, (unsigned long)(n - ch));
    if (fs->free_clus != ~0ull) fs->free_clus += ch;
    return 0;
}

int nt_clus_free_runs(ntfs_t *fs, const nrl_t *rl, u64 from_vcn) {
    int err = 0;
    for (u32 i = 0; i < rl->n; i++) {
        const nrun_t *x = &rl->r[i];
        u64 s = (u64)x->vcn, e = s + x->len;
        if (x->lcn < 0 || e <= from_vcn) continue;
        u64 f = MAX(s, from_vcn);
        int r = nt_clus_free(fs, x->lcn + (i64)(f - s), e - f);
        if (r && !err) err = r;
    }
    return err;
}

/* ------------------------------------------------------------------ */
/* MFT records                                                          */

int nt_mft_bit(ntfs_t *fs, u64 rec, int *used) {
    if (rec >= fs->mftbmp_size * 8) { *used = 0; return 0; }
    return bit_get(fs, &fs->mftbmp_runs, rec, used);
}

int nt_mft_free(ntfs_t *fs, u64 rec) {
    if (rec < NTFS_FILE_FIRST_USER || rec >= fs->mftbmp_size * 8) return E_IO;
    u64 ch;
    int r = bits_put(fs, &fs->mftbmp_runs, rec, 1, 0, &ch);
    if (!r && rec < fs->mft_hint) fs->mft_hint = rec;
    return r;
}

/* Grow $MFT by at least one cluster's worth of records (64 or an eighth
 * of the MFT, whichever is more): new clusters (from the MFT zone, after
 * the MFT if possible), the bitmap extended, the records formatted. Only an
 * $MFT whose $DATA and $BITMAP are single extents in record 0 can grow. */
static int mft_grow(ntfs_t *fs) {
    u32 per_clus = fs->csize >= fs->rec_size ? fs->csize / fs->rec_size : 1;
    u64 add = MAX(64ull, fs->mft_nrec / 8);
    add = ALIGN_UP(add, (u64)per_clus);
    if (fs->mft_nrec + add > (1ull << 32)) return E_NOSPC;
    u64 new_n = fs->mft_nrec + add;
    u64 new_bytes = new_n << fs->rec_shift;
    nrl_t runs = { 0 }, bruns = { 0 };
    nino_t ino;
    int r = nt_ino_open(fs, NTFS_FILE_MFT, 0, &ino);
    if (r) return r;
    ainfo_t ai, bi;
    if ((r = nt_attr_info(&ino, NTFS_AT_DATA, NULL, 0, &ai, &runs))) goto out;
    if ((r = nt_attr_info(&ino, NTFS_AT_BITMAP, NULL, 0, &bi, &bruns))) goto out;
    if (bi.resident) { r = E_NOTSUP; goto out; }
    u64 old_vcns = nrl_vcns(&runs), need_vcns = (new_bytes + fs->csize - 1) >> fs->cshift;
    if (need_vcns > old_vcns) {
        i64 hint = runs.n && runs.r[runs.n - 1].lcn >= 0 ? runs.r[runs.n - 1].lcn + (i64)runs.r[runs.n - 1].len : (i64)fs->mft_lcn;
        if ((r = nt_clus_alloc(fs, need_vcns - old_vcns, (u64)hint, 1, &runs, (i64)old_vcns))) goto out;
    }
    /* the bitmap: 8-byte multiples */
    u64 bsize = ALIGN_UP((new_n + 7) / 8, 8ull);
    u64 bvcns = nrl_vcns(&bruns), bneed = (bsize + fs->csize - 1) >> fs->cshift;
    if (bneed > bvcns) {
        i64 hint = bruns.n && bruns.r[bruns.n - 1].lcn >= 0 ? bruns.r[bruns.n - 1].lcn + (i64)bruns.r[bruns.n - 1].len : 0;
        if ((r = nt_clus_alloc(fs, bneed - bvcns, (u64)hint, 1, &bruns, (i64)bvcns))) goto undo_data;
        u8 *z = kzalloc(fs->csize);
        if (!z) { r = E_NOMEM; goto undo_bmp; }
        for (u64 v = bvcns; v < bneed && !r; v++) r = nt_runs_rw(fs, &bruns, v << fs->cshift, fs->csize, z, 1);
        kfree(z);
        for (u32 i = 0; i < bruns.n; i++)
            if ((u64)bruns.r[i].vcn + bruns.r[i].len > bvcns)
                nt_inval_range(fs, (u64)bruns.r[i].lcn << fs->cshift, bruns.r[i].len << fs->cshift);
        if (r) goto undo_bmp;
    }
    /* bitmap bytes between the old and new size must be zero (they may be
     * past the old initialized size) */
    if (bsize > fs->mftbmp_size) {
        u8 z[64];
        memset(z, 0, sizeof z);
        for (u64 o = fs->mftbmp_size; o < bsize && !r; o += sizeof z)
            r = nt_meta_rw(fs, &bruns, o, (u32)MIN((u64)sizeof z, bsize - o), z, 1);
        if (r) goto undo_bmp;
    }
    /* write the new attribute extents */
    ai.size = ai.init = new_bytes;
    ai.alloc = MAX(need_vcns, old_vcns) << fs->cshift;
    if ((r = nt_attr_store_runs(&ino, NTFS_AT_DATA, NULL, 0, &runs, &ai))) goto undo_bmp;
    bi.size = bi.init = MAX(bsize, bi.size);
    bi.alloc = bneed > bvcns ? bneed << fs->cshift : bi.alloc;
    if ((r = nt_attr_store_runs(&ino, NTFS_AT_BITMAP, NULL, 0, &bruns, &bi))) {
        /* put the old $DATA extent back */
        ainfo_t old = ai;
        old.size = old.init = fs->mft_size;
        old.alloc = fs->mft_alloc;
        nt_attr_store_runs(&ino, NTFS_AT_DATA, NULL, 0, &fs->mft_runs, &old);
        goto undo_bmp;
    }
    /* switch to the new layout and format the new records */
    nrl_free(&fs->mft_runs);
    fs->mft_runs = runs;
    runs.r = NULL;
    nrl_free(&fs->mftbmp_runs);
    fs->mftbmp_runs = bruns;
    bruns.r = NULL;
    u64 old_n = fs->mft_nrec;
    fs->mft_size = fs->mft_init = new_bytes;
    fs->mft_alloc = ai.alloc;
    fs->mft_nrec = new_n;
    fs->mftbmp_size = bi.size;
    fs->mftbmp_alloc = bi.alloc;
    for (u64 rec = old_n; rec < new_n && !r; rec++) {
        fblk_t *b;
        if ((r = nt_rec_get_fresh(fs, rec, &b))) break;
        nt_rec_init(fs, b, rec, 1, 0);
        nt_fb_put(fs, b);
        if ((rec & 15) == 15) r = nt_flush(fs);
    }
    if (r) nt_corrupt(fs, "could not format new MFT records", old_n);
    kprintf("ntfs: %s: MFT grown to %lu records\n", fs->bd->name, (unsigned long)new_n);
    goto out;
undo_bmp:
    if (bneed > bvcns) nt_clus_free_runs(fs, &bruns, bvcns);
undo_data:
    if (need_vcns > old_vcns) nt_clus_free_runs(fs, &runs, old_vcns);
out:
    nrl_free(&runs);
    nrl_free(&bruns);
    nt_ino_close(&ino);
    return r;
}

int nt_mft_alloc(ntfs_t *fs, u64 *rec) {
    if (fs->mftbmp_resident) return E_NOTSUP;
    for (int round = 0; round < 2; round++) {
        u64 limit = MIN(fs->mft_nrec, fs->mftbmp_size * 8);
        u64 from = MAX((u64)NT_FIRST_FREE, fs->mft_hint);
        for (int pass = 0; pass < 2; pass++) {
            u64 s, len;
            int r = from < limit ? scan_free(fs, &fs->mftbmp_runs, from, limit, 1, &s, &len) : E_NOSPC;
            if (!r) {
                u64 ch;
                if ((r = bits_put(fs, &fs->mftbmp_runs, s, 1, 1, &ch))) return r;
                fs->mft_hint = s + 1;
                *rec = s;
                return 0;
            }
            if (r != E_NOSPC) return r;
            limit = MIN(limit, from);
            from = NT_FIRST_FREE;
        }
        if (round) break;
        int r = mft_grow(fs);
        if (r) return r == E_NOTSUP ? E_NOSPC : r;
    }
    return E_NOSPC;
}
