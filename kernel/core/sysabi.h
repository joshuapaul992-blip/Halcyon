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
    /* threads */
    SYS_THREAD_CREATE = 13, SYS_THREAD_EXIT, SYS_THREAD_JOIN, SYS_FUTEX_WAIT, SYS_FUTEX_WAKE,
    SYS_SET_TLS, SYS_GETTID,
    /* files */
    SYS_OPEN = 20, SYS_CLOSE, SYS_READ, SYS_WRITE, SYS_SEEK, SYS_FSIZE, SYS_TRUNCATE,
    SYS_STAT, SYS_READDIR, SYS_MKDIR, SYS_UNLINK, SYS_RENAME, SYS_VOLUMES,
    /* shell and shared memory */
    SYS_SHELL_OPEN = 33, SYS_SHELL_FORMAT, SYS_EJECT, SYS_SHM_OPEN, SYS_SHM_MAP, SYS_SHM_UNMAP,
    SYS_THREAD_DETACH,
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
     *   50-59, 70-79, 87-89, 94-99, 152-159   more window / drawing / control /
     *           menu calls (gui/usergui.c; 48 and 49 are SYS_WIN_RESIZE and
     *           SYS_WIN_SET_ICON)
     *   140-151 sound (sound/): streams, the master volume, system sounds
     *   100-139 sockets and poll (net/): 100-115 sockets, 116-127 the
     *           internet stack (interfaces, configuration, name lookup),
     *           128 SYS_GETRANDOM, 129-139 free
     *   160     SYS_WIN_CANVAS: the window's pixels mapped into the program
     *   164     SYS_VOLUME_SET_WRITE: drive properties (core/syscall.c)
     *   168-169 Explorer: built-in shell tools, file details (core/syscall.c)
     *   170-171 programs' settings in SETTINGS.INI (core/syscall.c)
     *   176     SYS_KEEP_AWAKE: no screen saver while a program plays (gui/saver.c)
     *   177     SYS_WIN_INVALIDATE_RECT (gui/usergui.c)
     *   180-182 drag and drop (gui/dnd.c, through gui/usergui.c)
     *   183-189 the Recycle Bin and Explorer's settings (apps/shellsys.c) */
    SYS_WIN_CANVAS = 160,
    SYS_DATETIME = 161, SYS_VOLUME_INFO, SYS_VFS_GENERATION,
    SYS_VOLUME_SET_WRITE = 164, /* (drive, allow): allow Halcyon to write to an NTFS volume on an
                                   internal disk (VOLF_WRITE_OPT) or take it back; saved in SETTINGS.INI,
                                   and the volume is mounted again (E_BUSY while files are open on it) */
    SYS_GETRANDOM = 128,        /* (buf, len <= 65536): cryptographically strong bytes */
    SYS_SHELL_APP = 168, SYS_STAT_EX,
    SYS_SETTING_GET = 170,      /* (key, buf, n): the value's length (NUL-terminated in buf), E_NOENT */
    SYS_SETTING_SET = 171,      /* (key, value or NULL to remove): keys "<program>.*" only (E_PERM);
                                   saved in SETTINGS.INI a moment later (changes are coalesced) */
    SYS_KEEP_AWAKE = 176,       /* (on): while on (and the program runs) the screen saver does not start */
    SYS_WIN_INVALIDATE_RECT = 177,  /* (win, x << 48 | y << 32 | w << 16 | h): only that part of the canvas changed */
    SYS_SOUND_FIRST = 140, SYS_SOUND_LAST = 151,
    /* drag and drop between windows of any program and the desktop (DESIGN-M8-EXPLORER.md) */
    SYS_DND_BEGIN = 180,        /* (win, paths, len, effects | icon << 8, count): start a drag of paths
                                   (NUL-separated, len bytes) with the mouse button held in win; the
                                   drag's serial number (> 0), E_AGAIN if no button is held any more */
    SYS_DND_ACCEPT,             /* (win, serial, effect, possible): a target's answer to EVT_DRAG
                                   DND_ENTER / DND_OVER: the effect a drop here would have now, and
                                   every effect it could have (DND_COPY | DND_MOVE; the right-drag menu) */
    SYS_DND_DATA,               /* (serial, buf, size): the paths of a drag (or the last drop): bytes
                                   (with the NULs; more than size: buf holds the first size), E_NOENT */
    /* the Recycle Bin (apps/recycle.c) */
    SYS_RECYCLE_DELETE = 183,   /* (path): move to its drive's bin: the entry's id, E_NOTSUP (no bin on
                                   that drive: delete permanently), E_ROFS, E_NOSPC (too big for it) */
    SYS_RECYCLE_LIST,           /* (drive or 0 = every drive, hal_rbentry_t *buf, max): entries (all of
                                   them if max is 0), oldest first per drive */
    SYS_RECYCLE_RESTORE,        /* (drive, id, mode RBR_*, out path buf, size) */
    SYS_RECYCLE_PURGE,          /* (drive, id): delete one entry for good; id 0 = empty that drive's bin,
                                   drive 0 = every bin */
    SYS_RECYCLE_INFO,           /* (drive, hal_rbinfo_t *) */
    SYS_RECYCLE_SET,            /* (drive, percent, 0): the bin's size limit, or (0, 0, RBF_* options):
                                   saved in SETTINGS.INI */
    SYS_SHELL_SETTING = 189,    /* (key, value or NULL, buf, size): read (value NULL: the length, E_NOENT)
                                   or set (value; "" removes it) one of Explorer's settings: keys
                                   "explorer.*", kept in SETTINGS.INI */
    /* 196-199: graphics cards (drivers/nv/, M10) */
    SYS_NV_QUERY = 196,         /* (NVQ_* op, arg, buf, size): what the NVIDIA identification found, for
                                   NVREPORT.EXE; see NVQ_* below */
    SYS_COUNT = 200
};

/* SYS_NV_QUERY operations. arg packs a card number and an index as gpu << 8 | index. */
#define NVQ_COUNT    0          /* -> NVIDIA cards identified */
#define NVQ_REPORT   1          /* (gpu, or -1 for the machine's display adapters) -> length of the text
                                   (at most size bytes copied, no NUL) */
#define NVQ_ROM      2          /* (gpu << 8 | source) -> size of that video BIOS copy (E_NOENT none,
                                   E_INVAL past the last source) */
#define NVQ_ROMNAME  3          /* (gpu << 8 | source) -> the source's short name ("PRAMIN", NUL-terminated) */
#define NVQ_EDID     4          /* (gpu << 8 | n) -> connector number << 16 | EDID bytes of the card's
                                   nth connector (E_NOENT no EDID, E_INVAL past the last) */
#define NVQ_FBTEST   5          /* (test 0..NV_FBTESTS-1, hal_nvfbtest_t *, sizeof) -> 0: one frame buffer
                                   speed test, about 0.2 s, drawing grey frames on the screen (repainted
                                   after); E_NODEV without a frame buffer */
#define NV_FBTESTS   6
typedef struct hal_nvfbtest {
    int test;                   /* 0 fill (rep stosq), 1 fill (32-bit stores), 2 RAM -> screen,
                                   3 screen -> screen, 4 screen read, 5 RAM -> RAM */
    int cache_type;             /* the frame buffer's page attribute: 1 write-combining, 0 / 7 uncached,
                                   6 write-back, -1 unknown */
    unsigned width, height, pitch, bpp;
    unsigned long long frame_bytes;     /* pitch x height */
    unsigned long long bytes, us;       /* moved in that time */
    unsigned passes, sink;
    char name[48];
} hal_nvfbtest_t;

/* SYS_SHM_OPEN flags */
#define SHM_CREATE 0x01
#define SHM_EXCL   0x02

/* SYS_WIN_CANVAS: the window's pixels, mapped into the program (0x00RRGGBB,
 * stride in pixels). Valid until the next EVT_RESIZE: call it again then. */
typedef struct hal_canvas {
    unsigned int *pixels;
    int w, h, stride;
} hal_canvas_t;

/* SYS_DATETIME: local time from the real-time clock */
typedef struct hal_datetime {
    int year, month, day, hour, minute, second, weekday;   /* weekday 0 = Sunday */
} hal_datetime_t;

/* SYS_VOLUME_INFO */
enum { VOL_HDD = 0, VOL_REMOVABLE, VOL_CDROM, VOL_FLOPPY };
typedef struct hal_volinfo {
    char letter, kind, is_boot, readonly;   /* readonly: nothing on it can be changed (VOLF_* say why) */
    char label[64];           /* UTF-8, up to 32 characters (NTFS) */
    char fs[8];               /* "FAT32", "exFAT", "ISO9660", "NTFS" */
    char device[16];          /* "sata0p1", "usb0", "cd0" */
    unsigned long long total, free_bytes;
    unsigned long long serial;    /* the volume's serial number, 0 = none */
    unsigned int flags;           /* VOLF_* */
    unsigned int pad;
    char ro_reason[128];          /* why it is read-only, "" if writable or not known */
} hal_volinfo_t;
#define VOLF_DEVICE_RO   0x01     /* the device is write-protected (or a CD) */
#define VOLF_FS_RO       0x02     /* the file system driver mounted it read-only (ro_reason) */
#define VOLF_WRITE_OPT   0x04     /* NTFS on an internal disk: writing needs the user's permission,
                                     which SYS_VOLUME_SET_WRITE gives or takes back */
#define VOLF_WRITE_OK    0x08     /* ... and the user has allowed it (SETTINGS.INI ntfs.write.<serial>) */

/* SYS_SHELL_APP: open one of the shell's built-in tools (does not wait) */
enum { SHELL_APP_TERMINAL = 1,   /* Command Prompt; arg: its folder, or NULL */
       SHELL_APP_SYSINFO,        /* System Information */
       SHELL_APP_CONTROL,        /* Control Panel */
       SHELL_APP_ABOUT,          /* About Halcyon */
       SHELL_APP_NOTEPAD };      /* Notepad; arg: a file to open, or NULL */

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

/* SYS_STAT_EX: SYS_STAT's entry and the details a Properties dialog shows */
typedef struct hal_statx {
    hal_dirent_t d;                       /* as SYS_STAT */
    unsigned short cyear; unsigned char cmonth, cday, chour, cminute, csecond, pad;   /* created (year 0: unknown) */
    char short_name[16];                  /* 8.3 name ("README.TXT"); "" if the file system has none */
    char model[48];                       /* the device holding the file ("QEMU HARDDISK"), or "" */
} hal_statx_t;

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
    char version[24];                   /* "1.0.346": major.minor.build */
    unsigned build;                     /* the build number (commits in the history built) */
    int pad2;
} hal_sysinfo_t;

/* window events (SYS_WIN_GET_EVENT) */
enum { EVT_NONE = 0, EVT_PAINT, EVT_CLOSE, EVT_KEY, EVT_MOUSE_DOWN, EVT_MOUSE_UP, EVT_MOUSE_MOVE, EVT_DBLCLICK,
       EVT_WHEEL, EVT_COMMAND, EVT_TIMER, EVT_RESIZE, EVT_FOCUS,
       EVT_MOUSE_LEAVE,              /* the pointer left the window (clear hover highlights) */
       EVT_MENU,                     /* a menu item was chosen: id = its id */
       EVT_SCROLL,                   /* a scroll bar moved: id = control, x = new position, button = SCROLL_* part */
       EVT_DRAG };                   /* drag and drop: button = DND_* (below) */
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

/* EVT_DRAG (button field): DND_ENTER, DND_OVER (x, y: the pointer, client coordinates; answer with
 * SYS_DND_ACCEPT, else nothing can be dropped), DND_LEAVE, DND_DROP (pressed = the effect chosen:
 * do it, the paths come from SYS_DND_DATA), DND_END (to the source window: pressed = the effect of
 * the drop, 0 if canceled). id = the drag's serial number, dz = how many paths, vk = the effects the
 * source allows, mods = the modifier keys, buttons = the button that drags (MOUSE_RIGHT: the effect
 * comes from a "Copy Here / Move Here" menu at the drop) */
enum { DND_ENTER = 1, DND_OVER, DND_LEAVE, DND_DROP, DND_END };
#define DND_NONE 0
#define DND_COPY 1
#define DND_MOVE 2

/* SYS_RECYCLE_LIST entry */
typedef struct hal_rbentry {
    unsigned id;                          /* per drive */
    char drive;
    unsigned char is_dir, pad[2];
    unsigned long long size;              /* bytes (a folder: all of it) */
    unsigned short year; unsigned char month, day, hour, minute, second, pad2;   /* deleted */
    char orig[520];                       /* where it was: "C:/Documents/a.txt" */
} hal_rbentry_t;
/* SYS_RECYCLE_RESTORE modes: when something is at the original place now */
enum { RBR_FAIL = 0, RBR_REPLACE, RBR_RENAME };
/* SYS_RECYCLE_INFO */
typedef struct hal_rbinfo {
    char drive, pad[3];
    int supported;                        /* 0, E_ROFS (nothing can be deleted), E_NOTSUP (removable:
                                             deletes are permanent), E_NODEV */
    int count, percent;                   /* entries; the share of the drive the bin may use */
    unsigned long long bytes, limit, total;   /* in the bin; the most it may hold; the drive's size */
    unsigned flags;                       /* RBF_* (the same for every drive) */
    unsigned generation;                  /* changes with every change of any bin */
} hal_rbinfo_t;
#define RBF_CONFIRM 0x01                  /* ask before moving things to the bin */
#define RBF_NUKE    0x02                  /* no bin: delete at once */

/* EVT_COMMAND notifications (button field) */
#define CMD_CLICK    0           /* button / tool button clicked, check box or list selection changed */
#define CMD_ACTIVATE 1           /* CTL_NOTIFY: Enter in a text box, double click or Enter in a list box */
#define CMD_CHANGE   2           /* CTL_CHANGES: the text of a text box changed (typing, cut, paste) */

/* EVT_SCROLL parts (button field) */
enum { SCROLL_LINE_UP = 1, SCROLL_LINE_DOWN, SCROLL_PAGE_UP, SCROLL_PAGE_DOWN, SCROLL_THUMB_TRACK,
       SCROLL_THUMB_END, SCROLL_WHEEL };

/* SYS_WIN_CREATE flags */
#define WIN_RESIZABLE 0x01
#define WIN_DIALOG    0x02
#define WIN_NO_MAX    0x04
#define WIN_FULLSCREEN 0x100     /* no frame, the whole screen, above the taskbar (screen savers, video);
                                    w and h are ignored. While one is shown the screen saver waits. */
#define WIN_PREVIEW   0x200      /* a screen saver's "/p <handle>" preview: the window is drawn inside
                                    the box Display Properties shows; win_create's 5th argument is the
                                    handle, w and h are the box's (win_create_preview) */

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
#define CTL_CHANGES  0x200       /* text box: send EVT_COMMAND with CMD_CHANGE on every change by the user */

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
       HICON_RECYCLE, HICON_WEB, HICON_BACK, HICON_FORWARD, HICON_RELOAD, HICON_STOP, HICON_HOME,
       HICON_NETWORK, HICON_NETWORK_OFF, HICON_SPEAKER, HICON_SPEAKER_MUTED, HICON_FILE_AUDIO, HICON_MEDIA,
       HICON_RECYCLE_FULL,          /* HICON_RECYCLE is the empty bin */
       HICON_SETUP,                 /* Install Halcyon */
       HICON_COUNT };

/* message boxes */
enum { MBX_OK = 1, MBX_OKCANCEL, MBX_YESNO, MBX_YESNOCANCEL };
enum { MBX_ICON_INFO = 0, MBX_ICON_WARNING, MBX_ICON_ERROR, MBX_ICON_QUESTION };
enum { IDR_OK = 1, IDR_CANCEL = 2, IDR_YES = 6, IDR_NO = 7 };

/* sockets and poll (100-139) */
#include "../net/sockabi.h"
/* sound (140-151) */
#include "../sound/sndabi.h"

#endif
