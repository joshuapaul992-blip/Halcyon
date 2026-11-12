/* Halcyon OS - the installer's engine (see instcore.h and docs/DESIGN-M8-INSTALL.md)
 *
 * Rules it keeps:
 *   - only internal SATA / NVMe disks, never the disk Halcyon runs from;
 *   - "whole disk" erases after the wizard's confirmation; "free space"
 *     only adds a partition (and, on GPT, a 1 MiB BIOS boot partition) in
 *     unpartitioned space: other partitions are never shrunk, moved,
 *     rewritten or written to, and their table entries keep every byte;
 *   - the boot sector code of another boot loader (GRUB, ...) is never
 *     replaced; a standard MBR's code is (Halcyon's stage 2 offers the
 *     active partition, e.g. Windows, in its menu) and saved first;
 *   - a UEFI boot entry is added through the runtime services; no file is
 *     written outside Halcyon's own partition (Windows' EFI system
 *     partition, \EFI\Microsoft and \EFI\BOOT on it are left alone).
 */
#include "instcore.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/thread.h"
#include "core/settings.h"
#include "core/efirt.h"
#include "apps/fileops.h"

#define MIB(ss) ((1u << 20) / (ss))
#define STAGE2_SECTORS 64          /* what boot/mbr.asm loads */
#define MBR_STAGE2_LBA_OFF 432     /* boot/mbr.asm: stage2_lba */
#define COPY_CHUNK (256 * 1024)

static const char *const step_names[IS_COUNT] = {
    "Preparing", "Creating the partition", "Formatting the partition (FAT32)", "Installing the boot code",
    "Copying files", "Verifying the copied files", "Adding Halcyon to the firmware's boot menu", "Done",
};
const char *inst_step_name(int s) { return s >= 0 && s < IS_COUNT ? step_names[s] : ""; }

/* ---------------- disk I/O for parttab ---------------- */
static int io_read(void *ctx, u64 lba, u32 n, void *buf) { return blk_read(ctx, lba, n, buf); }
static int io_write(void *ctx, u64 lba, u32 n, const void *buf) { return blk_write(ctx, lba, n, buf); }
static void io_for(blockdev_t *bd, pt_io_t *io) {
    io->ctx = bd; io->sector_size = bd->sector_size; io->sectors = bd->sector_count;
    io->read = io_read; io->write = io_write;
}

static volume_t *boot_volume(void) {
    for (int i = 0; i < vfs_volume_count(); i++) if (vfs_volume(i)->is_boot) return vfs_volume(i);
    return NULL;
}
static blockdev_t *disk_of(blockdev_t *bd) { return bd && bd->parent ? bd->parent : bd; }

static int is_dir(const char *path) {
    vfs_dirent_t e;
    return !vfs_stat(path, &e) && e.is_dir;
}
static int exists(const char *path) {
    vfs_dirent_t e;
    return !vfs_stat(path, &e);
}

/* ---------------- survey ---------------- */

void inst_disk_title(const inst_disk_t *d, char *out, int n) {
    char sz[32];
    format_size(d->bd->sector_count * d->bd->sector_size, sz, sizeof sz);
    snprintf(out, n, "%s (%s), %s", d->bd->model[0] ? d->bd->model : "Hard disk", d->bd->name, sz);
}

void inst_vol_line(const inst_disk_t *d, const inst_vol_t *v, char *out, int n) {
    char sz[32], what[160];
    format_size(v->count * d->bd->sector_size, sz, sizeof sz);
    if (v->extended) snprintf(what, sizeof what, "Extended partition");
    else if (v->fs[0]) snprintf(what, sizeof what, "%s%s%s%s", v->fs, v->label[0] ? " '" : "", v->label,
                                v->label[0] ? "'" : "");
    else snprintf(what, sizeof what, "%s", v->type);
    char drive[8] = "";
    if (v->letter) snprintf(drive, sizeof drive, " (%c:)", v->letter);
    snprintf(out, n, "Partition %d: %s%s, %s%s%s", v->index, what, drive, sz,
             v->windows ? " - Windows" : v->winboot ? " - Windows Boot Manager" : "",
             v->active ? ", active" : "");
}

static void describe_vols(inst_disk_t *d) {
    pt_table_t *t = d->pt;
    for (int i = 0; i < t->nparts && d->nvols < INST_MAX_VOLS; i++) {
        pt_part_t *p = &t->parts[i];
        inst_vol_t *v = &d->vols[d->nvols++];
        memset(v, 0, sizeof *v);
        v->start = p->start; v->count = p->count; v->index = p->index;
        v->type = pt_type_name(t->kind, p);
        v->active = p->active; v->logical = p->logical; v->extended = p->extended;
        /* mounted? (the block layer's partition at the same place) */
        for (blockdev_t *b = blockdev_first(); b; b = b->next) {
            if (b->parent != d->bd || b->part_start != p->start) continue;
            for (int k = 0; k < vfs_volume_count(); k++) {
                volume_t *vol = vfs_volume(k);
                if (vol->bd != b) continue;
                v->letter = vol->letter;
                strlcpy(v->fs, vol->fs_name, sizeof v->fs);
                strlcpy(v->label, vol->label, sizeof v->label);
                if (vol->open_files) d->busy = 1;
                char path[64];
                snprintf(path, sizeof path, "%c:/Windows/System32", vol->letter);
                v->windows = (u8)is_dir(path);
                snprintf(path, sizeof path, "%c:/EFI/Microsoft/Boot/bootmgfw.efi", vol->letter);
                v->winboot = (u8)exists(path);
            }
        }
        if (v->windows || v->winboot) d->windows = 1;
    }
}

static void free_space(inst_disk_t *d) {
    pt_table_t *t = d->pt;
    char sz[32];
    if (t->kind == PT_NONE) { strlcpy(d->free_why, "The disk is empty (it has no partitions), so Halcyon uses the whole disk.", sizeof d->free_why); return; }
    if (t->kind == PT_OTHER) { snprintf(d->free_why, sizeof d->free_why, "%s", t->problem); return; }
    if (t->kind == PT_MBR && !t->free_slots) {
        strlcpy(d->free_why, "All four primary partition entries of the disk are in use.", sizeof d->free_why);
        return;
    }
    if (t->kind == PT_GPT && !t->free_entries) {
        strlcpy(d->free_why, "The disk's partition table has no free entry.", sizeof d->free_why);
        return;
    }
    pt_region_t r[4];
    int n = pt_free_regions(t, MIB(d->bd->sector_size), r, 4);
    if (n) { d->free_start = r[0].start; d->free_count = r[0].count; }
    u64 have = n ? r[0].count * d->bd->sector_size : 0;
    if (have < INST_MIN_BYTES) {
        format_size(have, sz, sizeof sz);
        char need[32];
        format_size(INST_MIN_BYTES, need, sizeof need);
        if (!have) snprintf(d->free_why, sizeof d->free_why, "There is no unpartitioned space on the disk%s.",
                            t->kind == PT_MBR ? " (below 2 TB, where an MBR disk can have partitions)" : "");
        else snprintf(d->free_why, sizeof d->free_why, "The largest unpartitioned space on the disk is %s; Halcyon needs at least %s.", sz, need);
        return;
    }
    d->free_ok = 1;
}

int inst_survey(inst_disk_t *out, int max) {
    mutex_lock(&storage_lock);
    volume_t *bv = boot_volume();
    blockdev_t *boot_disk = bv ? disk_of(bv->bd) : NULL;
    int n = 0;
    for (blockdev_t *b = blockdev_first(); b && n < max; b = b->next) {
        if (b->parent || !b->present || (b->type != BDEV_DISK_SATA && b->type != BDEV_DISK_NVME)) continue;
        inst_disk_t *d = &out[n++];
        memset(d, 0, sizeof *d);
        d->bd = b;
        d->pt = kzalloc(sizeof *d->pt);
        if (!d->pt) { strlcpy(d->why, "Out of memory.", sizeof d->why); continue; }
        pt_io_t io;
        io_for(b, &io);
        int r = pt_read(&io, d->pt);
        if (r) d->pt->kind = PT_OTHER, snprintf(d->pt->problem, sizeof d->pt->problem, "The disk cannot be read (%s).", strerror(r));
        describe_vols(d);
        free_space(d);
        d->usable = 1;
        if (b == boot_disk) { d->usable = 0; strlcpy(d->why, "Halcyon is running from this disk.", sizeof d->why); }
        else if (b->removable) { d->usable = 0; strlcpy(d->why, "The disk is removable.", sizeof d->why); }
        else if (b->readonly) { d->usable = 0; strlcpy(d->why, "The disk is read-only.", sizeof d->why); }
        else if (b->sector_size != 512 && b->sector_size != 4096) { d->usable = 0; strlcpy(d->why, "The disk's sector size is not supported.", sizeof d->why); }
        else if (b->sector_count * b->sector_size < INST_MIN_BYTES + (4u << 20)) { d->usable = 0; strlcpy(d->why, "The disk is too small for Halcyon (it needs 132 MB).", sizeof d->why); }
        if (r && d->usable) d->free_ok = 0;
    }
    mutex_unlock(&storage_lock);
    return n;
}

void inst_survey_free(inst_disk_t *d, int n) {
    for (int i = 0; i < n; i++) { kfree(d[i].pt); d[i].pt = NULL; }
}

/* ---------------- the plan ---------------- */

static char source_letter(void) { volume_t *v = boot_volume(); return v ? v->letter : 0; }

static int skip_entry(const char *rel, const vfs_dirent_t *e, int depth) {
    if (e->attr & (VFS_ATTR_VOLUME | VFS_ATTR_LINK)) return 1;
    if (depth == 0 && e->is_dir && ((e->attr & (VFS_ATTR_HIDDEN | VFS_ATTR_SYSTEM)) == (VFS_ATTR_HIDDEN | VFS_ATTR_SYSTEM) ||
                                    !strcasecmp(e->name, "System Volume Information") || !strcasecmp(e->name, "$RECYCLE.BIN") ||
                                    !strcasecmp(e->name, "RECYCLER") || !strcasecmp(e->name, "RECYCLED")))
        return 1;
    return !strcasecmp(rel, "HALCYON/BOOT.LOG") || !strcasecmp(rel, "HALCYON/BOOTPREV.LOG");
}

enum { W_COUNT, W_COPY, W_VERIFY };
typedef struct walk {
    inst_job_t *j;
    inst_plan_t *p;
    char src, dst;
    int mode;
    u8 *buf, *buf2;
    int err;
    char errpath[VFS_MAX_PATH + 1];
} walk_t;

static int copy_file(walk_t *w, const char *s, const char *d, u64 size) {
    vfs_file_t *in = NULL, *out = NULL;
    int r = vfs_open(s, VFS_O_READ, &in);
    if (!r) r = vfs_open(d, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC, &out);
    u64 done = 0;
    while (!r) {
        int n = vfs_read(in, w->buf, COPY_CHUNK);
        if (n < 0) { r = n; break; }
        if (!n) break;
        int m = vfs_write(out, w->buf, (u32)n);
        if (m != n) { r = m < 0 ? m : E_NOSPC; break; }
        done += (u64)n;
        w->j->done_bytes += (u64)n;
    }
    if (!r && done != size) r = E_IO;
    if (out) { int c = vfs_close(out); if (!r) r = c; }
    if (in) vfs_close(in);
    return r;
}

static int compare_file(walk_t *w, const char *s, const char *d, u64 size) {
    vfs_file_t *a = NULL, *b = NULL;
    int r = vfs_open(s, VFS_O_READ, &a);
    if (!r) r = vfs_open(d, VFS_O_READ, &b);
    if (!r && vfs_size(b) != size) r = E_IO;
    while (!r) {
        int n = vfs_read(a, w->buf, COPY_CHUNK);
        if (n < 0) { r = n; break; }
        if (!n) break;
        int m = vfs_read(b, w->buf2, (u32)n);
        if (m != n || memcmp(w->buf, w->buf2, (size_t)n)) { r = E_IO; break; }
        w->j->done_bytes += (u64)n;
    }
    if (a) vfs_close(a);
    if (b) vfs_close(b);
    return r;
}

static int walk_dir(walk_t *w, const char *rel, int depth) {
    if (depth > 24) return E_NAMETOOLONG;
    char *path = kmalloc(3 * (VFS_MAX_PATH + 1));
    if (!path) return E_NOMEM;
    char *crel = path + VFS_MAX_PATH + 1, *other = crel + VFS_MAX_PATH + 1;
    snprintf(path, VFS_MAX_PATH + 1, "%c:/%s", w->src, rel);
    vfs_dirent_t *ents = NULL;
    int n = vfs_list(path, &ents), r = n < 0 ? n : 0;
    for (int i = 0; !r && i < n; i++) {
        vfs_dirent_t *e = &ents[i];
        snprintf(crel, VFS_MAX_PATH + 1, "%s%s%s", rel, rel[0] ? "/" : "", e->name);
        if (skip_entry(crel, e, depth)) continue;
        snprintf(path, VFS_MAX_PATH + 1, "%c:/%s", w->src, crel);
        snprintf(other, VFS_MAX_PATH + 1, "%c:/%s", w->dst, crel);
        if (w->j) strlcpy(w->j->current, crel, sizeof w->j->current);
        if (e->is_dir) {
            if (w->mode == W_COPY) {
                r = vfs_mkdir(other);
                if (r == E_EXIST) r = 0;
                if (!r && w->j) w->j->dirs++;
            } else if (w->mode == W_VERIFY && !is_dir(other)) r = E_NOENT;
            if (!r) r = walk_dir(w, crel, depth + 1);
            if (r && !w->errpath[0]) strlcpy(w->errpath, crel, sizeof w->errpath);
            continue;
        }
        if (w->mode == W_COUNT) { w->p->copy_bytes += e->size; w->p->copy_files++; }
        else if (w->mode == W_COPY) { r = copy_file(w, path, other, e->size); if (!r) w->j->files++; }
        else r = compare_file(w, path, other, e->size);
        if (r && !w->errpath[0]) strlcpy(w->errpath, crel, sizeof w->errpath);
        thread_yield();
    }
    kfree(ents);
    kfree(path);
    return r;
}

/* the start of the disk is free for stage 2 (zeros, or an older Halcyon's)
 * and its boot code is one Halcyon may replace */
static int mbr_code_kind(const u8 *code) {
    int zero = 1;
    for (int i = 0; i < 440; i++) if (code[i]) zero = 0;
    if (zero) return 0;                                       /* none */
    for (int i = 0; i + 7 <= 440; i++) if (!memcmp(code + i, "Halcyon", 7)) return 1;   /* ours */
    static const char *const loaders[] = { "GRUB", "LILO", "SYSLINUX", "ISOLINUX", "rEFInd", "Clover", "BOOTMGR " };
    for (u32 k = 0; k < ARRAY_SIZE(loaders); k++) {
        size_t l = strlen(loaders[k]);
        for (size_t i = 0; i + l <= 440; i++) if (!memcmp(code + i, loaders[k], l)) return 3;   /* another loader */
    }
    return 2;                                                 /* a standard MBR (it boots the active partition) */
}

static int gap_free(blockdev_t *bd, u64 first_part) {
    if (bd->sector_size != 512 || first_part < 1 + STAGE2_SECTORS) return 0;
    u8 *b = kmalloc(STAGE2_SECTORS * 512);
    if (!b) return 0;
    int ok = !blk_read(bd, 1, STAGE2_SECTORS, b);
    int zero = 1, ours = 0;
    for (u32 i = 0; ok && i < STAGE2_SECTORS * 512; i++) if (b[i]) { zero = 0; break; }
    for (u32 i = 0; ok && !zero && i + 14 <= STAGE2_SECTORS * 512; i++) if (!memcmp(b + i, "Halcyon loader", 14)) { ours = 1; break; }
    kfree(b);
    return ok && (zero || ours);
}

int inst_plan(const inst_disk_t *d, int mode, inst_plan_t *p, char *why, int whysz) {
    memset(p, 0, sizeof *p);
    why[0] = 0;
    blockdev_t *bd = d->bd;
    u32 ss = bd->sector_size;
    u64 A = MIB(ss);
    int uefi = g_bootinfo.boot_type == BOOT_TYPE_UEFI;
    if (!d->usable) { snprintf(why, whysz, "%s", d->why); return E_INVAL; }
    p->disk = bd;
    p->mode = mode;
    p->uefi_entry = uefi && efi_rt_available();
    char src = source_letter();
    if (!src) { snprintf(why, whysz, "The drive Halcyon started from is not available."); return E_NODEV; }
    char path[64];
    snprintf(path, sizeof path, "%c:/HALCYON/KERNEL.BIN", src);
    if (!exists(path)) { snprintf(why, whysz, "The installation files (\\HALCYON\\KERNEL.BIN) are missing on %c:.", src); return E_NOENT; }
    walk_t w = { .p = p, .src = src, .mode = W_COUNT };
    int r = walk_dir(&w, "", 0);
    if (r) { snprintf(why, whysz, "The files to install cannot be read (%s: %s).", w.errpath, strerror(r)); return r; }

    if (mode == INST_WHOLE) {
        if (d->busy) { snprintf(why, whysz, "Files on the disk are open: close them first."); return E_BUSY; }
        p->gpt = uefi || ss != 512 || bd->sector_count > 0xFFFFFFFFull;
        if (!p->gpt) {
            p->start = A;
            p->count = ALIGN_DOWN(MIN(bd->sector_count, 0xFFFFFFFFull), A) - A;
            p->bios = 1;
            p->stage2_lba = 1;
        } else {
            u64 entsec = 128 * 128 / ss;
            u64 end = ALIGN_DOWN(bd->sector_count - 1 - entsec, A);      /* last usable + 1, aligned */
            if (ss == 512) {
                /* a BIOS boot partition for stage 2 (LBA 1 holds the GPT header) */
                p->bb_start = A; p->bb_count = A;
                p->bios = 1;
                p->stage2_lba = A;
                p->start = 2 * A;
            } else {
                p->start = A;
                strlcpy(p->bios_note, "PC BIOSes cannot start disks with 4 KB sectors: Halcyon starts from this disk with UEFI only.", sizeof p->bios_note);
            }
            p->count = end - p->start;
        }
    } else {
        pt_table_t *t = d->pt;
        if (!d->free_ok) { snprintf(why, whysz, "%s", d->free_why); return E_NOSPC; }
        p->gpt = t->kind == PT_GPT;
        p->start = d->free_start;
        p->count = d->free_count;
        if (p->gpt) {
            int pmbr = mbr_code_kind(t->mbr);
            if (ss != 512) strlcpy(p->bios_note, "PC BIOSes cannot start disks with 4 KB sectors.", sizeof p->bios_note);
            else if (t->free_entries < 2) strlcpy(p->bios_note, "The partition table has no entry left for a BIOS boot partition.", sizeof p->bios_note);
            /* (a standard MBR's code in a protective MBR is never run: Windows
             * starts GPT disks with UEFI only; another loader's, e.g. GRUB's
             * for BIOS + GPT, is) */
            else if (pmbr == 3) strlcpy(p->bios_note, "Another boot loader's code is at the start of the disk; Halcyon leaves it there.", sizeof p->bios_note);
            else if (p->count < A + INST_MIN_BYTES / ss) strlcpy(p->bios_note, "There is no room for a BIOS boot partition next to Halcyon's.", sizeof p->bios_note);
            else {
                p->bios = 1;
                p->bb_start = p->start; p->bb_count = A;
                p->stage2_lba = p->start;
                p->start += A; p->count -= A;
            }
        } else {
            u64 first = ~0ull;
            for (int i = 0; i < t->nparts; i++) if (!t->parts[i].logical) first = MIN(first, t->parts[i].start);
            int kind = mbr_code_kind(t->mbr);
            if (ss != 512) strlcpy(p->bios_note, "PC BIOSes cannot start disks with 4 KB sectors.", sizeof p->bios_note);
            else if (kind == 3) strlcpy(p->bios_note, "Another boot loader (such as GRUB) is installed on the disk; Halcyon leaves it alone.", sizeof p->bios_note);
            else if (!gap_free(bd, first)) strlcpy(p->bios_note, "The space before the first partition is in use or too small for Halcyon's boot code.", sizeof p->bios_note);
            else { p->bios = 1; p->stage2_lba = 1; }
        }
        if (!p->bios && !uefi) {
            snprintf(why, whysz, "This computer started Halcyon with its BIOS, but Halcyon could not start from this disk with the BIOS: %s",
                     p->bios_note);
            return E_NOTSUP;
        }
    }
    /* FAT32 holds at most 2^32 - 1 sectors */
    if (p->count > ALIGN_DOWN(0xFFFFFFFFull, A)) p->count = ALIGN_DOWN(0xFFFFFFFFull, A);
    if (p->count * ss < INST_MIN_BYTES) { snprintf(why, whysz, "There is not enough room for Halcyon on the disk."); return E_NOSPC; }
    if (p->copy_bytes + p->copy_bytes / 8 + (16u << 20) > p->count * ss) {
        char a[32], b[32];
        format_size(p->copy_bytes, a, sizeof a);
        format_size(p->count * ss, b, sizeof b);
        snprintf(why, whysz, "The files to install (%s) do not fit in the %s partition.", a, b);
        return E_NOSPC;
    }
    if (p->bios) {
        snprintf(path, sizeof path, "%c:/HALCYON/BOOT/STAGE2.BIN", src);
        char p2[64];
        snprintf(p2, sizeof p2, "%c:/HALCYON/BOOT/MBR.BIN", src);
        if (!exists(path) || !exists(p2)) {
            if (!uefi) { snprintf(why, whysz, "The BIOS boot files (\\HALCYON\\BOOT\\MBR.BIN, STAGE2.BIN) are missing on %c:.", src); return E_NOENT; }
            p->bios = 0;
            p->bb_count = 0;
            strlcpy(p->bios_note, "The BIOS boot files are missing on the drive Halcyon started from.", sizeof p->bios_note);
        }
    }
    return 0;
}

/* ---------------- the installation ---------------- */

static void fail(inst_job_t *j, int r, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char msg[200];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(j->error, sizeof j->error, "%s (%s)", msg, strerror(r));
    j->result = r;
    kprintf("install: failed: %s\n", j->error);
}

static void step(inst_job_t *j, int s) {
    j->step = s;
    strlcpy(j->status, inst_step_name(s), sizeof j->status);
    kprintf("install: %s\n", inst_step_name(s));
}

static void rnd_guid(u8 g[16]) {
    u8 r[16];
    random_bytes(r, sizeof r);
    pt_guid_v4(g, r);
}

static int zero_range(blockdev_t *bd, u64 lba, u64 count) {
    u32 per = 65536 / bd->sector_size;
    u8 *z = kzalloc(65536);
    if (!z) return E_NOMEM;
    int r = 0;
    while (!r && count) {
        u32 n = (u32)MIN(count, (u64)per);
        r = blk_write(bd, lba, n, z);
        lba += n; count -= n;
    }
    kfree(z);
    return r;
}

static int read_file(const char *path, u8 *buf, u32 max, u32 *got) {
    vfs_file_t *f;
    int r = vfs_open(path, VFS_O_READ, &f);
    if (r) return r;
    int n = vfs_read(f, buf, max);
    vfs_close(f);
    if (n < 0) return n;
    *got = (u32)n;
    return 0;
}

/* a disk signature no other disk here has */
static u32 new_disk_sig(void) {
    for (;;) {
        u32 s = random_u32();
        int used = !s;
        for (blockdev_t *b = blockdev_first(); b; b = b->next) if (!b->parent && b->disk_sig == s) used = 1;
        if (!used) return s;
    }
}

static int do_partition(inst_job_t *j, u8 *saved_mbr, u8 *part_guid, int *index) {
    inst_plan_t *p = &j->plan;
    blockdev_t *bd = p->disk;
    pt_io_t io;
    io_for(bd, &io);
    int r;
    u8 tguid[16];
    memcpy(tguid, PT_GUID_ESP, 16);
    rnd_guid(part_guid);
    pt_table_t *t = kzalloc(sizeof *t);
    if (!t) return E_NOMEM;
    r = pt_read(&io, t);
    if (r) { fail(j, r, "The disk cannot be read"); kfree(t); return r; }
    memcpy(saved_mbr, t->mbr, 512);
    if (p->mode == INST_WHOLE) {
        /* nothing on the disk may be in use; its volumes go away first (their
         * caches are written back before the disk is overwritten) */
        for (int i = 0; i < vfs_volume_count(); i++) {
            volume_t *v = vfs_volume(i);
            if (v->bd && disk_of(v->bd) == bd && (v->is_boot || v->open_files)) {
                kfree(t);
                fail(j, E_BUSY, "Files on drive %c: are open", v->letter);
                return E_BUSY;
            }
        }
        for (blockdev_t *b = blockdev_first(); b; b = b->next) if (b->parent == bd) vfs_unmount_blockdev(b);
        vfs_unmount_blockdev(bd);
        /* the old tables, boot code and file system signatures at both ends */
        j->wrote = 1;
        u64 edge = MIN(bd->sector_count / 2, (u64)MIB(bd->sector_size));
        r = zero_range(bd, 0, edge);
        if (!r) r = zero_range(bd, bd->sector_count - edge, edge);
        /* (and the start of the new partitions: no old file system mounts there) */
        if (!r) r = zero_range(bd, p->start, MIN(p->count, (u64)MIB(bd->sector_size)));
        if (!r && p->bb_count) r = zero_range(bd, p->bb_start, p->bb_count);
        if (r) { fail(j, r, "Writing to the disk failed"); kfree(t); return r; }
        if (p->gpt) {
            pt_new_t np[2];
            int n = 0;
            memset(np, 0, sizeof np);
            np[n].start = p->start; np[n].count = p->count; np[n].name = "Halcyon";
            memcpy(np[n].type_guid, tguid, 16); memcpy(np[n].guid, part_guid, 16); n++;
            if (p->bb_count) {
                np[n].start = p->bb_start; np[n].count = p->bb_count; np[n].name = "BIOS boot partition";
                memcpy(np[n].type_guid, PT_GUID_BIOS_BOOT, 16); rnd_guid(np[n].guid); n++;
            }
            u8 dg[16];
            rnd_guid(dg);
            r = pt_write_gpt(&io, np, n, dg, NULL);
            *index = 1;
        } else {
            pt_new_t np = { .start = p->start, .count = p->count, .mbr_type = 0x0C, .active = 1 };
            r = pt_write_mbr(&io, &np, 1, new_disk_sig(), NULL);
            *index = 1;
        }
        if (r) { fail(j, r, "Writing the partition table failed"); kfree(t); return r; }
        r = blockdev_rescan(bd);
        if (r) { fail(j, r, "The new partition table could not be read back"); kfree(t); return r; }
    } else {
        int idx[2] = { 0, 0 };
        if (p->gpt) {
            pt_new_t np[2];
            int n = 0;
            memset(np, 0, sizeof np);
            np[n].start = p->start; np[n].count = p->count; np[n].name = "Halcyon";
            memcpy(np[n].type_guid, tguid, 16); memcpy(np[n].guid, part_guid, 16); n++;
            if (p->bb_count) {
                np[n].start = p->bb_start; np[n].count = p->bb_count; np[n].name = "BIOS boot partition";
                memcpy(np[n].type_guid, PT_GUID_BIOS_BOOT, 16); rnd_guid(np[n].guid); n++;
            }
            r = pt_add_gpt(&io, t, np, n, idx);
        } else {
            int active = 0;
            for (int i = 0; i < t->nparts; i++) if (t->parts[i].active) active = 1;
            pt_new_t np = { .start = p->start, .count = p->count, .mbr_type = 0x0C, .active = (u8)!active };
            r = pt_add_mbr(&io, t, &np, idx);
        }
        if (r) { fail(j, r, r == E_BUSY ? "The disk's partition table changed after Setup looked at it" : "Adding the partition failed"); kfree(t); return r; }
        *index = idx[0];
        j->wrote = 1;
        /* the new partitions are ours (the table accepted them: they overlap
         * nothing): no stale file system signatures in them */
        r = zero_range(bd, p->start, MIN(p->count, (u64)MIB(bd->sector_size)));
        if (!r && p->bb_count) r = zero_range(bd, p->bb_start, p->bb_count);
        if (r) { fail(j, r, "Writing to the disk failed"); kfree(t); return r; }
        if (!blockdev_add_part(bd, p->start, p->count, idx[0], p->gpt ? 0 : 0x0C, p->gpt ? PTAB_GPT : PTAB_MBR,
                               p->gpt ? -1 : idx[0] - 1, p->gpt ? tguid : NULL, p->gpt ? part_guid : NULL)) {
            fail(j, E_IO, "The new partition could not be registered");
            kfree(t);
            return E_IO;
        }
    }
    /* read the table back: Halcyon's partition must be there as planned */
    r = pt_read(&io, t);
    int found = 0;
    for (int i = 0; !r && i < t->nparts; i++)
        if (t->parts[i].start == p->start && t->parts[i].count == p->count && t->parts[i].index == *index &&
            (p->gpt ? !memcmp(t->parts[i].type_guid, PT_GUID_ESP, 16) : t->parts[i].mbr_type == 0x0C)) found = 1;
    kfree(t);
    if (!found) { fail(j, r ? r : E_IO, "The new partition table does not read back as written"); return E_IO; }
    for (blockdev_t *b = blockdev_first(); b; b = b->next)
        if (b->parent == bd && b->part_start == p->start) j->part = b;
    if (!j->part) { fail(j, E_NODEV, "The new partition is not there"); return E_NODEV; }
    return 0;
}

static int do_bootcode(inst_job_t *j, char src) {
    inst_plan_t *p = &j->plan;
    blockdev_t *bd = p->disk;
    u8 *s2 = kzalloc(STAGE2_SECTORS * 512), *mbr = kzalloc(512), *chk = kmalloc(STAGE2_SECTORS * 512);
    int r = s2 && mbr && chk ? 0 : E_NOMEM;
    u32 n2 = 0, nm = 0;
    char path[64];
    snprintf(path, sizeof path, "%c:/HALCYON/BOOT/STAGE2.BIN", src);
    if (!r) r = read_file(path, s2, STAGE2_SECTORS * 512, &n2);
    snprintf(path, sizeof path, "%c:/HALCYON/BOOT/MBR.BIN", src);
    if (!r) r = read_file(path, mbr, 512, &nm);
    if (!r && (nm != 440 || n2 < 512 || mbr[MBR_STAGE2_LBA_OFF] != 1)) r = E_INVAL;
    if (r) { fail(j, r, "The BIOS boot files cannot be read"); goto out; }
    mbr[MBR_STAGE2_LBA_OFF] = (u8)p->stage2_lba;
    mbr[MBR_STAGE2_LBA_OFF + 1] = (u8)(p->stage2_lba >> 8);
    mbr[MBR_STAGE2_LBA_OFF + 2] = (u8)(p->stage2_lba >> 16);
    mbr[MBR_STAGE2_LBA_OFF + 3] = (u8)(p->stage2_lba >> 24);
    r = blk_write(bd, p->stage2_lba, STAGE2_SECTORS, s2);
    if (!r) r = blk_read(bd, p->stage2_lba, STAGE2_SECTORS, chk);
    if (!r && memcmp(chk, s2, STAGE2_SECTORS * 512)) r = E_IO;
    if (r) { fail(j, r, "Writing the boot loader (stage 2) failed"); goto out; }
    pt_io_t io;
    io_for(bd, &io);
    r = pt_write_bootcode(&io, mbr);
    if (!r) r = blk_read(bd, 0, 1, chk);
    if (!r && memcmp(chk, mbr, 440)) r = E_IO;
    if (r) fail(j, r, "Writing the boot code to the disk's first sector failed");
out:
    kfree(s2); kfree(mbr); kfree(chk);
    return r;
}

void inst_run(inst_job_t *j) {
    inst_plan_t *p = &j->plan;
    j->result = 0;
    j->error[0] = j->warning[0] = 0;
    j->boot_entry = -1000;
    u8 saved_mbr[512], part_guid[16];
    int index = 0, r;
    walk_t w;
    memset(&w, 0, sizeof w);
    char src = source_letter();

    step(j, IS_PREPARE);
    kprintf("install: %s, %s, partition at %lu, %lu sectors%s%s\n", p->disk->name,
            p->mode == INST_WHOLE ? "whole disk" : "free space", p->start, p->count, p->gpt ? ", GPT" : ", MBR",
            p->bios ? ", BIOS boot code" : "");
    settings_save();                             /* the settings as they are now */
    w.buf = kmalloc(COPY_CHUNK);
    w.buf2 = kmalloc(COPY_CHUNK);
    if (!w.buf || !w.buf2 || !src) { fail(j, E_NOMEM, "Setup could not start"); goto out; }

    step(j, IS_PARTITION);
    mutex_lock(&storage_lock);
    r = do_partition(j, saved_mbr, part_guid, &index);
    mutex_unlock(&storage_lock);
    if (r) goto out;

    step(j, IS_FORMAT);
    r = vfs_format(j->part, "FAT32", "HALCYON", 0);
    for (int i = 0; !r && i < vfs_volume_count(); i++) if (vfs_volume(i)->bd == j->part) j->letter = vfs_volume(i)->letter;
    if (r || !j->letter) { fail(j, r ? r : E_NODEV, "Formatting the new partition failed"); goto out; }
    if (strcmp(vfs_volume_by_letter(j->letter)->fs_name, "FAT32")) { fail(j, E_INVAL, "The new partition is not FAT32"); goto out; }
    kprintf("install: Halcyon's partition is %c: (%s)\n", j->letter, j->part->name);

    if (p->bios) {
        step(j, IS_BOOTCODE);
        mutex_lock(&storage_lock);
        r = do_bootcode(j, src);
        mutex_unlock(&storage_lock);
        if (r) goto out;
    }

    step(j, IS_COPY);
    j->total_bytes = p->copy_bytes;
    j->done_bytes = 0;
    w.j = j; w.p = p; w.src = src; w.dst = j->letter; w.mode = W_COPY;
    r = walk_dir(&w, "", 0);
    if (r) { fail(j, r, "Copying %s failed", w.errpath); goto out; }
    if (p->mode == INST_FREE && p->bios && (!p->gpt || mbr_code_kind(saved_mbr) == 2)) {
        /* the disk's original first sector, for going back to the old boot code */
        char path[64];
        snprintf(path, sizeof path, "%c:/HALCYON/BOOT/MBRSAVE.BIN", j->letter);
        vfs_write_file(path, saved_mbr, 512);
    }
    r = vfs_sync_all();
    if (r) { fail(j, r, "Writing the files to the disk failed"); goto out; }

    step(j, IS_VERIFY);
    /* mounted again, so every file is read from the disk, not from a cache */
    r = vfs_remount(j->letter);
    if (r) { fail(j, r, "The new partition could not be opened again"); goto out; }
    j->done_bytes = 0;
    w.mode = W_VERIFY;
    w.errpath[0] = 0;
    r = walk_dir(&w, "", 0);
    if (r) { fail(j, r, "%s on the hard disk differs from the original", w.errpath); goto out; }
    kprintf("install: %u files, %u folders, %lu bytes copied and verified\n", j->files, j->dirs, j->done_bytes);

    if (p->uefi_entry) {
        step(j, IS_FIRMWARE);
        efi_hd_t hd = { .part_num = (u32)index, .start = p->start, .count = p->count, .gpt = p->gpt };
        if (p->gpt) memcpy(hd.sig, part_guid, 16);
        else { u32 sig = p->disk->disk_sig; memcpy(hd.sig, &sig, 4); }
        j->boot_entry = efi_add_boot_entry("Halcyon", &hd, "\\EFI\\BOOT\\BOOTX64.EFI");
        if (j->boot_entry < 0)
            snprintf(j->warning, sizeof j->warning,
                     "Halcyon could not add itself to the firmware's boot menu (%s). To start it, choose this hard disk "
                     "in your computer's boot menu, or add \\EFI\\BOOT\\BOOTX64.EFI on the Halcyon partition in the "
                     "firmware setup.", strerror(j->boot_entry));
    } else if (g_bootinfo.boot_type == BOOT_TYPE_UEFI) {
        snprintf(j->warning, sizeof j->warning, "The firmware's boot menu could not be changed: choose this hard disk "
                 "in your computer's boot menu to start Halcyon.");
    }
    if (!p->bios && p->bios_note[0] && !j->warning[0])
        snprintf(j->warning, sizeof j->warning, "%s", p->bios_note);
    step(j, IS_DONE);
out:
    kfree(w.buf);
    kfree(w.buf2);
    j->done = 1;
}
