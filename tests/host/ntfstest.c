/* NTFS host tests (included by fstest.c): kernel/fs/ntfs*.c against images
 * made by mkntfs and populated through the ntfs-3g FUSE driver
 * (ntfs_mkimg.py). Every result is checked with independent tools:
 *   - reads: the image mounted read-only by ntfs-3g, compared file by file
 *     (names, types, sizes, contents, links);
 *   - writes: after a session the image is checked by ntfsfix -n, by
 *     ntfsresize -i -n (cluster accounting: $Bitmap against every run list),
 *     by ntfs_check.py (an independent Python checker of the on-disk
 *     structures: records, attributes, index B+ trees and their order,
 *     duplicated information, link counts, MFT bitmap) and by mounting it
 *     with ntfs-3g and comparing the whole tree with the test's model.
 *
 * Scenarios: ntfs (reads), ntfsw (writes), ntfssafe (the read-only rules),
 * ntfsbig (100k files in one directory; NTFS_BIG=N to change N).
 * Needs root (FUSE), mkntfs, ntfs-3g, ntfsfix, ntfsresize; skipped with a
 * message if they are missing. */
#include <dirent.h>
#include <sys/statvfs.h>
#include <errno.h>
#include <time.h>

static int g_ntfs_ok = -1;

static int ntfs_tools(void) {
    if (g_ntfs_ok >= 0) return g_ntfs_ok;
    g_ntfs_ok = geteuid() == 0 && access("/dev/fuse", R_OK | W_OK) == 0 &&
                !sh("which mkntfs ntfs-3g ntfsfix ntfsresize fusermount python3");
    if (!g_ntfs_ok) printf("   SKIP: NTFS tests need root, /dev/fuse, mkntfs, ntfs-3g, ntfsfix, ntfsresize\n");
    return g_ntfs_ok;
}

static void ntfs_img(img_t *im, const char *name, u32 mb, const char *profile, const char *opts) {
    memset(im, 0, sizeof *im);
    snprintf(im->path, sizeof im->path, "%s", wpath(name));
    im->len = (u64)mb << 20;
    int r = sh("python3 ntfs_mkimg.py %s %u %s %s", q(im->path), mb, profile, opts ? opts : "");
    CHECK(r == 0, "ntfs_mkimg.py %s %s failed", name, profile);
}

/* mount an image with ntfs-3g (read-only unless rw); the mount point */
static const char *ntfs3g_mount(const img_t *im, int rw) {
    static char mnt[1100];
    snprintf(mnt, sizeof mnt, "%s/mnt3g", W);
    sh("fusermount -u %s; mkdir -p %s", q(mnt), q(mnt));
    int r = sh("ntfs-3g -o %s %s %s", rw ? "rw" : "ro", q(im->path), q(mnt));
    CHECK(r == 0, "ntfs-3g cannot mount %s", im->path);
    return r ? NULL : mnt;
}

static void ntfs3g_umount(const char *mnt) { if (mnt) sh("fusermount -u %s", q(mnt)); }

static u8 *file_read_all(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 65536, len = 0;
    u8 *b = malloc(cap);
    for (;;) {
        if (len == cap) b = realloc(b, cap *= 2);
        size_t k = fread(b + len, 1, cap - len, f);
        if (!k) break;
        len += k;
    }
    fclose(f);
    *n = len;
    return b;
}

/* the whole content of a file through the VFS (NULL on error) */
static u8 *v_read_all(const char *path, u64 *n, int *err) {
    vfs_file_t *f;
    int r = vfs_open(path, VFS_O_READ, &f);
    if (r) { *err = r; return NULL; }
    u64 sz = vfs_size(f), got = 0;
    u8 *b = malloc(sz ? sz : 1);
    while (got < sz) {
        u32 want = (u32)MIN(sz - got, (u64)(1u << 20));
        int k = vfs_read(f, b + got, want);
        if (k <= 0) { *err = k < 0 ? k : E_IO; free(b); vfs_close(f); return NULL; }
        got += (u32)k;
    }
    u8 extra[4];
    if (vfs_read(f, extra, 4) != 0) { *err = E_INVAL; free(b); vfs_close(f); return NULL; }
    vfs_close(f);
    *n = sz;
    *err = 0;
    return b;
}

typedef struct { int files, dirs, links; u64 bytes; } tstat_t;

/* compare the tree under host directory hp with VFS directory vp */
static void cmp_tree(const char *hp, const char *vp, int depth, tstat_t *st) {
    vfs_dirent_t *ents = NULL;
    int n = vfs_list(vp, &ents);
    CHECK(n >= 0, "vfs_list(%s) = %s", vp, ename(n));
    if (n < 0) return;
    char *seen = calloc((size_t)n + 1, 1);
    DIR *d = opendir(hp);
    CHECK(d != NULL, "opendir %s", hp);
    struct dirent *de;
    int hn = 0;
    while (d && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        hn++;
        char hpath[2048], vpath[VFS_MAX_PATH + 300];
        snprintf(hpath, sizeof hpath, "%s/%s", hp, de->d_name);
        snprintf(vpath, sizeof vpath, "%s/%s", vp, de->d_name);
        int k;
        for (k = 0; k < n; k++) if (!seen[k] && !strcmp(ents[k].name, de->d_name)) break;
        CHECK(k < n, "%s: not listed by the driver", vpath);
        if (k >= n) continue;
        seen[k] = 1;
        vfs_dirent_t *e = &ents[k];
        struct stat hs;
        int lerr = lstat(hpath, &hs);
        if (lerr || de->d_type == DT_LNK || S_ISLNK(hs.st_mode)) {
            st->links++;
            if (!(e->attr & VFS_ATTR_LINK)) {
                /* an Interix symbolic link (ntfs-3g's default): a system file for Windows and Halcyon */
                CHECK(e->attr & VFS_ATTR_SYSTEM, "%s: Interix link attr %02x", vpath, e->attr);
                u64 n2;
                int err;
                u8 *b = v_read_all(vpath, &n2, &err);
                CHECK(b && n2 >= 8 && !memcmp(b, "IntxLNK\1", 8), "%s: not an Interix link (%s)", vpath, ename(err));
                free(b);
                continue;
            }
            /* a reparse point: listed, not followed */
            vfs_file_t *f;
            int r = vfs_open(vpath, VFS_O_READ, &f);
            if (!r) vfs_close(f);
            CHECK(r == E_NOTSUP || r == E_ISDIR, "%s: opening a link = %s", vpath, ename(r));
            if (e->is_dir) {
                vfs_dirent_t *x;
                r = vfs_list(vpath, &x);
                if (r >= 0) kfree(x);
                CHECK(r == E_NOTSUP, "%s: listing a directory link = %s", vpath, ename(r));
            }
            continue;
        }
        CHECK(!(e->attr & VFS_ATTR_LINK), "%s: marked as a link", vpath);
        if (S_ISDIR(hs.st_mode)) {
            st->dirs++;
            CHECK(e->is_dir && (e->attr & VFS_ATTR_DIR), "%s: not a directory", vpath);
            if (depth < 40) cmp_tree(hpath, vpath, depth + 1, st);
            continue;
        }
        st->files++;
        CHECK(!e->is_dir, "%s: listed as a directory", vpath);
        CHECK(e->size == (u64)hs.st_size, "%s: listed size %llu, ntfs-3g %lld", vpath,
              (unsigned long long)e->size, (long long)hs.st_size);
        vfs_dirent_t vs;
        int r = vfs_stat(vpath, &vs);
        CHECK(r == 0 && vs.size == (u64)hs.st_size, "%s: stat %s size %llu", vpath, ename(r), (unsigned long long)vs.size);
        if (!r) {
            struct tm tm;
            time_t t = hs.st_mtime;
            gmtime_r(&t, &tm);
            CHECK(vs.mtime.year == tm.tm_year + 1900 && vs.mtime.month == tm.tm_mon + 1 && vs.mtime.day == tm.tm_mday &&
                  vs.mtime.hour == tm.tm_hour && vs.mtime.minute == tm.tm_min && vs.mtime.second == tm.tm_sec,
                  "%s: mtime %04u-%02u-%02u %02u:%02u:%02u", vpath, vs.mtime.year, vs.mtime.month, vs.mtime.day,
                  vs.mtime.hour, vs.mtime.minute, vs.mtime.second);
        }
        size_t hsz;
        u8 *hb = file_read_all(hpath, &hsz);
        u64 vsz;
        int err;
        u8 *vb = v_read_all(vpath, &vsz, &err);
        CHECK(vb != NULL, "%s: read: %s", vpath, ename(err));
        if (vb && hb) {
            CHECK(vsz == hsz, "%s: read %llu bytes, ntfs-3g %zu", vpath, (unsigned long long)vsz, hsz);
            if (vsz == hsz) CHECK(!memcmp(vb, hb, hsz), "%s: content differs", vpath);
            st->bytes += vsz;
        }
        free(hb);
        free(vb);
    }
    if (d) closedir(d);
    for (int k = 0; k < n; k++) CHECK(seen[k], "%s/%s: listed by the driver, not by ntfs-3g", vp, ents[k].name);
    CHECK(hn == n, "%s: %d entries, ntfs-3g %d", vp, n, hn);
    free(seen);
    kfree(ents);
}

static void ntfs_cmp_all(const char *tag, const img_t *im, char L) {
    const char *mnt = ntfs3g_mount(im, 0);
    if (!mnt) return;
    tstat_t st = { 0 };
    cmp_tree(mnt, P(L, ""), 0, &st);
    if (shim_verbose) printf("    [%s] %d files (%llu bytes), %d directories, %d links compared\n", tag, st.files,
                             (unsigned long long)st.bytes, st.dirs, st.links);
    struct statvfs sv;
    if (!statvfs(mnt, &sv)) {
        u64 tot = 0, fr = 0;
        OK(vfs_statfs(L, &tot, &fr));
        CHECK(fr == (u64)sv.f_bfree * sv.f_bsize, "[%s] free %llu, ntfs-3g %llu", tag, (unsigned long long)fr,
              (unsigned long long)sv.f_bfree * sv.f_bsize);
    }
    ntfs3g_umount(mnt);
}

/* ------------------------------------------------------------------ */
/* reads                                                                */

static void t_ntfs_read_basic(const char *tag, img_t *im, u32 ss) {
    hdisk_t *h = disk_attach(im, ss);
    char L = letter_of(h);
    CHECK(L, "[%s] not mounted", tag);
    if (!L) { disk_detach(h); return; }
    volume_t *v = vfs_volume_by_letter(L);
    CHECK(!strcmp(v->fs_name, "NTFS"), "[%s] fs_name %s", tag, v->fs_name);
    CHECK(!strcmp(v->label, "Halcyon Test"), "[%s] label '%s'", tag, v->label);
    CHECK(!v->readonly, "[%s] read-only: %s", tag, v->ro_reason);
    {
        int fd = open(im->path, O_RDONLY);
        u64 serial = 0;
        pread(fd, &serial, 8, 0x48);
        close(fd);
        CHECK(v->serial == serial, "[%s] serial %llx", tag, (unsigned long long)v->serial);
    }
    ntfs_cmp_all(tag, im, L);
    /* lookups ignore case through $UpCase */
    static const char *const ci[][2] = {
        { "Unicode/ÄÖÜ.txt", "UNICODE/äöü.TXT" }, { "Unicode/Ελληνικά.txt", "unicode/ελληνικά.txt" },
        { "Unicode/Кириллица.txt", "Unicode/кИРИЛЛИЦА.TXT" }, { "Unicode/ωμέγα.txt", "Unicode/ΩΜΈΓΑ.txt" },
        { "level00/level01/file01.dat", "LEVEL00/Level01/FILE01.DAT" },
    };
    for (size_t i = 0; i < ARRAY_SIZE(ci); i++) {
        u64 a, b;
        int ea, eb;
        u8 *x = v_read_all(P(L, ci[i][0]), &a, &ea), *y = v_read_all(P(L, ci[i][1]), &b, &eb);
        CHECK(x && y && a == b && !memcmp(x, y, a), "[%s] %s vs %s: %s %s", tag, ci[i][0], ci[i][1], ename(ea), ename(eb));
        free(x);
        free(y);
    }
    /* names that differ only in case are two files; the exact one wins */
    {
        u64 a, b;
        int ea, eb;
        u8 *x = v_read_all(P(L, "Unicode/a.txt"), &a, &ea), *y = v_read_all(P(L, "Unicode/A.txt"), &b, &eb);
        CHECK(x && y && a == b && memcmp(x, y, a), "[%s] a.txt / A.txt not told apart", tag);
        free(x);
        free(y);
    }
    /* short names */
    vfs_dirent_t e;
    if (v_listed(P(L, "DosNames"), "A long file name.txt", &e))
        CHECK(!strcmp(e.short_name, "ALONGF~1.TXT"), "[%s] short name '%s'", tag, e.short_name);
    else CHECK(0, "[%s] long name not listed", tag);
    CHECK(!v_listed(P(L, "DosNames"), "ALONGF~1.TXT", NULL), "[%s] DOS name listed", tag);
    if (!vfs_stat(P(L, "DosNames/Program Files"), &e)) CHECK(!strcmp(e.short_name, "PROGRA~1"), "[%s] short '%s'", tag, e.short_name);
    else CHECK(0, "[%s] stat Program Files", tag);
    CHECK(v_exists(P(L, "DosNames/PROGRA~1")), "[%s] DOS name lookup", tag);
    if (v_listed(P(L, ""), "small.txt", &e)) CHECK(!strcmp(e.short_name, ""), "[%s] short of small.txt '%s'", tag, e.short_name);
    /* system files are hidden, System Volume Information is not */
    CHECK(!v_exists(P(L, "$MFT")) && !v_listed(P(L, ""), "$MFT", NULL), "[%s] $MFT visible", tag);
    CHECK(!v_listed(P(L, ""), "$Extend", NULL), "[%s] $Extend visible", tag);
    if (v_listed(P(L, ""), "System Volume Information", &e))
        CHECK((e.attr & (VFS_ATTR_HIDDEN | VFS_ATTR_SYSTEM | VFS_ATTR_DIR)) == (VFS_ATTR_HIDDEN | VFS_ATTR_SYSTEM | VFS_ATTR_DIR),
              "[%s] SVI attr %02x", tag, e.attr);
    else CHECK(0, "[%s] System Volume Information not listed", tag);
    if (v_listed(P(L, ""), "hidden.txt", &e)) CHECK(e.attr & VFS_ATTR_HIDDEN, "[%s] hidden attr %02x", tag, e.attr);
    if (v_listed(P(L, ""), "readonly.txt", &e)) CHECK(e.attr & VFS_ATTR_READONLY, "[%s] ro attr %02x", tag, e.attr);
    /* streams are not shown */
    CHECK(!v_exists(P(L, "with_streams.txt:secret")), "[%s] stream visible", tag);
    /* read-only files refuse writes */
    vfs_file_t *f;
    EXPECT(vfs_open(P(L, "readonly.txt"), VFS_O_RDWR, &f), E_ROFS);
    /* compressed and sparse files refuse writes */
    if (!vfs_open(P(L, "Sparse/holes.bin"), VFS_O_RDWR, &f)) {
        EXPECT(vfs_write(f, "x", 1), E_NOTSUP);
        vfs_close(f);
    }
    disk_detach(h);
}

static void s_ntfs(void) {
    if (!ntfs_tools()) return;
    static const struct { u32 cluster, ss; const char *name; } cfg[] = {
        { 4096, 512, "4k" }, { 512, 512, "512" }, { 1024, 512, "1k" }, { 16384, 512, "16k" },
        { 65536, 512, "64k" }, { 4096, 4096, "4kn" }, { 65536, 4096, "64k-4kn" },
    };
    for (size_t i = 0; i < ARRAY_SIZE(cfg); i++) {
        if (getenv("NTFS_ONLY") && strcmp(getenv("NTFS_ONLY"), cfg[i].name)) continue;
        img_t im;
        char name[64], opts[64], tag[64];
        snprintf(name, sizeof name, "ntfs_%s.img", cfg[i].name);
        snprintf(opts, sizeof opts, "-c %u -s %u", cfg[i].cluster, cfg[i].ss);
        snprintf(tag, sizeof tag, "ntfs read %s", cfg[i].name);
        ntfs_img(&im, name, 128, "basic", opts);
        t_ntfs_read_basic(tag, &im, cfg[i].ss);
        unlink(im.path);
    }
}
