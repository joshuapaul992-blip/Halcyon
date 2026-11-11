/* Halcyon OS - Notepad text editor */
#include "gui/wm.h"
#include "core/lib.h"
#include "core/unicode.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "fs/vfs.h"

#define STATUS_H 20
#define SB_W 17
#define MAX_FILE (4 * 1024 * 1024)

typedef struct np {
    char *buf; int len, cap;
    int cur, anchor;
    int top, left;              /* first visible line, horizontal scroll in px */
    int *lines; int nlines, lcap;
    char path[VFS_MAX_PATH];
    int modified, crlf;
    int pref_x;
    int caret_on;
    u64 caret_t;
    int sb_drag, mouse_sel;
    window_t *win;
} np_t;

enum { NP_NEW = 1, NP_OPEN, NP_SAVE, NP_SAVEAS, NP_EXIT, NP_CUT, NP_COPY, NP_PASTE, NP_DEL, NP_SELALL, NP_TIME, NP_ABOUT, NP_FIND };

static menu_item_t f_items[] = {
    { "New", "Ctrl+N", NP_NEW, 0, 0 }, { "Open...", "Ctrl+O", NP_OPEN, 0, 0 }, { "Save", "Ctrl+S", NP_SAVE, 0, 0 },
    { "Save As...", NULL, NP_SAVEAS, 0, 0 }, { "", NULL, 0, MI_SEPARATOR, 0 }, { "Exit", NULL, NP_EXIT, 0, 0 },
};
static menu_item_t e_items[] = {
    { "Cut", "Ctrl+X", NP_CUT, 0, 0 }, { "Copy", "Ctrl+C", NP_COPY, 0, 0 }, { "Paste", "Ctrl+V", NP_PASTE, 0, 0 },
    { "Delete", "Del", NP_DEL, 0, 0 }, { "", NULL, 0, MI_SEPARATOR, 0 }, { "Find...", "Ctrl+F", NP_FIND, 0, 0 },
    { "", NULL, 0, MI_SEPARATOR, 0 },
    { "Select All", "Ctrl+A", NP_SELALL, 0, 0 }, { "Time/Date", "F5", NP_TIME, 0, 0 },
};
static menu_item_t h_items[] = { { "About Notepad", NULL, NP_ABOUT, 0, ICON_NOTEPAD } };
static menu_t np_menus[] = {
    { "File", f_items, ARRAY_SIZE(f_items) }, { "Edit", e_items, ARRAY_SIZE(e_items) }, { "Help", h_items, ARRAY_SIZE(h_items) },
};

static int cw(void) { return gfx_char_width(&font_mono, 'M'); }
static int lh(void) { return font_mono.height + 1; }

/* ---------------- buffer ---------------- */
static void relines(np_t *n) {
    int count = 1;
    for (int i = 0; i < n->len; i++) if (n->buf[i] == '\n') count++;
    if (count > n->lcap) {
        kfree(n->lines);
        n->lcap = count + 256;
        n->lines = kmalloc(sizeof(int) * n->lcap);
    }
    int k = 0;
    n->lines[k++] = 0;
    for (int i = 0; i < n->len; i++) if (n->buf[i] == '\n') n->lines[k++] = i + 1;
    n->nlines = k;
}

static int line_of(np_t *n, int off) {
    int lo = 0, hi = n->nlines - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (n->lines[mid] <= off) lo = mid; else hi = mid - 1;
    }
    return lo;
}
static int line_end(np_t *n, int l) { return l + 1 < n->nlines ? n->lines[l + 1] - 1 : n->len; }

/* The text is UTF-8 (other bytes show as Latin-1): one column per character,
 * tabs to multiples of 4. Offsets are bytes at character boundaries. */
static int next_ch(np_t *n, int i) { return utf8_after(n->buf, n->len, i); }
static int prev_ch(np_t *n, int i) { return utf8_prev(n->buf, i); }

static int col_of(np_t *n, int off) {
    int l = line_of(n, off), col = 0;
    for (int i = n->lines[l]; i < off; i = next_ch(n, i)) col = n->buf[i] == '\t' ? (col / 4 + 1) * 4 : col + 1;
    return col;
}
static int x_of(np_t *n, int off) { return col_of(n, off) * cw(); }
static int off_at_x(np_t *n, int l, int px) {
    int col = 0, e = line_end(n, l);
    for (int i = n->lines[l]; i < e; i = next_ch(n, i)) {
        int nc = n->buf[i] == '\t' ? (col / 4 + 1) * 4 : col + 1;
        if (px < (col * cw() + nc * cw()) / 2) return i;
        col = nc;
    }
    return e;
}

static int reserve(np_t *n, int extra) {
    if (n->len + extra + 1 <= n->cap) return 0;
    int nc = MAX(n->cap * 2, n->len + extra + 4096);
    char *nb = kmalloc(nc);
    if (!nb) return -1;
    memcpy(nb, n->buf, n->len);
    kfree(n->buf);
    n->buf = nb; n->cap = nc;
    return 0;
}

static void del_range(np_t *n, int a, int b) {
    if (a > b) { int t = a; a = b; b = t; }
    if (a == b) return;
    memmove(n->buf + a, n->buf + b, n->len - b);
    n->len -= b - a;
    n->cur = n->anchor = a;
    n->modified = 1;
    relines(n);
}
static int del_sel(np_t *n) {
    if (n->cur == n->anchor) return 0;
    del_range(n, n->cur, n->anchor);
    return 1;
}
static void insert(np_t *n, const char *s, int len) {
    del_sel(n);
    if (n->len + len > MAX_FILE || reserve(n, len) < 0) return;
    memmove(n->buf + n->cur + len, n->buf + n->cur, n->len - n->cur);
    memcpy(n->buf + n->cur, s, len);
    n->len += len;
    n->cur += len;
    n->anchor = n->cur;
    n->modified = 1;
    relines(n);
}

static void update_title(np_t *n) {
    char t[VFS_MAX_PATH + 32];
    snprintf(t, sizeof t, "%s%s - Notepad", n->modified ? "*" : "", n->path[0] ? vfs_basename(n->path) : "Untitled");
    if (strcmp(t, n->win->title)) wm_set_title(n->win, t);
}

/* ---------------- view ---------------- */
static rect_t text_rect(window_t *w) {
    int y = menubar_height(w);
    return mkrect(0, y, w->client->w - SB_W, w->client->h - y - STATUS_H);
}
static int vis_lines(window_t *w) { return MAX(1, (text_rect(w).h - 4) / lh()); }

static void scroll_to_caret(np_t *n) {
    window_t *w = n->win;
    int l = line_of(n, n->cur), vl = vis_lines(w);
    if (l < n->top) n->top = l;
    if (l >= n->top + vl) n->top = l - vl + 1;
    int x = x_of(n, n->cur), tw = text_rect(w).w - 8;
    if (x < n->left) n->left = MAX(0, x - tw / 3);
    if (x > n->left + tw - cw()) n->left = x - tw + tw / 3;
    n->caret_on = 1; n->caret_t = time_ms();
}

static void np_paint(window_t *w, gc_t *g) {
    np_t *n = w->data;
    rect_t tr = text_rect(w);
    gfx_fill(g, tr, HEX(0xFFFFFF));
    gc_t sub = gc_sub(g, rect_inset(tr, 2));
    int vl = vis_lines(w) + 1;
    int s0 = MIN(n->cur, n->anchor), s1 = MAX(n->cur, n->anchor);
    int active = wm_active() == w;
    for (int i = 0; i < vl && n->top + i < n->nlines; i++) {
        int l = n->top + i, y = i * lh() + 1;
        int a = n->lines[l], e = line_end(n, l);
        int col = 0;
        for (int k = a; k <= e;) {
            int x = col * cw() - n->left + 2;
            int nc = (k < e && n->buf[k] == '\t') ? (col / 4 + 1) * 4 : col + 1;
            int k2 = k < e ? next_ch(n, k) : k + 1;
            int sel = k >= s0 && k < s1;
            if (sel) gfx_fill(&sub, mkrect(x, y, (nc - col) * cw(), lh()), active ? g_theme->select : HEX(0xC8C8C8));
            if (k < e && n->buf[k] != '\t' && x > -cw() && x < tr.w)
                gfx_text_n(&sub, &font_mono, x, y, n->buf + k, k2 - k, sel && active ? g_theme->select_text : HEX(0x000000));
            col = nc;
            if (x > tr.w && k >= s1) break;
            k = k2;
        }
    }
    if (active && n->caret_on) {
        int l = line_of(n, n->cur);
        if (l >= n->top && l < n->top + vl) {
            int x = x_of(n, n->cur) - n->left + 2;
            gfx_fill(&sub, mkrect(x, (l - n->top) * lh() + 1, 2, lh()), HEX(0x000000));
        }
    }
    gfx_rect(g, tr, HEX(0x7F9DB9));
    draw_scrollbar(g, mkrect(tr.x + tr.w, tr.y, SB_W, tr.h), n->nlines + vl - 2, vl - 1, n->top, 1);
    /* status bar */
    rect_t sb = mkrect(0, w->client->h - STATUS_H, w->client->w, STATUS_H);
    gfx_hline(g, 0, sb.y, sb.w, color_lighten(g_theme->face, -30));
    int l = line_of(n, n->cur);
    char st[96];
    snprintf(st, sizeof st, "Ln %d, Col %d", l + 1, utf8_count(n->buf + n->lines[l], n->cur - n->lines[l]) + 1);
    gfx_text_box(g, &font_ui, mkrect(sb.w - 160, sb.y + 1, 150, STATUS_H - 1), st, HEX(0x000000), 0);
    char sz[48];
    snprintf(sz, sizeof sz, "%d bytes, %d lines%s", n->len, n->nlines, n->crlf ? ", CR/LF" : "");
    gfx_text_box(g, &font_ui, mkrect(8, sb.y + 1, sb.w - 180, STATUS_H - 1), sz, HEX(0x505050), 0);
    gfx_vline(g, sb.w - 168, sb.y + 3, STATUS_H - 5, color_lighten(g_theme->face, -30));
}

/* ---------------- files ---------------- */
static int load_file(np_t *n, const char *path) {
    char *data; u32 size;
    int r = vfs_read_file(path, &data, &size);
    if (r < 0) return r;
    if (size > MAX_FILE) { kfree(data); return E_NOSPC; }
    kfree(n->buf);
    n->cap = (int)size + 4096;
    n->buf = kmalloc(n->cap);
    int o = 0, cr = 0;
    for (u32 i = 0; i < size; i++) {
        char c = data[i];
        if (c == '\r') { if (i + 1 < size && data[i + 1] == '\n') { cr = 1; continue; } c = '\n'; }
        if (c == 0) c = ' ';
        n->buf[o++] = c;
    }
    kfree(data);
    n->len = o;
    n->crlf = cr || size == 0;
    n->cur = n->anchor = 0;
    n->top = 0; n->left = 0;
    n->modified = 0;
    strlcpy(n->path, path, sizeof n->path);
    relines(n);
    update_title(n);
    wm_invalidate(n->win);
    return 0;
}

static int save_to(np_t *n, const char *path) {
    int extra = 0;
    if (n->crlf) for (int i = 0; i < n->len; i++) if (n->buf[i] == '\n') extra++;
    char *out = kmalloc(n->len + extra + 1);
    int o = 0;
    for (int i = 0; i < n->len; i++) {
        if (n->crlf && n->buf[i] == '\n') out[o++] = '\r';
        out[o++] = n->buf[i];
    }
    wm_set_busy(1);
    int r = vfs_write_file(path, out, (u32)o);
    wm_set_busy(0);
    kfree(out);
    if (r < 0) {
        char msg[VFS_MAX_PATH + 80];
        snprintf(msg, sizeof msg, "Cannot save %s.\n\n%s", path, strerror(r));
        msgbox(n->win, "Notepad", msg, ICON_ERROR, MB_OK);
        return r;
    }
    strlcpy(n->path, path, sizeof n->path);
    n->modified = 0;
    update_title(n);
    wm_invalidate(n->win);
    return 0;
}

static int save_as(np_t *n) {
    char p[VFS_MAX_PATH] = "";
    char dir[VFS_MAX_PATH] = "C:/Documents";
    if (n->path[0]) vfs_dirname(n->path, dir, sizeof dir);
    else { vfs_dirent_t st; if (vfs_stat(dir, &st) < 0) strlcpy(dir, "C:/", sizeof dir); }
    if (!file_dialog(n->win, 1, "Save As", dir, NULL, p, sizeof p)) return -1;
    if (!strchr(vfs_basename(p), '.')) strlcat(p, ".txt", sizeof p);
    vfs_dirent_t st;
    if (vfs_stat(p, &st) == 0) {
        char msg[VFS_MAX_PATH + 64];
        snprintf(msg, sizeof msg, "%s already exists.\nDo you want to replace it?", vfs_basename(p));
        if (msgbox(n->win, "Save As", msg, ICON_WARNING, MB_YESNO) != ID_YES) return -1;
    }
    return save_to(n, p);
}
static int save(np_t *n) { return n->path[0] ? save_to(n, n->path) : save_as(n); }

static int confirm_discard(np_t *n) {
    if (!n->modified) return 1;
    char msg[VFS_MAX_PATH + 80];
    snprintf(msg, sizeof msg, "The text in the %s file has changed.\n\nDo you want to save the changes?", n->path[0] ? vfs_basename(n->path) : "Untitled");
    int r = msgbox(n->win, "Notepad", msg, ICON_WARNING, MB_YESNOCANCEL);
    if (r == ID_CANCEL) return 0;
    if (r == ID_YES) return save(n) == 0;
    return 1;
}

static void copy_sel(np_t *n) {
    int s0 = MIN(n->cur, n->anchor), s1 = MAX(n->cur, n->anchor);
    if (s1 > s0) clipboard_set(n->buf + s0, s1 - s0);
}

static char find_text[128];
static void do_find(np_t *n) {
    if (!inputbox(n->win, "Find", "Find what:", find_text, sizeof find_text) || !find_text[0]) return;
    int fl = (int)strlen(find_text);
    for (int k = 0; k < n->len; k++) {
        int i = (MAX(n->cur, n->anchor) + k) % MAX(1, n->len);
        if (i + fl <= n->len && !strncasecmp(n->buf + i, find_text, fl)) {
            n->anchor = i; n->cur = i + fl;
            scroll_to_caret(n);
            wm_invalidate(n->win);
            return;
        }
    }
    msgbox(n->win, "Notepad", "Cannot find the text.", ICON_INFO, MB_OK);
}

static void np_menu(window_t *w, int id) {
    np_t *n = w->data;
    switch (id) {
    case NP_NEW:
        if (!confirm_discard(n)) return;
        n->len = 0; n->cur = n->anchor = 0; n->top = 0; n->left = 0; n->path[0] = 0; n->modified = 0; n->crlf = 1;
        relines(n);
        break;
    case NP_OPEN: {
        if (!confirm_discard(n)) return;
        char p[VFS_MAX_PATH], dir[VFS_MAX_PATH] = "C:/Documents";
        if (n->path[0]) vfs_dirname(n->path, dir, sizeof dir);
        if (file_dialog(w, 0, "Open", dir, NULL, p, sizeof p)) {
            int r = load_file(n, p);
            if (r < 0) { char m[VFS_MAX_PATH + 64]; snprintf(m, sizeof m, "Cannot open %s.\n\n%s", p, strerror(r)); msgbox(w, "Notepad", m, ICON_ERROR, MB_OK); }
        }
        break;
    }
    case NP_SAVE: save(n); break;
    case NP_SAVEAS: save_as(n); break;
    case NP_EXIT: wm_close(w); return;
    case NP_CUT: copy_sel(n); del_sel(n); break;
    case NP_COPY: copy_sel(n); break;
    case NP_PASTE: { int l; const char *c = clipboard_get(&l); if (c && l) insert(n, c, l); break; }
    case NP_DEL: if (!del_sel(n) && n->cur < n->len) del_range(n, n->cur, next_ch(n, n->cur)); break;
    case NP_SELALL: n->anchor = 0; n->cur = n->len; break;
    case NP_FIND: do_find(n); return;
    case NP_TIME: {
        struct rtc_time t; rtc_read(&t);
        char s[48];
        snprintf(s, sizeof s, "%d:%02d %s %d/%d/%d", t.hour % 12 ? t.hour % 12 : 12, t.minute, t.hour < 12 ? "AM" : "PM", t.month, t.day, t.year);
        insert(n, s, (int)strlen(s));
        break;
    }
    case NP_ABOUT: msgbox(w, "About Notepad", "Halcyon Notepad\nA simple text editor.\n\nFiles are saved with Windows-style line endings.", ICON_NOTEPAD, MB_OK); return;
    }
    if (!wm_alive(w)) return;
    scroll_to_caret(n);
    update_title(n);
    wm_invalidate(w);
}

/* ---------------- input ---------------- */
static int pos_at(np_t *n, int x, int y) {
    rect_t tr = text_rect(n->win);
    int l = n->top + (y - tr.y - 3) / lh();
    if (y < tr.y + 3) l = n->top - 1;
    l = CLAMP(l, 0, n->nlines - 1);
    return off_at_x(n, l, x - tr.x - 2 + n->left);
}

static int is_word(char c) { return isalnum(c) || c == '_' || (u8)c >= 0x80; }   /* non-ASCII: letters */

static void np_event(window_t *w, gui_event_t *e) {
    np_t *n = w->data;
    rect_t tr = text_rect(w);
    rect_t sbr = mkrect(tr.x + tr.w, tr.y, SB_W, tr.h);
    int vl = vis_lines(w);
    switch (e->type) {
    case GE_MOUSE_DOWN:
    case GE_DBLCLICK:
        if (rect_contains(sbr, e->x, e->y)) {
            int part;
            n->top = scrollbar_hit(sbr, n->nlines + vl - 1, vl, n->top, e->x, e->y, 1, &part);
            n->top = CLAMP(n->top, 0, MAX(0, n->nlines - 1));
            if (part == 5) n->sb_drag = 1;
            wm_invalidate(w);
            return;
        }
        if (!rect_contains(tr, e->x, e->y)) return;
        if (e->button == MOUSE_RIGHT) {
            static menu_item_t ctx[] = { { "Cut", NULL, NP_CUT, 0, 0 }, { "Copy", NULL, NP_COPY, 0, 0 }, { "Paste", NULL, NP_PASTE, 0, 0 },
                                         { "Delete", NULL, NP_DEL, 0, 0 }, { "", NULL, 0, MI_SEPARATOR, 0 }, { "Select All", NULL, NP_SELALL, 0, 0 } };
            static menu_t m = { "", ctx, ARRAY_SIZE(ctx) };
            rect_t c = wm_client_rect(w);
            menu_popup(w, &m, c.x + e->x, c.y + e->y);
            return;
        }
        n->cur = pos_at(n, e->x, e->y);
        if (e->type == GE_DBLCLICK) {
            int a = n->cur, b = n->cur;
            while (a > 0 && is_word(n->buf[a - 1])) a--;
            while (b < n->len && is_word(n->buf[b])) b++;
            n->anchor = a; n->cur = b;
        } else if (!(input_mods() & MOD_SHIFT)) n->anchor = n->cur;
        n->mouse_sel = 1;
        n->pref_x = x_of(n, n->cur);
        scroll_to_caret(n);
        wm_invalidate(w);
        return;
    case GE_MOUSE_MOVE:
        if (n->sb_drag && (e->buttons & MOUSE_LEFT)) {
            n->top = CLAMP((e->y - tr.y - SB_W) * MAX(1, n->nlines - 1) / MAX(1, tr.h - 2 * SB_W), 0, MAX(0, n->nlines - 1));
            wm_invalidate(w);
        } else if (n->mouse_sel && (e->buttons & MOUSE_LEFT)) {
            n->cur = pos_at(n, e->x, e->y);
            scroll_to_caret(n);
            wm_invalidate(w);
        }
        w->cursor = rect_contains(tr, e->x, e->y) ? CURSOR_IBEAM : CURSOR_ARROW;
        return;
    case GE_MOUSE_UP:
        n->sb_drag = 0; n->mouse_sel = 0;
        return;
    case GE_WHEEL:
        n->top = CLAMP(n->top - e->dz * 3, 0, MAX(0, n->nlines - 1));
        wm_invalidate(w);
        return;
    case GE_FOCUS: case GE_BLUR:
        wm_invalidate(w);
        return;
    case GE_KEY:
        break;
    default:
        return;
    }
    input_event_t *k = &e->key;
    if (!k->pressed) return;
    int shift = k->mods & MOD_SHIFT, ctrl = k->mods & MOD_CTRL;
    int l = line_of(n, n->cur);
    int moved = 1, keep_pref = 0;
    if (ctrl) {
        switch (k->key) {
        case KEY_A + 13: np_menu(w, NP_NEW); return;          /* ^N */
        case KEY_A + 14: np_menu(w, NP_OPEN); return;         /* ^O */
        case KEY_A + 18: np_menu(w, NP_SAVE); return;         /* ^S */
        case KEY_A + 5: np_menu(w, NP_FIND); return;          /* ^F */
        case KEY_A: np_menu(w, NP_SELALL); return;
        case KEY_A + 2: np_menu(w, NP_COPY); return;
        case KEY_A + 23: np_menu(w, NP_CUT); return;
        case KEY_A + 21: np_menu(w, NP_PASTE); return;
        case KEY_HOME: n->cur = 0; break;
        case KEY_END: n->cur = n->len; break;
        case KEY_LEFT:
            while (n->cur > 0 && !is_word(n->buf[n->cur - 1])) n->cur--;
            while (n->cur > 0 && is_word(n->buf[n->cur - 1])) n->cur--;
            break;
        case KEY_RIGHT:
            while (n->cur < n->len && is_word(n->buf[n->cur])) n->cur++;
            while (n->cur < n->len && !is_word(n->buf[n->cur])) n->cur++;
            break;
        default: return;
        }
    } else {
        switch (k->key) {
        case KEY_LEFT: if (!shift && n->cur != n->anchor) n->cur = MIN(n->cur, n->anchor); else if (n->cur > 0) n->cur = prev_ch(n, n->cur); break;
        case KEY_RIGHT: if (!shift && n->cur != n->anchor) n->cur = MAX(n->cur, n->anchor); else if (n->cur < n->len) n->cur = next_ch(n, n->cur); break;
        case KEY_UP: if (l > 0) n->cur = off_at_x(n, l - 1, n->pref_x); keep_pref = 1; break;
        case KEY_DOWN: if (l + 1 < n->nlines) n->cur = off_at_x(n, l + 1, n->pref_x); keep_pref = 1; break;
        case KEY_PAGEUP: n->cur = off_at_x(n, MAX(0, l - vl), n->pref_x); n->top = MAX(0, n->top - vl); keep_pref = 1; break;
        case KEY_PAGEDOWN: n->cur = off_at_x(n, MIN(n->nlines - 1, l + vl), n->pref_x); n->top = MIN(MAX(0, n->nlines - 1), n->top + vl); keep_pref = 1; break;
        case KEY_HOME: {
            /* smart home: first non-blank, then column 0 */
            int s = n->lines[l], fnb = s;
            while (fnb < line_end(n, l) && (n->buf[fnb] == ' ' || n->buf[fnb] == '\t')) fnb++;
            n->cur = n->cur == fnb ? s : fnb;
            break;
        }
        case KEY_END: n->cur = line_end(n, l); break;
        case KEY_BACKSPACE:
            if (!del_sel(n) && n->cur > 0) del_range(n, prev_ch(n, n->cur), n->cur);
            moved = 0;
            break;
        case KEY_DELETE:
            if (!del_sel(n) && n->cur < n->len) del_range(n, n->cur, next_ch(n, n->cur));
            moved = 0;
            break;
        case KEY_ENTER: case KEY_KP_ENTER: {
            /* keep indentation */
            char ind[64]; int ni = 0;
            ind[ni++] = '\n';
            for (int i = n->lines[l]; i < n->cur && ni < 63 && (n->buf[i] == ' ' || n->buf[i] == '\t'); i++) ind[ni++] = n->buf[i];
            insert(n, ind, ni);
            moved = 0;
            break;
        }
        case KEY_TAB: insert(n, "\t", 1); moved = 0; break;
        case KEY_F5: np_menu(w, NP_TIME); return;
        case KEY_ESC: return;
        default:
            if ((u8)k->ch >= 32 && k->ch != 127) { insert(n, &k->ch, 1); moved = 0; }
            else return;
        }
    }
    if (moved && !shift) n->anchor = n->cur;
    if (!moved) n->anchor = n->cur;
    if (!keep_pref) n->pref_x = x_of(n, n->cur);
    scroll_to_caret(n);
    update_title(n);
    wm_invalidate(w);
}

static void np_tick(window_t *w) {
    np_t *n = w->data;
    if (wm_active() != w) return;
    if (time_ms() - n->caret_t >= 530) {
        n->caret_on = !n->caret_on;
        n->caret_t = time_ms();
        wm_invalidate(w);
    }
}

static int np_close(window_t *w) { return confirm_discard(w->data); }
static void np_destroy(window_t *w) {
    np_t *n = w->data;
    kfree(n->buf); kfree(n->lines); kfree(n);
}

void app_notepad(const char *path) {
    window_t *w = wm_create("Untitled - Notepad", ICON_NOTEPAD, -1, -1, 620, 440, WF_RESIZABLE);
    np_t *n = kzalloc(sizeof *n);
    n->win = w;
    n->cap = 4096;
    n->buf = kmalloc(n->cap);
    n->crlf = 1;
    relines(n);
    w->data = n;
    w->cursor = CURSOR_IBEAM;
    w->on_paint = np_paint;
    w->on_event = np_event;
    w->on_menu = np_menu;
    w->on_tick = np_tick;
    w->on_close = np_close;
    w->on_destroy = np_destroy;
    menubar_attach(w, np_menus, ARRAY_SIZE(np_menus));
    if (path) {
        int r = load_file(n, path);
        if (r == E_NOENT) {
            /* new file at that path */
            strlcpy(n->path, path, sizeof n->path);
            update_title(n);
        } else if (r < 0) {
            char msg[VFS_MAX_PATH + 64];
            snprintf(msg, sizeof msg, "Cannot open %s.\n\n%s", path, strerror(r));
            msgbox(w, "Notepad", msg, ICON_ERROR, MB_OK);
        }
    }
}

/* open an untitled document pre-filled with text (e.g. the kernel log) */
void app_notepad_text(const char *title, const char *text) {
    app_notepad(NULL);
    window_t *w = wm_active();
    if (!w || w->on_paint != np_paint) return;
    np_t *n = w->data;
    int len = (int)strlen(text);
    if (reserve(n, len) < 0) return;
    memcpy(n->buf, text, len);
    n->len = len;
    n->cur = n->anchor = len;
    relines(n);
    scroll_to_caret(n);
    char t[96];
    snprintf(t, sizeof t, "%s - Notepad", title);
    wm_set_title(w, t);
    wm_invalidate(w);
}
