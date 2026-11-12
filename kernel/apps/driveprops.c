/* Halcyon OS - drive properties (Explorer: right-click a drive, Properties)
 * and the "Allow Halcyon to write to this drive" setting.
 *
 * NTFS volumes on internal disks with Windows on them are mounted
 * read-only (the NTFS driver decides, and sets volume_t.ro_policy when that
 * rule is the only reason). The user can allow writing per volume: the key
 * ntfs.write.<serial as 16 uppercase hex digits> = 1 in SETTINGS.INI, which
 * the driver honours at mount time; the volume is mounted again at once.
 * The user-mode Explorer reaches volume_set_write through
 * SYS_VOLUME_SET_WRITE and shows its own dialog; this one serves the
 * kernel's fallback Explorer. */
#include "gui/wm.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/settings.h"
#include "fs/vfs.h"
#include "fileops.h"

void ntfs_write_key(u64 serial, char *out, size_t n) {
    snprintf(out, n, "ntfs.write.%016llX", (unsigned long long)serial);
}

int volume_write_allowed(const volume_t *v) {
    if (!v || !v->serial || strcmp(v->fs_name, "NTFS")) return 0;
    char key[40];
    ntfs_write_key(v->serial, key, sizeof key);
    return settings_get_int(key, 0) == 1;
}

int volume_write_option(const volume_t *v) {
    if (!v || !v->bd || v->bd->readonly || !v->serial || strcmp(v->fs_name, "NTFS")) return 0;
    return v->ro_policy || volume_write_allowed(v);
}

int volume_set_write(char letter, int allow) {
    volume_t *v = vfs_volume_by_letter(letter);
    if (!v) return E_NOENT;
    if (!volume_write_option(v)) return E_NOTSUP;
    allow = !!allow;
    int was = volume_write_allowed(v);
    if (allow == was) return 0;
    if (v->open_files) return E_BUSY;
    char key[40];
    ntfs_write_key(v->serial, key, sizeof key);
    settings_set(key, allow ? "1" : NULL);
    int r = vfs_remount(letter);            /* (v may be gone now) */
    if (r == E_BUSY) {                      /* files were opened meanwhile: nothing changes */
        settings_set(key, was ? "1" : NULL);
        return r;
    }
    int s = settings_save();
    kprintf("volume: %c: writing %s by the user%s%s\n", letter, allow ? "allowed" : "no longer allowed",
            s ? "; SETTINGS.INI not saved: " : "", s ? strerror(s) : "");
    return r;
}

const char *volume_write_warning(void) {
    return "Halcyon can write to this NTFS drive, but it is an internal disk with Windows on it.\n\n"
           "- Writing to it from Halcyon could damage files that Windows needs. Keep backups of "
           "everything important on it.\n"
           "- Shut Windows down completely before you start Halcyon: with Fast Startup "
           "(or hibernation) Windows keeps the drive in use, and Halcyon then keeps it read-only.\n\n"
           "Do you want Halcyon to write to this drive?";
}

/* ------------------------------------------------------------------ */
/* the dialog                                                           */

enum { ID_WRITE = 400, ID_OK_BTN, ID_CANCEL_BTN };

typedef struct props {
    volatile int done;
    char letter;
    u64 total, free_bytes;
    int bar_y;
    int sep_y[2];
    int write_was;
} props_t;

static void props_paint(window_t *w, gc_t *g) {
    props_t *p = w->data;
    if (!p) return;
    int cw = w->client->w;
    for (int i = 0; i < 2; i++) {
        gfx_hline(g, 14, p->sep_y[i], cw - 28, HEX(0xACA899));
        gfx_hline(g, 14, p->sep_y[i] + 1, cw - 28, HEX(0xFFFFFF));
    }
    /* used / free bar, and the colour keys of the two lines above it */
    rect_t bar = mkrect(14, p->bar_y, cw - 28, 16);
    gfx_sunken_field(g, bar, HEX(0xFF00FF));
    rect_t in = rect_inset(bar, 2);
    int used = p->total ? (int)((p->total - p->free_bytes) * (u64)in.w / p->total) : 0;
    gfx_fill(g, mkrect(in.x, in.y, in.w, in.h), HEX(0xFF00FF));
    if (used > 0) gfx_fill(g, mkrect(in.x, in.y, MIN(used, in.w), in.h), HEX(0x0000FF));
    gfx_fill(g, mkrect(14, p->bar_y - 66, 12, 12), HEX(0x0000FF));
    gfx_rect(g, mkrect(14, p->bar_y - 66, 12, 12), HEX(0x000000));
    gfx_fill(g, mkrect(14, p->bar_y - 48, 12, 12), HEX(0xFF00FF));
    gfx_rect(g, mkrect(14, p->bar_y - 48, 12, 12), HEX(0x000000));
}

static int props_close(window_t *w) { ((props_t *)w->data)->done = 1; return 0; }
static void props_cancel(window_t *w, widget_t *wd) { ((props_t *)w->data)->done = 1; }

static void props_write_click(window_t *w, widget_t *wd) {
    /* ticking the box: say what it means first */
    if (wd->value && msgbox(w, "Allow Writing", volume_write_warning(), ICON_WARNING, MB_YESNO) != ID_YES) {
        wd->value = 0;
        wm_invalidate(w);
    }
}

static void props_ok(window_t *w, widget_t *wd) {
    props_t *p = w->data;
    widget_t *cb = widget_find(w, ID_WRITE);
    if (cb && !!cb->value != p->write_was) {
        wm_set_busy(1);
        int r = volume_set_write(p->letter, cb->value);
        wm_set_busy(0);
        if (r) {
            char msg[300];
            snprintf(msg, sizeof msg, "The setting could not be changed.\n\n%s",
                     r == E_BUSY ? "Files on the drive are open. Close them (and any window showing the drive's "
                                   "files from a program), then try again." : strerror(r));
            msgbox(w, "Drive Properties", msg, ICON_WARNING, MB_OK);
            if (r == E_BUSY) return;
        }
    }
    p->done = 1;
}

static void row(window_t *w, int y, const char *k, const char *v) {
    widget_add(w, W_LABEL, mkrect(14, y, 96, 16), k, 0);
    widget_add(w, W_LABEL, mkrect(112, y, 220, 16), v, 0);
}

void drive_props_dialog(window_t *owner, char letter) {
    volume_t *v = vfs_volume_by_letter(letter);
    if (!v) return;
    props_t p = { 0 };
    p.letter = v->letter;
    vfs_statfs(v->letter, &p.total, &p.free_bytes);
    int ro = v->readonly || (v->bd && v->bd->readonly);
    int opt = volume_write_option(v);
    p.write_was = volume_write_allowed(v);
    char reason[200];
    if (v->readonly && v->ro_reason[0]) strlcpy(reason, v->ro_reason, sizeof reason);
    else if (v->bd && v->bd->readonly)
        strlcpy(reason, drive_icon(v) == ICON_CDROM ? "CDs and DVDs cannot be written to." :
                        "The disk is write-protected.", sizeof reason);
    else strlcpy(reason, "The file system is mounted read-only.", sizeof reason);

    int W = 346, H = 330 + (ro ? 72 : 0) + (opt ? 28 : 0);
    char title[96], name[96];
    snprintf(name, sizeof name, "%s (%c:)", v->label[0] ? v->label : drive_kind(v), v->letter);
    snprintf(title, sizeof title, "%s Properties", name);
    window_t *w = wm_create(title, drive_icon(v), -1, -1, W, H, WF_DIALOG | WF_CENTER);
    if (!w) return;
    w->data = &p;
    w->on_close = props_close;
    w->on_paint = props_paint;
    widget_t *ic = widget_add(w, W_ICON, mkrect(14, 12, 32, 32), "", 0);
    ic->value = drive_icon(v);
    widget_add(w, W_LABEL, mkrect(62, 20, W - 76, 16), name, 0)->style |= WS_BOLD;
    p.sep_y[0] = 54;
    int y = 66;
    char buf[96], a[48], b[48];
    row(w, y, "Type:", drive_kind(v)); y += 20;
    row(w, y, "File system:", v->fs_name); y += 20;
    snprintf(buf, sizeof buf, "%s (%s)", v->bd ? v->bd->model : "?", v->bd ? v->bd->name : "?");
    row(w, y, "Device:", buf); y += 20;
    if (v->serial) {
        snprintf(buf, sizeof buf, "%04X-%04X", (unsigned)(v->serial >> 16) & 0xFFFF, (unsigned)v->serial & 0xFFFF);
        row(w, y, "Serial number:", buf);
    }
    y += 26;
    p.sep_y[1] = y;
    y += 12;
    u64 used = p.total - p.free_bytes;
    format_size_exact(used, a, sizeof a); format_size(used, b, sizeof b);
    widget_add(w, W_LABEL, mkrect(34, y, 80, 16), "Used space:", 0);
    widget_add(w, W_LABEL, mkrect(120, y, 140, 16), a, 0);
    widget_add(w, W_LABEL, mkrect(262, y, 70, 16), b, 0);
    y += 18;
    format_size_exact(p.free_bytes, a, sizeof a); format_size(p.free_bytes, b, sizeof b);
    widget_add(w, W_LABEL, mkrect(34, y, 80, 16), "Free space:", 0);
    widget_add(w, W_LABEL, mkrect(120, y, 140, 16), a, 0);
    widget_add(w, W_LABEL, mkrect(262, y, 70, 16), b, 0);
    y += 24;
    format_size_exact(p.total, a, sizeof a); format_size(p.total, b, sizeof b);
    widget_add(w, W_LABEL, mkrect(34, y, 80, 16), "Capacity:", 0);
    widget_add(w, W_LABEL, mkrect(120, y, 140, 16), a, 0);
    widget_add(w, W_LABEL, mkrect(262, y, 70, 16), b, 0);
    y += 26;
    p.bar_y = y;
    y += 28;
    if (ro) {
        widget_t *wi = widget_add(w, W_ICON, mkrect(14, y, 32, 32), "", 0);
        wi->value = ICON_WARNING;
        snprintf(buf, sizeof buf, "This drive is read-only.");
        widget_add(w, W_LABEL, mkrect(56, y, W - 70, 16), buf, 0)->style |= WS_BOLD;
        widget_add(w, W_LABEL, mkrect(56, y + 16, W - 70, 54), reason, 0)->style |= WS_WRAP;
        y += 72;
    }
    if (opt) {
        widget_t *cb = widget_add(w, W_CHECKBOX, mkrect(14, y, W - 28, 18), "Allow Halcyon to write to this drive", ID_WRITE);
        cb->value = p.write_was;
        cb->on_click = props_write_click;
        y += 28;
    }
    widget_t *ok = widget_add(w, W_BUTTON, mkrect(W - 2 * 83, H - 36, 75, 23), "OK", ID_OK_BTN);
    ok->style |= WS_DEFAULT; ok->on_click = props_ok;
    widget_t *cl = widget_add(w, W_BUTTON, mkrect(W - 83, H - 36, 75, 23), "Cancel", ID_CANCEL_BTN);
    cl->style |= WS_CANCEL; cl->on_click = props_cancel;
    w->focus = ok;
    if (owner && wm_alive(owner)) { owner->modal = w; w->owner = owner; }
    while (!p.done && wm_alive(w)) gui_step();
    if (owner && wm_alive(owner) && owner->modal == w) owner->modal = NULL;
    if (wm_alive(w)) { w->on_close = NULL; w->on_paint = NULL; w->data = NULL; wm_destroy(w); }
    if (owner && wm_alive(owner)) wm_raise(owner);
}
