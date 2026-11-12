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

/* ------------------------------------------------------------------ */
/* verification of a written image                                      */

static hdisk_t *ntfs_attach(const img_t *im, u32 ss, int removable, int readonly) {
    hdisk_t *h = calloc(1, sizeof *h);
    h->fd = open(im->path, readonly ? O_RDONLY : O_RDWR);
    struct stat st;
    fstat(h->fd, &st);
    snprintf(h->bd.name, sizeof h->bd.name, "nt%d", g_disk_no++);
    snprintf(h->bd.model, sizeof h->bd.model, "image");
    h->bd.type = removable ? BDEV_DISK_USB : BDEV_DISK_SATA;
    h->bd.sector_size = ss;
    h->bd.sector_count = (u64)st.st_size / ss;
    h->bd.removable = (u8)removable;
    h->bd.readonly = (u8)readonly;
    h->bd.read = hd_read;
    h->bd.write = hd_write;
    int r = blockdev_register(&h->bd);
    CHECK(r == 0, "blockdev_register = %d", r);
    return h;
}

/* run a command, capture its output; returns the exit status */
static int sh_out(char *out, size_t outsz, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int sh_out(char *out, size_t outsz, const char *fmt, ...) {
    char cmd[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    strlcat(cmd, " 2>&1", sizeof cmd);
    FILE *p = popen(cmd, "r");
    size_t n = 0;
    out[0] = 0;
    if (!p) return -1;
    while (n + 1 < outsz) {
        size_t k = fread(out + n, 1, outsz - 1 - n, p);
        if (!k) break;
        n += k;
    }
    out[n] = 0;
    char sink[4096];
    while (fread(sink, 1, sizeof sink, p) > 0) {}
    int r = pclose(p);
    return WIFEXITED(r) ? WEXITSTATUS(r) : -1;
}

static int g_ntfs_verifies;

/* the independent checks of an image that is not mounted */
static void ntfs_verify(const char *tag, const img_t *im) {
    static char out[65536];
    g_ntfs_verifies++;
    int r = sh_out(out, sizeof out, "ntfsfix -n %s", q(im->path));
    CHECK(r == 0 && strstr(out, "processed successfully"), "[%s] ntfsfix -n: %d\n%s", tag, r, out);
    r = sh_out(out, sizeof out, "ntfsresize -i -f -n %s", q(im->path));
    /* (a full volume is an error for resizing only) */
    int full = strstr(out, "ERROR: Volume is full") != NULL;
    char *fe = strstr(out, "ERROR: Volume is full");
    if (fe) memset(fe, '-', 6);
    CHECK((r == 0 || full) && !strstr(out, "ERROR") && !strstr(out, "rror") && strstr(out, "Accounting clusters"),
          "[%s] ntfsresize -i: %d\n%s", tag, r, out);
    r = sh_out(out, sizeof out, "python3 ntfs_check.py %s", q(im->path));
    CHECK(r == 0 && strstr(out, "volume clean"), "[%s] ntfs_check.py: %d\n%s", tag, r, out);
    if (shim_verbose) printf("    [%s] %s", tag, out);
}

/* ------------------------------------------------------------------ */
/* the model of a random test: files and directories                    */

typedef struct { char path[300]; int dir; u8 *data; u32 size; int used; } nm_t;
#define NM_MAX 120
static nm_t g_nm[NM_MAX];

static nm_t *nm_find(const char *path) {
    for (int i = 0; i < NM_MAX; i++) if (g_nm[i].used && !utf8_casecmp(g_nm[i].path, path)) return &g_nm[i];
    return NULL;
}

static nm_t *nm_new(void) {
    for (int i = 0; i < NM_MAX; i++) if (!g_nm[i].used) { memset(&g_nm[i], 0, sizeof g_nm[i]); return &g_nm[i]; }
    return NULL;
}

static int nm_children(const char *dir) {
    size_t l = strlen(dir);
    int n = 0;
    for (int i = 0; i < NM_MAX; i++)
        if (g_nm[i].used && !strncasecmp(g_nm[i].path, dir, l) && g_nm[i].path[l] == '/') n++;
    return n;
}

static const char *nm_random_dir(void) {
    int cand[NM_MAX], n = 0;
    for (int i = 0; i < NM_MAX; i++) if (g_nm[i].used && g_nm[i].dir) cand[n++] = i;
    if (!n || rnd(4) == 0) return "";
    return g_nm[cand[rnd(n)]].path;
}

static void nm_random_name(char *out, size_t sz) {
    static const char *pool[] = { "f%u.txt", "Long File Name %u.dat", "MIXED case %u", "X%u", "With Space %u.c",
                                  "UPPER%u.TXT", "dots.in.name.%u.tar.gz", "%u", "Grüße %u.txt", "日本語 %u",
                                  "Ελληνικά%u", "README%u", "a_very_long_name_that_goes_on_and_on_and_on_and_on_%u_end_of_it.text",
                                  "ПРИВЕТ%u.TXT", "emoji \xF0\x9F\x98\x80 %u" };
    snprintf(out, sz, pool[rnd(ARRAY_SIZE(pool))], rnd(30));
}

static void nm_join(char *out, size_t sz, const char *dir, const char *name) {
    snprintf(out, sz, "%s%s%s", dir, *dir ? "/" : "", name);
}

/* compare the model with the image mounted by ntfs-3g (everything under
 * base must be in the model) */
static void nm_cmp_ntfs3g(const char *tag, const img_t *im, const char *base) {
    const char *mnt = ntfs3g_mount(im, 0);
    if (!mnt) return;
    int n = 0;
    for (int i = 0; i < NM_MAX; i++) {
        nm_t *m = &g_nm[i];
        if (!m->used) continue;
        char hp[2048];
        snprintf(hp, sizeof hp, "%s/%s%s%s", mnt, base, *base ? "/" : "", m->path);
        struct stat st;
        int r = stat(hp, &st);
        CHECK(r == 0, "[%s] ntfs-3g: %s missing", tag, m->path);
        if (r) continue;
        n++;
        if (m->dir) { CHECK(S_ISDIR(st.st_mode), "[%s] ntfs-3g: %s not a directory", tag, m->path); continue; }
        size_t hsz = 0;
        u8 *hb = file_read_all(hp, &hsz);
        CHECK(hb && hsz == m->size && !memcmp(hb, m->data, hsz), "[%s] ntfs-3g: %s content (%zu bytes, model %u)", tag,
              m->path, hsz, m->size);
        free(hb);
    }
    char dir[2048];
    snprintf(dir, sizeof dir, "%s/%s", mnt, base);
    static char out[256];
    sh_out(out, sizeof out, "find %s -mindepth 1 | wc -l", q(dir));
    CHECK(atoi(out) == n, "[%s] ntfs-3g lists %d entries, the model has %d", tag, atoi(out), n);
    ntfs3g_umount(mnt);
}

static void nm_verify_all(char L, const char *base, const char *tag) {
    for (int i = 0; i < NM_MAX; i++) {
        nm_t *m = &g_nm[i];
        if (!m->used) continue;
        char p[VFS_MAX_PATH + 300];
        snprintf(p, sizeof p, "%c:/%s%s%s", L, base, *base ? "/" : "", m->path);
        if (m->dir) {
            vfs_dirent_t st;
            int r = vfs_stat(p, &st);
            CHECK(r == 0 && st.is_dir, "[%s] %s: %s", tag, p, ename(r));
        } else {
            v_cmp(p, m->data, m->size);
        }
    }
}

static void nm_clear(void) {
    for (int i = 0; i < NM_MAX; i++) { free(g_nm[i].data); g_nm[i].data = NULL; g_nm[i].used = 0; }
}

/* random operations below base (a new directory, "" = the root) */
static void t_ntfs_random(const char *tag, img_t *im, u32 ss, u32 seed, int ops, u32 maxsz, const char *base) {
    g_rng = seed;
    nm_clear();
    hdisk_t *h = ntfs_attach(im, ss, 1, 0);
    char L = letter_of(h);
    if (!L) { CHECK(0, "[%s] not mounted", tag); disk_detach(h); return; }
    char bp[300];
    snprintf(bp, sizeof bp, "%c:/%s", L, base);
    if (*base) OK(vfs_mkdir(bp));
    u8 *buf = malloc(maxsz + 1);
    int nfull = 0, counts[8] = { 0 };
    char path[300], name[200], full[VFS_MAX_PATH + 300], full2[VFS_MAX_PATH + 300];
#define FULLP(dst, rel) snprintf(dst, sizeof dst, "%c:/%s%s%s", L, base, *base ? "/" : "", rel)
    int paranoid = getenv("NTFS_PARANOID") != NULL;
    for (int op = 0; op < ops; op++) {
        if (paranoid) {
            /* every operation leaves the image consistent (only the dirty flag is set) */
            static char out[16384];
            int r = sh_out(out, sizeof out, "python3 ntfs_check.py %s", q(im->path));
            if (r) { printf("    [%s] inconsistent before op %d:\n%s", tag, op, out); CHECK(0, "paranoid"); break; }
        }
        int kind = rnd(100);
        int idx = rnd(NM_MAX);
        nm_t *m = &g_nm[idx];
        if (paranoid && shim_verbose) printf("    op %d: kind %d on %s\n", op, kind, m->used ? m->path : "-");
        if (kind < 18 || !m->used) {
            /* create a file or a directory */
            nm_random_name(name, sizeof name);
            nm_join(path, sizeof path, nm_random_dir(), name);
            if (strlen(path) > 250 || nm_find(path)) continue;
            nm_t *n = nm_new();
            if (!n) continue;
            FULLP(full, path);
            if (rnd(5) == 0) {
                int r = vfs_mkdir(full);
                if (r == E_NOSPC) { nfull++; if (shim_verbose) printf("    NOSPC at line %d: %s\n", __LINE__, m->used ? m->path : path); continue; }
                CHECK(r == 0, "[%s] mkdir %s: %s", tag, path, ename(r));
                if (r) continue;
                strlcpy(n->path, path, sizeof n->path);
                n->dir = 1;
                n->used = 1;
                counts[0]++;
                continue;
            }
            u32 sz = rnd(3) == 0 ? rnd(900) : rnd(maxsz);
            pattern(buf, sz, seed + (u32)op);
            int r = vfs_write_file(full, buf, sz);
            if (r < 0 && r != E_NOSPC) { CHECK(0, "[%s] create %s: %s", tag, path, ename(r)); continue; }
            if (r == E_NOSPC && !v_exists(full)) { nfull++; if (shim_verbose) printf("    NOSPC at line %d: %s\n", __LINE__, m->used ? m->path : path); continue; }
            strlcpy(n->path, path, sizeof n->path);
            n->data = malloc(sz ? sz : 1);
            memcpy(n->data, buf, sz);
            n->size = sz;
            n->used = 1;
            counts[1]++;
            if (r < 0) {
                nfull++;
                char *d = NULL;
                u32 got = 0;
                int k = vfs_read_file(full, &d, &got);
                CHECK(k >= 0 && got <= sz && !memcmp(d, buf, got), "[%s] partial create of %s", tag, path);
                n->size = got;
                kfree(d);
            }
        } else if (kind < 40 && !m->dir) {
            /* write somewhere (maybe past the end) */
            u32 off = rnd(m->size + 20000), len = 1 + rnd(maxsz / 2);
            pattern(buf, len, seed ^ (u32)op);
            vfs_file_t *f;
            FULLP(full, m->path);
            int r = vfs_open(full, VFS_O_RDWR, &f);
            CHECK(r == 0, "[%s] open %s: %s", tag, m->path, ename(r));
            if (r) continue;
            vfs_seek(f, off);
            int w = vfs_write(f, buf, len);
            vfs_close(f);
            if (w == E_NOSPC) { nfull++; if (shim_verbose) printf("    NOSPC at line %d: %s\n", __LINE__, m->used ? m->path : path); continue; }
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
            counts[2]++;
        } else if (kind < 50 && !m->dir) {
            u32 ns = rnd(3) ? rnd(m->size + 1) : rnd(m->size * 2 + 30000);
            vfs_file_t *f;
            FULLP(full, m->path);
            if (vfs_open(full, VFS_O_RDWR, &f)) { CHECK(0, "[%s] open %s", tag, m->path); continue; }
            int r = vfs_truncate(f, ns);
            vfs_close(f);
            if (r == E_NOSPC) { nfull++; if (shim_verbose) printf("    NOSPC at line %d: %s\n", __LINE__, m->used ? m->path : path); continue; }
            CHECK(r == 0, "[%s] truncate %s to %u: %s", tag, m->path, ns, ename(r));
            if (r) continue;
            m->data = realloc(m->data, ns ? ns : 1);
            if (ns > m->size) memset(m->data + m->size, 0, ns - m->size);
            m->size = ns;
            counts[3]++;
        } else if (kind < 62) {
            /* delete a file / remove a directory */
            FULLP(full, m->path);
            int r = vfs_unlink(full);
            if (m->dir && nm_children(m->path)) { EXPECT(r, E_NOTEMPTY); continue; }
            CHECK(r == 0, "[%s] unlink %s: %s", tag, m->path, ename(r));
            if (r) continue;
            free(m->data);
            m->data = NULL;
            m->used = 0;
            counts[4]++;
        } else if (kind < 80) {
            /* rename / move a file or a directory */
            const char *nd = nm_random_dir();
            if (rnd(3) == 0) {
                /* a new name only in case */
                strlcpy(path, m->path, sizeof path);
                for (char *c = strrchr(path, '/') ? strrchr(path, '/') + 1 : path; *c; c++)
                    if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
                    else if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);
            } else {
                nm_random_name(name, sizeof name);
                nm_join(path, sizeof path, nd, name);
            }
            nm_t *other = nm_find(path);
            size_t l = strlen(m->path), longest = l;
            for (int i = 0; i < NM_MAX; i++)
                if (g_nm[i].used && !strncasecmp(g_nm[i].path, m->path, l) && g_nm[i].path[l] == '/')
                    longest = MAX(longest, strlen(g_nm[i].path));
            if (strlen(path) + longest - l > 250) continue;     /* the model's paths are limited */
            FULLP(full, m->path);
            FULLP(full2, path);
            int into_self = m->dir && !strncasecmp(path, m->path, l) && path[l] == '/';
            int r = vfs_rename(full, full2);
            if (into_self) { EXPECT(r, E_INVAL); continue; }
            if (other && other != m) { EXPECT(r, E_EXIST); continue; }
            if (r == E_NOSPC) { nfull++; if (shim_verbose) printf("    NOSPC at line %d: %s\n", __LINE__, m->used ? m->path : path); continue; }
            CHECK(r == 0, "[%s] rename %s -> %s: %s", tag, m->path, path, ename(r));
            if (r) continue;
            if (m->dir) {
                for (int i = 0; i < NM_MAX; i++) {
                    nm_t *c = &g_nm[i];
                    if (c->used && c != m && !strncasecmp(c->path, m->path, l) && c->path[l] == '/') {
                        char np[600];
                        snprintf(np, sizeof np, "%s%s", path, c->path + l);
                        strlcpy(c->path, np, sizeof c->path);
                    }
                }
            }
            strlcpy(m->path, path, sizeof m->path);
            counts[5]++;
        } else if (kind < 97) {
            if (!m->dir) { FULLP(full, m->path); v_cmp(full, m->data, m->size); }
        } else {
            /* unplug, check the image, plug in again */
            disk_detach(h);
            ntfs_verify(tag, im);
            h = ntfs_attach(im, ss, 1, 0);
            L = letter_of(h);
            if (!L) { CHECK(0, "[%s] remount failed", tag); break; }
            CHECK(!vfs_volume_by_letter(L)->readonly, "[%s] read-only after a session: %s", tag,
                  vfs_volume_by_letter(L)->ro_reason);
            nm_verify_all(L, base, tag);
            counts[6]++;
        }
    }
#undef FULLP
    if (L) nm_verify_all(L, base, tag);
    if (shim_verbose)
        printf("    [%s] %d ops: %d mkdir, %d create, %d write, %d truncate, %d delete, %d rename, %d remount, %d disk full\n",
               tag, ops, counts[0], counts[1], counts[2], counts[3], counts[4], counts[5], counts[6], nfull);
    disk_detach(h);
    ntfs_verify(tag, im);
    nm_cmp_ntfs3g(tag, im, base);
    nm_clear();
    free(buf);
}

/* a debugging aid: NTFS_DBG="ops" runs c<name> (create) / d<name> (delete)
 * / m<name> (mkdir) on an empty image and leaves it in the work directory */
static void s_ntfsdbg(void) {
    const char *ops = getenv("NTFS_DBG");
    if (!ops || !ntfs_tools()) return;
    img_t im;
    ntfs_img(&im, "dbg.img", 64, "empty", getenv("NTFS_DBG_OPTS"));
    hdisk_t *h = ntfs_attach(&im, 512, 1, 0);
    char L = letter_of(h);
    char buf[300];
    while (L && *ops) {
        char kind = *ops++;
        const char *e = strchr(ops, ',');
        size_t n = e ? (size_t)(e - ops) : strlen(ops);
        char name[200];
        snprintf(name, sizeof name, "%.*s", (int)n, ops);
        ops += n + (e ? 1 : 0);
        snprintf(buf, sizeof buf, "%c:/%s", L, name);
        int r = kind == 'c' ? vfs_write_file(buf, (const u8 *)"data", 4) : kind == 'd' ? vfs_unlink(buf) : vfs_mkdir(buf);
        printf("    %c %s: %s\n", kind, name, ename(r < 0 ? r : 0));
    }
    disk_detach(h);
    ntfs_verify("dbg", &im);
}

/* ------------------------------------------------------------------ */
/* writes into an image populated by ntfs-3g: every operation is also   */
/* applied to a host copy of the original tree (the oracle)             */

typedef struct { char L; const char *ref; const char *tag; } wx_t;

static const char *wx_host(wx_t *x, const char *rel) {
    static char b[4][2048];
    static int i;
    char *o = b[i++ & 3];
    snprintf(o, 2048, "%s/%s", x->ref, rel);
    return o;
}

static void wx_write(wx_t *x, const char *rel, u64 off, u32 len, u32 seed, int want) {
    u8 *buf = pat_alloc(len, seed);
    vfs_file_t *f;
    int r = vfs_open(P(x->L, rel), VFS_O_RDWR | VFS_O_CREATE, &f);
    if (!r) {
        vfs_seek(f, off);
        r = vfs_write(f, buf, len);
        int c = vfs_close(f);
        if (r >= 0 && c) r = c;
    }
    if (want == 1 && r == E_NOTSUP) { free(buf); return; }     /* may be refused */
    if (want > 1 || want < 0) { EXPECT(r, want); free(buf); return; }
    CHECK(r == (int)len, "[%s] write %s at %llu: %s", x->tag, rel, (unsigned long long)off, ename(r));
    int fd = open(wx_host(x, rel), O_RDWR | O_CREAT, 0644);
    pwrite(fd, buf, len, (off_t)off);
    close(fd);
    free(buf);
}

static void wx_trunc(wx_t *x, const char *rel, u64 size) {
    vfs_file_t *f;
    int r = vfs_open(P(x->L, rel), VFS_O_RDWR, &f);
    if (!r) { r = vfs_truncate(f, size); vfs_close(f); }
    CHECK(r == 0, "[%s] truncate %s: %s", x->tag, rel, ename(r));
    truncate(wx_host(x, rel), (off_t)size);
}

static void wx_del(wx_t *x, const char *rel) {
    int r = vfs_unlink(P(x->L, rel));
    CHECK(r == 0, "[%s] delete %s: %s", x->tag, rel, ename(r));
    if (unlink(wx_host(x, rel))) rmdir(wx_host(x, rel));
}

static void wx_ren(wx_t *x, const char *a, const char *b) {
    int r = vfs_rename(P(x->L, a), P(x->L, b));
    CHECK(r == 0, "[%s] rename %s -> %s: %s", x->tag, a, b, ename(r));
    char ha[2048];
    strlcpy(ha, wx_host(x, a), sizeof ha);
    rename(ha, wx_host(x, b));
}

static void wx_mkdir(wx_t *x, const char *rel) {
    OK(vfs_mkdir(P(x->L, rel)));
    mkdir(wx_host(x, rel), 0755);
}

static void t_ntfs_write_existing(const char *tag, u32 cluster) {
    img_t im;
    char opts[64];
    snprintf(opts, sizeof opts, "-c %u", cluster);
    ntfs_img(&im, "ntfsx.img", 128, "basic", opts);
    /* the oracle: a host copy of the original tree */
    char ref[1100];
    snprintf(ref, sizeof ref, "%s/ref", W);
    sh("rm -rf %s", q(ref));
    const char *mnt = ntfs3g_mount(&im, 0);
    if (!mnt) return;
    sh("cp -a %s %s", q(mnt), q(ref));
    ntfs3g_umount(mnt);
    hdisk_t *h = ntfs_attach(&im, 512, 1, 0);
    wx_t x = { letter_of(h), ref, tag };
    CHECK(x.L, "[%s] not mounted", tag);
    if (!x.L) { disk_detach(h); return; }
    /* data of existing files: resident -> non-resident, appends, overwrites */
    wx_write(&x, "small.txt", 100, 9000, 1, 0);
    wx_write(&x, "medium.bin", 50000, 123456, 2, 0);
    wx_write(&x, "large.bin", 1000, 5000, 3, 0);
    wx_trunc(&x, "large.bin", 2 * 1024 * 1024 + 17);
    wx_trunc(&x, "page.bin", 10);
    wx_trunc(&x, "empty.txt", 100000);
    wx_write(&x, "one.bin", 300000, 10, 4, 0);
    /* hard links and the file with 160 of them (an attribute list) */
    wx_del(&x, "Links/second.txt");
    wx_ren(&x, "Links/original.txt", "Links/other/renamed original.txt");
    wx_write(&x, "Links/other/third.txt", 10, 100, 5, 0);
    wx_ren(&x, "ManyLinks/link_with_a_long_name_000.bin", "ManyLinks/renamed link.bin");
    wx_ren(&x, "ManyLinks/link_with_a_long_name_159.bin", "Links/moved link.bin");
    for (int i = 1; i < 60; i += 2) {
        char n[100];
        snprintf(n, sizeof n, "ManyLinks/link_with_a_long_name_%03d.bin", i);
        wx_del(&x, n);
    }
    wx_write(&x, "ManyLinks/target.bin", 19000, 50000, 6, 0);
    /* an ntfs-3g B+ tree: deletions and insertions */
    for (int i = 0; i < 3000; i += 3) {
        char n[100];
        snprintf(n, sizeof n, "BigDir/entry number %05d with padding.txt", i);
        wx_del(&x, n);
    }
    for (int i = 0; i < 600; i++) {
        char n[100];
        snprintf(n, sizeof n, "BigDir/new %04d.dat", i * 7);
        wx_write(&x, n, 0, (u32)(i % 5) * 300 + 1, 100 + (u32)i, 0);
    }
    /* files Halcyon may only read, but delete */
    wx_write(&x, "Fragmented/frag_a.bin", 1000, 30000, 7, 0);      /* in place: fine */
    {
        vfs_dirent_t st;
        if (!vfs_stat(P(x.L, "Fragmented/frag_a.bin"), &st))
            wx_write(&x, "Fragmented/frag_a.bin", st.size, 3000000, 8, 1);   /* the run list is in extents: may be refused */
    }
    wx_del(&x, "Fragmented/frag_b.bin");
    wx_write(&x, "Compressed/text.txt", 0, 10, 9, 1);       /* (64 KiB clusters: not compressed) */
    wx_del(&x, "Compressed/text.txt");
    wx_del(&x, "Sparse/holes.bin");
    wx_del(&x, "with_streams.txt");
    wx_del(&x, "win_symlink");
    wx_del(&x, "win_junction");
    wx_del(&x, "link_to_small");
    /* directories */
    wx_del(&x, "EmptyDir");
    EXPECT(vfs_unlink(P(x.L, "Unicode")), E_NOTEMPTY);
    wx_ren(&x, "level00", "Deep tree");
    wx_ren(&x, "Deep tree/level01/level02", "Unicode/level02 moved");
    wx_mkdir(&x, "Unicode/Neuer Ordner");
    wx_write(&x, "Unicode/Neuer Ordner/Größe.txt", 0, 777, 10, 0);
    wx_ren(&x, "Unicode/Grüße.txt", "Unicode/GRÜßE.TXT");
    wx_ren(&x, "DosNames/A long file name.txt", "DosNames/short.txt");
    wx_del(&x, "DosNames/Program Files");
    wx_write(&x, "NEWFILE.TXT", 0, 1234, 11, 0);
    disk_detach(h);
    ntfs_verify(tag, &im);
    mnt = ntfs3g_mount(&im, 0);
    if (mnt) {
        static char out[8192];
        int r = sh_out(out, sizeof out, "diff -r --no-dereference %s %s", q(ref), q(mnt));
        CHECK(r == 0, "[%s] the image differs from the oracle:\n%s", tag, out);
        ntfs3g_umount(mnt);
    }
    sh("rm -rf %s", q(ref));
    unlink(im.path);
}

/* index splits and merges, MFT growth: thousands of files in one
 * directory, deleted in random order */
static void t_ntfs_many(const char *tag, u32 cluster, int n) {
    img_t im;
    char opts[64];
    snprintf(opts, sizeof opts, "-c %u", cluster);
    ntfs_img(&im, "ntfsmany.img", 256, "empty", opts);
    hdisk_t *h = ntfs_attach(&im, 512, 1, 0);
    char L = letter_of(h);
    if (!L) { CHECK(0, "[%s] not mounted", tag); disk_detach(h); return; }
    OK(vfs_mkdir(P(L, "Many")));
    char p[300];
    int *order = malloc(sizeof(int) * (size_t)n);
    for (int i = 0; i < n; i++) order[i] = i;
    g_rng = 7;
    for (int i = n - 1; i > 0; i--) { int j = (int)rnd((u32)i + 1); int t = order[i]; order[i] = order[j]; order[j] = t; }
    for (int i = 0; i < n; i++) {
        snprintf(p, sizeof p, "Many/%0*d %s.txt", 6, order[i], (order[i] % 5) == 0 ? "with a much longer name to vary the entry sizes" : "f");
        int r = vfs_write_file(P(L, p), (const u8 *)p, (u32)strlen(p));
        if (r < 0) { CHECK(0, "[%s] create %s: %s", tag, p, ename(r)); break; }
    }
    CHECK(v_count(P(L, "Many")) == n, "[%s] %d listed, want %d", tag, v_count(P(L, "Many")), n);
    disk_detach(h);
    ntfs_verify(tag, &im);
    h = ntfs_attach(&im, 512, 1, 0);
    L = letter_of(h);
    for (int i = n - 1; i > 0; i--) { int j = (int)rnd((u32)i + 1); int t = order[i]; order[i] = order[j]; order[j] = t; }
    int left = n;
    for (int i = 0; i < n && L; i++) {
        snprintf(p, sizeof p, "Many/%0*d %s.txt", 6, order[i], (order[i] % 5) == 0 ? "with a much longer name to vary the entry sizes" : "f");
        int r = vfs_unlink(P(L, p));
        if (r) { CHECK(0, "[%s] delete %s: %s", tag, p, ename(r)); break; }
        left--;
        if (i == n / 2) {
            CHECK(v_count(P(L, "Many")) == left, "[%s] %d listed, want %d", tag, v_count(P(L, "Many")), left);
            disk_detach(h);
            ntfs_verify(tag, &im);
            h = ntfs_attach(&im, 512, 1, 0);
            L = letter_of(h);
        }
    }
    if (L) {
        CHECK(v_count(P(L, "Many")) == 0, "[%s] not empty", tag);
        OK(vfs_unlink(P(L, "Many")));
    }
    disk_detach(h);
    ntfs_verify(tag, &im);
    free(order);
    unlink(im.path);
}

/* a volume made by Halcyon's own formatter (ntfs_mkfs.c, through
 * vfs_format) mounts read-write here, takes writes, and stays clean */
int ntfs_format(blockdev_t *bd, const char *label, u32 cluster_bytes);

static void t_ntfs_formatted(const char *tag, u32 mb, u32 ss, u32 cluster) {
    img_t im;
    memset(&im, 0, sizeof im);
    snprintf(im.path, sizeof im.path, "%s", wpath("ntfsfmt.img"));
    unlink(im.path);
    sh("truncate -s %uM %s", mb, q(im.path));
    hdisk_t *h = ntfs_attach(&im, ss, 1, 0);
    EXPECT(vfs_format(&h->bd, "NTFS", "Formatted", cluster), 0);
    char L = letter_of(h);
    CHECK(L, "[%s] the formatted volume did not mount", tag);
    if (L) {
        volume_t *v = vfs_volume_by_letter(L);
        CHECK(!strcmp(v->fs_name, "NTFS") && !strcmp(v->label, "Formatted"), "[%s] %s '%s'", tag, v->fs_name, v->label);
        CHECK(!v->readonly, "[%s] read-only: %s", tag, v->ro_reason);
        u8 *d = pat_alloc(200000, 5);
        OK(vfs_mkdir(P(L, "Folder")));
        v_put(P(L, "Folder/data.bin"), d, 200000);
        v_put(P(L, "small.txt"), d, 100);
        v_cmp(P(L, "Folder/data.bin"), d, 200000);
        free(d);
    }
    disk_detach(h);
    ntfs_verify(tag, &im);
    t_ntfs_random(tag, &im, ss, 77, 400, 200000, "Random");
    unlink(im.path);
}

static void s_ntfsw(void) {
    if (!ntfs_tools()) return;
    if (!getenv("NTFS_ONLY") || !strcmp(getenv("NTFS_ONLY"), "formatted")) {
        t_ntfs_formatted("ntfs formatted 64M", 64, 512, 0);
        t_ntfs_formatted("ntfs formatted 512B clusters", 32, 512, 512);
        t_ntfs_formatted("ntfs formatted 64K clusters", 128, 512, 65536);
        t_ntfs_formatted("ntfs formatted 4Kn", 64, 4096, 0);
    }
    if (!getenv("NTFS_ONLY") || !strcmp(getenv("NTFS_ONLY"), "existing")) {
        t_ntfs_write_existing("ntfs write existing 4k", 4096);
        t_ntfs_write_existing("ntfs write existing 64k", 65536);
    }
    if (!getenv("NTFS_ONLY") || !strcmp(getenv("NTFS_ONLY"), "many")) {
        t_ntfs_many("ntfs many 4k", 4096, 5000);
        t_ntfs_many("ntfs many 64k", 65536, 3000);
        t_ntfs_many("ntfs many 512", 512, 3000);
    }
    static const struct { u32 cluster, ss, mb; const char *name; u32 maxsz; } cfg[] = {
        { 4096, 512, 64, "4k", 300000 }, { 512, 512, 48, "512", 100000 }, { 65536, 512, 128, "64k", 400000 },
        { 4096, 4096, 64, "4kn", 300000 }, { 4096, 512, 12, "full", 2000000 },
    };
    for (size_t i = 0; i < ARRAY_SIZE(cfg); i++) {
        if (getenv("NTFS_ONLY") && strcmp(getenv("NTFS_ONLY"), cfg[i].name)) continue;
        img_t im;
        char name[64], opts[64], tag[64];
        snprintf(name, sizeof name, "ntfsw_%s.img", cfg[i].name);
        snprintf(opts, sizeof opts, "-c %u -s %u", cfg[i].cluster, cfg[i].ss);
        snprintf(tag, sizeof tag, "ntfs random %s", cfg[i].name);
        ntfs_img(&im, name, cfg[i].mb, "empty", opts);
        t_ntfs_random(tag, &im, cfg[i].ss, (getenv("NTFS_SEED") ? (u32)atoi(getenv("NTFS_SEED")) : 100) + (u32)i, getenv("NTFS_OPS") ? atoi(getenv("NTFS_OPS")) : 1500,
                      cfg[i].maxsz, "");
        unlink(im.path);
    }
}
