/* Host tests for the installer's partition tables (kernel/fs/parttab.c).
 *
 * New MBRs and GPTs (512- and 4096-byte sectors, up to 3 TiB), partitions
 * added next to tables made by sfdisk and sgdisk (Windows-like GPT layouts,
 * MBRs with extended partitions), refusals (full tables, damaged headers,
 * stale tables, overlaps, the MBR's 2 TiB limit), backup repair and random
 * round trips. Every table is checked by tests/host/gpt_check.py (written
 * from the UEFI specification, CRC32s by zlib), by sfdisk --verify / --dump
 * and, if installed, by sgdisk -v; data next to added partitions must keep
 * its hash and the other partitions' entries their bytes.
 *
 * usage: parttest WORKDIR [-v]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include "core/types.h"
#include "core/lib.h"
#include "fs/parttab.h"

size_t strcspn(const char *s, const char *reject);

extern int shim_verbose;
static int g_checks, g_fail, g_verbose;
static const char *g_work;
static int have_sgdisk, have_losetup_4k;
static char g_out[65536];

#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

/* run a shell command, its output (stdout and stderr) in g_out; exit status */
static int sh(const char *fmt, ...) {
    char cmd[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    char full[2200];
    snprintf(full, sizeof full, "(%s) 2>&1", cmd);
    FILE *f = popen(full, "r");
    if (!f) return -1;
    size_t n = fread(g_out, 1, sizeof g_out - 1, f);
    g_out[n] = 0;
    int st = pclose(f);
    if (g_verbose) printf("    $ %s\n%s", cmd, g_out);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* ---- image files as disks ---- */
typedef struct { int fd; } fdisk_t;
typedef struct { pt_io_t io; fdisk_t d; char path[512]; } disk_t;    /* (io first: ctx points to it) */
static int f_read(void *ctx, u64 lba, u32 n, void *buf) {
    disk_t *k = ctx;
    size_t len = (size_t)n * k->io.sector_size;
    if (lba + n > k->io.sectors) return E_INVAL;
    return pread(k->d.fd, buf, len, (off_t)(lba * k->io.sector_size)) == (ssize_t)len ? 0 : E_IO;
}
static int f_write(void *ctx, u64 lba, u32 n, const void *buf) {
    disk_t *k = ctx;
    size_t len = (size_t)n * k->io.sector_size;
    if (lba + n > k->io.sectors) return E_INVAL;
    return pwrite(k->d.fd, buf, len, (off_t)(lba * k->io.sector_size)) == (ssize_t)len ? 0 : E_IO;
}

static void disk_open(disk_t *k, const char *name, u64 bytes, u32 ss, int create) {
    snprintf(k->path, sizeof k->path, "%s/%s", g_work, name);
    if (create) { unlink(k->path); }
    k->d.fd = open(k->path, O_RDWR | O_CREAT, 0644);
    if (create && ftruncate(k->d.fd, (off_t)bytes)) perror("ftruncate");
    if (!create) bytes = (u64)lseek(k->d.fd, 0, SEEK_END);
    k->io.ctx = &k->io;
    k->io.sector_size = ss;
    k->io.sectors = bytes / ss;
    k->io.read = f_read;
    k->io.write = f_write;
}
static void disk_close(disk_t *k) { close(k->d.fd); }

static u64 g_rng = 0x2545F4914F6CDD1Dull;
static u64 rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return g_rng; }
static void rnd_guid(u8 g[16]) {
    u8 r[16];
    for (int i = 0; i < 16; i++) r[i] = (u8)rnd();
    pt_guid_v4(g, r);
}

/* FNV-1a over a sector range of a file */
static u64 hash_range(disk_t *k, u64 lba, u64 count) {
    u64 h = 1469598103934665603ull;
    static u8 buf[1 << 20];
    u64 off = lba * k->io.sector_size, left = count * k->io.sector_size;
    while (left) {
        size_t n = left > sizeof buf ? sizeof buf : (size_t)left;
        if (pread(k->d.fd, buf, n, (off_t)off) != (ssize_t)n) return 0;
        for (size_t i = 0; i < n; i++) { h ^= buf[i]; h *= 1099511628211ull; }
        off += n; left -= n;
    }
    return h;
}
static void fill_range(disk_t *k, u64 lba, u64 count, u8 seed) {
    static u8 buf[1 << 16];
    for (size_t i = 0; i < sizeof buf; i++) buf[i] = (u8)(i * 31 + seed + (i >> 9));
    u64 off = lba * k->io.sector_size, left = count * k->io.sector_size;
    /* the first and last MiB and a few places between: enough to see a stray write */
    for (int part = 0; part < 3 && left; part++) {
        u64 at = part == 0 ? off : part == 1 ? off + left / 2 : off + left - MIN(left, sizeof buf);
        size_t n = (size_t)MIN(left, sizeof buf);
        if (pwrite(k->d.fd, buf, n, (off_t)at) != (ssize_t)n) perror("pwrite");
    }
}

/* the independent checker: 0 = clean; its partition lines in g_out */
static int gpt_check(disk_t *k) {
    return sh("python3 gpt_check.py '%s' %u", k->path, k->io.sector_size);
}
/* sfdisk and sgdisk take an image file as a disk of 512-byte sectors: a
 * 4K image goes through a loop device with 4 KiB logical blocks (root) */
static char g_loop[64];
static const char *dev_for(disk_t *k) {
    if (k->io.sector_size == 512) return k->path;
    if (!have_losetup_4k || sh("losetup -f --show -b %u '%s'", k->io.sector_size, k->path)) return NULL;
    snprintf(g_loop, sizeof g_loop, "%s", g_out);
    g_loop[strcspn(g_loop, "\n")] = 0;
    return g_loop;
}
static void dev_done(disk_t *k) {
    if (k->io.sector_size != 512 && g_loop[0]) {
        static char keep[sizeof g_out];
        char d[64];
        snprintf(d, sizeof d, "%s", g_loop);
        memcpy(keep, g_out, sizeof keep);       /* (the caller wants the tool's output) */
        sh("losetup -d %s", d);
        memcpy(g_out, keep, sizeof keep);
    }
    g_loop[0] = 0;
}
static int sgdisk_ok(disk_t *k) {
    const char *dev = have_sgdisk ? dev_for(k) : NULL;
    if (!dev) return 1;
    int st = sh("sgdisk -v '%s'", dev);
    int ok = st == 0 && strstr(g_out, "No problems found");
    dev_done(k);
    return ok;
}
static int sfdisk_cmd(disk_t *k, const char *opt) {
    const char *dev = dev_for(k);
    if (!dev) { strcpy(g_out, "(skipped)\nlabel: gpt\nlabel: dos\n"); return 0; }
    int st = sh("sfdisk %s '%s'", opt, dev);
    dev_done(k);
    return st;
}
static int sfdisk_verify(disk_t *k) { return sfdisk_cmd(k, "-V") == 0; }

/* ---------------- scenarios ---------------- */

static u8 bootcode[440];

static void t_mbr_new(void) {
    printf("== mbr_new\n");
    disk_t k;
    disk_open(&k, "mbr_new.img", 1ull << 30, 512, 1);
    for (int i = 0; i < 440; i++) bootcode[i] = (u8)(0x90 ^ i);
    u64 n = ALIGN_DOWN(k.io.sectors, 2048) - 2048;
    pt_new_t p = { .start = 2048, .count = n, .mbr_type = 0x0C, .active = 1 };
    CHECK(pt_write_mbr(&k.io, &p, 1, 0x1A2B3C4D, bootcode) == 0, "pt_write_mbr");
    CHECK(gpt_check(&k) == 0, "gpt_check: %s", g_out);
    char want[96];
    snprintf(want, sizeof want, "mbr 1 2048 %llu 0c 1", (unsigned long long)n);
    CHECK(strstr(g_out, want) != NULL, "gpt_check lists '%s': %s", want, g_out);
    CHECK(sfdisk_verify(&k), "sfdisk -V: %s", g_out);
    sh("sfdisk -d '%s'", k.path);
    snprintf(want, sizeof want, "start=        2048, size=%12llu, type=c, bootable", (unsigned long long)n);
    CHECK(strstr(g_out, "label-id: 0x1a2b3c4d") && strstr(g_out, want), "sfdisk -d: %s", g_out);
    u8 s[512];
    CHECK(pread(k.d.fd, s, 512, 0) == 512 && !memcmp(s, bootcode, 440), "boot code kept");
    pt_table_t *t = malloc(sizeof *t);
    CHECK(pt_read(&k.io, t) == 0 && t->kind == PT_MBR && t->nparts == 1 && t->free_slots == 3, "read back");
    CHECK(t->parts[0].start == 2048 && t->parts[0].count == n && t->parts[0].mbr_type == 0x0C && t->parts[0].active &&
          t->disk_sig == 0x1A2B3C4D, "read back fields");
    pt_region_t r[8];
    CHECK(pt_free_regions(t, 2048, r, 8) == 0, "no free space left");
    /* a second write: a smaller partition leaves free space at the end */
    p.count = 1000000;
    CHECK(pt_write_mbr(&k.io, &p, 1, 0x1A2B3C4D, bootcode) == 0, "rewrite");
    CHECK(pt_read(&k.io, t) == 0 && pt_free_regions(t, 2048, r, 8) == 1, "one free region");
    CHECK(r[0].start == ALIGN_UP(2048 + 1000000ull, 2048) && r[0].start + r[0].count == ALIGN_DOWN(k.io.sectors, 2048),
          "free region %llu+%llu", (unsigned long long)r[0].start, (unsigned long long)r[0].count);
    /* boot code alone */
    u8 other[440];
    memset(other, 0xCC, sizeof other);
    CHECK(pt_write_bootcode(&k.io, other) == 0, "pt_write_bootcode");
    CHECK(pread(k.d.fd, s, 512, 0) == 512 && !memcmp(s, other, 440) && s[440] == 0x4D && s[446 + 4] == 0x0C,
          "boot code replaced, signature and table kept");
    /* refusals */
    pt_new_t two[2] = { p, p };
    CHECK(pt_write_mbr(&k.io, two, 2, 1, NULL) == E_INVAL, "overlapping partitions refused");
    two[1].start = p.start + p.count; two[1].active = 1;
    CHECK(pt_write_mbr(&k.io, two, 2, 1, NULL) == E_INVAL, "two active partitions refused");
    pt_new_t big = { .start = 2048, .count = k.io.sectors, .mbr_type = 0x0C };
    CHECK(pt_write_mbr(&k.io, &big, 1, 1, NULL) == E_INVAL, "partition past the end refused");
    free(t);
    disk_close(&k);
}

static void t_gpt_new(u32 ss, u64 bytes) {
    printf("== gpt_new (%u-byte sectors, %llu MiB)\n", ss, (unsigned long long)(bytes >> 20));
    disk_t k;
    char name[64];
    snprintf(name, sizeof name, "gpt_new_%u_%llu.img", ss, (unsigned long long)(bytes >> 20));
    disk_open(&k, name, bytes, ss, 1);
    u64 mib = (1 << 20) / ss;
    u64 entsec = 128 * 128 / ss;
    u64 last = k.io.sectors - 2 - entsec;
    pt_new_t p[2] = { { .start = 2 * mib, .count = ALIGN_DOWN(last + 1, mib) - 2 * mib, .name = "Halcyon" },
                      { .start = mib, .count = mib, .name = "BIOS boot partition" } };
    memcpy(p[0].type_guid, PT_GUID_ESP, 16);
    memcpy(p[1].type_guid, PT_GUID_BIOS_BOOT, 16);
    rnd_guid(p[0].guid);
    rnd_guid(p[1].guid);
    u8 dg[16];
    rnd_guid(dg);
    CHECK(pt_write_gpt(&k.io, p, 2, dg, bootcode) == 0, "pt_write_gpt");
    CHECK(gpt_check(&k) == 0, "gpt_check: %s", g_out);
    char g0[37], g1[37], want[200];
    pt_guid_str(p[0].guid, g0);
    pt_guid_str(p[1].guid, g1);
    snprintf(want, sizeof want, "gpt 1 %llu %llu C12A7328-F81F-11D2-BA4B-00A0C93EC93B %s Halcyon",
             (unsigned long long)p[0].start, (unsigned long long)(p[0].start + p[0].count - 1), g0);
    CHECK(strstr(g_out, want) != NULL, "gpt_check lists '%s': %s", want, g_out);
    snprintf(want, sizeof want, "gpt 2 %llu %llu 21686148-6449-6E6F-744E-656564454649 %s BIOS boot partition",
             (unsigned long long)p[1].start, (unsigned long long)(p[1].start + p[1].count - 1), g1);
    CHECK(strstr(g_out, want) != NULL, "gpt_check lists '%s': %s", want, g_out);
    CHECK(sgdisk_ok(&k), "sgdisk -v: %s", g_out);
    CHECK(sfdisk_verify(&k), "sfdisk -V: %s", g_out);
    sfdisk_cmd(&k, "-d");
    char dgs[37];
    pt_guid_str(dg, dgs);
    snprintf(want, sizeof want, "label-id: %s", dgs);
    CHECK(strstr(g_out, "(skipped)") ||
          (strstr(g_out, "label: gpt") && strstr(g_out, want) && strstr(g_out, g0) && strstr(g_out, "name=\"Halcyon\"")),
          "sfdisk -d: %s", g_out);
    pt_table_t *t = malloc(sizeof *t);
    CHECK(pt_read(&k.io, t) == 0 && t->kind == PT_GPT && t->nparts == 2, "read back");
    CHECK(t->first_usable == 2 + entsec && t->last_usable == last && t->alt_lba == k.io.sectors - 1 &&
          t->ent_lba == 2 && t->nent == 128 && t->esz == 128 && !memcmp(t->disk_guid, dg, 16) &&
          t->free_entries == 126, "header fields");
    for (int i = 0; i < 2; i++)
        CHECK(t->parts[i].start == p[i].start && t->parts[i].count == p[i].count &&
              !memcmp(t->parts[i].type_guid, p[i].type_guid, 16) && !memcmp(t->parts[i].guid, p[i].guid, 16) &&
              !strcmp(t->parts[i].name, p[i].name) && t->parts[i].index == i + 1, "partition %d read back", i + 1);
    u8 s[4096];
    CHECK(pread(k.d.fd, s, ss, 0) == (ssize_t)ss && !memcmp(s, bootcode, 440) && s[440] == 0 && s[446 + 4] == 0xEE,
          "protective MBR with the boot code, signature 0");
    pt_region_t r[4];
    CHECK(pt_free_regions(t, mib, r, 4) == 0, "no free space");
    free(t);
    disk_close(&k);
}

/* a Windows-like GPT made by sgdisk (or sfdisk): ESP, MSR, basic data,
 * recovery, free space at the end; Halcyon's two partitions are added */
static void t_gpt_add(void) {
    printf("== gpt_add (next to a Windows-like layout)\n");
    disk_t k;
    disk_open(&k, "gpt_add.img", 2ull << 30, 512, 1);
    disk_close(&k);
    int st;
    if (have_sgdisk)
        st = sh("sgdisk -n 1:2048:+100M -t 1:EF00 -c 1:'EFI system partition' -n 2:0:+16M -t 2:0C01 "
                "-c 2:'Microsoft reserved partition' -n 3:0:+700M -t 3:0700 -c 3:'Basic data partition' "
                "-n 4:0:+500M -t 4:2700 -c 4:'Basic data partition' '%s/gpt_add.img'", g_work);
    else
        st = sh("printf 'label: gpt\\nstart=2048,size=100M,type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B\\n"
                "size=16M,type=E3C9E316-0B5C-4DB8-817D-F92DF00215AE\\nsize=700M,type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7\\n"
                "size=500M,type=DE94BBA4-06D1-4D40-A16A-BFD50179D6AC\\n' | sfdisk -q '%s/gpt_add.img'", g_work);
    CHECK(st == 0, "making the layout: %s", g_out);
    disk_open(&k, "gpt_add.img", 0, 512, 0);
    pt_table_t *t = malloc(sizeof *t);
    CHECK(pt_read(&k.io, t) == 0 && t->kind == PT_GPT && t->nparts == 4, "read the layout (kind %d, %d parts: %s)",
          t->kind, t->nparts, t->problem);
    if (t->nparts != 4) { free(t); disk_close(&k); return; }
    CHECK(!memcmp(t->parts[0].type_guid, PT_GUID_ESP, 16) && !memcmp(t->parts[1].type_guid, PT_GUID_MS_RESERVED, 16) &&
          !memcmp(t->parts[2].type_guid, PT_GUID_BASIC_DATA, 16) && !memcmp(t->parts[3].type_guid, PT_GUID_WIN_RECOVERY, 16),
          "types");
    u64 hashes[4];
    for (int i = 0; i < 4; i++) {
        fill_range(&k, t->parts[i].start, t->parts[i].count, (u8)(i * 40 + 1));
        hashes[i] = hash_range(&k, t->parts[i].start, t->parts[i].count);
    }
    u8 old_ents[128 * 128], pmbr_before[512];
    CHECK(pread(k.d.fd, old_ents, sizeof old_ents, 2 * 512) == sizeof old_ents, "entries");
    CHECK(pread(k.d.fd, pmbr_before, 512, 0) == 512, "pmbr");
    pt_region_t r[8];
    int nr = pt_free_regions(t, 2048, r, 8);
    CHECK(nr >= 1 && r[0].start == ALIGN_UP(t->parts[3].start + t->parts[3].count, 2048) &&
          r[0].start + r[0].count == ALIGN_DOWN(t->last_usable + 1, 2048), "the free space at the end (%d regions)", nr);
    pt_new_t p[2] = { { .start = r[0].start, .count = 2048, .name = "BIOS boot partition" },
                      { .start = r[0].start + 2048, .count = r[0].count - 2048, .name = "Halcyon" } };
    memcpy(p[0].type_guid, PT_GUID_BIOS_BOOT, 16);
    memcpy(p[1].type_guid, PT_GUID_ESP, 16);
    rnd_guid(p[0].guid);
    rnd_guid(p[1].guid);
    int idx[2] = { 0, 0 };
    CHECK(pt_add_gpt(&k.io, t, p, 2, idx) == 0 && idx[0] == 5 && idx[1] == 6, "pt_add_gpt -> %d %d", idx[0], idx[1]);
    CHECK(gpt_check(&k) == 0, "gpt_check: %s", g_out);
    CHECK(strstr(g_out, "gpt 6 ") && strstr(g_out, " Halcyon\n") && strstr(g_out, "gpt 5 "), "new entries: %s", g_out);
    CHECK(sgdisk_ok(&k), "sgdisk -v: %s", g_out);
    CHECK(sfdisk_verify(&k), "sfdisk -V: %s", g_out);
    u8 new_ents[128 * 128], pmbr_after[512];
    CHECK(pread(k.d.fd, new_ents, sizeof new_ents, 2 * 512) == sizeof new_ents, "entries");
    CHECK(!memcmp(old_ents, new_ents, 4 * 128), "the four existing entries are byte for byte unchanged");
    CHECK(!memcmp(old_ents + 6 * 128, new_ents + 6 * 128, sizeof old_ents - 6 * 128), "the unused entries stay zero");
    CHECK(pread(k.d.fd, pmbr_after, 512, 0) == 512 && !memcmp(pmbr_before, pmbr_after, 512), "protective MBR unchanged");
    for (int i = 0; i < 4; i++)
        CHECK(hash_range(&k, t->parts[i].start, t->parts[i].count) == hashes[i], "partition %d data unchanged", i + 1);
    pt_table_t *t2 = malloc(sizeof *t2);
    CHECK(pt_read(&k.io, t2) == 0 && t2->nparts == 6 && !memcmp(t2->disk_guid, t->disk_guid, 16), "read back");
    CHECK(pt_free_regions(t2, 2048, r, 8) == 0, "no free space left");
    /* the old table is stale now */
    CHECK(pt_add_gpt(&k.io, t, p, 1, idx) == E_INVAL, "a stale plan overlapping the new partitions is refused");
    if (have_sgdisk) {
        sh("sgdisk -i 6 '%s'", k.path);
        char g[37];
        pt_guid_str(p[1].guid, g);
        CHECK(strstr(g_out, "C12A7328-F81F-11D2-BA4B-00A0C93EC93B") && strstr(g_out, g) && strstr(g_out, "'Halcyon'"),
              "sgdisk -i 6: %s", g_out);
    }
    free(t2);
    free(t);
    disk_close(&k);
}

/* an MBR with Windows' active NTFS partition and free space; and one with
 * an extended partition holding logical ones */
static void t_mbr_add(void) {
    printf("== mbr_add\n");
    disk_t k;
    disk_open(&k, "mbr_add.img", 1ull << 30, 512, 1);
    disk_close(&k);
    CHECK(sh("printf 'label: dos\\nlabel-id: 0x5EED1234\\nstart=2048, size=600M, type=7, bootable\\n' | "
             "sfdisk -q '%s/mbr_add.img'", g_work) == 0, "sfdisk: %s", g_out);
    disk_open(&k, "mbr_add.img", 0, 512, 0);
    pt_table_t *t = malloc(sizeof *t);
    CHECK(pt_read(&k.io, t) == 0 && t->kind == PT_MBR && t->nparts == 1 && t->parts[0].active &&
          t->parts[0].mbr_type == 7 && t->disk_sig == 0x5EED1234, "read the layout");
    fill_range(&k, t->parts[0].start, t->parts[0].count, 7);
    u64 h = hash_range(&k, t->parts[0].start, t->parts[0].count);
    u8 before[512], after[512];
    CHECK(pread(k.d.fd, before, 512, 0) == 512, "read");
    pt_region_t r[4];
    CHECK(pt_free_regions(t, 2048, r, 4) == 1 && r[0].start == 2048 + 600 * 2048, "free region");
    pt_new_t p = { .start = r[0].start, .count = r[0].count, .mbr_type = 0x0C, .active = 0 };
    int idx = 0;
    pt_new_t act = p;
    act.active = 1;
    CHECK(pt_add_mbr(&k.io, t, &act, &idx) == E_INVAL, "a second active partition is refused");
    CHECK(pt_add_mbr(&k.io, t, &p, &idx) == 0 && idx == 2, "pt_add_mbr -> %d", idx);
    CHECK(gpt_check(&k) == 0 && strstr(g_out, "mbr 1 2048 1228800 07 1"), "gpt_check: %s", g_out);
    CHECK(sfdisk_verify(&k), "sfdisk -V: %s", g_out);
    sh("sfdisk -d '%s'", k.path);
    CHECK(strstr(g_out, "type=c\n") && strstr(g_out, "type=7, bootable"), "sfdisk -d: %s", g_out);
    CHECK(pread(k.d.fd, after, 512, 0) == 512, "read");
    CHECK(!memcmp(before, after, 446 + 16) && !memcmp(before + 446 + 32, after + 446 + 32, 512 - 446 - 32),
          "everything but the new entry unchanged (boot code, signature, Windows' entry)");
    CHECK(hash_range(&k, 2048, 600 * 2048) == h, "Windows' partition data unchanged");
    CHECK(pt_add_mbr(&k.io, t, &p, &idx) == E_BUSY, "a stale table is refused");
    free(t);
    disk_close(&k);

    /* extended partition with two logical partitions, then the full table */
    disk_open(&k, "mbr_ext.img", 1ull << 30, 512, 1);
    disk_close(&k);
    CHECK(sh("printf 'label: dos\\nstart=2048, size=100M, type=7\\nstart=206848, size=300M, type=f\\n"
             "start=208896, size=100M, type=83\\nstart=415744, size=100M, type=82\\n' | sfdisk -q '%s/mbr_ext.img'",
             g_work) == 0, "sfdisk: %s", g_out);
    disk_open(&k, "mbr_ext.img", 0, 512, 0);
    t = malloc(sizeof *t);
    CHECK(pt_read(&k.io, t) == 0 && t->kind == PT_MBR && t->nparts == 4 && t->has_extended, "read (%d parts)", t->nparts);
    int logical = 0;
    for (int i = 0; i < t->nparts; i++) if (t->parts[i].logical) logical++;
    CHECK(logical == 2 && t->parts[2].index == 5 && t->parts[3].index == 6 && t->parts[2].mbr_type == 0x83,
          "two logical partitions, numbered 5 and 6");
    int nr = pt_free_regions(t, 2048, r, 4);
    CHECK(nr == 1 && r[0].start == 206848 + 300 * 2048, "free space only outside the extended partition");
    p.start = r[0].start; p.count = 100 * 2048;
    CHECK(pt_add_mbr(&k.io, t, &p, &idx) == 0 && idx == 3, "added as partition %d", idx);
    CHECK(sfdisk_verify(&k), "sfdisk -V: %s", g_out);
    sh("sfdisk -d '%s'", k.path);
    CHECK(strstr(g_out, "type=83") && strstr(g_out, "type=82") && strstr(g_out, "type=c"), "logical partitions kept: %s", g_out);
    pt_read(&k.io, t);
    pt_new_t q = { .start = p.start + p.count, .count = 2048 * 10, .mbr_type = 0x0C };
    CHECK(pt_add_mbr(&k.io, t, &q, &idx) == 0 && idx == 4, "fourth entry");
    pt_read(&k.io, t);
    CHECK(t->free_slots == 0 && pt_free_regions(t, 2048, r, 4) == 0, "a full table has no usable free space");
    q.start += q.count;
    CHECK(pt_add_mbr(&k.io, t, &q, &idx) == E_NOSPC, "no free entry");
    free(t);
    disk_close(&k);
}

static void t_refusals(void) {
    printf("== refusals and damaged tables\n");
    disk_t k;
    pt_table_t *t = malloc(sizeof *t);
    disk_open(&k, "blank.img", 64 << 20, 512, 1);
    CHECK(pt_read(&k.io, t) == 0 && t->kind == PT_NONE, "blank disk");
    pt_region_t r[4];
    CHECK(pt_free_regions(t, 2048, r, 4) == 0, "a blank disk has no table to add to");
    disk_close(&k);
    /* superfloppy */
    disk_open(&k, "floppy.img", 64 << 20, 512, 1);
    disk_close(&k);
    sh("mkfs.fat -F 32 '%s/floppy.img'", g_work);
    disk_open(&k, "floppy.img", 0, 512, 0);
    CHECK(pt_read(&k.io, t) == 0 && t->kind == PT_OTHER && strstr(t->problem, "whole disk"), "superfloppy: %s", t->problem);
    disk_close(&k);
    /* damaged GPTs */
    disk_open(&k, "bad_gpt.img", 256 << 20, 512, 1);
    pt_new_t p = { .start = 2048, .count = 4096, .name = "x" };
    memcpy(p.type_guid, PT_GUID_BASIC_DATA, 16);
    rnd_guid(p.guid);
    u8 dg[16];
    rnd_guid(dg);
    pt_write_gpt(&k.io, &p, 1, dg, NULL);
    u8 b = 0x55;
    CHECK(pwrite(k.d.fd, &b, 1, 512 + 60) == 1, "corrupt the header");
    CHECK(pt_read(&k.io, t) == 0 && t->kind == PT_OTHER && strstr(t->problem, "primary header"), "bad header CRC: %s", t->problem);
    pt_write_gpt(&k.io, &p, 1, dg, NULL);
    CHECK(pwrite(k.d.fd, &b, 1, 2 * 512 + 200) == 1, "corrupt the entries");
    CHECK(pt_read(&k.io, t) == 0 && t->kind == PT_OTHER && strstr(t->problem, "CRC"), "bad entry CRC: %s", t->problem);
    /* missing backup: read fine, repaired by an addition */
    pt_write_gpt(&k.io, &p, 1, dg, NULL);
    static u8 zero[33 * 512];
    CHECK(pwrite(k.d.fd, zero, sizeof zero, (off_t)((k.io.sectors - 33) * 512)) == sizeof zero, "wipe the backup");
    CHECK(pt_read(&k.io, t) == 0 && t->kind == PT_GPT, "a missing backup GPT is no reason to refuse");
    pt_new_t q = p;
    q.start = 8192; q.count = 2048;
    rnd_guid(q.guid);
    int idx;
    CHECK(pt_add_gpt(&k.io, t, &q, 1, &idx) == 0 && idx == 2, "added");
    CHECK(gpt_check(&k) == 0, "backup rewritten: %s", g_out);
    CHECK(sgdisk_ok(&k), "sgdisk -v: %s", g_out);
    /* overlapping and out-of-range additions */
    pt_read(&k.io, t);
    q.start = 5000; q.count = 100;
    CHECK(pt_add_gpt(&k.io, t, &q, 1, &idx) == E_INVAL, "overlap refused");
    q.start = t->last_usable; q.count = 2;
    CHECK(pt_add_gpt(&k.io, t, &q, 1, &idx) == E_INVAL, "past the last usable LBA refused");
    /* another program changed the table meanwhile */
    pt_read(&k.io, t);
    u8 hdr[512];
    pread(k.d.fd, hdr, 512, 512);
    t->first_usable++;
    q.start = 20480; q.count = 2048;
    CHECK(pt_add_gpt(&k.io, t, &q, 1, &idx) == E_BUSY, "changed header refused");
    disk_close(&k);
    /* the MBR's limit: 2^32 sectors */
    disk_open(&k, "huge.img", 3ull << 40, 512, 1);
    pt_new_t h = { .start = 2048, .count = k.io.sectors - 4096, .mbr_type = 0x0C };
    CHECK(pt_write_mbr(&k.io, &h, 1, 1, NULL) == E_INVAL, "an MBR partition past 2 TiB is refused");
    h.count = 0xFFFFFFFFull - 2048;
    CHECK(pt_write_mbr(&k.io, &h, 1, 1, NULL) == 0, "an MBR partition up to 2 TiB");
    CHECK(pt_read(&k.io, t) == 0 && pt_free_regions(t, 2048, r, 4) == 0, "no MBR free space beyond 2 TiB");
    /* a 3 TiB GPT */
    pt_new_t g = { .start = 2048, .count = ALIGN_DOWN(k.io.sectors - 34, 2048) - 2048, .name = "Halcyon" };
    memcpy(g.type_guid, PT_GUID_ESP, 16);
    rnd_guid(g.guid);
    CHECK(pt_write_gpt(&k.io, &g, 1, dg, NULL) == 0, "3 TiB GPT");
    CHECK(gpt_check(&k) == 0, "gpt_check: %s", g_out);
    CHECK(sgdisk_ok(&k), "sgdisk -v: %s", g_out);
    u8 s[512];
    pread(k.d.fd, s, 512, 0);
    CHECK(s[446 + 12] == 0xFF && s[446 + 13] == 0xFF && s[446 + 14] == 0xFF && s[446 + 15] == 0xFF,
          "protective MBR size 0xFFFFFFFF on a disk over 2 TiB");
    disk_close(&k);
    unlink(k.path);
    free(t);
}

/* random layouts written and read back */
static void t_random(int rounds) {
    printf("== random round trips (%d)\n", rounds);
    pt_table_t *t = malloc(sizeof *t);
    for (int round = 0; round < rounds; round++) {
        u32 ss = (rnd() & 3) == 0 ? 4096 : 512;
        u64 mib = (1 << 20) / ss;
        u64 bytes = (64ull + rnd() % 4000) << 20;
        disk_t k;
        disk_open(&k, "random.img", bytes, ss, 1);
        int gpt = rnd() & 1;
        int n = 1 + (int)(rnd() % (gpt ? 12 : 4));
        pt_new_t p[12];
        u64 lo = gpt ? mib : mib, hi = gpt ? k.io.sectors - 2 - 128 * 128 / ss : k.io.sectors;
        u64 cur = lo;
        int made = 0;
        for (int i = 0; i < n; i++) {
            u64 gap = (rnd() % 4) * mib, len = 1 + rnd() % (hi / (n + 1));
            if (cur + gap + len > hi) break;
            memset(&p[made], 0, sizeof p[made]);
            p[made].start = cur + gap;
            p[made].count = len;
            p[made].mbr_type = (u8)(1 + rnd() % 0xEC);
            if (p[made].mbr_type == 0x05 || p[made].mbr_type == 0x0F || p[made].mbr_type == 0x85) p[made].mbr_type = 0x0C;
            p[made].active = !gpt && made == 0 && (rnd() & 1);
            rnd_guid(p[made].type_guid);
            rnd_guid(p[made].guid);
            static char names[12][40];
            snprintf(names[made], sizeof names[made], "Part %d \xc3\xa9\xe2\x82\xac", made + 1);
            p[made].name = names[made];
            cur = p[made].start + len;
            made++;
        }
        u8 dg[16];
        rnd_guid(dg);
        int r = gpt ? pt_write_gpt(&k.io, p, made, dg, NULL) : pt_write_mbr(&k.io, p, made, (u32)rnd() | 1, NULL);
        CHECK(r == 0, "round %d: write (%s, %d partitions): %d", round, gpt ? "GPT" : "MBR", made, r);
        CHECK(gpt_check(&k) == 0, "round %d: gpt_check: %s", round, g_out);
        if (gpt) CHECK(sgdisk_ok(&k), "round %d: sgdisk -v: %s", round, g_out);
        CHECK(pt_read(&k.io, t) == 0 && t->kind == (gpt ? PT_GPT : PT_MBR) && t->nparts == made,
              "round %d: read back (%d of %d)", round, t->nparts, made);
        for (int i = 0; i < made && i < t->nparts; i++) {
            pt_part_t *q = &t->parts[i];
            int ok = q->start == p[i].start && q->count == p[i].count && q->index == i + 1;
            if (gpt) ok = ok && !memcmp(q->type_guid, p[i].type_guid, 16) && !memcmp(q->guid, p[i].guid, 16) &&
                          !strcmp(q->name, p[i].name);
            else ok = ok && q->mbr_type == p[i].mbr_type && q->active == p[i].active;
            CHECK(ok, "round %d: partition %d read back", round, i + 1);
        }
        /* free regions never overlap a partition and are aligned */
        pt_region_t fr[16];
        int nf = pt_free_regions(t, mib, fr, 16);
        for (int i = 0; i < nf; i++) {
            int bad = fr[i].start % mib || fr[i].count % mib || !fr[i].count;
            for (int j = 0; j < made; j++)
                if (fr[i].start < p[j].start + p[j].count && p[j].start < fr[i].start + fr[i].count) bad = 1;
            CHECK(!bad, "round %d: free region %d", round, i);
        }
        /* fill the largest free region and check again */
        if (nf && (gpt ? t->free_entries : t->free_slots)) {
            pt_new_t a = { .start = fr[0].start, .count = fr[0].count, .mbr_type = 0x0C, .name = "Halcyon" };
            memcpy(a.type_guid, PT_GUID_ESP, 16);
            rnd_guid(a.guid);
            int idx;
            r = gpt ? pt_add_gpt(&k.io, t, &a, 1, &idx) : pt_add_mbr(&k.io, t, &a, &idx);
            CHECK(r == 0, "round %d: add into the free space: %d", round, r);
            CHECK(gpt_check(&k) == 0, "round %d: after the addition: %s", round, g_out);
            if (gpt) CHECK(sgdisk_ok(&k), "round %d: sgdisk -v: %s", round, g_out);
        }
        disk_close(&k);
    }
    free(t);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s WORKDIR [-v]\n", argv[0]); return 2; }
    g_work = argv[1];
    for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "-v")) g_verbose = shim_verbose = 1;
    have_sgdisk = sh("command -v sgdisk") == 0;
    have_losetup_4k = getuid() == 0 && have_sgdisk && sh("command -v losetup") == 0;
    if (!have_sgdisk) printf("(sgdisk not installed: checked with sfdisk and gpt_check.py only)\n");
    t_mbr_new();
    t_gpt_new(512, 1ull << 30);
    t_gpt_new(4096, 2ull << 30);
    t_gpt_add();
    t_mbr_add();
    t_refusals();
    t_random(60);
    printf("parttest: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
