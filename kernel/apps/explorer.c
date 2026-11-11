/* Halcyon OS - Explorer: My Computer / folder browser and file manager */
#include "gui/wm.h"
#include "core/lib.h"
#include "core/mm.h"
#include "fs/vfs.h"
#include "fileops.h"

typedef struct item {
    char name[VFS_MAX_NAME + 1];
    int is_dir, icon;
    u64 size;
    vfs_time_t mtime;
    char letter;          /* drives (My Computer) */
    u64 total, free;
} item_t;

#define MAX_LINKS 16
typedef struct link { rect_t r; int id; } link_t;

typedef struct ex {
    char path[VFS_MAX_PATH];        /* "" = My Computer */
    item_t *items; int n;
    int sel, hover;
    int scroll;
    int view;                       /* 0 icons, 1 details */
    char back[16][VFS_MAX_PATH]; int nback;
    u32 vgen;
    widget_t *addr;
    rect_t view_r;
    int content_h;
    int sb_drag;
    link_t links[MAX_LINKS]; int nlinks; int link_hover;
    int tb_hover, tb_pressed;
    window_t *win;
} ex_t;

/* explorer clipboard */
static char clip_path[VFS_MAX_PATH];
static int clip_cut;

#define TOOLBAR_H 38
#define ADDR_H 28
#define STATUS_H 22
#define PANE_W 196
#define CELL_W 92
#define CELL_H 76
#define TILE_W 250
#define TILE_H 60
#define ROW_H 19

enum { CMD_NEWFOLDER = 1, CMD_NEWTEXT, CMD_RENAME, CMD_DELETE, CMD_COPY, CMD_CUT, CMD_PASTE, CMD_PROPS, CMD_CLOSE,
       CMD_OPEN, CMD_VIEW_ICONS, CMD_VIEW_DETAILS, CMD_REFRESH, CMD_BACK, CMD_UP, CMD_MYCOMP, CMD_MYDOCS,
       CMD_TERMINAL, CMD_SYSINFO, CMD_CONTROL, CMD_OPEN_NOTEPAD, CMD_ABOUT, CMD_FORMAT };

static menu_item_t file_items[] = {
    { "Open", "Enter", CMD_OPEN, MI_BOLD, 0 },
    { "", NULL, 0, MI_SEPARATOR, 0 },
    { "New Folder", NULL, CMD_NEWFOLDER, 0, ICON_FOLDER },
    { "New Text Document", NULL, CMD_NEWTEXT, 0, ICON_FILE_TEXT },
    { "", NULL, 0, MI_SEPARATOR, 0 },
    { "Delete", "Del", CMD_DELETE, 0, 0 },
    { "Rename", "F2", CMD_RENAME, 0, 0 },
    { "Properties", NULL, CMD_PROPS, 0, 0 },
    { "", NULL, 0, MI_SEPARATOR, 0 },
    { "Close", NULL, CMD_CLOSE, 0, 0 },
};
static menu_item_t edit_items[] = {
    { "Cut", "Ctrl+X", CMD_CUT, 0, 0 },
    { "Copy", "Ctrl+C", CMD_COPY, 0, 0 },
    { "Paste", "Ctrl+V", CMD_PASTE, 0, 0 },
};
static menu_item_t view_items[] = {
    { "Icons", NULL, CMD_VIEW_ICONS, 0, 0 },
    { "Details", NULL, CMD_VIEW_DETAILS, 0, 0 },
    { "", NULL, 0, MI_SEPARATOR, 0 },
    { "Refresh", "F5", CMD_REFRESH, 0, 0 },
};
static menu_item_t go_items[] = {
    { "Back", "Alt+Left", CMD_BACK, 0, 0 },
    { "Up One Level", "Backspace", CMD_UP, 0, 0 },
    { "", NULL, 0, MI_SEPARATOR, 0 },
    { "My Computer", NULL, CMD_MYCOMP, 0, ICON_COMPUTER },
    { "My Documents", NULL, CMD_MYDOCS, 0, ICON_DOCUMENTS },
};
static menu_item_t help_items[] = {
    { "About Halcyon", NULL, CMD_ABOUT, 0, ICON_HELP },
};
static menu_t ex_menus[] = {
    { "File", file_items, ARRAY_SIZE(file_items) },
    { "Edit", edit_items, ARRAY_SIZE(edit_items) },
    { "View", view_items, ARRAY_SIZE(view_items) },
    { "Go", go_items, ARRAY_SIZE(go_items) },
    { "Help", help_items, ARRAY_SIZE(help_items) },
};

static menu_item_t ctx_file_items[] = {
    { "Open", NULL, CMD_OPEN, MI_BOLD, 0 },
    { "Open with Notepad", NULL, CMD_OPEN_NOTEPAD, 0, ICON_NOTEPAD },
    { "", NULL, 0, MI_SEPARATOR, 0 },
    { "Cut", NULL, CMD_CUT, 0, 0 },
    { "Copy", NULL, CMD_COPY, 0, 0 },
    { "", NULL, 0, MI_SEPARATOR, 0 },
    { "Delete", NULL, CMD_DELETE, 0, 0 },
    { "Rename", NULL, CMD_RENAME, 0, 0 },
    { "", NULL, 0, MI_SEPARATOR, 0 },
    { "Properties", NULL, CMD_PROPS, 0, 0 },
};
static menu_t ctx_file = { "", ctx_file_items, ARRAY_SIZE(ctx_file_items) };
static menu_item_t ctx_bg_items[] = {
    { "View: Icons", NULL, CMD_VIEW_ICONS, 0, 0 },
    { "View: Details", NULL, CMD_VIEW_DETAILS, 0, 0 },
    { "Refresh", NULL, CMD_REFRESH, 0, 0 },
    { "", NULL, 0, MI_SEPARATOR, 0 },
    { "Paste", NULL, CMD_PASTE, 0, 0 },
    { "", NULL, 0, MI_SEPARATOR, 0 },
    { "New Folder", NULL, CMD_NEWFOLDER, 0, ICON_FOLDER },
    { "New Text Document", NULL, CMD_NEWTEXT, 0, ICON_FILE_TEXT },
    { "", NULL, 0, MI_SEPARATOR, 0 },
    { "Command Prompt Here", NULL, CMD_TERMINAL, 0, ICON_TERMINAL },
};
static menu_t ctx_bg = { "", ctx_bg_items, ARRAY_SIZE(ctx_bg_items) };
static menu_item_t ctx_drive_items[] = {
    { "Open", NULL, CMD_OPEN, MI_BOLD, 0 },
    { "", NULL, 0, MI_SEPARATOR, 0 },
    { "Format...", NULL, CMD_FORMAT, 0, 0 },
    { "", NULL, 0, MI_SEPARATOR, 0 },
    { "Properties", NULL, CMD_PROPS, 0, 0 },
};
static menu_t ctx_drive = { "", ctx_drive_items, ARRAY_SIZE(ctx_drive_items) };

/* ---------------- model ---------------- */
static int item_cmp(const item_t *a, const item_t *b) {
    if (a->is_dir != b->is_dir) return b->is_dir - a->is_dir;
    return strcasecmp(a->name, b->name);
}

static void sort_items(item_t *it, int n) {
    for (int i = 1; i < n; i++) {
        item_t tmp = it[i];
        int j = i - 1;
        while (j >= 0 && item_cmp(&it[j], &tmp) > 0) { it[j + 1] = it[j]; j--; }
        it[j + 1] = tmp;
    }
}

static void update_title(ex_t *x) {
    char t[VFS_MAX_PATH + 16];
    if (!x->path[0]) strlcpy(t, "My Computer", sizeof t);
    else if (strlen(x->path) <= 3) {
        volume_t *v = vfs_volume_by_letter(x->path[0]);
        snprintf(t, sizeof t, "%s (%c:)", v && v->label[0] ? v->label : "Local Disk", x->path[0]);
    } else strlcpy(t, vfs_basename(x->path), sizeof t);
    wm_set_title(x->win, t);
    x->win->icon = !x->path[0] ? ICON_COMPUTER : ICON_FOLDER_OPEN;
    if (x->addr) widget_set_text(x->win, x->addr, x->path[0] ? x->path : "My Computer");
}

static void load(ex_t *x) {
    kfree(x->items);
    x->items = NULL; x->n = 0;
    x->sel = -1; x->hover = -1; x->scroll = 0;
    x->vgen = vfs_generation();
    if (!x->path[0]) {
        int nv = vfs_volume_count();
        x->items = kzalloc(sizeof(item_t) * (nv + 1));
        for (int i = 0; i < nv; i++) {
            volume_t *v = vfs_volume(i);
            item_t *it = &x->items[x->n++];
            snprintf(it->name, sizeof it->name, "%s (%c:)", v->label[0] ? v->label : drive_kind(v), v->letter);
            it->letter = v->letter;
            it->is_dir = 1;
            it->icon = drive_icon(v);
            vfs_statfs(v->letter, &it->total, &it->free);
        }
    } else {
        vfs_dirent_t *ents;
        int n = vfs_list(x->path, &ents);
        if (n < 0) {
            char msg[VFS_MAX_PATH + 64];
            snprintf(msg, sizeof msg, "%s is not accessible.\n\n%s", x->path, strerror(n));
            x->path[0] = 0;
            msgbox(x->win, "Explorer", msg, ICON_ERROR, MB_OK);
            load(x);
            return;
        }
        x->items = kzalloc(sizeof(item_t) * (n + 1));
        for (int i = 0; i < n; i++) {
            if (ents[i].attr & (VFS_ATTR_HIDDEN | VFS_ATTR_VOLUME)) continue;
            item_t *it = &x->items[x->n++];
            strlcpy(it->name, ents[i].name, sizeof it->name);
            it->is_dir = ents[i].is_dir;
            it->size = ents[i].size;
            it->mtime = ents[i].mtime;
            it->icon = file_icon(it->name, it->is_dir);
        }
        kfree(ents);
        sort_items(x->items, x->n);
    }
    update_title(x);
    wm_invalidate(x->win);
}

static void navigate(ex_t *x, const char *path, int push) {
    char norm[VFS_MAX_PATH] = "";
    if (path[0] && strcasecmp(path, "My Computer")) {
        if (vfs_normalize(path, norm, sizeof norm) < 0) {
            msgbox(x->win, "Explorer", "The path is not valid.", ICON_ERROR, MB_OK);
            return;
        }
        vfs_dirent_t st;
        if (vfs_stat(norm, &st) < 0) {
            char msg[VFS_MAX_PATH + 64];
            snprintf(msg, sizeof msg, "Cannot find '%s'.", path);
            msgbox(x->win, "Explorer", msg, ICON_ERROR, MB_OK);
            return;
        }
        if (!st.is_dir) { shell_open(norm); return; }
    }
    if (push && strcmp(x->path, norm)) {
        if (x->nback == 16) { memmove(x->back[0], x->back[1], sizeof(x->back[0]) * 15); x->nback--; }
        strlcpy(x->back[x->nback++], x->path, VFS_MAX_PATH);
    }
    strlcpy(x->path, norm, sizeof x->path);
    load(x);
}

static void go_up(ex_t *x) {
    if (!x->path[0]) return;
    if (strlen(x->path) <= 3) { navigate(x, "", 1); return; }
    char p[VFS_MAX_PATH];
    vfs_dirname(x->path, p, sizeof p);
    navigate(x, p, 1);
}

static void item_path(ex_t *x, int i, char *out, int n) {
    if (!x->path[0]) { snprintf(out, n, "%c:/", x->items[i].letter); return; }
    vfs_join(x->path, x->items[i].name, out, n);
}

static void open_item(ex_t *x, int i) {
    if (i < 0 || i >= x->n) return;
    char p[VFS_MAX_PATH];
    item_path(x, i, p, sizeof p);
    if (x->items[i].is_dir) navigate(x, p, 1);
    else shell_open(p);
}

static void select_name(ex_t *x, const char *name) {
    for (int i = 0; i < x->n; i++) if (!strcasecmp(x->items[i].name, name)) x->sel = i;
}

/* ---------------- commands ---------------- */
static void report(ex_t *x, const char *what, int err) {
    char msg[160];
    snprintf(msg, sizeof msg, "%s\n\n%s", what, strerror(err));
    msgbox(x->win, "Error", msg, ICON_ERROR, MB_OK);
}

static void cmd_new(ex_t *x, int folder) {
    if (!x->path[0]) return;
    char path[VFS_MAX_PATH];
    fileops_unique_name(x->path, folder ? "New Folder" : "New Text Document.txt", path, sizeof path);
    char name[VFS_MAX_NAME + 1];
    strlcpy(name, vfs_basename(path), sizeof name);
    if (!inputbox(x->win, folder ? "New Folder" : "New Text Document", folder ? "Name of the new folder:" : "Name of the new document:", name, sizeof name) || !name[0]) return;
    vfs_join(x->path, name, path, sizeof path);
    int r;
    if (folder) r = vfs_mkdir(path);
    else r = vfs_write_file(path, "", 0);
    if (r < 0) { report(x, folder ? "Cannot create the folder." : "Cannot create the file.", r); return; }
    load(x);
    select_name(x, name);
}

static void cmd_rename(ex_t *x) {
    if (!x->path[0] || x->sel < 0) return;
    char name[VFS_MAX_NAME + 1];
    strlcpy(name, x->items[x->sel].name, sizeof name);
    if (!inputbox(x->win, "Rename", "New name:", name, sizeof name) || !name[0] || !strcmp(name, x->items[x->sel].name)) return;
    char from[VFS_MAX_PATH], to[VFS_MAX_PATH];
    item_path(x, x->sel, from, sizeof from);
    vfs_join(x->path, name, to, sizeof to);
    int r = vfs_rename(from, to);
    if (r < 0) { report(x, "Cannot rename the item.", r); return; }
    load(x);
    select_name(x, name);
}

static void cmd_delete(ex_t *x) {
    if (!x->path[0] || x->sel < 0) return;
    item_t *it = &x->items[x->sel];
    char msg[VFS_MAX_NAME + 96];
    snprintf(msg, sizeof msg, it->is_dir ? "Are you sure you want to delete the folder '%s' and all of its contents?"
                                         : "Are you sure you want to delete '%s'?", it->name);
    if (msgbox(x->win, it->is_dir ? "Confirm Folder Delete" : "Confirm File Delete", msg, ICON_WARNING, MB_YESNO) != ID_YES) return;
    char p[VFS_MAX_PATH];
    item_path(x, x->sel, p, sizeof p);
    wm_set_busy(1);
    int r = fileops_delete_tree(p);
    wm_set_busy(0);
    if (r < 0) report(x, "Cannot delete the item.", r);
    int keep = x->sel;
    load(x);
    x->sel = MIN(keep, x->n - 1);
}

static void cmd_copy(ex_t *x, int cut) {
    if (!x->path[0] || x->sel < 0) return;
    item_path(x, x->sel, clip_path, sizeof clip_path);
    clip_cut = cut;
}

static void cmd_paste(ex_t *x) {
    if (!x->path[0] || !clip_path[0]) return;
    vfs_dirent_t st;
    if (vfs_stat(clip_path, &st) < 0) { msgbox(x->win, "Paste", "The copied item no longer exists.", ICON_WARNING, MB_OK); clip_path[0] = 0; return; }
    char dst[VFS_MAX_PATH];
    const char *base = vfs_basename(clip_path);
    char srcdir[VFS_MAX_PATH];
    vfs_dirname(clip_path, srcdir, sizeof srcdir);
    if (clip_cut && !strcasecmp(srcdir, x->path)) { clip_path[0] = 0; return; }
    if (!clip_cut && !strcasecmp(srcdir, x->path)) {
        char cand[VFS_MAX_NAME + 16];
        snprintf(cand, sizeof cand, "Copy of %s", base);
        fileops_unique_name(x->path, cand, dst, sizeof dst);
    } else {
        fileops_unique_name(x->path, base, dst, sizeof dst);
    }
    wm_set_busy(1);
    int r;
    if (clip_cut && toupper(clip_path[0]) == toupper(x->path[0])) r = vfs_rename(clip_path, dst);
    else {
        r = fileops_copy_tree(clip_path, dst);
        if (r >= 0 && clip_cut) r = fileops_delete_tree(clip_path);
    }
    wm_set_busy(0);
    if (r < 0) report(x, clip_cut ? "Cannot move the item." : "Cannot copy the item.", r);
    if (clip_cut) clip_path[0] = 0;
    load(x);
    select_name(x, vfs_basename(dst));
}

static void cmd_props(ex_t *x) {
    char msg[600], sz[48], szx[48], tm[48];
    if (x->sel < 0) {
        if (!x->path[0]) { app_sysinfo(); return; }
        snprintf(msg, sizeof msg, "Location: %s\nContains: %d items", x->path, x->n);
        msgbox(x->win, "Properties", msg, ICON_FOLDER, MB_OK);
        return;
    }
    item_t *it = &x->items[x->sel];
    if (!x->path[0]) {
        volume_t *v = vfs_volume_by_letter(it->letter);
        char used[48], fr[48], tot[48];
        format_size(it->total - it->free, used, sizeof used);
        format_size(it->free, fr, sizeof fr);
        format_size(it->total, tot, sizeof tot);
        snprintf(msg, sizeof msg, "%s\n\nType: %s\nFile system: %s\nDevice: %s (%s)\n\nUsed space: %s\nFree space: %s\nCapacity: %s",
                 it->name, drive_kind(v), v ? v->fs_name : "?", v && v->bd ? v->bd->model : "?", v && v->bd ? v->bd->name : "?", used, fr, tot);
        msgbox(x->win, "Disk Properties", msg, it->icon, MB_OK);
        return;
    }
    char p[VFS_MAX_PATH];
    item_path(x, x->sel, p, sizeof p);
    vfs_dirent_t st;
    vfs_stat(p, &st);
    format_size(st.size, sz, sizeof sz);
    format_size_exact(st.size, szx, sizeof szx);
    format_time(&st.mtime, tm, sizeof tm);
    char ct[48];
    format_time(&st.ctime, ct, sizeof ct);
    snprintf(msg, sizeof msg, "%s\n\nType: %s\nLocation: %s\n%s%s%s%s\n\nCreated: %s\nModified: %s\nShort name: %s\nAttributes:%s%s%s%s",
             it->name, file_type_name(it->name, it->is_dir), x->path,
             it->is_dir ? "" : "Size: ", it->is_dir ? "" : sz, it->is_dir ? "" : " (", it->is_dir ? "" : szx,
             ct[0] ? ct : "unknown", tm[0] ? tm : "unknown", st.short_name,
             (st.attr & VFS_ATTR_READONLY) ? " Read-only" : "", (st.attr & VFS_ATTR_HIDDEN) ? " Hidden" : "",
             (st.attr & VFS_ATTR_SYSTEM) ? " System" : "", (st.attr & VFS_ATTR_ARCHIVE) ? " Archive" : "");
    if (!it->is_dir) strlcat(msg, "", sizeof msg);
    /* close the "(…bytes" parenthesis for files */
    if (!it->is_dir) {
        char *q = strstr(msg, " bytes\n");
        if (q) { memmove(q + 7, q + 6, strlen(q + 6) + 1); q[6] = ')'; }
    }
    msgbox(x->win, "Properties", msg, it->icon, MB_OK);
}

static void ex_menu(window_t *w, int id) {
    ex_t *x = w->data;
    switch (id) {
    case CMD_OPEN: open_item(x, x->sel); break;
    case CMD_OPEN_NOTEPAD: if (x->sel >= 0 && !x->items[x->sel].is_dir) { char p[VFS_MAX_PATH]; item_path(x, x->sel, p, sizeof p); app_notepad(p); } break;
    case CMD_NEWFOLDER: cmd_new(x, 1); break;
    case CMD_NEWTEXT: cmd_new(x, 0); break;
    case CMD_RENAME: cmd_rename(x); break;
    case CMD_DELETE: cmd_delete(x); break;
    case CMD_COPY: cmd_copy(x, 0); break;
    case CMD_CUT: cmd_copy(x, 1); break;
    case CMD_PASTE: cmd_paste(x); break;
    case CMD_PROPS: cmd_props(x); break;
    case CMD_FORMAT:
        if (!x->path[0] && x->sel >= 0 && x->sel < x->n) {
            int format_dialog(window_t *owner, char letter);
            format_dialog(w, x->items[x->sel].letter);
            load(x);
        }
        break;
    case CMD_CLOSE: wm_close(w); break;
    case CMD_VIEW_ICONS: x->view = 0; x->scroll = 0; wm_invalidate(w); break;
    case CMD_VIEW_DETAILS: x->view = 1; x->scroll = 0; wm_invalidate(w); break;
    case CMD_REFRESH: { int s = x->sel; load(x); x->sel = MIN(s, x->n - 1); break; }
    case CMD_BACK: if (x->nback) { x->nback--; navigate(x, x->back[x->nback], 0); } break;
    case CMD_UP: go_up(x); break;
    case CMD_MYCOMP: navigate(x, "", 1); break;
    case CMD_MYDOCS: navigate(x, "C:/Documents", 1); break;
    case CMD_TERMINAL: app_terminal(); break;
    case CMD_SYSINFO: app_sysinfo(); break;
    case CMD_CONTROL: app_control(); break;
    case CMD_ABOUT: app_about(); break;
    }
}

/* ---------------- layout ---------------- */
static int pane_w(window_t *w) { return w->client->w >= 560 ? PANE_W : 0; }
static int top_y(window_t *w) { return menubar_height(w) + TOOLBAR_H + ADDR_H; }

static void compute_view(window_t *w, ex_t *x) {
    int y = top_y(w);
    x->view_r = mkrect(pane_w(w), y, w->client->w - pane_w(w), w->client->h - y - STATUS_H);
}

static int cols(ex_t *x) {
    int cw = !x->path[0] && x->view == 0 ? TILE_W : CELL_W;
    return MAX(1, (x->view_r.w - 20) / cw);
}

static rect_t item_rect(ex_t *x, int i) {
    rect_t v = x->view_r;
    if (x->view == 1) return mkrect(v.x, v.y + 22 + i * ROW_H - x->scroll, v.w - 17, ROW_H);
    int c = cols(x);
    if (!x->path[0]) return mkrect(v.x + 10 + (i % c) * TILE_W, v.y + 34 + (i / c) * (TILE_H + 8) - x->scroll, TILE_W - 10, TILE_H);
    return mkrect(v.x + 6 + (i % c) * CELL_W, v.y + 8 + (i / c) * CELL_H - x->scroll, CELL_W - 4, CELL_H - 4);
}

static int content_height(ex_t *x) {
    if (x->view == 1) return 22 + x->n * ROW_H + 4;
    int c = cols(x), rows = (x->n + c - 1) / c;
    if (!x->path[0]) return 34 + rows * (TILE_H + 8) + 8;
    return 8 + rows * CELL_H + 8;
}

static int max_scroll(ex_t *x) { return MAX(0, content_height(x) - x->view_r.h); }

static void ensure_visible(ex_t *x, int i) {
    if (i < 0) return;
    rect_t r = item_rect(x, i);
    int top = x->view_r.y + (x->view == 1 ? 22 : 0);
    if (r.y < top) x->scroll -= top - r.y;
    if (r.y + r.h > x->view_r.y + x->view_r.h) x->scroll += r.y + r.h - (x->view_r.y + x->view_r.h);
    x->scroll = CLAMP(x->scroll, 0, max_scroll(x));
}

/* ---------------- painting ---------------- */
static void draw_arrow_btn(gc_t *g, int x, int y, int dir, int enabled) {
    /* round button with an arrow: dir 0 = left, 1 = up */
    color_t top = enabled ? HEX(0x8FD883) : HEX(0xD8D8D0), bot = enabled ? HEX(0x2E9A3A) : HEX(0xB0B0A8);
    gfx_rounded(g, mkrect(x, y, 26, 26), 13, 15, top, bot);
    gfx_rounded_outline(g, mkrect(x, y, 26, 26), 13, 15, enabled ? HEX(0x1F6A26) : HEX(0x9A9A90));
    color_t c = HEX(0xFFFFFF);
    int cx = x + 13, cy = y + 13;
    if (dir == 0) {
        for (int i = 0; i < 6; i++) gfx_vline(g, cx - 6 + i, cy - i, 2 * i + 1, c);
        gfx_fill(g, mkrect(cx, cy - 2, 7, 5), c);
    } else {
        for (int i = 0; i < 6; i++) gfx_hline(g, cx - i, cy - 6 + i, 2 * i + 1, c);
        gfx_fill(g, mkrect(cx - 2, cy, 5, 7), c);
    }
}

static rect_t tb_rect(int i) {
    static const int xs[] = { 6, 84, 152, 238 };
    static const int ws[] = { 74, 64, 82, 82 };
    return mkrect(xs[i], 0, ws[i], TOOLBAR_H - 4);
}

static void paint_toolbar(window_t *w, ex_t *x, gc_t *g) {
    int y0 = menubar_height(w);
    gfx_gradient_v(g, mkrect(0, y0, w->client->w, TOOLBAR_H), color_lighten(g_theme->face, 50), g_theme->face);
    gfx_hline(g, 0, y0 + TOOLBAR_H - 1, w->client->w, color_lighten(g_theme->face, -25));
    static const char *labels[] = { "Back", "Up", "Folders", "Views" };
    for (int i = 0; i < 4; i++) {
        rect_t r = tb_rect(i);
        r.y += y0 + 2;
        int en = i == 0 ? x->nback > 0 : i == 1 ? x->path[0] != 0 : 1;
        if (en && x->tb_hover == i) {
            gfx_rounded(g, r, 3, 15, HEX(0xFFFFFF), color_lighten(g_theme->face, -15));
            gfx_rounded_outline(g, r, 3, 15, color_lighten(g_theme->face, -60));
        }
        if (i < 2) draw_arrow_btn(g, r.x + 3, r.y + 4, i, en);
        else gfx_icon(g, r.x + 6, r.y + 8, &icons16[i == 2 ? ICON_FOLDER_OPEN : (x->view ? ICON_FILE_TEXT : ICON_FILE)]);
        gfx_text_box(g, &font_ui, mkrect(r.x + (i < 2 ? 32 : 26), r.y, r.w - 30, r.h), i == 3 ? (x->view ? "Details" : "Icons") : labels[i],
                     en ? HEX(0x000000) : HEX(0xA0A0A0), 0);
    }
    /* address bar */
    int ay = y0 + TOOLBAR_H;
    gfx_fill(g, mkrect(0, ay, w->client->w, ADDR_H), g_theme->face);
    gfx_text_box(g, &font_ui, mkrect(8, ay, 50, ADDR_H), "Address", HEX(0x6D6D6D), 0);
    gfx_hline(g, 0, ay + ADDR_H - 1, w->client->w, color_lighten(g_theme->face, -25));
}

static void add_link(ex_t *x, rect_t r, int id) {
    if (x->nlinks < MAX_LINKS) { x->links[x->nlinks].r = r; x->links[x->nlinks].id = id; x->nlinks++; }
}

static int pane_box(gc_t *g, ex_t *x, int y, int wdt, const char *title, int nlines, const char **lines, const int *icons, const int *ids) {
    rect_t head = mkrect(12, y, wdt - 24, 24);
    gfx_rounded(g, head, 5, 3, HEX(0xFFFFFF), color_lighten(g_theme->sidebar_top, 120));
    gfx_text_box(g, &font_ui_bold, mkrect(head.x + 10, head.y, head.w - 12, head.h), title, color_lighten(g_theme->menu_header_bottom, -40), 0);
    int bh = nlines * 20 + 12;
    rect_t body = mkrect(head.x, y + 24, head.w, bh);
    gfx_fill(g, body, color_lighten(g_theme->sidebar_top, 165));
    gfx_rect(g, body, HEX(0xFFFFFF));
    for (int i = 0; i < nlines; i++) {
        rect_t lr = mkrect(body.x + 8, body.y + 6 + i * 20, body.w - 12, 20);
        int hov = ids && x->link_hover == ids[i];
        if (icons && icons[i] >= 0) gfx_icon(g, lr.x, lr.y + 2, &icons16[icons[i]]);
        int tx = lr.x + (icons && icons[i] >= 0 ? 22 : 0);
        color_t tc = ids ? color_lighten(g_theme->menu_header_bottom, hov ? 40 : -20) : HEX(0x202020);
        gfx_text_box(g, &font_ui, mkrect(tx, lr.y, lr.w - (tx - lr.x), 20), lines[i], tc, 0);
        if (hov) gfx_hline(g, tx, lr.y + 15, gfx_text_width(&font_ui, lines[i]), tc);
        if (ids) add_link(x, lr, ids[i]);
    }
    return y + 24 + bh + 12;
}

static void paint_pane(window_t *w, ex_t *x, gc_t *g) {
    int pw = pane_w(w);
    if (!pw) return;
    int y0 = top_y(w);
    rect_t r = mkrect(0, y0, pw, w->client->h - y0 - STATUS_H);
    gfx_gradient_v(g, r, g_theme->sidebar_top, g_theme->sidebar_bottom);
    gc_t sub = gc_sub(g, r);
    x->nlinks = 0;
    int y = 12;
    /* links are recorded in pane-local coordinates; offset them afterwards */
    int first_link = 0;
    if (!x->path[0]) {
        const char *l[] = { "View system information", "Change a setting", "Open Command Prompt" };
        const int ic[] = { ICON_SYSINFO, ICON_CONTROL, ICON_TERMINAL };
        const int id[] = { CMD_SYSINFO, CMD_CONTROL, CMD_TERMINAL };
        y = pane_box(&sub, x, y, pw, "System Tasks", 3, l, ic, id);
    } else {
        if (x->sel >= 0) {
            const char *l[] = { "Rename this item", "Copy this item", "Delete this item", "Make a new folder" };
            const int ic[] = { ICON_FILE_TEXT, ICON_DOCUMENTS, ICON_RECYCLE, ICON_FOLDER };
            const int id[] = { CMD_RENAME, CMD_COPY, CMD_DELETE, CMD_NEWFOLDER };
            y = pane_box(&sub, x, y, pw, "File and Folder Tasks", 4, l, ic, id);
        } else {
            const char *l[] = { "Make a new folder", "New text document", clip_path[0] ? "Paste here" : "Open Command Prompt" };
            const int ic[] = { ICON_FOLDER, ICON_FILE_TEXT, clip_path[0] ? ICON_DOCUMENTS : ICON_TERMINAL };
            const int id[] = { CMD_NEWFOLDER, CMD_NEWTEXT, clip_path[0] ? CMD_PASTE : CMD_TERMINAL };
            y = pane_box(&sub, x, y, pw, "File and Folder Tasks", 3, l, ic, id);
        }
    }
    {
        const char *l[] = { "My Computer", "My Documents", "Control Panel" };
        const int ic[] = { ICON_COMPUTER, ICON_DOCUMENTS, ICON_CONTROL };
        const int id[] = { CMD_MYCOMP, CMD_MYDOCS, CMD_CONTROL };
        y = pane_box(&sub, x, y, pw, "Other Places", 3, l, ic, id);
    }
    for (int i = first_link; i < x->nlinks; i++) { x->links[i].r.x += r.x; x->links[i].r.y += r.y; }
    /* details box */
    char l1[VFS_MAX_NAME + 1], l2[64], l3[64], l4[64];
    const char *lines[4] = { l1, l2, l3, l4 };
    int n = 0;
    if (x->sel >= 0 && x->sel < x->n) {
        item_t *it = &x->items[x->sel];
        strlcpy(l1, it->name, sizeof l1); n = 1;
        if (!x->path[0]) {
            volume_t *v = vfs_volume_by_letter(it->letter);
            snprintf(l2, sizeof l2, "%s, %s", drive_kind(v), v ? v->fs_name : "");
            char a[32], b[32];
            format_size(it->free, a, sizeof a);
            format_size(it->total, b, sizeof b);
            snprintf(l3, sizeof l3, "Free Space: %s", a);
            snprintf(l4, sizeof l4, "Total Size: %s", b);
            n = 4;
        } else {
            strlcpy(l2, file_type_name(it->name, it->is_dir), sizeof l2);
            n = 2;
            if (!it->is_dir) { char s[32]; format_size(it->size, s, sizeof s); snprintf(l3, sizeof l3, "Size: %s", s); n = 3; }
            char t[48]; format_time(&it->mtime, t, sizeof t);
            if (t[0]) { snprintf(n == 3 ? l4 : l3, 64, "Modified: %s", t); n++; }
        }
    } else if (x->path[0]) {
        strlcpy(l1, strlen(x->path) <= 3 ? x->path : vfs_basename(x->path), sizeof l1);
        strlcpy(l2, "File Folder", sizeof l2);
        n = 2;
    } else {
        strlcpy(l1, "My Computer", sizeof l1);
        strlcpy(l2, "System Folder", sizeof l2);
        n = 2;
    }
    pane_box(&sub, x, y, pw, "Details", n, lines, NULL, NULL);
}

static void paint_view(window_t *w, ex_t *x, gc_t *g) {
    rect_t v = x->view_r;
    gfx_fill(g, v, HEX(0xFFFFFF));
    gc_t sub = gc_sub(g, mkrect(v.x, v.y, v.w - 17, v.h));
    gc_t *gg = &sub;
    /* sub has origin at v; draw with coordinates relative to client by offsetting */
    sub.ox -= v.x; sub.oy -= v.y;
    const theme_t *th = g_theme;
    if (x->view == 1) {
        /* details */
        int cw = v.w - 17;
        int c1 = MAX(160, cw - 330), c2 = 80, c3 = 120;
        for (int i = 0; i < x->n; i++) {
            rect_t r = item_rect(x, i);
            if (r.y + r.h < v.y + 22 || r.y > v.y + v.h) continue;
            item_t *it = &x->items[i];
            int sel = i == x->sel;
            if (sel) gfx_fill(gg, mkrect(r.x + 2, r.y, c1 - 4, r.h), th->select);
            else if (i == x->hover) gfx_fill(gg, mkrect(r.x + 2, r.y, c1 - 4, r.h), color_lighten(th->select, 200));
            gfx_icon(gg, r.x + 6, r.y + 1, &icons16[it->icon]);
            gfx_text_box(gg, &font_ui, mkrect(r.x + 26, r.y, c1 - 30, r.h), it->name, sel ? th->select_text : HEX(0x000000), 0);
            char buf[64];
            if (!x->path[0]) { format_size(it->total, buf, sizeof buf); }
            else if (it->is_dir) buf[0] = 0;
            else format_size(it->size, buf, sizeof buf);
            gfx_text_box(gg, &font_ui, mkrect(r.x + c1, r.y, c2 - 8, r.h), buf, HEX(0x000000), 2);
            gfx_text_box(gg, &font_ui, mkrect(r.x + c1 + c2 + 8, r.y, c3 - 8, r.h),
                         x->path[0] ? file_type_name(it->name, it->is_dir) : drive_kind(vfs_volume_by_letter(it->letter)), HEX(0x000000), 0);
            if (!x->path[0]) { format_size(it->free, buf, sizeof buf); }
            else format_time(&it->mtime, buf, sizeof buf);
            gfx_text_box(gg, &font_ui, mkrect(r.x + c1 + c2 + c3 + 8, r.y, cw - (c1 + c2 + c3 + 8), r.h), buf, HEX(0x000000), 0);
        }
        /* header */
        rect_t hr = mkrect(v.x, v.y, cw, 21);
        gfx_gradient_v(gg, hr, HEX(0xFFFFFF), HEX(0xEBEADB));
        gfx_hline(gg, hr.x, hr.y + 20, hr.w, HEX(0xD6D2C2));
        const char *hdr[4] = { "Name", "Size", "Type", x->path[0] ? "Date Modified" : "Free Space" };
        int xs[5] = { 0, c1, c1 + c2, c1 + c2 + c3, cw };
        for (int i = 0; i < 4; i++) {
            gfx_text_box(gg, &font_ui, mkrect(hr.x + xs[i] + 8, hr.y, xs[i + 1] - xs[i] - 12, 21), hdr[i], HEX(0x000000), i == 1 ? 2 : 0);
            gfx_vline(gg, hr.x + xs[i + 1] - 1, hr.y + 3, 15, HEX(0xD6D2C2));
        }
    } else if (!x->path[0]) {
        /* My Computer tiles */
        gfx_text(gg, &font_ui_bold, v.x + 12, v.y + 10 - x->scroll, "Files Stored on This Computer", color_lighten(th->menu_header_bottom, -40));
        gfx_gradient_h(gg, mkrect(v.x + 12, v.y + 26 - x->scroll, v.w - 60, 1), th->sidebar_bottom, HEX(0xFFFFFF));
        for (int i = 0; i < x->n; i++) {
            rect_t r = item_rect(x, i);
            item_t *it = &x->items[i];
            int sel = i == x->sel;
            if (sel) gfx_rounded(gg, r, 3, 15, color_lighten(th->select, 170), color_lighten(th->select, 140));
            else if (i == x->hover) gfx_rounded(gg, r, 3, 15, color_lighten(th->select, 215), color_lighten(th->select, 200));
            gfx_icon(gg, r.x + 6, r.y + 6, &icons48[it->icon]);
            gfx_text_box(gg, &font_ui_bold, mkrect(r.x + 60, r.y + 4, r.w - 64, 16), it->name, HEX(0x000000), 0);
            char a[32], b[32], line[80];
            format_size(it->free, a, sizeof a);
            format_size(it->total, b, sizeof b);
            snprintf(line, sizeof line, "%s free of %s", a, b);
            gfx_text_box(gg, &font_ui, mkrect(r.x + 60, r.y + 20, r.w - 64, 16), line, HEX(0x6D6D6D), 0);
            rect_t bar = mkrect(r.x + 60, r.y + 40, r.w - 70, 10);
            gfx_fill(gg, bar, HEX(0xFFFFFF));
            gfx_rect(gg, bar, HEX(0x8C8C8C));
            int used = it->total ? (int)((it->total - it->free) * (u64)(bar.w - 2) / it->total) : 0;
            gfx_gradient_v(gg, mkrect(bar.x + 1, bar.y + 1, used, bar.h - 2), HEX(0x74A9F4), HEX(0x2860C8));
        }
        if (!x->n) gfx_text(gg, &font_ui, v.x + 14, v.y + 40, "No drives found.", HEX(0x6D6D6D));
    } else {
        for (int i = 0; i < x->n; i++) {
            rect_t r = item_rect(x, i);
            if (r.y + r.h < v.y || r.y > v.y + v.h) continue;
            item_t *it = &x->items[i];
            int sel = i == x->sel;
            int ix = r.x + (r.w - 32) / 2;
            if (sel) gfx_icon_tinted(gg, ix, r.y + 4, &icons32[it->icon], th->select, 100);
            else gfx_icon(gg, ix, r.y + 4, &icons32[it->icon]);
            /* name: up to two lines */
            const char *nm = it->name;
            int len = (int)strlen(nm), maxw = r.w - 4;
            int n1 = len, w1 = gfx_text_width(&font_ui, nm);
            if (w1 > maxw) {
                int acc = 0, lastsp = -1;
                n1 = 0;
                while (n1 < len && acc + gfx_char_width(&font_ui, nm[n1]) <= maxw) { if (nm[n1] == ' ') lastsp = n1; acc += gfx_char_width(&font_ui, nm[n1]); n1++; }
                if (lastsp > 0 && n1 < len) n1 = lastsp + 1;
            }
            char l1[VFS_MAX_NAME + 1], l2[VFS_MAX_NAME + 1] = "";
            memcpy(l1, nm, n1); l1[n1] = 0;
            while (n1 > 0 && l1[n1 - 1] == ' ') l1[--n1] = 0;
            if ((int)strlen(l1) < len) strlcpy(l2, nm + strlen(l1) + (nm[strlen(l1)] == ' '), sizeof l2);
            int ty = r.y + 40;
            for (int line = 0; line < 2; line++) {
                const char *t = line ? l2 : l1;
                if (!*t) continue;
                int tw = MIN(gfx_text_width(&font_ui, t), maxw);
                rect_t tr = mkrect(r.x + (r.w - tw) / 2, ty + line * 13, tw, 13);
                if (sel) gfx_fill(gg, mkrect(tr.x - 2, tr.y, tr.w + 4, 13), th->select);
                gfx_text_box(gg, &font_ui, tr, t, sel ? th->select_text : HEX(0x000000), 1);
            }
        }
        if (!x->n) gfx_text(gg, &font_ui, v.x + 14, v.y + 14, "This folder is empty.", HEX(0x6D6D6D));
    }
    draw_scrollbar(g, mkrect(v.x + v.w - 17, v.y, 17, v.h), content_height(x), v.h, x->scroll, 1);
}

static void ex_paint(window_t *w, gc_t *g) {
    ex_t *x = w->data;
    compute_view(w, x);
    x->scroll = CLAMP(x->scroll, 0, max_scroll(x));
    paint_toolbar(w, x, g);
    paint_pane(w, x, g);
    paint_view(w, x, g);
    /* status bar */
    rect_t sb = mkrect(0, w->client->h - STATUS_H, w->client->w, STATUS_H);
    gfx_fill(g, sb, g_theme->face);
    gfx_hline(g, 0, sb.y, sb.w, color_lighten(g_theme->face, -30));
    char st[160];
    if (!x->path[0]) snprintf(st, sizeof st, "%d drive%s", x->n, x->n == 1 ? "" : "s");
    else {
        u64 total = 0, fr = 0;
        vfs_statfs(x->path[0], &total, &fr);
        char f[32]; format_size(fr, f, sizeof f);
        if (x->sel >= 0) {
            char s[32]; format_size(x->items[x->sel].size, s, sizeof s);
            snprintf(st, sizeof st, "Selected: %s%s%s    (Disk free space: %s)", x->items[x->sel].name, x->items[x->sel].is_dir ? "" : ", ",
                     x->items[x->sel].is_dir ? "" : s, f);
        } else snprintf(st, sizeof st, "%d object%s    (Disk free space: %s)", x->n, x->n == 1 ? "" : "s", f);
    }
    gfx_text_box(g, &font_ui, mkrect(8, sb.y + 1, sb.w - 16, STATUS_H - 1), st, HEX(0x000000), 0);
}

/* ---------------- events ---------------- */
static int item_at(ex_t *x, int px, int py) {
    if (!rect_contains(x->view_r, px, py) || px >= x->view_r.x + x->view_r.w - 17) return -1;
    if (x->view == 1 && py < x->view_r.y + 22) return -1;
    for (int i = 0; i < x->n; i++) {
        rect_t r = item_rect(x, i);
        if (x->view == 1) r.w = MAX(160, x->view_r.w - 17 - 330);
        if (rect_contains(r, px, py)) return i;
    }
    return -1;
}

static void addr_go(window_t *w, widget_t *wd) {
    ex_t *x = w->data;
    navigate(x, wd->text, 1);
}
static void go_click(window_t *w, widget_t *wd) { addr_go(w, ((ex_t *)w->data)->addr); }

static void ex_event(window_t *w, gui_event_t *e) {
    ex_t *x = w->data;
    int y0 = menubar_height(w);
    if (e->type == GE_MOUSE_MOVE || e->type == GE_MOUSE_LEAVE) {
        if (x->sb_drag && (e->buttons & MOUSE_LEFT)) {
            rect_t v = x->view_r;
            int track = v.h - 34;
            x->scroll = CLAMP((e->y - v.y - 17) * max_scroll(x) / MAX(1, track), 0, max_scroll(x));
            wm_invalidate(w);
            return;
        }
        int h = item_at(x, e->x, e->y), lh = -1, th = -1;
        for (int i = 0; i < x->nlinks; i++) if (rect_contains(x->links[i].r, e->x, e->y)) lh = x->links[i].id;
        for (int i = 0; i < 4; i++) { rect_t r = tb_rect(i); r.y += y0 + 2; if (rect_contains(r, e->x, e->y)) th = i; }
        if (h != x->hover || lh != x->link_hover || th != x->tb_hover) {
            x->hover = h; x->link_hover = lh; x->tb_hover = th;
            w->cursor = lh >= 0 ? CURSOR_HAND : CURSOR_ARROW;
            wm_invalidate(w);
        }
        return;
    }
    if (e->type == GE_MOUSE_UP) { x->sb_drag = 0; return; }
    if (e->type == GE_WHEEL) {
        x->scroll = CLAMP(x->scroll - e->dz * 57, 0, max_scroll(x));
        wm_invalidate(w);
        return;
    }
    if (e->type == GE_MOUSE_DOWN || e->type == GE_DBLCLICK) {
        /* toolbar */
        for (int i = 0; i < 4; i++) {
            rect_t r = tb_rect(i); r.y += y0 + 2;
            if (rect_contains(r, e->x, e->y) && e->button == MOUSE_LEFT) {
                if (i == 0) ex_menu(w, CMD_BACK);
                else if (i == 1) ex_menu(w, CMD_UP);
                else if (i == 2) ex_menu(w, CMD_MYCOMP);
                else ex_menu(w, x->view ? CMD_VIEW_ICONS : CMD_VIEW_DETAILS);
                return;
            }
        }
        for (int i = 0; i < x->nlinks; i++)
            if (rect_contains(x->links[i].r, e->x, e->y) && e->button == MOUSE_LEFT) { ex_menu(w, x->links[i].id); return; }
        rect_t v = x->view_r;
        if (rect_contains(mkrect(v.x + v.w - 17, v.y, 17, v.h), e->x, e->y)) {
            int part;
            x->scroll = scrollbar_hit(mkrect(v.x + v.w - 17, v.y, 17, v.h), content_height(x), v.h, x->scroll, e->x, e->y, 1, &part);
            x->scroll = CLAMP(x->scroll, 0, max_scroll(x));
            if (part == 1 || part == 2) x->scroll = CLAMP(x->scroll + (part == 1 ? -40 : 40) + (part == 1 ? 3 : -3), 0, max_scroll(x));
            if (part == 5) x->sb_drag = 1;
            wm_invalidate(w);
            return;
        }
        if (!rect_contains(v, e->x, e->y)) return;
        int i = item_at(x, e->x, e->y);
        if (i != x->sel) { x->sel = i; wm_invalidate(w); }
        if (e->type == GE_DBLCLICK && e->button == MOUSE_LEFT && i >= 0) { open_item(x, i); return; }
        if (e->button == MOUSE_RIGHT) {
            rect_t c = wm_client_rect(w);
            if (i >= 0) menu_popup(w, x->path[0] ? &ctx_file : &ctx_drive, c.x + e->x, c.y + e->y);
            else if (x->path[0]) menu_popup(w, &ctx_bg, c.x + e->x, c.y + e->y);
        }
        return;
    }
    if (e->type == GE_KEY && e->key.pressed) {
        input_event_t *k = &e->key;
        if (w->focus == x->addr && k->key != KEY_F5) return;   /* typing in the address bar */
        int c = x->view == 1 ? 1 : cols(x), ns = x->sel;
        switch (k->key) {
        case KEY_ENTER: open_item(x, x->sel); return;
        case KEY_BACKSPACE: go_up(x); return;
        case KEY_DELETE: cmd_delete(x); return;
        case KEY_F2: cmd_rename(x); return;
        case KEY_F5: ex_menu(w, CMD_REFRESH); return;
        case KEY_LEFT: if (k->mods & MOD_ALT) { ex_menu(w, CMD_BACK); return; } ns = x->view == 1 ? ns : ns - 1; break;
        case KEY_RIGHT: ns = x->view == 1 ? ns : ns + 1; break;
        case KEY_UP: ns -= c; break;
        case KEY_DOWN: ns = ns < 0 ? 0 : ns + c; break;
        case KEY_HOME: ns = 0; break;
        case KEY_END: ns = x->n - 1; break;
        default:
            if (k->mods & MOD_CTRL) {
                if (k->key == KEY_A + 2) cmd_copy(x, 0);
                else if (k->key == KEY_A + 23) cmd_copy(x, 1);
                else if (k->key == KEY_A + 21) cmd_paste(x);
                return;
            }
            if (k->ch > 32 && x->n) {
                /* jump to the next item starting with the typed letter */
                for (int j = 1; j <= x->n; j++) {
                    int idx = (x->sel + j + x->n) % x->n;
                    if (tolower(x->items[idx].name[0]) == tolower(k->ch)) { ns = idx; break; }
                }
            }
            break;
        }
        if (x->n) ns = CLAMP(ns, 0, x->n - 1); else ns = -1;
        if (ns != x->sel) { x->sel = ns; ensure_visible(x, ns); wm_invalidate(w); }
    }
}

static void ex_tick(window_t *w) {
    ex_t *x = w->data;
    if (vfs_generation() != x->vgen) {
        /* drives changed: if our drive vanished go to My Computer */
        if (x->path[0] && !vfs_volume_by_letter(x->path[0])) x->path[0] = 0;
        int s = x->sel;
        load(x);
        x->sel = MIN(s, x->n - 1);
    }
}

static void ex_resize(window_t *w) {
    ex_t *x = w->data;
    if (x->addr) {
        x->addr->r.w = w->client->w - 62 - 50;
        widget_t *go = widget_find(w, 2);
        if (go) go->r.x = w->client->w - 46;
    }
}

static void ex_destroy(window_t *w) {
    ex_t *x = w->data;
    kfree(x->items);
    kfree(x);
}

void app_explorer(const char *path) {
    window_t *w = wm_create("My Computer", ICON_COMPUTER, -1, -1, 700, 480, WF_RESIZABLE);
    w->min_w = 360; w->min_h = 240;
    ex_t *x = kzalloc(sizeof *x);
    x->win = w;
    x->sel = -1; x->hover = -1; x->link_hover = -1; x->tb_hover = -1;
    w->data = x;
    w->on_paint = ex_paint;
    w->on_event = ex_event;
    w->on_menu = ex_menu;
    w->on_tick = ex_tick;
    w->on_resize = ex_resize;
    w->on_destroy = ex_destroy;
    menubar_attach(w, ex_menus, ARRAY_SIZE(ex_menus));
    int ay = MENUBAR_H + TOOLBAR_H + 3;
    x->addr = widget_add(w, W_TEXTBOX, mkrect(62, ay, w->client->w - 62 - 50, 22), "", 1);
    x->addr->on_activate = addr_go;
    widget_t *go = widget_add(w, W_BUTTON, mkrect(w->client->w - 46, ay, 40, 22), "Go", 2);
    go->on_click = go_click;
    w->focus = NULL;
    navigate(x, path ? path : "", 0);
}
