/* Host test: the atomic save of SETTINGS.INI (kernel/core/settings.c) on
 * real file systems, through the kernel's own FAT, exFAT and NTFS drivers
 * (kernel/fs, compiled unmodified with shim.c), with a power cut after every
 * single sector the save writes.
 *
 * For each file system: an image (mkfs.fat / mkfs.exfat / mkntfs) with a
 * \HALCYON directory like the boot stick's and table A saved in it. Then, for
 * k = 0, 1, 2 ... until a save completes without reaching the cut: boot (load
 * A), change to table B, save; the disk keeps only the first k sectors the
 * save wrote (a write cut in the middle keeps its first sectors). Then boot
 * again from what is on the disk: the settings must be A or B, and once B
 * was reported saved, B. The next save must work, and fsck.fat / fsck.exfat
 * / ntfsfix -n must find the volume consistent (lost clusters and a dirty
 * flag are allowed; two names on the same clusters are not).
 *
 * usage: settingsfs WORKDIR [-v] */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include "core/types.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/settings.h"
#include "fs/block.h"
#include "fs/vfs.h"

extern int shim_verbose;
u64 time_ms(void) { static u64 t = 1000; return t += 10; }

static int g_checks, g_fail, verbose;
static const char *W;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int sh(const char *fmt, ...) {
    char cmd[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    char full[2200];
    snprintf(full, sizeof full, "%s >/dev/null 2>&1", cmd);
    return system(full);
}

/* ---------------- a disk with an overlay of written sectors ---------------- */
typedef struct { u64 lba; u8 *data; } osec_t;
typedef struct {
    blockdev_t bd;
    int fd;                  /* the base image (read only) */
    osec_t *ov;              /* sectors written since the base */
    int nov, cap;
    long written;            /* sectors written while cut_after >= 0 counts */
    long cut_after;          /* < 0: no cut; else keep only that many more */
} odisk_t;

static osec_t *ov_find(odisk_t *d, u64 lba) {
    for (int i = d->nov - 1; i >= 0; i--) if (d->ov[i].lba == lba) return &d->ov[i];
    return NULL;
}
static int od_read(blockdev_t *bd, u64 lba, u32 count, void *buf) {
    odisk_t *d = (odisk_t *)bd;
    u32 ss = bd->sector_size;
    for (u32 i = 0; i < count; i++) {
        osec_t *o = ov_find(d, lba + i);
        if (o) memcpy((u8 *)buf + (size_t)i * ss, o->data, ss);
        else if (pread(d->fd, (u8 *)buf + (size_t)i * ss, ss, (off_t)((lba + i) * ss)) != (ssize_t)ss) return E_IO;
    }
    return 0;
}
static int od_write(blockdev_t *bd, u64 lba, u32 count, const void *buf) {
    odisk_t *d = (odisk_t *)bd;
    u32 ss = bd->sector_size;
    for (u32 i = 0; i < count; i++) {
        if (d->cut_after >= 0 && d->written >= d->cut_after) return 0;   /* the power is off: lost */
        d->written++;
        osec_t *o = ov_find(d, lba + i);
        if (!o) {
            if (d->nov == d->cap) { d->cap = d->cap ? d->cap * 2 : 64; d->ov = realloc(d->ov, sizeof(osec_t) * (size_t)d->cap); }
            o = &d->ov[d->nov++];
            o->lba = lba + i;
            o->data = malloc(ss);
        }
        memcpy(o->data, (const u8 *)buf + (size_t)i * ss, ss);
    }
    return 0;
}

static int g_no;
static odisk_t *od_new(const char *img, u32 ss) {
    odisk_t *d = calloc(1, sizeof *d);
    d->fd = open(img, O_RDONLY);
    struct stat st;
    fstat(d->fd, &st);
    snprintf(d->bd.name, sizeof d->bd.name, "hd%d", g_no++);
    snprintf(d->bd.model, sizeof d->bd.model, "image");
    d->bd.type = BDEV_DISK_RAM;
    d->bd.sector_size = ss;
    d->bd.sector_count = (u64)st.st_size / ss;
    d->bd.removable = 1;
    d->bd.read = od_read;
    d->bd.write = od_write;
    d->cut_after = -1;
    return d;
}
static void od_copy_overlay(odisk_t *to, const odisk_t *from) {
    for (int i = 0; i < from->nov; i++) {
        long c = to->cut_after;
        to->cut_after = -1;
        od_write(&to->bd, from->ov[i].lba, 1, from->ov[i].data);
        to->cut_after = c;
    }
    to->written = 0;
}
static void od_free(odisk_t *d) {
    for (int i = 0; i < d->nov; i++) free(d->ov[i].data);
    free(d->ov);
    close(d->fd);
    free(d);
}
/* the image with the overlay applied, for the host's checker */
static void od_materialize(const odisk_t *d, const char *base, const char *out) {
    sh("cp --sparse=always '%s' '%s'", base, out);
    int fd = open(out, O_RDWR);
    for (int i = 0; i < d->nov; i++) pwrite(fd, d->ov[i].data, d->bd.sector_size, (off_t)(d->ov[i].lba * d->bd.sector_size));
    close(fd);
}

/* mount: the volume becomes the "boot volume" the settings live on */
static volume_t *od_mount(odisk_t *d) {
    int r = blockdev_register(&d->bd);
    CHECK(r == 0, "blockdev_register = %d", r);
    for (int i = 0; i < vfs_volume_count(); i++) {
        volume_t *v = vfs_volume(i);
        if (v->bd == &d->bd) { v->is_boot = 1; return v; }
    }
    return NULL;
}
static void od_unmount(odisk_t *d) { blockdev_unregister(&d->bd); }

/* ---------------- tables ---------------- */
#define NK 120
typedef struct { int n; char k[NK][48]; char v[NK][80]; } tbl_t;
static void tbl_make(tbl_t *t, int n, int tag) {
    t->n = n;
    for (int i = 0; i < n; i++) {
        snprintf(t->k[i], sizeof t->k[i], "%s.k%d", i % 9 == 0 ? "display" : i % 3 ? "window" : "paint", i);
        snprintf(t->v[i], sizeof t->v[i], "%d,%d,value of table %d with some length to fill sectors", i * 7, tag, tag);
    }
}
static void tbl_set(const tbl_t *t) {
    settings_clear();
    for (int i = 0; i < t->n; i++) settings_set(t->k[i], t->v[i]);
}
static int tbl_is(const tbl_t *t) {
    if (settings_count() != t->n) return 0;
    for (int i = 0; i < t->n; i++) {
        const char *v = settings_get(t->k[i], NULL);
        if (!v || strcmp(v, t->v[i])) return 0;
    }
    return 1;
}

/* ---------------- one file system ---------------- */
enum { K_FAT, K_EXFAT, K_NTFS };

static int fsck_ok(int kind, const char *img, char *why, int n) {
    char out[1024];
    snprintf(out, sizeof out, "%s/fsck.txt", W);
    int r;
    if (kind == K_FAT) r = system(({ static char c[1400]; snprintf(c, sizeof c, "fsck.fat -n '%s' > '%s' 2>&1", img, out); c; }));
    else if (kind == K_EXFAT) r = system(({ static char c[1400]; snprintf(c, sizeof c, "fsck.exfat -n '%s' > '%s' 2>&1", img, out); c; }));
    else r = system(({ static char c[1400]; snprintf(c, sizeof c, "ntfsfix -n '%s' > '%s' 2>&1", img, out); c; }));
    FILE *f = fopen(out, "r");
    char line[512];
    int bad = 0;
    why[0] = 0;
    while (f && fgets(line, sizeof line, f)) {
        /* cross-linked clusters (two files on the same data) or a broken directory */
        if (strstr(line, "cross-link") || strstr(line, "Cross-link") || strstr(line, "shared") ||
            strstr(line, "is used by") || strstr(line, "corrupt") || strstr(line, "Corrupt")) {
            bad = 1;
            if (!why[0]) { strlcpy(why, line, (size_t)n); char *nl = strchr(why, '\n'); if (nl) *nl = 0; }
        }
    }
    if (f) fclose(f);
    (void)r;
    return !bad;
}

static void run_fs(const char *name, int kind) {
    char base[1024], img[1024];
    snprintf(base, sizeof base, "%s/%s.img", W, name);
    snprintf(img, sizeof img, "%s/%s-check.img", W, name);
    unlink(base);
    int r;
    if (kind == K_FAT) r = sh("mkfs.fat -C -F 32 -s 1 -n SETTEST '%s' 34000", base);
    else if (kind == K_EXFAT) r = sh("truncate -s 32M '%s' && mkfs.exfat -c 4K '%s'", base, base);
    else r = sh("truncate -s 32M '%s' && mkntfs -F -Q -q -s 512 -c 4096 -L SETTEST '%s'", base, base);
    if (r) { printf("settingsfs: %s: SKIP (cannot make the image)\n", name); return; }
    /* the boot stick's \HALCYON: other files around the settings */
    odisk_t *d = od_new(base, 512);
    volume_t *v = od_mount(d);
    CHECK(v != NULL, "%s: mounted", name);
    if (!v) { od_unmount(d); od_free(d); return; }
    char p[64];
    snprintf(p, sizeof p, "%c:/HALCYON", v->letter);
    vfs_mkdir(p);
    static const char *const others[] = { "KERNEL.BIN", "BOOT.LOG", "BOOTPREV.LOG", "README.TXT" };
    static char filler[20000];
    memset(filler, 'k', sizeof filler);
    for (int i = 0; i < 4; i++) {
        snprintf(p, sizeof p, "%c:/HALCYON/%s", v->letter, others[i]);
        vfs_write_file(p, filler, (u32)(1000 + i * 4000));
    }
    snprintf(p, sizeof p, "%c:/HALCYON/MEDIA", v->letter);
    vfs_mkdir(p);
    static tbl_t A, B;
    tbl_make(&A, 90, 1);
    tbl_make(&B, 110, 2);
    tbl_set(&A);
    r = settings_save();
    CHECK(r == 0, "%s: first save: %d", name, r);
    tbl_set(&B);                         /* a second save: SETTINGS.BAK exists too */
    settings_save();
    tbl_set(&A);
    settings_save();
    od_unmount(d);
    /* d's overlay is the starting point of every cut */
    /* FAT and exFAT: every sector, strictly. NTFS (no journal in Halcyon;
     * never the boot volume, which the loaders need on FAT): every 23rd
     * sector, and only counted: after a cut the volume is not clean, so
     * Halcyon mounts it read-only (DESIGN-M7.5) */
    int strict = kind != K_NTFS, stride = strict ? 1 : 23;
    int k, done = 0, as_a = 0, as_b = 0, fsck_bad = 0, neither = 0, ro = 0;
    long total = -1;
    for (k = 0; !done && k < 20000; k += stride) {
        odisk_t *c = od_new(base, 512);
        od_copy_overlay(c, d);
        od_mount(c);
        settings_clear();
        settings_load();
        CHECK(tbl_is(&A), "%s: boot before the save (k=%d)", name, k);
        tbl_set(&B);
        c->cut_after = k;
        c->written = 0;
        r = settings_save();
        if (c->written < k) { done = 1; total = c->written; }   /* the save finished before the cut */
        od_unmount(c);                    /* (what it writes now is lost too) */
        /* the next boot */
        odisk_t *b = od_new(base, 512);
        od_copy_overlay(b, c);
        od_mount(b);
        settings_clear();
        settings_load();
        int ia = tbl_is(&A), ib = tbl_is(&B);
        as_a += ia; as_b += ib; neither += !ia && !ib;
        if (strict) CHECK(ia || ib, "%s: power cut after %d sectors: the next boot has neither table (%d settings)", name, k, settings_count());
        if (done && !r) CHECK(ib, "%s: a completed save, but the old table came back", name);
        tbl_set(&B);
        settings_set("paint.after", "1");
        r = settings_save();
        if (!strict && r == E_ROFS) { ro++; od_unmount(b); od_free(b); od_free(c); continue; }
        CHECK(r == 0, "%s: save after the cut at %d: %d", name, k, r);
        od_unmount(b);
        /* the result as the host's checker sees it */
        char why[300];
        od_materialize(b, base, img);
        if (!fsck_ok(kind, img, why, sizeof why)) {
            fsck_bad++;
            if (strict) CHECK(0, "%s: power cut after %d sectors: checker: %s", name, k, why);
            if (strict || verbose) {
                char cmd[1200];
                snprintf(cmd, sizeof cmd, "sed 's/^/    | /' '%s/fsck.txt' | head -12", W);
                fflush(stdout);
                if (system(cmd)) { }
            }
        }
        odisk_t *e = od_new(base, 512);
        od_copy_overlay(e, b);
        od_mount(e);
        settings_clear();
        settings_load();
        if (strict) CHECK(settings_get_int("paint.after", 0) == 1 && settings_count() == B.n + 1, "%s: table after the cut at %d", name, k);
        od_unmount(e);
        od_free(e); od_free(b); od_free(c);
    }
    if (strict)
        printf("settingsfs: %-6s a save writes %ld sectors; cut after each: %d boots with the old table, %d with the new, %d checker complaints\n",
               name, total, as_a, as_b, fsck_bad);
    else
        printf("settingsfs: %-6s a save writes %ld sectors; cut every %d sectors: %d old table, %d new, %d neither, "
               "%d read-only afterwards (not clean), %d checker complaints (not counted as failures)\n",
               name, total, stride, as_a, as_b, neither, ro, fsck_bad);
    od_free(d);
    unlink(img);
    unlink(base);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: settingsfs WORKDIR [-v]\n"); return 2; }
    W = argv[1];
    setvbuf(stdout, NULL, _IOLBF, 0);
    int only = -1;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) { verbose = 1; shim_verbose = 1; }
        else if (!strcmp(argv[i], "fat32")) only = K_FAT;
        else if (!strcmp(argv[i], "exfat")) only = K_EXFAT;
        else if (!strcmp(argv[i], "ntfs")) only = K_NTFS;
    }
    blockdev_init();
    vfs_init();
    if (only < 0 || only == K_FAT) run_fs("fat32", K_FAT);
    if (only < 0 || only == K_EXFAT) run_fs("exfat", K_EXFAT);
    if (only < 0 || only == K_NTFS) run_fs("ntfs", K_NTFS);
    settings_clear();
    printf("settingsfs: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail != 0;
}
