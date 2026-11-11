/* Halcyon OS - virtual file system: drive letters, paths, file handles */
#include "core/types.h"
#include "core/lib.h"
#include "core/unicode.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "fs/block.h"
#include "fs/vfs.h"
#include "fs/fat.h"

#define NLETTERS 26
#define COPY_BUF (64 * 1024)

struct vfs_file {
    volume_t *v;
    void *fh;
    u64 pos;
    int flags;
};

/* file system drivers, probed in order */
static const fs_probe_fn g_probes[] = {
    fat_probe,
};

static volume_t *g_vol[NLETTERS];   /* indexed by letter - 'A' */
static u32 g_gen;

void vfs_init(void) {
    memset(g_vol, 0, sizeof g_vol);
    g_gen = 1;
}

u32 vfs_generation(void) { return g_gen; }

/* ---- volumes ---- */

static int is_boot_volume(volume_t *v) {
    blockdev_t *bd = v->bd;
    if (g_bootinfo.boot_disk_sig) {
        if (bd->type == BDEV_PART && bd->parent)
            return bd->parent->disk_sig == g_bootinfo.boot_disk_sig &&
                   bd->part_start == g_bootinfo.boot_part_lba;
        return bd->disk_sig == g_bootinfo.boot_disk_sig && g_bootinfo.boot_part_lba == 0;
    }
    return !strcasecmp(v->label, "HALCYON");
}

static void volume_release(volume_t *v) {
    if (!v->alive && !v->open_files) kfree(v);
}

int vfs_try_mount(blockdev_t *bd) {
    if (!bd || !bd->present) return E_NODEV;
    for (int i = 0; i < NLETTERS; i++)
        if (g_vol[i] && g_vol[i]->bd == bd) return E_EXIST;
    volume_t *v = kzalloc(sizeof *v);
    if (!v) return E_NOMEM;
    v->bd = bd;
    int r = E_NOTSUP;
    for (size_t i = 0; i < ARRAY_SIZE(g_probes); i++) {
        r = g_probes[i](bd, v);
        if (!r) break;
    }
    if (r) {
        kfree(v);
        return r;
    }
    int slot = -1;
    if (!g_vol['C' - 'A'] && is_boot_volume(v)) {
        slot = 'C' - 'A';
        v->is_boot = 1;
    } else {
        for (int i = 'D' - 'A'; i < NLETTERS; i++) if (!g_vol[i]) { slot = i; break; }
    }
    if (slot < 0) {
        kprintf("vfs: %s: no free drive letter\n", bd->name);
        v->ops->unmount(v);
        kfree(v);
        return E_BUSY;
    }
    v->letter = (char)('A' + slot);
    v->alive = 1;
    g_vol[slot] = v;
    g_gen++;
    kprintf("vfs: %c: %s '%s' on %s%s\n", v->letter, v->fs_name, v->label, bd->name,
            v->is_boot ? " (boot volume)" : "");
    return 0;
}

static void unmount_slot(int i) {
    volume_t *v = g_vol[i];
    g_vol[i] = NULL;
    kprintf("vfs: %c: unmounted\n", v->letter);
    if (v->ops && v->ops->unmount) v->ops->unmount(v);
    v->alive = 0;
    v->bd = NULL;
    g_gen++;
    volume_release(v);
}

void vfs_unmount_blockdev(blockdev_t *bd) {
    for (int i = 0; i < NLETTERS; i++)
        if (g_vol[i] && g_vol[i]->bd == bd) unmount_slot(i);
}

int vfs_volume_count(void) {
    int n = 0;
    for (int i = 0; i < NLETTERS; i++) if (g_vol[i]) n++;
    return n;
}

volume_t *vfs_volume(int index) {
    for (int i = 0; i < NLETTERS; i++)
        if (g_vol[i] && index-- == 0) return g_vol[i];
    return NULL;
}

volume_t *vfs_volume_by_letter(char letter) {
    int c = toupper((u8)letter);
    if (c < 'A' || c > 'Z') return NULL;
    return g_vol[c - 'A'];
}

int vfs_statfs(char letter, u64 *total, u64 *free_bytes) {
    volume_t *v = vfs_volume_by_letter(letter);
    if (!v) return E_NODEV;
    if (!v->ops->statfs) return E_NOTSUP;
    return v->ops->statfs(v, total, free_bytes);
}

/* ---- paths ---- */

int vfs_normalize(const char *in, char *out, size_t outsz) {
    if (!in || !out || outsz < 4) return E_INVAL;
    if (!isalpha((u8)in[0]) || in[1] != ':') return E_INVAL;
    size_t o = 0;
    out[o++] = (char)toupper((u8)in[0]);
    out[o++] = ':';
    const char *s = in + 2;
    for (;;) {
        while (*s == '/' || *s == '\\') s++;
        if (!*s) break;
        const char *e = s;
        while (*e && *e != '/' && *e != '\\') e++;
        size_t len = (size_t)(e - s);
        if (len == 1 && s[0] == '.') {
            /* nothing */
        } else if (len == 2 && s[0] == '.' && s[1] == '.') {
            while (o > 2 && out[o - 1] != '/') o--;
            if (o > 2) o--;            /* drop the '/' too */
        } else {
            if (len > VFS_MAX_NAME) return E_INVAL;
            if (o + 1 + len + 1 > outsz || o + 1 + len > VFS_MAX_PATH) return E_INVAL;
            out[o++] = '/';
            memcpy(out + o, s, len);
            o += len;
        }
        s = e;
    }
    if (o == 2) out[o++] = '/';
    out[o] = 0;
    return 0;
}

void vfs_join(const char *dir, const char *name, char *out, size_t outsz) {
    if (!outsz) return;
    if (name && isalpha((u8)name[0]) && name[1] == ':') {
        strlcpy(out, name, outsz);
        return;
    }
    strlcpy(out, dir ? dir : "", outsz);
    size_t n = strlen(out);
    while (name && (*name == '/' || *name == '\\')) name++;
    if (n && out[n - 1] != '/' && out[n - 1] != '\\') strlcat(out, "/", outsz);
    if (name) strlcat(out, name, outsz);
}

const char *vfs_basename(const char *path) {
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\' || (*p == ':' && p == path + 1)) b = p + 1;
    return b;
}

void vfs_dirname(const char *path, char *out, size_t outsz) {
    char tmp[VFS_MAX_PATH + 1];
    if (!outsz) return;
    if (vfs_normalize(path, tmp, sizeof tmp)) {
        strlcpy(out, path, outsz);
        return;
    }
    char *sl = strrchr(tmp, '/');
    if (sl == tmp + 2) sl[1] = 0;       /* "C:/x" -> "C:/" ; "C:/" stays */
    else *sl = 0;
    strlcpy(out, tmp, outsz);
}

/* normalise and split into volume + driver path ("" = root) */
static int resolve(const char *path, volume_t **vp, char *rel, size_t relsz) {
    char norm[VFS_MAX_PATH + 1];
    if (!path) return E_INVAL;
    int r = vfs_normalize(path, norm, sizeof norm);
    if (r) return r;
    volume_t *v = vfs_volume_by_letter(norm[0]);
    if (!v || !v->alive) return E_NODEV;
    strlcpy(rel, norm + 3, relsz);
    *vp = v;
    return 0;
}

/* ---- files ---- */

int vfs_open(const char *path, int flags, vfs_file_t **out) {
    char rel[VFS_MAX_PATH + 1];
    volume_t *v;
    if (!out) return E_INVAL;
    *out = NULL;
    if (!(flags & VFS_O_RDWR)) return E_INVAL;
    int r = resolve(path, &v, rel, sizeof rel);
    if (r) return r;
    if (!rel[0]) return E_ISDIR;
    vfs_file_t *f = kzalloc(sizeof *f);
    if (!f) return E_NOMEM;
    r = v->ops->open(v, rel, flags, &f->fh);
    if (r) { kfree(f); return r; }
    f->v = v;
    f->flags = flags;
    v->open_files++;
    *out = f;
    return 0;
}

static int file_ok(vfs_file_t *f) {
    if (!f || !f->v) return E_INVAL;
    if (!f->v->alive) return E_NODEV;
    return 0;
}

int vfs_read(vfs_file_t *f, void *buf, u32 len) {
    int r = file_ok(f);
    if (r) return r;
    if (!(f->flags & VFS_O_READ)) return E_INVAL;
    if (len > 0x7FFFFFFF) len = 0x7FFFFFFF;
    r = f->v->ops->read(f->v, f->fh, f->pos, buf, len);
    if (r > 0) f->pos += (u32)r;
    return r;
}

int vfs_write(vfs_file_t *f, const void *buf, u32 len) {
    int r = file_ok(f);
    if (r) return r;
    if (!(f->flags & VFS_O_WRITE)) return E_INVAL;
    if (len > 0x7FFFFFFF) len = 0x7FFFFFFF;
    if (f->flags & VFS_O_APPEND) f->pos = f->v->ops->size(f->v, f->fh);
    r = f->v->ops->write(f->v, f->fh, f->pos, buf, len);
    if (r > 0) f->pos += (u32)r;
    return r;
}

int vfs_seek(vfs_file_t *f, u64 pos) {
    int r = file_ok(f);
    if (r) return r;
    f->pos = pos;
    return 0;
}

u64 vfs_tell(vfs_file_t *f) { return f ? f->pos : 0; }

u64 vfs_size(vfs_file_t *f) {
    if (file_ok(f)) return 0;
    return f->v->ops->size(f->v, f->fh);
}

int vfs_truncate(vfs_file_t *f, u64 size) {
    int r = file_ok(f);
    if (r) return r;
    if (!(f->flags & VFS_O_WRITE)) return E_INVAL;
    return f->v->ops->truncate(f->v, f->fh, size);
}

int vfs_close(vfs_file_t *f) {
    if (!f || !f->v) return E_INVAL;
    volume_t *v = f->v;
    int r = 0;
    if (v->alive) r = v->ops->close(v, f->fh);   /* dead volume: driver already freed it */
    v->open_files--;
    volume_release(v);
    kfree(f);
    return r;
}

/* ---- directories ---- */

typedef struct {
    vfs_dirent_t *arr;
    int n, cap;
    int err;
} list_ctx_t;

static int list_cb(void *vctx, const vfs_dirent_t *e) {
    list_ctx_t *c = vctx;
    if (c->n == c->cap) {
        int ncap = c->cap ? c->cap * 2 : 16;
        vfs_dirent_t *na = krealloc(c->arr, (size_t)ncap * sizeof *na);
        if (!na) { c->err = E_NOMEM; return 1; }
        c->arr = na;
        c->cap = ncap;
    }
    c->arr[c->n++] = *e;
    return 0;
}

int vfs_list(const char *path, vfs_dirent_t **out) {
    char rel[VFS_MAX_PATH + 1];
    volume_t *v;
    if (!out) return E_INVAL;
    *out = NULL;
    int r = resolve(path, &v, rel, sizeof rel);
    if (r) return r;
    list_ctx_t c = { NULL, 0, 0, 0 };
    r = v->ops->readdir(v, rel, list_cb, &c);
    if (!r) r = c.err;
    if (r) { kfree(c.arr); return r; }
    if (!c.arr) c.arr = kmalloc(sizeof(vfs_dirent_t));   /* non-NULL even if empty */
    *out = c.arr;
    return c.n;
}

int vfs_stat(const char *path, vfs_dirent_t *out) {
    char rel[VFS_MAX_PATH + 1];
    volume_t *v;
    int r = resolve(path, &v, rel, sizeof rel);
    if (r) return r;
    r = v->ops->stat(v, rel, out);
    if (!r && !rel[0]) {
        out->name[0] = v->letter;
        out->name[1] = ':';
        out->name[2] = 0;
    }
    return r;
}

int vfs_mkdir(const char *path) {
    char rel[VFS_MAX_PATH + 1];
    volume_t *v;
    int r = resolve(path, &v, rel, sizeof rel);
    if (r) return r;
    if (!rel[0]) return E_EXIST;
    return v->ops->mkdir(v, rel);
}

int vfs_unlink(const char *path) {
    char rel[VFS_MAX_PATH + 1];
    volume_t *v;
    int r = resolve(path, &v, rel, sizeof rel);
    if (r) return r;
    if (!rel[0]) return E_INVAL;
    return v->ops->unlink(v, rel);
}

int vfs_rename(const char *from, const char *to) {
    char rf[VFS_MAX_PATH + 1], rt[VFS_MAX_PATH + 1];
    volume_t *vf, *vt;
    int r = resolve(from, &vf, rf, sizeof rf);
    if (r) return r;
    if ((r = resolve(to, &vt, rt, sizeof rt))) return r;
    if (vf != vt) return E_NOTSUP;
    if (!rf[0] || !rt[0]) return E_INVAL;
    size_t lf = strlen(rf);
    if (!strncasecmp(rf, rt, lf) && rt[lf] == '/') return E_INVAL;   /* into itself */
    return vf->ops->rename(vf, rf, rt);
}

int vfs_copy(const char *from, const char *to) {
    vfs_dirent_t *st = kmalloc(sizeof *st);
    if (!st) return E_NOMEM;
    int r = vfs_stat(from, st);
    int isdir = st->is_dir;
    kfree(st);
    if (r) return r;
    if (isdir) return E_ISDIR;
    char nf[VFS_MAX_PATH + 1], nt[VFS_MAX_PATH + 1];
    if (vfs_normalize(from, nf, sizeof nf) || vfs_normalize(to, nt, sizeof nt)) return E_INVAL;
    if (!utf8_casecmp(nf, nt)) return E_EXIST;
    vfs_file_t *in = NULL, *out = NULL;
    u8 *buf = kmalloc(COPY_BUF);
    if (!buf) return E_NOMEM;
    if ((r = vfs_open(from, VFS_O_READ, &in))) goto done;
    if ((r = vfs_open(to, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC, &out))) goto done;
    for (;;) {
        int n = vfs_read(in, buf, COPY_BUF);
        if (n < 0) { r = n; break; }
        if (n == 0) break;
        int w = vfs_write(out, buf, (u32)n);
        if (w < 0) { r = w; break; }
        if (w != n) { r = E_NOSPC; break; }
    }
done:
    if (in) vfs_close(in);
    if (out) {
        int rc = vfs_close(out);
        if (!r) r = rc;
        if (r) vfs_unlink(to);          /* don't leave a partial copy behind */
    }
    kfree(buf);
    return r;
}

int vfs_sync_all(void) {
    int err = 0;
    for (int i = 0; i < NLETTERS; i++) {
        volume_t *v = g_vol[i];
        if (v && v->alive && v->ops->sync) {
            int r = v->ops->sync(v);
            if (r && !err) err = r;
        }
    }
    return err;
}

/* ---- convenience ---- */

int vfs_read_file(const char *path, char **data, u32 *size) {
    vfs_file_t *f;
    if (!data) return E_INVAL;
    *data = NULL;
    int r = vfs_open(path, VFS_O_READ, &f);
    if (r) return r;
    u64 sz = vfs_size(f);
    if (sz > 0x7FFFFFFE) { vfs_close(f); return E_NOMEM; }
    char *buf = kmalloc((size_t)sz + 1);
    if (!buf) { vfs_close(f); return E_NOMEM; }
    u32 got = 0;
    while (got < sz) {
        int n = vfs_read(f, buf + got, (u32)sz - got);
        if (n < 0) { kfree(buf); vfs_close(f); return n; }
        if (n == 0) break;
        got += (u32)n;
    }
    vfs_close(f);
    buf[got] = 0;
    *data = buf;
    if (size) *size = got;
    return (int)got;
}

int vfs_write_file(const char *path, const void *data, u32 size) {
    vfs_file_t *f;
    int r = vfs_open(path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC, &f);
    if (r) return r;
    u32 done = 0;
    while (done < size) {
        int n = vfs_write(f, (const u8 *)data + done, size - done);
        if (n < 0) { r = n; break; }
        if (n == 0) { r = E_NOSPC; break; }
        done += (u32)n;
    }
    int rc = vfs_close(f);
    if (!r) r = rc;
    return r ? r : (int)done;
}
