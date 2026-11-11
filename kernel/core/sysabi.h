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
    SYS_GFX_FILL = 60, SYS_GFX_RECT, SYS_GFX_LINE, SYS_GFX_TEXT, SYS_GFX_TEXT_WIDTH, SYS_GFX_BLIT,
    SYS_GFX_ICON, SYS_GFX_BEVEL, SYS_GFX_CIRCLE, SYS_GFX_FONT_HEIGHT,
    SYS_CTL_CREATE = 80, SYS_CTL_SET_TEXT, SYS_CTL_GET_TEXT, SYS_CTL_SET_VALUE, SYS_CTL_GET_VALUE,
    SYS_CTL_SET_STYLE, SYS_CTL_LIST_SET,
    SYS_MSGBOX = 90, SYS_FILE_DIALOG, SYS_CLIPBOARD_SET, SYS_CLIPBOARD_GET,
    SYS_COUNT = 100
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
       EVT_MOUSE_LEAVE };            /* the pointer left the window (clear hover highlights) */
typedef struct hal_event {
    int type;
    int win;                     /* window handle */
    int x, y;                    /* mouse: client coordinates; EVT_RESIZE: new client size */
    int button, buttons, dz;
    int id;                      /* EVT_COMMAND: control id; EVT_KEY: key code (usage) */
    int pressed, mods;           /* EVT_KEY */
    unsigned int ch;             /* EVT_KEY: character (Unicode code point) or 0 */
} hal_event_t;

/* SYS_WIN_CREATE flags */
#define WIN_RESIZABLE 0x01
#define WIN_DIALOG    0x02
#define WIN_NO_MAX    0x04

/* fonts for SYS_GFX_TEXT* */
enum { FONT_UI = 0, FONT_UI_BOLD, FONT_TITLE, FONT_MONO, FONT_LARGE, FONT_HUGE, FONT_SMALL };

/* controls (SYS_CTL_CREATE) */
enum { CTL_BUTTON = 1, CTL_LABEL, CTL_TEXTBOX, CTL_CHECKBOX, CTL_LISTBOX, CTL_GROUP, CTL_PROGRESS };
#define CTL_DEFAULT  0x01
#define CTL_CANCEL   0x02
#define CTL_DISABLED 0x04
#define CTL_HIDDEN   0x08
#define CTL_PASSWORD 0x10
#define CTL_BOLD     0x20
#define CTL_READONLY 0x40
#define CTL_WRAP     0x80

/* message boxes */
enum { MBX_OK = 1, MBX_OKCANCEL, MBX_YESNO, MBX_YESNOCANCEL };
enum { MBX_ICON_INFO = 0, MBX_ICON_WARNING, MBX_ICON_ERROR, MBX_ICON_QUESTION };
enum { IDR_OK = 1, IDR_CANCEL = 2, IDR_YES = 6, IDR_NO = 7 };

#endif
