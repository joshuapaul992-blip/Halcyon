/* Halcyon OS - window manager, widgets, menus, dialogs */
#ifndef WM_H
#define WM_H
#include "gfx.h"
#include "drivers/input.h"

/* ---------------- theme ---------------- */
typedef struct theme {
    const char *name;
    color_t title_top, title_mid, title_bottom;        /* active caption gradient */
    color_t title_in_top, title_in_bottom;             /* inactive caption */
    color_t frame, frame_in;                           /* window border */
    color_t title_text, title_in_text;
    color_t face;                                      /* dialog / window background */
    color_t taskbar_top, taskbar_bottom, taskbar_line;
    color_t start_top, start_bottom;
    color_t select, select_text;                       /* selection highlight */
    color_t sidebar_top, sidebar_bottom;               /* explorer task pane */
    color_t menu_header_top, menu_header_bottom;       /* start menu banner */
    color_t button_border, hover;
} theme_t;

extern const theme_t *g_theme;
extern const theme_t g_themes[];
extern const int g_theme_count;
void theme_set(int index);
int  theme_index(void);

/* ---------------- events ---------------- */
enum {
    GE_MOUSE_DOWN = 1, GE_MOUSE_UP, GE_MOUSE_MOVE, GE_DBLCLICK, GE_WHEEL,
    GE_KEY, GE_FOCUS, GE_BLUR, GE_MOUSE_LEAVE,
};

typedef struct gui_event {
    int type;
    int x, y;             /* client coordinates */
    int button;           /* MOUSE_* for down/up/dblclick */
    int buttons;          /* buttons currently held */
    int dz;               /* wheel: >0 up */
    input_event_t key;    /* GE_KEY */
} gui_event_t;

/* ---------------- widgets ---------------- */
typedef struct window window_t;
typedef struct widget widget_t;

enum { W_BUTTON = 1, W_LABEL, W_TEXTBOX, W_CHECKBOX, W_LISTBOX, W_GROUP, W_PROGRESS, W_ICON, W_COMBO };

#define WS_DEFAULT  0x01      /* default button (Enter) */
#define WS_CANCEL   0x02      /* cancel button (Esc) */
#define WS_DISABLED 0x04
#define WS_HIDDEN   0x08
#define WS_PASSWORD 0x10
#define WS_BOLD     0x20
#define WS_READONLY 0x40
#define WS_WRAP     0x80      /* label: word wrap */

struct widget {
    int type;
    rect_t r;                 /* client coordinates */
    u32 style;
    int id;
    char text[1024];          /* labels can hold a message box's text */
    int value;                /* checkbox state, listbox selection, progress 0..1000, icon index */
    /* textbox */
    int cursor, sel_anchor, scroll;
    /* listbox */
    char **items; int nitems, top;
    /* state */
    int hover, pressed;
    void (*on_click)(window_t *w, widget_t *wd);
    void (*on_change)(window_t *w, widget_t *wd);
    void (*on_activate)(window_t *w, widget_t *wd);   /* listbox double click / Enter in textbox */
    widget_t *next;
};

/* ---------------- windows ---------------- */
#define WF_RESIZABLE   0x0001
#define WF_NO_MAXIMIZE 0x0002
#define WF_NO_MINIMIZE 0x0004
#define WF_DIALOG      0x0008   /* no taskbar button, no min/max */
#define WF_POPUP       0x0010   /* no frame (menus), closes when clicking elsewhere */
#define WF_TOPMOST     0x0020
#define WF_NO_CLOSE    0x0040
#define WF_CENTER      0x0080   /* center on screen when created */
#define WF_NOSHADOW    0x0100

enum { WS_NORMAL = 0, WS_MINIMIZED, WS_MAXIMIZED };

#define TITLE_H 26
#define BORDER  4

struct window {
    rect_t frame;             /* screen coordinates, including decorations */
    char title[96];
    int icon;                 /* ICON_* or -1 */
    u32 flags;
    int state;
    rect_t restore;
    surface_t *client;
    int needs_paint;
    int cursor;               /* CURSOR_* while the pointer is over the client area */

    void (*on_paint)(window_t *w, gc_t *g);
    void (*on_event)(window_t *w, gui_event_t *e);
    int  (*on_close)(window_t *w);       /* return 0 to cancel closing; NULL = close */
    void (*on_destroy)(window_t *w);
    void (*on_tick)(window_t *w);        /* ~10 Hz when set */
    void (*on_resize)(window_t *w);
    void (*on_menu)(window_t *w, int id);/* menu bar / popup menu commands */
    void *data;

    widget_t *widgets, *focus;
    struct menubar *menubar;
    window_t *owner;          /* dialogs: the window they belong to */
    window_t *modal;          /* a modal dialog blocking this window */
    int min_w, min_h;
    int result;               /* dialog result */
    int closed;               /* set when destroyed (for nested loops) */
    window_t *next;           /* z-order, bottom to top */
};

enum { CURSOR_ARROW = 0, CURSOR_IBEAM, CURSOR_BUSY, CURSOR_RESIZE_NWSE, CURSOR_RESIZE_H, CURSOR_RESIZE_V, CURSOR_HAND, CURSOR_CROSS };

window_t *wm_create(const char *title, int icon, int x, int y, int cw, int ch, u32 flags);  /* client size; x/y < 0 = auto */
void wm_destroy(window_t *w);
void wm_close(window_t *w);            /* asks on_close first */
void wm_show(window_t *w);             /* restore + raise + activate */
void wm_raise(window_t *w);
void wm_minimize(window_t *w);
void wm_maximize(window_t *w, int on);
void wm_set_title(window_t *w, const char *title);
void wm_invalidate(window_t *w);       /* repaint client */
void wm_invalidate_rect(rect_t screen_r);
rect_t wm_client_rect(window_t *w);    /* screen coordinates */
window_t *wm_active(void);
window_t *wm_first(void);
int  wm_alive(window_t *w);
void wm_resize_client(window_t *w, int cw, int ch);
int  wm_mouse_x(void);
int  wm_mouse_y(void);
void wm_set_busy(int busy);            /* hourglass cursor */

/* main loop */
void gui_main(void);
void gui_step(void);
/* run fn on the GUI thread (directly if this is it) and wait for it */
void gui_call(void (*fn)(void *arg), void *arg);
/* run fn on the GUI thread later; no waiting */
void gui_post(void (*fn)(void *arg), void *arg);                   /* one iteration: input, ticks, render */
void gui_run_until(volatile int *flag);/* nested loop for modal dialogs */
void gui_full_redraw(void);

/* widgets */
widget_t *widget_add(window_t *w, int type, rect_t r, const char *text, int id);
widget_t *widget_find(window_t *w, int id);
void widget_set_text(window_t *w, widget_t *wd, const char *text);
void widget_list_set(window_t *w, widget_t *wd, char **items, int n);   /* copies strings */
void widget_list_add(window_t *w, widget_t *wd, const char *item);
void widgets_paint(window_t *w, gc_t *g);
int  widgets_event(window_t *w, gui_event_t *e);    /* 1 if consumed */
void draw_button(gc_t *g, rect_t r, const char *text, int state, int is_default);   /* state: 0 normal 1 hover 2 pressed 3 disabled */
void draw_scrollbar(gc_t *g, rect_t r, int total, int visible, int pos, int vertical);
/* scrollbar hit test helper: returns new position for a click/drag at (x,y) */
int  scrollbar_hit(rect_t r, int total, int visible, int pos, int x, int y, int vertical, int *part);
void draw_group(gc_t *g, rect_t r, const char *title);

/* menus */
#define MI_SEPARATOR 0x01
#define MI_DISABLED  0x02
#define MI_CHECKED   0x04
#define MI_BOLD      0x08
typedef struct menu_item { const char *label; const char *shortcut; int id; u32 flags; int icon; } menu_item_t;
typedef struct menu { const char *title; menu_item_t *items; int count; } menu_t;
typedef struct menubar { menu_t *menus; int count; int open; int hover; } menubar_t;
#define MENUBAR_H 20

/* pop up a menu at screen position; selecting calls owner->on_menu(owner, id) */
void menu_popup(window_t *owner, menu_t *m, int x, int y);
void menu_close_all(void);
void menubar_attach(window_t *w, menu_t *menus, int count);   /* adds MENUBAR_H at the top of the client */
int  menubar_height(window_t *w);
void menubar_paint(window_t *w, gc_t *g);
int  menubar_event(window_t *w, gui_event_t *e);

/* dialogs (modal, run a nested loop) */
enum { MB_OK = 1, MB_OKCANCEL, MB_YESNO, MB_YESNOCANCEL };
enum { ID_OK = 1, ID_CANCEL = 2, ID_YES = 6, ID_NO = 7 };
int  msgbox(window_t *owner, const char *title, const char *text, int icon, int buttons);
/* a message box that does not wait: buttons call on_click; the caller destroys it */
window_t *msgbox_window(const char *title, const char *text, int icon, int buttons,
                        void (*on_click)(window_t *w, widget_t *wd));
/* text input; returns 1 and fills buf if OK */
int  inputbox(window_t *owner, const char *title, const char *prompt, char *buf, int bufsz);
/* file dialogs: dir is the starting folder; result path in out */
int  file_dialog(window_t *owner, int save, const char *title, const char *dir, const char *filter_ext, char *out, int outsz);

/* clipboard (text) */
void clipboard_set(const char *text, int len);
const char *clipboard_get(int *len);

/* desktop / shell (desktop.c) */
void desktop_init(void);
void desktop_paint(gc_t *g, rect_t clip);
int  desktop_event(gui_event_t *e);    /* screen coords */
void desktop_launch_calculator(void);   /* the Calculator key of multimedia keyboards */
void taskbar_paint(gc_t *g, rect_t clip);
int  taskbar_event(gui_event_t *e);
rect_t taskbar_rect(void);
void taskbar_invalidate(void);
void desktop_wallpaper_set(int style);
int  desktop_wallpaper_style(void);
int  desktop_screen_changed(void);
void desktop_restart(void);              /* save the log, sync, reboot */
void desktop_wallpaper_render(int style, surface_t *s);   /* draw a wallpaper style into any surface (previews) */
void start_menu_toggle(void);
void shell_tick(void);
#define TASKBAR_H 30

/* application launchers (apps/) */
void app_explorer(const char *path);
void app_notepad(const char *path);
void app_terminal(void);
void app_paint(const char *path);
void app_calc(void);
void app_sysinfo(void);
void app_control(void);
void app_display(void);
void app_about(void);
void app_imageview(const char *path);
void app_mines(void);
void app_keyboard(void);               /* Keyboard settings (apps/keyboard.c) */
void keyboard_menu(int x, int y);      /* the taskbar layout indicator's menu */
void shutdown_dialog(void);
/* open a file with the right application, by extension */
void shell_open(const char *path);
int  format_dialog(window_t *owner, char letter);   /* apps/format.c; 1 if formatted */
struct proc;
int  gui_ask_permission(struct proc *asker, const char *title, const char *text);   /* permit.c */
int  shell_run(const char *path, const char *args);   /* start a user program; pid or error (reported) */

#endif
