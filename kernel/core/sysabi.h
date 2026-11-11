/* Halcyon OS - system call interface shared by the kernel and user programs.
 *
 * Calling convention (x86-64 SYSCALL): number in rax, arguments in rdi, rsi,
 * rdx, r10, r8, r9; result in rax (>= 0 success, negative E_* error codes as
 * in core/types.h). rcx and r11 are clobbered; every other register,
 * including the SSE registers, is preserved. Strings are UTF-8, paths are
 * "C:/dir/file" or relative to the process's current directory. */
#ifndef SYSABI_H
#define SYSABI_H

enum {
    /* processes */
    SYS_EXIT = 0, SYS_SPAWN, SYS_WAIT, SYS_GETPID, SYS_YIELD, SYS_SLEEP, SYS_TIME, SYS_SBRK,
    SYS_GETCWD, SYS_CHDIR, SYS_KILL, SYS_PROCLIST, SYS_SYSINFO,
    /* files */
    SYS_OPEN = 20, SYS_CLOSE, SYS_READ, SYS_WRITE, SYS_SEEK, SYS_FSIZE, SYS_TRUNCATE,
    SYS_STAT, SYS_READDIR, SYS_MKDIR, SYS_UNLINK, SYS_RENAME, SYS_VOLUMES,
    /* windows and drawing (user32 / GDI style) */
    SYS_WIN_CREATE = 40, SYS_WIN_DESTROY, SYS_WIN_SET_TITLE, SYS_WIN_INVALIDATE, SYS_WIN_GET_EVENT,
    SYS_WIN_SIZE, SYS_WIN_SET_TIMER, SYS_WIN_SHOW,
    SYS_WIN_RESIZE, SYS_WIN_SET_ICON,     /* client size (kept on screen); title bar / taskbar icon */
    /* menus, cursor, geometry, dialogs, clipping, font metrics (50-59) */
    SYS_MENU_SET = 50, SYS_MENU_POPUP, SYS_MENU_STATE, SYS_WIN_SET_CURSOR, SYS_WIN_GET_RECT,
    SYS_WIN_SET_MIN_SIZE, SYS_WIN_SET_OWNER, SYS_GFX_SET_CLIP, SYS_GFX_FONT_METRICS, SYS_GFX_TEXT_N,
    SYS_GFX_FILL = 60, SYS_GFX_RECT, SYS_GFX_LINE, SYS_GFX_TEXT, SYS_GFX_TEXT_WIDTH, SYS_GFX_BLIT,
    SYS_GFX_ICON, SYS_GFX_BEVEL, SYS_GFX_CIRCLE, SYS_GFX_FONT_HEIGHT,
    /* shaded shapes, theme colours, control layout / focus / scroll bars / tool buttons, long clipboard text (70-76) */
    SYS_GFX_SHADE = 70, SYS_GFX_THEME, SYS_CTL_MOVE, SYS_CTL_FOCUS, SYS_CTL_SCROLL_SET, SYS_CTL_SET_TOOL,
    SYS_CLIPBOARD_SET_N,
    SYS_CTL_CREATE = 80, SYS_CTL_SET_TEXT, SYS_CTL_GET_TEXT, SYS_CTL_SET_VALUE, SYS_CTL_GET_VALUE,
    SYS_CTL_SET_STYLE, SYS_CTL_LIST_SET,
    SYS_MSGBOX = 90, SYS_FILE_DIALOG, SYS_CLIPBOARD_SET, SYS_CLIPBOARD_GET,
    /* M9 number ranges (each owned by one part of the kernel):
     *   13-19   threads and futexes (core/proc.c, core/syscall.c)
     *   33-39   shell and shared memory (core/syscall.c)
     *   48      SYS_WIN_CANVAS: the window's pixels mapped into the program
     *   49-59, 70-79, 87-89, 94-99, 140-159   more window / drawing / control /
     *           menu calls (gui/usergui.c)
     *   100-139 sockets and poll (net/) */
    SYS_COUNT = 160
};

/* SYS_OPEN flags */
#define O_READ   0x01
#define O_WRITE  0x02
#define O_CREATE 0x04
#define O_TRUNC  0x08
#define O_APPEND 0x10
#define O_EXCL   0x20

/* SYS_STAT / SYS_READDIR entry */
typedef struct hal_dirent {
    char name[256];
    unsigned long long size;
    unsigned char is_dir, attr, pad[6];
    unsigned short year; unsigned char month, day, hour, minute, second, pad2;
} hal_dirent_t;

/* SYS_PROCLIST entry */
typedef struct hal_procinfo {
    unsigned int pid, parent;
    char name[32];
    unsigned long long mem_kib, cpu_ms, run_ms;
    int threads, state;          /* 1 running, 2 ended */
} hal_procinfo_t;

/* SYS_SYSINFO */
typedef struct hal_sysinfo {
    unsigned long long uptime_ms, ram_kib, heap_kib, heap_used_kib;
    int cpu_load, nprocs, nthreads, pad;
    char cpu[48];
} hal_sysinfo_t;

/* window events (SYS_WIN_GET_EVENT) */
enum { EVT_NONE = 0, EVT_PAINT, EVT_CLOSE, EVT_KEY, EVT_MOUSE_DOWN, EVT_MOUSE_UP, EVT_MOUSE_MOVE, EVT_DBLCLICK,
       EVT_WHEEL, EVT_COMMAND, EVT_TIMER, EVT_RESIZE, EVT_FOCUS,
       EVT_MOUSE_LEAVE,              /* the pointer left the window (clear hover highlights) */
       EVT_MENU,                     /* a menu item was chosen: id = its id */
       EVT_SCROLL };                 /* a scroll bar moved: id = control, x = new position, button = SCROLL_* part */
typedef struct hal_event {
    int type;
    int win;                     /* window handle */
    int x, y;                    /* mouse: client coordinates; EVT_RESIZE: new client size */
    int button, buttons, dz;     /* EVT_COMMAND: button = CMD_CLICK or CMD_ACTIVATE */
    int id;                      /* EVT_COMMAND / EVT_SCROLL: control id; EVT_MENU: item id; EVT_KEY: key code (usage) */
    int pressed, mods;           /* EVT_KEY; mods also for mouse events (Shift+click...) */
    unsigned int ch;             /* EVT_KEY: character (Unicode code point) or 0; id 0 = the accent of
                                    a dead key that did not compose, just before the key's own event */
    int vk;                      /* EVT_KEY: the key's letter 'A'..'Z' or digit '0'..'9' on the keyboard
                                    layout, for Ctrl shortcuts (US letters on Russian and Greek), or 0 */
} hal_event_t;

/* EVT_COMMAND notifications (button field) */
#define CMD_CLICK    0           /* button / tool button clicked, check box or list selection changed */
#define CMD_ACTIVATE 1           /* CTL_NOTIFY: Enter in a text box, double click or Enter in a list box */

/* EVT_SCROLL parts (button field) */
enum { SCROLL_LINE_UP = 1, SCROLL_LINE_DOWN, SCROLL_PAGE_UP, SCROLL_PAGE_DOWN, SCROLL_THUMB_TRACK,
       SCROLL_THUMB_END, SCROLL_WHEEL };

/* SYS_WIN_CREATE flags */
#define WIN_RESIZABLE 0x01
#define WIN_DIALOG    0x02
#define WIN_NO_MAX    0x04

/* fonts for SYS_GFX_TEXT* */
enum { FONT_UI = 0, FONT_UI_BOLD, FONT_TITLE, FONT_MONO, FONT_LARGE, FONT_HUGE, FONT_SMALL };

/* controls (SYS_CTL_CREATE) */
enum { CTL_BUTTON = 1, CTL_LABEL, CTL_TEXTBOX, CTL_CHECKBOX, CTL_LISTBOX, CTL_GROUP, CTL_PROGRESS,
       CTL_VSCROLL, CTL_HSCROLL,     /* scroll bars: SYS_CTL_SCROLL_SET, EVT_SCROLL */
       CTL_TOOLBUTTON,               /* flat toolbar button: icon and tooltip with SYS_CTL_SET_TOOL; value 1 = checked */
       CTL_STATUSBAR };              /* docked at the bottom (height = h or 22); parts separated by '\t',
                                        the parts after the first are value px wide (default 150) */
#define CTL_DEFAULT  0x01
#define CTL_CANCEL   0x02
#define CTL_DISABLED 0x04
#define CTL_HIDDEN   0x08
#define CTL_PASSWORD 0x10
#define CTL_BOLD     0x20
#define CTL_READONLY 0x40
#define CTL_WRAP     0x80
#define CTL_NOTIFY   0x100       /* text box / list box: send EVT_COMMAND with CMD_ACTIVATE */

/* menus (SYS_MENU_SET, SYS_MENU_POPUP): a flat array. For a menu bar each
 * MENU_TITLE entry starts a menu ("&File") and the entries after it are its
 * items; a popup menu has items only. '&' marks the mnemonic letter. */
typedef struct hal_menu_item {
    const char *label;           /* up to 63 bytes; ignored for separators */
    const char *shortcut;        /* right-aligned hint such as "Ctrl+S", or NULL (up to 31 bytes) */
    int id;                      /* EVT_MENU id */
    int flags;                   /* MENU_* */
    int icon;                    /* HICON_* shown left of the label; 0 = none */
    int pad;
} hal_menu_item_t;
#define MENU_SEPARATOR 0x01
#define MENU_DISABLED  0x02
#define MENU_CHECKED   0x04
#define MENU_BOLD      0x08      /* the default item */
#define MENU_TITLE     0x10      /* menu bar: starts a menu */
#define MENU_MAX_ITEMS 256       /* entries per call (titles included) */
#define MENU_MAX_TITLES 16

/* mouse cursor shapes (SYS_WIN_SET_CURSOR) */
enum { CUR_ARROW = 0, CUR_IBEAM, CUR_WAIT, CUR_SIZE_NWSE, CUR_SIZE_WE, CUR_SIZE_NS,
       CUR_HAND, CUR_CROSS };

/* SYS_GFX_SET_CLIP flags */
#define CLIP_ORIGIN 0x01         /* also move the drawing origin to the clip rectangle's corner */
#define CLIP_RESET  0x02         /* whole canvas, origin 0,0 */

/* SYS_GFX_FONT_METRICS */
typedef struct hal_fontinfo { int height, ascent, descent, pad; } hal_fontinfo_t;
#define FONT_METRICS_MAX 4096    /* code points per call */

/* SYS_GFX_SHADE kinds */
enum { SHADE_GRADIENT_V = 1,     /* c1 top, c2 bottom */
       SHADE_GRADIENT_H,         /* c1 left, c2 right */
       SHADE_ROUNDED,            /* c1 top, c2 bottom; param = radius | corners << 8 (1 TL, 2 TR, 4 BR, 8 BL; 0 = all) */
       SHADE_ROUNDED_OUTLINE,    /* c1; param as SHADE_ROUNDED */
       SHADE_SUNKEN,             /* sunken field (text box frame) filled with c1 */
       SHADE_FOCUS,              /* dotted focus rectangle */
       SHADE_SHADOW,             /* soft shadow around the rectangle; param = size | strength << 8 */
       SHADE_ICON_TINTED,        /* icon at x, y: c2 = icon | size << 16 (as SYS_GFX_ICON), c1 tint, param 0..255 */
       SHADE_ICON_ALPHA };       /* icon: c2 as above, param = opacity 0..255 (disabled look: 90) */

/* SYS_GFX_THEME: the current colour scheme, 0xRRGGBB */
typedef struct hal_theme {
    unsigned title_top, title_mid, title_bottom, title_in_top, title_in_bottom;
    unsigned frame, frame_in, title_text, title_in_text;
    unsigned face;                       /* window and dialog background */
    unsigned taskbar_top, taskbar_bottom, taskbar_line, start_top, start_bottom;
    unsigned select, select_text;        /* selection highlight */
    unsigned sidebar_top, sidebar_bottom;   /* Explorer's task pane */
    unsigned menu_header_top, menu_header_bottom;
    unsigned button_border, hover;
    int index;                           /* which scheme (0 Halcyon Blue, 1 Evergreen, 2 Graphite) */
} hal_theme_t;

/* icons (SYS_GFX_ICON, SYS_WIN_SET_ICON, menus, tool buttons) */
enum { HICON_COMPUTER = 0, HICON_HDD, HICON_USBDRIVE, HICON_FLOPPY, HICON_CDROM, HICON_FOLDER, HICON_FOLDER_OPEN,
       HICON_DOCUMENTS, HICON_FILE_TEXT, HICON_FILE, HICON_FILE_IMAGE, HICON_FILE_APP, HICON_FILE_SYS,
       HICON_NOTEPAD, HICON_TERMINAL, HICON_PAINT, HICON_CALC, HICON_SYSINFO, HICON_CONTROL, HICON_INFO,
       HICON_QUESTION, HICON_ERROR, HICON_WARNING, HICON_SHUTDOWN, HICON_RESTART, HICON_LOGO, HICON_USER,
       HICON_USB, HICON_DISPLAY, HICON_KEYBOARD, HICON_MOUSE, HICON_MEMORY, HICON_HUB, HICON_HELP, HICON_GAME,
       HICON_RECYCLE, HICON_COUNT };

/* message boxes */
enum { MBX_OK = 1, MBX_OKCANCEL, MBX_YESNO, MBX_YESNOCANCEL };
enum { MBX_ICON_INFO = 0, MBX_ICON_WARNING, MBX_ICON_ERROR, MBX_ICON_QUESTION };
enum { IDR_OK = 1, IDR_CANCEL = 2, IDR_YES = 6, IDR_NO = 7 };

#endif
