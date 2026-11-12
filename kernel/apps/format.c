/* Halcyon OS - Format dialog (Explorer: right-click a drive, Format...) */
#include "gui/wm.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/unicode.h"
#include "core/kernel.h"
#include "fs/vfs.h"
#include "fileops.h"

enum { ID_FS = 300, ID_LABEL = 301, ID_START = 302, ID_CLOSE = 303, ID_CLUSTER = 304 };

enum { FS_FAT32, FS_FAT, FS_EXFAT, FS_NTFS, FS_COUNT };
static const char *const fs_items[FS_COUNT] = { "FAT32", "FAT (FAT12 / FAT16)", "exFAT", "NTFS" };
static const char *const fs_names[FS_COUNT] = { "FAT32", "FAT", "exFAT", "NTFS" };
/* label length (characters) each file system keeps */
static const int fs_label_max[FS_COUNT] = { 11, 11, 11, 32 };
/* the largest cluster each file system takes (bytes) */
static const u32 fs_cluster_max[FS_COUNT] = { 65536, 65536, 32u << 20, 65536 };

#define MAX_CLUSTERS 18

typedef struct fmt {
    volatile int done;
    blockdev_t *bd;
    char letter;
    int fs;
    u32 clusters[MAX_CLUSTERS];         /* the cluster list's entries (0 = default) */
    int nclusters;
} fmt_t;

/* the file system Windows would suggest: NTFS on fixed disks, FAT32 on
 * removable ones up to 32 GiB, exFAT on larger removable ones (and FAT on
 * floppy-sized ones) */
static int default_fs(blockdev_t *bd) {
    u64 bytes = bd->sector_count * bd->sector_size;
    blockdev_t *d = bd->parent ? bd->parent : bd;
    int removable = d->removable || d->type == BDEV_DISK_USB;
    if (bytes < (32ull << 20)) return FS_FAT;
    if (!removable && bytes >= (64ull << 20)) return FS_NTFS;
    return bytes > (32ull << 30) ? FS_EXFAT : FS_FAT32;
}

static void cluster_name(u32 c, char *out, size_t n) {
    if (!c) strlcpy(out, "Default allocation size", n);
    else if (c < 1024) snprintf(out, n, "%u bytes", c);
    else snprintf(out, n, "%u kilobytes", c / 1024);
}

static void fill_clusters(window_t *w, fmt_t *f) {
    widget_t *cl = widget_find(w, ID_CLUSTER);
    if (!cl) return;
    char names[MAX_CLUSTERS][32], *items[MAX_CLUSTERS];
    f->nclusters = 0;
    f->clusters[f->nclusters++] = 0;
    for (u32 c = 512; c <= fs_cluster_max[f->fs] && f->nclusters < MAX_CLUSTERS; c *= 2)
        if (c >= f->bd->sector_size) f->clusters[f->nclusters++] = c;
    for (int i = 0; i < f->nclusters; i++) {
        cluster_name(f->clusters[i], names[i], sizeof names[i]);
        items[i] = names[i];
    }
    widget_list_set(w, cl, items, f->nclusters);
    cl->value = 0;
    cl->top = 0;
}

static void fs_changed(window_t *w, widget_t *wd) {
    fmt_t *f = w->data;
    int fs = CLAMP(wd->value, 0, FS_COUNT - 1);
    if (fs == f->fs) return;
    f->fs = fs;
    fill_clusters(w, f);
    wm_invalidate(w);
}

static void fmt_close_btn(window_t *w, widget_t *wd) { ((fmt_t *)w->data)->done = 1; }
static int fmt_on_close(window_t *w) { ((fmt_t *)w->data)->done = 1; return 0; }

static void fmt_start(window_t *w, widget_t *wd) {
    fmt_t *f = w->data;
    widget_t *lb = widget_find(w, ID_LABEL), *cl = widget_find(w, ID_CLUSTER);
    int sel = f->fs;
    u32 cluster = cl && cl->value >= 0 && cl->value < f->nclusters ? f->clusters[cl->value] : 0;
    const char *label = lb ? lb->text : "";
    char msg[300];
    if (utf8_count(label, (int)strlen(label)) > fs_label_max[sel]) {
        snprintf(msg, sizeof msg, "The volume label is too long: %s keeps up to %d characters.", fs_names[sel],
                 fs_label_max[sel]);
        msgbox(w, "Format", msg, ICON_WARNING, MB_OK);
        return;
    }
    snprintf(msg, sizeof msg, "WARNING: Formatting will erase ALL data on this disk.\n"
                              "To format the disk, click OK. To quit, click Cancel.");
    if (msgbox(w, "Format", msg, ICON_WARNING, MB_OKCANCEL) != ID_OK) return;
    wm_set_busy(1);
    int r = vfs_format(f->bd, fs_names[sel], label, cluster);
    wm_set_busy(0);
    if (r) {
        const char *why = r == E_BUSY ? "The disk is in use: close the files open on it and try again." :
                          r == E_INVAL ? "The disk is too small or too large for this file system and allocation "
                                         "unit size, or the volume label holds characters it does not allow." :
                          r == E_NOSPC ? "The disk is too small for this file system." :
                          r == E_ROFS ? "The disk is write-protected." :
                          r == E_NOTSUP ? "This file system is not available." : strerror(r);
        snprintf(msg, sizeof msg, "Halcyon was unable to complete the format.\n\n%s", why);
        msgbox(w, "Format", msg, ICON_WARNING, MB_OK);
        return;
    }
    /* (until Halcyon has an NTFS driver an NTFS volume does not mount) */
    int mounted = 0;
    for (int i = 0; i < vfs_volume_count(); i++) if (vfs_volume(i)->bd == f->bd) mounted = 1;
    msgbox(w, "Formatting Complete", mounted ? "Format Complete." :
           "Format Complete.\n\nHalcyon cannot open drives with this file system yet, so the drive is not shown "
           "in My Computer. Windows can use it.", ICON_HDD, MB_OK);
    f->done = 1;
}

/* Format the volume on drive letter; returns 1 if it was formatted */
int format_dialog(window_t *owner, char letter) {
    volume_t *v = vfs_volume_by_letter(letter);
    if (!v || !v->bd) return 0;
    if (v->is_boot) {
        msgbox(owner, "Format", "Halcyon cannot format the drive it was started from.", ICON_WARNING, MB_OK);
        return 0;
    }
    char title[96], cap[64], sz[32];
    snprintf(title, sizeof title, "Format %s (%c:)", v->label[0] ? v->label : "Local Disk", letter);
    format_size(v->bd->sector_count * v->bd->sector_size, sz, sizeof sz);
    snprintf(cap, sizeof cap, "Capacity: %s", sz);

    window_t *w = wm_create(title, ICON_HDD, 0, 0, 280, 392, WF_DIALOG | WF_CENTER);
    if (w) w->help = "format";              /* F1 */
    fmt_t f = { 0 };
    f.bd = v->bd;
    f.letter = letter;
    w->data = &f;
    w->on_close = fmt_on_close;
    widget_add(w, W_LABEL, mkrect(14, 12, 252, 16), cap, 0);
    widget_add(w, W_LABEL, mkrect(14, 36, 252, 16), "File system:", 0);
    widget_t *fs = widget_add(w, W_LISTBOX, mkrect(14, 54, 252, FS_COUNT * 18 + 2), "", ID_FS);
    widget_list_set(w, fs, (char **)fs_items, FS_COUNT);
    /* the current file system, else what Windows would suggest */
    f.fs = !strcmp(v->fs_name, "exFAT") ? FS_EXFAT : !strcmp(v->fs_name, "FAT32") ? FS_FAT32 :
           !strcmp(v->fs_name, "NTFS") ? FS_NTFS :
           (!strcmp(v->fs_name, "FAT16") || !strcmp(v->fs_name, "FAT12")) ? FS_FAT : default_fs(v->bd);
    fs->value = f.fs;
    fs->on_change = fs_changed;
    widget_add(w, W_LABEL, mkrect(14, 136, 252, 16), "Allocation unit size:", 0);
    widget_add(w, W_LISTBOX, mkrect(14, 154, 252, 4 * 18 + 2), "", ID_CLUSTER);
    fill_clusters(w, &f);
    widget_add(w, W_LABEL, mkrect(14, 238, 252, 16), "Volume label:", 0);
    widget_t *lb = widget_add(w, W_TEXTBOX, mkrect(14, 256, 252, 22), v->label, ID_LABEL);
    widget_add(w, W_LABEL, mkrect(14, 290, 252, 48),
               "Quick format: the disk is not scanned, only a new, empty file system is written.", 0)->style |= WS_WRAP;
    widget_t *st = widget_add(w, W_BUTTON, mkrect(110, 352, 75, 23), "Start", ID_START);
    st->style |= WS_DEFAULT; st->on_click = fmt_start;
    widget_t *cl = widget_add(w, W_BUTTON, mkrect(191, 352, 75, 23), "Close", ID_CLOSE);
    cl->style |= WS_CANCEL; cl->on_click = fmt_close_btn;
    w->focus = lb;
    if (owner && wm_alive(owner)) { owner->modal = w; w->owner = owner; }
    while (!f.done && wm_alive(w)) gui_step();
    if (owner && wm_alive(owner) && owner->modal == w) owner->modal = NULL;
    if (wm_alive(w)) { w->on_close = NULL; wm_destroy(w); }
    if (owner && wm_alive(owner)) wm_raise(owner);
    return vfs_volume_by_letter(letter) && vfs_volume_by_letter(letter)->bd == f.bd;
}

/* test hook: QEMU fw_cfg "opt/halcyon/formattest" = "MODEL FS LABEL [CLUSTER]"
 * formats the disk whose model is MODEL (its first partition if it has
 * any) once the desktop is up, as the Format dialog would (boottest.py's
 * ntfs variant checks the result on the host). Never active on real
 * hardware (fw_cfg exists only under QEMU). */
void format_test_hook(void) {
    char spec[160];
    if (!qemu_fw_file("opt/halcyon/formattest", spec, sizeof spec) || !spec[0]) return;
    char *tok[4] = { 0 };
    int n = 0;
    for (char *p = spec; *p && n < 4;) {
        while (*p == ' ') *p++ = 0;
        if (!*p) break;
        tok[n++] = p;
        while (*p && *p != ' ') p++;
    }
    if (n < 3) { kprintf("format test: bad spec\n"); return; }
    blockdev_t *bd = NULL;
    for (blockdev_t *b = blockdev_first(); b && !bd; b = b->next)
        if (b->type != BDEV_PART && b->present && !strcmp(b->model, tok[0])) bd = b;
    for (blockdev_t *b = blockdev_first(); bd && b; b = b->next)
        if (b->parent == bd && b->part_index == 1) { bd = b; break; }
    if (!bd) { kprintf("format test: no disk '%s'\n", tok[0]); return; }
    u32 cluster = n > 3 ? (u32)strtol(tok[3], NULL, 10) : 0;
    int r = vfs_format(bd, tok[1], tok[2], cluster);
    /* (unmounted again, if a driver mounted it: the host checks the image as
     * the formatter left it, and QEMU is not shut down cleanly) */
    vfs_unmount_blockdev(bd);
    kprintf("format test: %s as %s '%s': %s\n", bd->name, tok[1], tok[2], r ? strerror(r) : "done");
}
