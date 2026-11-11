/* Host tests for the Halcyon storage stack (kernel/fs/block.c, vfs.c, fat.c).
 *
 * The kernel sources are compiled unmodified against a small libc shim
 * (shim.c). Disk images are created with mkfs.fat / sfdisk / mtools, the
 * kernel code mounts them through a file-backed blockdev, and after every
 * scenario the result is verified with fsck.fat -n and mtools.
 *
 * ISO 9660 tests (scenario "iso") are in isotest.c.
 *
 * usage: fstest WORKDIR [-v] [scenario...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "core/types.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/unicode.h"
#include "fs/block.h"
#include "fs/vfs.h"
#include "fuzz_fs.h"

extern int shim_verbose;
extern size_t shim_live_allocs;

static const char *W;
static int g_fail, g_checks;

static const char *ename(int r) {
    static char buf[16];
    switch (r) {
    case E_IO: return "E_IO"; case E_NOMEM: return "E_NOMEM"; case E_NOENT: return "E_NOENT";
    case E_EXIST: return "E_EXIST"; case E_NOTDIR: return "E_NOTDIR"; case E_ISDIR: return "E_ISDIR";
    case E_NOSPC: return "E_NOSPC"; case E_INVAL: return "E_INVAL"; case E_NOTSUP: return "E_NOTSUP";
    case E_NOTEMPTY: return "E_NOTEMPTY"; case E_ROFS: return "E_ROFS"; case E_NODEV: return "E_NODEV";
    case E_BUSY: return "E_BUSY"; case E_NAMETOOLONG: return "E_NAMETOOLONG";
    }
    snprintf(buf, sizeof buf, "%d", r);
    return buf;
}

#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; \
    printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define EXPECT(expr, want) do { long _r = (long)(expr); long _w = (long)(want); \
    CHECK(_r == _w, "%s = %ld (%s), want %ld (%s)", #expr, _r, ename((int)_r), _w, ename((int)_w)); } while (0)
#define OK(expr) do { long _r = (long)(expr); CHECK(_r >= 0, "%s = %ld (%s)", #expr, _r, ename((int)_r)); } while (0)

/* ------------------------------------------------------------------ */
/* shell helpers                                                        */

static int sh(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int g_sh_keep_stdout;
static int sh(const char *fmt, ...) {
    char cmd[8192];
    va_list ap;
    va_start(ap, fmt);
    cmd[0] = '(';
    vsnprintf(cmd + 1, sizeof cmd - 1, fmt, ap);
    va_end(ap);
    strlcat(cmd, ")", sizeof cmd);
    if (g_sh_keep_stdout) strlcat(cmd, " 2>/dev/null", sizeof cmd);
    else if (!shim_verbose) strlcat(cmd, " >/dev/null 2>&1", sizeof cmd);
    else printf("    $ %s\n", cmd);
    int r = system(cmd);
    return WIFEXITED(r) ? WEXITSTATUS(r) : -1;
}

/* single-quote s for the shell */
static const char *q(const char *s) {
    static char bufs[8][2048];
    static int i;
    char *b = bufs[i++ & 7], *o = b;
    *o++ = '\'';
    for (; *s && o < b + 2040; s++) {
        if (*s == '\'') { memcpy(o, "'\\''", 4); o += 4; }
        else *o++ = *s;
    }
    *o++ = '\'';
    *o = 0;
    return b;
}

/* mtools path; esc: escape mtools wildcards (source names are globbed) */
static const char *mpx(const char *path, int esc) {
    static char bufs[4][1100];
    static int i;
    char *b = bufs[i++ & 3], *o = b;
    o += snprintf(b, 1100, "::/");
    for (; *path && o < b + 1090; path++) {
        if (esc && strchr("[]*?\\", *path)) *o++ = '\\';
        *o++ = *path;
    }
    *o = 0;
    return q(b);
}
#define mp(p) mpx(p, 1)

static const char *wpath(const char *name) {
    static char bufs[8][1024];
    static int i;
    char *b = bufs[i++ & 7];
    snprintf(b, 1024, "%s/%s", W, name);
    return b;
}

/* ------------------------------------------------------------------ */
/* data                                                                 */

static void pattern(u8 *b, size_t n, u32 seed) {
    u32 x = seed * 2654435761u + 12345;
    for (size_t i = 0; i < n; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        b[i] = (u8)x;
    }
}

static u8 *pat_alloc(size_t n, u32 seed) {
    u8 *b = malloc(n ? n : 1);
    pattern(b, n, seed);
    return b;
}

static int host_write(const char *path, const void *d, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(d, 1, n, f);
    fclose(f);
    return 0;
}

static u8 *host_read(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *b = malloc(sz + 1);
    *n = fread(b, 1, sz, f);
    fclose(f);
    return b;
}

/* ------------------------------------------------------------------ */
/* images                                                               */

typedef struct {
    char path[1024];
    u64 off;            /* byte offset of the file system */
    u64 len;            /* byte length of the file system */
} img_t;

static const char *mt(const img_t *im) {
    static char bufs[4][1200];
    static int i;
    char *b = bufs[i++ & 3];
    if (im->off) snprintf(b, 1200, "'%s@@%lu'", im->path, (unsigned long)im->off);
    else snprintf(b, 1200, "'%s'", im->path);
    return b;
}

static void img_super(img_t *im, const char *name, int fat, u32 kb, const char *label, const char *extra) {
    snprintf(im->path, sizeof im->path, "%s", wpath(name));
    im->off = 0;
    im->len = (u64)kb * 1024;
    unlink(im->path);
    int r = sh("mkfs.fat -C %s -F %d -n %s '%s' %u", extra ? extra : "", fat, label, im->path, kb);
    CHECK(r == 0, "mkfs.fat failed for %s", name);
}

static void img_mbr(img_t *im, const char *name, int fat, u32 mb, const char *label, u32 disk_sig) {
    snprintf(im->path, sizeof im->path, "%s", wpath(name));
    u32 total = mb * 2048, start = 2048, size = total - start;
    unlink(im->path);
    int r = sh("truncate -s %uM '%s' && printf 'label: dos\\nlabel-id: 0x%08x\\nstart=%u, size=%u, type=%s\\n'"
               " | sfdisk -q '%s' && mkfs.fat --offset=%u -F %d %s -n %s '%s' %u",
               mb, im->path, disk_sig, start, size, fat == 32 ? "c" : fat == 16 ? "6" : "1", im->path,
               start, fat, fat == 32 ? "-s 1" : "", label, im->path, size / 2);
    CHECK(r == 0, "image creation failed for %s", name);
    im->off = (u64)start * 512;
    im->len = (u64)size * 512;
}

static void img_gpt(img_t *im, const char *name, u32 mb, const char *label) {
    snprintf(im->path, sizeof im->path, "%s", wpath(name));
    u32 total = mb * 2048, start = 2048, size = total - start - 2048;
    unlink(im->path);
    int r = sh("truncate -s %uM '%s' && printf 'label: gpt\\nstart=%u, size=%u, "
               "type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, name=data\\n' | sfdisk -q '%s'"
               " && mkfs.fat --offset=%u -F 32 -s 1 -n %s '%s' %u",
               mb, im->path, start, size, im->path, start, label, im->path, size / 2);
    CHECK(r == 0, "image creation failed for %s", name);
    im->off = (u64)start * 512;
    im->len = (u64)size * 512;
}

/* fsck.fat -n on the file system of im (extracted if partitioned) */
static void fsck(const img_t *im, const char *what) {
    char target[1100];
    if (im->off) {
        snprintf(target, sizeof target, "%s", wpath("fsck.part"));
        sh("dd if='%s' of='%s' bs=512 skip=%lu count=%lu", im->path, target,
           (unsigned long)(im->off / 512), (unsigned long)(im->len / 512));
    } else {
        snprintf(target, sizeof target, "%s", im->path);
    }
    int r = sh("fsck.fat -n '%s'", target);
    CHECK(r == 0, "fsck.fat reports errors after '%s'", what);
    if (r) {
        char cmd[2400];
        snprintf(cmd, sizeof cmd, "fsck.fat -n -v '%s' 2>&1 | head -40", target);
        system(cmd);
    }
    if (im->off) unlink(target);
}

/* copy a file out with mtools and compare */
static void mt_cmp(const img_t *im, const char *mpath, const u8 *data, size_t n) {
    const char *out = wpath("mt.out");
    unlink(out);
    int r = sh("mcopy -n -o -i %s %s '%s'", mt(im), mp(mpath), out);
    CHECK(r == 0, "mcopy of '%s' failed", mpath);
    if (r) return;
    size_t got;
    u8 *b = host_read(out, &got);
    CHECK(b && got == n, "mtools size of '%s': %zu, want %zu", mpath, b ? got : 0, n);
    if (b && got == n) CHECK(!memcmp(b, data, n), "mtools content of '%s' differs", mpath);
    free(b);
}

static void mt_put(const img_t *im, const char *mpath, const u8 *data, size_t n) {
    const char *in = wpath("mt.in");
    host_write(in, data, n);
    int r = sh("mcopy -o -i %s '%s' %s", mt(im), in, mpx(mpath, 0));
    CHECK(r == 0, "mcopy to '%s' failed", mpath);
}

/* does mdir -b list exactly this (long) path? */
static int mt_listed(const img_t *im, const char *mdirpath, const char *fullname) {
    const char *out = wpath("mdir.out");
    g_sh_keep_stdout = 1;
    sh("mdir -b -i %s %s > '%s'", mt(im), mp(mdirpath), out);
    g_sh_keep_stdout = 0;
    size_t n;
    u8 *b = host_read(out, &n);
    if (!b) return 0;
    b[n] = 0;
    char want[1200], want2[1200];
    snprintf(want, sizeof want, "::/%s%s%s\n", mdirpath, *mdirpath ? "/" : "", fullname);
    snprintf(want2, sizeof want2, "::/%s%s%s/\n", mdirpath, *mdirpath ? "/" : "", fullname);
    int found = strstr((char *)b, want) || strstr((char *)b, want2);
    if (!found) printf("    mdir listing of '%s':\n%s", mdirpath, (char *)b);
    free(b);
    return found;
}

static u32 img_cluster_size(const img_t *im) {
    int fd = open(im->path, O_RDONLY);
    u8 bs[512];
    pread(fd, bs, 512, im->off);
    close(fd);
    return (u32)(bs[11] | (bs[12] << 8)) * bs[13];
}

/* ------------------------------------------------------------------ */
/* file-backed block device                                             */

typedef struct {
    blockdev_t bd;
    int fd;
    u64 reads, writes, rbytes, wbytes;
    u32 max_count;
} hdisk_t;

static int hd_read(blockdev_t *bd, u64 lba, u32 count, void *buf) {
    hdisk_t *h = (hdisk_t *)bd;
    size_t n = (size_t)count * bd->sector_size;
    h->reads++; h->rbytes += n;
    if (count > h->max_count) h->max_count = count;
    return pread(h->fd, buf, n, (off_t)(lba * bd->sector_size)) == (ssize_t)n ? 0 : E_IO;
}

static int hd_write(blockdev_t *bd, u64 lba, u32 count, const void *buf) {
    hdisk_t *h = (hdisk_t *)bd;
    size_t n = (size_t)count * bd->sector_size;
    h->writes++; h->wbytes += n;
    if (count > h->max_count) h->max_count = count;
    return pwrite(h->fd, buf, n, (off_t)(lba * bd->sector_size)) == (ssize_t)n ? 0 : E_IO;
}

static int g_disk_no;

static hdisk_t *disk_attach(const img_t *im, u32 ss) {
    hdisk_t *h = calloc(1, sizeof *h);
    h->fd = open(im->path, O_RDWR);
    struct stat st;
    fstat(h->fd, &st);
    snprintf(h->bd.name, sizeof h->bd.name, "hd%d", g_disk_no++);
    snprintf(h->bd.model, sizeof h->bd.model, "image");
    h->bd.type = BDEV_DISK_RAM;
    h->bd.sector_size = ss;
    h->bd.sector_count = (u64)st.st_size / ss;
    h->bd.removable = 1;
    h->bd.read = hd_read;
    h->bd.write = hd_write;
    int r = blockdev_register(&h->bd);
    CHECK(r == 0, "blockdev_register = %d", r);
    return h;
}

static void disk_detach(hdisk_t *h) {
    blockdev_unregister(&h->bd);
    CHECK(!h->bd.present, "present still set after unregister");
    close(h->fd);
    free(h);
}

static void disk_stats_reset(hdisk_t *h) { h->reads = h->writes = h->rbytes = h->wbytes = 0; h->max_count = 0; }

/* drive letter of the first volume on disk h (0 if none) */
static char letter_of(hdisk_t *h) {
    for (int i = 0; i < vfs_volume_count(); i++) {
        volume_t *v = vfs_volume(i);
        if (v->bd == &h->bd || (v->bd && v->bd->parent == &h->bd)) return v->letter;
    }
    return 0;
}

static const char *P(char letter, const char *rel) {
    static char bufs[8][VFS_MAX_PATH + 16];
    static int i;
    char *b = bufs[i++ & 7];
    snprintf(b, VFS_MAX_PATH + 16, "%c:/%s", letter, rel);
    return b;
}

/* ------------------------------------------------------------------ */
/* VFS helpers                                                          */

static int v_put(const char *path, const u8 *d, u32 n) {
    int r = vfs_write_file(path, d, n);
    CHECK(r == (int)n, "vfs_write_file(%s) = %d (%s)", path, r, ename(r));
    return r;
}

static void v_cmp(const char *path, const u8 *d, u32 n) {
    char *data = NULL;
    u32 sz = 0;
    int r = vfs_read_file(path, &data, &sz);
    CHECK(r >= 0, "vfs_read_file(%s) = %d (%s)", path, r, ename(r));
    if (r >= 0) {
        CHECK(sz == n, "size of %s = %u, want %u", path, sz, n);
        if (sz == n) CHECK(!memcmp(data, d, n), "content of %s differs", path);
        CHECK(data[sz] == 0, "not NUL terminated");
    }
    kfree(data);
}

static int v_exists(const char *path) {
    vfs_dirent_t st;
    return vfs_stat(path, &st) == 0;
}

static int v_count(const char *dir) {
    vfs_dirent_t *ents;
    int n = vfs_list(dir, &ents);
    if (n >= 0) kfree(ents);
    return n;
}

static int v_listed(const char *dir, const char *name, vfs_dirent_t *out) {
    vfs_dirent_t *ents;
    int n = vfs_list(dir, &ents), found = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(ents[i].name, name)) {
            found = 1;
            if (out) *out = ents[i];
        }
    }
    if (n >= 0) kfree(ents);
    return found;
}

static u64 v_free(char letter) {
    u64 t = 0, f = 0;
    int r = vfs_statfs(letter, &t, &f);
    CHECK(r == 0, "statfs = %d", r);
    return f;
}

/* ------------------------------------------------------------------ */
/* scenarios                                                            */

static void t_paths(void) {
    char out[VFS_MAX_PATH + 1];
    struct { const char *in, *out; } ok[] = {
        { "c:", "C:/" }, { "C:/", "C:/" }, { "c:\\", "C:/" }, { "c:\\a\\..\\B/./x", "C:/B/x" },
        { "D:/a/b/../../..", "D:/" }, { "e://x//y/", "E:/x/y" }, { "C:/Long Name/x.txt", "C:/Long Name/x.txt" },
        { "C:..", "C:/" }, { "z:/A/./B/../C", "Z:/A/C" },
    };
    for (size_t i = 0; i < ARRAY_SIZE(ok); i++) {
        int r = vfs_normalize(ok[i].in, out, sizeof out);
        CHECK(r == 0 && !strcmp(out, ok[i].out), "normalize(%s) = %d '%s', want '%s'", ok[i].in, r, out, ok[i].out);
    }
    const char *bad[] = { "", "C", "/foo", "foo/bar", "1:/x", ":x" };
    for (size_t i = 0; i < ARRAY_SIZE(bad); i++)
        CHECK(vfs_normalize(bad[i], out, sizeof out) < 0, "normalize(%s) should fail", bad[i]);
    char longp[400];
    memset(longp, 'a', sizeof longp);
    memcpy(longp, "C:/", 3);
    longp[399] = 0;
    CHECK(vfs_normalize(longp, out, sizeof out) < 0, "overlong path accepted");

    vfs_join("C:/A", "b.txt", out, sizeof out);
    CHECK(!strcmp(out, "C:/A/b.txt"), "join = %s", out);
    vfs_join("C:/", "b.txt", out, sizeof out);
    CHECK(!strcmp(out, "C:/b.txt"), "join = %s", out);
    vfs_join("C:/A", "D:/x", out, sizeof out);
    CHECK(!strcmp(out, "D:/x"), "join abs = %s", out);
    CHECK(!strcmp(vfs_basename("C:/A/b.txt"), "b.txt"), "basename");
    CHECK(!strcmp(vfs_basename("C:/"), ""), "basename root");
    CHECK(!strcmp(vfs_basename("C:\\x\\y"), "y"), "basename backslash");
    vfs_dirname("C:/A/b.txt", out, sizeof out);
    CHECK(!strcmp(out, "C:/A"), "dirname = %s", out);
    vfs_dirname("C:/A", out, sizeof out);
    CHECK(!strcmp(out, "C:/"), "dirname = %s", out);
    vfs_dirname("C:/", out, sizeof out);
    CHECK(!strcmp(out, "C:/"), "dirname = %s", out);
    vfs_file_t *f;
    EXPECT(vfs_open("Q:/nothing", VFS_O_READ, &f), E_NODEV);
    EXPECT(vfs_open("nothing", VFS_O_READ, &f), E_INVAL);
}

/* files placed with mtools, read back through the kernel driver */
static void t_read_mtools(const char *tag, img_t *im, u32 ss, const char *label) {
    int small = im->len < 4 * 1024 * 1024;
    u32 bigsz = small ? 300 * 1024 : 3 * 1024 * 1024 + 777;
    u8 *readme = (u8 *)"Hello from mtools!\r\n";
    u8 *lfn = pat_alloc(5000, 1), *data = pat_alloc(100000, 2), *big = pat_alloc(bigsz, 3);
    u8 *frag = pat_alloc(3000, 4);

    sh("mmd -i %s '::/Docs' '::/Docs/Nested Dir' '::/Docs/Nested Dir/deeper' '::/frag'", mt(im));
    mt_put(im, "readme.txt", readme, strlen((char *)readme));
    mt_put(im, "A Long File Name.text", lfn, 5000);
    mt_put(im, "UPPER.TXT", readme, 5);
    mt_put(im, "empty.dat", readme, 0);
    mt_put(im, "Docs/Nested Dir/deeper/data.bin", data, 100000);
    char name[64];
    for (int i = 0; i < 20; i++) {
        snprintf(name, sizeof name, "frag/small file %02d.bin", i);
        mt_put(im, name, frag, 3000);
    }
    for (int i = 0; i < 20; i += 2) {
        snprintf(name, sizeof name, "::/frag/small file %02d.bin", i);
        sh("mdel -i %s '%s'", mt(im), name);
    }
    mt_put(im, "frag/big fragmented file.bin", big, bigsz);

    hdisk_t *h = disk_attach(im, ss);
    char L = letter_of(h);
    CHECK(L >= 'C', "[%s] not mounted", tag);
    if (L) {
        volume_t *v = vfs_volume_by_letter(L);
        CHECK(!strcmp(v->label, label), "[%s] label '%s', want '%s'", tag, v->label, label);
        vfs_dirent_t e;
        CHECK(v_listed(P(L, ""), "readme.txt", &e), "[%s] readme.txt not listed", tag);
        CHECK(!strcmp(e.short_name, "README.TXT"), "short name %s", e.short_name);
        CHECK(v_listed(P(L, ""), "A Long File Name.text", &e), "[%s] LFN not listed", tag);
        CHECK(e.size == 5000 && !e.is_dir, "LFN entry size %lu", (unsigned long)e.size);
        char alias[32];
        snprintf(alias, sizeof alias, "%s", e.short_name);
        CHECK(v_listed(P(L, ""), "UPPER.TXT", NULL), "UPPER.TXT");
        CHECK(v_listed(P(L, ""), "Docs", &e) && e.is_dir, "Docs dir");
        CHECK(e.mtime.year >= 2020, "mtime year %u", e.mtime.year);
        CHECK(v_count(P(L, "")) == 6, "[%s] root count %d", tag, v_count(P(L, "")));
        v_cmp(P(L, "readme.txt"), readme, strlen((char *)readme));
        v_cmp(P(L, "A Long File Name.text"), lfn, 5000);
        v_cmp(P(L, alias), lfn, 5000);                       /* open by 8.3 alias */
        v_cmp(P(L, "a long file NAME.TEXT"), lfn, 5000);     /* case-insensitive */
        v_cmp(P(L, "empty.dat"), readme, 0);
        disk_stats_reset(h);
        v_cmp(P(L, "docs\\NESTED DIR/Deeper/./DATA.BIN"), data, 100000);
        CHECK(h->reads <= 12, "[%s] contiguous 100k read took %lu requests", tag, (unsigned long)h->reads);
        CHECK(v_count(P(L, "Docs/Nested Dir")) == 1, "nested count");
        CHECK(v_count(P(L, "frag")) == 11, "frag count %d", v_count(P(L, "frag")));
        disk_stats_reset(h);
        v_cmp(P(L, "frag/big fragmented file.bin"), big, bigsz);
        if (shim_verbose) printf("    big read: %lu requests, max %u sectors\n", (unsigned long)h->reads, h->max_count);
        /* partial reads at odd offsets */
        vfs_file_t *f;
        OK(vfs_open(P(L, "frag/big fragmented file.bin"), VFS_O_READ, &f));
        if (f) {
            u8 *buf = malloc(70000);
            u32 offs[] = { 1, 511, 513, 4095, 12345, bigsz - 70000, bigsz - 3 };
            for (size_t i = 0; i < ARRAY_SIZE(offs); i++) {
                vfs_seek(f, offs[i]);
                int n = vfs_read(f, buf, 70000);
                u32 want = MIN(70000u, bigsz - offs[i]);
                CHECK(n == (int)want && !memcmp(buf, big + offs[i], want), "partial read at %u: %d", offs[i], n);
            }
            vfs_seek(f, bigsz + 10);
            EXPECT(vfs_read(f, buf, 10), 0);
            free(buf);
            vfs_close(f);
        }
        EXPECT(vfs_open(P(L, "Docs"), VFS_O_READ, &f), E_ISDIR);
        EXPECT(vfs_open(P(L, "nope.txt"), VFS_O_READ, &f), E_NOENT);
        EXPECT(vfs_open(P(L, "readme.txt/x"), VFS_O_READ, &f), E_NOTDIR);
        EXPECT(vfs_list(P(L, "readme.txt"), &(vfs_dirent_t *){0}), E_NOTDIR);
        u64 tot, fr;
        OK(vfs_statfs(L, &tot, &fr));
        CHECK(tot > 0 && fr > 0 && fr < tot, "statfs %lu %lu", (unsigned long)tot, (unsigned long)fr);
    }
    disk_detach(h);
    fsck(im, tag);
    free(lfn); free(data); free(big); free(frag);
}

/* writes through the driver, verified by mtools */
static void t_write(const char *tag, img_t *im, u32 ss) {
    hdisk_t *h = disk_attach(im, ss);
    char L = letter_of(h);
    CHECK(L, "[%s] not mounted", tag);
    if (!L) { disk_detach(h); return; }
    u64 free0 = v_free(L);
    u32 csize = img_cluster_size(im);

    OK(vfs_mkdir(P(L, "Projects")));
    OK(vfs_mkdir(P(L, "Projects/Halcyon OS")));
    OK(vfs_mkdir(P(L, "projects/HALCYON os/src")));
    EXPECT(vfs_mkdir(P(L, "Projects")), E_EXIST);
    EXPECT(vfs_mkdir(P(L, "nope/sub")), E_NOENT);

    const char *names[] = {
        "Projects/Halcyon OS/My Notes.txt",
        "Projects/Halcyon OS/src/MAIN.C",
        "mixed Case.Name.With.Dots.txt",
        "name with  spaces & (parens) [br] {braces} +plus+ ;semi; =eq= ,comma, 'q' #1 $2 %3 ^ ~ !.dat",
        "readme.txt",
        ".hidden",
        "x",
        "NOEXT",
        "a.b.c",
        "  leading spaces.txt",
        "UPPER.TX",
    };
    u32 sizes[] = { 1234, 0, 513, 4096, 1, 77777, 2 * 4096 + 1, 100, 3, 9, 10 };
    u8 *bufs[ARRAY_SIZE(names)];
    for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
        bufs[i] = pat_alloc(sizes[i], 100 + (u32)i);
        v_put(P(L, names[i]), bufs[i], sizes[i]);
    }
    /* 255-character name */
    char longname[256];
    for (int i = 0; i < 255; i++) longname[i] = (char)('a' + i % 26);
    memcpy(longname + 251, ".txt", 5);
    u8 *lbuf = pat_alloc(300, 99);
    v_put(P(L, longname), lbuf, 300);
    char toolong[260];
    memset(toolong, 'b', 256);
    toolong[256] = 0;
    CHECK(vfs_write_file(P(L, toolong), lbuf, 1) < 0, "256-char name accepted");

    vfs_dirent_t e;
    CHECK(vfs_stat(P(L, "Projects/Halcyon OS/src/MAIN.C"), &e) == 0 && !strcmp(e.name, "MAIN.C") &&
          !strcmp(e.short_name, "MAIN.C"), "MAIN.C stat: %s / %s", e.name, e.short_name);
    CHECK(vfs_stat(P(L, "readme.txt"), &e) == 0 && !strcmp(e.name, "readme.txt") &&
          !strcmp(e.short_name, "README.TXT"), "readme stat: %s / %s", e.name, e.short_name);
    CHECK(vfs_stat(P(L, "mixed Case.Name.With.Dots.txt"), &e) == 0 && !strcmp(e.short_name, "MIXEDC~1.TXT"),
          "alias %s", e.short_name);
    CHECK(e.mtime.year >= 2024 && e.ctime.year >= 2024, "timestamps %u", e.mtime.year);
    CHECK(e.attr & VFS_ATTR_ARCHIVE, "archive bit");
    CHECK(vfs_stat(P(L, ".hidden"), &e) == 0 && !strcmp(e.short_name, "HIDDEN~1"), "alias %s", e.short_name);

    /* overwrite (O_TRUNC) with smaller content */
    u8 *ow = pat_alloc(700, 555);
    v_put(P(L, names[5]), ow, 700);
    memcpy(bufs[5], ow, 700);
    sizes[5] = 700;
    /* append */
    vfs_file_t *f, *g;
    OK(vfs_open(P(L, names[0]), VFS_O_WRITE | VFS_O_APPEND, &f));
    u8 *app = pat_alloc(3000, 777);
    if (f) {
        vfs_seek(f, 0);
        EXPECT(vfs_write(f, app, 3000), 3000);
        EXPECT(vfs_tell(f), 1234 + 3000);
        EXPECT(vfs_read(f, app, 1), E_INVAL);   /* write-only handle */
        vfs_close(f);
    }
    bufs[0] = realloc(bufs[0], 4234);
    memcpy(bufs[0] + 1234, app, 3000);
    sizes[0] = 4234;
    /* shrink and extend with truncate, then a write past EOF leaving a hole */
    OK(vfs_open(P(L, names[3]), VFS_O_RDWR, &f));
    OK(vfs_open(P(L, names[3]), VFS_O_READ, &g));
    if (f && g) {
        OK(vfs_truncate(f, 1000));
        EXPECT(vfs_size(g), 1000);
        OK(vfs_truncate(f, 3 * csize + 5));
        EXPECT(vfs_size(g), 3 * csize + 5);
        vfs_seek(f, 5 * csize + 100);
        EXPECT(vfs_write(f, "TAIL", 4), 4);
        EXPECT(vfs_size(g), 5 * csize + 104);
        EXPECT(vfs_truncate(g, 0), E_INVAL);    /* read-only handle */
    }
    if (f) vfs_close(f);
    if (g) vfs_close(g);
    u32 n3 = 5 * csize + 104;
    bufs[3] = realloc(bufs[3], n3);
    memset(bufs[3] + 1000, 0, n3 - 1000);
    memcpy(bufs[3] + n3 - 4, "TAIL", 4);
    sizes[3] = n3;
    /* overwrite in the middle of an existing file */
    OK(vfs_open(P(L, names[6]), VFS_O_RDWR, &f));
    if (f) {
        vfs_seek(f, 4000);
        EXPECT(vfs_write(f, "MIDDLE", 6), 6);
        vfs_close(f);
        memcpy(bufs[6] + 4000, "MIDDLE", 6);
    }
    /* O_EXCL */
    EXPECT(vfs_open(P(L, names[2]), VFS_O_WRITE | VFS_O_CREATE | VFS_O_EXCL, &f), E_EXIST);
    EXPECT(vfs_open(P(L, "new.txt"), VFS_O_WRITE, &f), E_NOENT);
    EXPECT(vfs_open(P(L, "Projects"), VFS_O_WRITE, &f), E_ISDIR);
    EXPECT(vfs_open(P(L, ""), VFS_O_READ, &f), E_ISDIR);

    for (size_t i = 0; i < ARRAY_SIZE(names); i++) v_cmp(P(L, names[i]), bufs[i], sizes[i]);
    vfs_copy(P(L, names[0]), P(L, "Projects/copy of notes.txt"));
    v_cmp(P(L, "Projects/copy of notes.txt"), bufs[0], sizes[0]);
    EXPECT(vfs_copy(P(L, "Projects"), P(L, "x2")), E_ISDIR);
    u64 free1 = v_free(L);
    CHECK(free1 < free0, "free space did not shrink");
    disk_detach(h);
    fsck(im, tag);

    for (size_t i = 0; i < ARRAY_SIZE(names); i++) mt_cmp(im, names[i], bufs[i], sizes[i]);
    mt_cmp(im, longname, lbuf, 300);
    mt_cmp(im, "Projects/copy of notes.txt", bufs[0], sizes[0]);
    CHECK(mt_listed(im, "Projects/Halcyon OS", "My Notes.txt"), "mdir does not list My Notes.txt");
    CHECK(mt_listed(im, "", names[3]), "mdir does not list the punctuation name");

    /* remount: persistence */
    h = disk_attach(im, ss);
    L = letter_of(h);
    for (size_t i = 0; i < ARRAY_SIZE(names); i++) v_cmp(P(L, names[i]), bufs[i], sizes[i]);
    CHECK(v_free(L) == free1, "free space changed across remount");
    /* clean up everything, free space must return */
    for (size_t i = 0; i < ARRAY_SIZE(names); i++) OK(vfs_unlink(P(L, names[i])));
    OK(vfs_unlink(P(L, longname)));
    OK(vfs_unlink(P(L, "Projects/copy of notes.txt")));
    EXPECT(vfs_unlink(P(L, "Projects")), E_NOTEMPTY);
    OK(vfs_unlink(P(L, "Projects/Halcyon OS/src")));
    OK(vfs_unlink(P(L, "Projects/Halcyon OS")));
    OK(vfs_unlink(P(L, "Projects")));
    /* FAT32: the root directory is a cluster chain that grew and never shrinks */
    u64 slack = !strcmp(vfs_volume_by_letter(L)->fs_name, "FAT32") ? 4 * csize : 0;
    CHECK(v_free(L) <= free0 && v_free(L) + slack >= free0, "[%s] free %lu after cleanup, want %lu",
          tag, (unsigned long)v_free(L), (unsigned long)free0);
    CHECK(v_count(P(L, "")) == 0, "root not empty: %d", v_count(P(L, "")));
    disk_detach(h);
    fsck(im, tag);
    for (size_t i = 0; i < ARRAY_SIZE(names); i++) free(bufs[i]);
    free(lbuf); free(ow); free(app);
}

/* large files into fragmented free space */
static void t_large(const char *tag, img_t *im, u32 ss, u32 bigsz) {
    hdisk_t *h = disk_attach(im, ss);
    char L = letter_of(h);
    CHECK(L, "not mounted");
    if (!L) { disk_detach(h); return; }
    u32 csize = img_cluster_size(im);
    u64 free0 = v_free(L);

    /* contiguous write in one call: few large requests */
    u8 *c = pat_alloc(2 * 1024 * 1024, 42);
    disk_stats_reset(h);
    v_put(P(L, "contig.bin"), c, 2 * 1024 * 1024);
    if (shim_verbose) printf("    contiguous 2 MiB write: %lu writes %lu reads\n", (unsigned long)h->writes, (unsigned long)h->reads);
    CHECK(h->writes < 200, "[%s] 2 MiB write took %lu requests", tag, (unsigned long)h->writes);
    disk_stats_reset(h);
    v_cmp(P(L, "contig.bin"), c, 2 * 1024 * 1024);
    CHECK(h->reads < 40, "[%s] 2 MiB read took %lu requests", tag, (unsigned long)h->reads);

    OK(vfs_mkdir(P(L, "fill")));
    char name[64];
    u8 *small = pat_alloc(20000, 5);
    for (int i = 0; i < 120; i++) {
        snprintf(name, sizeof name, "fill/filler number %03d.bin", i);
        v_put(P(L, name), small, 20000);
    }
    for (int i = 0; i < 120; i += 2) {
        snprintf(name, sizeof name, "fill/filler number %03d.bin", i);
        OK(vfs_unlink(P(L, name)));
    }
    u64 free1 = v_free(L);
    u8 *big = pat_alloc(bigsz, 6);
    vfs_file_t *f;
    OK(vfs_open(P(L, "Big File.bin"), VFS_O_WRITE | VFS_O_CREATE, &f));
    if (f) {
        u32 done = 0, step = 70001;
        while (done < bigsz) {
            u32 n = MIN(step, bigsz - done);
            int w = vfs_write(f, big + done, n);
            if (w != (int)n) { CHECK(0, "write at %u = %d", done, w); break; }
            done += n;
            step = step == 70001 ? 1000000 : step == 1000000 ? 333 : 70001;
        }
        vfs_close(f);
    }
    u64 free2 = v_free(L);
    u64 used = ((u64)bigsz + csize - 1) / csize * csize;
    CHECK(free1 - free2 == used, "[%s] big file used %lu bytes, want %lu", tag, (unsigned long)(free1 - free2), (unsigned long)used);
    v_cmp(P(L, "Big File.bin"), big, bigsz);
    disk_detach(h);
    fsck(im, tag);
    mt_cmp(im, "Big File.bin", big, bigsz);
    mt_cmp(im, "contig.bin", c, 2 * 1024 * 1024);
    snprintf(name, sizeof name, "fill/filler number %03d.bin", 77);
    mt_cmp(im, name, small, 20000);

    h = disk_attach(im, ss);
    L = letter_of(h);
    OK(vfs_unlink(P(L, "Big File.bin")));
    CHECK(v_free(L) == free1, "free after delete");
    for (int i = 1; i < 120; i += 2) {
        snprintf(name, sizeof name, "fill/filler number %03d.bin", i);
        OK(vfs_unlink(P(L, name)));
    }
    OK(vfs_unlink(P(L, "fill")));
    OK(vfs_unlink(P(L, "contig.bin")));
    CHECK(v_free(L) == free0, "free after cleanup");
    disk_detach(h);
    fsck(im, tag);
    free(c); free(small); free(big);
}

/* many files in one directory: growth, alias collisions, root full */
static void t_many(const char *tag, img_t *im, u32 ss, int nfiles) {
    hdisk_t *h = disk_attach(im, ss);
    char L = letter_of(h);
    CHECK(L, "not mounted");
    if (!L) { disk_detach(h); return; }
    char name[96], content[64];
    OK(vfs_mkdir(P(L, "Many Files")));
    for (int i = 1; i <= nfiles; i++) {
        snprintf(name, sizeof name, "Many Files/Long Name %d.txt", i);
        snprintf(content, sizeof content, "file number %d", i);
        if (v_put(P(L, name), (u8 *)content, (u32)strlen(content)) < 0) break;
    }
    vfs_dirent_t *ents;
    int n = vfs_list(P(L, "Many Files"), &ents);
    EXPECT(n, nfiles);
    int dup = 0;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (!strcmp(ents[i].short_name, ents[j].short_name)) dup++;
    CHECK(dup == 0, "%d duplicate short names", dup);
    int found1 = 0, found5 = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(ents[i].name, "Long Name 1.txt")) found1 = !strcmp(ents[i].short_name, "LONGNA~1.TXT");
        if (!strcmp(ents[i].name, "Long Name 5.txt")) found5 = !strcmp(ents[i].short_name, "LONGNA~5.TXT");
    }
    CHECK(found1 && found5, "alias numbering");
    if (n >= 0) kfree(ents);
    for (int i = 1; i <= nfiles; i += 37) {
        snprintf(name, sizeof name, "Many Files/long name %d.TXT", i);
        snprintf(content, sizeof content, "file number %d", i);
        v_cmp(P(L, name), (u8 *)content, (u32)strlen(content));
    }
    /* delete every third and create new ones reusing the slots */
    for (int i = 3; i <= nfiles; i += 3) {
        snprintf(name, sizeof name, "Many Files/Long Name %d.txt", i);
        OK(vfs_unlink(P(L, name)));
    }
    for (int i = 0; i < nfiles / 6; i++) {
        snprintf(name, sizeof name, "Many Files/Another long name %d", i);
        v_put(P(L, name), (u8 *)"x", 1);
    }
    EXPECT(v_count(P(L, "Many Files")), nfiles - nfiles / 3 + nfiles / 6);
    disk_detach(h);
    fsck(im, tag);
    for (int i = 1; i <= nfiles; i += 50) {
        if (i % 3 == 0) continue;
        snprintf(name, sizeof name, "Many Files/Long Name %d.txt", i);
        snprintf(content, sizeof content, "file number %d", i);
        mt_cmp(im, name, (u8 *)content, strlen(content));
    }
}

/* fill a fixed FAT12/16 root directory */
static void t_rootfull(const char *tag, img_t *im, u32 ss, int lfn) {
    hdisk_t *h = disk_attach(im, ss);
    char L = letter_of(h);
    CHECK(L, "not mounted");
    if (!L) { disk_detach(h); return; }
    u64 free0 = v_free(L);
    char name[96];
    int created = 0, r = 0;
    for (int i = 0; i < 2000; i++) {
        if (lfn) snprintf(name, sizeof name, "Root file with long name %d.txt", i);
        else snprintf(name, sizeof name, "F%05d.TXT", i);
        r = vfs_write_file(P(L, name), "", 0);
        if (r < 0) break;
        created++;
    }
    EXPECT(r, E_NOSPC);
    CHECK(created > 50, "[%s] only %d files", tag, created);
    if (!lfn) {
        /* 1.44M floppy: 224 entries, one used by the volume label */
        EXPECT(created, 223);
    }
    /* a directory whose name needs as many slots as the files did */
    EXPECT(vfs_mkdir(P(L, lfn ? "Directory with long name 77777" : "SOMEDIR")), E_NOSPC);
    CHECK(v_free(L) == free0, "[%s] mkdir on full root leaked a cluster", tag);
    EXPECT(v_count(P(L, "")), created);
    disk_detach(h);
    fsck(im, tag);
    CHECK(mt_listed(im, "", lfn ? "Root file with long name 7.txt" : "F00007.TXT"), "mdir");
}

static void t_rename(const char *tag, img_t *im, u32 ss) {
    hdisk_t *h = disk_attach(im, ss);
    char L = letter_of(h);
    CHECK(L, "not mounted");
    if (!L) { disk_detach(h); return; }
    u8 *d1 = pat_alloc(5000, 11), *d2 = pat_alloc(7000, 12), *d3 = pat_alloc(100, 13);
    OK(vfs_mkdir(P(L, "dir1")));
    OK(vfs_mkdir(P(L, "dir1/inner")));
    OK(vfs_mkdir(P(L, "y")));
    OK(vfs_mkdir(P(L, "y/sub")));
    v_put(P(L, "a.txt"), d1, 5000);
    v_put(P(L, "dir1/inner/deep file.txt"), d2, 7000);
    v_put(P(L, "dir1/f.txt"), d3, 100);
    v_put(P(L, "other.txt"), d3, 100);

    OK(vfs_rename(P(L, "a.txt"), P(L, "Renamed File.txt")));
    CHECK(!v_exists(P(L, "a.txt")), "old name still exists");
    v_cmp(P(L, "Renamed File.txt"), d1, 5000);
    OK(vfs_rename(P(L, "Renamed File.txt"), P(L, "RENAMED file.TXT")));      /* case only */
    CHECK(v_listed(P(L, ""), "RENAMED file.TXT", NULL), "case rename not visible");
    EXPECT(v_count(P(L, "")), 4);
    EXPECT(vfs_rename(P(L, "other.txt"), P(L, "renamed FILE.txt")), E_EXIST);
    EXPECT(vfs_rename(P(L, "nope"), P(L, "x")), E_NOENT);
    EXPECT(vfs_rename(P(L, "other.txt"), P(L, "nodir/x")), E_NOENT);
    EXPECT(vfs_rename(P(L, "other.txt"), P(L, "bad?name")), E_INVAL);
    OK(vfs_rename(P(L, "other.txt"), P(L, "other.txt")));
    /* move a file between directories */
    OK(vfs_rename(P(L, "dir1/f.txt"), P(L, "y/f moved.txt")));
    v_cmp(P(L, "y/f moved.txt"), d3, 100);
    /* rename a directory, then move it (with contents) into another one */
    OK(vfs_rename(P(L, "dir1"), P(L, "Dir Renamed")));
    v_cmp(P(L, "Dir Renamed/inner/deep file.txt"), d2, 7000);
    OK(vfs_rename(P(L, "Dir Renamed"), P(L, "y/sub/Moved Dir")));
    v_cmp(P(L, "y/sub/Moved Dir/inner/deep file.txt"), d2, 7000);
    /* into itself: by path and via an alias the VFS cannot see through */
    EXPECT(vfs_rename(P(L, "y"), P(L, "y/sub/x")), E_INVAL);
    OK(vfs_mkdir(P(L, "Parent Dir")));
    OK(vfs_mkdir(P(L, "Parent Dir/child")));
    EXPECT(vfs_rename(P(L, "Parent Dir"), P(L, "PARENT~1/child/x")), E_INVAL);
    EXPECT(vfs_rename(P(L, "Parent Dir"), P(L, "PARENT~1")), 0);   /* to its own alias: allowed */
    /* move a subdirectory up to the root ('..' must become 0) */
    OK(vfs_rename(P(L, "y/sub/Moved Dir/inner"), P(L, "inner at root")));
    v_cmp(P(L, "inner at root/deep file.txt"), d2, 7000);
    /* move a directory from the root down */
    OK(vfs_rename(P(L, "PARENT~1"), P(L, "y/sub/Moved Dir/parent")));
    /* rename an open file, keep writing through the handle */
    vfs_file_t *f;
    OK(vfs_open(P(L, "y/f moved.txt"), VFS_O_RDWR, &f));
    OK(vfs_rename(P(L, "y/f moved.txt"), P(L, "f final.txt")));
    if (f) {
        vfs_seek(f, 100);
        EXPECT(vfs_write(f, "+appended", 9), 9);
        vfs_close(f);
    }
    u8 *d3b = malloc(109);
    memcpy(d3b, d3, 100);
    memcpy(d3b + 100, "+appended", 9);
    v_cmp(P(L, "f final.txt"), d3b, 109);
    EXPECT(vfs_unlink(P(L, "y/f moved.txt")), E_NOENT);
    disk_detach(h);
    fsck(im, tag);
    mt_cmp(im, "RENAMED file.TXT", d1, 5000);
    mt_cmp(im, "inner at root/deep file.txt", d2, 7000);
    mt_cmp(im, "f final.txt", d3b, 109);
    CHECK(mt_listed(im, "y/sub/Moved Dir", "parent"), "moved dir not listed by mtools");
    CHECK(mt_listed(im, "y/sub/Moved Dir/parent", "child"), "moved dir contents not listed");
    free(d1); free(d2); free(d3); free(d3b);
}

static void t_delete(const char *tag, img_t *im, u32 ss) {
    hdisk_t *h = disk_attach(im, ss);
    char L = letter_of(h);
    CHECK(L, "not mounted");
    if (!L) { disk_detach(h); return; }
    u64 free0 = v_free(L);
    u8 *d = pat_alloc(50000, 21);
    OK(vfs_mkdir(P(L, "D")));
    OK(vfs_mkdir(P(L, "D/E")));
    v_put(P(L, "D/E/file one.bin"), d, 50000);
    v_put(P(L, "D/two.bin"), d, 10);
    EXPECT(vfs_unlink(P(L, "D")), E_NOTEMPTY);
    EXPECT(vfs_unlink(P(L, "D/E")), E_NOTEMPTY);
    EXPECT(vfs_unlink(P(L, "D/zzz")), E_NOENT);
    EXPECT(vfs_unlink(P(L, "")), E_INVAL);
    vfs_file_t *f;
    OK(vfs_open(P(L, "D/two.bin"), VFS_O_READ, &f));
    EXPECT(vfs_unlink(P(L, "D/two.bin")), E_BUSY);
    if (f) vfs_close(f);
    OK(vfs_unlink(P(L, "D/two.bin")));
    OK(vfs_unlink(P(L, "D/E/FILEON~1.BIN")));
    OK(vfs_unlink(P(L, "D/E")));
    OK(vfs_unlink(P(L, "D")));
    CHECK(!v_exists(P(L, "D")), "D still exists");
    CHECK(v_free(L) == free0, "[%s] space not reclaimed", tag);
    disk_detach(h);
    fsck(im, tag);
    free(d);
}

static void t_diskfull(const char *tag, img_t *im, u32 ss) {
    hdisk_t *h = disk_attach(im, ss);
    char L = letter_of(h);
    CHECK(L, "not mounted");
    if (!L) { disk_detach(h); return; }
    u64 free0 = v_free(L);
    u32 csize = img_cluster_size(im);
    u32 n = (u32)free0 + 5 * csize + 123;
    u8 *d = pat_alloc(n, 31);
    v_put(P(L, "first.bin"), d, 3 * csize);
    vfs_file_t *f;
    int w = 0;
    OK(vfs_open(P(L, "fill.bin"), VFS_O_WRITE | VFS_O_CREATE, &f));
    if (f) {
        w = vfs_write(f, d, n);
        CHECK(w > 0 && (u64)w == free0 - 3 * csize, "[%s] partial write %d, free was %lu", tag, w, (unsigned long)free0);
        EXPECT(vfs_write(f, d, 100), E_NOSPC);
        EXPECT(vfs_size(f), w);
        vfs_seek(f, (u64)w + 100000);
        EXPECT(vfs_write(f, d, 1), E_NOSPC);
        EXPECT(vfs_size(f), w);
        vfs_close(f);
    }
    EXPECT(v_free(L), 0);
    EXPECT(vfs_mkdir(P(L, "no room")), E_NOSPC);
    OK(vfs_write_file(P(L, "empty but fine.txt"), "", 0));
    EXPECT(vfs_write_file(P(L, "x.txt"), "1", 1), E_NOSPC);
    /* copy must not leave a partial file behind */
    EXPECT(vfs_copy(P(L, "first.bin"), P(L, "copy.bin")), E_NOSPC);
    CHECK(!v_exists(P(L, "copy.bin")), "partial copy left behind");
    disk_detach(h);
    fsck(im, tag);
    if (w > 0) mt_cmp(im, "fill.bin", d, (size_t)w);
    h = disk_attach(im, ss);
    L = letter_of(h);
    EXPECT(v_free(L), 0);
    OK(vfs_unlink(P(L, "fill.bin")));
    OK(vfs_unlink(P(L, "first.bin")));
    OK(vfs_unlink(P(L, "empty but fine.txt")));
    EXPECT(v_free(L), free0);
    disk_detach(h);
    fsck(im, tag);
    free(d);
}

static void t_illegal(const char *tag, img_t *im, u32 ss) {
    hdisk_t *h = disk_attach(im, ss);
    char L = letter_of(h);
    if (!L) { CHECK(0, "not mounted"); disk_detach(h); return; }
    const char *bad[] = { "a*b", "a?b", "a:b", "a<b", "a>b", "a|b", "a\"b", "tab\there", "\x01", "caf\xe9" };
    for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
        EXPECT(vfs_write_file(P(L, bad[i]), "x", 1), E_INVAL);
        EXPECT(vfs_mkdir(P(L, bad[i])), E_INVAL);
    }
    EXPECT(vfs_mkdir(P(L, "...")), E_INVAL);
    OK(vfs_write_file(P(L, "trailing dots... "), "x", 1));
    CHECK(v_listed(P(L, ""), "trailing dots", NULL), "trailing dots not stripped");
    CHECK(v_exists(P(L, "trailing dots.")), "lookup with trailing dot");
    EXPECT(v_count(P(L, "")), 1);
    disk_detach(h);
    fsck(im, tag);
}

static void t_unplug(const char *tag, img_t *im, u32 ss) {
    hdisk_t *h = disk_attach(im, ss);
    char L = letter_of(h);
    if (!L) { CHECK(0, "not mounted"); disk_detach(h); return; }
    u32 gen = vfs_generation();
    int nvol = vfs_volume_count();
    vfs_file_t *f;
    OK(vfs_open(P(L, "unplug.txt"), VFS_O_RDWR | VFS_O_CREATE, &f));
    EXPECT(vfs_write(f, "before", 6), 6);
    /* yank the stick between operations */
    disk_detach(h);
    CHECK(vfs_generation() != gen, "generation not bumped");
    EXPECT(vfs_volume_count(), nvol - 1);
    CHECK(!vfs_volume_by_letter(L), "letter still assigned");
    EXPECT(vfs_write(f, "after", 5), E_NODEV);
    EXPECT(vfs_read(f, (char[8]){0}, 5), E_NODEV);
    EXPECT(vfs_seek(f, 0), E_NODEV);
    EXPECT(vfs_close(f), 0);
    EXPECT(vfs_open(P(L, "unplug.txt"), VFS_O_READ, &f), E_NODEV);
    fsck(im, tag);
    mt_cmp(im, "unplug.txt", (u8 *)"before", 6);
}

static void t_letters(void) {
    img_t a, b, c;
    img_super(&a, "letters_a.img", 12, 1440, "DATA", NULL);
    img_super(&b, "letters_b.img", 12, 1440, "HALCYON", NULL);
    img_mbr(&c, "letters_c.img", 16, 20, "BOOTED", 0x1234ABCD);
    memset(&g_bootinfo, 0, sizeof g_bootinfo);
    hdisk_t *ha = disk_attach(&a, 512), *hb = disk_attach(&b, 512);
    EXPECT(letter_of(ha), 'D');
    EXPECT(letter_of(hb), 'C');
    CHECK(vfs_volume_by_letter('C') && vfs_volume_by_letter('C')->is_boot, "is_boot");
    EXPECT(vfs_volume(0)->letter, 'C');
    EXPECT(vfs_volume(1)->letter, 'D');
    /* across volumes: copy works, rename does not */
    u8 *x = pat_alloc(40000, 9);
    v_put("C:/cross.bin", x, 40000);
    EXPECT(vfs_rename("C:/cross.bin", "D:/cross.bin"), E_NOTSUP);
    OK(vfs_copy("C:/cross.bin", "d:\\Copied Across Volumes.bin"));
    v_cmp("D:/copied across volumes.bin", x, 40000);
    EXPECT(vfs_copy("C:/cross.bin", "c:/CROSS.BIN"), E_EXIST);
    OK(vfs_sync_all());
    disk_detach(hb);
    fsck(&b, "cross-volume copy (src)");
    fsck(&a, "cross-volume copy (dst)");
    mt_cmp(&a, "Copied Across Volumes.bin", x, 40000);
    free(x);
    /* read-only device */
    {
        hdisk_t *ro = calloc(1, sizeof *ro);
        ro->fd = open(b.path, O_RDONLY);
        snprintf(ro->bd.name, sizeof ro->bd.name, "ro0");
        ro->bd.sector_size = 512;
        ro->bd.sector_count = 1440 * 2;
        ro->bd.readonly = 1;
        ro->bd.read = hd_read;
        ro->bd.write = hd_write;
        OK(blockdev_register(&ro->bd));
        char R = letter_of(ro);
        CHECK(R, "read-only disk not mounted");
        if (R) {
            v_cmp(P(R, "cross.bin"), x = pat_alloc(40000, 9), 40000);
            free(x);
            EXPECT(vfs_write_file(P(R, "new.txt"), "x", 1), E_ROFS);
            EXPECT(vfs_mkdir(P(R, "dir")), E_ROFS);
            EXPECT(vfs_unlink(P(R, "cross.bin")), E_ROFS);
            EXPECT(vfs_rename(P(R, "cross.bin"), P(R, "y.bin")), E_ROFS);
            vfs_file_t *f;
            EXPECT(vfs_open(P(R, "cross.bin"), VFS_O_RDWR, &f), E_ROFS);
            u64 t, fr;
            OK(vfs_statfs(R, &t, &fr));
        }
        EXPECT(ro->writes, 0);
        disk_detach(ro);
    }
    hdisk_t *hc = disk_attach(&c, 512);
    EXPECT(letter_of(hc), 'E');                      /* not the boot disk (sig unknown) */
    disk_detach(hc);
    g_bootinfo.boot_disk_sig = 0x1234ABCD;
    g_bootinfo.boot_part_lba = 2048;
    hb = disk_attach(&b, 512);
    EXPECT(letter_of(hb), 'E');                      /* label alone no longer counts */
    hc = disk_attach(&c, 512);
    EXPECT(letter_of(hc), 'C');
    blockdev_t *part = NULL;
    for (blockdev_t *b2 = blockdev_first(); b2; b2 = b2->next) if (b2->parent == &hc->bd) part = b2;
    CHECK(part && part->type == BDEV_PART && part->part_index == 1 && part->part_start == 2048 &&
          part->part_type == 0x06, "partition fields");
    if (part) {
        char want[32];
        snprintf(want, sizeof want, "%sp1", hc->bd.name);
        CHECK(!strcmp(part->name, want), "partition name %s", part->name);
        EXPECT(blk_read(part, part->sector_count, 1, (u8[512]){0}), E_INVAL);
        EXPECT(blk_read(part, part->sector_count - 1, 2, (u8[1024]){0}), E_INVAL);
        OK(blk_read(part, part->sector_count - 1, 1, (u8[512]){0}));
    }
    EXPECT(hc->bd.disk_sig, 0x1234ABCD);
    int before = blockdev_count();
    disk_detach(hc);
    EXPECT(blockdev_count(), before - 2);
    disk_detach(ha);
    disk_detach(hb);
    memset(&g_bootinfo, 0, sizeof g_bootinfo);
    EXPECT(vfs_volume_count(), 0);
    EXPECT(blockdev_count(), 0);
}

/* MBR with an extended partition holding two logical partitions */
static void t_extended(void) {
    img_t im;
    snprintf(im.path, sizeof im.path, "%s", wpath("ext.img"));
    unlink(im.path);
    int r = sh("truncate -s 64M '%s' && printf 'label: dos\\nstart=2048, size=20480, type=6\\n"
               "start=22528, size=100000, type=5\\nstart=24576, size=20480, type=1\\n"
               "start=47104, size=40960, type=6\\n' | sfdisk -q '%s'"
               " && mkfs.fat --offset=2048 -F 16 -n PRIMARY '%s' 10240"
               " && mkfs.fat --offset=24576 -F 12 -n LOGICAL5 '%s' 10240"
               " && mkfs.fat --offset=47104 -F 16 -n LOGICAL6 '%s' 20480",
               im.path, im.path, im.path, im.path, im.path);
    CHECK(r == 0, "extended image creation");
    struct { u64 start, len; int idx; const char *label; } parts[] = {
        { 2048, 20480, 1, "PRIMARY" }, { 24576, 20480, 5, "LOGICAL5" }, { 47104, 40960, 6, "LOGICAL6" } };
    hdisk_t *h = disk_attach(&im, 512);
    EXPECT(vfs_volume_count(), 3);
    for (size_t i = 0; i < ARRAY_SIZE(parts); i++) {
        blockdev_t *p = NULL;
        for (blockdev_t *b = blockdev_first(); b; b = b->next)
            if (b->parent == &h->bd && b->part_index == parts[i].idx) p = b;
        CHECK(p && p->part_start == parts[i].start && p->sector_count == parts[i].len,
              "partition %d", parts[i].idx);
        if (!p) continue;
        volume_t *v = NULL;
        for (int k = 0; k < vfs_volume_count(); k++) if (vfs_volume(k)->bd == p) v = vfs_volume(k);
        CHECK(v && !strcmp(v->label, parts[i].label), "label of partition %d", parts[i].idx);
        if (v) {
            char path[64];
            snprintf(path, sizeof path, "%c:/written to %s.txt", v->letter, parts[i].label);
            v_put(path, (u8 *)parts[i].label, (u32)strlen(parts[i].label));
        }
    }
    disk_detach(h);
    for (size_t i = 0; i < ARRAY_SIZE(parts); i++) {
        img_t pi = im;
        pi.off = parts[i].start * 512;
        pi.len = parts[i].len * 512;
        fsck(&pi, parts[i].label);
        char name[64];
        snprintf(name, sizeof name, "written to %s.txt", parts[i].label);
        mt_cmp(&pi, name, (u8 *)parts[i].label, strlen(parts[i].label));
    }
}

/* superfloppy whose boot code happens to contain a plausible partition entry,
 * and a GPT disk with a destroyed primary header */
static void t_detect(void) {
    img_t im;
    img_super(&im, "fakembr.img", 16, 16384, "FAKEMBR", NULL);
    int fd = open(im.path, O_RDWR);
    u8 ent[16] = { 0x80, 0, 0, 0, 0x06, 0, 0, 0, 1, 0, 0, 0, 100, 0, 0, 0 };   /* start 1, 100 sectors */
    pwrite(fd, ent, 16, 446);
    close(fd);
    hdisk_t *h = disk_attach(&im, 512);
    char L = letter_of(h);
    CHECK(L && !strcmp(vfs_volume_by_letter(L)->label, "FAKEMBR"), "superfloppy with fake MBR entry not mounted");
    CHECK(L && vfs_volume_by_letter(L)->bd == &h->bd, "mounted a bogus partition");
    disk_detach(h);

    img_gpt(&im, "gptbackup.img", 40, "BACKUPGPT");
    fd = open(im.path, O_RDWR);
    u8 z[512] = { 0 };
    pwrite(fd, z, 512, 512);      /* primary GPT header */
    close(fd);
    h = disk_attach(&im, 512);
    L = letter_of(h);
    CHECK(L && !strcmp(vfs_volume_by_letter(L)->label, "BACKUPGPT"), "backup GPT header not used");
    disk_detach(h);

    /* garbage disk: nothing mounted, no crash */
    img_t g;
    snprintf(g.path, sizeof g.path, "%s", wpath("garbage.img"));
    u8 *junk = pat_alloc(1024 * 1024, 77);
    host_write(g.path, junk, 1024 * 1024);
    free(junk);
    h = disk_attach(&g, 512);
    EXPECT(vfs_volume_count(), 0);
    disk_detach(h);
}

/* the same 4096-byte-sector file system seen through a 512-byte device */
static void t_shift(img_t *im) {
    u8 *d = pat_alloc(123457, 61);
    hdisk_t *h = disk_attach(im, 512);
    char L = letter_of(h);
    CHECK(L, "4K FS on 512 device not mounted");
    if (L) {
        OK(vfs_mkdir(P(L, "via 512 device")));
        v_put(P(L, "via 512 device/odd sized file.bin"), d, 123457);
        v_cmp(P(L, "via 512 device/odd sized file.bin"), d, 123457);
    }
    disk_detach(h);
    fsck(im, "4K FS via 512 device");
    mt_cmp(im, "via 512 device/odd sized file.bin", d, 123457);
    free(d);
}

/* ------------------------------------------------------------------ */

/* randomized operations mirrored in an in-memory model */
typedef struct { char path[160]; u8 *data; u32 size; int used; } mfile_t;
#define MAXF 48
static mfile_t g_m[MAXF];
static u32 g_rng;
static u32 rnd(u32 n) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return n ? g_rng % n : 0;
}
static const char *g_dirs[] = { "", "Sub Dir One", "Sub Dir One/Nested", "SUB2" };

static mfile_t *m_find(const char *path) {
    for (int i = 0; i < MAXF; i++) if (g_m[i].used && !strcasecmp(g_m[i].path, path)) return &g_m[i];
    return NULL;
}

static void m_random_path(char *out, size_t sz) {
    static const char *pool[] = { "a%u.txt", "Long File Name %u.dat", "MIXED case %u", "x%u", "With Space %u.c",
                                  "UPPER%u.TXT", "dots.in.name.%u.tar.gz", "%u", "verylongname_without_spaces_%u.text" };
    const char *d = g_dirs[rnd(ARRAY_SIZE(g_dirs))];
    char name[96];
    snprintf(name, sizeof name, pool[rnd(ARRAY_SIZE(pool))], rnd(40));
    snprintf(out, sz, "%s%s%s", d, *d ? "/" : "", name);
}

/* re-read a file after a failed (disk full) operation and adopt its state */
static void m_resync(char L, mfile_t *m, const char *what) {
    char *data = NULL;
    u32 sz = 0;
    int r = vfs_read_file(P(L, m->path), &data, &sz);
    CHECK(r >= 0, "resync read of %s after %s: %s", m->path, what, ename(r));
    if (r < 0) return;
    u32 common = MIN(sz, m->size);
    CHECK(!memcmp(data, m->data, common), "%s: content of %s corrupted by failed %s", "random", m->path, what);
    m->data = realloc(m->data, sz ? sz : 1);
    memcpy(m->data, data, sz);
    m->size = sz;
    kfree(data);
}

static void m_verify_all(char L, const char *tag) {
    int n = 0;
    for (int i = 0; i < MAXF; i++) {
        if (!g_m[i].used) continue;
        v_cmp(P(L, g_m[i].path), g_m[i].data, g_m[i].size);
        n++;
    }
    int listed = 0;
    for (size_t d = 0; d < ARRAY_SIZE(g_dirs); d++) {
        vfs_dirent_t *ents;
        int c = vfs_list(P(L, g_dirs[d]), &ents);
        for (int k = 0; k < c; k++) if (!ents[k].is_dir) listed++;
        if (c >= 0) kfree(ents);
    }
    CHECK(listed == n, "[%s] %d files listed, model has %d", tag, listed, n);
}

static void t_random(const char *tag, img_t *im, u32 ss, u32 seed, int ops, u32 maxsz) {
    g_rng = seed;
    memset(g_m, 0, sizeof g_m);
    hdisk_t *h = disk_attach(im, ss);
    char L = letter_of(h);
    if (!L) { CHECK(0, "not mounted"); disk_detach(h); return; }
    for (size_t d = 1; d < ARRAY_SIZE(g_dirs); d++) OK(vfs_mkdir(P(L, g_dirs[d])));
    u8 *buf = malloc(maxsz + 1);
    int nfull = 0;
    int paranoid = getenv("FSTEST_PARANOID") != NULL;
    for (int op = 0; op < ops; op++) {
        if (paranoid) {
            /* every operation leaves the volume consistent on disk */
            int f0 = g_fail;
            fsck(im, tag);
            if (g_fail != f0) { printf("    first inconsistency before op %d\n", op); break; }
            if (shim_verbose) printf("    op %d\n", op);
        }
        int kind = rnd(100);
        int idx = rnd(MAXF);
        mfile_t *m = &g_m[idx];
        char path[160];
        if (!m->used || kind < 15) {
            /* create */
            if (m->used) continue;
            m_random_path(path, sizeof path);
            if (m_find(path)) continue;
            u32 sz = rnd(4) == 0 ? 0 : rnd(maxsz);
            pattern(buf, sz, seed + op);
            int r = vfs_write_file(P(L, path), buf, sz);
            if (r < 0 && r != E_NOSPC) { CHECK(0, "[%s] create %s: %s", tag, path, ename(r)); continue; }
            if (r == E_NOSPC && !v_exists(P(L, path))) { nfull++; continue; }
            snprintf(m->path, sizeof m->path, "%s", path);
            m->data = malloc(sz ? sz : 1);
            memcpy(m->data, buf, sz);
            m->size = sz;
            m->used = 1;
            if (r < 0) { nfull++; m_resync(L, m, "create"); }
        } else if (kind < 45) {
            /* write somewhere */
            u32 off = rnd(m->size + 5000), len = 1 + rnd(maxsz / 2);
            pattern(buf, len, seed ^ (u32)op);
            vfs_file_t *f;
            int r = vfs_open(P(L, m->path), VFS_O_RDWR, &f);
            CHECK(r == 0, "[%s] open %s: %s", tag, m->path, ename(r));
            if (r) continue;
            vfs_seek(f, off);
            int w = vfs_write(f, buf, len);
            vfs_close(f);
            if (w == E_NOSPC) { nfull++; m_resync(L, m, "write"); continue; }
            CHECK(w > 0, "[%s] write %s: %s", tag, m->path, ename(w));
            if (w <= 0) continue;
            if (w < (int)len) nfull++;
            u32 end = off + (u32)w;
            if (end > m->size) {
                m->data = realloc(m->data, end);
                if (off > m->size) memset(m->data + m->size, 0, off - m->size);
                m->size = end;
            }
            memcpy(m->data + off, buf, (u32)w);
        } else if (kind < 55) {
            /* truncate */
            u32 ns = rnd(m->size * 2 + 3000);
            vfs_file_t *f;
            if (vfs_open(P(L, m->path), VFS_O_RDWR, &f)) { CHECK(0, "open"); continue; }
            int r = vfs_truncate(f, ns);
            vfs_close(f);
            if (r == E_NOSPC) { nfull++; m_resync(L, m, "truncate"); continue; }
            CHECK(r == 0, "[%s] truncate %s: %s", tag, m->path, ename(r));
            if (r) continue;
            m->data = realloc(m->data, ns ? ns : 1);
            if (ns > m->size) memset(m->data + m->size, 0, ns - m->size);
            m->size = ns;
        } else if (kind < 65) {
            /* delete */
            int r = vfs_unlink(P(L, m->path));
            CHECK(r == 0, "[%s] unlink %s: %s", tag, m->path, ename(r));
            free(m->data);
            m->used = 0;
        } else if (kind < 80) {
            /* rename / move */
            m_random_path(path, sizeof path);
            mfile_t *other = m_find(path);
            int r = vfs_rename(P(L, m->path), P(L, path));
            if (other && other != m) { EXPECT(r, E_EXIST); continue; }
            if (r == E_NOSPC) { nfull++; continue; }
            CHECK(r == 0, "[%s] rename %s -> %s: %s", tag, m->path, path, ename(r));
            if (!r) snprintf(m->path, sizeof m->path, "%s", path);
        } else if (kind < 95) {
            v_cmp(P(L, m->path), m->data, m->size);
        } else {
            /* unplug / replug and check everything */
            disk_detach(h);
            fsck(im, tag);
            h = disk_attach(im, ss);
            L = letter_of(h);
            if (!L) { CHECK(0, "remount failed"); break; }
            m_verify_all(L, tag);
        }
    }
    if (L) m_verify_all(L, tag);
    if (shim_verbose) printf("    %d ops, %d disk-full events\n", ops, nfull);
    disk_detach(h);
    fsck(im, tag);
    int checked = 0;
    for (int i = 0; i < MAXF; i++) {
        if (!g_m[i].used) continue;
        if (checked++ < 12) mt_cmp(im, g_m[i].path, g_m[i].data, g_m[i].size);
        free(g_m[i].data);
        g_m[i].used = 0;
    }
    free(buf);
}

static void s_random(void) {
    img_t im;
    const char *only = getenv("FSTEST_RANDOM_ONLY");
    if (only && !strcmp(only, "32")) {
        img_mbr(&im, "rnd32.img", 32, 40, "RND32", 2);
        t_random("random fat32", &im, 512, 3, 1500, 300000);
        return;
    }
    img_super(&im, "rnd12.img", 12, 1440, "RND12", NULL);
    t_random("random fat12", &im, 512, 1, 1500, 60000);        /* fills the floppy repeatedly */
    img_mbr(&im, "rnd16.img", 16, 20, "RND16", 1);
    t_random("random fat16", &im, 512, 2, 1500, 1000000);
    img_mbr(&im, "rnd32.img", 32, 40, "RND32", 2);
    t_random("random fat32", &im, 512, 3, 1500, 300000);
    img_super(&im, "rnd4k.img", 16, 65536, "RND4K", "-S 4096");
    t_random("random fat16 4k", &im, 4096, 4, 800, 200000);
    img_super(&im, "rnd32small.img", 32, 36000, "RNDFULL", "-s 1");
    t_random("random fat32 small", &im, 512, 5, 1500, 2000000);   /* frequent disk full */
}

/* the integration test images from tools/mktestdisks.sh */
static void s_testdisks(void) {
    char dir[1100];
    snprintf(dir, sizeof dir, "%s", wpath("testdisks"));
    int r = sh("BUILD=%s sh ../../tools/mktestdisks.sh", q(dir));
    CHECK(r == 0, "mktestdisks.sh failed");
    if (r) return;
    struct { const char *file, *label, *fs; } d[] = {
        { "fat12.img", "FLOPPY", "FAT12" }, { "fat16.img", "DATA16", "FAT16" }, { "fat32gpt.img", "GPTDISK", "FAT32" } };
    u8 pat[32768];
    for (int i = 0; i < 32768; i++) pat[i] = (u8)i;
    const char *readme = "Halcyon test disk.\r\nThis file has an 8.3 name.\r\n";
    for (size_t i = 0; i < ARRAY_SIZE(d); i++) {
        img_t im;
        snprintf(im.path, sizeof im.path, "%s/%s", dir, d[i].file);
        im.off = 0;
        hdisk_t *h = disk_attach(&im, 512);
        char L = letter_of(h);
        CHECK(L, "%s not mounted", d[i].file);
        if (L) {
            volume_t *v = vfs_volume_by_letter(L);
            CHECK(!strcmp(v->label, d[i].label) && !strcmp(v->fs_name, d[i].fs), "%s: %s %s", d[i].file, v->label, v->fs_name);
            v_cmp(P(L, "README.TXT"), (const u8 *)readme, (u32)strlen(readme));
            v_cmp(P(L, "SUBDIR/pattern.bin"), pat, sizeof pat);
            CHECK(v_exists(P(L, "Folder With Spaces/Nested Dir/deep file.txt")), "deep file");
            CHECK(v_listed(P(L, ""), "Long File Name Example.txt", NULL), "long name");
            CHECK(v_listed(P(L, ""), "Mixed Case.Data", NULL), "mixed case name");
            EXPECT(v_count(P(L, "")), 5);
        }
        disk_detach(h);
    }
}

/* ------------------------------------------------------------------ */
/* hostile volumes: corrupted / malicious partition tables and FAT      */
/* volumes, built by patching the fuzzer seeds (fuzz_seeds.sh). Every   */
/* image goes through the fuzz harness (fuzz_fs.c: mount, walk, mutate, */
/* unmount; aborts on any sanitizer report, leak, hang, ...), some get  */
/* specific checks on top.                                              */

static u16 g16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 g32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u64 g64(const u8 *p) { return g32(p) | ((u64)g32(p + 4) << 32); }
static void s16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void s32(u8 *p, u32 v) { s16(p, v); s16(p + 2, v >> 16); }
static void s64(u8 *p, u64 v) { s32(p, (u32)v); s32(p + 4, (u32)(v >> 32)); }

static u32 h_crc32(const u8 *p, size_t n) {
    u32 c = 0xFFFFFFFF;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & -(c & 1));
    }
    return ~c;
}

typedef struct { u8 *b; size_t n; u8 cfg; } himg_t;

static char g_seed_dir[1100];

/* load a seed (config byte + image) into a fresh buffer */
static himg_t h_seed(const char *name) {
    char path[1300];
    himg_t h = { NULL, 0, 0 };
    snprintf(path, sizeof path, "%s/%s", g_seed_dir, name);
    size_t n = 0;
    u8 *b = host_read(path, &n);
    CHECK(b && n > 512, "seed %s missing", name);
    if (!b || n <= 512) { free(b); return h; }
    h.cfg = b[0];
    memmove(b, b + 1, n - 1);
    h.b = b;
    h.n = n - 1;
    return h;
}

static himg_t h_copy(const himg_t *src) {
    himg_t h = *src;
    h.b = malloc(src->n);
    memcpy(h.b, src->b, src->n);
    return h;
}

static void h_free(himg_t *h) { free(h->b); h->b = NULL; }

/* run an image through the fuzz harness (read-write, read-only, another
 * operation variant, then with I/O errors injected at a few points);
 * returns the number of mounted volumes */
static int h_run(const char *what, himg_t *h, int want_vols) {
    int nv = -1;
    u8 base = h->cfg & FZ_CFG_4K;
    const u8 cfgs[] = { base, (u8)(base | FZ_CFG_RO), (u8)(base | (0x0D << 2)),
                        (u8)(base | FZ_CFG_FAULTS | (3 << 2)), (u8)(base | FZ_CFG_FAULTS | FZ_CFG_BOOT | (9 << 2)),
                        (u8)(base | FZ_CFG_FAULTS | (0x1F << 2)) };
    for (size_t i = 0; i < ARRAY_SIZE(cfgs); i++) {
        if (shim_verbose) printf("    hostile: %s (cfg %02x)\n", what, cfgs[i]);
        int r = fuzz_fs_image(cfgs[i], h->b, h->n);
        if (!i) nv = r;
        else if (!(cfgs[i] & FZ_CFG_FAULTS))
            CHECK(r == nv, "%s: %d volumes with cfg %02x, %d without", what, r, cfgs[i], nv);
    }
    if (want_vols >= 0) CHECK(nv == want_vols, "%s: %d volume(s) mounted, want %d", what, nv, want_vols);
    else g_checks++;
    return nv;
}

/* FAT geometry of the file system at byte offset off */
typedef struct { u32 off, bps, spc, rsv, nfats, rootent, tot, fatsz, type, root0, data0, rootclus, nclus; } fsg_t;

static void h_fsg(const himg_t *h, u32 off, fsg_t *g) {
    const u8 *s = h->b + off;
    g->off = off;
    g->bps = g16(s + 11); g->spc = s[13]; g->rsv = g16(s + 14); g->nfats = s[16]; g->rootent = g16(s + 17);
    g->tot = g16(s + 19) ? g16(s + 19) : g32(s + 32);
    g->fatsz = g16(s + 22) ? g16(s + 22) : g32(s + 36);
    g->root0 = g->rsv + g->nfats * g->fatsz;
    g->data0 = g->root0 + (g->rootent * 32 + g->bps - 1) / g->bps;
    g->nclus = (g->tot - g->data0) / g->spc;
    g->type = g->nclus < 4085 ? 12 : g->nclus < 65525 ? 16 : 32;
    g->rootclus = g->type == 32 ? g32(s + 44) : 0;
}

static u8 *h_sec(himg_t *h, fsg_t *g, u32 sec) { return h->b + g->off + (size_t)sec * g->bps; }
static u8 *h_clus(himg_t *h, fsg_t *g, u32 c) { return h_sec(h, g, g->data0 + (c - 2) * g->spc); }

static u32 h_fat_get(himg_t *h, fsg_t *g, u32 c) {
    u8 *f = h_sec(h, g, g->rsv);
    if (g->type == 12) { u32 v = g16(f + c + c / 2); return (c & 1) ? v >> 4 : v & 0xFFF; }
    if (g->type == 16) return g16(f + c * 2);
    return g32(f + c * 4) & 0x0FFFFFFF;
}

static void h_fat_set(himg_t *h, fsg_t *g, u32 c, u32 v) {
    for (u32 i = 0; i < g->nfats; i++) {
        u8 *f = h_sec(h, g, g->rsv + i * g->fatsz);
        if (g->type == 12) {
            u8 *p = f + c + c / 2;
            u32 o = g16(p);
            s16(p, (c & 1) ? (o & 0x000F) | (v << 4) : (o & 0xF000) | (v & 0xFFF));
        } else if (g->type == 16) {
            s16(f + c * 2, v);
        } else {
            s32(f + c * 4, v);
        }
    }
}

/* replace the end-of-chain marker of every chain with target(c) */
static void h_chain_ends(himg_t *h, fsg_t *g, u32 (*target)(fsg_t *g, u32 c)) {
    u32 eoc = g->type == 12 ? 0xFF8 : g->type == 16 ? 0xFFF8 : 0x0FFFFFF8;
    u32 maxc = MIN(g->nclus + 2, (u32)((u64)g->fatsz * g->bps * 8 / g->type));
    for (u32 c = 2; c < maxc; c++)
        if (h_fat_get(h, g, c) >= eoc) h_fat_set(h, g, c, target(g, c));
}
static u32 t_self(fsg_t *g, u32 c) { return c; }
static u32 t_two(fsg_t *g, u32 c) { return 2; }
static u32 t_past(fsg_t *g, u32 c) { return g->nclus + 2; }
static u32 t_one(fsg_t *g, u32 c) { return 1; }
static u32 t_zero(fsg_t *g, u32 c) { return 0; }
static u32 t_bad(fsg_t *g, u32 c) { return g->type == 12 ? 0xFF7 : g->type == 16 ? 0xFFF7 : 0x0FFFFFF7; }
static u32 t_rsvd(fsg_t *g, u32 c) { return g->type == 12 ? 0xFF0 : g->type == 16 ? 0xFFF0 : 0x0FFFFFF0; }

/* directory entry by raw 8.3 name in directory dirclus (0 = root), first cluster only */
static u8 *h_dirent(himg_t *h, fsg_t *g, u32 dirclus, const char *n11) {
    u8 *d;
    u32 cnt;
    if (!dirclus && g->type != 32) { d = h_sec(h, g, g->root0); cnt = g->rootent; }
    else { d = h_clus(h, g, dirclus ? dirclus : g->rootclus); cnt = g->spc * g->bps / 32; }
    for (u32 i = 0; i < cnt; i++) if (!memcmp(d + 32 * i, n11, 11)) return d + 32 * i;
    return NULL;
}
static u32 h_ent_clus(fsg_t *g, const u8 *de) { return g16(de + 26) | (g->type == 32 ? (u32)g16(de + 20) << 16 : 0); }
static void h_ent_set_clus(u8 *de, u32 c) { s16(de + 26, c); s16(de + 20, c >> 16); }
static u32 h_dir_clus(himg_t *h, fsg_t *g, const char *n11) {
    u8 *de = h_dirent(h, g, 0, n11);
    CHECK(de, "%.11s not found", n11);
    return de ? h_ent_clus(g, de) : 0;
}

/* first free root directory slot (FAT12/16) */
static u8 *h_root_free(himg_t *h, fsg_t *g) {
    u8 *d = h_sec(h, g, g->root0);
    for (u32 i = 0; i < g->rootent; i++) if (d[32 * i] == 0) return d + 32 * i;
    return NULL;
}

static u8 h_lfn_chk(const char *n11) {
    u8 s = 0;
    for (int i = 0; i < 11; i++) s = (u8)(((s & 1) << 7) + (s >> 1) + (u8)n11[i]);
    return s;
}

/* an LFN slot: ord byte, up to 13 chars of name, checksum */
static void h_lfn(u8 *p, u8 ord, const char *part, u8 chk) {
    static const u8 pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    memset(p, 0, 32);
    p[0] = ord;
    p[11] = 0x0F;
    p[13] = chk;
    int len = (int)strlen(part);
    for (int i = 0; i < 13; i++) s16(p + pos[i], i < len ? (u8)part[i] : i == len ? 0 : 0xFFFF);
}

static void h_short(u8 *p, const char *n11, u8 attr, u32 clus, u32 size) {
    memset(p, 0, 32);
    memcpy(p, n11, 11);
    p[11] = attr;
    h_ent_set_clus(p, clus);
    s32(p + 28, size);
}

/* mount an image through a file-backed device, for checks beyond the harness */
static hdisk_t *h_attach(himg_t *h, int readonly) {
    const char *path = wpath("hostile.img");
    host_write(path, h->b, h->n);
    hdisk_t *d = calloc(1, sizeof *d);
    d->fd = open(path, readonly ? O_RDONLY : O_RDWR);
    snprintf(d->bd.name, sizeof d->bd.name, "hz%d", g_disk_no++);
    d->bd.sector_size = (h->cfg & FZ_CFG_4K) ? 4096 : 512;
    d->bd.sector_count = h->n / d->bd.sector_size;
    d->bd.readonly = (u8)readonly;
    d->bd.read = hd_read;
    d->bd.write = hd_write;
    int r = blockdev_register(&d->bd);
    CHECK(r == 0, "blockdev_register = %d", r);
    return d;
}

/* read a whole file; returns the last result (0 at EOF or an error), bytes in *got */
static int h_read_all(const char *path, u64 *got) {
    static u8 buf[65536];
    vfs_file_t *f;
    *got = 0;
    int r = vfs_open(path, VFS_O_READ, &f);
    if (r) return r;
    while ((r = vfs_read(f, buf, sizeof buf)) > 0) *got += (u32)r;
    vfs_close(f);
    return r;
}

#define CASE(desc) for (int _once = (h = h_copy(&base), snprintf(what, sizeof what, "%s: %s", tag, desc), 1); \
                        _once; _once = 0, h_free(&h))

static void t_hostile_fat(const char *tag) {
    himg_t base = h_seed(tag), h;
    if (!base.b) return;
    fsg_t g;
    h_fsg(&base, 0, &g);
    char what[200], desc[80];
    u8 *de;
    CASE("unmodified") h_run(what, &h, 1);

    /* cluster chains: loops, out of range, reserved values, free */
    struct { const char *d; u32 (*t)(fsg_t *, u32); } ends[] = {
        { "chains end in a self loop", t_self }, { "chains loop back to cluster 2", t_two },
        { "chains run past the last cluster", t_past }, { "chains point to cluster 1", t_one },
        { "chains point to a free cluster", t_zero }, { "chains point to a bad cluster", t_bad },
        { "chains point to a reserved value", t_rsvd } };
    for (size_t i = 0; i < ARRAY_SIZE(ends); i++) {
        CASE(ends[i].d) {
            h_chain_ends(&h, &g, ends[i].t);
            /* and a file claiming far more data than its chain holds */
            u32 sub = h_dir_clus(&h, &g, "SUBDIR     ");
            if (sub && (de = h_dirent(&h, &g, sub, "MULTI   BIN"))) s32(de + 28, 0xFFFFFFF0);
            if (h_run(what, &h, -1) == 1) {
                hdisk_t *d = h_attach(&h, 0);
                u64 got;
                int r = h_read_all(P(letter_of(d), "SUBDIR/MULTI.BIN"), &got);
                CHECK(r == E_IO || r == E_NOENT || r == E_NOTDIR, "%s: reading MULTI.BIN = %d (%s), want E_IO",
                      what, r, ename(r));
                CHECK(got <= h.n, "%s: read %lu bytes from a %lu byte disk", what, (unsigned long)got,
                      (unsigned long)h.n);
                disk_detach(d);
            }
        }
    }
    CASE("file size exceeds its chain") {
        de = h_dirent(&h, &g, 0, "README  TXT");
        CHECK(de, "README.TXT not found");
        if (de) s32(de + 28, 0xFFFFFFFF);
        h_run(what, &h, 1);
        hdisk_t *d = h_attach(&h, 0);
        u64 got;
        EXPECT(h_read_all(P(letter_of(d), "README.TXT"), &got), E_IO);
        disk_detach(d);
    }
    CASE("directory entries with out-of-range clusters") {
        if ((de = h_dirent(&h, &g, 0, "SUBDIR     "))) h_ent_set_clus(de, g.nclus + 100);
        if ((de = h_dirent(&h, &g, 0, "README  TXT"))) h_ent_set_clus(de, 1);
        h_run(what, &h, 1);
        hdisk_t *d = h_attach(&h, 0);
        char L = letter_of(d);
        vfs_dirent_t *ents;
        EXPECT(vfs_list(P(L, "SUBDIR"), &ents), E_IO);
        u64 got;
        EXPECT(h_read_all(P(L, "README.TXT"), &got), E_IO);
        disk_detach(d);
    }
    CASE("directory cluster chain loops onto itself") {
        u32 sub = h_dir_clus(&h, &g, "SUBDIR     ");
        if (sub) {
            h_fat_set(&h, &g, sub, sub);
            /* fill the directory so that a scan never hits an end marker */
            u8 *d = h_clus(&h, &g, sub);
            for (u32 i = 4; i < g.spc * g.bps / 32; i++) {
                char n11[16];
                snprintf(n11, sizeof n11, "F%07uTXT", i);
                h_short(d + 32 * i, n11, 0x20, 0, 0);
            }
            h_run(what, &h, 1);
            hdisk_t *dk = h_attach(&h, 0);
            char L = letter_of(dk);
            int n = v_count(P(L, "SUBDIR"));
            CHECK(n > 0 && n <= 65536, "looping directory: %d entries", n);
            EXPECT(vfs_mkdir(P(L, "SUBDIR/one more")), E_NOSPC);
            disk_detach(dk);
        }
    }
    CASE("'.' and '..' entries pointing anywhere") {
        u32 sub = h_dir_clus(&h, &g, "SUBDIR     "), lng = h_dir_clus(&h, &g, "LONGDI~1   ");
        if (sub && lng) {
            u8 *d = h_clus(&h, &g, sub);
            h_ent_set_clus(d, 2);            /* "." -> cluster 2 */
            h_ent_set_clus(d + 32, sub);     /* ".." -> itself */
            d = h_clus(&h, &g, lng);
            h_ent_set_clus(d + 32, 1);       /* ".." -> invalid */
            h_run(what, &h, 1);
            hdisk_t *dk = h_attach(&h, 0);
            char L = letter_of(dk);
            /* moving a directory into SUBDIR walks its looping ".." chain */
            int r = vfs_rename(P(L, "Long Directory Name/Nested"), P(L, "SUBDIR/Nested"));
            CHECK(r == 0 || r == E_IO, "rename into a dir with a '..' loop = %d (%s)", r, ename(r));
            /* and into "Long Directory Name" its invalid ".." */
            r = vfs_rename(P(L, "SUBDIR"), P(L, "Long Directory Name/SUBDIR"));
            CHECK(r == E_IO, "rename into a dir with an invalid '..' = %d (%s)", r, ename(r));
            disk_detach(dk);
        }
    }
    CASE("moving a directory with an invalid cluster") {
        if ((de = h_dirent(&h, &g, 0, "LONGDI~1   "))) h_ent_set_clus(de, 1);
        h_run(what, &h, 1);
        hdisk_t *d = h_attach(&h, 0);
        char L = letter_of(d);
        EXPECT(vfs_rename(P(L, "Long Directory Name"), P(L, "SUBDIR/moved")), E_IO);
        disk_detach(d);
    }
    if (g.type != 32) {
        CASE("hostile names: LFN sequences, checksums, path characters") {
            u8 *p = h_root_free(&h, &g);
            CHECK(p && p + 32 * 24 <= h_sec(&h, &g, g.data0), "no room in the root directory");
            if (p && p + 32 * 24 <= h_sec(&h, &g, g.data0)) {
                const char *n1 = "BADSEQ  TXT", *n2 = "BADCHK  TXT", *n3 = "DOTDOT  TXT", *n4 = "SLASH   TXT",
                           *n5 = "DRIVE   TXT", *n6 = "BIGORD  TXT", *n7 = "EMPTYLFNTXT";
                h_lfn(p, 0x43, "missing", h_lfn_chk(n1)); p += 32;     /* 3, then 1: 2 is missing */
                h_lfn(p, 0x01, "sequence", h_lfn_chk(n1)); p += 32;
                h_short(p, n1, 0x20, 0, 0); p += 32;
                h_lfn(p, 0x41, "wrong checksum", 0x55); p += 32;
                h_short(p, n2, 0x20, 0, 0); p += 32;
                h_lfn(p, 0x41, "..", h_lfn_chk(n3)); p += 32;
                h_short(p, n3, 0x10, 0, 0); p += 32;
                h_lfn(p, 0x41, "../../x", h_lfn_chk(n4)); p += 32;
                h_short(p, n4, 0x20, 0, 0); p += 32;
                h_lfn(p, 0x41, "C:", h_lfn_chk(n5)); p += 32;
                h_short(p, n5, 0x20, 0, 0); p += 32;
                h_lfn(p, 0x40 | 25, "huge ordinal", h_lfn_chk(n6)); p += 32;
                h_short(p, n6, 0x20, 0, 0); p += 32;
                h_lfn(p, 0x41, "", h_lfn_chk(n7)); p += 32;
                h_short(p, n7, 0x20, 0, 0); p += 32;
                h_short(p, "A/B:C\\D EXT", 0x20, 0, 0); p += 32;    /* path characters in a short name */
                h_short(p, "           ", 0x20, 0, 0); p += 32;    /* blank short name */
                h_lfn(p, 0x42, "orphan", 0x12); p += 32;            /* orphaned LFN slots */
                h_lfn(p, 0x01, "slots", 0x12); p += 32;
                p[0] = 0xE5;
                h_run(what, &h, 1);
                hdisk_t *d = h_attach(&h, 0);
                char L = letter_of(d);
                CHECK(v_listed(P(L, ""), "BADSEQ.TXT", NULL), "bad LFN sequence: short name not used");
                CHECK(v_listed(P(L, ""), "BADCHK.TXT", NULL), "bad LFN checksum: short name not used");
                CHECK(v_listed(P(L, ""), "DOTDOT.TXT", NULL), "LFN '..' not rejected");
                CHECK(v_listed(P(L, ""), "SLASH.TXT", NULL), "LFN with '/' not rejected");
                CHECK(v_listed(P(L, ""), "DRIVE.TXT", NULL), "LFN 'C:' not rejected");
                CHECK(v_listed(P(L, ""), "BIGORD.TXT", NULL), "LFN ordinal 25 not rejected");
                CHECK(v_listed(P(L, ""), "EMPTYLFN.TXT", NULL), "empty LFN not rejected");
                CHECK(v_listed(P(L, ""), "A_B_C_D.EXT", NULL), "short name path characters not replaced");
                CHECK(v_listed(P(L, ""), "_", NULL), "blank short name not shown as '_'");
                CHECK(!v_listed(P(L, ""), "..", NULL), "'..' listed");
                /* the entries are usable under their safe names */
                EXPECT(vfs_write_file(P(L, "A_B_C_D.EXT"), "x", 1), 1);
                OK(vfs_unlink(P(L, "SLASH.TXT")));
                disk_detach(d);
            }
        }
    }

    /* BPB fields */
    struct { const char *d; int off, width; u32 val; int want; } bpb[] = {
        { "bytes/sector 0", 11, 2, 0, 0 }, { "bytes/sector 1000", 11, 2, 1000, 0 },
        { "bytes/sector 256", 11, 2, 256, 0 }, { "bytes/sector 8192", 11, 2, 8192, 0 },
        { "sectors/cluster 0", 13, 1, 0, 0 }, { "sectors/cluster 3", 13, 1, 3, 0 },
        { "sectors/cluster 128", 13, 1, 128, -1 }, { "reserved sectors 0", 14, 2, 0, 0 },
        { "FAT count 0", 16, 1, 0, 0 }, { "FAT count 3", 16, 1, 3, 0 },
        { "media byte 0x12", 21, 1, 0x12, 0 },
        { "total sectors 0xFFFFFFFF", 32, 4, 0xFFFFFFFF, -1 },
        { "total sectors 3x the device", 32, 4, 0, -1 },
        { "total sectors below the metadata", 32, 4, 3, 0 },
        { "reserved sectors past the device", 14, 2, 0xFFFF, 0 },
    };
    for (size_t i = 0; i < ARRAY_SIZE(bpb); i++) {
        CASE(bpb[i].d) {
            u8 *s = h.b + bpb[i].off;
            u32 v = bpb[i].val;
            if (bpb[i].off == 32) {
                if (!v) v = (u32)(h.n / g.bps * 3);
                s16(h.b + 19, 0);          /* use the 32-bit field */
            }
            if (bpb[i].width == 1) *s = (u8)v;
            else if (bpb[i].width == 2) s16(s, v);
            else s32(s, v);
            h_run(what, &h, bpb[i].want);
        }
    }
    if (g.type != 32) {
        CASE("root entry count 0") { s16(h.b + 17, 0); h_run(what, &h, 0); }
        CASE("root entry count 1") { s16(h.b + 17, 1); h_run(what, &h, -1); }
        CASE("FAT size 0xFFFF") { s16(h.b + 22, 0xFFFF); h_run(what, &h, 0); }
    } else {
        u32 rc[] = { 0, 1, g.nclus + 2, 0x0FFFFFFF, 0xFFFFFFFF };
        for (size_t i = 0; i < ARRAY_SIZE(rc); i++) {
            snprintf(desc, sizeof desc, "root cluster %u", rc[i]);
            CASE(desc) { s32(h.b + 44, rc[i]); h_run(what, &h, 0); }
        }
        CASE("root cluster past the end of the device") { s32(h.b + 44, g.nclus + 1); h_run(what, &h, 0); }
        CASE("FAT size 0xFFFFFFFF") { s32(h.b + 36, 0xFFFFFFFF); h_run(what, &h, 0); }
        CASE("FAT size wrapping the layout") { h.b[16] = 2; s32(h.b + 36, 0x80000001); h_run(what, &h, 0); }
        CASE("FSInfo hints out of range") {
            u8 *fsi = h.b + g16(h.b + 48) * g.bps;
            s32(fsi + 488, 0xFFFFFFF0);
            s32(fsi + 492, 0xFFFFFFFF);
            h_run(what, &h, 1);
            /* read-only device: counting the free clusters must not leave
             * a dirty FSInfo sector behind that can never be written */
            hdisk_t *d = h_attach(&h, 1);
            char L = letter_of(d);
            u64 t, fr;
            OK(vfs_statfs(L, &t, &fr));
            vfs_file_t *fh;
            if (vfs_open(P(L, "README.TXT"), VFS_O_READ, &fh) == 0) EXPECT(vfs_close(fh), 0);
            EXPECT(vfs_sync_all(), 0);
            disk_detach(d);
        }
        CASE("FSInfo sector past the reserved area") { s16(h.b + 48, 0xFFF0); h_run(what, &h, 1); }
        CASE("active FAT out of range") { s16(h.b + 40, 0x8F); h_run(what, &h, 1); }
    }
    h_free(&base);
}

/* MBR / EBR chains on the mbr_extended seed (primary at 8, extended at
 * 320 holding logicals at 328 and 640) */
static void t_hostile_mbr(void) {
    const char *tag = "mbr";
    himg_t base = h_seed("mbr_extended"), h;
    if (!base.b) return;
    char what[200];
    CHECK(base.b[446 + 16 + 4] == 0x05 && g32(base.b + 446 + 16 + 8) == 320, "unexpected seed layout");
    u32 ebr1 = 320 * 512;
    u32 ebr2 = (320 + g32(base.b + ebr1 + 462 + 8)) * 512;
    CHECK(ebr2 > ebr1 && ebr2 < base.n, "second EBR");
    CASE("unmodified") h_run(what, &h, 3);
    CASE("EBR points to itself") { s32(h.b + ebr1 + 462 + 8, 0); h_run(what, &h, 2); }
    CASE("EBR loops back") { s32(h.b + ebr2 + 462 + 8, 0); h_run(what, &h, 3); }
    CASE("EBR points outside the disk") { s32(h.b + ebr1 + 462 + 8, 0xFFFFFF00); h_run(what, &h, 2); }
    CASE("logical partition outside the disk") { s32(h.b + ebr1 + 446 + 8, 0xFFFFFFF0); h_run(what, &h, 2); }
    CASE("logical partition huge") { s32(h.b + ebr2 + 446 + 12, 0xFFFFFFFF); h_run(what, &h, 3); }
    CASE("partition beyond the end of the disk") { s32(h.b + 446 + 12, 0xFFFFFFF0); h_run(what, &h, -1); }
    CASE("duplicate primary entries") {
        memcpy(h.b + 446 + 32, h.b + 446, 16);
        memcpy(h.b + 446 + 48, h.b + 446, 16);
        h_run(what, &h, 3);
    }
    CASE("primary overlapping a logical partition") {
        memcpy(h.b + 446 + 32, h.b + 446, 16);
        s32(h.b + 446 + 32 + 8, 500);
        h_run(what, &h, 3);
    }
    CASE("zero-length partition") {
        memcpy(h.b + 446 + 32, h.b + 446, 16);
        s32(h.b + 446 + 32 + 12, 0);
        h_run(what, &h, -1);
    }
    CASE("partition starting at LBA 0") {
        memcpy(h.b + 446 + 32, h.b + 446, 16);
        s32(h.b + 446 + 32 + 8, 0);
        h_run(what, &h, -1);
    }
    CASE("extended partition over the primary") { s32(h.b + 446 + 16 + 8, 8); h_run(what, &h, -1); }
    CASE("long EBR chain") {
        /* 300 EBRs, each followed by a one-sector logical partition */
        memset(h.b + 446, 0, h.n - 446);
        h.b[510] = 0x55; h.b[511] = 0xAA;
        u8 *e = h.b + 446;
        e[4] = 0x05; s32(e + 8, 2); s32(e + 12, 1000);
        for (u32 k = 0; k < 300 && (size_t)(2 + 3 * (k + 1)) * 512 <= h.n; k++) {
            u8 *b = h.b + (size_t)(2 + 3 * k) * 512;
            b[446 + 4] = 0x01; s32(b + 446 + 8, 1); s32(b + 446 + 12, 1);
            b[462 + 4] = 0x05; s32(b + 462 + 8, 3 * (k + 1)); s32(b + 462 + 12, 3);
            b[510] = 0x55; b[511] = 0xAA;
        }
        h_run(what, &h, 0);
    }
    h_free(&base);
}

static void h_gpt_fix(himg_t *h, u8 **hdr) {
    const u32 ss = 512;
    for (int i = 0; i < 2; i++) {
        u8 *hd = hdr[i];
        if (memcmp(hd, "EFI PART", 8)) continue;
        u64 n = (u64)g32(hd + 80) * g32(hd + 84), lba = g64(hd + 72);
        if (n <= 1024 * 1024 && lba * ss + n <= h->n) s32(hd + 88, h_crc32(h->b + lba * ss, (size_t)n));
        u32 hs = g32(hd + 12);
        if (hs < 92 || hs > ss) continue;
        s32(hd + 16, 0);
        s32(hd + 16, h_crc32(hd, hs));
    }
}

/* GPT on the gpt_two seed (partitions at 40..439 and 480..1079); edits go
 * to both the primary and the backup header / entry array */
static void t_hostile_gpt(void) {
    const char *tag = "gpt";
    himg_t base = h_seed("gpt_two"), h;
    if (!base.b) return;
    char what[200];
    const u32 ss = 512;
    u64 alt = g64(base.b + ss + 32);
    CHECK(alt == base.n / ss - 1, "unexpected seed layout");
    u64 ent[2] = { g64(base.b + ss + 72), g64(base.b + alt * ss + 72) };
    u8 *hdr[2];
#define EACH for (int k = 0; k < 2; k++)
#define ENT(i) (h.b + ent[k] * ss + (i) * 128)
#define FIX() (hdr[0] = h.b + ss, hdr[1] = h.b + alt * ss, h_gpt_fix(&h, hdr))
#define HDR(k) (h.b + ((k) ? alt : 1) * ss)
    CASE("unmodified") h_run(what, &h, 2);
    CASE("entry count 0xFFFFFFFF") { EACH s32(HDR(k) + 80, 0xFFFFFFFF); FIX(); h_run(what, &h, 2); }
    CASE("entry count 2^20, entry size 4096 (32-bit wrap)") {
        EACH { s32(HDR(k) + 80, 1u << 20); s32(HDR(k) + 84, 4096); }
        FIX();
        h_run(what, &h, 0);     /* 256 entries of 4 KiB don't fit the disk */
    }
    CASE("entry count 1000") { EACH s32(HDR(k) + 80, 1000); FIX(); h_run(what, &h, 2); }
    CASE("entry size 0") { EACH s32(HDR(k) + 84, 0); FIX(); h_run(what, &h, 0); }
    CASE("entry size 0xFFFFFFF8") { EACH s32(HDR(k) + 84, 0xFFFFFFF8); FIX(); h_run(what, &h, 0); }
    CASE("entry array outside the disk") { EACH s64(HDR(k) + 72, 0xFFFFFFFFFFull); FIX(); h_run(what, &h, 0); }
    CASE("entry array at the last sector") { EACH s64(HDR(k) + 72, h.n / ss - 1); FIX(); h_run(what, &h, 0); }
    CASE("header size 0xFFFFFFFF") { EACH s32(HDR(k) + 12, 0xFFFFFFFF); h_run(what, &h, 0); }
    CASE("overlapping partitions") {
        EACH { s64(ENT(1) + 32, 100); s64(ENT(1) + 40, 700); }
        FIX();
        h_run(what, &h, 1);
    }
    CASE("identical partitions") { EACH memcpy(ENT(1), ENT(0), 128); FIX(); h_run(what, &h, 1); }
    CASE("partition out of range") { EACH s64(ENT(1) + 40, 0xFFFFFFFFFFFFull); FIX(); h_run(what, &h, 1); }
    CASE("zero-length partition (end before start)") {
        EACH { s64(ENT(1) + 32, 900); s64(ENT(1) + 40, 899); }
        FIX();
        h_run(what, &h, 1);
    }
    CASE("partition over the GPT itself") {
        EACH { s64(HDR(k) + 40, 0); s64(ENT(1) + 32, 0); s64(ENT(1) + 40, 2000); }
        FIX();
        h_run(what, &h, -1);
    }
    CASE("primary header destroyed, backup absurd") {
        s32(HDR(1) + 80, 1u << 20);
        s32(HDR(1) + 84, 4096);
        FIX();
        memset(HDR(0), 0, 92);
        h_run(what, &h, 0);
    }
#undef HDR
#undef FIX
#undef ENT
#undef EACH
    h_free(&base);
}
#undef CASE

/* minimized fuzzer crash inputs (fuzz_regress/NAME.gz: config byte + image) */
static void t_fuzz_regress(void) {
    char path[1300];
    int n = 0;
    snprintf(path, sizeof path, "%s", wpath("regress"));
    sh("rm -rf %s && mkdir -p %s && for f in fuzz_regress/*.gz; do [ -e \"$f\" ] || continue; "
       "gzip -dc \"$f\" > %s/$(basename \"$f\" .gz); done", q(path), q(path), q(path));
    FILE *ls = popen("ls fuzz_regress 2>/dev/null", "r");
    char name[512];
    while (ls && fgets(name, sizeof name, ls)) {
        size_t len = strlen(name);
        while (len && (name[len - 1] == '\n' || name[len - 1] == '\r')) name[--len] = 0;
        if (len < 4 || strcmp(name + len - 3, ".gz")) continue;
        name[len - 3] = 0;
        char file[1900];
        snprintf(file, sizeof file, "%s/%s", path, name);
        size_t sz = 0;
        u8 *b = host_read(file, &sz);
        CHECK(b && sz > 1, "fuzz regression input %s unreadable", name);
        if (shim_verbose) printf("    regress: %s\n", name);
        if (b && sz > 1) fuzz_fs_run(b, sz);
        free(b);
        n++;
    }
    if (ls) pclose(ls);
    CHECK(n > 0, "no fuzz regression inputs found");
}

static void s_hostile(void) {
    snprintf(g_seed_dir, sizeof g_seed_dir, "%s", wpath("seeds"));
    int r = sh("rm -rf %s && sh fuzz_seeds.sh %s", q(g_seed_dir), q(g_seed_dir));
    CHECK(r == 0, "fuzz_seeds.sh failed");
    if (r) return;
    t_hostile_fat("super_fat12");
    t_hostile_fat("super_fat16");
    t_hostile_fat("super_fat32");
    t_hostile_mbr();
    t_hostile_gpt();
    t_fuzz_regress();
}

#include "isotest.c"

typedef struct { const char *name; void (*fn)(void); } scen_t;

static void s_paths(void) { t_paths(); }

static void s_fat12(void) {
    img_t im;
    img_super(&im, "fat12_read.img", 12, 1440, "FLOPPY", NULL);
    t_read_mtools("fat12 read", &im, 512, "FLOPPY");
    img_super(&im, "fat12_write.img", 12, 1440, "FLOPPY", NULL);
    t_write("fat12 write", &im, 512);
    t_rename("fat12 rename", &im, 512);
    t_delete("fat12 delete", &im, 512);
    img_super(&im, "fat12_full.img", 12, 1440, "FLOPPY", NULL);
    t_diskfull("fat12 full", &im, 512);
    img_super(&im, "fat12_root.img", 12, 1440, "FLOPPY", NULL);
    t_rootfull("fat12 root 8.3", &im, 512, 0);
    img_super(&im, "fat12_rootlfn.img", 12, 1440, "FLOPPY", NULL);
    t_rootfull("fat12 root lfn", &im, 512, 1);
    img_super(&im, "fat12_many.img", 12, 1440, "FLOPPY", NULL);
    t_many("fat12 many", &im, 512, 150);
    img_super(&im, "fat12_ill.img", 12, 1440, "FLOPPY", NULL);
    t_illegal("fat12 illegal", &im, 512);
}

/* Unicode long names: UTF-8 in the VFS, UTF-16 on disk. mtools (run in a
 * UTF-8 locale) is the cross-check; it cannot handle characters outside the
 * Basic Multilingual Plane, so those are checked through the driver only. */
static void t_unicode(const char *tag, img_t *im, u32 ss) {
    printf("  %s\n", tag);
    u8 *d1 = pat_alloc(3000, 41), *d2 = pat_alloc(70000, 42), *d3 = pat_alloc(10, 43);
    /* written by mtools, read by the driver */
    mt_put(im, "Café.txt", d1, 3000);
    sh("mmd -i %s %s", mt(im), mp("Ünïcödé Dir"));
    mt_put(im, "Ünïcödé Dir/Привет мир.txt", d2, 70000);
    mt_put(im, "日本語のファイル.txt", d3, 10);
    hdisk_t *h = disk_attach(im, ss);
    char L = letter_of(h);
    CHECK(L, "not mounted");
    if (!L) { disk_detach(h); return; }
    CHECK(v_listed(P(L, ""), "Café.txt", NULL), "Café.txt not listed");
    CHECK(v_listed(P(L, ""), "Ünïcödé Dir", NULL), "Ünïcödé Dir not listed");
    CHECK(v_listed(P(L, ""), "日本語のファイル.txt", NULL), "CJK name not listed");
    v_cmp(P(L, "Café.txt"), d1, 3000);
    v_cmp(P(L, "CAFÉ.TXT"), d1, 3000);                         /* case-insensitive, Latin-1 */
    v_cmp(P(L, "ünïcödé dir/ПРИВЕТ МИР.txt"), d2, 70000);       /* ... and Cyrillic */
    v_cmp(P(L, "日本語のファイル.txt"), d3, 10);

    /* written by the driver, read by mtools */
    v_put(P(L, "Ελληνικά.txt"), d1, 3000);
    OK(vfs_mkdir(P(L, "Ñandú")));
    v_put(P(L, "Ñandú/niño.txt"), d3, 10);
    v_put(P(L, "😀 smile.txt"), d3, 10);                         /* surrogate pair on disk */
    v_put(P(L, "中文文件名称很长很长很长很长很长很长.dat"), d2, 70000); /* several LFN slots */
    EXPECT(vfs_open(P(L, "café.TXT"), VFS_O_WRITE | VFS_O_CREATE | VFS_O_EXCL, &(vfs_file_t *){0}), E_EXIST);
    EXPECT(vfs_write_file(P(L, "bad\xFFname.txt"), d3, 10), E_INVAL);   /* not UTF-8 */
    EXPECT(vfs_write_file(P(L, "bad\xC3.txt"), d3, 10), E_INVAL);
    /* 255 UTF-16 units are fine (and 255 bytes of UTF-8 here), 256 are not */
    char nm[300];
    memset(nm, 'u', 255); nm[255] = 0;
    char path[400];
    snprintf(path, sizeof path, "%s", P(L, nm));
    OK(vfs_write_file(path, d3, 10));
    char nm2[300] = "é";                                        /* 2 bytes, 1 unit */
    memset(nm2 + 2, 'v', 254); nm2[256] = 0;                     /* 255 units, 256 bytes */
    snprintf(path, sizeof path, "%c:/%s", L, nm2);
    CHECK(vfs_write_file(path, d3, 10) < 0, "a 256-byte name was accepted");
    OK(vfs_rename(P(L, "Café.txt"), P(L, "Crème brûlée.txt")));
    OK(vfs_rename(P(L, "Crème brûlée.txt"), P(L, "CRÈME BRÛLÉE.TXT")));  /* case only */
    CHECK(v_listed(P(L, ""), "CRÈME BRÛLÉE.TXT", NULL), "Unicode case rename not visible");
    CHECK(v_listed(P(L, ""), "😀 smile.txt", NULL), "emoji name not listed");
    v_cmp(P(L, "😀 SMILE.txt"), d3, 10);
    vfs_sync_all();
    disk_detach(h);
    fsck(im, tag);
    CHECK(mt_listed(im, "", "Ελληνικά.txt"), "mtools: Greek name missing");
    CHECK(mt_listed(im, "Ñandú", "niño.txt"), "mtools: Spanish names missing");
    CHECK(mt_listed(im, "", "CRÈME BRÛLÉE.TXT"), "mtools: renamed file missing");
    mt_cmp(im, "Ελληνικά.txt", d1, 3000);
    mt_cmp(im, "Ñandú/niño.txt", d3, 10);
    mt_cmp(im, "中文文件名称很长很长很长很长很长很长.dat", d2, 70000);
    mt_cmp(im, "CRÈME BRÛLÉE.TXT", d1, 3000);
    /* the emoji file: its short name is plain ASCII */
    h = disk_attach(im, ss);
    L = letter_of(h);
    vfs_dirent_t de;
    CHECK(L && v_listed(P(L, ""), "😀 smile.txt", &de), "emoji name lost after remount");
    if (L) CHECK(de.short_name[0] && (u8)de.short_name[0] < 0x80, "short name of the emoji file: '%s'", de.short_name);
    disk_detach(h);
    free(d1); free(d2); free(d3);
}

static void t_utf8(void) {
    printf("  utf8 helpers\n");
    const char *s = "aé日😀";                       /* 1 + 2 + 3 + 4 bytes */
    const char *p = s;
    u32 c[5];
    for (int i = 0; i < 5; i++) c[i] = utf8_next(&p);
    CHECK(c[0] == 'a' && c[1] == 0xE9 && c[2] == 0x65E5 && c[3] == 0x1F600 && c[4] == 0, "decode %x %x %x %x", c[0], c[1], c[2], c[3]);
    EXPECT(utf8_count(s, 100), 4);
    EXPECT(utf8_count(s, 3), 2);
    EXPECT(utf8_offset(s, 3), 6);
    EXPECT(utf8_prev(s, 10), 6);
    EXPECT(utf8_prev(s, 6), 3);
    EXPECT(utf8_after(s, 10, 3), 6);
    const char *latin1 = "caf\xE9!";                   /* not UTF-8: Latin-1 fallback */
    p = latin1;
    for (int i = 0; i < 5; i++) c[i] = utf8_next(&p);
    CHECK(c[3] == 0xE9 && c[4] == '!', "latin-1 fallback %x %x", c[3], c[4]);
    EXPECT(utf8_prev(latin1, 4), 3);
    CHECK(!utf8_valid(latin1) && utf8_valid(s), "utf8_valid");
    CHECK(!utf8_valid("\xC0\xAF") && !utf8_valid("\xED\xA0\x80") && !utf8_valid("\xF4\x90\x80\x80"), "overlong / surrogate / > 10FFFF accepted");
    CHECK(!utf8_casecmp("ÄÖÜ straße ΣΟΦΊΑ привет", "äöü STRAßE σοφία ПРИВЕТ"), "casecmp");
    CHECK(utf8_casecmp("a", "b") < 0 && utf8_casecmp("é", "f") > 0, "casecmp order");
    u16 u[8];
    EXPECT(utf8_to_utf16("😀x", u, 8), 3);
    CHECK(u[0] == 0xD83D && u[1] == 0xDE00 && u[2] == 'x', "utf16 encode");
    char o[16];
    EXPECT(utf16_to_utf8(u, 3, o, sizeof o), 5);
    CHECK(!strcmp(o, "😀x"), "utf16 round trip");
    u16 lone[2] = { 0xDC00, 'a' };
    EXPECT(utf16_to_utf8(lone, 2, o, sizeof o), -1);
    EXPECT(utf16_to_utf8(u, 3, o, 5), -1);               /* does not fit */
    CHECK(cp437_to_uc(0x82) == 0xE9 && cp437_to_uc(0xE1) == 0xDF, "cp437");
}

static void s_unicode(void) {
    img_t im;
    t_utf8();
    img_mbr(&im, "unicode16.img", 16, 32, "UNI16", 0xCAFE0116);
    t_unicode("fat16 unicode", &im, 512);
    img_mbr(&im, "unicode32.img", 32, 64, "UNI32", 0xCAFE0132);
    t_unicode("fat32 unicode", &im, 512);
}

/* ------------------------------------------------------------------ */
/* formatting                                                           */

/* blank image: an MBR partition of type 0x83 (to see the type change), or
 * no partition table (superfloppy) */
static void img_blank(img_t *im, const char *name, u32 mb, int part, u32 ss) {
    snprintf(im->path, sizeof im->path, "%s", wpath(name));
    unlink(im->path);
    u32 start = part ? 1024 * 1024 / ss : 0, total = (u32)((u64)mb * 1024 * 1024 / ss);
    int r = sh("truncate -s %uM '%s'", mb, im->path);
    if (part)
        /* MBR written directly: entries count device sectors (sfdisk assumes 512) */
        r |= sh("python3 -c 'import struct,sys; f=open(sys.argv[1],\"r+b\"); f.seek(440);"
                " f.write(struct.pack(\"<IH\",0x1234ABCD,0)+struct.pack(\"<B3sB3sII\",0,b\"\\0\\0\\0\",0x83,b\"\\0\\0\\0\",%u,%u)+bytes(48)+b\"\\x55\\xaa\")' '%s'",
                start, total - start, im->path);
    CHECK(r == 0, "blank image %s", name);
    im->off = (u64)start * ss;
    im->len = (u64)(total - start) * ss;
}

static blockdev_t *volume_bd(hdisk_t *h) {
    for (blockdev_t *b = blockdev_first(); b; b = b->next)
        if (b->parent == &h->bd) return b;
    return &h->bd;
}

static int fsck_type(const img_t *im) {
    /* the FAT type fsck.fat reports, via the boot sector's type string */
    u8 bs[512];
    FILE *f = fopen(im->path, "rb");
    if (!f) return 0;
    fseek(f, (long)im->off, SEEK_SET);
    size_t n = fread(bs, 1, 512, f);
    fclose(f);
    if (n != 512) return 0;
    if (!memcmp(bs + 82, "FAT32", 5)) return 32;
    if (!memcmp(bs + 54, "FAT16", 5)) return 16;
    if (!memcmp(bs + 54, "FAT12", 5)) return 12;
    return 0;
}

static void t_format(const char *tag, u32 mb, int part, u32 ss, const char *fs, int want_type, u32 cluster) {
    printf("  %s\n", tag);
    img_t im;
    char nm[64];
    snprintf(nm, sizeof nm, "fmt_%s_%u_%u.img", fs, mb, ss);
    img_blank(&im, nm, mb, part, ss);
    hdisk_t *h = disk_attach(&im, ss);
    CHECK(!letter_of(h), "blank disk mounted");
    blockdev_t *bd = volume_bd(h);
    EXPECT(vfs_format(bd, fs, "Fmt Tést", cluster), 0);
    char L = letter_of(h);
    CHECK(L, "not mounted after format");
    if (L) {
        volume_t *v = vfs_volume_by_letter(L);
        char want[8]; snprintf(want, sizeof want, "FAT%d", want_type);
        CHECK(v && !strcmp(v->fs_name, want), "fs %s, want %s", v ? v->fs_name : "?", want);
        CHECK(v && !strcmp(v->label, "FMT T_ST"), "label '%s'", v ? v->label : "?");
        u64 total = 0, freeb = 0;
        OK(vfs_statfs(L, &total, &freeb));
        CHECK(total > im.len * 9 / 10 && freeb > total * 9 / 10, "statfs %lu/%lu of %lu", (unsigned long)freeb, (unsigned long)total, (unsigned long)im.len);
        u8 *d = pat_alloc(100000, 77);
        v_put(P(L, "after format.bin"), d, 100000);
        OK(vfs_mkdir(P(L, "Ordner")));
        v_put(P(L, "Ordner/x.txt"), d, 10);
        /* formatting a mounted volume: same letter, contents gone */
        EXPECT(vfs_format(bd, fs, "AGAIN", cluster), 0);
        CHECK(letter_of(h) == L, "letter changed from %c to %c", L, letter_of(h));
        EXPECT(v_count(P(L, "")), 0);
        v_put(P(L, "after format.bin"), d, 100000);
        vfs_sync_all();
        free(d);
    }
    disk_detach(h);
    fsck(&im, tag);
    CHECK(fsck_type(&im) == want_type, "boot sector says FAT%d, want FAT%d", fsck_type(&im), want_type);
    if (ss == 512) {
        u8 *d = pat_alloc(100000, 77);
        mt_cmp(&im, "after format.bin", d, 100000);
        free(d);
    }
    if (part) {
        u8 mbr[512];
        FILE *f = fopen(im.path, "rb");
        size_t n = f ? fread(mbr, 1, 512, f) : 0;
        if (f) fclose(f);
        u8 want = want_type == 32 ? 0x0C : want_type == 16 ? 0x0E : 0x01;
        CHECK(n == 512 && mbr[446 + 4] == want, "partition type after format: %02x, want %02x", n == 512 ? mbr[450] : 0, want);
    }
}

static void s_format(void) {
    t_format("superfloppy 1.44 MB auto", 2, 0, 512, "FAT", 12, 0);
    t_format("partition 20 MB auto", 20, 1, 512, "FAT", 16, 0);
    t_format("partition 20 MB FAT12", 20, 1, 512, "FAT12", 12, 0);
    t_format("partition 300 MB auto", 300, 1, 512, "FAT", 16, 0);
    t_format("partition 300 MB FAT32", 300, 1, 512, "FAT32", 32, 0);
    t_format("partition 600 MB auto", 600, 1, 512, "FAT", 32, 0);
    t_format("partition 600 MB FAT32 4K", 600, 1, 512, "FAT32", 32, 4096);
    t_format("partition 2 GB FAT16 32K", 2048, 1, 512, "FAT16", 16, 32768);
    t_format("4K sectors 300 MB FAT32", 300, 1, 4096, "FAT32", 32, 0);
    t_format("4K sectors 64 MB FAT16", 64, 0, 4096, "FAT16", 16, 0);
    /* refusals */
    img_t im;
    img_blank(&im, "fmt_refuse.img", 20, 1, 512);
    hdisk_t *h = disk_attach(&im, 512);
    EXPECT(vfs_format(&h->bd, "FAT", "X", 0), E_BUSY);           /* partitioned disk */
    EXPECT(vfs_format(volume_bd(h), "NTFS", "X", 0), E_INVAL);
    EXPECT(vfs_format(volume_bd(h), "FAT32", "X", 0), E_INVAL);  /* 19 MB: too small for FAT32 */
    EXPECT(vfs_format(volume_bd(h), "FAT16", "X", 0), 0);
    char L = letter_of(h);
    vfs_file_t *f = NULL;
    OK(vfs_open(P(L, "open.txt"), VFS_O_WRITE | VFS_O_CREATE, &f));
    EXPECT(vfs_format(volume_bd(h), "FAT16", "X", 0), E_BUSY);   /* open file */
    if (f) vfs_close(f);
    h->bd.readonly = 1;
    EXPECT(vfs_format(volume_bd(h), "FAT16", "X", 0), E_ROFS);
    h->bd.readonly = 0;
    disk_detach(h);
}

static void s_fat16(void) {
    img_t im;
    img_mbr(&im, "fat16_read.img", 16, 32, "DATA16", 0xCAFE0016);
    t_read_mtools("fat16 read", &im, 512, "DATA16");
    img_mbr(&im, "fat16_write.img", 16, 32, "DATA16", 0xCAFE0016);
    t_write("fat16 write", &im, 512);
    t_rename("fat16 rename", &im, 512);
    t_delete("fat16 delete", &im, 512);
    img_mbr(&im, "fat16_large.img", 16, 32, "DATA16", 0xCAFE0016);
    t_large("fat16 large", &im, 512, 6 * 1024 * 1024 + 4321);
    img_mbr(&im, "fat16_many.img", 16, 32, "DATA16", 0xCAFE0016);
    t_many("fat16 many", &im, 512, 400);
    img_mbr(&im, "fat16_root.img", 16, 32, "DATA16", 0xCAFE0016);
    t_rootfull("fat16 root lfn", &im, 512, 1);
    img_mbr(&im, "fat16_full.img", 16, 20, "DATA16", 0xCAFE0016);
    t_diskfull("fat16 full", &im, 512);
    img_mbr(&im, "fat16_unplug.img", 16, 20, "DATA16", 0xCAFE0016);
    t_unplug("fat16 unplug", &im, 512);
}

static void s_fat32(void) {
    img_t im;
    img_mbr(&im, "fat32_read.img", 32, 64, "DATA32", 0xCAFE0032);
    t_read_mtools("fat32 read", &im, 512, "DATA32");
    img_mbr(&im, "fat32_write.img", 32, 64, "DATA32", 0xCAFE0032);
    t_write("fat32 write", &im, 512);
    t_rename("fat32 rename", &im, 512);
    t_delete("fat32 delete", &im, 512);
    img_mbr(&im, "fat32_large.img", 32, 64, "DATA32", 0xCAFE0032);
    t_large("fat32 large", &im, 512, 8 * 1024 * 1024 + 99);
    img_mbr(&im, "fat32_many.img", 32, 64, "DATA32", 0xCAFE0032);
    t_many("fat32 many", &im, 512, 600);
    img_mbr(&im, "fat32_full.img", 32, 40, "DATA32", 0xCAFE0032);
    t_diskfull("fat32 full", &im, 512);
    img_mbr(&im, "fat32_unplug.img", 32, 40, "DATA32", 0xCAFE0032);
    t_unplug("fat32 unplug", &im, 512);
}

static void s_gpt(void) {
    img_t im;
    img_gpt(&im, "gpt_read.img", 64, "GPTDISK");
    t_read_mtools("gpt read", &im, 512, "GPTDISK");
    img_gpt(&im, "gpt_write.img", 64, "GPTDISK");
    t_write("gpt write", &im, 512);
    t_large("gpt large", &im, 512, 5 * 1024 * 1024 + 1);
}

static void s_4k(void) {
    img_t im;
    img_super(&im, "fat16_4k.img", 16, 65536, "SECT4K", "-S 4096");
    t_read_mtools("4k fat16 read", &im, 4096, "SECT4K");
    img_super(&im, "fat16_4k_w.img", 16, 65536, "SECT4K", "-S 4096");
    t_write("4k fat16 write", &im, 4096);
    t_rename("4k fat16 rename", &im, 4096);
    t_large("4k fat16 large", &im, 4096, 3 * 1024 * 1024 + 5);
    t_shift(&im);
    img_super(&im, "fat32_4k.img", 32, 300 * 1024, "SECT4K32", "-S 4096 -s 1");
    t_write("4k fat32 write", &im, 4096);
    t_many("4k fat32 many", &im, 4096, 300);
    img_super(&im, "fat12_4k.img", 12, 8192, "SECT4K12", "-S 4096");
    t_write("4k fat12 write", &im, 4096);
    t_diskfull("4k fat12 full", &im, 4096);
}

static void s_misc(void) {
    t_letters();
    t_extended();
    t_detect();
}

static const scen_t scenarios[] = {
    { "paths", s_paths }, { "fat12", s_fat12 }, { "fat16", s_fat16 }, { "fat32", s_fat32 },
    { "gpt", s_gpt }, { "4k", s_4k }, { "misc", s_misc }, { "random", s_random }, { "testdisks", s_testdisks },
    { "hostile", s_hostile }, { "unicode", s_unicode }, { "format", s_format }, { "iso", s_iso },
};

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s WORKDIR [-v] [scenario...]\n", argv[0]);
        return 2;
    }
    W = argv[1];
    setenv("MTOOLS_SKIP_CHECK", "1", 1);
    setenv("LC_ALL", "C.UTF-8", 1);      /* mtools converts long names with the locale */
    setenv("MTOOLS_NO_VFAT", "", 1);
    unsetenv("MTOOLS_NO_VFAT");
    int first = 2;
    if (argc > 2 && !strcmp(argv[2], "-v")) { shim_verbose = 1; first = 3; }
    blockdev_init();
    vfs_init();
    size_t base_allocs = shim_live_allocs;
    for (size_t i = 0; i < ARRAY_SIZE(scenarios); i++) {
        int run = argc <= first;
        for (int a = first; a < argc; a++) if (!strcmp(argv[a], scenarios[i].name)) run = 1;
        if (!run) continue;
        int f0 = g_fail, c0 = g_checks;
        printf("== %s\n", scenarios[i].name);
        fflush(stdout);
        scenarios[i].fn();
        CHECK(vfs_volume_count() == 0 && blockdev_count() == 0, "devices left registered");
        CHECK(shim_live_allocs == base_allocs, "kernel heap leak: %zu live allocations",
              shim_live_allocs - base_allocs);
        base_allocs = shim_live_allocs;
        printf("   %s: %d checks, %d failed\n", scenarios[i].name, g_checks - c0, g_fail - f0);
        fflush(stdout);
    }
    printf("TOTAL: %d checks, %d failed -> %s\n", g_checks, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
