/* NTFSTEST.EXE - checks an NTFS volume through the file system calls. Run
 * by the automated QEMU test (the `ntfsrw` variant of tools/boottest.py:
 * an internal SATA disk with an NTFS volume labelled "Halcyon Test" that
 * has \Windows\System32, made on the host by tests/host/ntfs_mkimg.py):
 *   - the volume is read-only because it is an internal Windows disk, and
 *     the user may allow writing (VOLF_WRITE_OPT); writes fail;
 *   - HELLO.TXT and Folder\data.bin read back (data.bin's CRC-32 is
 *     printed for the host to compare); system files are not listed;
 *   - writing is allowed (volume_set_write: SETTINGS.INI, remount); files
 *     and a folder are created, written, renamed, deleted and read back;
 *   - writing is taken back (remount: the volume is synced and its dirty
 *     flag cleared), so the host finds a clean volume and checks the
 *     result with ntfsfix, ntfs_check.py and ntfs-3g.
 * Prints "ntfstest: all N checks passed" or the failures. */
#include "hal.h"

static int checks, failed;
#define CHECK(c, ...) do { checks++; if (!(c)) { failed++; printf("ntfstest: FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static unsigned crc32(const unsigned char *p, long n) {
    unsigned c = 0xFFFFFFFFu;
    for (long i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

/* the content of the written file (the host checks the same) */
static void fill(unsigned char *b, long n) {
    for (long i = 0; i < n; i++) b[i] = (unsigned char)(i * 7 + i / 251);
}

static char drv;

static const char *P(const char *rel) {
    static char b[4][300];
    static int i;
    char *o = b[i++ & 3];
    snprintf(o, 300, "%c:/%s", drv, rel);
    return o;
}

static int listed(const char *dir, const char *name) {
    static hal_dirent_t ents[64];
    int n = readdir(P(dir), ents, 64);
    for (int i = 0; i < n; i++) if (!strcmp(ents[i].name, name)) return 1;
    return 0;
}

int main(int argc, char **argv) {
    hal_volinfo_t vi;
    for (char d = 'D'; d <= 'Z' && !drv; d++)
        if (!volume_info(d, &vi) && !strcmp(vi.fs, "NTFS") && !strcmp(vi.label, "Halcyon Test")) drv = d;
    if (!drv) { printf("ntfstest: FAIL: no NTFS volume 'Halcyon Test'\n"); return 1; }
    printf("ntfstest: %c: on %s, %s\n", drv, vi.device, vi.readonly ? vi.ro_reason : "writable");
    CHECK(vi.readonly && (vi.flags & VOLF_WRITE_OPT) && !(vi.flags & VOLF_WRITE_OK),
          "an internal Windows disk is read-only until allowed (readonly %d, flags %x)", vi.readonly, vi.flags);
    CHECK(write_file(P("refused.txt"), "x", 1) < 0, "a write to the read-only volume worked");
    /* reads */
    char *data;
    long size;
    CHECK(read_file(P("HELLO.TXT"), &data, &size) >= 0 && size == 18 && !memcmp(data, "Hello from NTFS!\r\n", 18),
          "HELLO.TXT");
    if (read_file(P("folder/DATA.BIN"), &data, &size) >= 0) {
        CHECK(size == 100000, "data.bin size %ld", size);
        printf("ntfstest: data.bin %ld bytes crc %08x\n", size, crc32((unsigned char *)data, size));
    } else {
        CHECK(0, "data.bin not readable");
    }
    CHECK(listed("", "Folder") && listed("", "HELLO.TXT") && listed("", "Windows"), "root listing");
    CHECK(!listed("", "$MFT") && !listed("", "$Bitmap"), "system files listed");
    /* allow writing (as Drive Properties does) */
    CHECK(volume_set_write(drv, 1) == 0, "volume_set_write(1)");
    CHECK(!volume_info(drv, &vi) && !vi.readonly && (vi.flags & VOLF_WRITE_OK), "writable after allowing (%s)", vi.ro_reason);
    long n = 300000;
    unsigned char *buf = malloc((size_t)n);
    fill(buf, n);
    CHECK(write_file(P("Written by Halcyon.txt"), buf, n) == 0, "write a file");
    CHECK(mkdir(P("New Folder")) == 0, "mkdir");
    CHECK(write_file(P("New Folder/inner.txt"), "inner file\r\n", 12) == 0, "write in the new folder");
    CHECK(rename(P("HELLO.TXT"), P("New Folder/HELLO moved.txt")) == 0, "move HELLO.TXT");
    CHECK(write_file(P("temporary.tmp"), buf, 5000) == 0 && unlink(P("temporary.tmp")) == 0, "create and delete");
    for (int i = 0; i < 40; i++) {
        char name[64];
        snprintf(name, sizeof name, "New Folder/file %02d.txt", i);
        CHECK(write_file(P(name), name, (long)strlen(name)) == 0, "%s", name);
    }
    CHECK(read_file(P("Written by Halcyon.txt"), &data, &size) >= 0 && size == n && !memcmp(data, buf, (size_t)n),
          "read back");
    CHECK(read_file(P("new folder/hello MOVED.txt"), &data, &size) >= 0 && size == 18, "the moved file");
    /* take writing back: the volume is synced and mounted read-only again */
    CHECK(volume_set_write(drv, 0) == 0, "volume_set_write(0)");
    CHECK(!volume_info(drv, &vi) && vi.readonly, "read-only again");
    if (failed) printf("ntfstest: %d of %d checks failed\n", failed, checks);
    else printf("ntfstest: all %d checks passed\n", checks);
    return failed ? 1 : 0;
}
