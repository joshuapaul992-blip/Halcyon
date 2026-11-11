/* GUIDEMO.EXE - shows the program window API: a menu bar (checked, disabled
 * and replaced items, mnemonics), a context menu, scroll bars, tool buttons
 * with tooltips, a status bar, cursor shapes, clipping, local text
 * measurement, mouse capture, keyboard focus and an input box. Every event
 * is also written to the console (the serial log when started by the
 * QEMU tests' autorun). */
#include "hal.h"

#define W0 560
#define H0 380
#define TOOL_H 30
#define SB 17
#define GUTTER 44
#define NLINES 300

enum {
    /* menu items */
    M_RENAME = 1, M_REPLACE, M_EXIT, M_COPY, M_PASTE, M_ENABLE_PASTE, M_GRID, M_CROSS, M_STATUS, M_ABOUT,
    M_BACK, M_EXTRA, M_CTX_SELECT, M_CTX_CHECK, M_CTX_DISABLED,
    /* controls */
    C_VSCROLL = 100, C_HSCROLL, C_STATUS, C_TB_GRID, C_TB_RENAME, C_TB_HELP, C_FIND_LABEL, C_FIND,
};

static int win, cw = W0, ch = H0;
static int top, left;                       /* first line, horizontal scroll (px) */
static int sel = -1;                         /* selected line */
static int grid = 1, cross, status = 1, paste_on, ctx_check, alt_bar, bar_on = 1;
static int dragging, drag_x0, drag_y0, drag_x1, drag_y1, outside_reported;
static int cursor = -1;
static int line_h, maxw;
static char lines[NLINES][160];
static char last[96] = "Ready";
static hal_theme_t th;

static hal_menu_item_t bar_a[] = {
    { "&File", 0, 0, MENU_TITLE, 0, 0 },
    { "&Rename Window...", "F3", M_RENAME, 0, HICON_FILE_TEXT, 0 },
    { "Re&place Menu Bar", 0, M_REPLACE, 0, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "E&xit", "Alt+F4", M_EXIT, 0, 0, 0 },
    { "&Edit", 0, 0, MENU_TITLE, 0, 0 },
    { "&Copy Line", "Ctrl+C", M_COPY, MENU_BOLD, 0, 0 },
    { "&Paste", "Ctrl+V", M_PASTE, MENU_DISABLED, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "&Enable Paste", 0, M_ENABLE_PASTE, 0, 0, 0 },
    { "&View", 0, 0, MENU_TITLE, 0, 0 },
    { "&Grid", 0, M_GRID, MENU_CHECKED, 0, 0 },
    { "Cross&hair Cursor", 0, M_CROSS, 0, 0, 0 },
    { "Status &Bar", 0, M_STATUS, MENU_CHECKED, 0, 0 },
    { "&Help", 0, 0, MENU_TITLE, 0, 0 },
    { "&About GUI Demo", 0, M_ABOUT, 0, HICON_HELP, 0 },
};
static hal_menu_item_t bar_b[] = {
    { "&File", 0, 0, MENU_TITLE, 0, 0 },
    { "E&xit", 0, M_EXIT, 0, 0, 0 },
    { "E&xtra", 0, 0, MENU_TITLE, 0, 0 },
    { "&Back to the First Menu Bar", 0, M_BACK, 0, 0, 0 },
    { "&Something", "Ctrl+Q", M_EXTRA, MENU_CHECKED, HICON_GAME, 0 },
};
#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* ---- layout ---- */
static int area_x(void) { return GUTTER; }
static int area_y(void) { return TOOL_H; }
static int area_w(void) { return cw - SB - GUTTER; }
static int status_h(void) { return status ? 22 : 0; }
static int area_h(void) { return ch - TOOL_H - SB - status_h(); }
static int vis_lines(void) { int v = area_h() / line_h; return v < 1 ? 1 : v; }

static void set_cursor(int c) {
    if (c == cursor) return;
    cursor = c;
    win_set_cursor(win, c);
    printf("guidemo: cursor %d\n", c);
}

static void layout(void) {
    ctl_move(win, C_VSCROLL, cw - SB, TOOL_H, SB, area_h());
    ctl_move(win, C_HSCROLL, 0, TOOL_H + area_h(), cw - SB, SB);
    ctl_move(win, C_FIND, cw - 170, 4, 164, 22);
    ctl_move(win, C_FIND_LABEL, cw - 206, 4, 34, 22);
    ctl_scroll_set(win, C_VSCROLL, NLINES, vis_lines(), top, 1);
    ctl_scroll_set(win, C_HSCROLL, maxw + 8, area_w(), left, 16);
    top = ctl_get_value(win, C_VSCROLL);
    left = ctl_get_value(win, C_HSCROLL);
}

static void update_status(void) {
    char s[256];
    snprintf(s, sizeof s, "%s\tLine %d of %d\t%s", last, sel + 1, NLINES, grid ? "Grid on" : "Grid off");
    ctl_set_text(win, C_STATUS, s);
}

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last, sizeof last, fmt, ap);
    va_end(ap);
    printf("guidemo: %s\n", last);
    update_status();
}

/* ---- drawing ---- */
static void paint(void) {
    gfx_theme(&th);
    gfx_clip_reset(win);
    /* tool bar band and the corner between the scroll bars */
    gfx_gradient(win, 0, 0, cw, TOOL_H, 0xFFFFFF, th.face);
    gfx_line(win, 0, TOOL_H - 1, cw, TOOL_H - 1, 0xB0B0A8);
    gfx_fill(win, cw - SB, TOOL_H + area_h(), SB, SB, th.face);
    /* the gutter with line numbers: its own clip so numbers never run into the tool bar */
    gfx_clip_origin(win, 0, area_y(), GUTTER, area_h());
    gfx_fill(win, 0, 0, GUTTER, area_h(), 0xE8E6DC);
    for (int i = 0; i <= vis_lines() && top + i < NLINES; i++) {
        char n[8];
        snprintf(n, sizeof n, "%d", top + i + 1);
        gfx_text_box(win, 0, i * line_h, GUTTER - 6, line_h, n, 0x808080, FONT_UI, 2);
    }
    gfx_line(win, GUTTER - 1, 0, GUTTER - 1, area_h(), 0xB0B0A8);
    /* the text: clipped to the area, so scrolled text never covers the gutter or the tool bar */
    gfx_clip_origin(win, area_x(), area_y(), area_w(), area_h());
    gfx_fill(win, 0, 0, area_w(), area_h(), 0xFFFFFF);
    for (int i = 0; i <= vis_lines() && top + i < NLINES; i++) {
        int l = top + i, y = i * line_h;
        int len = (int)strlen(lines[l]);
        if (l == sel) gfx_fill(win, 0, y, text_width_n(lines[l], len, FONT_UI) + 8 - left, line_h, th.select);
        if (grid) gfx_line(win, 0, y + line_h - 1, area_w(), y + line_h - 1, 0xEEF0F8);
        gfx_text_n(win, 4 - left, y + 1, lines[l], len, l == sel ? th.select_text : 0x000000, FONT_UI);
    }
    if (dragging) {
        int x0 = drag_x0 < drag_x1 ? drag_x0 : drag_x1, y0 = drag_y0 < drag_y1 ? drag_y0 : drag_y1;
        int w = abs(drag_x1 - drag_x0), h = abs(drag_y1 - drag_y0);
        gfx_rect(win, x0 - area_x(), y0 - area_y(), w + 1, h + 1, th.select);
    }
    gfx_clip_reset(win);
    win_invalidate(win);
}

/* ---- commands ---- */
static void about(void) {
    msgbox(win, "About GUI Demo", "GUI Demo shows the program window API:\nmenus, scroll bars, tool buttons, "
           "tooltips, a status bar, cursors, clipping and text measurement.", MBX_ICON_INFO, MBX_OK);
}

static void toggle_status(void) {
    status = !status;
    menu_check(win, M_STATUS, status);
    ctl_set_style(win, C_STATUS, status ? 0 : CTL_HIDDEN);
    layout();
    paint();
}

static void command(int id) {
    switch (id) {
    case M_RENAME: case C_TB_RENAME: {
        char name[64] = "GUI Demo";
        if (inputbox(win, "Rename Window", "New window title:", name, sizeof name)) { win_set_title(win, name); note("title set to \"%s\"", name); }
        else note("rename cancelled");
        break;
    }
    case M_REPLACE: alt_bar = 1; menu_set(win, bar_b, N(bar_b)); note("menu bar replaced"); break;
    case M_BACK: alt_bar = 0; menu_set(win, bar_a, N(bar_a)); menu_check(win, M_GRID, grid); menu_check(win, M_CROSS, cross);
        menu_check(win, M_STATUS, status); menu_enable(win, M_PASTE, paste_on); menu_check(win, M_ENABLE_PASTE, paste_on);
        note("first menu bar back"); break;
    case M_EXIT: win_destroy(win); exit(0);
    case M_COPY: case M_CTX_SELECT:
        if (sel >= 0) { clipboard_set_n(lines[sel], (int)strlen(lines[sel])); note("copied line %d", sel + 1); }
        else note("nothing selected");
        break;
    case M_PASTE: { char b[96]; clipboard_get(b, sizeof b); note("clipboard: %s", b); break; }
    case M_ENABLE_PASTE:
        paste_on = !paste_on;
        note("paste %s (%d items changed)", paste_on ? "enabled" : "disabled", menu_enable(win, M_PASTE, paste_on));
        menu_check(win, M_ENABLE_PASTE, paste_on);
        break;
    case M_GRID: case C_TB_GRID:
        grid = !grid;
        menu_check(win, M_GRID, grid);
        ctl_set_value(win, C_TB_GRID, grid);
        note("grid %s", grid ? "on" : "off");
        paint();
        break;
    case M_CROSS: cross = !cross; menu_check(win, M_CROSS, cross); note("crosshair %s", cross ? "on" : "off"); break;
    case M_STATUS: toggle_status(); break;
    case M_ABOUT: case C_TB_HELP: about(); break;
    case M_CTX_CHECK: ctx_check = !ctx_check; note("context item %s", ctx_check ? "checked" : "unchecked"); break;
    case M_EXTRA: note("extra item"); break;
    }
}

static void context_menu(int x, int y) {
    hal_menu_item_t m[] = {
        { "&Copy Line", 0, M_CTX_SELECT, MENU_BOLD | (sel < 0 ? MENU_DISABLED : 0), 0, 0 },
        { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
        { "C&heck Me", 0, M_CTX_CHECK, ctx_check ? MENU_CHECKED : 0, 0, 0 },
        { "&Disabled Item", 0, M_CTX_DISABLED, MENU_DISABLED, 0, 0 },
        { "&Grid", 0, M_GRID, grid ? MENU_CHECKED : 0, HICON_FILE_IMAGE, 0 },
    };
    int r = menu_popup(win, m, N(m), x, y);
    printf("guidemo: popup at %d,%d: %d\n", x, y, r);
}

static int find_line(const char *what) {
    for (int k = 1; k <= NLINES; k++) {
        int l = (sel + k + NLINES) % NLINES;
        if (strstr(lines[l], what)) return l;
    }
    return -1;
}

static void select_line(int l) {
    sel = l;
    if (sel < top) top = sel;
    if (sel >= top + vis_lines()) top = sel - vis_lines() + 1;
    ctl_set_value(win, C_VSCROLL, top);
    top = ctl_get_value(win, C_VSCROLL);
    update_status();
    paint();
}

/* ---- checks of the local text measurement against the kernel ---- */
static void check_metrics(void) {
    static const char *samples[] = { "Hello, world", "The quick brown fox jumps over the lazy dog", "na\xc3\xafve caf\xc3\xa9 \xe2\x80\x93 \xce\x95\xce\xbb\xce\xbb\xce\xb7\xce\xbd\xce\xb9\xce\xba\xce\xac",
                                     "\xd0\xa0\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9 \xe2\x9c\x93 \xe4\xb8\xad", "bad \xff\xfe bytes", "" };
    int ok = 0, n = 0;
    for (int f = 0; f < 7; f++)
        for (int i = 0; i < N(samples); i++) {
            n++;
            int a = text_width(samples[i], f), b = gfx_text_width(samples[i], f);
            if (a == b) ok++;
            else printf("guidemo: metrics mismatch font %d \"%s\": %d vs %d\n", f, samples[i], a, b);
        }
    int fit = text_fit(samples[1], (int)strlen(samples[1]), FONT_UI, 100);
    char part[64];
    memcpy(part, samples[1], (size_t)fit);
    part[fit] = 0;
    int fw = gfx_text_width(part, FONT_UI);
    int fit_ok = fw <= 100 && fw + text_char_width((unsigned char)samples[1][fit], FONT_UI) > 100;
    printf("guidemo: metrics %d/%d widths match, fit %s, height %d/%d\n", ok, n, fit_ok ? "ok" : "WRONG",
           text_height(FONT_UI), gfx_font_height(FONT_UI));
}

int main(void) {
    for (int i = 0; i < NLINES; i++)
        snprintf(lines[i], sizeof lines[i], "%s line %d: %s", i % 3 ? "Plain" : "Mixed",
                 i + 1, i % 3 == 0 ? "na\xc3\xafve caf\xc3\xa9 \xe2\x80\x93 \xce\x95\xce\xbb\xce\xbb\xce\xb7\xce\xbd\xce\xb9\xce\xba\xce\xac \xd0\xa0\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9, a much longer line that needs the horizontal scroll bar"
                                      : i % 3 == 1 ? "the quick brown fox" : "jumps over the lazy dog, scroll right to see the end of this line");
    check_metrics();
    line_h = text_height(FONT_UI) + 3;
    for (int i = 0; i < NLINES; i++) { int w = text_width(lines[i], FONT_UI); if (w > maxw) maxw = w; }

    win = win_create("GUI Demo", W0, H0, WIN_RESIZABLE);
    if (win < 0) { printf("guidemo: no window (%d)\n", win); return 1; }
    win_set_icon(win, HICON_CONTROL);
    win_set_min_size(win, 360, 200);
    printf("guidemo: menu_set %d\n", menu_set(win, bar_a, N(bar_a)));
    ctl_create(win, CTL_TOOLBUTTON, 4, 3, 26, 24, "", C_TB_GRID, 0);
    ctl_set_tool(win, C_TB_GRID, HICON_FILE_IMAGE, "Show or hide the grid");
    ctl_set_value(win, C_TB_GRID, grid);
    ctl_create(win, CTL_TOOLBUTTON, 32, 3, 80, 24, "Rename", C_TB_RENAME, 0);
    ctl_set_tool(win, C_TB_RENAME, HICON_FILE_TEXT, "Change the window title");
    ctl_create(win, CTL_TOOLBUTTON, 114, 3, 26, 24, "", C_TB_HELP, 0);
    ctl_set_tool(win, C_TB_HELP, HICON_HELP, "About GUI Demo");
    ctl_create(win, CTL_LABEL, cw - 206, 4, 34, 22, "Find:", C_FIND_LABEL, 0);
    ctl_create(win, CTL_TEXTBOX, cw - 170, 4, 164, 22, "", C_FIND, CTL_NOTIFY);
    ctl_create(win, CTL_VSCROLL, 0, 0, SB, 10, 0, C_VSCROLL, 0);
    ctl_create(win, CTL_HSCROLL, 0, 0, 10, SB, 0, C_HSCROLL, 0);
    ctl_create(win, CTL_STATUSBAR, 0, 0, 0, 0, "", C_STATUS, 0);
    ctl_set_value(win, C_STATUS, 110);
    ctl_focus(win, 0);                      /* keys go to the text area, not the Find box */
    layout();
    update_status();
    hal_rect_t r;
    int st = win_get_rect(win, &r);
    printf("guidemo: window client at %d,%d size %dx%d state %d\n", r.x, r.y, r.w, r.h, st);

    for (;;) {
        hal_event_t ev;
        if (!win_get_event(&ev, -1)) continue;
        switch (ev.type) {
        case EVT_PAINT: paint(); break;
        case EVT_CLOSE: win_destroy(win); return 0;
        case EVT_RESIZE:
            cw = ev.x; ch = ev.y;
            layout();
            st = win_get_rect(win, &r);
            printf("guidemo: resized to %dx%d, on screen %d,%d %dx%d state %d\n", cw, ch, r.x, r.y, r.w, r.h, st);
            break;
        case EVT_MENU:
            printf("guidemo: menu id %d\n", ev.id);
            command(ev.id);
            break;
        case EVT_COMMAND:
            printf("guidemo: command id %d code %d\n", ev.id, ev.button);
            if (ev.id == C_FIND && ev.button == CMD_ACTIVATE) {
                char what[64];
                ctl_get_text(win, C_FIND, what, sizeof what);
                int l = what[0] ? find_line(what) : -1;
                if (l >= 0) { note("found \"%s\" in line %d", what, l + 1); select_line(l); }
                else note("\"%s\" not found", what);
                printf("guidemo: focus back from %d\n", ctl_focus(win, 0));
            } else {
                command(ev.id);
            }
            break;
        case EVT_SCROLL:
            printf("guidemo: scroll id %d pos %d part %d\n", ev.id, ev.x, ev.button);
            if (ev.id == C_VSCROLL) top = ev.x; else left = ev.x;
            paint();
            break;
        case EVT_WHEEL:
            ctl_set_value(win, C_VSCROLL, top - ev.dz * 3);
            top = ctl_get_value(win, C_VSCROLL);
            paint();
            break;
        case EVT_MOUSE_DOWN: case EVT_DBLCLICK: {
            int in_area = ev.x >= area_x() && ev.x < area_x() + area_w() && ev.y >= area_y() && ev.y < area_y() + area_h();
            if (!in_area) break;
            ctl_focus(win, 0);
            int l = top + (ev.y - area_y()) / line_h;
            if (l < NLINES) select_line(l);
            if (ev.button == MOUSE_RIGHT) { context_menu(ev.x, ev.y); break; }
            if (ev.button == MOUSE_LEFT) {
                dragging = 1; outside_reported = 0;
                drag_x0 = drag_x1 = ev.x; drag_y0 = drag_y1 = ev.y;
                note("press at %d,%d%s", ev.x, ev.y, (ev.mods & MOD_SHIFT) ? " with Shift" : "");
            }
            break;
        }
        case EVT_MOUSE_MOVE: {
            int in_area = ev.x >= area_x() && ev.x < area_x() + area_w() && ev.y >= area_y() && ev.y < area_y() + area_h();
            int in_gutter = ev.x < GUTTER && ev.y >= area_y() && ev.y < area_y() + area_h();
            set_cursor(in_area ? (cross ? CUR_CROSS : CUR_IBEAM) : in_gutter ? CUR_HAND : CUR_ARROW);
            if (dragging) {
                drag_x1 = ev.x; drag_y1 = ev.y;
                if ((ev.x < 0 || ev.y < 0 || ev.x >= cw || ev.y >= ch) && !outside_reported) {
                    outside_reported = 1;
                    note("captured move outside at %d,%d", ev.x, ev.y);
                }
                paint();
            }
            break;
        }
        case EVT_MOUSE_UP:
            if (dragging && ev.button == MOUSE_LEFT) {
                dragging = 0;
                note("drag %d,%d to %d,%d", drag_x0, drag_y0, ev.x, ev.y);
                paint();
            }
            break;
        case EVT_KEY:
            if (!ev.pressed) break;
            if (key_shortcut(&ev, 'C')) { command(M_COPY); break; }
            if (key_shortcut(&ev, 'V') && paste_on) { command(M_PASTE); break; }
            switch (ev.id) {
            case KEY_F3: command(M_RENAME); break;
            case KEY_F2:
                /* remove and restore the menu bar: the canvas keeps its size */
                bar_on = !bar_on;
                if (bar_on) command(alt_bar ? M_REPLACE : M_BACK);
                else menu_set(win, NULL, 0);
                st = win_get_rect(win, &r);
                note("menu bar %s; client %dx%d at %d,%d", bar_on ? "on" : "off", r.w, r.h, r.x, r.y);
                break;
            case KEY_DOWN: if (sel < NLINES - 1) select_line(sel + 1); break;
            case KEY_UP: if (sel > 0) select_line(sel - 1); break;
            case KEY_PAGEDOWN: select_line(sel + vis_lines() < NLINES ? sel + vis_lines() : NLINES - 1); break;
            case KEY_PAGEUP: select_line(sel - vis_lines() > 0 ? sel - vis_lines() : 0); break;
            case KEY_MENU: context_menu(area_x() + 20, area_y() + 20); break;
            default:
                if (ev.ch >= 32) printf("guidemo: key '%c' to the text area (focus %d)\n", ev.ch < 127 ? (char)ev.ch : '?', ctl_get_focus(win));
            }
            break;
        }
    }
}
