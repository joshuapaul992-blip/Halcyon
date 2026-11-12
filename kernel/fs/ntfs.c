/* Halcyon OS - NTFS file system driver: mount, the safety rules, the VFS
 * operations (milestone 7.5; design and rules in docs/DESIGN-M7.5.md).
 *
 * Files: ntfs.c (this), ntfs_mft.c (I/O, caches, records, attributes, run
 * lists), ntfs_index.c (B+ tree indexes), ntfs_alloc.c (clusters, MFT
 * records), ntfs_lznt1.c (decompression); on-disk structures in
 * ntfs_layout.h.
 *
 * Writing is allowed only on a volume Windows shut down cleanly (no dirty
 * flag, not hibernated, $LogFile clean) that is not an internal Windows
 * disk without the user's permission. The first change of a session sets
 * the dirty flag and empties $LogFile; sync and unmount clear the flag
 * again. Every operation leaves the volume consistent when it returns
 * (dirty metadata is flushed at its end). */
#include "core/types.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/unicode.h"
#include "core/settings.h"
#include "fs/block.h"
#include "fs/vfs.h"
#include "fs/ntfs.h"
#include "fs/ntfs_priv.h"

#define NTFS(v) ((ntfs_t *)(v)->fs_priv)
#define ZBUF 65536u
#define LOG_FILL 65536u
#define DOS_MAP_MAX 8192            /* short names collected per listing */

static const fs_ops_t ntfs_ops;

static const char *const ro_text[] = {
    "",
    "The disk is write-protected.",
    "The volume needs a check in Windows (chkdsk).",
    "Windows did not shut down completely (Fast Startup or hibernation). Shut Windows down fully to write here.",
    "This is an internal Windows drive: writing is off until you allow it in the drive's properties.",
    "Halcyon cannot write to this NTFS version or layout.",
};

/* the first (most important) reason stays */
static void set_ro(ntfs_t *fs, int why) {
    if (!fs->ro) fs->ro = (u8)why;
}

/* ------------------------------------------------------------------ */
/* time                                                                 */

#define NT_EPOCH_DIFF 11644473600ll     /* seconds from 1601-01-01 to 1970-01-01 */

static i64 days_from_civil(i64 y, int m, int d) {
    y -= m <= 2;
    i64 era = (y >= 0 ? y : y - 399) / 400;
    i64 yoe = y - era * 400;
    i64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(i64 z, int *y, int *m, int *d) {
    z += 719468;
    i64 era = (z >= 0 ? z : z - 146096) / 146097;
    i64 doe = z - era * 146097;
    i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    i64 yy = yoe + era * 400;
    i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    i64 mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

/* minutes east of UTC (Control Panel, Time Zone); the clock is local time */
static int utc_offset(void) {
    int m = settings_get_int("clock.utc_offset", 0);
    return m < -16 * 60 || m > 16 * 60 ? 0 : m;
}

i64 nt_now(void) {
    struct rtc_time t;
    rtc_read(&t);
    i64 s = days_from_civil(t.year, t.month ? t.month : 1, t.day ? t.day : 1) * 86400 +
            t.hour * 3600 + t.minute * 60 + t.second;
    s -= (i64)utc_offset() * 60;
    return (s + NT_EPOCH_DIFF) * 10000000ll;
}

static void nt_to_vtime(i64 t, vfs_time_t *o) {
    memset(o, 0, sizeof *o);
    if (t <= 0) { o->year = 1601; o->month = o->day = 1; return; }
    i64 s = t / 10000000ll - NT_EPOCH_DIFF + (i64)utc_offset() * 60;
    i64 days = s >= 0 ? s / 86400 : -((-s + 86399) / 86400);
    i64 rem = s - days * 86400;
    int y, m, d;
    civil_from_days(days, &y, &m, &d);
    if (y < 1601 || y > 9999) { o->year = 1601; o->month = o->day = 1; return; }
    o->year = (u16)y;
    o->month = (u8)m;
    o->day = (u8)d;
    o->hour = (u8)(rem / 3600);
    o->minute = (u8)(rem / 60 % 60);
    o->second = (u8)(rem % 60);
}

/* ------------------------------------------------------------------ */
/* names                                                                */

static u16 name_hash(const u16 *u, int n) {
    u16 h = 0;
    for (int i = 0; i < n; i++) h = (u16)(((h & 1) ? 0x8000 : 0) + (h >> 1) + u[i]);
    return h;
}

/* the name handed to the VFS for the on-disk name u[0..n): UTF-8, one
 * usable path component, at most VFS_MAX_NAME bytes */
static void display_name(const u16 *u, int n, char *out) {
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
            int total = o;
            for (int k = i; k < n; k++) total += u[k] < 0x80 ? 1 : u[k] < 0x800 ? 2 : 3;
            if (total > VFS_MAX_NAME) { cut = 1; break; }
        }
        memcpy(out + o, tmp, l);
        o += l;
    }
    if (cut) o += snprintf(out + o, VFS_MAX_NAME + 1 - o, "~%04X", name_hash(u, n));
    if (!o || out[o - 1] == '.' || out[o - 1] == ' ') {
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

/* a name for a new entry: valid UTF-8, no characters Windows forbids, at
 * most 255 UTF-16 units */
static int name_units(const char *n, u16 *u, int *nu) {
    size_t len = strlen(n);
    if (!len) return E_INVAL;
    if (len > VFS_MAX_NAME) return E_NAMETOOLONG;
    for (size_t i = 0; i < len; i++) {
        u8 c = (u8)n[i];
        if (c < 0x20 || (c < 0x80 && strchr("\"*/:<>?\\|", c))) return E_INVAL;
    }
    if (!utf8_valid(n)) return E_INVAL;
    int k = utf8_to_utf16(n, u, NT_MAX_NAME);
    if (k < 0) return E_NAMETOOLONG;
    if (!k) return E_INVAL;
    *nu = k;
    return 0;
}

/* the namespace of a new name: Win32 and DOS if it is a valid upper-case
 * 8.3 name (as Windows does), else Win32 only (no short name generated) */
static u8 name_space(const u16 *u, int n) {
    int dot = -1, i;
    for (i = 0; i < n; i++) {
        u16 c = u[i];
        if (c == '.') { if (dot >= 0) return NTFS_NS_WIN32; dot = i; continue; }
        if (c >= 'A' && c <= 'Z') continue;
        if (c >= '0' && c <= '9') continue;
        if (c < 0x80 && strchr("!#$%&'()-@^_`{}~", c)) continue;
        return NTFS_NS_WIN32;
    }
    int base = dot < 0 ? n : dot, ext = dot < 0 ? 0 : n - dot - 1;
    if (base < 1 || base > 8 || ext > 3 || (dot >= 0 && ext == 0)) return NTFS_NS_WIN32;
    return NTFS_NS_WIN32_AND_DOS;
}

/* ------------------------------------------------------------------ */
/* standard information and file names                                  */

typedef struct {
    i64 ct, mt, rt, at;
    u32 attrs, secid;
    u32 len;
} nsi_t;

static int si_get(nino_t *ino, nsi_t *si) {
    actx_t c;
    memset(si, 0, sizeof *si);
    int r = nt_attr_find(ino, &c, NTFS_AT_STANDARD_INFORMATION, NULL, 0);
    if (r) return r == E_NOENT ? E_IO : r;
    ntfs_attr_t *a = ac_attr(&c);
    u32 len = a->non_resident ? 0 : a->r.value_length;
    if (len >= 48) {
        ntfs_std_info_t *s = (ntfs_std_info_t *)attr_value(a);
        si->ct = s->creation_time;
        si->mt = s->data_change_time;
        si->rt = s->mft_change_time;
        si->at = s->access_time;
        si->attrs = s->file_attributes;
        if (len >= 72) si->secid = s->security_id;
        si->len = len;
    } else {
        r = E_IO;
    }
    nt_actx_done(&c);
    return r;
}

#define T_C 1
#define T_M 2
#define T_R 4
#define T_A 8

/* update times (what: T_*) and attribute bits */
static int si_touch(nino_t *ino, int what, u32 set, u32 clear) {
    actx_t c;
    int r = nt_attr_find(ino, &c, NTFS_AT_STANDARD_INFORMATION, NULL, 0);
    if (r) return r == E_NOENT ? E_IO : r;
    ntfs_attr_t *a = ac_attr(&c);
    if (a->non_resident || a->r.value_length < 48) { nt_actx_done(&c); return E_IO; }
    ntfs_std_info_t *s = (ntfs_std_info_t *)attr_value(a);
    i64 now = nt_now();
    if (what & T_C) s->creation_time = now;
    if (what & T_M) s->data_change_time = now;
    if (what & T_R) s->mft_change_time = now;
    if (what & T_A) s->access_time = now;
    s->file_attributes = (s->file_attributes & ~clear) | set;
    c.rec->dirty = 1;
    nt_actx_done(&c);
    return 0;
}

/* sizes for the duplicated information of a file */
static void dup_sizes(nino_t *ino, int is_dir, u64 *alloc, u64 *size) {
    *alloc = *size = 0;
    if (is_dir) return;
    ainfo_t ai;
    if (nt_attr_info(ino, NTFS_AT_DATA, NULL, 0, &ai, NULL)) return;
    *size = ai.size;
    *alloc = ai.resident ? ALIGN_UP(ai.size, 8ull)
             : (ai.flags & (NTFS_ATTR_COMPRESSED | NTFS_ATTR_SPARSE)) ? ai.comp : ai.alloc;
}

static int ino_is_dir(nino_t *ino) { return (fb_rec(ino->base)->flags & NTFS_MFT_DIRECTORY) != 0; }

static u32 reparse_tag(nino_t *ino) {
    u32 tag = 0;
    if (nt_attr_read(ino, NTFS_AT_REPARSE_POINT, NULL, 0, 0, 4, &tag) != 4) return 0;
    return tag;
}

/* build a $FILE_NAME value; returns its length */
static u32 fn_build(u8 *out, u64 parent, const nsi_t *si, u64 alloc, u64 size, u32 attrs,
                    u32 reparse, const u16 *name, int nlen, u8 ns) {
    ntfs_file_name_t *fn = (ntfs_file_name_t *)out;
    u32 len = (u32)sizeof *fn + 2u * (u32)nlen;
    memset(out, 0, len);
    fn->parent = parent;
    fn->creation_time = si->ct;
    fn->data_change_time = si->mt;
    fn->mft_change_time = si->rt;
    fn->access_time = si->at;
    fn->allocated_size = (i64)alloc;
    fn->data_size = (i64)size;
    fn->file_attributes = attrs;
    if (attrs & NTFS_FA_REPARSE_POINT) fn->reparse_tag = reparse;
    fn->name_length = (u8)nlen;
    fn->name_type = ns;
    memcpy(fn->name, name, 2u * (u32)nlen);
    return len;
}

static u32 ie_build(u8 *out, u64 file, const u8 *fn, u32 fnlen) {
    ntfs_index_entry_t *e = (ntfs_index_entry_t *)out;
    u32 len = ALIGN_UP((u32)sizeof *e + fnlen, 8u);
    memset(out, 0, len);
    e->file = file;
    e->length = (u16)len;
    e->key_length = (u16)fnlen;
    memcpy(e->key, fn, fnlen);
    return len;
}

/* bring the duplicated information (times, sizes, attributes) of every name
 * of a file up to date: in its $FILE_NAME attributes and in the directory
 * index entries */
static int names_update(ntfs_t *fs, nino_t *ino) {
    nsi_t si;
    int r = si_get(ino, &si);
    if (r) return r;
    int dir = ino_is_dir(ino);
    u64 alloc, size;
    dup_sizes(ino, dir, &alloc, &size);
    u32 tag = (si.attrs & NTFS_FA_REPARSE_POINT) ? reparse_tag(ino) : 0;
    u8 *ie = kmalloc(NT_IE_MAX + 16);
    if (!ie) return E_NOMEM;
    actx_t c;
    int err = 0;
    r = nt_attr_first(ino, &c, NTFS_AT_FILE_NAME, NULL, 0, 0);
    while (!r) {
        ntfs_attr_t *a = ac_attr(&c);
        if (!a->non_resident && a->r.value_length >= sizeof(ntfs_file_name_t)) {
            ntfs_file_name_t *fn = (ntfs_file_name_t *)attr_value(a);
            fn->creation_time = si.ct;
            fn->data_change_time = si.mt;
            fn->mft_change_time = si.rt;
            fn->access_time = si.at;
            fn->allocated_size = (i64)alloc;
            fn->data_size = (i64)size;
            fn->file_attributes = (si.attrs & ~(u32)NTFS_FA_DIRECTORY) | (dir ? NTFS_FA_DUP_FILE_NAME_INDEX : 0);
            if (fn->file_attributes & NTFS_FA_REPARSE_POINT) fn->reparse_tag = tag;
            c.rec->dirty = 1;
            ie_build(ie, NTFS_MK_MREF(ino->rec, ino->seq), (u8 *)fn, a->r.value_length);
            nino_t pi;
            if (NTFS_MREF(fn->parent) != ino->rec && !nt_ino_open(fs, fn->parent, 1, &pi)) {
                nidx_t ix;
                int k = nt_idx_open(&pi, nt_I30, 4, nt_coll_filename, &ix);
                if (!k) { k = nt_idx_update(&ix, (ntfs_index_entry_t *)ie); nt_idx_close(&ix); }
                if (k && k != E_NOENT && !err) err = k;
                nt_ino_close(&pi);
            }
        }
        r = nt_attr_next(&c);
    }
    kfree(ie);
    if (r != E_NOENT) return r;
    return err;
}

/* ------------------------------------------------------------------ */
/* lookups                                                              */

static int key_from_name(const char *name, u8 *key, u32 *klen) {
    ntfs_file_name_t *fn = (ntfs_file_name_t *)key;
    memset(key, 0, sizeof *fn);
    int nu = utf8_to_utf16(name, fn->name, NT_MAX_NAME);
    if (nu <= 0) return E_NOENT;
    fn->name_length = (u8)nu;
    *klen = (u32)sizeof *fn + 2u * (u32)nu;
    return 0;
}

static void ent_fill(nent_t *e, const ntfs_index_entry_t *ie, u64 dir) {
    e->mref = ie->file;
    e->dir = dir;
    e->fn_len = MIN((u32)ie->key_length, (u32)sizeof e->fn);
    memcpy(e->fn, ie->key, e->fn_len);
    ntfs_file_name_t *fn = ent_fn(e);
    display_name(fn->name, fn->name_length, e->name);
}

typedef struct {
    ntfs_t *fs;
    const char *want;
    u64 dir;
    nent_t *out;
    int found;
} scan_t;

static int scan_cb(void *vctx, const ntfs_index_entry_t *ie) {
    scan_t *s = vctx;
    const ntfs_file_name_t *fn = (const ntfs_file_name_t *)ie->key;
    char dn[VFS_MAX_NAME + 1];
    display_name(fn->name, fn->name_length, dn);
    if (utf8_casecmp(dn, s->want)) return 0;
    ent_fill(s->out, ie, s->dir);
    s->found = 1;
    return 1;
}

static int hidden_entry(u64 dir, u64 file) {
    return NTFS_MREF(dir) == NTFS_FILE_ROOT && (NTFS_MREF(file) < NTFS_FILE_FIRST_USER);
}

/* look a name up in directory dir (an open inode); fills e */
static int dir_lookup(ntfs_t *fs, nino_t *dir, const char *name, nent_t *e) {
    u8 *key = kmalloc(NT_FN_MAX + NT_IE_MAX);
    if (!key) return E_NOMEM;
    u8 *ent = key + NT_FN_MAX;
    u32 klen;
    nidx_t ix;
    u64 dmref = NTFS_MK_MREF(dir->rec, dir->seq);
    int r = nt_idx_open(dir, nt_I30, 4, nt_coll_filename, &ix);
    if (r) { kfree(key); return r == E_NOENT ? E_NOTDIR : r; }
    r = key_from_name(name, key, &klen);
    if (!r) r = nt_idx_lookup(&ix, key, klen, 1, ent, NT_IE_MAX);
    if (!r) {
        ent_fill(e, (ntfs_index_entry_t *)ent, dmref);
        if (hidden_entry(dmref, e->mref)) r = E_NOENT;
    }
    if (r == E_NOENT) {
        /* a display name that differs from the stored name ('_' for a
         * character the VFS cannot show, U+FFFD, a hash suffix) */
        int maybe = 0;
        for (const char *p = name; *p; p++) if (*p == '_' || (u8)*p == 0xEF || *p == '~') maybe = 1;
        if (maybe) {
            scan_t s = { fs, name, dmref, e, 0 };
            int k = nt_idx_walk(&ix, scan_cb, &s);
            if (k < 0) r = k;
            else if (s.found && !hidden_entry(dmref, e->mref)) r = 0;
        }
    }
    nt_idx_close(&ix);
    kfree(key);
    return r;
}

/* the directory record of a path's parent and the last component's name.
 * path "" = root. *isroot: the path is the root itself. With ent != NULL the
 * last component is looked up too. */
static int resolve(ntfs_t *fs, const char *path, nent_t *ent, int *isroot, u64 *parent, char *last) {
    char *comp = kmalloc(VFS_MAX_NAME + 1);
    if (!comp) return E_NOMEM;
    u64 cur = NTFS_MK_MREF(NTFS_FILE_ROOT, 0);
    int r = 0;
    *isroot = 0;
    if (parent) *parent = 0;            /* set only when the last component is reached */
    const char *s = path;
    while (*s == '/') s++;
    if (!*s) {
        *isroot = 1;
        if (parent) *parent = cur;
        kfree(comp);
        return 0;
    }
    for (int depth = 0;; depth++) {
        const char *e = s;
        while (*e && *e != '/') e++;
        if ((r = get_comp(s, (size_t)(e - s), comp))) break;
        const char *n = e;
        while (*n == '/') n++;
        int is_last = !*n;
        if (is_last) {
            if (parent) *parent = cur;
            if (last) strlcpy(last, comp, VFS_MAX_NAME + 1);
            if (!ent) break;
        }
        nino_t d;
        if ((r = nt_ino_open(fs, cur, 1, &d))) { if (r == E_NOENT) r = E_IO; break; }
        if (!ino_is_dir(&d)) { nt_ino_close(&d); r = E_NOTDIR; break; }
        r = dir_lookup(fs, &d, comp, ent);
        nt_ino_close(&d);
        if (r) break;
        if (is_last) break;
        ntfs_file_name_t *fn = ent_fn(ent);
        if (!(fn->file_attributes & NTFS_FA_DUP_FILE_NAME_INDEX)) { r = E_NOTDIR; break; }
        if (fn->file_attributes & NTFS_FA_REPARSE_POINT) {
            kprintf("ntfs: %s: '%s' is a link (reparse point): not followed\n", fs->bd->name, ent->name);
            r = E_NOTSUP;
            break;
        }
        if (depth > 256) { r = E_INVAL; break; }
        cur = ent->mref;
        s = n;
    }
    kfree(comp);
    return r;
}

/* ------------------------------------------------------------------ */
/* the first write of a session                                         */

static int vol_flags_store(ntfs_t *fs, u16 flags) {
    nino_t ino;
    int r = nt_ino_open(fs, NTFS_FILE_VOLUME, 0, &ino);
    if (r) return r;
    actx_t c;
    if (!(r = nt_attr_find(&ino, &c, NTFS_AT_VOLUME_INFORMATION, NULL, 0))) {
        ntfs_attr_t *a = ac_attr(&c);
        if (a->non_resident || a->r.value_length < sizeof(ntfs_volume_info_t)) r = E_IO;
        else {
            ((ntfs_volume_info_t *)attr_value(a))->flags = flags;
            c.rec->dirty = 1;
        }
        nt_actx_done(&c);
    }
    nt_ino_close(&ino);
    if (!r) r = nt_flush(fs);
    return r;
}

/* empty $LogFile: fill it with 0xFF, as Windows' own chkdsk-less tools and
 * other non-Windows drivers do; Windows then starts a new log */
static int log_reset(ntfs_t *fs) {
    nino_t ino;
    int r = nt_ino_open(fs, NTFS_FILE_LOGFILE, 0, &ino);
    if (r) return r;
    ainfo_t ai;
    nrl_t rl = { 0 };
    r = nt_attr_info(&ino, NTFS_AT_DATA, NULL, 0, &ai, &rl);
    nt_ino_close(&ino);
    if (!r && (ai.resident || nrl_vcns(&rl) << fs->cshift < ai.size)) r = E_IO;
    u8 *buf = r ? NULL : kmalloc(LOG_FILL);
    if (!r && !buf) r = E_NOMEM;
    if (!r) {
        memset(buf, 0xFF, LOG_FILL);
        for (u64 o = 0; o < ai.size && !r; o += LOG_FILL)
            r = nt_runs_rw(fs, &rl, o, (u32)MIN((u64)LOG_FILL, ai.size - o), buf, 1);
    }
    kfree(buf);
    nrl_free(&rl);
    return r;
}

int nt_writable(ntfs_t *fs) {
    if (fs->ro || fs->corrupt) return E_ROFS;
    int r;
    if (!fs->dirty_set) {
        if ((r = vol_flags_store(fs, (u16)(fs->vol_flags | NTFS_VOL_DIRTY)))) {
            nt_corrupt(fs, "cannot set the dirty flag", NTFS_FILE_VOLUME);
            return r;
        }
        fs->dirty_set = 1;
    }
    if (!fs->log_reset) {
        if ((r = log_reset(fs))) {
            nt_corrupt(fs, "cannot reset $LogFile", NTFS_FILE_LOGFILE);
            return r;
        }
        fs->log_reset = 1;
        kprintf("ntfs: %s: first change: volume marked dirty, $LogFile emptied\n", fs->bd->name);
    }
    return 0;
}

/* flush, and clear the dirty flag that we set */
static int flush_clean(ntfs_t *fs) {
    int r = nt_flush(fs);
    if (r || !fs->dirty_set || fs->corrupt) return r;
    r = vol_flags_store(fs, fs->vol_flags);
    if (!r) fs->dirty_set = 0;
    return r;
}

/* end of a mutating operation */
static int op_end(ntfs_t *fs, int r) {
    int f = nt_flush(fs);
    return r ? r : f;
}

/* ------------------------------------------------------------------ */
/* open files                                                           */

static nnode_t *node_find(ntfs_t *fs, u64 rec) {
    for (nnode_t *n = fs->nodes; n; n = n->next) if (n->rec == rec) return n;
    return NULL;
}

static void node_free(nnode_t *n) {
    nrl_free(&n->runs);
    kfree(n->cu_buf);
    kfree(n);
}

static void node_unref(ntfs_t *fs, nnode_t *n) {
    if (--n->refs) return;
    for (nnode_t **pp = &fs->nodes; *pp; pp = &(*pp)->next)
        if (*pp == n) { *pp = n->next; break; }
    node_free(n);
}

static int node_refresh(ntfs_t *fs, nnode_t *n, nino_t *ino) {
    nrl_free(&n->runs);
    n->runs_ok = 0;
    int r = nt_attr_info(ino, NTFS_AT_DATA, NULL, 0, &n->ai, &n->runs);
    if (r == E_NOENT) {
        memset(&n->ai, 0, sizeof n->ai);
        n->ai.resident = 1;
        n->nowrite = 1;
        return 0;
    }
    if (r) return r;
    n->runs_ok = 1;
    n->cu_vcn = -1;
    return 0;
}

/* why a file cannot be opened (0: it can), and whether it can be written */
static int file_check(ntfs_t *fs, nino_t *ino, nnode_t *n, const char *name) {
    nsi_t si;
    int r = si_get(ino, &si);
    if (r) return r;
    if (si.attrs & NTFS_FA_REPARSE_POINT) {
        u32 tag = reparse_tag(ino);
        if (tag == NTFS_REPARSE_TAG_SYMLINK || tag == NTFS_REPARSE_TAG_MOUNT_POINT) {
            kprintf("ntfs: %s: '%s' is a symbolic link or junction: not followed\n", fs->bd->name, name);
            return E_NOTSUP;
        }
        if (tag == NTFS_REPARSE_TAG_WOF || tag == NTFS_REPARSE_TAG_DEDUP) {
            kprintf("ntfs: %s: '%s' is compressed by Windows (%s): not supported\n", fs->bd->name, name,
                    tag == NTFS_REPARSE_TAG_WOF ? "CompactOS / WOF" : "deduplication");
            return E_NOTSUP;
        }
        if (si.attrs & (NTFS_FA_OFFLINE | 0x00040000u | 0x00400000u)) {
            kprintf("ntfs: %s: '%s' is a cloud placeholder (not on this disk)\n", fs->bd->name, name);
            return E_NOTSUP;
        }
        n->nowrite = 1;
    }
    if ((si.attrs & NTFS_FA_ENCRYPTED) || (n->ai.flags & NTFS_ATTR_ENCRYPTED)) {
        kprintf("ntfs: %s: '%s' is encrypted (EFS): cannot be opened\n", fs->bd->name, name);
        return E_PERM;
    }
    if (!n->ai.resident && (n->ai.flags & NTFS_ATTR_COMPRESSED)) {
        if ((n->ai.flags & 0xFF) != NTFS_ATTR_COMPRESSED || n->ai.cu != 4 || fs->csize > 4096) {
            kprintf("ntfs: %s: '%s': unsupported compression\n", fs->bd->name, name);
            return E_NOTSUP;
        }
        n->nowrite = 1;
    }
    if (!n->ai.resident && (n->ai.flags & NTFS_ATTR_SPARSE)) n->nowrite = 1;
    if (!n->ai.resident && nrl_vcns(&n->runs) << fs->cshift != n->ai.alloc &&
        !(n->ai.flags & (NTFS_ATTR_COMPRESSED | NTFS_ATTR_SPARSE)))
        n->nowrite = 1;
    if (ino->rec < NT_FIRST_FREE) n->nowrite = 1;
    return 0;
}

static int node_get(ntfs_t *fs, const nent_t *e, nnode_t **out) {
    u64 rec = NTFS_MREF(e->mref);
    nnode_t *n = node_find(fs, rec);
    if (n) { n->refs++; *out = n; return 0; }
    n = kzalloc(sizeof *n);
    if (!n) return E_NOMEM;
    n->refs = 1;
    n->rec = rec;
    n->cu_vcn = -1;
    nino_t ino;
    int r = nt_ino_open(fs, e->mref, 1, &ino);
    if (r) { kfree(n); return r == E_NOENT ? E_IO : r; }
    n->seq = ino.seq;
    n->is_dir = (u8)ino_is_dir(&ino);
    if (n->is_dir) r = E_ISDIR;
    if (!r) r = node_refresh(fs, n, &ino);
    if (!r) r = file_check(fs, &ino, n, e->name);
    nt_ino_close(&ino);
    if (r) { node_free(n); return r; }
    n->next = fs->nodes;
    fs->nodes = n;
    *out = n;
    return 0;
}

/* ------------------------------------------------------------------ */
/* file data                                                            */

static int read_compressed(ntfs_t *fs, nnode_t *n, u64 pos, u8 *buf, u32 len) {
    u32 ucl = 1u << n->ai.cu, ubytes = ucl << fs->cshift;
    if (!n->cu_buf) {
        n->cu_buf = kmalloc((size_t)ubytes * 2);
        if (!n->cu_buf) return E_NOMEM;
        n->cu_vcn = -1;
    }
    u8 *raw = n->cu_buf + ubytes;
    while (len) {
        i64 v0 = (i64)(pos / ubytes) * ucl;
        if (n->cu_vcn != v0) {
            n->cu_vcn = -1;
            u32 real = 0;
            int contiguous_real = 1;
            for (u32 i = 0; i < ucl; i++) {
                i64 l = nrl_lcn(&n->runs, v0 + i, NULL);
                if (l >= 0) { if (real != i) contiguous_real = 0; real++; }
            }
            if (!real) {
                memset(n->cu_buf, 0, ubytes);
            } else if (real == ucl) {
                int r = nt_runs_rw(fs, &n->runs, (u64)v0 << fs->cshift, ubytes, n->cu_buf, 0);
                if (r) return r;
            } else {
                if (!contiguous_real) return E_IO;
                int r = nt_runs_rw(fs, &n->runs, (u64)v0 << fs->cshift, real << fs->cshift, raw, 0);
                if (r) return r;
                int got = nt_lznt1_decompress(raw, real << fs->cshift, n->cu_buf, ubytes);
                if (got < 0) {
                    kprintf("ntfs: %s: record %lu: corrupt compressed data\n", fs->bd->name, (unsigned long)n->rec);
                    return E_IO;
                }
                memset(n->cu_buf + got, 0, ubytes - (u32)got);
            }
            n->cu_vcn = v0;
        }
        u32 in = (u32)(pos % ubytes), k = MIN(len, ubytes - in);
        memcpy(buf, n->cu_buf + in, k);
        pos += k; buf += k; len -= k;
    }
    return 0;
}

static int node_read(ntfs_t *fs, nnode_t *n, u64 pos, u8 *buf, u32 len) {
    if (pos >= n->ai.size) return 0;
    if (len > n->ai.size - pos) len = (u32)(n->ai.size - pos);
    if (len > 0x7FFFFFFF) len = 0x7FFFFFFF;
    if (!len) return 0;
    if (n->ai.resident) {
        nino_t ino;
        int r = nt_ino_open(fs, n->rec, 0, &ino);
        if (r) return r;
        r = nt_attr_read(&ino, NTFS_AT_DATA, NULL, 0, pos, len, buf);
        nt_ino_close(&ino);
        if (r >= 0 && (u32)r < len) memset(buf + r, 0, len - (u32)r);
        return r < 0 ? r : (int)len;
    }
    u32 disk = pos < n->ai.init ? (u32)MIN((u64)len, n->ai.init - pos) : 0;
    int r = 0;
    if (disk) {
        if (n->ai.flags & NTFS_ATTR_COMPRESSED) r = read_compressed(fs, n, pos, buf, disk);
        else r = nt_runs_rw(fs, &n->runs, pos, disk, buf, 0);
    }
    if (r) return r;
    if (disk < len) memset(buf + disk, 0, len - disk);
    return (int)len;
}

/* zeros on disk for [from, to) of a non-resident attribute */
static int zero_range(ntfs_t *fs, const nrl_t *rl, u64 from, u64 to) {
    if (from >= to) return 0;
    u8 *z = kzalloc(ZBUF);
    if (!z) return E_NOMEM;
    int r = 0;
    for (u64 o = from; o < to && !r; o += ZBUF)
        r = nt_runs_rw(fs, rl, o, (u32)MIN((u64)ZBUF, to - o), z, 1);
    kfree(z);
    return r;
}

/* make the clusters cover [0, end): append clusters; returns the bytes
 * now allocated (less than end when the volume is full) */
static int grow_alloc(ntfs_t *fs, nnode_t *n, u64 end, u64 *cap) {
    u64 have = nrl_vcns(&n->runs), need = (end + fs->csize - 1) >> fs->cshift;
    *cap = have << fs->cshift;
    if (need <= have) { *cap = end; return 0; }
    i64 hint = n->runs.n && n->runs.r[n->runs.n - 1].lcn >= 0
               ? n->runs.r[n->runs.n - 1].lcn + (i64)n->runs.r[n->runs.n - 1].len : (i64)fs->alloc_hint;
    int r = nt_clus_alloc(fs, need - have, (u64)hint, 0, &n->runs, (i64)have);
    if (r == E_NOSPC) {
        if (fs->free_clus == ~0ull && (r = nt_bmp_count_free(fs))) return r;
        u64 avail = MIN(fs->free_clus, need - have);
        if (!avail) return E_NOSPC;
        r = nt_clus_alloc(fs, avail, (u64)hint, 0, &n->runs, (i64)have);
        if (r) return r;
        *cap = (have + avail) << fs->cshift;
        return 0;
    }
    if (r) return r;
    *cap = end;
    return 0;
}

/* store the data attribute after an allocation change; on failure the new
 * clusters are given back */
static int data_store(ntfs_t *fs, nnode_t *n, nino_t *ino, u64 old_vcns, const ainfo_t *na) {
    int r = nt_attr_store_runs(ino, NTFS_AT_DATA, NULL, 0, &n->runs, na);
    if (r) {
        if (nrl_vcns(&n->runs) > old_vcns) {
            nt_clus_free_runs(fs, &n->runs, old_vcns);
            nrl_truncate(&n->runs, old_vcns);
        }
        if (r == E_NOSPC || r == E_NOTSUP)
            kprintf("ntfs: %s: record %lu: the run list does not fit in the MFT record\n",
                    fs->bd->name, (unsigned long)n->rec);
        return r == E_NOTSUP ? E_NOSPC : r;
    }
    n->ai = *na;
    return 0;
}

/* resident data: replace the value with the first newlen bytes of
 * (old value, zero padded, with data written at pos) */
static int resident_put(nino_t *ino, nnode_t *n, u64 pos, const u8 *data, u32 len, u64 newlen) {
    if (newlen > ino->fs->rec_size) return E_NOSPC;
    u8 *v = kzalloc((size_t)newlen + 1);
    if (!v) return E_NOMEM;
    int r = nt_attr_read(ino, NTFS_AT_DATA, NULL, 0, 0, (u32)MIN(n->ai.size, newlen), v);
    if (r >= 0) {
        if (data) memcpy(v + pos, data, len);
        r = nt_attr_set_resident(ino, NTFS_AT_DATA, NULL, 0, v, (u32)newlen);
    }
    kfree(v);
    if (!r) n->ai.size = n->ai.init = newlen, n->ai.alloc = ALIGN_UP(newlen, 8ull);
    return r;
}

static int node_write(ntfs_t *fs, nnode_t *n, u64 pos, const u8 *buf, u32 len) {
    nino_t ino;
    int r = nt_ino_open(fs, n->rec, 0, &ino);
    if (r) return r;
    u64 end = pos + len, old_size = n->ai.size, old_alloc = n->ai.alloc;
    if (n->ai.resident) {
        r = resident_put(&ino, n, pos, buf, len, MAX(end, n->ai.size));
        if (r == E_NOSPC) {
            r = nt_attr_make_nonres(&ino, NTFS_AT_DATA, NULL, 0, end);
            if (!r) r = node_refresh(fs, n, &ino);
        } else if (!r) {
            goto done;
        }
        if (r) goto out;
    }
    {
        u64 old_vcns = nrl_vcns(&n->runs), cap;
        if ((r = grow_alloc(fs, n, end, &cap))) goto out;
        if (cap < end) {
            if (cap <= pos) { r = E_NOSPC; goto undo; }
            end = cap;
        }
        if (pos > n->ai.init && (r = zero_range(fs, &n->runs, n->ai.init, pos))) goto undo;
        if ((r = nt_runs_rw(fs, &n->runs, pos, (u32)(end - pos), (void *)buf, 1))) goto undo;
        ainfo_t na = n->ai;
        na.size = MAX(na.size, end);
        na.init = MAX(na.init, end);
        na.alloc = nrl_vcns(&n->runs) << fs->cshift;
        if (nrl_vcns(&n->runs) != old_vcns) r = data_store(fs, n, &ino, old_vcns, &na);
        else if (!(r = nt_attr_set_sizes(&ino, NTFS_AT_DATA, NULL, 0, &na))) n->ai = na;
        if (r) goto out;
        goto done;
    undo:
        if (nrl_vcns(&n->runs) > old_vcns) {
            nt_clus_free_runs(fs, &n->runs, old_vcns);
            nrl_truncate(&n->runs, old_vcns);
        }
        goto out;
    }
done:
    si_touch(&ino, T_M | T_R | T_A, NTFS_FA_ARCHIVE, 0);
    if (n->ai.size != old_size || n->ai.alloc != old_alloc) r = names_update(fs, &ino);
    if (!r) r = (int)(end - pos);
out:
    nt_ino_close(&ino);
    return r;
}

static int node_truncate(ntfs_t *fs, nnode_t *n, u64 size) {
    nino_t ino;
    int r = nt_ino_open(fs, n->rec, 0, &ino);
    if (r) return r;
    if (n->ai.resident) {
        r = resident_put(&ino, n, 0, NULL, 0, size);
        if (r == E_NOSPC) {
            r = nt_attr_make_nonres(&ino, NTFS_AT_DATA, NULL, 0, size);
            if (!r) r = node_refresh(fs, n, &ino);
            if (r) goto out;
        } else {
            goto done;
        }
    }
    {
        u64 have = nrl_vcns(&n->runs), need = (size + fs->csize - 1) >> fs->cshift;
        ainfo_t na = n->ai;
        na.size = size;
        na.init = MIN(na.init, size);
        if (need > have) {
            u64 cap;
            if ((r = grow_alloc(fs, n, size, &cap))) goto out;
            if (cap < size) {
                nt_clus_free_runs(fs, &n->runs, have);
                nrl_truncate(&n->runs, have);
                r = E_NOSPC;
                goto out;
            }
            na.alloc = need << fs->cshift;
            r = data_store(fs, n, &ino, have, &na);
        } else if (need < have) {
            nrl_t tail = { 0 };
            for (u32 i = 0; i < n->runs.n && !r; i++) {
                nrun_t *x = &n->runs.r[i];
                u64 s = (u64)x->vcn, e = s + x->len;
                if (e <= need || x->lcn < 0) continue;
                u64 f = MAX(s, need);
                r = nrl_add(&tail, (i64)f, x->lcn + (i64)(f - s), e - f);
            }
            if (!r) {
                nrl_truncate(&n->runs, need);
                na.alloc = need << fs->cshift;
                r = data_store(fs, n, &ino, need, &na);
                if (!r) r = nt_clus_free_runs(fs, &tail, 0);
                else { nrl_free(&tail); node_refresh(fs, n, &ino); goto out; }
            }
            nrl_free(&tail);
        } else {
            if (!(r = nt_attr_set_sizes(&ino, NTFS_AT_DATA, NULL, 0, &na))) n->ai = na;
        }
        if (r) goto out;
    }
done:
    if (r) goto out;
    si_touch(&ino, T_M | T_R | T_A, NTFS_FA_ARCHIVE, 0);
    r = names_update(fs, &ino);
out:
    nt_ino_close(&ino);
    return r;
}

/* ------------------------------------------------------------------ */
/* VFS: files                                                           */

static int ntfs_open(volume_t *v, const char *path, int flags, void **fh);

static int ntfs_close(volume_t *v, void *fh) {
    ntfs_t *fs = NTFS(v);
    int r = nt_flush(fs);
    node_unref(fs, fh);
    return r;
}

static int ntfs_read(volume_t *v, void *fh, u64 pos, void *buf, u32 len) {
    return node_read(NTFS(v), fh, pos, buf, len);
}

static int ntfs_write(volume_t *v, void *fh, u64 pos, const void *buf, u32 len) {
    ntfs_t *fs = NTFS(v);
    nnode_t *n = fh;
    if (fs->ro) return E_ROFS;
    if (n->nowrite) return E_NOTSUP;
    if (!len) return 0;
    if (len > 0x7FFFFFFF) len = 0x7FFFFFFF;
    if (pos >= (1ull << 52)) return E_NOSPC;
    int r = nt_writable(fs);
    if (r) return r;
    return op_end(fs, node_write(fs, n, pos, buf, len));
}

static int ntfs_truncate(volume_t *v, void *fh, u64 size) {
    ntfs_t *fs = NTFS(v);
    nnode_t *n = fh;
    if (fs->ro) return E_ROFS;
    if (n->nowrite) return E_NOTSUP;
    if (size >= (1ull << 52)) return E_NOSPC;
    if (size == n->ai.size) return 0;
    int r = nt_writable(fs);
    if (r) return r;
    return op_end(fs, node_truncate(fs, n, size));
}

static u64 ntfs_size(volume_t *v, void *fh) { return ((nnode_t *)fh)->ai.size; }

/* ------------------------------------------------------------------ */
/* VFS: directories                                                     */

static void fill_from_fn(const ntfs_file_name_t *fn, vfs_dirent_t *o) {
    u32 fa = fn->file_attributes;
    o->attr = (u8)(fa & (NTFS_FA_READONLY | NTFS_FA_HIDDEN | NTFS_FA_SYSTEM | NTFS_FA_ARCHIVE));
    o->is_dir = (fa & NTFS_FA_DUP_FILE_NAME_INDEX) != 0;
    if (o->is_dir) o->attr |= VFS_ATTR_DIR;
    if ((fa & NTFS_FA_REPARSE_POINT) &&
        (fn->reparse_tag == NTFS_REPARSE_TAG_SYMLINK || fn->reparse_tag == NTFS_REPARSE_TAG_MOUNT_POINT))
        o->attr |= VFS_ATTR_LINK;
    o->size = o->is_dir ? 0 : (u64)MAX(fn->data_size, 0);
    nt_to_vtime(fn->data_change_time, &o->mtime);
    nt_to_vtime(fn->creation_time, &o->ctime);
}

static void short_from_fn(const ntfs_file_name_t *fn, char *out) {
    out[0] = 0;
    if (fn->name_length > 12) return;
    char tmp[VFS_MAX_NAME + 1];
    display_name(fn->name, fn->name_length, tmp);
    if (strlen(tmp) <= 12) strlcpy(out, tmp, 13);
}

typedef struct { u64 rec; char sn[13]; } dosmap_t;

typedef struct {
    ntfs_t *fs;
    u64 dir;
    int (*cb)(void *ctx, const vfs_dirent_t *e);
    void *ctx;
    vfs_dirent_t de;
    dosmap_t *dos;
    u32 ndos, cap;
    int overflow;
    int pass;
} rd_t;

static int dos_cmp_find(const rd_t *c, u64 rec) {
    u32 lo = 0, hi = c->ndos;
    while (lo < hi) {
        u32 m = (lo + hi) / 2;
        if (c->dos[m].rec < rec) lo = m + 1; else hi = m;
    }
    return lo < c->ndos && c->dos[lo].rec == rec ? (int)lo : -1;
}

static int rd_cb(void *vctx, const ntfs_index_entry_t *ie) {
    rd_t *c = vctx;
    const ntfs_file_name_t *fn = (const ntfs_file_name_t *)ie->key;
    u64 rec = NTFS_MREF(ie->file);
    if (c->pass == 0) {
        if (fn->name_type != NTFS_NS_DOS) return 0;
        if (c->ndos >= DOS_MAP_MAX) { c->overflow = 1; return 0; }
        if (c->ndos == c->cap) {
            u32 nc = c->cap ? c->cap * 2 : 64;
            dosmap_t *n = krealloc(c->dos, nc * sizeof *n);
            if (!n) { c->overflow = 1; return 0; }
            c->dos = n;
            c->cap = nc;
        }
        c->dos[c->ndos].rec = rec;
        short_from_fn(fn, c->dos[c->ndos].sn);
        c->ndos++;
        return 0;
    }
    if (fn->name_type == NTFS_NS_DOS) return 0;
    if (rec == NTFS_MREF(c->dir) || hidden_entry(c->dir, ie->file)) return 0;
    vfs_dirent_t *o = &c->de;
    memset(o, 0, sizeof *o);
    display_name(fn->name, fn->name_length, o->name);
    fill_from_fn(fn, o);
    if (fn->name_type == NTFS_NS_WIN32_AND_DOS) short_from_fn(fn, o->short_name);
    else if (fn->name_type == NTFS_NS_WIN32) {
        int i = dos_cmp_find(c, rec);
        if (i >= 0) strlcpy(o->short_name, c->dos[i].sn, sizeof o->short_name);
    }
    return c->cb(c->ctx, o) ? 1 : 0;
}

static void dos_sort(dosmap_t *a, u32 n) {
    /* shell sort by record number */
    for (u32 gap = n / 2; gap; gap /= 2)
        for (u32 i = gap; i < n; i++) {
            dosmap_t t = a[i];
            u32 j = i;
            while (j >= gap && a[j - gap].rec > t.rec) { a[j] = a[j - gap]; j -= gap; }
            a[j] = t;
        }
}

static int ntfs_readdir(volume_t *v, const char *path, int (*cb)(void *ctx, const vfs_dirent_t *e), void *ctx) {
    ntfs_t *fs = NTFS(v);
    nent_t *e = kmalloc(sizeof *e);
    rd_t *c = kzalloc(sizeof *c);
    int r = (!e || !c) ? E_NOMEM : 0, isroot = 0;
    u64 dir = NTFS_MK_MREF(NTFS_FILE_ROOT, 0);
    if (!r) r = resolve(fs, path, e, &isroot, NULL, NULL);
    if (!r && !isroot) {
        ntfs_file_name_t *fn = ent_fn(e);
        if (!(fn->file_attributes & NTFS_FA_DUP_FILE_NAME_INDEX)) r = E_NOTDIR;
        else if (fn->file_attributes & NTFS_FA_REPARSE_POINT) r = E_NOTSUP;
        dir = e->mref;
    }
    nino_t d;
    if (!r && !(r = nt_ino_open(fs, dir, 1, &d))) {
        nidx_t ix;
        if (!(r = nt_idx_open(&d, nt_I30, 4, nt_coll_filename, &ix))) {
            c->fs = fs;
            c->dir = NTFS_MK_MREF(d.rec, d.seq);
            c->cb = cb;
            c->ctx = ctx;
            c->pass = 0;
            r = nt_idx_walk(&ix, rd_cb, c);
            if (!r) {
                dos_sort(c->dos, c->ndos);
                c->pass = 1;
                r = nt_idx_walk(&ix, rd_cb, c);
                if (r > 0) r = 0;
            }
            nt_idx_close(&ix);
        } else if (r == E_NOENT) {
            r = E_NOTDIR;
        }
        nt_ino_close(&d);
    }
    if (c) kfree(c->dos);
    kfree(c);
    kfree(e);
    return r;
}

static int ntfs_stat(volume_t *v, const char *path, vfs_dirent_t *out) {
    ntfs_t *fs = NTFS(v);
    nent_t *e = kmalloc(sizeof *e);
    if (!e) return E_NOMEM;
    int isroot, r = resolve(fs, path, e, &isroot, NULL, NULL);
    if (!r && isroot) {
        memset(out, 0, sizeof *out);
        out->attr = VFS_ATTR_DIR;
        out->is_dir = 1;
    } else if (!r) {
        memset(out, 0, sizeof *out);
        ntfs_file_name_t *fn = ent_fn(e);
        strlcpy(out->name, e->name, sizeof out->name);
        fill_from_fn(fn, out);
        /* the record has the authoritative times, sizes and short name */
        nino_t ino;
        if (!nt_ino_open(fs, e->mref, 1, &ino)) {
            nsi_t si;
            if (!si_get(&ino, &si)) {
                out->attr = (u8)((out->attr & (VFS_ATTR_DIR | VFS_ATTR_LINK)) |
                                 (si.attrs & (NTFS_FA_READONLY | NTFS_FA_HIDDEN | NTFS_FA_SYSTEM | NTFS_FA_ARCHIVE)));
                nt_to_vtime(si.mt, &out->mtime);
                nt_to_vtime(si.ct, &out->ctime);
            }
            if (!out->is_dir) {
                nnode_t *n = node_find(fs, ino.rec);
                u64 alloc, size;
                dup_sizes(&ino, 0, &alloc, &size);
                out->size = n ? n->ai.size : size;
            }
            if (fn->name_type == NTFS_NS_WIN32_AND_DOS || fn->name_type == NTFS_NS_DOS) {
                short_from_fn(fn, out->short_name);
            } else if (fn->name_type == NTFS_NS_WIN32) {
                actx_t c;
                int k = nt_attr_first(&ino, &c, NTFS_AT_FILE_NAME, NULL, 0, 0);
                while (!k) {
                    ntfs_attr_t *a = ac_attr(&c);
                    ntfs_file_name_t *f2 = (ntfs_file_name_t *)attr_value(a);
                    if (!a->non_resident && a->r.value_length >= sizeof *f2 &&
                        sizeof *f2 + 2u * f2->name_length <= a->r.value_length &&
                        f2->name_type == NTFS_NS_DOS && NTFS_MREF(f2->parent) == NTFS_MREF(fn->parent)) {
                        short_from_fn(f2, out->short_name);
                        nt_actx_done(&c);
                        break;
                    }
                    k = nt_attr_next(&c);
                }
            }
            nt_ino_close(&ino);
        }
    }
    kfree(e);
    return r;
}

/* ------------------------------------------------------------------ */
/* creating                                                             */

/* the descriptor of new files on volumes without a usable security id
 * (formatted by mkntfs): self-relative, owner and group Administrators,
 * Everyone full control (inherited by files and folders) */
static const u8 default_sd[80] = {
    1, 0, 0x04, 0x80, 48, 0, 0, 0, 64, 0, 0, 0, 0, 0, 0, 0, 20, 0, 0, 0,
    /* DACL: revision 2, size 28, one ACE */
    2, 0, 28, 0, 1, 0, 0, 0,
    /* ACCESS_ALLOWED, OBJECT_INHERIT | CONTAINER_INHERIT, size 20, FILE_ALL_ACCESS, S-1-1-0 */
    0, 3, 20, 0, 0xFF, 0x01, 0x1F, 0x00, 1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0,
    /* owner, group: S-1-5-32-544 */
    1, 2, 0, 0, 0, 0, 0, 5, 32, 0, 0, 0, 0x20, 0x02, 0, 0,
    1, 2, 0, 0, 0, 0, 0, 5, 32, 0, 0, 0, 0x20, 0x02, 0, 0,
};

/* a new file or directory name in directory dmref; *out: its reference */
static int create_entry(ntfs_t *fs, u64 dmref, const u16 *name, int nlen, int is_dir, u64 *out) {
    nino_t d;
    int r = nt_ino_open(fs, dmref, 1, &d);
    if (r) return r == E_NOENT ? E_IO : r;
    nsi_t psi;
    u8 *sd = NULL;
    u32 sdlen = 0;
    if ((r = si_get(&d, &psi))) goto out_d;
    /* security: the directory's descriptor id in $Secure; else a copy of
     * its own (resident) descriptor; else the root's id; else a default
     * descriptor in the record ($Secure itself is never written) */
    u32 secid = psi.secid;
    if (!secid) {
        ainfo_t sa;
        if (!nt_attr_info(&d, NTFS_AT_SECURITY_DESCRIPTOR, NULL, 0, &sa, NULL) && sa.resident && sa.size >= 20 &&
            sa.size <= 512) {
            sdlen = (u32)sa.size;
            if (!(sd = kmalloc(sdlen))) { r = E_NOMEM; goto out_d; }
            if ((r = nt_attr_read(&d, NTFS_AT_SECURITY_DESCRIPTOR, NULL, 0, 0, sdlen, sd)) < 0) goto out_d;
            r = 0;
        } else if (fs->sec_id) {
            secid = fs->sec_id;
        } else {
            sdlen = sizeof default_sd;
            if (!(sd = kmalloc(sdlen))) { r = E_NOMEM; goto out_d; }
            memcpy(sd, default_sd, sdlen);
        }
    }
    u64 rec;
    if ((r = nt_mft_alloc(fs, &rec))) goto out_d;
    /* the record's old sequence number (a reused record gets the next one,
     * already incremented when it was freed) */
    u16 seq = 1;
    fblk_t *b;
    if (!nt_rec_get(fs, rec, &b)) {
        ntfs_mft_record_t *m = fb_rec(b);
        if (m->flags & NTFS_MFT_IN_USE) {
            nt_fb_put(fs, b);
            nt_corrupt(fs, "a free MFT record is in use", rec);
            r = E_IO;
            goto out_d;
        }
        seq = m->sequence ? m->sequence : 1;
        nt_fb_put(fs, b);
    }
    if ((r = nt_rec_get_fresh(fs, rec, &b))) { nt_mft_free(fs, rec); goto out_d; }
    nt_rec_init(fs, b, rec, seq, NTFS_MFT_IN_USE | (is_dir ? NTFS_MFT_DIRECTORY : 0));
    fb_rec(b)->link_count = 1;
    nino_t f;
    memset(&f, 0, sizeof f);
    f.fs = fs;
    f.rec = rec;
    f.seq = seq;
    f.base = b;
    /* $STANDARD_INFORMATION */
    i64 now = nt_now();
    ntfs_std_info_t si;
    memset(&si, 0, sizeof si);
    si.creation_time = si.data_change_time = si.mft_change_time = si.access_time = now;
    si.file_attributes = is_dir ? 0 : NTFS_FA_ARCHIVE;
    si.security_id = secid;
    r = nt_attr_add_resident(&f, NTFS_AT_STANDARD_INFORMATION, NULL, 0, &si, secid ? 72 : 48, 0);
    /* $FILE_NAME */
    u8 *fnb = kmalloc(NT_FN_MAX + NT_IE_MAX);
    if (!fnb) r = E_NOMEM;
    nsi_t nsi = { now, now, now, now, si.file_attributes, secid, 72 };
    u32 fnlen = 0;
    if (!r) {
        fnlen = fn_build(fnb, dmref, &nsi, 0, 0, si.file_attributes | (is_dir ? NTFS_FA_DUP_FILE_NAME_INDEX : 0), 0,
                         name, nlen, name_space(name, nlen));
        r = nt_attr_add_resident(&f, NTFS_AT_FILE_NAME, NULL, 0, fnb, fnlen, NTFS_RESIDENT_INDEXED);
    }
    if (!r && sd) r = nt_attr_add_resident(&f, NTFS_AT_SECURITY_DESCRIPTOR, NULL, 0, sd, sdlen, 0);
    if (!r && !is_dir) r = nt_attr_add_resident(&f, NTFS_AT_DATA, NULL, 0, "", 0, 0);
    if (!r && is_dir) {
        u8 root[sizeof(ntfs_index_root_t) + 16];
        memset(root, 0, sizeof root);
        ntfs_index_root_t *ir = (ntfs_index_root_t *)root;
        ir->type = NTFS_AT_FILE_NAME;
        ir->collation_rule = NTFS_COLLATION_FILE_NAME;
        ir->index_block_size = fs->ib_size;
        ir->clusters_per_index_block = (u8)(fs->ib_size >= fs->csize ? fs->ib_size / fs->csize : fs->ib_size / 512);
        ir->ih.entries_offset = 0x10;
        ir->ih.index_length = ir->ih.allocated_size = 0x20;
        ntfs_index_entry_t *end = (ntfs_index_entry_t *)(root + sizeof *ir);
        end->length = 16;
        end->flags = NTFS_IE_END;
        r = nt_attr_add_resident(&f, NTFS_AT_INDEX_ROOT, nt_I30, 4, root, sizeof root, 0);
    }
    /* the directory entry */
    if (!r) {
        u8 *ie = fnb + NT_FN_MAX;
        ie_build(ie, NTFS_MK_MREF(rec, seq), fnb, fnlen);
        nidx_t ix;
        if (!(r = nt_idx_open(&d, nt_I30, 4, nt_coll_filename, &ix))) {
            r = nt_idx_insert(&ix, (ntfs_index_entry_t *)ie);
            nt_idx_close(&ix);
        }
    }
    kfree(fnb);
    if (r) {
        /* give the record back */
        ntfs_mft_record_t *m = fb_rec(b);
        m->flags = 0;
        b->dirty = 1;
        nt_fb_put(fs, b);
        nt_mft_free(fs, rec);
        goto out_d;
    }
    nt_fb_put(fs, b);
    si_touch(&d, T_M | T_R | T_A, 0, 0);
    *out = NTFS_MK_MREF(rec, seq);
out_d:
    kfree(sd);
    nt_ino_close(&d);
    return r;
}

static int ntfs_open(volume_t *v, const char *path, int flags, void **fh) {
    ntfs_t *fs = NTFS(v);
    if (!*path) return E_ISDIR;
    int wr = (flags & (VFS_O_WRITE | VFS_O_TRUNC)) != 0;
    if (fs->ro && (wr || (flags & VFS_O_CREATE))) return E_ROFS;
    nent_t *e = kmalloc(sizeof *e);
    char *last = kmalloc(VFS_MAX_NAME + 1);
    u16 *units = kmalloc(2 * (NT_MAX_NAME + 1));
    int r = (!e || !last || !units) ? E_NOMEM : 0, isroot;
    u64 dir;
    nnode_t *n = NULL;
    if (!r) r = resolve(fs, path, e, &isroot, &dir, last);
    if (r == E_NOENT && (flags & VFS_O_CREATE) && dir) {
        /* the parent exists: resolve stopped at the last component */
        int nu;
        if ((r = name_units(last, units, &nu))) goto out;
        if ((r = nt_writable(fs))) goto out;
        u64 mref;
        r = op_end(fs, create_entry(fs, dir, units, nu, 0, &mref));
        if (r) goto out;
        e->mref = mref;
        strlcpy(e->name, last, sizeof e->name);
        if ((r = node_get(fs, e, &n))) goto out;
    } else if (r) {
        goto out;
    } else {
        if (isroot) { r = E_ISDIR; goto out; }
        if ((flags & VFS_O_CREATE) && (flags & VFS_O_EXCL)) { r = E_EXIST; goto out; }
        if (ent_fn(e)->file_attributes & NTFS_FA_DUP_FILE_NAME_INDEX) { r = E_ISDIR; goto out; }
        if ((r = node_get(fs, e, &n))) goto out;
        if (wr) {
            nino_t ino;
            nsi_t si;
            if (n->nowrite) r = E_NOTSUP;
            else if (!(r = nt_ino_open(fs, n->rec, 0, &ino))) {
                if (!(r = si_get(&ino, &si)) && (si.attrs & NTFS_FA_READONLY)) r = E_ROFS;
                nt_ino_close(&ino);
            }
            if (r) { node_unref(fs, n); n = NULL; goto out; }
        }
        if ((flags & VFS_O_TRUNC) && (flags & VFS_O_WRITE) && n->ai.size) {
            if (!(r = nt_writable(fs))) r = op_end(fs, node_truncate(fs, n, 0));
            if (r) { node_unref(fs, n); n = NULL; goto out; }
        }
    }
    *fh = n;
out:
    kfree(e);
    kfree(last);
    kfree(units);
    return r;
}

static int ntfs_mkdir(volume_t *v, const char *path) {
    ntfs_t *fs = NTFS(v);
    if (fs->ro) return E_ROFS;
    nent_t *e = kmalloc(sizeof *e);
    char *last = kmalloc(VFS_MAX_NAME + 1);
    u16 *units = kmalloc(2 * (NT_MAX_NAME + 1));
    int r = (!e || !last || !units) ? E_NOMEM : 0, isroot, nu;
    u64 dir, mref;
    if (!r) r = resolve(fs, path, e, &isroot, &dir, last);
    if (!r) r = E_EXIST;
    else if (r == E_NOENT && dir) {
        r = name_units(last, units, &nu);
        if (!r) r = nt_writable(fs);
        if (!r) r = op_end(fs, create_entry(fs, dir, units, nu, 1, &mref));
    }
    kfree(e);
    kfree(last);
    kfree(units);
    return r;
}

/* ------------------------------------------------------------------ */
/* deleting and renaming                                                */

/* remove the entry for (tag/guid) key from an index of a $Extend file */
static void extend_index_remove(ntfs_t *fs, const char *file, const u16 *iname, const u8 *key, u32 klen) {
    nino_t ext, f;
    nent_t *e = kmalloc(sizeof *e);
    if (!e) return;
    if (!nt_ino_open(fs, NTFS_FILE_EXTEND, 0, &ext)) {
        int r = dir_lookup(fs, &ext, file, e);
        nt_ino_close(&ext);
        if (!r && !nt_ino_open(fs, e->mref, 1, &f)) {
            nidx_t ix;
            if (!nt_idx_open(&f, iname, 2, nt_coll_ulongs, &ix)) {
                r = nt_idx_delete(&ix, key, klen, 0);
                if (r && r != E_NOENT) kprintf("ntfs: %s: %s: cannot remove an entry (%d)\n", fs->bd->name, file, r);
                nt_idx_close(&ix);
            }
            nt_ino_close(&f);
        }
    }
    kfree(e);
}

static const u16 nt_R[2] = { '$', 'R' }, nt_O[2] = { '$', 'O' };

/* the $FILE_NAME attributes of ino for one name (and its DOS / Win32
 * partner): their values are copied to names[] (each NT_FN_MAX bytes) */
static int names_of_link(nino_t *ino, const ntfs_file_name_t *key, u8 *names, int *count) {
    actx_t c;
    int n = 0;
    int r = nt_attr_first(ino, &c, NTFS_AT_FILE_NAME, NULL, 0, 0);
    while (!r) {
        ntfs_attr_t *a = ac_attr(&c);
        ntfs_file_name_t *fn = (ntfs_file_name_t *)attr_value(a);
        if (!a->non_resident && a->r.value_length >= sizeof *fn &&
            sizeof *fn + 2u * fn->name_length <= a->r.value_length &&
            NTFS_MREF(fn->parent) == NTFS_MREF(key->parent)) {
            int same = fn->name_length == key->name_length &&
                       !memcmp(fn->name, key->name, 2u * key->name_length);
            int partner = (key->name_type == NTFS_NS_WIN32 && fn->name_type == NTFS_NS_DOS) ||
                          (key->name_type == NTFS_NS_DOS && fn->name_type == NTFS_NS_WIN32);
            if ((same || partner) && n < 2) {
                memcpy(names + (size_t)n * NT_FN_MAX, fn, MIN(a->r.value_length, (u32)NT_FN_MAX));
                n++;
            }
        }
        r = nt_attr_next(&c);
    }
    *count = n;
    return r == E_NOENT ? 0 : r;
}

/* remove the $FILE_NAME attribute equal to fn */
static int fn_attr_remove(nino_t *ino, const ntfs_file_name_t *fn) {
    actx_t c;
    int r = nt_attr_first(ino, &c, NTFS_AT_FILE_NAME, NULL, 0, 0);
    while (!r) {
        ntfs_attr_t *a = ac_attr(&c);
        ntfs_file_name_t *x = (ntfs_file_name_t *)attr_value(a);
        if (!a->non_resident && a->r.value_length >= sizeof *x && x->parent == fn->parent &&
            x->name_type == fn->name_type && x->name_length == fn->name_length &&
            !memcmp(x->name, fn->name, 2u * fn->name_length))
            return nt_attr_remove(ino, &c);
        r = nt_attr_next(&c);
    }
    return r == E_NOENT ? E_IO : r;
}

static int dir_is_empty(ntfs_t *fs, nino_t *ino) {
    nidx_t ix;
    int r = nt_idx_open(ino, nt_I30, 4, nt_coll_filename, &ix);
    if (r) return r;
    r = nt_idx_empty(&ix);
    nt_idx_close(&ix);
    return r;
}

static int protected_rec(u64 rec) { return rec < NT_FIRST_FREE; }

static int ntfs_unlink(volume_t *v, const char *path) {
    ntfs_t *fs = NTFS(v);
    if (fs->ro) return E_ROFS;
    if (!*path) return E_INVAL;
    nent_t *e = kmalloc(sizeof *e);
    u8 *names = kmalloc(2 * NT_FN_MAX);
    int r = (!e || !names) ? E_NOMEM : 0, isroot, cnt = 0;
    nino_t ino, d;
    int have_ino = 0, have_d = 0;
    if (!r) r = resolve(fs, path, e, &isroot, NULL, NULL);
    if (r) goto out;
    if (isroot) { r = E_INVAL; goto out; }
    if (protected_rec(NTFS_MREF(e->mref)) || NTFS_MREF(e->dir) == NTFS_FILE_EXTEND) { r = E_PERM; goto out; }
    if (node_find(fs, NTFS_MREF(e->mref))) { r = E_BUSY; goto out; }
    if ((r = nt_ino_open(fs, e->mref, 1, &ino))) { if (r == E_NOENT) r = E_IO; goto out; }
    have_ino = 1;
    if (ino.al_nonres) { r = E_NOTSUP; goto out; }
    if (ino_is_dir(&ino)) {
        r = dir_is_empty(fs, &ino);
        if (r < 0) goto out;
        if (r == 0) { r = E_NOTEMPTY; goto out; }
        r = 0;
    }
    if ((r = names_of_link(&ino, ent_fn(e), names, &cnt))) goto out;
    if (!cnt) { nt_corrupt(fs, "directory entry without its $FILE_NAME", ino.rec); r = E_IO; goto out; }
    if ((r = nt_writable(fs))) goto out;
    if ((r = nt_ino_open(fs, e->dir, 1, &d))) { if (r == E_NOENT) r = E_IO; goto out; }
    have_d = 1;
    /* the directory entries first, then the names in the record */
    {
        nidx_t ix;
        if ((r = nt_idx_open(&d, nt_I30, 4, nt_coll_filename, &ix))) goto out;
        u64 me = NTFS_MK_MREF(ino.rec, ino.seq);
        if (cnt > 1) nt_idx_reserve(&ix, cnt);
        int done = 0;
        for (int i = 0; i < cnt && !r; i++) {
            ntfs_file_name_t *fn = (ntfs_file_name_t *)(names + (size_t)i * NT_FN_MAX);
            r = nt_idx_delete(&ix, (u8 *)fn, (u32)sizeof *fn + 2u * fn->name_length, me);
            if (r == E_NOENT) {
                kprintf("ntfs: %s: record %lu: a name missing from its directory\n", fs->bd->name, (unsigned long)ino.rec);
                r = 0;
            }
            if (!r) done++;
        }
        if (r && done > 0) {
            /* the second name could not go: put the first one back */
            ntfs_file_name_t *fn = (ntfs_file_name_t *)names;
            u8 *ie = kmalloc(NT_IE_MAX);
            if (ie) ie_build(ie, me, (u8 *)fn, (u32)sizeof *fn + 2u * fn->name_length);
            if (!ie || nt_idx_insert(&ix, (ntfs_index_entry_t *)ie))
                nt_corrupt(fs, "a failed deletion left the directory inconsistent", ino.rec);
            kfree(ie);
        }
        nt_idx_close(&ix);
        if (r) goto out;
    }
    for (int i = 0; i < cnt && !r; i++) r = fn_attr_remove(&ino, (ntfs_file_name_t *)(names + (size_t)i * NT_FN_MAX));
    if (r) goto out;
    ntfs_mft_record_t *m = fb_rec(ino.base);
    m->link_count = (u16)(m->link_count > cnt ? m->link_count - cnt : 0);
    ino.base->dirty = 1;
    actx_t c;
    int more = nt_attr_first(&ino, &c, NTFS_AT_FILE_NAME, NULL, 0, 0);
    if (!more) {
        nt_actx_done(&c);
        si_touch(&ino, T_R, 0, 0);
        names_update(fs, &ino);
    } else {
        /* the last name: the file goes */
        nsi_t si;
        if (!si_get(&ino, &si) && (si.attrs & NTFS_FA_REPARSE_POINT)) {
            u8 key[12];
            *(u32 *)key = reparse_tag(&ino);
            *(u64 *)(key + 4) = NTFS_MK_MREF(ino.rec, ino.seq);
            extend_index_remove(fs, "$Reparse", nt_R, key, 12);
        }
        u8 guid[16];
        if (nt_attr_read(&ino, NTFS_AT_OBJECT_ID, NULL, 0, 0, 16, guid) == 16)
            extend_index_remove(fs, "$ObjId", nt_O, guid, 16);
        r = nt_ino_free_all(&ino);
    }
    si_touch(&d, T_M | T_R | T_A, 0, 0);
out:
    if (have_d) nt_ino_close(&d);
    if (have_ino) nt_ino_close(&ino);
    kfree(e);
    kfree(names);
    return op_end(fs, r);
}

/* is rec dir itself or one of its ancestors? */
static int is_ancestor(ntfs_t *fs, u64 rec, u64 dir) {
    for (int i = 0; i < 1024; i++) {
        if (NTFS_MREF(dir) == rec) return 1;
        if (NTFS_MREF(dir) == NTFS_FILE_ROOT) return 0;
        nino_t d;
        if (nt_ino_open(fs, dir, 0, &d)) return 1;
        actx_t c;
        u64 parent = 0;
        if (!nt_attr_first(&d, &c, NTFS_AT_FILE_NAME, NULL, 0, 0)) {
            ntfs_attr_t *a = ac_attr(&c);
            if (!a->non_resident && a->r.value_length >= sizeof(ntfs_file_name_t))
                parent = ((ntfs_file_name_t *)attr_value(a))->parent;
            nt_actx_done(&c);
        }
        nt_ino_close(&d);
        if (!parent) return 1;
        dir = parent;
    }
    return 1;
}

static int ntfs_rename(volume_t *v, const char *from, const char *to) {
    ntfs_t *fs = NTFS(v);
    if (fs->ro) return E_ROFS;
    if (!*from || !*to) return E_INVAL;
    nent_t *src = kmalloc(sizeof *src), *dst = kmalloc(sizeof *dst);
    char *last = kmalloc(VFS_MAX_NAME + 1);
    u16 *units = kmalloc(2 * (NT_MAX_NAME + 1));
    u8 *names = kmalloc(2 * NT_FN_MAX + NT_FN_MAX + NT_IE_MAX);
    int r = (!src || !dst || !last || !units || !names) ? E_NOMEM : 0, isroot, nu, cnt = 0;
    u64 ddir = 0;
    nino_t ino, sd, dd;
    int have_ino = 0, have_sd = 0, have_dd = 0;
    if (!r) r = resolve(fs, from, src, &isroot, NULL, NULL);
    if (!r && isroot) r = E_INVAL;
    if (r) goto out;
    u64 srec = NTFS_MREF(src->mref);
    if (protected_rec(srec) || NTFS_MREF(src->dir) == NTFS_FILE_EXTEND) { r = E_PERM; goto out; }
    r = resolve(fs, to, dst, &isroot, &ddir, last);
    if (!r && isroot) r = E_INVAL;
    int same_file = 0;
    if (!r) {
        if (NTFS_MREF(dst->mref) != srec) { r = E_EXIST; goto out; }
        same_file = 1;
    } else if (r != E_NOENT || !ddir) {
        goto out;
    }
    if (NTFS_MREF(ddir) == NTFS_FILE_EXTEND) { r = E_PERM; goto out; }
    if ((r = name_units(last, units, &nu))) goto out;
    ntfs_file_name_t *sfn = ent_fn(src);
    if (same_file && NTFS_MREF(ddir) == NTFS_MREF(src->dir) && sfn->name_length == nu &&
        !memcmp(sfn->name, units, 2u * (u32)nu)) {
        r = 0;                          /* the same name */
        goto out;
    }
    if ((sfn->file_attributes & NTFS_FA_DUP_FILE_NAME_INDEX) && is_ancestor(fs, srec, ddir)) { r = E_INVAL; goto out; }
    if ((r = nt_ino_open(fs, src->mref, 1, &ino))) { if (r == E_NOENT) r = E_IO; goto out; }
    have_ino = 1;
    if (ino.al_nonres) { r = E_NOTSUP; goto out; }
    if ((r = names_of_link(&ino, sfn, names, &cnt))) goto out;
    if (!cnt) { nt_corrupt(fs, "directory entry without its $FILE_NAME", srec); r = E_IO; goto out; }
    /* the new name must fit in the base record once the old ones are gone */
    {
        u32 freed = 0;
        for (int i = 0; i < cnt; i++) {
            ntfs_file_name_t *fn = (ntfs_file_name_t *)(names + (size_t)i * NT_FN_MAX);
            freed += ALIGN_UP(0x18u + (u32)sizeof *fn + 2u * fn->name_length, 8u);
        }
        u32 need = ALIGN_UP(0x18u + (u32)sizeof(ntfs_file_name_t) + 2u * (u32)nu, 8u) + (ino.al ? 40u : 0u);
        if (need > nt_rec_free_space(ino.base) + freed) { r = E_NOSPC; goto out; }
    }
    if ((r = nt_writable(fs))) goto out;
    if ((r = nt_ino_open(fs, src->dir, 1, &sd))) { if (r == E_NOENT) r = E_IO; goto out; }
    have_sd = 1;
    int same_dir = NTFS_MREF(ddir) == NTFS_MREF(src->dir);
    if (!same_dir) {
        if ((r = nt_ino_open(fs, ddir, 1, &dd))) { if (r == E_NOENT) r = E_IO; goto out; }
        have_dd = 1;
    }
    nino_t *dp = same_dir ? &sd : &dd;
    /* the new name, with the file's current duplicated information */
    nsi_t si;
    if ((r = si_get(&ino, &si))) goto out;
    int dir = ino_is_dir(&ino);
    u64 alloc, size;
    dup_sizes(&ino, dir, &alloc, &size);
    u8 *nfn = names + 2 * NT_FN_MAX, *nie = nfn + NT_FN_MAX;
    u32 fattrs = (si.attrs & ~(u32)NTFS_FA_DIRECTORY) | (dir ? NTFS_FA_DUP_FILE_NAME_INDEX : 0);
    u32 nfnlen = fn_build(nfn, NTFS_MK_MREF(dp->rec, dp->seq), &si, alloc, size, fattrs,
                          (si.attrs & NTFS_FA_REPARSE_POINT) ? reparse_tag(&ino) : 0, units, nu, name_space(units, nu));
    ie_build(nie, NTFS_MK_MREF(ino.rec, ino.seq), nfn, nfnlen);
    nidx_t six, dix;
    if ((r = nt_idx_open(&sd, nt_I30, 4, nt_coll_filename, &six))) goto out;
    if (!same_dir && (r = nt_idx_open(&dd, nt_I30, 4, nt_coll_filename, &dix))) { nt_idx_close(&six); goto out; }
    nidx_t *dixp = same_dir ? &six : &dix;
    /* the blocks the deletions may need; then the new entry first (a
     * failure leaves the old name) and the old entries out (a failure
     * takes the new one back out). A new name only in case (same
     * directory) would clash with the old one: the old entries go first. */
    u64 me = NTFS_MK_MREF(ino.rec, ino.seq);
    int case_only = same_file && same_dir, done = 0, inserted = 0;
    nt_idx_reserve(&six, cnt + 1);
    if (!case_only && !(r = nt_idx_insert(dixp, (ntfs_index_entry_t *)nie))) inserted = 1;
    for (int i = 0; i < cnt && !r; i++) {
        ntfs_file_name_t *fn = (ntfs_file_name_t *)(names + (size_t)i * NT_FN_MAX);
        int k = nt_idx_delete(&six, (u8 *)fn, (u32)sizeof *fn + 2u * fn->name_length, me);
        if (k && k != E_NOENT) r = k;
        else done++;
    }
    if (!r && case_only) r = nt_idx_insert(dixp, (ntfs_index_entry_t *)nie);
    if (r) {
        /* put the old entries back, take the new one out */
        int bad = 0;
        if (inserted) {
            ntfs_index_entry_t *ne = (ntfs_index_entry_t *)nie;
            if (nt_idx_delete(dixp, ne->key, ne->key_length, me)) bad = 1;
        }
        for (int i = 0; i < done; i++) {
            ntfs_file_name_t *fn = (ntfs_file_name_t *)(names + (size_t)i * NT_FN_MAX);
            u8 *ie = kmalloc(NT_IE_MAX);
            if (!ie) { bad = 1; continue; }
            ie_build(ie, me, (u8 *)fn, (u32)sizeof *fn + 2u * fn->name_length);
            if (nt_idx_insert(&six, (ntfs_index_entry_t *)ie)) bad = 1;
            kfree(ie);
        }
        if (bad) nt_corrupt(fs, "a failed rename left the directories inconsistent", ino.rec);
    }
    nt_idx_close(&six);
    if (!same_dir) nt_idx_close(&dix);
    if (r) goto out;
    /* the record: old names out, the new one in */
    for (int i = 0; i < cnt && !r; i++) r = fn_attr_remove(&ino, (ntfs_file_name_t *)(names + (size_t)i * NT_FN_MAX));
    if (!r) r = nt_attr_add_resident(&ino, NTFS_AT_FILE_NAME, NULL, 0, nfn, nfnlen, NTFS_RESIDENT_INDEXED);
    if (r) { nt_corrupt(fs, "rename could not update the MFT record", ino.rec); goto out; }
    ntfs_mft_record_t *m = fb_rec(ino.base);
    m->link_count = (u16)(m->link_count > cnt ? m->link_count - cnt + 1 : 1);
    ino.base->dirty = 1;
    si_touch(&ino, T_R, 0, 0);
    names_update(fs, &ino);
    si_touch(&sd, T_M | T_R, 0, 0);
    if (!same_dir) si_touch(&dd, T_M | T_R, 0, 0);
out:
    if (have_dd) nt_ino_close(&dd);
    if (have_sd) nt_ino_close(&sd);
    if (have_ino) nt_ino_close(&ino);
    kfree(src);
    kfree(dst);
    kfree(last);
    kfree(units);
    kfree(names);
    return op_end(fs, r);
}

/* ------------------------------------------------------------------ */
/* volume                                                               */

static int ntfs_statfs(volume_t *v, u64 *total, u64 *free_bytes) {
    ntfs_t *fs = NTFS(v);
    if (fs->free_clus == ~0ull) {
        int r = nt_bmp_count_free(fs);
        if (r) return r;
    }
    if (total) *total = fs->nclus << fs->cshift;
    if (free_bytes) *free_bytes = fs->free_clus << fs->cshift;
    return 0;
}

static int ntfs_sync(volume_t *v) {
    ntfs_t *fs = NTFS(v);
    if (fs->ro && !fs->dirty_set) return 0;
    return flush_clean(fs);
}

static void fs_free(ntfs_t *fs) {
    while (fs->nodes) {
        nnode_t *n = fs->nodes;
        fs->nodes = n->next;
        node_free(n);
    }
    nrl_free(&fs->mft_runs);
    nrl_free(&fs->mftbmp_runs);
    nrl_free(&fs->mirr_runs);
    nrl_free(&fs->bmp_runs);
    kfree(fs->upcase);
    kfree(fs->fb_mem);
    kfree(fs->pg_mem);
    kfree(fs->bounce);
    kfree(fs->scan);
    kfree(fs);
}

static int ntfs_unmount(volume_t *v) {
    ntfs_t *fs = NTFS(v);
    if (!fs) return 0;
    int r = (fs->ro && !fs->dirty_set) ? 0 : flush_clean(fs);
    fs_free(fs);
    v->fs_priv = NULL;
    return r;
}

static const fs_ops_t ntfs_ops = {
    .name = "ntfs",
    .unmount = ntfs_unmount,
    .open = ntfs_open,
    .close = ntfs_close,
    .read = ntfs_read,
    .write = ntfs_write,
    .truncate = ntfs_truncate,
    .size = ntfs_size,
    .readdir = ntfs_readdir,
    .stat = ntfs_stat,
    .mkdir = ntfs_mkdir,
    .unlink = ntfs_unlink,
    .rename = ntfs_rename,
    .statfs = ntfs_statfs,
    .sync = ntfs_sync,
};

/* ------------------------------------------------------------------ */
/* mount                                                                */

int ntfs_boot_sane(const u8 *s, u32 dev_ss) {
    const ntfs_boot_t *b = (const ntfs_boot_t *)s;
    if (memcmp(b->oem, "NTFS    ", 8) || b->signature != 0xAA55) return 0;
    u32 ss = b->bytes_per_sector;
    if (ss < 256 || ss > 4096 || (ss & (ss - 1))) return 0;
    u32 spc = b->sectors_per_cluster;
    if (spc > 0x80 && spc < 0xF4) return 0;
    if (spc <= 0x80 && (!spc || (spc & (spc - 1)))) return 0;
    u32 shift = spc >= 0xF4 ? 256 - spc : 0;
    u64 csize = spc >= 0xF4 ? (1ull << shift) * ss : (u64)spc * ss;
    if (csize > 2u * 1024 * 1024) return 0;
    if (!b->total_sectors || b->total_sectors >= (1ull << 48)) return 0;
    u64 nclus = b->total_sectors * ss / csize;
    if (!nclus || b->mft_lcn >= nclus || b->mftmirr_lcn >= nclus) return 0;
    u32 rs = ntfs_record_bytes(b->clusters_per_mft_record, (u32)csize);
    u32 is = ntfs_record_bytes(b->clusters_per_index_block, (u32)csize);
    if (rs < 512 || rs > 4096 || (rs & (rs - 1))) return 0;
    if (is < 512 || is > 65536 || (is & (is - 1))) return 0;
    (void)dev_ss;
    return 1;
}

/* is $LogFile clean? 1 clean (or empty), 0 not */
static int logfile_clean(ntfs_t *fs) {
    nino_t ino;
    if (nt_ino_open(fs, NTFS_FILE_LOGFILE, 0, &ino)) return 0;
    ainfo_t ai;
    nrl_t rl = { 0 };
    int r = nt_attr_info(&ino, NTFS_AT_DATA, NULL, 0, &ai, &rl);
    nt_ino_close(&ino);
    u8 *pg = NULL;
    int clean = 0, valid = 0;
    u64 best_lsn = 0;
    if (r || ai.resident || ai.size < 8192 || ai.init < 8192) goto out;
    pg = kmalloc(8192);
    if (!pg) goto out;
    if (nt_runs_rw(fs, &rl, 0, 8192, pg, 0)) goto out;
    int empty = 1;
    for (u32 i = 0; i < 8192; i++) if (pg[i] != 0xFF) { empty = 0; break; }
    if (empty) { clean = 1; goto out; }
    u32 psize = 4096;
    for (int k = 0; k < 2; k++) {
        u8 *p = pg + (u32)k * psize;
        if (*(u32 *)p != NTFS_MAGIC_RSTR) continue;
        u32 sys = *(u32 *)(p + 0x10), logp = *(u32 *)(p + 0x14);
        if (sys != 4096 || logp < 512 || logp > 65536 || (logp & (logp - 1))) continue;
        if (ntfs_fixup_read(p, 4096, NTFS_FIXUP_STRIDE)) continue;
        u16 ra_off = *(u16 *)(p + 0x18);
        i16 major = *(i16 *)(p + 0x1C);
        if (ra_off < 0x1E || ra_off + 0x30u > 4096 || (major != 1 && major != 2)) continue;
        const u8 *ra = p + ra_off;
        u64 lsn = *(const u64 *)ra;
        u16 in_use = *(const u16 *)(ra + 0x0C), flags = *(const u16 *)(ra + 0x0E);
        if (!valid || lsn >= best_lsn) {
            best_lsn = lsn;
            clean = in_use == 0xFFFF || (flags & 0x0002);
        }
        valid = 1;
    }
out:
    kfree(pg);
    nrl_free(&rl);
    return clean;
}

/* does hiberfil.sys hold a hibernated system (Fast Startup or hibernation)? */
static int hibernated(ntfs_t *fs) {
    nino_t root, f;
    nent_t *e = kmalloc(sizeof *e);
    int hib = 0;
    if (!e) return 0;
    if (!nt_ino_open(fs, NTFS_FILE_ROOT, 0, &root)) {
        int r = dir_lookup(fs, &root, "hiberfil.sys", e);
        nt_ino_close(&root);
        if (!r && !nt_ino_open(fs, e->mref, 1, &f)) {
            u8 sig[4];
            if (nt_attr_read(&f, NTFS_AT_DATA, NULL, 0, 0, 4, sig) == 4) {
                char s[5];
                for (int i = 0; i < 4; i++) s[i] = (char)tolower(sig[i]);
                s[4] = 0;
                hib = !strcmp(s, "hibr") || !strcmp(s, "rstr");
            }
            nt_ino_close(&f);
        }
    }
    kfree(e);
    return hib;
}

/* an internal disk holding Windows (\Windows\System32)? */
static int has_windows(ntfs_t *fs) {
    nent_t *e = kmalloc(sizeof *e);
    int isroot, r = e ? resolve(fs, "Windows/System32", e, &isroot, NULL, NULL) : E_NOMEM;
    int yes = !r && (ent_fn(e)->file_attributes & NTFS_FA_DUP_FILE_NAME_INDEX);
    kfree(e);
    return yes;
}

void ntfs_write_key(u64 serial, char *out, int outsz) {
    snprintf(out, (size_t)outsz, "ntfs.write.%016llX", (unsigned long long)serial);
}

static int internal_allowed(ntfs_t *fs) {
    char key[40];
    ntfs_write_key(fs->serial, key, sizeof key);
    return settings_get_int(key, 0) == 1;
}

void ntfs_settings_changed(void) {
    for (int i = 0; i < vfs_volume_count(); i++) {
        volume_t *v = vfs_volume(i);
        if (!v || v->ops != &ntfs_ops || !v->fs_priv) continue;
        ntfs_t *fs = NTFS(v);
        if (fs->ro == NT_RO_INTERNAL && internal_allowed(fs)) {
            fs->ro = NT_RO_NONE;
            v->readonly = 0;
            v->ro_reason[0] = 0;
            kprintf("ntfs: %c: writing allowed by the settings\n", v->letter);
        }
    }
}

/* the runs of a system file's unnamed $DATA */
static int sys_runs(ntfs_t *fs, u64 rec, nrl_t *rl, ainfo_t *ai) {
    nino_t ino;
    int r = nt_ino_open(fs, rec, 0, &ino);
    if (r) return r;
    r = nt_attr_info(&ino, NTFS_AT_DATA, NULL, 0, ai, rl);
    nt_ino_close(&ino);
    if (!r && ai->resident) r = E_IO;
    return r;
}

static int load_mft(ntfs_t *fs, u64 lcn) {
    /* bootstrap: record 0 at its cluster, then $MFT's own run list */
    nrl_free(&fs->mft_runs);
    u64 boot_clus = ((u64)fs->rec_size * 4 + fs->csize - 1) >> fs->cshift;
    int r = nrl_add(&fs->mft_runs, 0, (i64)lcn, boot_clus);
    if (r) return r;
    fs->mft_size = fs->mft_init = (u64)fs->rec_size;
    fblk_t *b;
    if ((r = nt_rec_get(fs, 0, &b))) return r;
    u32 pos = 0, off;
    ntfs_attr_t *a;
    nrl_t first = { 0 };
    int found = 0;
    while ((r = nt_rec_walk(fs, b, &pos, &off, &a)) == 1) {
        if (a->type == NTFS_AT_DATA && !a->name_length && a->non_resident && a->nr.lowest_vcn == 0) {
            r = nrl_decode(fs, a, &first);
            fs->mft_size = (u64)a->nr.data_size;
            fs->mft_init = (u64)a->nr.initialized_size;
            fs->mft_alloc = (u64)a->nr.allocated_size;
            found = 1;
            break;
        }
    }
    b->valid = 0;           /* reload through the real run list */
    nt_fb_put(fs, b);
    if (r < 0 || !found) { nrl_free(&first); return E_IO; }
    nrl_free(&fs->mft_runs);
    fs->mft_runs = first;
    if (fs->mft_init > fs->mft_size) fs->mft_init = fs->mft_size;
    if ((nrl_vcns(&fs->mft_runs) << fs->cshift) < fs->mft_init) {
        /* more extents (an attribute list): read them through the first */
        u64 keep = fs->mft_init;
        fs->mft_init = MIN(keep, nrl_vcns(&fs->mft_runs) << fs->cshift);
        nrl_t all = { 0 };
        ainfo_t ai;
        nino_t ino;
        if ((r = nt_ino_open(fs, 0, 0, &ino))) return r;
        r = nt_attr_info(&ino, NTFS_AT_DATA, NULL, 0, &ai, &all);
        nt_ino_close(&ino);
        if (r) { nrl_free(&all); return r; }
        nrl_free(&fs->mft_runs);
        fs->mft_runs = all;
        fs->mft_init = keep;
        if ((nrl_vcns(&fs->mft_runs) << fs->cshift) < fs->mft_init) return E_IO;
    }
    fs->mft_nrec = fs->mft_init >> fs->rec_shift;
    if (fs->mft_nrec < NTFS_FILE_FIRST_USER) return E_IO;
    for (int i = 0; i < NT_FB_N; i++) fs->fb[i].valid = 0;
    return 0;
}

static void load_label(ntfs_t *fs, volume_t *v) {
    nino_t ino;
    v->label[0] = 0;
    if (nt_ino_open(fs, NTFS_FILE_VOLUME, 0, &ino)) return;
    u16 u[64];
    int n = nt_attr_read(&ino, NTFS_AT_VOLUME_NAME, NULL, 0, 0, sizeof u, u);
    nt_ino_close(&ino);
    if (n <= 0) return;
    n /= 2;
    int o = 0;
    for (int i = 0; i < n; i++) {
        u32 cp = u[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            i++;
        } else if ((cp >= 0xD800 && cp <= 0xDFFF) || cp < 0x20) {
            cp = '_';
        }
        char tmp[4];
        int l = utf8_put(cp, tmp);
        if (o + l > (int)sizeof v->label - 1) break;
        memcpy(v->label + o, tmp, l);
        o += l;
    }
    while (o && v->label[o - 1] == ' ') o--;
    v->label[o] = 0;
}

static int load_upcase(ntfs_t *fs) {
    nino_t ino;
    int r = nt_ino_open(fs, NTFS_FILE_UPCASE, 0, &ino);
    if (r) return r;
    ainfo_t ai;
    r = nt_attr_info(&ino, NTFS_AT_DATA, NULL, 0, &ai, NULL);
    if (!r && (ai.size < 2 * 128 || ai.size > 2 * 65536 || (ai.size & 1))) r = E_IO;
    if (!r && !(fs->upcase = kmalloc((size_t)ai.size))) r = E_NOMEM;
    if (!r) {
        int n = nt_attr_read(&ino, NTFS_AT_DATA, NULL, 0, 0, (u32)ai.size, fs->upcase);
        if (n != (int)ai.size) r = n < 0 ? n : E_IO;
    }
    nt_ino_close(&ino);
    if (!r) {
        fs->upcase_n = (u32)(ai.size / 2);
        for (u32 c = 'a'; c <= 'z'; c++) if (fs->upcase[c] != c - 32) r = E_IO;
    }
    if (r) {
        kfree(fs->upcase);
        fs->upcase = NULL;
        fs->upcase_n = 0;
    }
    return r;
}

/* raw (not fixed up) copies of the first records must match $MFTMirr */
static int mirror_ok(ntfs_t *fs) {
    u8 *a = kmalloc(fs->rec_size * 2u);
    if (!a) return 0;
    int ok = 1;
    for (u32 i = 0; i < fs->mirr_n && ok; i++) {
        u64 pos = (u64)i << fs->rec_shift;
        if (nt_runs_rw(fs, &fs->mft_runs, pos, fs->rec_size, a, 0) ||
            nt_runs_rw(fs, &fs->mirr_runs, pos, fs->rec_size, a + fs->rec_size, 0) ||
            memcmp(a, a + fs->rec_size, fs->rec_size))
            ok = 0;
    }
    kfree(a);
    return ok;
}

int ntfs_probe(blockdev_t *bd, volume_t *v) {
    u32 dss = bd->sector_size;
    if (dss < 256 || dss > 4096 || (dss & (dss - 1)) || bd->sector_count < 16) return E_NOTSUP;
    u8 *s = kmalloc(4096);
    if (!s) return E_NOMEM;
    int r = blk_read(bd, 0, MAX(1u, 512 / dss), s);
    if (r) { kfree(s); return r; }
    int backup = 0;
    if (!ntfs_boot_sane(s, dss)) {
        /* the backup boot sector: the last sector of the volume */
        u64 bytes = bd->sector_count * dss;
        int ok = 0;
        for (u32 ss = 512; ss <= 4096 && !ok; ss *= 2) {
            if (ss < dss) continue;
            u64 lba = (bytes - ss) / dss;
            if (blk_read(bd, lba, MAX(1u, 512 / dss), s)) continue;
            ok = ntfs_boot_sane(s, dss) && ((ntfs_boot_t *)s)->bytes_per_sector == ss;
        }
        if (!ok) { kfree(s); return E_INVAL; }
        kprintf("ntfs: %s: damaged boot sector, using the backup\n", bd->name);
        backup = 1;
    }
    ntfs_boot_t boot;
    memcpy(&boot, s, sizeof boot);
    kfree(s);

    ntfs_t *fs = kzalloc(sizeof *fs);
    if (!fs) return E_NOMEM;
    fs->bd = bd;
    fs->dss = dss;
    fs->ss = boot.bytes_per_sector;
    u32 spc = boot.sectors_per_cluster;
    fs->csize = spc >= 0xF4 ? (1u << (256 - spc)) * fs->ss : spc * fs->ss;
    while ((1u << fs->cshift) < fs->csize) fs->cshift++;
    fs->nclus = boot.total_sectors * fs->ss / fs->csize;
    fs->vol_bytes = (boot.total_sectors + 1) * fs->ss;
    u64 dev_bytes = bd->sector_count * dss;
    int truncated = 0;
    if (fs->vol_bytes > dev_bytes) {
        fs->vol_bytes = dev_bytes;
        if ((fs->nclus << fs->cshift) > dev_bytes) truncated = 1;
    }
    fs->rec_size = ntfs_record_bytes(boot.clusters_per_mft_record, fs->csize);
    while ((1u << fs->rec_shift) < fs->rec_size) fs->rec_shift++;
    fs->ib_size = ntfs_record_bytes(boot.clusters_per_index_block, fs->csize);
    fs->mft_lcn = boot.mft_lcn;
    fs->mirr_lcn = boot.mftmirr_lcn;
    fs->serial = boot.serial;
    fs->free_clus = ~0ull;
    fs->fb_bufsize = MAX(fs->rec_size, fs->ib_size);
    fs->fb_mem = kmalloc((size_t)NT_FB_N * fs->fb_bufsize);
    fs->pg_mem = kmalloc((size_t)NT_PG_N * NT_PG_SIZE);
    fs->bounce = kmalloc(NT_PG_SIZE);
    fs->scan = kmalloc(4096);
    if (!fs->fb_mem || !fs->pg_mem || !fs->bounce || !fs->scan) { r = E_NOMEM; goto fail; }
    for (int i = 0; i < NT_FB_N; i++) fs->fb[i].buf = fs->fb_mem + (size_t)i * fs->fb_bufsize;
    for (int i = 0; i < NT_PG_N; i++) fs->pg[i].buf = fs->pg_mem + (size_t)i * NT_PG_SIZE;
    int check = backup || truncated;
    if (truncated) kprintf("ntfs: %s: the volume is larger than the device\n", bd->name);

    /* $MFT (from the mirror if record 0 is damaged) */
    if ((r = load_mft(fs, fs->mft_lcn))) {
        kprintf("ntfs: %s: $MFT unreadable, trying $MFTMirr\n", bd->name);
        if (r == E_NOMEM || (r = load_mft(fs, fs->mirr_lcn))) goto bad;
        check = 1;
    }
    {
        nino_t ino;
        ainfo_t bi;
        if ((r = nt_ino_open(fs, NTFS_FILE_MFT, 0, &ino))) goto bad;
        r = nt_attr_info(&ino, NTFS_AT_BITMAP, NULL, 0, &bi, &fs->mftbmp_runs);
        nt_ino_close(&ino);
        if (r) goto bad;
        fs->mftbmp_resident = bi.resident;
        fs->mftbmp_size = bi.init;
        fs->mftbmp_alloc = bi.alloc;
        if (fs->mftbmp_size * 8 < fs->mft_nrec) check = 1;
    }
    {
        ainfo_t mi;
        if ((r = sys_runs(fs, NTFS_FILE_MFTMIRR, &fs->mirr_runs, &mi))) goto bad;
        u64 n = MIN(mi.size, nrl_vcns(&fs->mirr_runs) << fs->cshift) >> fs->rec_shift;
        u32 want = fs->csize <= 4 * fs->rec_size ? 4 : fs->csize / fs->rec_size;
        fs->mirr_n = (u32)MIN(n, (u64)want);
        if (fs->mirr_n < 4 || !mirror_ok(fs)) {
            kprintf("ntfs: %s: $MFTMirr does not match $MFT\n", bd->name);
            check = 1;
        }
    }
    {
        ainfo_t bi;
        if ((r = sys_runs(fs, NTFS_FILE_BITMAP, &fs->bmp_runs, &bi))) goto bad;
        fs->bmp_size = bi.size;
        if (bi.size < (fs->nclus + 7) / 8 || bi.init < (fs->nclus + 7) / 8) {
            kprintf("ntfs: %s: $Bitmap too small\n", bd->name);
            check = 1;
            if ((nrl_vcns(&fs->bmp_runs) << fs->cshift) < (fs->nclus + 7) / 8)
                fs->nclus = (nrl_vcns(&fs->bmp_runs) << fs->cshift) * 8;
        }
    }
    {
        nino_t ino;
        if ((r = nt_ino_open(fs, NTFS_FILE_VOLUME, 0, &ino))) goto bad;
        ntfs_volume_info_t vi;
        int n = nt_attr_read(&ino, NTFS_AT_VOLUME_INFORMATION, NULL, 0, 0, sizeof vi, &vi);
        nt_ino_close(&ino);
        if (n != (int)sizeof vi) { r = E_IO; goto bad; }
        fs->ver_major = vi.major;
        fs->ver_minor = vi.minor;
        fs->vol_flags = vi.flags;
    }
    if (load_upcase(fs)) {
        kprintf("ntfs: %s: $UpCase damaged, using the built-in table\n", bd->name);
        check = 1;
    }
    {
        /* the root directory must be readable */
        nino_t ino;
        nidx_t ix;
        if ((r = nt_ino_open(fs, NTFS_FILE_ROOT, 0, &ino))) goto bad;
        r = nt_idx_open(&ino, nt_I30, 4, nt_coll_filename, &ix);
        if (!r) nt_idx_close(&ix);
        nsi_t si;
        if (!r && !si_get(&ino, &si)) fs->sec_id = si.secid;
        nt_ino_close(&ino);
        if (r) goto bad;
    }
    fs->mft_zone_start = fs->mft_lcn;
    fs->mft_zone_end = MIN(fs->nclus, fs->mft_lcn + fs->nclus / 8);
    fs->alloc_hint = fs->mft_zone_end;
    fs->mft_hint = NT_FIRST_FREE;
    fs->v = v;
    v->serial = fs->serial;
    load_label(fs, v);

    /* the safety rules: the first reason found is the one shown */
    if (bd->readonly) set_ro(fs, NT_RO_DEVICE);
    if (check || fs->corrupt || (fs->vol_flags & (NTFS_VOL_DIRTY | NTFS_VOL_CHKDSK_UNDERWAY | NTFS_VOL_REPAIR_OBJECT_ID |
                                                  NTFS_VOL_DELETE_USN_UNDERWAY))) {
        if (fs->vol_flags & NTFS_VOL_DIRTY) kprintf("ntfs: %s: the volume is marked dirty\n", bd->name);
        set_ro(fs, NT_RO_CHECK);
    }
    if (!logfile_clean(fs)) {
        kprintf("ntfs: %s: $LogFile is not clean\n", bd->name);
        set_ro(fs, NT_RO_HIBER);
    }
    if (hibernated(fs)) {
        kprintf("ntfs: %s: hiberfil.sys holds a hibernated Windows\n", bd->name);
        set_ro(fs, NT_RO_HIBER);
    }
    if (fs->ver_major != 3 || fs->ver_minor > 1 || (fs->vol_flags & (NTFS_VOL_UPGRADE_ON_MOUNT | NTFS_VOL_RESIZE_LOG_FILE)) ||
        fs->mftbmp_resident || fs->ib_size < 2048 || fs->rec_size < 1024 || !fs->upcase)
        set_ro(fs, NT_RO_LAYOUT);
    if (!bd->removable && has_windows(fs) && !internal_allowed(fs)) set_ro(fs, NT_RO_INTERNAL);
    if (fs->corrupt) set_ro(fs, NT_RO_CHECK);
    v->readonly = fs->ro != NT_RO_NONE;
    strlcpy(v->ro_reason, ro_text[fs->ro], sizeof v->ro_reason);

    v->ops = &ntfs_ops;
    v->fs_priv = fs;
    strlcpy(v->fs_name, "NTFS", sizeof v->fs_name);
    kprintf("ntfs: %s: NTFS %u.%u, %lu clusters of %u bytes, %u-byte records, label '%s'%s%s\n", bd->name,
            fs->ver_major, fs->ver_minor, (unsigned long)fs->nclus, fs->csize, fs->rec_size, v->label,
            fs->ro ? ", read-only: " : "", fs->ro ? ro_text[fs->ro] : "");
    return 0;
bad:
    if (r != E_NOMEM) {
        kprintf("ntfs: %s: cannot mount (%d)\n", bd->name, r);
        r = E_INVAL;
    }
fail:
    v->readonly = 0;
    v->ro_reason[0] = 0;
    fs_free(fs);
    return r;
}
