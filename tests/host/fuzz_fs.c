/* Fuzz harness for the Halcyon storage stack (kernel/fs/block.c, vfs.c, fat.c,
 * exfat.c, iso9660.c).
 *
 * NTFS (ntfs*.c) is covered too.
 *
 * Input: one config byte (fuzz_fs.h: sector size, read-only, boot disk,
 * I/O fault injection, operation variant) and a disk image. The image becomes a RAM block device that is registered
 * (partition scan + mount), every mounted volume is walked (vfs_list of every
 * directory up to a bounded depth, vfs_stat, every file read to the end),
 * a few mutating operations are applied (create / write / mkdir / rename /
 * unlink / truncate / statfs), the volumes are walked again and the device is
 * unregistered with a file still open (the stale handle must fail cleanly).
 *
 * Bugs are reported with abort() (libFuzzer and the replay driver both treat
 * that as a crash):
 *   - ASan / UBSan reports (the build enables both)
 *   - kernel heap leaks (shim allocation counter), devices or volumes left
 *     registered after blockdev_unregister
 *   - device I/O outside the device, any write to a read-only device
 *   - unbounded work driven by on-disk data: more CPU time than
 *     fuzz_hang_secs per input, or a file that yields more data than the
 *     whole device holds (a cluster chain loop that is not detected)
 *   - API contract violations (read returning more than asked, unterminated
 *     names in directory entries, ...)
 *   - listed names that are not a single path component ("", ".", "..",
 *     containing '/', '\\' or ':'): a hostile volume could otherwise make
 *     callers that join dir + name operate on other paths or drives
 * The harness bounds its own work (directories, entries, files, bytes), so
 * a slow input means slow kernel code, not a slow harness.
 *
 * Built two ways (tests/host/Makefile): with clang -fsanitize=fuzzer as a
 * libFuzzer target, and with gcc + fuzz_main.c as a replay driver.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "core/types.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "fs/block.h"
#include "fs/vfs.h"
#include "fuzz_fs.h"

extern size_t shim_live_allocs;
extern int shim_kprintf_format;

double fuzz_hang_secs = 5.0;
int fuzz_verbose;

/* per-input limits of the harness itself */
#define MAX_DEPTH        6
#define MAX_DIRS         24       /* vfs_list calls per walk */
#define MAX_ENTRIES      48       /* entries looked at per directory */
#define MAX_STATS        64
#define MAX_FILES        24       /* files read per walk */
#define MAX_TOTAL_READ   (24u << 20)
#define MAX_VOLS_WALKED  4
#define MAX_VOLS_MUTATED 2
#define CHUNK            (64u * 1024)

#define BUG(...) do { fprintf(stderr, "\nFUZZ BUG: " __VA_ARGS__); fprintf(stderr, " [during: %s]\n", g_op); abort(); } while (0)
#define LOG(...) do { if (fuzz_verbose) fprintf(stderr, __VA_ARGS__); } while (0)

typedef struct {
    blockdev_t bd;
    u8 *data;
    u64 bytes;
    u64 writes;
} ramdisk_t;

static const char *g_op = "idle";
static struct timespec g_t0;
static u8 *g_buf;              /* CHUNK bytes */
static u64 g_total_read;
static u32 g_io_count, g_fail_at;   /* fault injection: fail I/O number g_fail_at, then every 23rd */

static int io_fault(void) {
    g_io_count++;
    return g_fail_at && g_io_count >= g_fail_at && (g_io_count - g_fail_at) % 23 == 0;
}

static double cpu_elapsed(void) {
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (double)(t.tv_sec - g_t0.tv_sec) + (t.tv_nsec - g_t0.tv_nsec) / 1e9;
}

static void budget_check(void) {
    if (cpu_elapsed() > fuzz_hang_secs)
        BUG("time budget of %.1fs exceeded: unbounded work driven by on-disk data?", fuzz_hang_secs);
}

static int rd_read(blockdev_t *bd, u64 lba, u32 count, void *buf) {
    ramdisk_t *r = (ramdisk_t *)bd;
    if (!count || lba >= bd->sector_count || count > bd->sector_count - lba)
        BUG("device read out of range: lba %lu count %u (device %lu sectors)",
            (unsigned long)lba, count, (unsigned long)bd->sector_count);
    budget_check();
    if (io_fault()) { memset(buf, 0xA5, (size_t)count * bd->sector_size); return E_IO; }
    memcpy(buf, r->data + lba * bd->sector_size, (size_t)count * bd->sector_size);
    return 0;
}

static int rd_write(blockdev_t *bd, u64 lba, u32 count, const void *buf) {
    ramdisk_t *r = (ramdisk_t *)bd;
    if (bd->readonly) BUG("write to a read-only device");
    if (!count || lba >= bd->sector_count || count > bd->sector_count - lba)
        BUG("device write out of range: lba %lu count %u (device %lu sectors)",
            (unsigned long)lba, count, (unsigned long)bd->sector_count);
    budget_check();
    if (io_fault()) return E_IO;
    r->writes++;
    memcpy(r->data + lba * bd->sector_size, buf, (size_t)count * bd->sector_size);
    return 0;
}

/* ------------------------------------------------------------------ */

typedef struct {
    char letter;
    u64 dev_bytes;            /* size of the volume's block device */
    int dirs, files, stats;
    char first_file[VFS_MAX_PATH + 1];
    char first_dir[VFS_MAX_PATH + 1];
    char second_file[VFS_MAX_PATH + 1];
} walk_t;

static void check_dirent(const vfs_dirent_t *e) {
    if (strnlen(e->name, sizeof e->name) >= sizeof e->name) BUG("unterminated dirent name");
    if (strnlen(e->short_name, sizeof e->short_name) >= sizeof e->short_name) BUG("unterminated short name");
    if (e->is_dir != !!(e->attr & VFS_ATTR_DIR)) BUG("is_dir / attr mismatch");
    /* a listed name must be usable as one path component: anything else lets
     * a hostile volume redirect operations to other paths or drives */
    if (!e->name[0] || !strcmp(e->name, ".") || !strcmp(e->name, "..") || strchr(e->name, '/') || strchr(e->name, '\\') || strchr(e->name, ':'))
        BUG("directory entry name '%s' is not a single path component", e->name);
}

static void read_file(walk_t *w, const char *path) {
    vfs_file_t *f;
    g_op = "open+read";
    if (vfs_open(path, VFS_O_READ, &f)) return;
    u64 size = vfs_size(f), got = 0;
    for (;;) {
        if (g_total_read >= MAX_TOTAL_READ) break;
        int n = vfs_read(f, g_buf, CHUNK);
        if (n < 0) break;
        if ((u32)n > CHUNK) BUG("vfs_read returned %d > %u", n, CHUNK);
        if (n == 0) break;
        got += (u32)n;
        g_total_read += (u32)n;
        if (got > size) BUG("read %lu bytes from a file of %lu", (unsigned long)got, (unsigned long)size);
        if (got > w->dev_bytes + CHUNK)
            BUG("file %s yields more data (%lu) than the device holds (%lu): undetected chain loop",
                path, (unsigned long)got, (unsigned long)w->dev_bytes);
    }
    /* a read past the end returns 0 */
    vfs_seek(f, size + 10);
    if (vfs_read(f, g_buf, 16) > 0) BUG("read past EOF returned data");
    vfs_close(f);
    if (size <= CHUNK) {
        char *data;
        u32 n;
        g_op = "vfs_read_file";
        if (vfs_read_file(path, &data, &n) >= 0) {
            if (n > size || data[n]) BUG("vfs_read_file: %u bytes of %lu, not terminated", n, (unsigned long)size);
            kfree(data);
        }
    }
}

static void walk(walk_t *w, const char *path, int depth) {
    if (depth > MAX_DEPTH || w->dirs >= MAX_DIRS) return;
    w->dirs++;
    vfs_dirent_t *ents = NULL;
    g_op = "vfs_list";
    int n = vfs_list(path, &ents);
    budget_check();
    if (n < 0) { if (ents) BUG("vfs_list failed but returned an array"); return; }
    if (!ents) BUG("vfs_list returned NULL array");
    for (int i = 0; i < n && i < MAX_ENTRIES; i++) {
        vfs_dirent_t *e = &ents[i];
        check_dirent(e);
        char child[VFS_MAX_PATH + 64];
        vfs_join(path, e->name, child, sizeof child);
        if (w->stats < MAX_STATS) {
            vfs_dirent_t st;
            w->stats++;
            g_op = "vfs_stat";
            if (vfs_stat(child, &st) == 0) check_dirent(&st);
        }
        if (e->is_dir) {
            if (!w->first_dir[0]) strlcpy(w->first_dir, child, sizeof w->first_dir);
            walk(w, child, depth + 1);
        } else if (w->files < MAX_FILES) {
            w->files++;
            if (!w->first_file[0]) strlcpy(w->first_file, child, sizeof w->first_file);
            else if (!w->second_file[0]) strlcpy(w->second_file, child, sizeof w->second_file);
            read_file(w, child);
        }
    }
    kfree(ents);
}

static void walk_volume(walk_t *w, volume_t *v) {
    memset(w, 0, sizeof *w);
    w->letter = v->letter;
    w->dev_bytes = v->bd->sector_count * v->bd->sector_size;
    /* NTFS: sparse files and data past the initialized size read as zeros
     * without using the device, so a file may hold more than it (reads stay
     * bounded by the file size and MAX_TOTAL_READ) */
    if (!strcmp(v->fs_name, "NTFS")) w->dev_bytes = ~0ull >> 1;
    char root[4] = { v->letter, ':', '/', 0 };
    vfs_dirent_t st;
    g_op = "vfs_stat root";
    vfs_stat(root, &st);
    walk(w, root, 0);
}

/* ------------------------------------------------------------------ */

static void P(char *out, char L, const char *rel) { snprintf(out, VFS_MAX_PATH + 1, "%c:/%s", L, rel); }

static void fill(u8 *b, u32 n, u32 seed) {
    for (u32 i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; b[i] = (u8)(seed >> 16); }
}

static void mutate_volume(walk_t *w, unsigned variant) {
    static const u32 sizes[] = { 0, 1, 511, 4096, 5000, 33000, 70000, 200000 };
    char L = w->letter, p[VFS_MAX_PATH + 1], q[VFS_MAX_PATH + 1];
    u32 sz = sizes[variant & 7];
    u8 *data = kmalloc(sz ? sz : 1);
    if (!data) return;
    fill(data, sz, variant);
    vfs_file_t *f, *f2;
    u64 tot, fr;

    g_op = "statfs";
    vfs_statfs(L, &tot, &fr);
    budget_check();

    g_op = "create long name";
    P(p, L, "A new file with a long name.dat");
    vfs_write_file(p, data, sz);
    budget_check();

    g_op = "mkdir";
    P(q, L, "NEWDIR");
    vfs_mkdir(q);
    P(q, L, "NEWDIR/inner.bin");
    vfs_write_file(q, data, sz / 2 + 1);
    budget_check();

    if (w->first_file[0]) {
        g_op = "rw existing";
        if (vfs_open(w->first_file, VFS_O_RDWR, &f) == 0) {
            u64 size = vfs_size(f);
            u64 pos = (variant & 8) ? size / 2 : size + (variant & 16 ? 70000 : 3);
            vfs_seek(f, pos);
            vfs_write(f, data, sz > 9000 ? 9000 : sz);
            budget_check();
            g_op = "open twice + unlink busy";
            if (vfs_open(w->first_file, VFS_O_READ, &f2) == 0) {
                if (vfs_unlink(w->first_file) == 0) BUG("unlink of an open file succeeded");
                vfs_read(f2, g_buf, 100);
                vfs_close(f2);
            }
            g_op = "truncate";
            vfs_truncate(f, (variant & 32) ? size / 3 : size + 12345);
            budget_check();
            vfs_truncate(f, size / 7);
            vfs_seek(f, 0);
            vfs_read(f, g_buf, CHUNK);
            vfs_close(f);
            budget_check();
        }
        g_op = "trunc on open";
        if ((variant & 3) == 1 && vfs_open(w->first_file, VFS_O_WRITE | VFS_O_TRUNC, &f) == 0) vfs_close(f);
        g_op = "rename file";
        P(q, L, "NEWDIR/renamed file.bin");
        if (vfs_rename(w->first_file, q) == 0) {
            g_op = "unlink renamed";
            if (variant & 4) vfs_unlink(q);
        }
        budget_check();
    }
    if (w->second_file[0]) {
        g_op = "unlink";
        vfs_unlink(w->second_file);
        budget_check();
    }
    if (w->first_dir[0]) {
        g_op = "rename dir";
        P(q, L, "NEWDIR/moved dir");
        if (vfs_rename(w->first_dir, q) == 0) {
            g_op = "rename dir back";
            vfs_rename(q, w->first_dir);
        }
        budget_check();
        g_op = "mkdir in dir";
        char sub[VFS_MAX_PATH + 64];
        vfs_join(w->first_dir, "fuzz sub", sub, sizeof sub);
        vfs_mkdir(sub);
        g_op = "unlink dir";
        vfs_unlink(sub);
        vfs_unlink(w->first_dir);
        budget_check();
    }
    g_op = "copy";
    P(p, L, "A new file with a long name.dat");
    P(q, L, "COPY.DAT");
    vfs_copy(p, q);
    budget_check();
    g_op = "unlink new";
    vfs_unlink(p);
    P(q, L, "NEWDIR/inner.bin");
    vfs_unlink(q);
    g_op = "sync";
    vfs_sync_all();
    g_op = "statfs";
    vfs_statfs(L, &tot, &fr);
    budget_check();
    kfree(data);
}

/* ------------------------------------------------------------------ */

static int g_inited;

static void init_once(void) {
    if (g_inited) return;
    g_inited = 1;
    const char *h = getenv("FUZZ_HANG_SECS");
    if (h && atof(h) > 0) fuzz_hang_secs = atof(h);
    if (getenv("FUZZ_VERBOSE")) fuzz_verbose = 1;
    shim_kprintf_format = 1;      /* format every kernel message (catches bad %s args) */
    memset(&g_bootinfo, 0, sizeof g_bootinfo);
    blockdev_init();
    vfs_init();
    g_buf = malloc(CHUNK);
}

int fuzz_fs_image(uint8_t cfg, const uint8_t *img, size_t size) {
    init_once();
    u32 ss = (cfg & FZ_CFG_4K) ? 4096 : 512;
    unsigned variant = FZ_CFG_VARIANT(cfg);
    if (size < ss || size > (64u << 20)) return 0;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &g_t0);
    g_total_read = 0;
    g_io_count = 0;
    g_fail_at = (cfg & FZ_CFG_FAULTS) ? 1 + (variant & 0x1F) * 5 : 0;
    memset(&g_bootinfo, 0, sizeof g_bootinfo);
    if (cfg & FZ_CFG_BOOT) {        /* pretend we booted from LBA 8 of disk 0x12345678 */
        g_bootinfo.boot_disk_sig = 0x12345678;
        g_bootinfo.boot_part_lba = 8;
    }
    size_t base_allocs = shim_live_allocs;
    if (blockdev_count() || vfs_volume_count()) BUG("state left over from a previous input");

    ramdisk_t *rd = calloc(1, sizeof *rd);
    rd->bytes = size / ss * ss;
    rd->data = malloc(rd->bytes);
    memcpy(rd->data, img, rd->bytes);
    strlcpy(rd->bd.name, "fz0", sizeof rd->bd.name);
    strlcpy(rd->bd.model, "fuzz", sizeof rd->bd.model);
    rd->bd.type = BDEV_DISK_RAM;
    rd->bd.sector_size = ss;
    rd->bd.sector_count = rd->bytes / ss;
    rd->bd.readonly = (cfg & FZ_CFG_RO) != 0;
    rd->bd.removable = 1;
    rd->bd.read = rd_read;
    rd->bd.write = rd_write;

    g_op = "blockdev_register";
    int r = blockdev_register(&rd->bd);
    if (r) BUG("blockdev_register = %d", r);
    budget_check();
    int nvol = vfs_volume_count();
    LOG("fuzz: %d volume(s), %d device(s)\n", nvol, blockdev_count());

    walk_t w;
    vfs_file_t *held = NULL;          /* stays open across the unplug */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0, mutated = 0; i < vfs_volume_count() && i < MAX_VOLS_WALKED; i++) {
            volume_t *v = vfs_volume(i);
            walk_volume(&w, v);
            LOG("fuzz: pass %d %c: %d dirs %d files\n", pass, v->letter, w.dirs, w.files);
            if (pass == 0 && mutated++ < MAX_VOLS_MUTATED) mutate_volume(&w, variant + (unsigned)i);
            g_op = "open held file";
            if (pass == 1 && !held && w.first_file[0]) vfs_open(w.first_file, VFS_O_READ, &held);
        }
    }

    g_op = "blockdev_unregister";
    blockdev_unregister(&rd->bd);
    if (held) {
        g_op = "stale handle";
        if (vfs_read(held, g_buf, 16) != E_NODEV) BUG("read on a handle of an unplugged volume");
        vfs_close(held);
    }
    if (rd->bd.present) BUG("device still present after unregister");
    if (blockdev_count() || vfs_volume_count()) BUG("devices/volumes left registered after unregister");
    if (shim_live_allocs != base_allocs)
        BUG("kernel heap leak: %ld allocation(s)", (long)(shim_live_allocs - base_allocs));
    if (rd->bd.readonly && rd->writes) BUG("read-only device was written");
    budget_check();
    g_op = "idle";
    free(rd->data);
    free(rd);
    return nvol;
}

int fuzz_fs_run(const uint8_t *data, size_t size) {
    if (size < 2) return 0;
    fuzz_fs_image(data[0], data + 1, size - 1);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    return fuzz_fs_run(data, size);
}

#ifdef FUZZ_LIBFUZZER
/* Inputs are hundreds of KiB, but the interesting bytes (boot sectors,
 * partition tables, FATs, directories) are a small part of them, mostly
 * near the start of a volume. Mutate a small window in place, biased
 * towards sector starts and the beginning of the image, and plant values
 * that matter to FAT / partition code (end-of-chain marks, cluster
 * numbers, ...). Every eighth mutation is a plain whole-input one (which
 * can also change the size). */
size_t LLVMFuzzerMutate(uint8_t *data, size_t size, size_t max_size);
size_t LLVMFuzzerCustomMutator(uint8_t *data, size_t size, size_t max_size, unsigned int seed);

static u32 g_mstate;

/* exFAT protects its boot region and every directory entry set with
 * checksums, so most mutations would only exercise the rejection paths.
 * Usually re-seal them: the boot checksum of exFAT boot regions near the
 * start of the image, and the SetChecksum of File entry sets around the
 * mutated bytes (data[0] is the config byte: image offset = index - 1). */
static void exfat_reseal(uint8_t *data, size_t size, size_t lo, size_t hi) {
    uint8_t *img = data + 1;
    size_t n = size - 1;
    for (size_t off = 0; off + 512 <= n && off < 64 * 512; off += 512) {
        const uint8_t *b = img + off;
        if (memcmp(b + 3, "EXFAT   ", 8) || b[108] < 9 || b[108] > 12) continue;
        for (int reg = 0; reg < 2; reg++) {
            u32 ss = 1u << b[108];
            size_t r0 = off + (size_t)reg * 12 * ss;
            if (r0 + 12 * (size_t)ss > n) break;
            u32 c = 0;
            for (u32 i = 0; i < 11 * ss; i++) {
                if (i == 106 || i == 107 || i == 112) continue;
                c = ((c & 1) ? 0x80000000u : 0) + (c >> 1) + img[r0 + i];
            }
            for (u32 i = 0; i < ss; i += 4) memcpy(img + r0 + 11 * ss + i, &c, 4);
        }
    }
    if (lo) lo--;
    if (hi) hi--;
    size_t from = lo > 19 * 32 ? (lo - 19 * 32) & ~(size_t)31 : 0;
    for (size_t p = from; p + 64 <= n && p < hi + 32; p += 32) {
        uint8_t *e = img + p;
        if (e[0] != 0x85) continue;
        size_t len = ((size_t)e[1] + 1) * 32;
        if (e[1] < 2 || p + len > n) continue;
        u16 c = 0;
        for (size_t i = 0; i < len; i++) {
            if (i == 2 || i == 3) continue;
            c = (u16)(((c & 1) ? 0x8000 : 0) + (c >> 1) + e[i]);
        }
        e[2] = (u8)c;
        e[3] = (u8)(c >> 8);
    }
}
static u32 mrand(void) { g_mstate ^= g_mstate << 13; g_mstate ^= g_mstate >> 17; g_mstate ^= g_mstate << 5; return g_mstate; }

size_t LLVMFuzzerCustomMutator(uint8_t *data, size_t size, size_t max_size, unsigned int seed) {
    static const u32 vals[] = { 0, 1, 2, 3, 0x7F, 0x80, 0xFF, 0x100, 0x200, 0x1000, 0xFF0, 0xFF6, 0xFF7, 0xFF8,
                                0xFFF, 0xFFF0, 0xFFF7, 0xFFF8, 0xFFFF, 0x10000, 0x0FFFFFF0, 0x0FFFFFF7,
                                0x0FFFFFF8, 0x0FFFFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFF0, 0xFFFFFFFF,
                                0x41615252, 0x61417272, 0xAA55, 0xE5, 0x2E, 0x0F, 0x10, 0x40, 0x41, 0x05, 0xEE,
                                /* exFAT entry types, flags */
                                0x81, 0x82, 0x83, 0x85, 0xA0, 0xC0, 0xC1, 0xE0, 0xE1, 0x03, 0x02, 0xFFFFFFF7 };
    g_mstate = seed | 1;
    if (size < 64 || mrand() % 8 == 0) return LLVMFuzzerMutate(data, size, max_size);
    size_t lo = size, hi = 0;           /* mutated range */
    u32 k = 1 + mrand() % 3;
    for (u32 m = 0; m < k; m++) {
        size_t start;
        switch (mrand() % 4) {
        case 0: start = mrand() % MIN(size, (size_t)8192); break;
        case 1: start = 1 + (size_t)(mrand() % (size >= 512 ? MIN(size, (size_t)262144) / 512 : 1)) * 512; break;
        case 2: start = 1 + (size_t)(mrand() % (size / 512 ? size / 512 : 1)) * 512; break;
        default: start = mrand() % size; break;
        }
        if (start >= size) start = size - 1;
        size_t len = MIN((size_t)(16 << (mrand() % 9)), size - start);   /* 16 B .. 4 KiB */
        if (start < lo) lo = start;
        if (start + len > hi) hi = start + len;
        if (mrand() % 2) {
            u32 v = vals[mrand() % ARRAY_SIZE(vals)];
            if (mrand() % 4 == 0) v += mrand() % 5 - 2;
            size_t w = (size_t)1 << (mrand() % 3);                    /* 1, 2 or 4 bytes */
            size_t at = start + mrand() % len;
            for (size_t i = 0; i < w && at + i < size; i++) data[at + i] = (u8)(v >> (8 * i));
        } else {
            size_t n = LLVMFuzzerMutate(data + start, len, len);
            (void)n;                                                  /* size stays */
        }
    }
    if (mrand() % 4) exfat_reseal(data, size, lo, hi);
    return size;
}
#endif
