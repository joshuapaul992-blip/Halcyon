/* Host tests of the Recycle Bin (kernel/apps/recycle.c), scenario "recycle"
 * of fstest (included by fstest.c): the bins of FAT32 and exFAT volumes made
 * by mkfs.fat / mkfs.exfat and of an NTFS volume made by the kernel's
 * formatter. Checked: the index format (read back through the VFS and,
 * independently, with mtools), unique stored names, restoring into folders
 * that are gone, conflicts (fail, replace, keep both), the size limit and
 * its oldest-first purge, recovery from damaged, truncated and half-written
 * indexes and from items the index does not list, emptying (the folder
 * goes), and the drives that have no bin (read-only, removable). Every image
 * is checked with fsck.fat / fsck.exfat / exfat_tool.py / ntfsfix afterwards. */
#include "apps/recycle.h"

static int rb_count(char L) {
    rb_entry_t *e;
    int n = rb_list(L, &e);
    kfree(e);
    return n;
}

/* the entry of drive L with this original path (copied to out), or 0 */
static int rb_find(char L, const char *orig, rb_entry_t *out) {
    rb_entry_t *e;
    int n = rb_list(L, &e), found = 0;
    for (int i = 0; i < n && !found; i++)
        if (!strcasecmp(e[i].orig, orig)) { found = 1; if (out) *out = e[i]; }
    kfree(e);
    return found;
}

static char *v_text(const char *path) {
    char *data = NULL;
    u32 sz;
    if (vfs_read_file(path, &data, &sz) < 0) return NULL;
    return data;
}

static void bin_path(char L, const char *name, char *out) { snprintf(out, VFS_MAX_PATH, "%c:/RECYCLED/%s", L, name); }

/* the index as mtools sees it on the image (FAT only) */
static char *mt_text(const img_t *im, const char *mpath) {
    const char *out = wpath("rb_mt.out");
    unlink(out);
    if (sh("mcopy -n -o -i %s %s '%s'", mt(im), mp(mpath), out)) return NULL;
    size_t n;
    u8 *b = host_read(out, &n);
    if (b) b[n] = 0;
    return (char *)b;
}

static void put_text(const char *path, const char *s) { v_put(path, (const u8 *)s, (u32)strlen(s)); }

/* the basic life of a bin on any file system */
static void t_rb_basic(const char *tag, char L) {
    printf("  %s\n", tag);
    char p[VFS_MAX_PATH], item[VFS_MAX_PATH];
    EXPECT(rb_supported(L), 0);
    EXPECT(rb_count(L), 0);
    OK(vfs_mkdir(P(L, "Docs")));
    OK(vfs_mkdir(P(L, "Docs/Sub")));
    u8 *a = pat_alloc(3000, 1), *b = pat_alloc(70000, 2);
    v_put(P(L, "Docs/a.txt"), a, 3000);
    v_put(P(L, "Docs/Sub/a.txt"), b, 70000);
    v_put(P(L, "Docs/Sub/Ünïcode näme.dat"), a, 100);
    u32 gen = rb_generation();

    /* delete: moved, listed, index written */
    int id1 = rb_delete(P(L, "Docs/a.txt"), ~0ull);
    CHECK(id1 > 0, "rb_delete = %d", id1);
    CHECK(rb_generation() != gen, "generation unchanged");
    CHECK(!v_exists(P(L, "Docs/a.txt")), "deleted file still there");
    bin_path(L, "D1.txt", item);
    v_cmp(item, a, 3000);
    rb_entry_t e;
    CHECK(rb_find(L, P(L, "Docs/a.txt"), &e), "entry not listed");
    CHECK(e.id == (u32)id1 && !e.is_dir && e.size == 3000 && e.when.year >= 2020, "entry %u dir %d size %lu year %d",
          e.id, e.is_dir, (unsigned long)e.size, e.when.year);
    char *ix = v_text(P(L, "RECYCLED/INDEX.TXT"));
    CHECK(ix && !strncmp(ix, "HALCYON RECYCLE BIN 1\r\n1\t", 25), "index header: %.40s", ix ? ix : "(none)");
    char want[VFS_MAX_PATH + 32];
    snprintf(want, sizeof want, "\t3000\tF\t%c:/Docs/a.txt\r\nEND 1\r\n", L);
    CHECK(ix && strstr(ix, want), "index line: %s", ix ? ix : "(none)");
    kfree(ix);
    CHECK(!v_exists(P(L, "RECYCLED/INDEX.NEW")), "INDEX.NEW left behind");

    /* unique names: the same name again, and from another folder */
    v_put(P(L, "Docs/a.txt"), b, 500);
    int id2 = rb_delete(P(L, "Docs/a.txt"), ~0ull);
    int id3 = rb_delete(P(L, "Docs/Sub/a.txt"), ~0ull);
    int id4 = rb_delete(P(L, "Docs/Sub/Ünïcode näme.dat"), ~0ull);
    CHECK(id2 > id1 && id3 > id2 && id4 > id3, "ids %d %d %d %d", id1, id2, id3, id4);
    EXPECT(rb_count(L), 4);
    bin_path(L, "D2.txt", item); v_cmp(item, b, 500);
    bin_path(L, "D3.txt", item); v_cmp(item, b, 70000);
    bin_path(L, "D4.dat", item); CHECK(v_exists(item), "D4.dat missing");
    CHECK(rb_find(L, P(L, "Docs/Sub/Ünïcode näme.dat"), NULL), "unicode entry");

    /* a folder */
    OK(vfs_mkdir(P(L, "Tree")));
    OK(vfs_mkdir(P(L, "Tree/Inner")));
    v_put(P(L, "Tree/one.bin"), a, 1000);
    v_put(P(L, "Tree/Inner/two.bin"), b, 2000);
    int idt = rb_delete(P(L, "Tree"), ~0ull);
    CHECK(idt > 0 && !v_exists(P(L, "Tree")), "folder delete = %d", idt);
    CHECK(rb_find(L, P(L, "Tree"), &e) && e.is_dir && e.size == 3000, "folder entry dir %d size %lu", e.is_dir, (unsigned long)e.size);
    snprintf(p, sizeof p, "D%d", idt);
    bin_path(L, p, item);
    strlcat(item, "/Inner/two.bin", sizeof item);
    v_cmp(item, b, 2000);

    /* refused: roots, the bin itself, missing files */
    EXPECT(rb_delete(P(L, ""), ~0ull), E_INVAL);
    EXPECT(rb_delete(P(L, "RECYCLED"), ~0ull), E_INVAL);
    EXPECT(rb_delete(P(L, "RECYCLED/D1.txt"), ~0ull), E_INVAL);
    EXPECT(rb_delete(P(L, "nothing.txt"), ~0ull), E_NOENT);

    /* restore into folders that are gone */
    OK(rb_delete_tree(P(L, "Docs")));
    EXPECT(rb_restore(L, (u32)id3, RB_FAIL, p, sizeof p), 0);
    CHECK(!strcmp(p, P(L, "Docs/Sub/a.txt")), "restored to %s", p);
    v_cmp(P(L, "Docs/Sub/a.txt"), b, 70000);
    EXPECT(rb_restore(L, (u32)id3, RB_FAIL, p, sizeof p), E_NOENT);       /* not in the bin any more */
    EXPECT(rb_restore(L, (u32)idt, RB_FAIL, p, sizeof p), 0);
    v_cmp(P(L, "Tree/Inner/two.bin"), b, 2000);

    /* conflicts: D1 and D2 both were Docs/a.txt */
    v_put(P(L, "Docs/a.txt"), b, 10);
    EXPECT(rb_restore(L, (u32)id1, RB_FAIL, p, sizeof p), E_EXIST);
    CHECK(rb_find(L, P(L, "Docs/a.txt"), NULL), "entry lost by a refused restore");
    v_cmp(P(L, "Docs/a.txt"), b, 10);
    EXPECT(rb_restore(L, (u32)id1, RB_RENAME, p, sizeof p), 0);
    CHECK(!strcmp(p, P(L, "Docs/a (2).txt")), "kept both as %s", p);
    v_cmp(P(L, "Docs/a (2).txt"), a, 3000);
    v_cmp(P(L, "Docs/a.txt"), b, 10);
    EXPECT(rb_restore(L, (u32)id2, RB_REPLACE, p, sizeof p), 0);
    v_cmp(P(L, "Docs/a.txt"), b, 500);
    EXPECT(rb_count(L), 1);

    /* a folder replaced by a restored folder */
    OK(vfs_mkdir(P(L, "Docs/Old")));
    v_put(P(L, "Docs/Old/x.txt"), a, 5);
    int ido = rb_delete(P(L, "Docs/Old"), ~0ull);
    OK(vfs_mkdir(P(L, "Docs/Old")));
    v_put(P(L, "Docs/Old/new.txt"), a, 6);
    EXPECT(rb_restore(L, (u32)ido, RB_REPLACE, p, sizeof p), 0);
    CHECK(v_exists(P(L, "Docs/Old/x.txt")) && !v_exists(P(L, "Docs/Old/new.txt")), "folder replace");

    /* purge one, then empty: the folder goes */
    int idp = rb_delete(P(L, "Docs/a (2).txt"), ~0ull);
    EXPECT(rb_purge(L, (u32)idp), 0);
    EXPECT(rb_purge(L, (u32)idp), E_NOENT);
    EXPECT(rb_count(L), 1);
    EXPECT(rb_empty(L, NULL, NULL), 0);
    EXPECT(rb_count(L), 0);
    CHECK(!v_exists(P(L, "RECYCLED")), "empty bin folder left behind");
    int c; u64 by;
    EXPECT(rb_usage(L, &c, &by), 0);
    CHECK(c == 0 && by == 0, "usage %d %lu", c, (unsigned long)by);
    free(a); free(b);
}

static int cancel_after;
static int tick_cancel(void *ctx, const char *name, u64 bytes) { (*(int *)ctx)++; return cancel_after && *(int *)ctx > cancel_after; }

/* the size limit: oldest first */
static void t_rb_limit(const char *tag, char L) {
    printf("  %s\n", tag);
    u8 *d = pat_alloc(200000, 7);
    char name[32];
    int ids[6];
    for (int i = 0; i < 6; i++) {
        snprintf(name, sizeof name, "L%d.bin", i);
        v_put(P(L, name), d, 100000);
        ids[i] = rb_delete(P(L, name), 350000);     /* room for 3 */
        CHECK(ids[i] > 0, "limit delete %d = %d", i, ids[i]);
    }
    EXPECT(rb_count(L), 3);
    for (int i = 0; i < 6; i++) {
        snprintf(name, sizeof name, "L%d.bin", i);
        CHECK(rb_find(L, P(L, name), NULL) == (i >= 3), "after the purge %s %s", name, i >= 3 ? "missing" : "kept");
    }
    /* too big for the bin at all: refused, left in place */
    v_put(P(L, "big.bin"), d, 200000);
    EXPECT(rb_delete(P(L, "big.bin"), 150000), E_NOSPC);
    CHECK(v_exists(P(L, "big.bin")), "refused item gone");
    /* the limit from the settings: percent of the drive */
    u64 total = 0, freeb = 0;
    vfs_statfs(L, &total, &freeb);
    char key[48];
    rb_percent_key(L, key, sizeof key);
    CHECK(!strncmp(key, "recycle.percent.", 16) && strlen(key) == 32, "key %s", key);
    EXPECT(rb_percent(L), RB_DEFAULT_PERCENT);
    settings_set("recycle.percent", "25");
    EXPECT(rb_percent(L), 25);
    settings_set(key, "1");
    EXPECT(rb_percent(L), 1);
    CHECK(rb_limit(L) == total / 100, "limit %lu of %lu", (unsigned long)rb_limit(L), (unsigned long)total);
    EXPECT(rb_delete(P(L, "big.bin"), 0), total / 100 >= 200000 ? 7 : E_NOSPC);
    settings_set(key, NULL);
    settings_set("recycle.percent", NULL);
    /* rb_enforce on its own, keeping one */
    EXPECT(rb_enforce(L, 100000, (u32)ids[3]), 2 + (total / 100 >= 200000));
    CHECK(rb_find(L, P(L, "L3.bin"), NULL) && rb_count(L) == 1, "enforce kept the wrong one");
    /* emptying with a tick that cancels */
    for (int i = 0; i < 3; i++) {
        snprintf(name, sizeof name, "M%d.bin", i);
        v_put(P(L, name), d, 1000);
        rb_delete(P(L, name), ~0ull);
    }
    int ticks = 0;
    cancel_after = 2;
    EXPECT(rb_empty(L, tick_cancel, &ticks), E_CANCELED);
    EXPECT(rb_count(L), 2);
    cancel_after = 0;
    ticks = 0;
    EXPECT(rb_empty(L, tick_cancel, &ticks), 0);
    EXPECT(ticks, 2);
    EXPECT(rb_count(L), 0);
    vfs_unlink(P(L, "big.bin"));
    free(d);
}

/* damaged indexes */
static void t_rb_recover(const char *tag, char L) {
    printf("  %s\n", tag);
    char ixp[VFS_MAX_PATH], newp[VFS_MAX_PATH], item[VFS_MAX_PATH];
    bin_path(L, "INDEX.TXT", ixp);
    bin_path(L, "INDEX.NEW", newp);
    u8 *d = pat_alloc(5000, 3);
    v_put(P(L, "r1.txt"), d, 100);
    v_put(P(L, "r2.txt"), d, 200);
    v_put(P(L, "r3.txt"), d, 300);
    int i1 = rb_delete(P(L, "r1.txt"), ~0ull), i2 = rb_delete(P(L, "r2.txt"), ~0ull), i3 = rb_delete(P(L, "r3.txt"), ~0ull);
    CHECK(i1 == 1 && i2 == 2 && i3 == 3, "ids %d %d %d", i1, i2, i3);

    /* garbage lines, a duplicate id, a line for a missing item, no END */
    char buf[2048];
    snprintf(buf, sizeof buf,
             "HALCYON RECYCLE BIN 1\r\n"
             "1\t2026-01-02 03:04:05\t100\tF\t%c:/r1.txt\r\n"
             "this is not an entry\r\n"
             "2\t2026-13-02 03:04:05\t200\tF\t%c:/r2.txt\r\n"        /* month 13 */
             "1\t2026-01-02 03:04:05\t100\tF\t%c:/dup.txt\r\n"       /* duplicate id */
             "9\t2026-01-02 03:04:05\t1\tF\t%c:/gone.txt\r\n"        /* no D9.txt */
             "3\t2026-01-02 03:04:05\t300\tF\tQ:/other drive.txt\r\n"
             "4\t2026-01-02 03:04:05\t300\tD\t%c:/wrong kind\r\n"   /* D4 does not exist either */
             "3\t2026-01-0", L, L, L, L, L);
    put_text(ixp, buf);
    /* an orphan the index never knew, and things that are not ours */
    bin_path(L, "D77.log", item);
    v_put(item, d, 77);
    bin_path(L, "desktop.ini", item);
    v_put(item, d, 5);
    bin_path(L, "Dc1.txt", item);                 /* (Windows 98's naming) */
    v_put(item, d, 6);
    rb_entry_t *e;
    int n = rb_list(L, &e);
    EXPECT(n, 4);
    int seen = 0;
    for (int i = 0; i < n; i++) {
        if (e[i].id == 1) { seen |= 1; CHECK(!strcmp(e[i].orig, P(L, "r1.txt")) && e[i].when.year == 2026 && e[i].when.second == 5, "kept entry 1: %s", e[i].orig); }
        if (e[i].id == 2) { seen |= 2; CHECK(!strcmp(e[i].orig, P(L, "D2.txt")) && e[i].size == 200, "recovered D2: %s %lu", e[i].orig, (unsigned long)e[i].size); }
        if (e[i].id == 3) { seen |= 4; CHECK(!strcmp(e[i].orig, P(L, "D3.txt")), "recovered D3: %s", e[i].orig); }
        if (e[i].id == 77) { seen |= 8; CHECK(!strcmp(e[i].orig, P(L, "D77.log")) && e[i].size == 77, "orphan: %s", e[i].orig); }
    }
    kfree(e);
    EXPECT(seen, 15);
    /* written back in order, complete */
    char *ix = v_text(ixp);
    CHECK(ix && strstr(ix, "\r\nEND 4\r\n") && !strstr(ix, "gone.txt") && !strstr(ix, "not an entry"), "rewritten index: %s", ix ? ix : "(none)");
    kfree(ix);
    /* the next id follows the highest */
    v_put(P(L, "r4.txt"), d, 10);
    EXPECT(rb_delete(P(L, "r4.txt"), ~0ull), 78);
    /* a restore of an orphan goes to the root */
    char p[VFS_MAX_PATH];
    EXPECT(rb_restore(L, 77, RB_FAIL, p, sizeof p), 0);
    v_cmp(P(L, "D77.log"), d, 77);
    vfs_unlink(P(L, "D77.log"));

    /* a complete INDEX.NEW (the old index was not replaced yet) wins */
    snprintf(buf, sizeof buf, "HALCYON RECYCLE BIN 1\r\n2\t2025-05-05 05:05:05\t200\tF\t%c:/from new.txt\r\nEND 1\r\n", L);
    put_text(newp, buf);
    n = rb_list(L, &e);
    CHECK(n == 4, "after INDEX.NEW: %d entries", n);      /* D1, D3, D78 recovered as orphans */
    int ok = 0;
    for (int i = 0; i < n; i++) if (e[i].id == 2 && !strcmp(e[i].orig, P(L, "from new.txt"))) ok = 1;
    kfree(e);
    CHECK(ok, "INDEX.NEW not used");
    CHECK(!v_exists(newp), "INDEX.NEW not renamed into place");
    /* an incomplete one (cut off) is ignored */
    snprintf(buf, sizeof buf, "HALCYON RECYCLE BIN 1\r\n2\t2024-04-04 04:04:04\t200\tF\t%c:/half.txt\r\n", L);
    put_text(newp, buf);
    n = rb_list(L, &e);
    ok = 1;
    for (int i = 0; i < n; i++) if (strstr(e[i].orig, "half")) ok = 0;
    kfree(e);
    CHECK(ok && n == 4, "half-written INDEX.NEW used (%d)", n);
    /* no index at all: everything recovered */
    vfs_unlink(ixp);
    EXPECT(rb_count(L), 4);
    /* an empty / binary index */
    put_text(ixp, "");
    EXPECT(rb_count(L), 4);
    u8 junk[600];
    pattern(junk, sizeof junk, 99);
    v_put(ixp, junk, sizeof junk);
    EXPECT(rb_count(L), 4);
    /* an index line with an escaped path round-trips */
    v_put(P(L, "100% sure.txt"), d, 3);
    int ip = rb_delete(P(L, "100% sure.txt"), ~0ull);
    ix = v_text(ixp);
    CHECK(ix && strstr(ix, "/100%25 sure.txt\r\n"), "escaped path: %s", ix ? ix : "(none)");
    kfree(ix);
    EXPECT(rb_restore(L, (u32)ip, RB_FAIL, p, sizeof p), 0);
    CHECK(v_exists(P(L, "100% sure.txt")), "escaped restore");
    vfs_unlink(P(L, "100% sure.txt"));
    /* the strangers stay; emptying keeps the folder for them */
    EXPECT(rb_empty(L, NULL, NULL), 0);
    CHECK(v_exists(P(L, "RECYCLED/desktop.ini")) && v_exists(P(L, "RECYCLED/Dc1.txt")), "foreign files deleted");
    bin_path(L, "desktop.ini", item); vfs_unlink(item);
    bin_path(L, "Dc1.txt", item); vfs_unlink(item);
    vfs_unlink(ixp);
    OK(vfs_unlink(P(L, "RECYCLED")));
    free(d);
}

static void t_rb_policy(void) {
    printf("  drives without a bin\n");
    img_t im = { 0 };
    img_super(&im, "rb_usb.img", 16, 16384, "RBUSB", NULL);
    hdisk_t *h = disk_attach(&im, 512);
    h->bd.type = BDEV_DISK_USB;                     /* a USB stick (not the boot drive) */
    char L = letter_of(h);
    EXPECT(rb_supported(L), E_NOTSUP);
    volume_t *v = vfs_volume_by_letter(L);
    v->is_boot = 1;                                 /* Halcyon's own stick keeps one */
    EXPECT(rb_supported(L), 0);
    v->is_boot = 0;
    h->bd.type = BDEV_DISK_RAM;
    v->readonly = 1;
    EXPECT(rb_supported(L), E_ROFS);
    v_exists(P(L, "x"));
    EXPECT(rb_delete(P(L, "nothing"), ~0ull), E_NOENT);
    v->readonly = 0;
    put_text(P(L, "ro.txt"), "read only");
    v->readonly = 1;
    EXPECT(rb_delete(P(L, "ro.txt"), ~0ull), E_ROFS);
    v->readonly = 0;
    CHECK(v_exists(P(L, "ro.txt")) && !v_exists(P(L, "RECYCLED")), "read-only delete changed something");
    EXPECT(rb_supported('Q'), E_NODEV);
    disk_detach(h);
    fsck(&im, "rb policy");
}

static void s_recycle(void) {
    img_t im = { 0 };
    /* FAT32: the index checked with mtools as well */
    img_super(&im, "rb_fat32.img", 32, 66000, "RBFAT32", "-s 1");
    hdisk_t *h = disk_attach(&im, 512);
    char L = letter_of(h);
    t_rb_basic("fat32 basic", L);
    t_rb_limit("fat32 limit", L);
    t_rb_recover("fat32 recovery", L);
    v_put(P(L, "kept.txt"), (const u8 *)"kept in the bin", 15);
    EXPECT(rb_delete(P(L, "kept.txt"), ~0ull), 1);
    disk_detach(h);
    char *t = mt_text(&im, "RECYCLED/INDEX.TXT");
    CHECK(t && !strncmp(t, "HALCYON RECYCLE BIN 1\r\n1\t", 25) && strstr(t, "\t15\tF\t") && strstr(t, ":/kept.txt\r\nEND 1\r\n"),
          "index read by mtools: %s", t ? t : "(none)");
    free(t);
    mt_cmp(&im, "RECYCLED/D1.txt", (const u8 *)"kept in the bin", 15);
    fsck(&im, "fat32 recycle");
    /* the bin read again after a remount */
    h = disk_attach(&im, 512);
    L = letter_of(h);
    EXPECT(rb_count(L), 1);
    EXPECT(rb_restore(L, 1, RB_FAIL, NULL, 0), 0);
    CHECK(v_exists(P(L, "kept.txt")) && !v_exists(P(L, "RECYCLED")), "restore after remount");
    disk_detach(h);
    fsck(&im, "fat32 recycle restore");

    img_exfat(&im, "rb_exfat.img", 64, 4096, "RBEXFAT", 0, 0);
    h = disk_attach(&im, 512);
    L = letter_of(h);
    t_rb_basic("exfat basic", L);
    t_rb_limit("exfat limit", L);
    t_rb_recover("exfat recovery", L);
    disk_detach(h);
    fsck(&im, "exfat recycle");
    im.fs = FS_FAT;

    /* NTFS, made by the kernel's formatter */
    img_t nt = { 0 };
    snprintf(nt.path, sizeof nt.path, "%s", wpath("rb_ntfs.img"));
    unlink(nt.path);
    sh("truncate -s 64M '%s'", nt.path);
    h = disk_attach(&nt, 512);
    EXPECT(vfs_format(&h->bd, "NTFS", "RBNTFS", 0), 0);
    L = letter_of(h);
    CHECK(L != 0, "NTFS volume not mounted");
    if (L) {
        t_rb_basic("ntfs basic", L);
        t_rb_limit("ntfs limit", L);
        t_rb_recover("ntfs recovery", L);
    }
    disk_detach(h);
    if (!sh("command -v ntfsfix")) {
        int r = sh("ntfsfix -n '%s'", nt.path);
        CHECK(r == 0, "ntfsfix -n reports errors after the NTFS recycle tests");
    } else {
        printf("    (ntfsfix missing: the NTFS volume was not checked)\n");
    }
    t_rb_policy();
}
