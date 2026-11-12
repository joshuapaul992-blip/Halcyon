/* Halcyon OS - the Recycle Bin: per-drive folders of deleted items and their
 * index (see recycle.h for the format). Only the VFS is used, so the host
 * tests (tests/host/recycletest.c) run this file unmodified. */
#include "recycle.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/thread.h"
#include "core/settings.h"

#define RB_HEADER "HALCYON RECYCLE BIN 1"
#define RB_MAX_ENTRIES 65536

static mutex_t rb_lock;            /* index read-modify-write (programs delete in parallel) */
static volatile u32 rb_gen = 1;

u32 rb_generation(void) { return rb_gen; }

typedef struct rb_index {
    char letter;
    rb_entry_t *e;
    int n, cap;
    int dirty;                     /* recovered something: write it back */
} rb_index_t;

/* ---------------- paths ---------------- */

static void bin_dir(char letter, char *out, size_t n) { snprintf(out, n, "%c:/" RB_DIR, letter); }
static void bin_file(char letter, const char *name, char *out, size_t n) { snprintf(out, n, "%c:/" RB_DIR "/%s", letter, name); }

int rb_is_bin_path(const char *path) {
    char norm[VFS_MAX_PATH];
    if (vfs_normalize(path, norm, sizeof norm)) return 0;
    size_t l = strlen(RB_DIR);
    return !strncasecmp(norm + 3, RB_DIR, l) && (norm[3 + l] == 0 || norm[3 + l] == '/');
}

/* the original extension, if it is a short, plain one */
static const char *ext_of(const char *orig) {
    const char *b = vfs_basename(orig);
    const char *d = strrchr(b, '.');
    if (!d || d == b || strlen(d) > 16) return "";
    for (const char *p = d + 1; *p; p++)
        if ((u8)*p < 0x21 || strchr("\\/:*?\"<>|%.", *p)) return "";
    return d;
}

void rb_stored_name(const rb_entry_t *e, char *out, size_t n) {
    snprintf(out, n, "D%u%s", e->id, e->is_dir ? "" : ext_of(e->orig));
}

/* "D<digits>[.ext]": an item this bin stored; its id, else 0 */
static u32 stored_id(const char *name) {
    if (name[0] != 'D' && name[0] != 'd') return 0;
    u32 id = 0;
    int i = 1;
    for (; isdigit(name[i]); i++) {
        if (id > 400000000u) return 0;
        id = id * 10 + (u32)(name[i] - '0');
    }
    if (i == 1 || (name[i] && name[i] != '.')) return 0;
    if (name[i] == '.' && (!name[i + 1] || strchr(name + i + 1, '.'))) return 0;
    return id;
}

/* ---------------- trees ---------------- */

static u64 tree_size(const char *path, int depth) {
    vfs_dirent_t st;
    if (vfs_stat(path, &st) < 0) return 0;
    if (!st.is_dir) return st.size;
    if (depth > 64) return 0;
    vfs_dirent_t *ents;
    int n = vfs_list(path, &ents);
    if (n < 0) return 0;
    char *child = kmalloc(VFS_MAX_PATH);
    u64 sum = 0;
    for (int i = 0; i < n && child; i++) {
        vfs_join(path, ents[i].name, child, VFS_MAX_PATH);
        sum += tree_size(child, depth + 1);
    }
    kfree(child);
    kfree(ents);
    return sum;
}
u64 rb_tree_size(const char *path) { return tree_size(path, 0); }

static int delete_tree(const char *path, int depth) {
    vfs_dirent_t st;
    int r = vfs_stat(path, &st);
    if (r < 0) return r;
    if (st.is_dir) {
        if (depth > 64) return E_INVAL;
        vfs_dirent_t *ents;
        int n = vfs_list(path, &ents);
        if (n < 0) return n;
        char *child = kmalloc(VFS_MAX_PATH);
        r = child ? 0 : E_NOMEM;
        for (int i = 0; i < n && !r; i++) {
            vfs_join(path, ents[i].name, child, VFS_MAX_PATH);
            r = delete_tree(child, depth + 1);
        }
        kfree(child);
        kfree(ents);
        if (r) return r;
    }
    return vfs_unlink(path);
}
int rb_delete_tree(const char *path) { return delete_tree(path, 0); }

/* mkdir -p for the parent folders of path */
static int make_parents(const char *path) {
    char dir[VFS_MAX_PATH];
    vfs_dirname(path, dir, sizeof dir);
    vfs_dirent_t st;
    if (strlen(dir) <= 3 || vfs_stat(dir, &st) == 0) return 0;     /* the root, or there */
    int r = make_parents(dir);
    if (r) return r;
    r = vfs_mkdir(dir);
    return r == E_EXIST ? 0 : r;
}

/* ---------------- the index ---------------- */

static int ix_add(rb_index_t *ix, const rb_entry_t *e) {
    if (ix->n >= RB_MAX_ENTRIES) return E_NOSPC;
    if (ix->n == ix->cap) {
        int cap = ix->cap ? ix->cap * 2 : 16;
        rb_entry_t *ne = krealloc(ix->e, sizeof *ne * (size_t)cap);
        if (!ne) return E_NOMEM;
        ix->e = ne;
        ix->cap = cap;
    }
    ix->e[ix->n++] = *e;
    return 0;
}

static void ix_remove(rb_index_t *ix, int i) {
    memmove(&ix->e[i], &ix->e[i + 1], sizeof *ix->e * (size_t)(ix->n - i - 1));
    ix->n--;
}

static int ix_find(const rb_index_t *ix, u32 id) {
    for (int i = 0; i < ix->n; i++) if (ix->e[i].id == id) return i;
    return -1;
}

static void ix_free(rb_index_t *ix) { kfree(ix->e); ix->e = NULL; ix->n = ix->cap = 0; }

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)toupper(c);
    return c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int num(const char **s, u64 *out, int maxdigits) {
    u64 v = 0;
    int k = 0;
    while (isdigit(**s) && k < maxdigits) { v = v * 10 + (u64)(**s - '0'); (*s)++; k++; }
    if (!k || isdigit(**s)) return -1;
    *out = v;
    return 0;
}

/* one index line; 0 if it is a valid entry of drive letter */
static int parse_line(const char *s, char letter, rb_entry_t *e) {
    memset(e, 0, sizeof *e);
    u64 v, y, mo, d, h, mi, se;
    if (num(&s, &v, 9) || !v || *s++ != '\t') return -1;
    e->id = (u32)v;
    if (num(&s, &y, 4) || *s++ != '-' || num(&s, &mo, 2) || *s++ != '-' || num(&s, &d, 2) || *s++ != ' ' ||
        num(&s, &h, 2) || *s++ != ':' || num(&s, &mi, 2) || *s++ != ':' || num(&s, &se, 2) || *s++ != '\t') return -1;
    if (y > 9999 || mo > 12 || d > 31 || h > 23 || mi > 59 || se > 59) return -1;
    e->when.year = (u16)y; e->when.month = (u8)mo; e->when.day = (u8)d;
    e->when.hour = (u8)h; e->when.minute = (u8)mi; e->when.second = (u8)se;
    if (num(&s, &v, 19) || *s++ != '\t') return -1;
    e->size = v;
    if ((*s != 'F' && *s != 'D') || s[1] != '\t') return -1;
    e->is_dir = *s == 'D';
    s += 2;
    size_t o = 0;
    for (; *s; s++) {
        char c = *s;
        if (c == '%') {
            int a = hexval(s[1]), b = a < 0 ? -1 : hexval(s[2]);
            if (b < 0) return -1;
            c = (char)(a * 16 + b);
            s += 2;
            if (!c) return -1;
        } else if ((u8)c < 0x20) return -1;
        if (o + 1 >= sizeof e->orig) return -1;
        e->orig[o++] = c;
    }
    e->orig[o] = 0;
    char norm[VFS_MAX_PATH];
    if (vfs_normalize(e->orig, norm, sizeof norm) || toupper(norm[0]) != toupper(letter) || strlen(norm) <= 3) return -1;
    strlcpy(e->orig, norm, sizeof e->orig);
    return 0;
}

/* parse a whole index file; *complete = it ends with its END line */
static int parse_index(char *data, char letter, rb_index_t *ix, int *complete) {
    *complete = 0;
    char *s = data;
    if ((u8)s[0] == 0xEF && (u8)s[1] == 0xBB && (u8)s[2] == 0xBF) s += 3;
    int first = 1, bad = 0;
    while (*s) {
        char *nl = strchr(s, '\n');
        if (nl) *nl = 0;
        size_t l = strlen(s);
        if (l && s[l - 1] == '\r') s[--l] = 0;
        if (first) {
            if (strcmp(s, RB_HEADER)) bad++;
            first = 0;
        } else if (!strncmp(s, "END ", 4)) {
            *complete = strtol(s + 4, NULL, 10) == ix->n;
            break;
        } else if (l) {
            rb_entry_t e;
            if (parse_line(s, letter, &e) || ix_find(ix, e.id) >= 0 || ix_add(ix, &e)) bad++;
        }
        if (!nl) break;
        s = nl + 1;
    }
    return bad;
}

/* the index of the drive, recovered against the folder's contents */
static int ix_load(char letter, rb_index_t *ix) {
    memset(ix, 0, sizeof *ix);
    ix->letter = (char)toupper(letter);
    char path[VFS_MAX_PATH], npath[VFS_MAX_PATH];
    bin_dir(ix->letter, path, sizeof path);
    vfs_dirent_t st;
    int r = vfs_stat(path, &st);
    if (r == E_NOENT) return 0;
    if (r) return r;
    if (!st.is_dir) return E_NOTDIR;
    /* the index: INDEX.NEW if it was written completely, else INDEX.TXT */
    bin_file(ix->letter, RB_INDEX, path, sizeof path);
    bin_file(ix->letter, RB_NEW, npath, sizeof npath);
    char *data = NULL;
    u32 size;
    int complete = 0, bad = 0, have = 0;
    if (vfs_read_file(npath, &data, &size) >= 0) {
        bad = parse_index(data, ix->letter, ix, &complete);
        kfree(data);
        if (complete && !bad) { have = 1; ix->dirty = 1; }      /* rename it into place */
        else ix_free(ix);
        vfs_unlink(npath);
    }
    if (!have && vfs_read_file(path, &data, &size) >= 0) {
        bad = parse_index(data, ix->letter, ix, &complete);
        kfree(data);
        if (bad || !complete) ix->dirty = 1;
    }
    /* entries whose item is gone are dropped */
    char name[64], item[VFS_MAX_PATH];
    for (int i = 0; i < ix->n;) {
        rb_stored_name(&ix->e[i], name, sizeof name);
        bin_file(ix->letter, name, item, sizeof item);
        if (vfs_stat(item, &st) < 0 || (st.is_dir != 0) != (ix->e[i].is_dir != 0)) { ix_remove(ix, i); ix->dirty = 1; }
        else i++;
    }
    /* items the index does not list are kept: they go back to the root */
    bin_dir(ix->letter, path, sizeof path);
    vfs_dirent_t *ents;
    int n = vfs_list(path, &ents);
    for (int i = 0; i < n; i++) {
        u32 id = stored_id(ents[i].name);
        if (!id || ix_find(ix, id) >= 0) continue;
        rb_entry_t e;
        memset(&e, 0, sizeof e);
        e.id = id;
        e.is_dir = ents[i].is_dir;
        e.when = ents[i].mtime;
        snprintf(e.orig, sizeof e.orig, "%c:/%s", ix->letter, ents[i].name);
        rb_entry_t probe = e;
        rb_stored_name(&probe, name, sizeof name);
        if (strcasecmp(name, ents[i].name)) continue;        /* "D5.a.b": not ours */
        bin_file(ix->letter, ents[i].name, item, sizeof item);
        e.size = rb_tree_size(item);
        if (!ix_add(ix, &e)) ix->dirty = 1;
    }
    if (n >= 0) kfree(ents);
    return 0;
}

static void esc_path(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (; *in && o + 4 < n; in++) {
        u8 c = (u8)*in;
        if (c < 0x20 || c == '%') o += (size_t)snprintf(out + o, n - o, "%%%02X", c);
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

/* write the index (atomically), or remove the folder when it is empty */
static int ix_save(rb_index_t *ix) {
    char path[VFS_MAX_PATH], npath[VFS_MAX_PATH], dir[VFS_MAX_PATH];
    bin_dir(ix->letter, dir, sizeof dir);
    bin_file(ix->letter, RB_INDEX, path, sizeof path);
    bin_file(ix->letter, RB_NEW, npath, sizeof npath);
    rb_gen++;
    if (!ix->n) {
        vfs_unlink(path);
        vfs_unlink(npath);
        vfs_dirent_t *ents;
        int n = vfs_list(dir, &ents);
        if (n == 0) vfs_unlink(dir);         /* (anything else left in it keeps the folder) */
        if (n >= 0) kfree(ents);
        ix->dirty = 0;
        return 0;
    }
    size_t cap = 64 + (size_t)ix->n * (VFS_MAX_PATH * 3 + 64);
    char *out = kmalloc(cap), *esc = kmalloc(VFS_MAX_PATH * 3 + 4);
    if (!out || !esc) { kfree(out); kfree(esc); return E_NOMEM; }
    size_t o = (size_t)snprintf(out, cap, RB_HEADER "\r\n");
    for (int i = 0; i < ix->n; i++) {
        rb_entry_t *e = &ix->e[i];
        esc_path(e->orig, esc, VFS_MAX_PATH * 3 + 4);
        o += (size_t)snprintf(out + o, cap - o, "%u\t%04u-%02u-%02u %02u:%02u:%02u\t%lu\t%c\t%s\r\n", e->id,
                              e->when.year, e->when.month, e->when.day, e->when.hour, e->when.minute, e->when.second,
                              (unsigned long)e->size, e->is_dir ? 'D' : 'F', esc);
    }
    o += (size_t)snprintf(out + o, cap - o, "END %d\r\n", ix->n);
    kfree(esc);
    int r = vfs_write_file(npath, out, (u32)o);
    kfree(out);
    if (r < 0) { vfs_unlink(npath); return r; }
    vfs_unlink(path);
    r = vfs_rename(npath, path);
    if (!r) ix->dirty = 0;
    return r;
}

static u32 next_id(const rb_index_t *ix) {
    u32 id = 0;
    for (int i = 0; i < ix->n; i++) if (ix->e[i].id > id) id = ix->e[i].id;
    return id + 1;
}

static void now(vfs_time_t *t) {
    struct rtc_time tm;
    rtc_read(&tm);
    t->year = (u16)tm.year; t->month = (u8)tm.month; t->day = (u8)tm.day;
    t->hour = (u8)tm.hour; t->minute = (u8)tm.minute; t->second = (u8)tm.second;
}

/* ---------------- policy ---------------- */

int rb_supported(char letter) {
    volume_t *v = vfs_volume_by_letter((char)toupper(letter));
    if (!v) return E_NODEV;
    if (v->readonly || (v->bd && v->bd->readonly) || !strcmp(v->fs_name, "ISO9660")) return E_ROFS;
    if (v->is_boot || !v->bd) return 0;      /* Halcyon's own drive keeps a bin, also on a USB stick */
    blockdev_t *d = v->bd->parent ? v->bd->parent : v->bd;
    if (d->type == BDEV_DISK_USB || d->type == BDEV_DISK_CD) return E_NOTSUP;
    if (!strcmp(v->fs_name, "FAT12") && d->sector_count <= 5760) return E_NOTSUP;    /* floppies */
    return 0;
}

void rb_percent_key(char letter, char *out, size_t n) {
    volume_t *v = vfs_volume_by_letter((char)toupper(letter));
    snprintf(out, n, "recycle.percent.%08X%08X", v ? (u32)(v->serial >> 32) : 0, v ? (u32)v->serial : 0);
}

int rb_percent(char letter) {
    char key[48];
    rb_percent_key(letter, key, sizeof key);
    int p = settings_get_int(key, settings_get_int("recycle.percent", RB_DEFAULT_PERCENT));
    return CLAMP(p, 0, 100);
}

u64 rb_limit(char letter) {
    u64 total = 0, freeb = 0;
    if (vfs_statfs((char)toupper(letter), &total, &freeb) < 0) return 0;
    return total / 100 * (u64)rb_percent(letter);
}

/* ---------------- operations ---------------- */

static int purge_at(rb_index_t *ix, int i) {
    char name[64], item[VFS_MAX_PATH];
    rb_stored_name(&ix->e[i], name, sizeof name);
    bin_file(ix->letter, name, item, sizeof item);
    int r = rb_delete_tree(item);
    if (r && r != E_NOENT) return r;
    ix_remove(ix, i);
    return 0;
}

static int enforce(rb_index_t *ix, u64 limit, u32 keep) {
    int purged = 0;
    for (;;) {
        u64 total = 0;
        for (int i = 0; i < ix->n; i++) total += ix->e[i].size;
        if (total <= limit) break;
        int victim = -1;      /* oldest first: by date, then by id (the index order) */
        for (int i = 0; i < ix->n; i++) {
            if (ix->e[i].id == keep) continue;
            if (victim < 0) { victim = i; continue; }
            const vfs_time_t *a = &ix->e[i].when, *b = &ix->e[victim].when;
            u64 ka = ((u64)a->year << 40) | ((u64)a->month << 32) | ((u64)a->day << 24) | ((u64)a->hour << 16) | ((u64)a->minute << 8) | a->second;
            u64 kb = ((u64)b->year << 40) | ((u64)b->month << 32) | ((u64)b->day << 24) | ((u64)b->hour << 16) | ((u64)b->minute << 8) | b->second;
            if (ka < kb || (ka == kb && ix->e[i].id < ix->e[victim].id)) victim = i;
        }
        if (victim < 0 || purge_at(ix, victim)) break;
        purged++;
    }
    return purged;
}

int rb_delete(const char *path, u64 limit) {
    char src[VFS_MAX_PATH];
    int r = vfs_normalize(path, src, sizeof src);
    if (r) return r;
    if (strlen(src) <= 3 || rb_is_bin_path(src)) return E_INVAL;
    char letter = src[0];
    if (!limit) limit = rb_limit(letter);
    vfs_dirent_t st;
    if ((r = vfs_stat(src, &st)) < 0) return r;
    mutex_lock(&rb_lock);
    u64 size = rb_tree_size(src);
    rb_index_t ix;
    if (size > limit) { r = E_NOSPC; goto out_unlocked; }
    if ((r = ix_load(letter, &ix)) < 0) goto out_unlocked;
    char dir[VFS_MAX_PATH], name[64], item[VFS_MAX_PATH];
    bin_dir(letter, dir, sizeof dir);
    r = vfs_mkdir(dir);
    if (r && r != E_EXIST) goto out;
    rb_entry_t e;
    memset(&e, 0, sizeof e);
    e.id = next_id(&ix);
    e.is_dir = st.is_dir;
    e.size = size;
    now(&e.when);
    strlcpy(e.orig, src, sizeof e.orig);
    for (int tries = 0;; tries++) {        /* a stray item of that name (not in the index) */
        rb_stored_name(&e, name, sizeof name);
        bin_file(letter, name, item, sizeof item);
        if (vfs_stat(item, &st) == E_NOENT || tries > 100) break;
        e.id++;
    }
    if ((r = ix_add(&ix, &e)) < 0) goto out;
    r = vfs_rename(src, item);
    if (r) { ix.n--; if (ix.dirty || !ix.n) ix_save(&ix); goto out; }
    enforce(&ix, limit, e.id);
    r = ix_save(&ix);
    if (r) {
        /* no index, no way back: put the item where it was */
        vfs_rename(item, src);
        goto out;
    }
    r = (int)e.id;
out:
    ix_free(&ix);
out_unlocked:
    mutex_unlock(&rb_lock);
    return r;
}

int rb_list(char letter, rb_entry_t **out) {
    *out = NULL;
    mutex_lock(&rb_lock);
    rb_index_t ix;
    int r = ix_load(letter, &ix);
    if (!r && ix.dirty) ix_save(&ix);
    if (!r) { *out = ix.e; r = ix.n; ix.e = NULL; }
    ix_free(&ix);
    mutex_unlock(&rb_lock);
    return r;
}

int rb_usage(char letter, int *count, u64 *bytes) {
    rb_entry_t *e;
    int n = rb_list(letter, &e);
    *count = 0;
    *bytes = 0;
    if (n < 0) return n;
    for (int i = 0; i < n; i++) *bytes += e[i].size;
    *count = n;
    kfree(e);
    return 0;
}

static void unique_name(const char *want, char *out, size_t n) {
    vfs_dirent_t st;
    strlcpy(out, want, n);
    if (vfs_stat(out, &st) < 0) return;
    char dir[VFS_MAX_PATH], base[VFS_MAX_NAME + 1], ext[VFS_MAX_NAME + 1] = "";
    vfs_dirname(want, dir, sizeof dir);
    strlcpy(base, vfs_basename(want), sizeof base);
    char *dot = strrchr(base, '.');
    if (dot && dot != base) { strlcpy(ext, dot, sizeof ext); *dot = 0; }
    for (int i = 2; i < 10000; i++) {
        char cand[VFS_MAX_NAME + 24];
        snprintf(cand, sizeof cand, "%s (%d)%s", base, i, ext);
        vfs_join(dir, cand, out, n);
        if (vfs_stat(out, &st) < 0) return;
    }
}

int rb_restore(char letter, u32 id, int mode, char *out, size_t outsz) {
    mutex_lock(&rb_lock);
    rb_index_t ix;
    int r = ix_load(letter, &ix);
    if (r) goto done;
    int i = ix_find(&ix, id);
    if (i < 0) { r = E_NOENT; goto save; }
    rb_entry_t *e = &ix.e[i];
    char name[64], item[VFS_MAX_PATH], to[VFS_MAX_PATH];
    rb_stored_name(e, name, sizeof name);
    bin_file(ix.letter, name, item, sizeof item);
    strlcpy(to, e->orig, sizeof to);
    vfs_dirent_t st;
    if (vfs_stat(to, &st) == 0) {
        if (mode == RB_FAIL) { r = E_EXIST; goto save; }
        if (mode == RB_REPLACE) { if ((r = rb_delete_tree(to)) < 0) goto save; }
        else unique_name(e->orig, to, sizeof to);
    }
    if ((r = make_parents(to)) < 0) goto save;
    if ((r = vfs_rename(item, to)) < 0) goto save;
    if (out) strlcpy(out, to, outsz);
    ix_remove(&ix, i);
    ix.dirty = 1;
save:
    if (ix.dirty) { int s = ix_save(&ix); if (!r) r = s; }
done:
    ix_free(&ix);
    mutex_unlock(&rb_lock);
    return r;
}

int rb_purge(char letter, u32 id) {
    mutex_lock(&rb_lock);
    rb_index_t ix;
    int r = ix_load(letter, &ix);
    if (!r) {
        int i = ix_find(&ix, id);
        r = i < 0 ? E_NOENT : purge_at(&ix, i);
        if (!r) ix.dirty = 1;
        if (ix.dirty) { int s = ix_save(&ix); if (!r) r = s; }
    }
    ix_free(&ix);
    mutex_unlock(&rb_lock);
    return r;
}

int rb_empty(char letter, rb_tick_t tick, void *ctx) {
    mutex_lock(&rb_lock);
    rb_index_t ix;
    int r = ix_load(letter, &ix);
    while (!r && ix.n) {
        if (tick && tick(ctx, vfs_basename(ix.e[0].orig), ix.e[0].size)) { r = E_CANCELED; break; }
        r = purge_at(&ix, 0);
        ix.dirty = 1;
    }
    if (ix.dirty) { int s = ix_save(&ix); if (!r) r = s; }
    ix_free(&ix);
    mutex_unlock(&rb_lock);
    return r;
}

int rb_enforce(char letter, u64 limit, u32 keep_id) {
    mutex_lock(&rb_lock);
    rb_index_t ix;
    int r = ix_load(letter, &ix);
    if (!r) {
        r = enforce(&ix, limit, keep_id);
        if (r || ix.dirty) ix_save(&ix);
    }
    ix_free(&ix);
    mutex_unlock(&rb_lock);
    return r;
}
