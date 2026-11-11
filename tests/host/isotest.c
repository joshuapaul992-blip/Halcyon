/* ISO 9660 tests (kernel/fs/iso9660.c), #included by fstest.c.
 *
 * Images are built with xorriso -as mkisofs from a sample tree:
 *   plain level 1 / level 2 (no Rock Ridge), Rock Ridge only, Joliet only
 *   (-joliet-long -joliet-utf16: long names, non-ASCII names, a surrogate
 *   pair), Joliet + Rock Ridge (Joliet must win), on 2048-byte CD devices and
 *   512 / 4096-byte disks; hybrid layouts (protective MBR, isohybrid MBR +
 *   GPT, partition offset, appended GPT partitions, a stale ISO header under
 *   a FAT partition); a hand-made multi-extent file; and hostile images
 *   (truncation, bad descriptor / record lengths, directory loops, Rock Ridge
 *   CE loops, random directory corruption) run through the fuzz harness.
 *
 * Scenario "iso". Needs xorriso, mkfs.fat.
 */

#define ISO_XORRISO "xorriso -as mkisofs -input-charset UTF-8 -output-charset UTF-8 -quiet"
#define ISO_LONG64  "This is a rather long file name, longer than sixty-four characters, for Joliet.txt"
#define ISO_LONG110 "LLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLL.txt"
#define ISO_MANY    200
#define ISO_BIG     300001u

enum { ISO_L1, ISO_L2, ISO_RR, ISO_JOLIET, ISO_JRR };

typedef struct { const char *path; const char *text; u32 size; u32 seed; int dos; } isof_t;

static const isof_t g_isof[] = {
    { "README.TXT", "Hello from the ISO test disc.\r\n", 0, 0, 1 },
    { "BIG.BIN", NULL, ISO_BIG, 5, 1 },
    { "EMPTY.TXT", "", 0, 0, 1 },
    { "DIRA/DIRB/DIRC/DEEP.TXT", "deep inside three directories\n", 0, 0, 1 },
    { "Caf\xC3\xA9.txt", "Caf\xC3\xA9 au lait\n", 0, 0, 0 },
    { ISO_LONG64, "long name\n", 0, 0, 0 },
    { "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE3\x83\x95\xE3\x82\xA1\xE3\x82\xA4\xE3\x83\xAB.txt",
      "Japanese name\n", 0, 0, 0 },
    { "Smile \xF0\x9F\x98\x80.txt", "outside the BMP\n", 0, 0, 0 },
    { "Mixed Case Dir/Sub Dir Two/deep file.txt", "mixed case path\n", 0, 0, 0 },
    { ISO_LONG110, "very long\n", 0, 0, 0 },
};
#define ISO_TOPLEVEL 11     /* README BIG EMPTY DIRA MANY Cafe LONG64 Japanese Smile Mixed LONG110 */

static char g_iso_src[1100];

static u8 *isof_data(const isof_t *f, u32 *n) {
    if (f->text) {
        *n = (u32)strlen(f->text);
        u8 *b = malloc(*n + 1);
        memcpy(b, f->text, *n + 1);
        return b;
    }
    *n = f->size;
    return pat_alloc(f->size, f->seed);
}

static void iso_many_text(int i, char *buf, size_t n) { snprintf(buf, n, "This is file number %d.\n", i); }

static void iso_mktree(void) {
    snprintf(g_iso_src, sizeof g_iso_src, "%s", wpath("isosrc"));
    const char *s = g_iso_src;
    int r = sh("rm -rf %s && mkdir -p %s/DIRA/DIRB/DIRC %s/MANY '%s/Mixed Case Dir/Sub Dir Two'", q(s), q(s), q(s), s);
    CHECK(r == 0, "cannot create the ISO source tree");
    char path[1500];
    for (size_t i = 0; i < ARRAY_SIZE(g_isof); i++) {
        u32 n;
        u8 *d = isof_data(&g_isof[i], &n);
        snprintf(path, sizeof path, "%s/%s", s, g_isof[i].path);
        CHECK(host_write(path, d, n) == 0, "cannot write %s", path);
        free(d);
    }
    for (int i = 0; i < ISO_MANY; i++) {
        char t[64];
        iso_many_text(i, t, sizeof t);
        snprintf(path, sizeof path, "%s/MANY/FILE%03d.TXT", s, i);
        host_write(path, t, strlen(t));
    }
    /* fixed timestamps */
    sh("find %s -exec touch -d '2024-05-06 07:08:09' {} +", q(s));
}

static int iso_build(const char *out, const char *opts, const char *src) {
    unlink(out);
    int r = sh(ISO_XORRISO " %s -o %s %s", opts, q(out), q(src));
    CHECK(r == 0, "xorriso failed: %s", opts);
    return r;
}

static hdisk_t *iso_attach(const char *path, u32 ss, int type) {
    hdisk_t *h = calloc(1, sizeof *h);
    h->fd = open(path, O_RDONLY);
    struct stat st;
    fstat(h->fd, &st);
    snprintf(h->bd.name, sizeof h->bd.name, type == BDEV_DISK_CD ? "cd%d" : "hd%d", g_disk_no++);
    snprintf(h->bd.model, sizeof h->bd.model, "image");
    h->bd.type = type;
    h->bd.sector_size = ss;
    h->bd.sector_count = (u64)st.st_size / ss;
    h->bd.removable = 1;
    h->bd.readonly = 1;
    h->bd.read = hd_read;
    int r = blockdev_register(&h->bd);
    CHECK(r == 0, "blockdev_register = %d", r);
    return h;
}

static void iso_cmp_file(char L, const char *rel, const u8 *d, u32 n) { v_cmp(P(L, rel), d, n); }

/* listing / content checks of the sample tree */
static void t_iso_tree(const char *tag, const char *img, u32 ss, int mode, const char *label) {
    if (shim_verbose) printf("  %s\n", tag);
    hdisk_t *h = iso_attach(img, ss, ss == 2048 ? BDEV_DISK_CD : BDEV_DISK_RAM);
    char L = letter_of(h);
    CHECK(L, "%s: not mounted", tag);
    if (!L) { disk_detach(h); return; }
    volume_t *v = vfs_volume_by_letter(L);
    CHECK(!strcmp(v->fs_name, "ISO9660"), "%s: fs_name '%s'", tag, v->fs_name);
    CHECK(!strcmp(v->label, label), "%s: label '%s', want '%s'", tag, v->label, label);
    CHECK(v->bd == &h->bd, "%s: not mounted on the whole device", tag);

    /* root listing */
    vfs_dirent_t *ents;
    int n = vfs_list(P(L, ""), &ents);
    EXPECT(n, ISO_TOPLEVEL);
    for (int i = 0; i < n; i++) {
        const char *nm = ents[i].name;
        size_t len = strlen(nm);
        CHECK(!strchr(nm, ';'), "%s: version suffix not stripped: '%s'", tag, nm);
        CHECK(len && nm[len - 1] != '.', "%s: trailing dot: '%s'", tag, nm);
        CHECK(ents[i].is_dir == !!(ents[i].attr & VFS_ATTR_DIR), "%s: attr", tag);
        if (!ents[i].is_dir) CHECK(ents[i].attr & VFS_ATTR_READONLY, "%s: '%s' not read-only", tag, nm);
        if (mode == ISO_L1) {
            for (const char *c = nm; *c; c++)
                CHECK((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '.',
                      "%s: unexpected level 1 name '%s'", tag, nm);
            CHECK(len <= 12, "%s: level 1 name too long: '%s'", tag, nm);
        }
    }
    if (n >= 0) kfree(ents);
    EXPECT(v_count(P(L, "MANY")), ISO_MANY);
    EXPECT(v_count(P(L, "DIRA/DIRB")), 1);

    /* 8.3 names are the same in every mode */
    for (size_t i = 0; i < ARRAY_SIZE(g_isof); i++) {
        if (!g_isof[i].dos && mode <= ISO_L2) continue;
        if (!strcmp(g_isof[i].path, ISO_LONG110) && mode != ISO_RR) continue;
        u32 len;
        u8 *d = isof_data(&g_isof[i], &len);
        iso_cmp_file(L, g_isof[i].path, d, len);
        free(d);
    }
    for (int i = 0; i < ISO_MANY; i += 41) {
        char t[64], rel[64];
        iso_many_text(i, t, sizeof t);
        snprintf(rel, sizeof rel, "MANY/FILE%03d.TXT", i);
        iso_cmp_file(L, rel, (const u8 *)t, (u32)strlen(t));
    }
    char t[64];
    iso_many_text(ISO_MANY - 1, t, sizeof t);
    iso_cmp_file(L, "many/file199.txt", (const u8 *)t, (u32)strlen(t));       /* case-insensitive */
    iso_cmp_file(L, "readme.txt", (const u8 *)g_isof[0].text, (u32)strlen(g_isof[0].text));
    iso_cmp_file(L, "dira/DirB/dirc/Deep.Txt", (const u8 *)g_isof[3].text, (u32)strlen(g_isof[3].text));

    vfs_dirent_t st;
    EXPECT(vfs_stat(P(L, "BIG.BIN"), &st), 0);
    EXPECT(st.size, ISO_BIG);
    EXPECT(st.is_dir, 0);
    EXPECT(st.mtime.year, 2024);
    EXPECT(st.mtime.month, 5);
    EXPECT(st.mtime.day, 6);
    EXPECT(vfs_stat(P(L, "DIRA"), &st), 0);
    EXPECT(st.is_dir, 1);
    EXPECT(st.size, 0);
    EXPECT(vfs_stat(P(L, ""), &st), 0);
    EXPECT(st.is_dir, 1);
    EXPECT(vfs_stat(P(L, "EMPTY.TXT"), &st), 0);
    EXPECT(st.size, 0);
    EXPECT(vfs_stat(P(L, "NOPE.TXT"), &st), E_NOENT);
    EXPECT(vfs_stat(P(L, "README.TXT/x"), &st), E_NOTDIR);
    EXPECT(vfs_stat(P(L, "NODIR/x"), &st), E_NOENT);

    if (mode >= ISO_RR) {
        CHECK(v_listed(P(L, ""), "Caf\xC3\xA9.txt", NULL), "%s: Caf\\xe9.txt not listed", tag);
        CHECK(v_listed(P(L, ""), "Mixed Case Dir", &st) && st.is_dir, "%s: Mixed Case Dir", tag);
        CHECK(v_listed(P(L, ""), ISO_LONG64, NULL), "%s: long name not listed", tag);
        CHECK(v_listed(P(L, ""), "Smile \xF0\x9F\x98\x80.txt", NULL), "%s: surrogate pair name not listed", tag);
        iso_cmp_file(L, "CAF\xC3\xA9.TXT", (const u8 *)g_isof[4].text, (u32)strlen(g_isof[4].text));
        iso_cmp_file(L, "mixed case dir/SUB DIR TWO/DEEP FILE.TXT", (const u8 *)g_isof[8].text,
                     (u32)strlen(g_isof[8].text));
        /* Joliet names are at most 103 characters: with Joliet present
         * (also next to Rock Ridge) the 114-character name is truncated */
        int full = v_exists(P(L, ISO_LONG110));
        CHECK(full == (mode == ISO_RR), "%s: 110-character name %s", tag, full ? "present" : "missing");
        if (mode != ISO_RR) {
            int found = 0;
            n = vfs_list(P(L, ""), &ents);
            for (int i = 0; i < n; i++) if (!strncmp(ents[i].name, "LLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLLL", 64)) found = 1;
            if (n >= 0) kfree(ents);
            CHECK(found, "%s: truncated Joliet name not listed", tag);
        }
    }

    /* partial reads */
    vfs_file_t *f;
    u8 *big = pat_alloc(ISO_BIG, 5), buf[6000];
    if (vfs_open(P(L, "BIG.BIN"), VFS_O_READ, &f) == 0) {
        EXPECT(vfs_size(f), ISO_BIG);
        vfs_seek(f, 70001);
        EXPECT(vfs_read(f, buf, 5000), 5000);
        CHECK(!memcmp(buf, big + 70001, 5000), "%s: partial read differs", tag);
        vfs_seek(f, ISO_BIG - 11);
        EXPECT(vfs_read(f, buf, 6000), 11);
        CHECK(!memcmp(buf, big + ISO_BIG - 11, 11), "%s: tail read differs", tag);
        EXPECT(vfs_read(f, buf, 6000), 0);
        vfs_seek(f, ISO_BIG + 5000);
        EXPECT(vfs_read(f, buf, 10), 0);
        EXPECT(vfs_write(f, buf, 10), E_INVAL);
        vfs_close(f);
    } else {
        CHECK(0, "%s: cannot open BIG.BIN", tag);
    }
    free(big);

    /* read-only */
    EXPECT(vfs_open(P(L, "README.TXT"), VFS_O_RDWR, &f), E_ROFS);
    EXPECT(vfs_open(P(L, "README.TXT"), VFS_O_WRITE | VFS_O_TRUNC, &f), E_ROFS);
    EXPECT(vfs_open(P(L, "NEW.TXT"), VFS_O_READ | VFS_O_CREATE, &f), E_ROFS);
    EXPECT(vfs_open(P(L, "README.TXT"), VFS_O_READ | VFS_O_CREATE | VFS_O_EXCL, &f), E_EXIST);
    EXPECT(vfs_open(P(L, "DIRA"), VFS_O_READ, &f), E_ISDIR);
    EXPECT(vfs_open(P(L, "nothere"), VFS_O_READ, &f), E_NOENT);
    EXPECT(vfs_write_file(P(L, "NEW.TXT"), "x", 1), E_ROFS);
    EXPECT(vfs_mkdir(P(L, "NEWDIR")), E_ROFS);
    EXPECT(vfs_unlink(P(L, "README.TXT")), E_ROFS);
    EXPECT(vfs_rename(P(L, "README.TXT"), P(L, "X.TXT")), E_ROFS);
    EXPECT(vfs_list(P(L, "README.TXT"), &ents), E_NOTDIR);
    u64 tot = 0, fr = 1;
    EXPECT(vfs_statfs(L, &tot, &fr), 0);
    CHECK(tot > ISO_BIG && fr == 0, "%s: statfs %lu / %lu", tag, (unsigned long)tot, (unsigned long)fr);

    /* a handle survives until close, then the media goes away */
    OK(vfs_open(P(L, "README.TXT"), VFS_O_READ, &f));
    disk_detach(h);
    EXPECT(vfs_read(f, buf, 10), E_NODEV);
    EXPECT(vfs_close(f), 0);
}

/* ------------------------------------------------------------------ */
/* image surgery helpers                                                */

typedef struct { u8 *b; size_t n; } ibuf_t;

static ibuf_t iso_load(const char *path) {
    ibuf_t r;
    r.b = host_read(path, &r.n);
    CHECK(r.b && r.n > 40000, "cannot load %s", path);
    return r;
}

/* descriptor of the given type (1 = PVD, 2 = first SVD) */
static u8 *iso_vd(ibuf_t *im, int type) {
    for (size_t off = 32768; off + 2048 <= im->n; off += 2048) {
        u8 *d = im->b + off;
        if (memcmp(d + 1, "CD001", 5) || d[0] == 255) break;
        if (d[0] == type) return d;
    }
    return NULL;
}

/* directory record whose identifier is id (plain) or id in UTF-16BE
 * (joliet), with or without a ";1" version */
static u8 *iso_find(ibuf_t *im, u32 lba, u32 len, const char *id, int joliet) {
    u8 want[256];
    u32 wl = 0;
    for (const char *c = id; *c && wl < 250; c++) {
        if (joliet) want[wl++] = 0;
        want[wl++] = (u8)*c;
    }
    for (u32 pos = 0; pos < len && (size_t)lba * 2048 + pos < im->n;) {
        u8 *r = im->b + (size_t)lba * 2048 + pos;
        if (!r[0]) { pos = (pos / 2048 + 1) * 2048; continue; }
        u32 nl = r[32];
        if (nl >= wl && !memcmp(r + 33, want, wl) &&
            (nl == wl || (!joliet && r[33 + wl] == ';') || (joliet && nl > wl + 1 && r[34 + wl] == ';')))
            return r;
        pos += r[0];
    }
    return NULL;
}

static u8 *iso_root_rec(u8 *vd) { return vd + 156; }
static u8 *iso_path(ibuf_t *im, u8 *vd, const char *path, int joliet) {
    u8 *rec = iso_root_rec(vd);
    char comp[256];
    while (rec && *path) {
        const char *sl = strchr(path, '/');
        size_t n = sl ? (size_t)(sl - path) : strlen(path);
        memcpy(comp, path, n);
        comp[n] = 0;
        rec = iso_find(im, g32(rec + 2), g32(rec + 10), comp, joliet);
        path += n + (sl ? 1 : 0);
    }
    CHECK(rec, "image layout: '%s' not found", path);
    return rec;
}

static void iso_set_both32(u8 *p, u32 v) {
    s32(p, v);
    p[4] = (u8)(v >> 24); p[5] = (u8)(v >> 16); p[6] = (u8)(v >> 8); p[7] = (u8)v;
}

static void iso_save(const ibuf_t *im, const char *path) { host_write(path, im->b, im->n); }

/* ------------------------------------------------------------------ */
/* hybrid layouts                                                       */

static int vols_on(hdisk_t *h, int *whole) {
    int n = 0;
    *whole = 0;
    for (int i = 0; i < vfs_volume_count(); i++) {
        volume_t *v = vfs_volume(i);
        if (v->bd == &h->bd) { n++; *whole = 1; }
        else if (v->bd && v->bd->parent == &h->bd) n++;
    }
    return n;
}

static volume_t *vol_fs(hdisk_t *h, const char *fs) {
    for (int i = 0; i < vfs_volume_count(); i++) {
        volume_t *v = vfs_volume(i);
        if ((v->bd == &h->bd || (v->bd && v->bd->parent == &h->bd)) && !strcmp(v->fs_name, fs)) return v;
    }
    return NULL;
}

static void t_iso_hybrid_one(const char *tag, const char *img, int want_vols, int want_whole, const char *fat_label) {
    hdisk_t *h = iso_attach(img, 512, BDEV_DISK_USB);
    int whole;
    int n = vols_on(h, &whole);
    CHECK(n == want_vols, "%s: %d volumes, want %d", tag, n, want_vols);
    volume_t *iv = vol_fs(h, "ISO9660");
    CHECK(iv && !strcmp(iv->label, "HYBRID"), "%s: ISO volume missing", tag);
    CHECK(whole == want_whole, "%s: whole-disk volume %d, want %d", tag, whole, want_whole);
    if (iv) {
        char L = iv->letter;
        iso_cmp_file(L, "readme.txt", (const u8 *)g_isof[0].text, (u32)strlen(g_isof[0].text));
        u32 len;
        u8 *d = isof_data(&g_isof[1], &len);
        iso_cmp_file(L, "BIG.BIN", d, len);
        free(d);
    }
    if (fat_label) {
        volume_t *fv = vol_fs(h, "FAT12");
        CHECK(fv && !strcmp(fv->label, fat_label), "%s: FAT partition not mounted", tag);
    }
    disk_detach(h);
}

static void t_iso_hybrid(void) {
    char src[1200], efi[1300], img[1300];
    snprintf(src, sizeof src, "%s", wpath("hybsrc"));
    snprintf(efi, sizeof efi, "%s/efi.img", src);
    int r = sh("rm -rf %s && mkdir -p %s && cp %s/README.TXT %s/BIG.BIN %s/ && mkfs.fat -C -n EFIBOOT %s 1440",
               q(src), q(src), q(g_iso_src), q(g_iso_src), q(src), q(efi));
    CHECK(r == 0, "hybrid source tree");
    const char *mbr = wpath("fake_mbr.bin");
    sh("head -c 432 /dev/zero > %s", q(mbr));

    /* plain image: superfloppy path on a 512-byte stick */
    snprintf(img, sizeof img, "%s", wpath("hyb_plain.iso"));
    if (!iso_build(img, "-V HYBRID -J", src)) t_iso_hybrid_one("plain ISO on a stick", img, 1, 1, NULL);

    /* xorriso --protective-msdos-label: one partition from LBA 1 over the image */
    snprintf(img, sizeof img, "%s", wpath("hyb_prot.iso"));
    if (!iso_build(img, "-V HYBRID -J --protective-msdos-label", src))
        t_iso_hybrid_one("protective MBR", img, 1, 1, NULL);

    /* isohybrid MBR: type 0 entry at LBA 0 over the image + EFI partition
     * (inside the image) + GPT that is not protective */
    snprintf(img, sizeof img, "%s", wpath("hyb_isohybrid.iso"));
    char opts[1600];
    snprintf(opts, sizeof opts, "-V HYBRID -J -isohybrid-mbr %s -e efi.img -no-emul-boot -isohybrid-gpt-basdat", q(mbr));
    if (!iso_build(img, opts, src)) t_iso_hybrid_one("isohybrid MBR + EFI", img, 2, 1, "EFIBOOT");

    /* partition 1 at 32 KiB is a self-contained ISO (second descriptor set) */
    snprintf(img, sizeof img, "%s", wpath("hyb_offset.iso"));
    snprintf(opts, sizeof opts, "-V HYBRID -J -partition_offset 16 -append_partition 2 0xef %s", q(efi));
    if (!iso_build(img, opts, src)) t_iso_hybrid_one("partition offset 16", img, 2, 0, "EFIBOOT");

    /* GPT: "Gap0" partition from 32 KiB, appended EFI partition */
    snprintf(img, sizeof img, "%s", wpath("hyb_gpt.iso"));
    snprintf(opts, sizeof opts, "-V HYBRID -J -append_partition 2 0xef %s -appended_part_as_gpt", q(efi));
    if (!iso_build(img, opts, src)) t_iso_hybrid_one("appended GPT", img, 2, 1, "EFIBOOT");

    /* the same image on a 4096-byte-sector disk */
    snprintf(img, sizeof img, "%s", wpath("hyb_plain.iso"));
    sh("truncate -s %%4096 %s", q(img));
    hdisk_t *h = iso_attach(img, 4096, BDEV_DISK_SATA);
    char L = letter_of(h);
    CHECK(L && !strcmp(vfs_volume_by_letter(L)->fs_name, "ISO9660"), "ISO on a 4K disk not mounted");
    if (L) iso_cmp_file(L, "README.TXT", (const u8 *)g_isof[0].text, (u32)strlen(g_isof[0].text));
    disk_detach(h);

    /* stale ISO descriptors under a FAT partition at 1 MiB: only FAT mounts */
    img_t fim;
    img_mbr(&fim, "stale_iso.img", 16, 16, "NEWFAT", 0x5741AE00);
    sh("dd if=%s of=%s bs=1024 skip=32 seek=32 count=32 conv=notrunc", q(wpath("hyb_prot.iso")), q(fim.path));
    h = iso_attach(fim.path, 512, BDEV_DISK_USB);
    int whole;
    EXPECT(vols_on(h, &whole), 1);
    CHECK(!whole && vol_fs(h, "FAT16"), "stale ISO header: FAT partition not the only volume");
    disk_detach(h);
}

/* ------------------------------------------------------------------ */
/* multi-extent files (made by hand: no tool writes them below 4 GiB)   */

static void iso_mkrec(u8 *r, u32 lba, u32 len, u8 flags, const char *id) {
    u32 nl = (u32)strlen(id);
    u32 rl = 33 + nl + ((nl & 1) ? 0 : 1);
    memset(r, 0, rl);
    r[0] = (u8)rl;
    iso_set_both32(r + 2, lba);
    iso_set_both32(r + 10, len);
    r[18] = 124; r[19] = 1; r[20] = 2; r[21] = 3; r[22] = 4; r[23] = 5;
    r[25] = flags;
    r[28] = 1; r[31] = 1;
    r[32] = (u8)nl;
    memcpy(r + 33, id, nl);
}

static void t_iso_multiextent(void) {
    char src[1200];
    snprintf(src, sizeof src, "%s", wpath("mxsrc"));
    sh("rm -rf %s && mkdir -p %s", q(src), q(src));
    u8 *a = pat_alloc(8192, 21), *b = pat_alloc(5000, 22);
    char p[1300];
    snprintf(p, sizeof p, "%s/AAAA.BIN", src); host_write(p, a, 8192);
    snprintf(p, sizeof p, "%s/BBBB.BIN", src); host_write(p, b, 5000);
    const char *img = wpath("multi.iso");
    if (iso_build(img, "--norock -V MULTI", src)) { free(a); free(b); return; }
    ibuf_t im = iso_load(img);
    u8 *pvd = iso_vd(&im, 1);
    u8 *ra = pvd ? iso_find(&im, g32(iso_root_rec(pvd) + 2), 2048, "AAAA.BIN", 0) : NULL;
    u8 *rb = pvd ? iso_find(&im, g32(iso_root_rec(pvd) + 2), 2048, "BBBB.BIN", 0) : NULL;
    CHECK(ra && rb && ra < rb, "multi-extent: unexpected layout");
    if (!ra || !rb) { free(im.b); free(a); free(b); return; }
    u32 la = g32(ra + 2), lb = g32(rb + 2);
    ra[25] |= 0x80;                       /* AAAA continues in the next record */
    memcpy(rb + 33, "AAAA", 4);
    iso_save(&im, img);
    u8 *ab = malloc(8192 + 5000);
    memcpy(ab, a, 8192);
    memcpy(ab + 8192, b, 5000);
    hdisk_t *h = iso_attach(img, 2048, BDEV_DISK_CD);
    char L = letter_of(h);
    CHECK(L, "multi-extent image not mounted");
    if (L) {
        EXPECT(v_count(P(L, "")), 1);
        vfs_dirent_t st;
        EXPECT(vfs_stat(P(L, "AAAA.BIN"), &st), 0);
        EXPECT(st.size, 8192 + 5000);
        iso_cmp_file(L, "aaaa.bin", ab, 8192 + 5000);
        vfs_file_t *f;
        u8 buf[3000];
        if (vfs_open(P(L, "AAAA.BIN"), VFS_O_READ, &f) == 0) {
            vfs_seek(f, 8192 - 1000);               /* across the extent boundary */
            EXPECT(vfs_read(f, buf, 3000), 3000);
            CHECK(!memcmp(buf, ab + 8192 - 1000, 3000), "multi-extent: read across extents differs");
            vfs_close(f);
        }
    }
    disk_detach(h);

    /* rebuild the root: 70 extents of AAAA (more than the driver takes),
     * then a chain whose extents add up to more than the disc */
    u32 root = g32(iso_root_rec(pvd) + 2), dlen = 4096;
    iso_set_both32(iso_root_rec(pvd) + 10, dlen);          /* two blocks */
    u8 *dir = im.b + (size_t)root * 2048;
    u32 pos0 = dir[0] + dir[dir[0]], pos = pos0;          /* keep "." and ".." */
    memset(dir + pos, 0, dlen - pos);
    for (int i = 0; i < 70; i++) {
        if (pos % 2048 + 44 > 2048) pos = (pos / 2048 + 1) * 2048;     /* records never cross blocks */
        iso_mkrec(dir + pos, la, 2048, i < 69 ? 0x80 : 0, "MANY.BIN;1");
        pos += dir[pos];
    }
    iso_save(&im, img);
    h = iso_attach(img, 2048, BDEV_DISK_CD);
    L = letter_of(h);
    if (L) {
        vfs_dirent_t st;
        EXPECT(v_count(P(L, "")), 1);
        EXPECT(vfs_stat(P(L, "MANY.BIN"), &st), 0);
        EXPECT(st.size, 70 * 2048);
        vfs_file_t *f;
        EXPECT(vfs_open(P(L, "MANY.BIN"), VFS_O_READ, &f), E_NOTSUP);
    }
    disk_detach(h);

    pos = pos0;
    memset(dir + pos, 0, dlen - pos);
    for (int i = 0; i < 3; i++) {
        iso_mkrec(dir + pos, lb, 0x7FFFF800, i < 2 ? 0x80 : 0, "HUGE.BIN;1");
        pos += dir[pos];
    }
    iso_mkrec(dir + pos, lb, 5000, 0x80, "LAST.BIN;1");      /* chain cut by the end of the directory */
    iso_save(&im, img);
    h = iso_attach(img, 2048, BDEV_DISK_CD);
    L = letter_of(h);
    if (L) {
        vfs_file_t *f;
        EXPECT(v_count(P(L, "")), 2);
        EXPECT(vfs_open(P(L, "HUGE.BIN"), VFS_O_READ, &f), E_IO);
        EXPECT(vfs_open(P(L, "LAST.BIN"), VFS_O_READ, &f), E_NOTSUP);
    }
    disk_detach(h);
    free(im.b);
    free(a);
    free(b);
    free(ab);
}

/* ------------------------------------------------------------------ */
/* hostile images                                                       */

#define ICASE(desc) for (int _once = (h = h_copy(&base), snprintf(what, sizeof what, "%s: %s", tag, desc), 1); \
                         _once; _once = 0, h_free(&h))

/* mount through a 2048-byte CD device and walk it (the fuzz harness covers
 * 512 / 4096-byte devices); returns the volume count */
static int iso_cd_walk(const char *what, himg_t *hi) {
    const char *path = wpath("hostile.iso");
    host_write(path, hi->b, hi->n / 2048 * 2048);
    hdisk_t *h = iso_attach(path, 2048, BDEV_DISK_CD);
    int nv = vfs_volume_count();
    char L = letter_of(h);
    if (L) {
        u64 got;
        vfs_dirent_t *ents;
        int n = vfs_list(P(L, ""), &ents);
        for (int i = 0; i < n && i < 64; i++) {
            char p[VFS_MAX_PATH + 16];
            snprintf(p, sizeof p, "%c:/%s", L, ents[i].name);
            if (ents[i].is_dir) v_count(p);
            else h_read_all(p, &got);
        }
        if (n >= 0) kfree(ents);
    }
    disk_detach(h);
    (void)what;
    return nv;
}

static int iso_run(const char *what, himg_t *h, int want) {
    int nv = h_run(what, h, want);
    int cv = iso_cd_walk(what, h);
    if (want >= 0) CHECK(cv == want, "%s: %d volume(s) on a CD device, want %d", what, cv, want);
    return nv;
}

static void t_iso_hostile_one(const char *tag, const char *img, int joliet) {
    ibuf_t im = iso_load(img);
    if (!im.b) return;
    himg_t base = { im.b, im.n, 0 }, h;
    char what[200];
    u8 *pvd = iso_vd(&im, 1), *svd = iso_vd(&im, 2);
    CHECK(pvd && (!joliet || svd), "%s: descriptors not found", tag);
    if (!pvd || (joliet && !svd)) { free(im.b); return; }
    size_t o_pvd = (size_t)(pvd - im.b), o_svd = svd ? (size_t)(svd - im.b) : 0;
    u8 *vd = joliet ? svd : pvd;
    size_t o_vd = (size_t)(vd - im.b);
    u32 root = g32(vd + 156 + 2), rootlen = g32(vd + 156 + 10);
    u8 *sub = iso_path(&im, vd, "SUB", joliet), *inner = iso_path(&im, vd, "SUB/INNER", joliet);
    u8 *data = iso_path(&im, vd, "DATA.BIN", joliet);
    if (!sub || !inner || !data) { free(im.b); return; }
    size_t o_sub = (size_t)(sub - im.b), o_inner = (size_t)(inner - im.b), o_data = (size_t)(data - im.b);
    u32 sublba = g32(sub + 2), innerlba = g32(inner + 2);
#define AT(o) (h.b + (o))

    ICASE("unmodified") iso_run(what, &h, 1);
    ICASE("truncated after the descriptors") { h.n = 36 * 1024; iso_run(what, &h, 0); }
    ICASE("truncated in the middle") { h.n = h.n / 2 / 2048 * 2048; iso_run(what, &h, -1); }
    ICASE("truncated to 4 KiB") { h.n = 4096; iso_run(what, &h, 0); }
    ICASE("no descriptor terminator, copies all the way") {
        for (size_t o = 32768 + 2048; o + 2048 <= h.n && o < 32768 + 40 * 2048; o += 2048) memcpy(AT(o), AT(o_pvd), 2048);
        iso_run(what, &h, -1);
    }
    ICASE("PVD block size 0") { s16(AT(o_pvd + 128), 0); iso_run(what, &h, 0); }
    ICASE("PVD block size 4096") { s16(AT(o_pvd + 128), 4096); iso_run(what, &h, 0); }
    ICASE("PVD block size 1000") { s16(AT(o_pvd + 128), 1000); iso_run(what, &h, 0); }
    ICASE("PVD root record length 0") { AT(o_pvd + 156)[0] = 0; iso_run(what, &h, 0); }
    ICASE("PVD root data length 0xFFFFFFFF") { iso_set_both32(AT(o_pvd + 156 + 10), 0xFFFFFFFF); iso_run(what, &h, 0); }
    ICASE("PVD root extent past the device") { iso_set_both32(AT(o_pvd + 156 + 2), 0xFFFFFFF0); iso_run(what, &h, 0); }
    ICASE("PVD root extent + XAR length wraps") {
        iso_set_both32(AT(o_pvd + 156 + 2), 0xFFFFFFFF); AT(o_pvd + 156)[1] = 5; iso_run(what, &h, 0);
    }
    ICASE("volume space size huge") { iso_set_both32(AT(o_pvd + 80), 0xFFFFFFFF); iso_run(what, &h, 1); }
    if (joliet) {
        ICASE("Joliet root past the device") { iso_set_both32(AT(o_svd + 156 + 2), 0xFFFFFF00); iso_run(what, &h, 1); }
        ICASE("Joliet block size differs") { s16(AT(o_svd + 128), 1024); iso_run(what, &h, 1); }
        ICASE("Joliet root on file data") { iso_set_both32(AT(o_svd + 156 + 2), g32(AT(o_data) + 2)); iso_run(what, &h, 1); }
    }
    ICASE("root records with length 1") {
        for (u32 i = 0; i < 2048; i += 97) AT((size_t)root * 2048 + i)[0] = 1;
        iso_run(what, &h, 1);
    }
    ICASE("record length past the block") { AT(o_data)[0] = 0xFF; iso_run(what, &h, 1); }
    ICASE("name length past the record") { AT(o_data)[32] = 0xFF; iso_run(what, &h, 1); }
    ICASE("name length 0") { AT(o_data)[32] = 0; iso_run(what, &h, 1); }
    ICASE("file data length 0xFFFFFFFF") { iso_set_both32(AT(o_data) + 10, 0xFFFFFFFF); iso_run(what, &h, 1); }
    ICASE("file extent past the device") { iso_set_both32(AT(o_data) + 2, 0xFFFFFF00); iso_run(what, &h, 1); }
    ICASE("file interleaved") { AT(o_data)[26] = 1; AT(o_data)[27] = 1; iso_run(what, &h, 1); }
    ICASE("multi-extent flag on the last file") { AT(o_data)[25] |= 0x80; iso_run(what, &h, 1); }
    ICASE("multi-extent directory") { AT(o_sub)[25] |= 0x80; iso_run(what, &h, 1); }
    ICASE("directory length huge") { iso_set_both32(AT(o_sub) + 10, 0x7FFFFFFF); iso_run(what, &h, 1); }
    ICASE("directory extent past the device") { iso_set_both32(AT(o_sub) + 2, 0xFFFFFFF0); iso_run(what, &h, 1); }
    ICASE("root length not a block multiple") { iso_set_both32(AT(o_vd + 156 + 10), rootlen - 7); iso_run(what, &h, 1); }
    ICASE("directory loop: SUB/INNER is the root") {
        iso_set_both32(AT(o_inner) + 2, root);
        iso_set_both32(AT(o_inner) + 10, rootlen);
        iso_run(what, &h, 1);
        /* the loop fails cleanly instead of nesting forever */
        const char *path = wpath("hostile.iso");
        host_write(path, h.b, h.n);
        hdisk_t *d = iso_attach(path, 2048, BDEV_DISK_CD);
        char L = letter_of(d);
        vfs_dirent_t *ents;
        if (L) {
            EXPECT(v_count(P(L, "SUB")), 2);
            EXPECT(vfs_list(P(L, "SUB/INNER"), &ents), E_IO);
            EXPECT(vfs_list(P(L, "SUB/INNER/SUB/INNER/SUB"), &ents), E_IO);
        }
        disk_detach(d);
    }
    ICASE("directory loop: SUB/INNER is SUB") {
        iso_set_both32(AT(o_inner) + 2, sublba);
        iso_run(what, &h, 1);
        const char *path = wpath("hostile.iso");
        host_write(path, h.b, h.n);
        hdisk_t *d = iso_attach(path, 2048, BDEV_DISK_CD);
        char L = letter_of(d);
        vfs_dirent_t st;
        if (L) EXPECT(vfs_stat(P(L, "SUB/INNER/FILE.TXT"), &st), E_IO);
        disk_detach(d);
    }
    ICASE("directory loop: SUB is the root, path through the root") {
        iso_set_both32(AT(o_sub) + 2, root);
        iso_run(what, &h, 1);
    }
    if (!joliet) {
        /* Rock Ridge: the SUSP area of DATA.BIN starts after its name */
        const u8 *dr = base.b + o_data;
        u32 nl = dr[32];
        u32 su = 33 + nl + ((nl & 1) ? 0 : 1);
        u32 rl = dr[0];
        int nm = -1;
        for (u32 p = su; p + 4 <= rl; p += dr[p + 2] ? dr[p + 2] : rl)
            if (dr[p] == 'N' && dr[p + 1] == 'M') nm = (int)p;
        CHECK(nm > 0, "%s: no NM entry", tag);
        ICASE("RR: NM length past the record") { if (nm > 0) AT(o_data)[nm + 2] = 0xFF; iso_run(what, &h, 1); }
        ICASE("RR: NM length 0") { if (nm > 0) AT(o_data)[nm + 2] = 0; iso_run(what, &h, 1); }
        ICASE("RR: NM marks the entry as '.'") { if (nm > 0) AT(o_data)[nm + 4] = 2; iso_run(what, &h, 1); }
        ICASE("RR: NM with a slash and control characters") {
            if (nm > 0) { AT(o_data)[nm + 5] = '/'; AT(o_data)[nm + 6] = 1; }
            iso_run(what, &h, 1);
        }
        ICASE("RR: SP skip length 255") { AT((size_t)root * 2048 + 34 + 6)[0] = 255; iso_run(what, &h, 1); }
        ICASE("RR: CE loops to itself") {
            /* replace the NM entry with a CE pointing at the record itself */
            if (nm > 0 && rl - nm >= 28) {
                u8 *e = AT(o_data) + nm;
                memset(e, 0, 28);
                e[0] = 'C'; e[1] = 'E'; e[2] = 28; e[3] = 1;
                iso_set_both32(e + 4, (u32)(o_data / 2048));
                iso_set_both32(e + 12, (u32)(o_data % 2048));
                iso_set_both32(e + 20, rl);
            }
            iso_run(what, &h, 1);
        }
        ICASE("RR: CE past the block") {
            if (nm > 0 && rl - nm >= 28) {
                u8 *e = AT(o_data) + nm;
                memset(e, 0, 28);
                e[0] = 'C'; e[1] = 'E'; e[2] = 28; e[3] = 1;
                iso_set_both32(e + 4, 0xFFFFFFF0);
                iso_set_both32(e + 12, 2040);
                iso_set_both32(e + 20, 2048);
            }
            iso_run(what, &h, 1);
        }
    }
    /* random damage to the directories (bounded number of rounds) */
    u32 dirs[3] = { root, sublba, innerlba };
    for (u32 round = 0; round < 60; round++) {
        h = h_copy(&base);
        u32 x = round * 2654435761u + 7;
        for (int k = 0; k < 12; k++) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            size_t o = (size_t)dirs[k % 3] * 2048 + (x >> 8) % 2048;
            if (o < h.n) h.b[o] = (u8)(x ^ (x >> 24));
        }
        fuzz_fs_image(0, h.b, h.n);
        h_free(&h);
    }
    g_checks++;
#undef AT
    free(im.b);
}
#undef ICASE

static void t_iso_hostile(void) {
    char src[1200];
    snprintf(src, sizeof src, "%s", wpath("hostsrc"));
    int r = sh("rm -rf %s && mkdir -p %s/SUB/INNER %s/MANY && echo inner > %s/SUB/INNER/FILE.TXT"
               " && echo sub > %s/SUB/FILE.TXT && head -c 10000 /dev/urandom > %s/DATA.BIN"
               " && for i in $(seq 10 69); do echo $i > %s/MANY/F$i.TXT; done",
               q(src), q(src), q(src), q(src), q(src), q(src), q(src));
    CHECK(r == 0, "hostile source tree");
    const char *jimg = wpath("hostile_j.iso"), *rimg = wpath("hostile_rr.iso");
    if (!iso_build(jimg, "-V HOSTILE -J", src)) t_iso_hostile_one("joliet", jimg, 1);
    if (!iso_build(rimg, "-V HOSTILE", src)) t_iso_hostile_one("rock ridge", rimg, 0);
}

static void s_iso(void) {
    iso_mktree();
    char img[1300];
    static const struct { const char *name, *opts, *label; int mode; } imgs[] = {
        { "level1.iso", "--norock -iso-level 1 -V LEVEL1", "LEVEL1", ISO_L1 },
        { "level2.iso", "--norock -iso-level 2 -V LEVEL2", "LEVEL2", ISO_L2 },
        { "rr.iso", "-V RockRidge", "RockRidge", ISO_RR },
        { "joliet.iso", "--norock -J -joliet-long -joliet-utf16 -V JolietDisc", "JolietDisc", ISO_JOLIET },
        { "jrr.iso", "-J -joliet-long -joliet-utf16 -V \"Caf\xC3\xA9 Disc 2024\"", "Caf\xC3\xA9 Disc 2024", ISO_JRR },
    };
    for (size_t i = 0; i < ARRAY_SIZE(imgs); i++) {
        snprintf(img, sizeof img, "%s", wpath(imgs[i].name));
        if (iso_build(img, imgs[i].opts, g_iso_src)) continue;
        char tag[64];
        snprintf(tag, sizeof tag, "%s (cd)", imgs[i].name);
        t_iso_tree(tag, img, 2048, imgs[i].mode, imgs[i].label);
        snprintf(tag, sizeof tag, "%s (512)", imgs[i].name);
        t_iso_tree(tag, img, 512, imgs[i].mode, imgs[i].label);
    }
    t_iso_hybrid();
    t_iso_multiextent();
    t_iso_hostile();
}
