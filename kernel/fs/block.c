/* Halcyon OS - block device layer: registry and partition scanning
 *
 * Supported layouts:
 *   - MBR with up to four primary partitions and an extended partition
 *     holding a chain of logical partitions (EBRs)
 *   - GPT (protective MBR type 0xEE, header at LBA 1, backup header at the
 *     last LBA if the primary is damaged)
 *   - no partition table: the whole disk is offered to the file systems
 *     ("superfloppy": floppy images, some USB sticks)
 *   - hybrid ISO images (isohybrid, xorriso --protective-msdos-label or
 *     -isohybrid-gpt-basdat) dd'ed to a USB stick or disk: when the disk has
 *     an ISO 9660 volume descriptor at byte 32768 and a partition entry
 *     covers it (starts within the first 32 KiB; type 0 entries included),
 *     the whole disk is offered to the file systems first (it mounts as
 *     ISO 9660), then the partitions as usual (e.g. an EFI system image).
 *     A partition that holds its own ISO descriptors (xorriso
 *     -partition_offset) mounts by itself instead. Stale descriptors left
 *     below a normal layout (partitions from 1 MiB) are ignored.
 *   - CD/DVD (BDEV_DISK_CD): never scanned for partitions, the whole disc
 *     is offered to the file systems.
 */
#include "core/types.h"
#include "core/lib.h"
#include "core/mm.h"
#include "fs/block.h"
#include "fs/vfs.h"
#include "fs/fat.h"
#include "fs/iso9660.h"

#define MAX_LOGICAL   128
#define MAX_GPT_PARTS 256

static blockdev_t *g_devs;

static inline u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static inline u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static inline u64 rd64(const u8 *p) { return rd32(p) | ((u64)rd32(p + 4) << 32); }

void blockdev_init(void) {
    g_devs = NULL;
}

blockdev_t *blockdev_first(void) { return g_devs; }

int blockdev_count(void) {
    int n = 0;
    for (blockdev_t *b = g_devs; b; b = b->next) n++;
    return n;
}

static void list_add(blockdev_t *bd) {
    bd->next = NULL;
    blockdev_t **pp = &g_devs;
    while (*pp) pp = &(*pp)->next;
    *pp = bd;
}

static void list_remove(blockdev_t *bd) {
    for (blockdev_t **pp = &g_devs; *pp; pp = &(*pp)->next)
        if (*pp == bd) { *pp = bd->next; bd->next = NULL; return; }
}

int blk_read(blockdev_t *bd, u64 lba, u32 count, void *buf) {
    if (!bd || !bd->present || !bd->read || !buf) return E_NODEV;
    if (!count) return 0;
    if (lba >= bd->sector_count || count > bd->sector_count - lba) return E_INVAL;
    return bd->read(bd, lba, count, buf);
}

int blk_write(blockdev_t *bd, u64 lba, u32 count, const void *buf) {
    if (!bd || !bd->present || !bd->write || !buf) return E_NODEV;
    if (bd->readonly) return E_ROFS;
    if (!count) return 0;
    if (lba >= bd->sector_count || count > bd->sector_count - lba) return E_INVAL;
    return bd->write(bd, lba, count, buf);
}

/* ---- partitions ---- */

static int part_read(blockdev_t *bd, u64 lba, u32 count, void *buf) {
    return blk_read(bd->parent, bd->part_start + lba, count, buf);
}

static int part_write(blockdev_t *bd, u64 lba, u32 count, const void *buf) {
    return blk_write(bd->parent, bd->part_start + lba, count, buf);
}

static blockdev_t *add_part(blockdev_t *disk, u64 start, u64 count, int index, u8 type) {
    if (!count || start >= disk->sector_count) return NULL;
    if (count > disk->sector_count - start) {
        kprintf("block: %s: partition %d exceeds the disk, clipped\n", disk->name, index);
        count = disk->sector_count - start;
    }
    /* two volumes on the same sectors would corrupt each other */
    for (blockdev_t *b = g_devs; b; b = b->next) {
        if (b->parent != disk || start >= b->part_start + b->sector_count || b->part_start >= start + count)
            continue;
        kprintf("block: %s: partition %d overlaps partition %d, ignored\n", disk->name, index, b->part_index);
        return NULL;
    }
    blockdev_t *p = kzalloc(sizeof *p);
    if (!p) return NULL;
    char nm[40];
    snprintf(nm, sizeof nm, "%.15sp%d", disk->name, index);
    strlcpy(p->name, nm, sizeof p->name);
    strlcpy(p->model, disk->model, sizeof p->model);
    p->type = BDEV_PART;
    p->sector_size = disk->sector_size;
    p->sector_count = count;
    p->removable = disk->removable;
    p->readonly = disk->readonly;
    p->present = 1;
    p->read = part_read;
    p->write = part_write;
    p->parent = disk;
    p->part_start = start;
    p->part_index = index;
    p->part_type = type;
    p->disk_sig = disk->disk_sig;
    list_add(p);
    kprintf("block: %s: start %lu, %lu sectors, type %02x\n", p->name, start, count, type);
    return p;
}

static int is_extended(u8 t) { return t == 0x05 || t == 0x0F || t == 0x85; }

/* MBR sanity: every entry well-formed, at least one partition */
static int mbr_valid(const u8 *s, u64 nsec, int *has_gpt) {
    int n = 0;
    *has_gpt = 0;
    if (s[510] != 0x55 || s[511] != 0xAA) return 0;
    for (int i = 0; i < 4; i++) {
        const u8 *e = s + 446 + 16 * i;
        u8 status = e[0], type = e[4];
        u32 start = rd32(e + 8), size = rd32(e + 12);
        if (status != 0x00 && status != 0x80) return 0;
        if (!type) continue;
        if (type == 0xEE) { *has_gpt = 1; n++; continue; }
        if (!start || !size || start >= nsec) return 0;
        n++;
    }
    return n > 0;
}

static void scan_ebr_chain(blockdev_t *disk, u64 ext_start, u8 *buf, int *count) {
    u64 cur = ext_start;
    int index = 5;
    for (int i = 0; i < MAX_LOGICAL; i++) {
        if (cur >= disk->sector_count || blk_read(disk, cur, 1, buf)) break;
        if (buf[510] != 0x55 || buf[511] != 0xAA) break;
        const u8 *e0 = buf + 446, *e1 = buf + 462;
        u32 lstart = rd32(e0 + 8), lsize = rd32(e0 + 12);
        u8 nxt_type = e1[4];
        u32 nxt = rd32(e1 + 8);
        if (e0[4] && !is_extended(e0[4]) && lstart && lsize) {
            blockdev_t *p = add_part(disk, cur + lstart, lsize, index, e0[4]);
            if (p) { (*count)++; p->ptab_kind = PTAB_MBR; p->ptab_lba = cur; p->ptab_slot = 0; }
            index++;
        }
        if (!is_extended(nxt_type) || !nxt) break;
        u64 next = ext_start + nxt;
        if (next <= cur) break;          /* chain must move forward: no loops */
        cur = next;
    }
}

static int scan_mbr(blockdev_t *disk, const u8 *mbr, u8 *buf) {
    int count = 0;
    /* copy the table: buf gets reused for EBRs */
    u8 tab[64];
    memcpy(tab, mbr + 446, 64);
    for (int i = 0; i < 4; i++) {
        const u8 *e = tab + 16 * i;
        u8 type = e[4];
        u32 start = rd32(e + 8), size = rd32(e + 12);
        if (!type || type == 0xEE || !start || !size) continue;
        if (is_extended(type)) scan_ebr_chain(disk, start, buf, &count);
        else {
            blockdev_t *p = add_part(disk, start, size, i + 1, type);
            if (p) { count++; p->ptab_kind = PTAB_MBR; p->ptab_lba = 0; p->ptab_slot = (u8)i; }
        }
    }
    return count;
}

/* ---- GPT ---- */

static u32 crc32(const u8 *p, size_t n) {
    u32 c = 0xFFFFFFFF;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & -(c & 1));
    }
    return ~c;
}

static int gpt_header_ok(blockdev_t *disk, u8 *h, u64 lba) {
    if (memcmp(h, "EFI PART", 8)) return 0;
    u32 hsize = rd32(h + 12);
    if (hsize < 92 || hsize > disk->sector_size) return 0;
    u32 crc = rd32(h + 16);
    h[16] = h[17] = h[18] = h[19] = 0;
    u32 calc = crc32(h, hsize);
    h[16] = (u8)crc; h[17] = (u8)(crc >> 8); h[18] = (u8)(crc >> 16); h[19] = (u8)(crc >> 24);
    if (calc != crc) return 0;
    if (rd64(h + 24) != lba) return 0;
    u32 esz = rd32(h + 84), num = rd32(h + 80);
    if (esz < 128 || esz > 4096 || (esz & 7) || !num) return 0;
    if (rd64(h + 72) >= disk->sector_count) return 0;
    return 1;
}

static int scan_gpt(blockdev_t *disk, u8 *buf) {
    u32 ss = disk->sector_size;
    u64 hdr_lba = 1;
    if (blk_read(disk, 1, 1, buf) || !gpt_header_ok(disk, buf, 1)) {
        hdr_lba = disk->sector_count - 1;
        kprintf("block: %s: primary GPT header bad, trying backup\n", disk->name);
        if (blk_read(disk, hdr_lba, 1, buf) || !gpt_header_ok(disk, buf, hdr_lba)) return -1;
    }
    u64 ent_lba = rd64(buf + 72);
    u32 num = rd32(buf + 80), esz = rd32(buf + 84), ecrc = rd32(buf + 88);
    u64 first_usable = rd64(buf + 40), last_usable = rd64(buf + 48);
    /* only the first MAX_GPT_PARTS entries are used: don't read (or overflow
     * on) an absurd entry count */
    u32 used = MIN(num, (u32)MAX_GPT_PARTS);
    u32 total = used * esz;             /* <= 256 * 4096 */
    u32 nsec = (total + ss - 1) / ss;
    if (nsec > disk->sector_count - ent_lba) return -1;
    u8 *ents = kmalloc((size_t)nsec * ss);
    if (!ents) return -1;
    if (blk_read(disk, ent_lba, nsec, ents)) { kfree(ents); return -1; }
    if (used == num && crc32(ents, total) != ecrc)
        kprintf("block: %s: GPT entry array CRC mismatch (using it anyway)\n", disk->name);
    int count = 0;
    for (u32 i = 0; i < used; i++) {
        const u8 *e = ents + (size_t)i * esz;
        int empty = 1;
        for (int k = 0; k < 16; k++) if (e[k]) { empty = 0; break; }
        if (empty) continue;
        u64 s = rd64(e + 32), l = rd64(e + 40);
        if (l < s || s < first_usable || l > last_usable || l >= disk->sector_count) {
            kprintf("block: %s: GPT entry %u out of range, ignored\n", disk->name, i + 1);
            continue;
        }
        blockdev_t *p = add_part(disk, s, l - s + 1, (int)i + 1, 0);
        if (p) { count++; p->ptab_kind = PTAB_GPT; }
    }
    kfree(ents);
    return count;
}

/* ---- hybrid ISO images ---- */

#define ISO_VD_BYTE 32768u

/* an MBR entry (other than the GPT protective one, even of type 0) that
 * starts below the ISO 9660 volume descriptors */
static int mbr_low_entry(const u8 *s, u32 ss) {
    for (int i = 0; i < 4; i++) {
        const u8 *e = s + 446 + 16 * i;
        if (e[4] == 0xEE || !rd32(e + 12)) continue;
        if ((u64)rd32(e + 8) * ss < ISO_VD_BYTE) return 1;
    }
    return 0;
}

static int hybrid_iso(blockdev_t *disk, int low_entry, int nparts) {
    if (!iso9660_detect(disk)) return 0;
    int covered = low_entry || nparts == 0;
    for (blockdev_t *b = g_devs; b; b = b->next) {
        if (b->parent != disk || b->part_start * disk->sector_size > ISO_VD_BYTE) continue;
        if (iso9660_detect(b)) return 0;        /* self-contained ISO partition */
        covered = 1;
    }
    return covered;
}

/* ---- registration ---- */

static void mount_children(blockdev_t *disk) {
    for (blockdev_t *b = g_devs; b; b = b->next)
        if (b->parent == disk) vfs_try_mount(b);
}

int blockdev_register(blockdev_t *bd) {
    if (!bd || !bd->read || bd->sector_size < 512 || bd->sector_size > 65536 || !bd->sector_count)
        return E_INVAL;
    for (blockdev_t *b = g_devs; b; b = b->next) if (b == bd) return E_EXIST;
    bd->parent = NULL;
    bd->part_start = 0;
    bd->part_index = 0;
    bd->part_type = 0;
    bd->present = 1;
    list_add(bd);
    kprintf("block: %s: %s, %lu sectors of %u bytes%s%s\n", bd->name, bd->model, bd->sector_count,
            bd->sector_size, bd->removable ? ", removable" : "", bd->readonly ? ", read-only" : "");

    if (bd->type == BDEV_DISK_CD) {
        vfs_try_mount(bd);
        return 0;
    }

    u8 *s0 = kmalloc(bd->sector_size), *buf = kmalloc(bd->sector_size);
    if (!s0 || !buf) { kfree(s0); kfree(buf); return E_NOMEM; }
    int r = blk_read(bd, 0, 1, s0);
    if (r) {
        kprintf("block: %s: cannot read sector 0 (%d)\n", bd->name, r);
        kfree(s0); kfree(buf);
        return 0;     /* stays registered (e.g. empty card reader) */
    }
    int has_gpt = 0;
    int mbr = mbr_valid(s0, bd->sector_count, &has_gpt);
    int bpb = fat_bpb_sane(s0, bd->sector_size);
    if (s0[510] == 0x55 && s0[511] == 0xAA) bd->disk_sig = rd32(s0 + 440);

    if (mbr && bpb && !has_gpt) {
        /* Both plausible: a FAT boot sector whose boot code happens to look
         * like a partition table, or an MBR whose code looks like a BPB.
         * Decide by checking whether the first partition holds a BPB. */
        int part_bpb = 0;
        for (int i = 0; i < 4; i++) {
            const u8 *e = s0 + 446 + 16 * i;
            u32 start = rd32(e + 8);
            if (!e[4] || is_extended(e[4])) continue;
            if (!blk_read(bd, start, 1, buf) && fat_bpb_sane(buf, bd->sector_size)) part_bpb = 1;
            break;
        }
        if (part_bpb) bpb = 0;
        else mbr = 0;
    }

    int nparts = -1;
    if (mbr && has_gpt) {
        nparts = scan_gpt(bd, buf);
        if (nparts < 0) {
            kprintf("block: %s: no valid GPT, using MBR entries\n", bd->name);
            nparts = scan_mbr(bd, s0, buf);
        }
    } else if (mbr) {
        nparts = scan_mbr(bd, s0, buf);
    }
    int low_entry = mbr ? mbr_low_entry(s0, bd->sector_size) : 0;
    kfree(s0);
    kfree(buf);

    if (nparts >= 0) {
        kprintf("block: %s: %d partition(s)\n", bd->name, nparts);
        if (hybrid_iso(bd, low_entry, nparts)) {
            kprintf("block: %s: hybrid ISO 9660 image, whole disk offered\n", bd->name);
            vfs_try_mount(bd);
        }
        mount_children(bd);
    } else {
        /* no partition table: superfloppy (or unknown) */
        if (bpb) kprintf("block: %s: no partition table, FAT boot sector at LBA 0\n", bd->name);
        vfs_try_mount(bd);
    }
    return 0;
}

void blockdev_unregister(blockdev_t *bd) {
    if (!bd) return;
    int found = 0;
    for (blockdev_t *b = g_devs; b; b = b->next) if (b == bd) found = 1;
    if (!found) { bd->present = 0; return; }
    kprintf("block: %s: removed\n", bd->name);
    /* unmount and drop partitions */
    for (;;) {
        blockdev_t *child = NULL;
        for (blockdev_t *b = g_devs; b; b = b->next) if (b->parent == bd) { child = b; break; }
        if (!child) break;
        vfs_unmount_blockdev(child);
        list_remove(child);
        child->present = 0;
        kfree(child);
    }
    vfs_unmount_blockdev(bd);
    list_remove(bd);
    bd->present = 0;
}

/* ---- partition type ---- */

int blockdev_set_mbr_type(blockdev_t *p, u8 type) {
    if (p->type != BDEV_PART || p->ptab_kind != PTAB_MBR || !p->parent) return 0;
    blockdev_t *disk = p->parent;
    u8 *buf = kmalloc(disk->sector_size);
    if (!buf) return E_NOMEM;
    int r = blk_read(disk, p->ptab_lba, 1, buf);
    u8 *e = buf + 446 + 16 * p->ptab_slot;
    /* make sure the entry still describes this partition */
    if (!r && (buf[510] != 0x55 || buf[511] != 0xAA || p->ptab_lba + rd32(e + 8) != p->part_start)) r = E_IO;
    if (!r && e[4] != type) {
        e[4] = type;
        r = blk_write(disk, p->ptab_lba, 1, buf);
        if (!r) {
            kprintf("block: %s: partition type set to %02x\n", p->name, type);
            p->part_type = type;
        }
    }
    kfree(buf);
    return r;
}
