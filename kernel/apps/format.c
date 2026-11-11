/* Halcyon OS - Format dialog (Explorer: right-click a drive, Format...) */
#include "gui/wm.h"
#include "core/lib.h"
#include "core/mm.h"
#include "fs/vfs.h"
#include "fileops.h"

enum { ID_FS = 300, ID_LABEL = 301, ID_START = 302, ID_CLOSE = 303 };

static const char *const fs_items[] = { "FAT32", "FAT (FAT12 / FAT16)", "exFAT" };
static const char *const fs_names[] = { "FAT32", "FAT", "exFAT" };

typedef struct fmt {
    volatile int done;
    int started;
    blockdev_t *bd;
    char letter;
} fmt_t;

static void fmt_close_btn(window_t *w, widget_t *wd) { ((fmt_t *)w->data)->done = 1; }
static int fmt_on_close(window_t *w) { ((fmt_t *)w->data)->done = 1; return 0; }

static void fmt_start(window_t *w, widget_t *wd) {
    fmt_t *f = w->data;
    widget_t *fs = widget_find(w, ID_FS), *lb = widget_find(w, ID_LABEL);
    int sel = fs ? CLAMP(fs->value, 0, 2) : 0;
    char msg[200];
    snprintf(msg, sizeof msg, "WARNING: Formatting will erase ALL data on this disk.\n"
                              "To format the disk, click OK. To quit, click Cancel.");
    if (msgbox(w, "Format", msg, ICON_WARNING, MB_OKCANCEL) != ID_OK) return;
    wm_set_busy(1);
    int r = vfs_format(f->bd, fs_names[sel], lb ? lb->text : "", 0);
    wm_set_busy(0);
    if (r) {
        const char *why = r == E_BUSY ? "The disk is in use: close the files open on it and try again." :
                          r == E_INVAL ? "The disk is too small or too large for this file system." :
                          r == E_ROFS ? "The disk is write-protected." :
                          r == E_NOTSUP ? "This file system is not available." : strerror(r);
        snprintf(msg, sizeof msg, "Halcyon was unable to complete the format.\n\n%s", why);
        msgbox(w, "Format", msg, ICON_WARNING, MB_OK);
        return;
    }
    msgbox(w, "Formatting Complete", "Format Complete.", ICON_HDD, MB_OK);
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
    char title[48], cap[64], sz[32];
    snprintf(title, sizeof title, "Format %s (%c:)", v->label[0] ? v->label : "Local Disk", letter);
    format_size(v->bd->sector_count * v->bd->sector_size, sz, sizeof sz);
    snprintf(cap, sizeof cap, "Capacity: %s", sz);

    window_t *w = wm_create(title, ICON_HDD, 0, 0, 280, 286, WF_DIALOG | WF_CENTER);
    fmt_t f = { 0 };
    f.bd = v->bd;
    f.letter = letter;
    w->data = &f;
    w->on_close = fmt_on_close;
    widget_add(w, W_LABEL, mkrect(14, 12, 252, 16), cap, 0);
    widget_add(w, W_LABEL, mkrect(14, 36, 252, 16), "File system:", 0);
    widget_t *fs = widget_add(w, W_LISTBOX, mkrect(14, 54, 252, 3 * 18 + 2), "", ID_FS);
    widget_list_set(w, fs, (char **)fs_items, 3);
    /* the current file system, else what Windows would suggest for the size */
    u64 bytes = v->bd->sector_count * v->bd->sector_size;
    fs->value = !strcmp(v->fs_name, "exFAT") ? 2 : !strcmp(v->fs_name, "FAT32") ? 0 :
                (!strcmp(v->fs_name, "FAT16") || !strcmp(v->fs_name, "FAT12")) ? 1 :
                bytes > (32ull << 30) ? 2 : bytes >= (512ull << 20) ? 0 : 1;
    widget_add(w, W_LABEL, mkrect(14, 124, 252, 16), "Volume label:", 0);
    widget_t *lb = widget_add(w, W_TEXTBOX, mkrect(14, 142, 252, 22), v->label, ID_LABEL);
    widget_add(w, W_LABEL, mkrect(14, 176, 252, 48),
               "Quick format: the disk is not scanned, only a new, empty file system is written.", 0)->style |= WS_WRAP;
    widget_t *st = widget_add(w, W_BUTTON, mkrect(110, 246, 75, 23), "Start", ID_START);
    st->style |= WS_DEFAULT; st->on_click = fmt_start;
    widget_t *cl = widget_add(w, W_BUTTON, mkrect(191, 246, 75, 23), "Close", ID_CLOSE);
    cl->style |= WS_CANCEL; cl->on_click = fmt_close_btn;
    w->focus = lb;
    if (owner && wm_alive(owner)) { owner->modal = w; w->owner = owner; }
    while (!f.done && wm_alive(w)) gui_step();
    if (owner && wm_alive(owner) && owner->modal == w) owner->modal = NULL;
    if (wm_alive(w)) { w->on_close = NULL; wm_destroy(w); }
    if (owner && wm_alive(owner)) wm_raise(owner);
    return vfs_volume_by_letter(letter) && vfs_volume_by_letter(letter)->bd == f.bd;
}
