/* Host tests for the NTFS formatter (kernel/fs/ntfs_mkfs.c, ntfs_upcase.c).
 *
 * The formatter is compiled unmodified with the storage stack (shim.c) and
 * run on image files of many sizes, sector sizes and cluster sizes. Every
 * volume is then checked by tools that share no code with it:
 *   - ntfsfix -n (ntfs-3g): must find nothing to fix;
 *   - ntfsinfo -m (ntfs-3g): must parse the volume; geometry and label
 *     must be what was asked for;
 *   - ntfs_tool.py check: an independent reader (bitmaps, indexes, $Secure,
 *     $LogFile, mirrors, ...), see that file;
 *   - for a subset, the ntfs-3g FUSE driver (ntfs_fuse.py): thousands of
 *     files and directories written, deleted, renamed; ntfsfix -n again;
 *     everything verified after a second mount.
 * The system files are also compared with an mkntfs-made volume of the same
 * geometry (ntfs_tool.py dump): the attribute layout of records 0-26 must
 * be the same apart from the documented differences (KNOWN_DIFFS below;
 * docs/DESIGN-M7.5-FORMAT.md explains them). And the $UpCase table must
 * equal mkntfs's (both follow Windows' table).
 *
 * usage: ntfsfmt WORKDIR [-v] [quick]   ("quick": fewer images, no FUSE)
 * Needs ntfs-3g (ntfsfix, ntfsinfo, mkntfs, ntfs-3g, fusermount; FUSE as
 * root) and python3; prints a skip message without them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "core/types.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "fs/block.h"
#include "fs/vfs.h"
#include "fs/ntfs_layout.h"
#include "fs/ntfs_mkfs.h"

extern int shim_verbose;
extern size_t shim_live_allocs;

static const char *W;
static int g_fail, g_checks, g_fuse_ok, g_quick;
static int n_images, n_fuse, n_cmp;

#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; \
    printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int sh(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int sh(const char *fmt, ...) {
    char cmd[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    if (shim_verbose) printf("    $ %s\n", cmd);
    int r = system(cmd);
    return WIFEXITED(r) ? WEXITSTATUS(r) : -1;
}

/* run a command, its output into WORKDIR/out.txt; returns the exit status */
static int shout(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int shout(const char *fmt, ...) {
    char cmd[4096], full[4300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    snprintf(full, sizeof full, "(%s) > %s/out.txt 2>&1", cmd, W);
    if (shim_verbose) printf("    $ %s\n", cmd);
    int r = system(full);
    return WIFEXITED(r) ? WEXITSTATUS(r) : -1;
}

static char *slurp(const char *name) {
    static char buf[1 << 16];
    char path[600];
    snprintf(path, sizeof path, "%s/%s", W, name);
    FILE *f = fopen(path, "r");
    buf[0] = 0;
    if (!f) return buf;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

static void dump_out(void) {
    char *s = slurp("out.txt");
    printf("%s", s);
    if (*s && s[strlen(s) - 1] != '\n') printf("\n");
}

/* ------------------------------------------------------------------ */
/* file-backed block device                                             */

typedef struct { blockdev_t bd; int fd; } hdisk_t;

static int hd_read(blockdev_t *bd, u64 lba, u32 count, void *buf) {
    hdisk_t *h = (hdisk_t *)bd;
    size_t n = (size_t)count * bd->sector_size;
    return pread(h->fd, buf, n, (off_t)(lba * bd->sector_size)) == (ssize_t)n ? 0 : E_IO;
}

static int hd_write(blockdev_t *bd, u64 lba, u32 count, const void *buf) {
    hdisk_t *h = (hdisk_t *)bd;
    size_t n = (size_t)count * bd->sector_size;
    return pwrite(h->fd, buf, n, (off_t)(lba * bd->sector_size)) == (ssize_t)n ? 0 : E_IO;
}

static int g_disk_no;

static hdisk_t *disk_attach(const char *path, u32 ss) {
    hdisk_t *h = calloc(1, sizeof *h);
    h->fd = open(path, O_RDWR);
    struct stat st;
    fstat(h->fd, &st);
    snprintf(h->bd.name, sizeof h->bd.name, "hd%d", g_disk_no++);
    snprintf(h->bd.model, sizeof h->bd.model, "image");
    h->bd.type = BDEV_DISK_SATA;
    h->bd.sector_size = ss;
    h->bd.sector_count = (u64)st.st_size / ss;
    h->bd.read = hd_read;
    h->bd.write = hd_write;
    CHECK(blockdev_register(&h->bd) == 0, "blockdev_register");
    return h;
}

static void disk_detach(hdisk_t *h) {
    blockdev_unregister(&h->bd);
    close(h->fd);
    free(h);
}

static const char *wpath(const char *name) {
    static char bufs[16][600];
    static int i;
    char *b = bufs[i++ & 15];
    snprintf(b, 600, "%s/%s", W, name);
    return b;
}

static void make_image(const char *path, u64 bytes) {
    unlink(path);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0 && ftruncate(fd, (off_t)bytes) == 0, "creating %s", path);
    close(fd);
}

/* ------------------------------------------------------------------ */
/* checks of a formatted image                                          */

/* the value after "key:" in ntfsinfo -m output */
static long long info_num(const char *text, const char *key) {
    const char *p = strstr(text, key);
    if (!p) return -1;
    p += strlen(key);
    while (*p == ':' || *p == ' ' || *p == '\t') p++;
    return strtoll(p, NULL, 10);
}

static int check_image(const char *img, const char *what, u32 cs, u32 ss, const char *label) {
    int bad = 0;
    int r = shout("ntfsfix -n %s", img);
    char *out = slurp("out.txt");
    if (r || !strstr(out, "processed successfully") || strstr(out, "FAILED") || strstr(out, "Corrupt")) {
        printf("  FAIL %s: ntfsfix -n (%d):\n", what, r);
        dump_out();
        bad++;
    }
    r = shout("ntfsinfo -m %s", img);
    out = slurp("out.txt");
    if (r) {
        printf("  FAIL %s: ntfsinfo -m (%d):\n", what, r);
        dump_out();
        bad++;
    } else {
        long long c = info_num(out, "Cluster Size"), s = info_num(out, "Sector Size");
        if (c != cs || s != ss || !strstr(out, "Volume Version: 3.1") || !strstr(out, "Volume Flags: 0x0000")) {
            printf("  FAIL %s: ntfsinfo says cluster %lld sector %lld:\n%s", what, c, s, out);
            bad++;
        }
        char want[128];
        snprintf(want, sizeof want, "Volume Name: %s\n", label);
        if (!strstr(out, want)) {
            printf("  FAIL %s: ntfsinfo: no '%s'\n", what, label);
            bad++;
        }
    }
    r = shout("python3 ntfs_tool.py %s check", img);
    if (r) {
        printf("  FAIL %s: ntfs_tool.py check:\n", what);
        dump_out();
        bad++;
    }
    /* every system file is listed and readable */
    r = shout("ntfsls -a -s %s && ntfscat %s '$UpCase' | cmp -s - %s", img, img, wpath("upcase_ref.bin"));
    out = slurp("out.txt");
    if (r || !strstr(out, "$Secure") || !strstr(out, "$Extend") || !strstr(out, "$UpCase")) {
        printf("  FAIL %s: ntfsls / ntfscat $UpCase (%d):\n", what, r);
        dump_out();
        bad++;
    }
    g_checks++;
    if (bad) g_fail++;
    n_images++;
    return bad;
}

static void fuse_test(const char *img, const char *what, int nfiles, int seed) {
    if (!g_fuse_ok) return;
    int r = shout("python3 ntfs_fuse.py %s %s %d %d", img, wpath("mnt"), nfiles, seed);
    char *out = slurp("out.txt");
    CHECK(r == 0, "%s: ntfs_fuse.py:\n%s", what, out);
    if (!r && shim_verbose) printf("    %s", out);
    if (!r) {
        char *p = strstr(out, "ntfs-3g: ");
        if (p && strchr(p, '\n')) *strchr(p, '\n') = 0;
        printf("  %s: %s\n", what, p ? p : out);
        n_fuse++;
        /* the volume after ntfs-3g wrote to it: the independent checker too */
        r = shout("python3 ntfs_tool.py %s check", img);
        CHECK(r == 0, "%s: ntfs_tool.py check after ntfs-3g:\n%s", what, slurp("out.txt"));
    }
}

/* ------------------------------------------------------------------ */
/* scenarios                                                            */

/* format a whole image (no partition table) and check it */
static int one(u64 bytes, u32 ss, u32 cs_req, const char *label, int fuse_files) {
    char what[96], img[600];
    snprintf(what, sizeof what, "%llu bytes, %u-byte sectors, cluster %u", (unsigned long long)bytes, ss, cs_req);
    snprintf(img, sizeof img, "%s", wpath("vol.img"));
    make_image(img, bytes);
    hdisk_t *h = disk_attach(img, ss);
    int r = ntfs_format(&h->bd, label, cs_req);
    disk_detach(h);
    CHECK(r == 0, "%s: ntfs_format = %d", what, r);
    if (r) return r;
    u32 cs = cs_req ? cs_req : ntfs_default_cluster(bytes, ss);
    int bad = check_image(img, what, cs, ss, label);
    if (shim_verbose || bad) {
        shout("python3 ntfs_tool.py %s info", img);
        printf("  %s:\n", what);
        dump_out();
    }
    if (!bad && fuse_files) fuse_test(img, what, fuse_files, (int)(bytes / 4096 + cs));
    return bad;
}

/* the system files' layout compared with mkntfs's; returns the number of
 * unexpected differences */
static const char *const KNOWN_DIFFS[] = {
    /* mkntfs puts a $SECURITY_DESCRIPTOR on some files (and a 48-byte
     * $STANDARD_INFORMATION without a security id); Halcyon uses security
     * ids for every file, like Windows */
    "0x50 $SECURITY_DESCRIPTOR",
    /* mkntfs adds Windows 8's $UpCase:$Info stream */
    "'$Info'",
};

static int m_rs_4k(u32 ss) { return ss >= 4096; }

static void compare_mkntfs(u64 bytes, u32 ss, u32 cs) {
    char cmd[1024], ref[600], img[600];
    strlcpy(ref, wpath("ref.img"), sizeof ref);
    strlcpy(img, wpath("vol.img"), sizeof img);
    make_image(ref, bytes);
    snprintf(cmd, sizeof cmd, "mkntfs -F -Q -q -s %u -c %u -L CMP %s", ss, cs, ref);
    if (sh("%s >/dev/null 2>&1", cmd)) { printf("  (mkntfs failed: %s)\n", cmd); return; }
    make_image(img, bytes);
    hdisk_t *h = disk_attach(img, ss);
    int r = ntfs_format(&h->bd, "CMP", cs);
    disk_detach(h);
    CHECK(r == 0, "ntfs_format = %d", r);
    /* normalise: no sizes that depend on placement policy (MFT size, log
     * size, sds size), only the attribute structure */
    const char *norm = "sed -E 's/ (size|alloc|init|len)=[0-9]+//g; s/ secid=0x[0-9a-f]+//; s/ links=[0-9]+//; "
                       "s/ len=[0-9]+ rflags/ rflags/; s/ runs=[0-9]+//'";
    sh("python3 ntfs_tool.py %s dump | %s > %s", ref, norm, wpath("dump_mkntfs.txt"));
    sh("python3 ntfs_tool.py %s dump | %s > %s", img, norm, wpath("dump_halcyon.txt"));
    char dname[64];
    snprintf(dname, sizeof dname, "dump_diff_%u_%u.txt", ss, cs);
    sh("diff %s %s > %s", wpath("dump_mkntfs.txt"), wpath("dump_halcyon.txt"), wpath(dname));
    /* every differing line must be a known difference */
    FILE *f = fopen(wpath(dname), "r");
    int res_data = 0;
    char line[512];
    int unexpected = 0, known = 0;
    while (f && fgets(line, sizeof line, f)) {
        if (line[0] != '<' && line[0] != '>') continue;
        int ok = 0;
        for (size_t i = 0; i < ARRAY_SIZE(KNOWN_DIFFS); i++) if (strstr(line, KNOWN_DIFFS[i])) ok = 1;
        /* mkntfs's 48-byte $STANDARD_INFORMATION (no security id) vs. 72 bytes */
        if (strstr(line, "$STANDARD_INFORMATION")) ok = 1;
        /* with 4 KiB records mkntfs keeps $AttrDef's data resident; Halcyon
         * keeps it non-resident on every volume */
        if (m_rs_4k(ss) && !strcmp(line, "<   0x80 $DATA                  res flags=0x0 rflags=0\n") && !res_data) ok = ++res_data;
        else if (m_rs_4k(ss) && !strcmp(line, ">   0x80 $DATA                  nonres flags=0x0\n") && res_data == 1) ok = ++res_data;
        if (ok) known++;
        else {
            if (unexpected < 12) printf("    unexpected difference: %s", line);
            unexpected++;
        }
    }
    if (f) fclose(f);
    CHECK(unexpected == 0, "%u/%u: %d unexpected differences from mkntfs's layout (%s)", ss, cs, unexpected,
          wpath(dname));
    if (shim_verbose) printf("  mkntfs comparison %u/%u: %d known differences\n", ss, cs, known);
    /* the $UpCase tables are the same */
    r = sh("ntfscat %s '$UpCase' | cmp -s - %s", ref, wpath("upcase_ref.bin"));
    CHECK(r == 0, "mkntfs's $UpCase differs from Halcyon's");
    /* and so are the $AttrDef tables */
    r = sh("ntfscat %s '$AttrDef' > %s && ntfscat %s '$AttrDef' | cmp -s - %s", ref, wpath("attrdef_ref.bin"), img,
           wpath("attrdef_ref.bin"));
    CHECK(r == 0, "mkntfs's $AttrDef differs from Halcyon's");
    n_cmp++;
}

/* a disk with an MBR partition table: vfs_format formats the partition,
 * sets its type to 0x07 and records its start in the boot sector */
static void partitioned(void) {
    const char *img = wpath("disk.img");
    make_image(img, 300ull << 20);
    CHECK(sh("printf 'start=2048, size=600000, type=c\\n' | sfdisk -q %s >/dev/null 2>&1", img) == 0, "sfdisk");
    hdisk_t *h = disk_attach(img, 512);
    blockdev_t *part = NULL;
    for (blockdev_t *b = blockdev_first(); b; b = b->next) if (b->parent == &h->bd) part = b;
    CHECK(part != NULL, "no partition");
    if (part) {
        int r = vfs_format(part, "NTFS", "Partition", 0);
        /* (no NTFS driver mounts it in this tree: E_NOTSUP is fine) */
        CHECK(r == 0 || r == E_NOTSUP, "vfs_format NTFS = %d", r);
    }
    disk_detach(h);
    int fd = open(img, O_RDONLY);
    u8 mbr[512], bs[512];
    pread(fd, mbr, 512, 0);
    pread(fd, bs, 512, 2048 * 512);
    close(fd);
    CHECK(mbr[0x1BE + 4] == 0x07, "partition type 0x%02x, want 0x07", mbr[0x1BE + 4]);
    ntfs_boot_t *b = (ntfs_boot_t *)bs;
    CHECK(b->hidden_sectors == 2048, "hidden sectors %u", b->hidden_sectors);
    CHECK(b->total_sectors == 600000 - 1, "total sectors %llu", (unsigned long long)b->total_sectors);
    sh("dd if=%s of=%s bs=512 skip=2048 count=600000 conv=sparse status=none", img, wpath("part.img"));
    check_image(wpath("part.img"), "MBR partition", 4096, 512, "Partition");
    /* the partition (as extracted) through ntfs-3g */
    fuse_test(wpath("part.img"), "MBR partition", g_quick ? 0 : 300, 5);
}

/* the checkers are not vacuous: damaged copies of a good volume must fail */
static void checker_selftest(void) {
    const char *img = wpath("vol.img");
    char bad[600];
    strlcpy(bad, wpath("bad.img"), sizeof bad);
    make_image(img, 64ull << 20);
    hdisk_t *h = disk_attach(img, 512);
    CHECK(ntfs_format(&h->bd, "Good", 0) == 0, "format");
    disk_detach(h);
    int fd = open(img, O_RDONLY);
    ntfs_boot_t b;
    pread(fd, &b, sizeof b, 0);
    close(fd);
    u64 cs = (u64)b.bytes_per_sector * b.sectors_per_cluster;
    static const struct { const char *what; int nr; } dmg[] = {
        { "backup boot sector", 0 }, { "$MFTMirr record 0", 1 }, { "torn MFT record 3", 2 }, { "$Volume dirty", 3 },
    };
    for (size_t i = 0; i < ARRAY_SIZE(dmg); i++) {
        sh("cp --sparse=always %s %s", img, bad);
        fd = open(bad, O_RDWR);
        u8 x = 0x5A;
        u16 dirty = 0;
        switch (dmg[i].nr) {
        case 0: pwrite(fd, &x, 1, (off_t)(b.total_sectors * b.bytes_per_sector + 0x48)); break;
        case 1: pwrite(fd, &x, 1, (off_t)(b.mftmirr_lcn * cs + 0x100)); break;
        case 2: pwrite(fd, &x, 1, (off_t)(b.mft_lcn * cs + 3 * 1024 + 510)); break;
        case 3: {
            /* the flags of $VOLUME_INFORMATION in record 3: find the attribute */
            u8 rec[1024];
            pread(fd, rec, sizeof rec, (off_t)(b.mft_lcn * cs + 3 * 1024));
            u32 o = rec[20] | rec[21] << 8;
            while (o + 8 < sizeof rec && *(u32 *)(rec + o) != NTFS_AT_VOLUME_INFORMATION) o += *(u32 *)(rec + o + 4);
            u16 vo = *(u16 *)(rec + o + 20);
            dirty = NTFS_VOL_DIRTY;
            pwrite(fd, &dirty, 2, (off_t)(b.mft_lcn * cs + 3 * 1024 + o + vo + 10));
            break;
        }
        }
        close(fd);
        CHECK(sh("python3 ntfs_tool.py %s check >/dev/null 2>&1", bad) != 0, "ntfs_tool.py accepts a damaged volume (%s)",
              dmg[i].what);
    }
    unlink(bad);
    unlink(img);
}

/* argument checks that fail before anything is written */
static int nd_rw(blockdev_t *bd, u64 lba, u32 count, void *buf) { return E_IO; }
static int nd_wr(blockdev_t *bd, u64 lba, u32 count, const void *buf) { return E_IO; }

static void refusals(void) {
    blockdev_t bd;
    memset(&bd, 0, sizeof bd);
    strlcpy(bd.name, "fake", sizeof bd.name);
    bd.present = 1;
    bd.read = nd_rw;
    bd.write = nd_wr;
    bd.sector_size = 512;
    bd.sector_count = (1u << 20) / 512;             /* 1 MiB: too small */
    CHECK(ntfs_format(&bd, "", 0) == E_NOSPC, "1 MiB volume");
    bd.sector_count = (20ull << 40) / 512;          /* 20 TiB with 4 KiB clusters: > 2^32 clusters */
    CHECK(ntfs_format(&bd, "", 4096) == E_INVAL, "20 TiB with 4 KiB clusters");
    bd.sector_count = (300ull << 40) / 512;         /* 300 TiB: too large even with 64 KiB */
    CHECK(ntfs_format(&bd, "", 0) == E_INVAL, "300 TiB");
    bd.sector_count = (8ull << 30) / 512;
    CHECK(ntfs_format(&bd, "", 3000) == E_INVAL, "cluster 3000");
    CHECK(ntfs_format(&bd, "", 256) == E_INVAL, "cluster 256");
    CHECK(ntfs_format(&bd, "", 131072) == E_INVAL, "cluster 128 KiB");
    CHECK(ntfs_format(&bd, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456", 0) == E_INVAL, "33-character label");
    CHECK(ntfs_format(&bd, "a/b", 0) == E_INVAL, "label with '/'");
    CHECK(ntfs_format(&bd, "\xC3\x28", 0) == E_INVAL, "label not UTF-8");
    bd.sector_size = 4096;
    bd.sector_count = (8ull << 30) / 4096;
    CHECK(ntfs_format(&bd, "", 2048) == E_INVAL, "cluster below the sector size");
    bd.sector_size = 520;
    CHECK(ntfs_format(&bd, "", 0) == E_NOTSUP, "520-byte sectors");
    bd.sector_size = 512;
    bd.readonly = 1;
    CHECK(ntfs_format(&bd, "", 0) == E_ROFS, "read-only device");
    bd.readonly = 0;
    CHECK(ntfs_format(&bd, "", 0) == E_IO, "write error");
    /* default cluster sizes (Windows' table) */
    CHECK(ntfs_default_cluster(2ull << 20, 512) == 4096, "2 MiB");
    CHECK(ntfs_default_cluster(500ull << 30, 512) == 4096, "500 GiB");
    CHECK(ntfs_default_cluster((16ull << 40) - 4096, 512) == 4096, "16 TiB - 4 KiB");
    CHECK(ntfs_default_cluster(16ull << 40, 512) == 8192, "16 TiB");
    CHECK(ntfs_default_cluster(40ull << 40, 512) == 16384, "40 TiB");
    CHECK(ntfs_default_cluster(100ull << 40, 512) == 32768, "100 TiB");
    CHECK(ntfs_default_cluster(200ull << 40, 512) == 65536, "200 TiB");
    CHECK(ntfs_default_cluster(257ull << 40, 512) == 0, "257 TiB");
    CHECK(ntfs_default_cluster(1ull << 30, 4096) == 4096, "4K sectors");
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: ntfsfmt WORKDIR [-v] [quick]\n"); return 2; }
    W = argv[1];
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) shim_verbose = 1;
        else if (!strcmp(argv[i], "quick")) g_quick = 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (sh("(command -v ntfsfix && command -v ntfsinfo && command -v mkntfs && command -v ntfsls && "
           "command -v ntfscat && command -v python3 && command -v sfdisk) >/dev/null 2>&1") != 0) {
        printf("ntfsfmt: SKIPPED (needs ntfs-3g: ntfsfix, ntfsinfo, mkntfs, ntfsls, ntfscat; sfdisk; python3)\n");
        return 0;
    }
    vfs_init();
    blockdev_init();
    /* the formatter's $UpCase table, for comparisons */
    {
        static u16 t[65536];
        ntfs_upcase_table(t);
        FILE *f = fopen(wpath("upcase_ref.bin"), "wb");
        fwrite(t, 2, 65536, f);
        fclose(f);
    }
    /* FUSE: needs ntfs-3g, fusermount and permission to mount */
    {
        make_image(wpath("probe.img"), 8ull << 20);
        sh("mkntfs -F -Q -q %s >/dev/null 2>&1", wpath("probe.img"));
        mkdir(wpath("mnt"), 0755);
        g_fuse_ok = sh("ntfs-3g %s %s >/dev/null 2>&1 && fusermount -u %s", wpath("probe.img"), wpath("mnt"),
                       wpath("mnt")) == 0;
        if (!g_fuse_ok) printf("ntfsfmt: ntfs-3g FUSE mounts are not possible here: skipping the FUSE tests\n");
        if (g_quick) g_fuse_ok = 0;
        unlink(wpath("probe.img"));
    }
    printf("ntfsfmt: refusals and default cluster sizes\n");
    refusals();
    checker_selftest();

    static const struct { u64 bytes; u32 ss, cs; const char *label; int fuse; } cases[] = {
        /* sizes (default cluster) */
        { 2ull << 20, 512, 0, "", 0 },
        { (3ull << 20) + 512, 512, 0, "Tiny", 200 },
        { 7ull << 20, 512, 0, "Seven", 0 },
        { 16ull << 20, 512, 0, "SIXTEEN", 0 },
        { (100ull << 20) + 3 * 512, 512, 0, "Odd size", 0 },
        { 257ull << 20, 512, 0, "Daten \xC3\xA4\xC3\xB6\xC3\xBC \xE6\x97\xA5\xE6\x9C\xAC", 5000 },
        { 1ull << 30, 512, 0, "GIG", 0 },
        { (4ull << 30) + 7 * 512, 512, 0, "Four", 0 },
        { 13ull << 30, 512, 0, "MFT at 3 GiB", 1000 },
        { 100ull << 30, 512, 0, "Hundred", 0 },
        { 2ull << 40, 512, 0, "Two TiB", 400 },
        /* cluster sizes */
        { 256ull << 20, 512, 512, "c512", 3000 },
        { 256ull << 20, 512, 1024, "c1k", 0 },
        { 256ull << 20, 512, 2048, "c2k", 0 },
        { 256ull << 20, 512, 8192, "c8k", 0 },
        { 256ull << 20, 512, 16384, "c16k", 0 },
        { 256ull << 20, 512, 32768, "c32k", 0 },
        { 256ull << 20, 512, 65536, "c64k", 2000 },
        { 3ull << 20, 512, 65536, "tiny64k", 0 },
        { 40ull << 30, 512, 65536, "big64k", 0 },
        /* 4096-byte sectors (4 KiB MFT records), and 2048 */
        { 16ull << 20, 4096, 4096, "4Kn", 0 },
        { 1ull << 30, 4096, 0, "4Kn GiB", 2000 },
        { 1ull << 30, 4096, 65536, "4Kn 64K", 0 },
        { 64ull << 20, 2048, 2048, "2K sectors", 0 },
        /* labels */
        { 64ull << 20, 512, 0, "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345", 0 },
        { 64ull << 20, 512, 0, "\xCE\x95\xCE\xBB\xCE\xBB\xCE\xB7\xCE\xBD\xCE\xB9\xCE\xBA\xCE\xAC "
                               "\xE2\x82\xAC\xF0\x9F\x98\x80", 0 },
    };
    printf("ntfsfmt: formatting images: ntfsfix -n, ntfsinfo -m, ntfs_tool.py check%s\n",
           g_fuse_ok ? ", ntfs-3g FUSE" : "");
    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        if (g_quick && (cases[i].bytes > (2ull << 30) || i % 3)) continue;
        one(cases[i].bytes, cases[i].ss, cases[i].cs, cases[i].label, cases[i].fuse);
        unlink(wpath("vol.img"));
    }
    printf("ntfsfmt: a partition (MBR) through vfs_format\n");
    partitioned();
    unlink(wpath("disk.img"));
    unlink(wpath("part.img"));
    printf("ntfsfmt: system files compared with mkntfs\n");
    static const struct { u64 bytes; u32 ss, cs; } cmp[] = {
        { 64ull << 20, 512, 4096 }, { 300ull << 20, 512, 512 }, { 300ull << 20, 512, 65536 },
        { 300ull << 20, 4096, 4096 }, { 2ull << 30, 512, 4096 },
    };
    for (size_t i = 0; i < ARRAY_SIZE(cmp); i++) {
        if (g_quick && i) break;
        compare_mkntfs(cmp[i].bytes, cmp[i].ss, cmp[i].cs);
    }
    unlink(wpath("ref.img"));
    unlink(wpath("vol.img"));
    CHECK(shim_live_allocs == 0, "%zu allocations leaked", shim_live_allocs);
    printf("ntfsfmt: %d images checked (ntfsfix, ntfsinfo, ntfs_tool.py), %d exercised through ntfs-3g, "
           "%d compared with mkntfs; %d checks, %d failed\n", n_images, n_fuse, n_cmp, g_checks, g_fail);
    return g_fail ? 1 : 0;
}
