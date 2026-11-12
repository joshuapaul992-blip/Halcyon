/* Halcyon user-mode runtime: system calls, a small C library, GUI calls.
 * Programs are static position-independent ELF64 executables (.EXE). */
#ifndef HAL_H
#define HAL_H
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include "../../kernel/core/sysabi.h"

/* error codes (negative results) */
#define E_IO      -1
#define E_NOMEM   -2
#define E_NOENT   -3
#define E_EXIST   -4
#define E_NOTDIR  -5
#define E_ISDIR   -6
#define E_NOSPC   -7
#define E_INVAL   -8
#define E_NOTSUP  -11
#define E_NOTEMPTY -13
#define E_ROFS    -14
#define E_BUSY    -16
#define E_CANCELED -18
#define E_PERM    -19
#define E_TIMEOUT -9
#define E_AGAIN   -12     /* would wait (non-blocking), or a socket timeout passed */
#define E_NAMETOOLONG -17
#define E_ADDRINUSE   -20 /* the name / address is taken */
#define E_NOTCONN     -21 /* the socket is not connected */
#define E_ISCONN      -22 /* the socket is already connected */
#define E_CONNREFUSED -23 /* nobody accepts connections / messages there */
#define E_CONNRESET   -24 /* the peer went away (data was lost) */
#define E_PIPE        -25 /* the peer does not take any more data */
#define E_AFNOSUPPORT -26 /* address family not supported */
#define E_MSGSIZE     -27 /* message too long */
#define E_NOTSOCK     -28 /* the handle is not a socket */
#define E_INPROGRESS  -29 /* a non-blocking connect has started */
#define E_HOSTUNREACH -30 /* no answer from that computer */
#define E_NETUNREACH  -31 /* no route to that network (not connected?) */
#define E_ADDRNOTAVAIL -32 /* not an address of this computer */
#define E_NODEV   -15     /* (resolve: no DNS server configured) */
const char *hal_strerror(int err);

/* ---- raw system calls ---- */
static inline long syscall6(long n, long a, long b, long c, long d, long e, long f) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}
#define SC0(n) syscall6(n, 0, 0, 0, 0, 0, 0)
#define SC1(n, a) syscall6(n, (long)(a), 0, 0, 0, 0, 0)
#define SC2(n, a, b) syscall6(n, (long)(a), (long)(b), 0, 0, 0, 0)
#define SC3(n, a, b, c) syscall6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define SC4(n, a, b, c, d) syscall6(n, (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)
#define SC5(n, a, b, c, d, e) syscall6(n, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), 0)
#define SC6(n, a, b, c, d, e, f) syscall6(n, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f))

/* ---- processes ---- */
__attribute__((noreturn)) void exit(int code);
int  spawn(const char *path, const char *args);      /* pid or error */
int  wait(int pid);                                   /* exit code */
static inline int getpid(void) { return (int)SC0(SYS_GETPID); }
static inline void yield(void) { SC0(SYS_YIELD); }
static inline void sleep_ms(unsigned ms) { SC1(SYS_SLEEP, ms); }
static inline uint64_t time_ms(void) { return (uint64_t)SC0(SYS_TIME); }
/* end a program: its ancestors may; for any other, the user is asked (E_PERM if refused) */
static inline int kill(int pid) { return (int)SC1(SYS_KILL, pid); }
static inline int getcwd(char *buf, size_t n) { return (int)SC2(SYS_GETCWD, buf, n); }
static inline int chdir(const char *p) { return (int)SC1(SYS_CHDIR, p); }
static inline int proclist(hal_procinfo_t *buf, int max) { return (int)SC2(SYS_PROCLIST, buf, max); }
static inline int sysinfo(hal_sysinfo_t *si) { return (int)SC1(SYS_SYSINFO, si); }
void *sbrk(long incr);

/* ---- threads (several per program; each has its own 1 MiB stack) ---- */
typedef struct { volatile int v; } hal_mutex_t;    /* zero-initialised = unlocked */
typedef struct { volatile int seq; } hal_cond_t;   /* zero-initialised */
/* start fn(arg) in a new thread; returns its id (> 0) or an error. fn's return
 * value is the thread's exit code (thread_join). At most 31 extra threads. */
int  thread_create(int (*fn)(void *), void *arg);
__attribute__((noreturn)) void thread_exit(int code);   /* this thread only; exit() ends the program */
static inline int thread_join(int tid) { return (int)SC1(SYS_THREAD_JOIN, tid); }      /* its exit code */
static inline int thread_detach(int tid) { return (int)SC1(SYS_THREAD_DETACH, tid); }  /* no join needed */
static inline int gettid(void) { return (int)SC0(SYS_GETTID); }                         /* main thread: 1 */
static inline int set_tls(void *base) { return (int)SC1(SYS_SET_TLS, base); }          /* this thread's FS base */
/* futexes: wait while *addr == expected (timeout_ms < 0: no timeout; 0, E_AGAIN,
 * E_TIMEOUT), wake up to n waiters (returns how many) */
static inline int futex_wait(volatile int *addr, int expected, int timeout_ms) {
    return (int)SC3(SYS_FUTEX_WAIT, addr, expected, (long)timeout_ms);
}
static inline int futex_wake(volatile int *addr, int n) { return (int)SC2(SYS_FUTEX_WAKE, addr, n); }
void mutex_lock(hal_mutex_t *m);
int  mutex_trylock(hal_mutex_t *m);                  /* 1 if locked */
void mutex_unlock(hal_mutex_t *m);
void cond_wait(hal_cond_t *c, hal_mutex_t *m);
int  cond_wait_ms(hal_cond_t *c, hal_mutex_t *m, int timeout_ms);   /* 0 or E_TIMEOUT */
void cond_signal(hal_cond_t *c);
void cond_broadcast(hal_cond_t *c);

/* ---- shared memory between programs: a name ("halcyon.demo": letters, digits,
 * . - _) opened by several programs maps the same pages ---- */
static inline int shm_open(const char *name, size_t size, int flags) { return (int)SC3(SYS_SHM_OPEN, name, size, flags); }
void *shm_map(int h, size_t *size);                  /* NULL on error; close() the handle when done */
static inline int shm_unmap(void *p) { return (int)SC1(SYS_SHM_UNMAP, p); }

/* ---- random bytes (ChaCha20 seeded from the CPU's generator; for keys) ---- */
static inline long getrandom(void *buf, size_t len) { return SC2(SYS_GETRANDOM, buf, len); }   /* bytes (<= 65536) */

/* ---- clock and volumes ---- */
static inline int datetime(hal_datetime_t *d) { return (int)SC1(SYS_DATETIME, d); }
static inline int volume_info(char drive, hal_volinfo_t *vi) { return (int)SC2(SYS_VOLUME_INFO, drive, vi); }
/* changes whenever a volume is mounted or removed (poll it to refresh drive lists) */
static inline unsigned vfs_generation(void) { return (unsigned)SC0(SYS_VFS_GENERATION); }

/* ---- the shell ---- */
static inline int shell_open(const char *path) { return (int)SC1(SYS_SHELL_OPEN, path); }   /* like a double click */
static inline int shell_format(char drive) { return (int)SC1(SYS_SHELL_FORMAT, drive); }    /* the user decides in it */
/* allow Halcyon to write to an NTFS volume on an internal disk (hal_volinfo_t
 * flags VOLF_WRITE_OPT / VOLF_WRITE_OK), or take it back: the volume is
 * mounted again (E_BUSY while files are open on it: then nothing changes) */
static inline int volume_set_write(char drive, int allow) { return (int)SC2(SYS_VOLUME_SET_WRITE, drive, allow); }
static inline int eject(char drive) { return (int)SC1(SYS_EJECT, drive); }                  /* CD/DVD drives */
/* open a built-in tool (SHELL_APP_*: Command Prompt in folder arg, Notepad
 * with file arg, System Information, Control Panel, About); does not wait */
static inline int shell_app(int app, const char *arg) { return (int)SC2(SYS_SHELL_APP, app, arg); }

/* ---- settings kept in C:\HALCYON\SETTINGS.INI. Any key can be read; a
 * program writes only keys named after it: "paint.*" for PAINT.EXE,
 * "marquee.*" for MARQUEE.SCR (else E_PERM), of letters, digits, '.', '-',
 * '_', with one-line values up to 599 bytes, at most 64 keys per program.
 * Changes are written to the file a moment later, coalesced, and survive
 * the program ending at once. ---- */
/* the value's length (the value, NUL-terminated, in buf), or E_NOENT */
static inline int setting_get(const char *key, char *buf, int n) { return (int)SC3(SYS_SETTING_GET, key, buf, n); }
static inline int setting_set(const char *key, const char *value) { return (int)SC2(SYS_SETTING_SET, key, value); }   /* NULL removes */
int  setting_get_int(const char *key, int def);
int  setting_set_int(const char *key, int value);

/* ---- the screen saver: while on (and the program runs), it does not
 * start (Media Player while it plays). A WIN_FULLSCREEN window keeps it
 * away too. ---- */
static inline int keep_awake(int on) { return (int)SC1(SYS_KEEP_AWAKE, on); }

/* ---- files (fd 0/1/2: console) ---- */
static inline int open(const char *p, int flags) { return (int)SC2(SYS_OPEN, p, flags); }
static inline int close(int fd) { return (int)SC1(SYS_CLOSE, fd); }
static inline long read(int fd, void *b, size_t n) { return SC3(SYS_READ, fd, b, n); }
static inline long write(int fd, const void *b, size_t n) { return SC3(SYS_WRITE, fd, b, n); }
static inline long seek(int fd, long off, int whence) { return SC3(SYS_SEEK, fd, off, whence); }
static inline long fsize(int fd) { return SC1(SYS_FSIZE, fd); }
static inline int stat(const char *p, hal_dirent_t *d) { return (int)SC2(SYS_STAT, p, d); }
/* stat plus creation time, 8.3 name and the device's model (hal_statx_t) */
static inline int stat_ex(const char *p, hal_statx_t *x) { return (int)SC2(SYS_STAT_EX, p, x); }
static inline int readdir(const char *p, hal_dirent_t *buf, int max) { return (int)SC3(SYS_READDIR, p, buf, max); }
static inline int mkdir(const char *p) { return (int)SC1(SYS_MKDIR, p); }
static inline int unlink(const char *p) { return (int)SC1(SYS_UNLINK, p); }
static inline int rename(const char *a, const char *b) { return (int)SC2(SYS_RENAME, a, b); }
/* whole file into a NUL-terminated malloc()ed buffer */
int  read_file(const char *path, char **data, long *size);
int  write_file(const char *path, const void *data, long size);

/* ---- C library subset ---- */
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, size_t n);
int   strcasecmp(const char *a, const char *b);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
size_t strlcpy(char *d, const char *s, size_t n);
size_t strlcat(char *d, const char *s, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *h, const char *n);
char *strdup(const char *s);
int   toupper(int c);
int   tolower(int c);
int   isdigit(int c);
int   isspace(int c);
int   isalpha(int c);
int   isalnum(int c);
long  strtol(const char *s, char **end, int base);
int   atoi(const char *s);
int   abs(int v);
void *malloc(size_t n);
void *calloc(size_t n, size_t m);
void *realloc(void *p, size_t n);
void  free(void *p);
unsigned rand(void);
void  srand(unsigned seed);

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int puts(const char *s);
int putchar(int c);
/* a line from the console (without the newline); -1 at end of input */
int getline(char *buf, int n);

/* ---- windows (see gui.c) ---- */
typedef struct { int x, y, w, h; } hal_rect_t;
#define RGB(r, g, b) ((unsigned)(((r) << 16) | ((g) << 8) | (b)))
int  win_create(const char *title, int w, int h, int flags);   /* client size; window handle */
int  win_create_preview(const char *title, int handle);           /* a screen saver's "/p <handle>" window (WIN_PREVIEW) */
/* only the rectangle x, y, w, h of the canvas changed: cheaper than win_invalidate (no controls over it) */
static inline void win_invalidate_rect(int win, int x, int y, int w, int h) {
    SC2(SYS_WIN_INVALIDATE_RECT, win, (long)((unsigned long)(unsigned short)x << 48 | (unsigned long)(unsigned short)y << 32 |
                                             (unsigned long)(unsigned short)w << 16 | (unsigned short)h));
}
void win_destroy(int win);
void win_set_title(int win, const char *title);
void win_invalidate(int win);                                    /* repaint the client area */
/* next event of any of the program's windows; timeout_ms < 0 waits forever.
 * Returns 1 with *ev filled, 0 on timeout. EVT_PAINT arrives when the client
 * area must be drawn (draw it, then return to the event loop). */
int  win_get_event(hal_event_t *ev, int timeout_ms);
void win_size(int win, int *w, int *h);
void win_set_timer(int win, int ms);                             /* EVT_TIMER every ms (0 = off) */
/* the client area's pixels mapped into the program: write 0x00RRGGBB at
 * c->pixels[y * c->stride + x], then win_invalidate(). Call again after each
 * EVT_RESIZE (the old mapping then shows nothing); 0 or an error. */
static inline int win_canvas(int win, hal_canvas_t *c) { return (int)SC2(SYS_WIN_CANVAS, win, c); }
void win_resize(int win, int w, int h);                          /* new client size; EVT_RESIZE + EVT_PAINT follow */
void win_set_icon(int win, int icon);                            /* title bar / taskbar icon (as gfx_icon) */

void gfx_fill(int win, int x, int y, int w, int h, unsigned color);
void gfx_rect(int win, int x, int y, int w, int h, unsigned color);
void gfx_line(int win, int x0, int y0, int x1, int y1, unsigned color);
int  gfx_text(int win, int x, int y, const char *s, unsigned color, int font);   /* returns end x */
int  gfx_text_width(const char *s, int font);
int  gfx_font_height(int font);
void gfx_blit(int win, int x, int y, int w, int h, const unsigned *pixels);    /* 0xRRGGBB */
void gfx_icon(int win, int x, int y, int icon);
void gfx_bevel(int win, int x, int y, int w, int h, int raised);
void gfx_circle(int win, int cx, int cy, int r, unsigned color, int filled);

int  ctl_create(int win, int type, int x, int y, int w, int h, const char *text, int id, int style);
void ctl_set_text(int win, int id, const char *text);
int  ctl_get_text(int win, int id, char *buf, int n);
void ctl_set_value(int win, int id, int v);
int  ctl_get_value(int win, int id);
void ctl_set_style(int win, int id, int style);
void ctl_list_set(int win, int id, const char *const *items, int n);

int  msgbox(int win, const char *title, const char *text, int icon, int buttons);
int  file_dialog(int win, int save, const char *title, const char *dir, const char *ext, char *out, int n);
void clipboard_set(const char *text);                            /* up to 1023 bytes */
int  clipboard_get(char *buf, int n);                            /* bytes copied; n = 0: length of the text */
int  clipboard_set_n(const char *text, int len);                 /* any text up to 1 MiB (SYS_CLIPBOARD_SET_N) */

/* ---- menus (SYS_MENU_*) ----
 * A menu bar is a flat array: each MENU_TITLE entry starts a menu, the
 * entries after it are its items (hal_menu_item_t in sysabi.h). '&' marks a
 * mnemonic ("&File" opens with Alt+F; "E&xit" is chosen with X while the
 * menu is open). F10 or tapping Alt opens the first menu; arrows, Enter and
 * Esc work as in Windows. Choosing an item queues EVT_MENU with its id.
 * The bar sits above the client area: adding one grows the window and leaves
 * the canvas, coordinates and controls as they were. The kernel copies the
 * items (up to MENU_MAX_ITEMS entries and MENU_MAX_TITLES menus; labels up to
 * 63 bytes, shortcuts up to 31) and frees them with the window.
 *
 *   static const hal_menu_item_t bar[] = {
 *       { "&File", 0, 0, MENU_TITLE, 0, 0 },
 *       { "&Open...", "Ctrl+O", ID_OPEN, 0, HICON_FOLDER_OPEN, 0 },
 *       { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
 *       { "E&xit", 0, ID_EXIT, 0, 0, 0 },
 *       { "&View", 0, 0, MENU_TITLE, 0, 0 },
 *       { "&Status Bar", 0, ID_STATUS, MENU_CHECKED, 0, 0 },
 *   };
 *   menu_set(win, bar, 6);
 */
int  menu_set(int win, const hal_menu_item_t *items, int n);    /* set or replace the menu bar; n = 0 removes it */
/* a context menu at client position x, y (the window must be the active one,
 * e.g. in answer to EVT_MOUSE_DOWN); no titles. The choice arrives as
 * EVT_MENU; nothing arrives when the menu is dismissed. */
int  menu_popup(int win, const hal_menu_item_t *items, int n, int x, int y);
/* change MENU_DISABLED / MENU_CHECKED / MENU_BOLD of every item with this id
 * (in the bar and the last popup); returns the number of items changed */
int  menu_state(int win, int id, int mask, int flags);
static inline int menu_enable(int win, int id, int on) { return menu_state(win, id, MENU_DISABLED, on ? 0 : MENU_DISABLED); }
static inline int menu_check(int win, int id, int on) { return menu_state(win, id, MENU_CHECKED, on ? MENU_CHECKED : 0); }

/* ---- more window calls ---- */
void win_set_cursor(int win, int shape);         /* CUR_* while the pointer is over the client area */
/* the client area on screen (below the menu bar); returns 0 normal, 1 minimized, 2 maximized */
int  win_get_rect(int win, hal_rect_t *r);
void win_set_min_size(int win, int w, int h);    /* smallest client size the user can resize to */
/* make win a dialog of owner (another window of this program; -1 = none).
 * modal: the owner takes no clicks or keys while win is open. A dialog
 * closes with its owner. */
int  win_set_owner(int win, int owner, int modal);
/* Mouse capture is automatic: after a press in the client area, the window
 * gets every EVT_MOUSE_MOVE and the EVT_MOUSE_UP until the buttons are
 * released, also outside the window (coordinates then lie outside the
 * client area). Mouse events carry the modifier keys in mods (MOD_SHIFT...). */

/* EVT_KEY: mods and key codes (USB HID usages, as kernel/drivers/input.h) */
#define MOD_CTRL  0x01
#define MOD_SHIFT 0x02
#define MOD_ALT   0x04
#define KEY_A 0x04                       /* ..KEY_A + 25 = Z; prefer ev.vk for letter shortcuts */
#define KEY_1 0x1E                       /* ..0x26 = 9, 0x27 = 0 */
#define KEY_ENTER 0x28
#define KEY_ESC 0x29
#define KEY_BACKSPACE 0x2A
#define KEY_TAB 0x2B
#define KEY_SPACE 0x2C
#define KEY_F1 0x3A                      /* ..KEY_F1 + 11 = F12 */
#define KEY_F2 0x3B
#define KEY_F3 0x3C
#define KEY_F4 0x3D
#define KEY_F5 0x3E
#define KEY_F9 0x42
#define KEY_F10 0x43
#define KEY_INSERT 0x49
#define KEY_HOME 0x4A
#define KEY_PAGEUP 0x4B
#define KEY_DELETE 0x4C
#define KEY_END 0x4D
#define KEY_PAGEDOWN 0x4E
#define KEY_RIGHT 0x4F
#define KEY_LEFT 0x50
#define KEY_DOWN 0x51
#define KEY_UP 0x52
#define KEY_KP_ENTER 0x58
#define KEY_MENU 0x65                    /* the context menu key */
/* mouse buttons (ev.button, ev.buttons) */
#define MOUSE_LEFT   0x01
#define MOUSE_RIGHT  0x02
#define MOUSE_MIDDLE 0x04
/* Ctrl+letter shortcuts on every keyboard layout (as the kernel's key_shortcut) */
static inline int key_shortcut(const hal_event_t *ev, char letter) {
    return ev->type == EVT_KEY && ev->pressed && (ev->mods & (MOD_CTRL | MOD_ALT)) == MOD_CTRL && ev->vk == letter;
}

/* ---- clipping, text, shapes ---- */
/* Limit the drawing calls of win to a rectangle (canvas coordinates), e.g. a
 * text area next to the program's own scroll bars; with gfx_clip_origin the
 * coordinates of later calls are also relative to its corner. */
void gfx_clip(int win, int x, int y, int w, int h);
void gfx_clip_origin(int win, int x, int y, int w, int h);
void gfx_clip_reset(int win);                    /* the whole canvas, origin 0,0 */
/* text of n bytes (no NUL needed, up to 2048); returns the end x */
int  gfx_text_n(int win, int x, int y, const char *s, int n, unsigned color, int font);
/* text in a box: align 0 left, 1 center, 2 right; vertically centered; cut with "..." when too long */
void gfx_text_box(int win, int x, int y, int w, int h, const char *s, unsigned color, int font, int align);
/* shaded shapes (SHADE_* in sysabi.h): gradients, rounded "XP" rectangles, sunken fields... */
void gfx_shade(int win, int x, int y, int w, int h, unsigned c1, unsigned c2, int kind, int param);
static inline void gfx_gradient(int win, int x, int y, int w, int h, unsigned top, unsigned bottom) {
    gfx_shade(win, x, y, w, h, top, bottom, SHADE_GRADIENT_V, 0);
}
static inline void gfx_rounded(int win, int x, int y, int w, int h, int radius, unsigned top, unsigned bottom) {
    gfx_shade(win, x, y, w, h, top, bottom, SHADE_ROUNDED, radius);
}
static inline void gfx_rounded_outline(int win, int x, int y, int w, int h, int radius, unsigned c) {
    gfx_shade(win, x, y, w, h, c, 0, SHADE_ROUNDED_OUTLINE, radius);
}
int  gfx_theme(hal_theme_t *t);                  /* the current colour scheme */

/* ---- text measurement without a system call per string ----
 * Advance widths are copied from the kernel once per font and block of 256
 * code points (gfx_font_metrics) and cached; these give the same widths as
 * gfx_text. */
int  gfx_font_metrics(int font, unsigned first, int count, unsigned char *adv, hal_fontinfo_t *info);
int  text_char_width(unsigned cp, int font);
int  text_width_n(const char *s, int n, int font);           /* n bytes of UTF-8 */
int  text_width(const char *s, int font);
int  text_fit(const char *s, int n, int font, int width);     /* bytes (whole characters) that fit in width */
int  text_height(int font);                                   /* line height (as gfx_font_height) */
int  text_ascent(int font);
/* UTF-8 (invalid bytes count as one Latin-1 character each, as the kernel draws them) */
unsigned utf8_get(const char *s, int len, int *pos);          /* code point at *pos, advances *pos */
int  utf8_put(unsigned cp, char *out);                        /* 1..4 bytes */
int  utf8_prev(const char *s, int pos);                       /* start of the character before pos */

/* ---- more controls ---- */
void ctl_move(int win, int id, int x, int y, int w, int h);  /* e.g. on EVT_RESIZE (status bars stay docked) */
/* keyboard focus: id of a control, or 0 for the program's own drawing (its
 * EVT_KEY). Returns the id that had it; ctl_get_focus asks only. */
int  ctl_focus(int win, int id);
static inline int ctl_get_focus(int win) { return ctl_focus(win, -1); }
/* scroll bars (CTL_VSCROLL / CTL_HSCROLL): positions 0..total-page, the
 * arrows move by line, the track by page; the user's changes arrive as
 * EVT_SCROLL (x = new position). ctl_set_value / ctl_get_value: the position. */
void ctl_scroll_set(int win, int id, int total, int page, int pos, int line);
/* icon (HICON_*, -1 none) of a CTL_TOOLBUTTON and the tooltip of any control (NULL none) */
void ctl_set_tool(int win, int id, int icon, const char *tip);

/* a modal "enter a value" dialog over owner (prompt, a text box with buf's
 * text, OK / Cancel). Returns 1 and fills buf on OK. Events of other windows
 * that arrive meanwhile are kept for win_get_event. */
int  inputbox(int owner, const char *title, const char *prompt, char *buf, int n);

/* ---- sockets and poll (see net.c and docs/DESIGN-M9.md) ----
 * BSD sockets. Sockets are handles: read(), write() and close() work on them
 * too. Results are >= 0 or a negative E_* code; E_AGAIN means the call would
 * have to wait (non-blocking socket, MSG_DONTWAIT) or SO_RCVTIMEO /
 * SO_SNDTIMEO passed. Every wait ends when the program is ended.
 *
 * AF_LOCAL (= AF_UNIX) connects programs on this computer by name, in one
 * flat namespace: 1 to 63 characters from A-Z a-z 0-9 . - _ starting with a
 * letter or digit, case-insensitive, e.g. "halcyon.clipboard" (use
 * "yourname.program[.service]"; "halcyon." is for the programs that come with
 * Halcyon). A name is taken until its socket is closed or its program ends.
 * SOCK_STREAM: listen / accept / connect, bytes in order; SOCK_DGRAM:
 * messages (up to 64 KiB) with boundaries kept. AF_INET: the internet (see
 * below).
 *
 * Limits: each socket's receive buffer (SO_RCVBUF, 64 KiB by default, 256 KiB
 * at most) is the back-pressure: a sender waits while the receiver's buffer
 * is full. A listen() backlog holds at most 16 connections. A program has at
 * most 128 sockets and 4 MiB in their buffers; see sockstat(). */
int  socket(int domain, int type, int protocol);           /* type: SOCK_STREAM / SOCK_DGRAM, | SOCK_NONBLOCK */
int  bind(int fd, const struct sockaddr *addr, socklen_t len);
int  listen(int fd, int backlog);
int  accept(int fd, struct sockaddr *addr, socklen_t *len);   /* addr / len may be 0 */
int  accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags);   /* flags: SOCK_NONBLOCK */
int  connect(int fd, const struct sockaddr *addr, socklen_t len);
long send(int fd, const void *buf, size_t n, int flags);    /* blocking streams send everything */
long recv(int fd, void *buf, size_t n, int flags);          /* 0 = end of stream */
long sendto(int fd, const void *buf, size_t n, int flags, const struct sockaddr *to, socklen_t tolen);
long recvfrom(int fd, void *buf, size_t n, int flags, struct sockaddr *from, socklen_t *fromlen);
int  shutdown(int fd, int how);                             /* SHUT_RD / SHUT_WR / SHUT_RDWR */
int  getsockname(int fd, struct sockaddr *addr, socklen_t *len);
int  getpeername(int fd, struct sockaddr *addr, socklen_t *len);
int  socketpair(int domain, int type, int protocol, int sv[2]);   /* two connected sockets */
int  setsockopt(int fd, int level, int name, const void *val, socklen_t len);
int  getsockopt(int fd, int level, int name, void *val, socklen_t *len);
int  ioctlsocket(int fd, long cmd, unsigned long *arg);     /* FIONBIO, FIONREAD (as in Winsock) */
/* wait until one of the handles is ready, at most timeout_ms (< 0: no
 * limit); returns how many entries have revents set, 0 on timeout. fd may be
 * a socket, a file (always ready), 1 / 2 (console output), FD_WINDOWS (the
 * program's window events: then call win_get_event(&ev, 0)), or < 0 (ignored) */
int  poll(struct pollfd *fds, unsigned nfds, int timeout_ms);
int  sockstat(hal_sockstat_t *st);                          /* socket use and limits */

/* helpers */
socklen_t local_addr(struct sockaddr_local *a, const char *name);   /* fill an AF_LOCAL address; its length */
int  sock_nonblock(int fd, int on);
int  sock_timeouts(int fd, int recv_ms, int send_ms);      /* SO_RCVTIMEO / SO_SNDTIMEO; 0 = none */
int  local_listen(const char *name, int backlog);          /* stream socket bound to name, listening */
int  local_connect(const char *name);                      /* stream socket connected to name */

/* ---- the internet (AF_INET, IPv4; see docs/DESIGN-M6.md) ----
 * socket(AF_INET, SOCK_STREAM, 0) is TCP, SOCK_DGRAM UDP, and
 * socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP) a "ping socket": send ICMP echo
 * requests (type 8: at least the 8-byte header; the kernel sets the
 * identifier and the checksum), receive the echo replies. Addresses are
 * struct sockaddr_in with port and address in network byte order. Errors
 * of note: E_CONNREFUSED (nobody listens), E_TIMEOUT (no answer),
 * E_HOSTUNREACH / E_NETUNREACH, E_CONNRESET (the peer reset the connection).
 * Options: SO_REUSEADDR, SO_RCVBUF / SO_SNDBUF (TCP's windows), SO_KEEPALIVE
 * with TCP_KEEPIDLE / TCP_KEEPINTVL / TCP_KEEPCNT, SO_LINGER {1, 0} (close
 * resets), TCP_NODELAY, SO_BROADCAST (UDP), IP_TTL, IP_TOS; FIONREAD. */
static inline unsigned short htons(unsigned short v) { return (unsigned short)__builtin_bswap16(v); }
static inline unsigned short ntohs(unsigned short v) { return (unsigned short)__builtin_bswap16(v); }
static inline unsigned int htonl(unsigned int v) { return __builtin_bswap32(v); }
static inline unsigned int ntohl(unsigned int v) { return __builtin_bswap32(v); }
int  inet_aton(const char *s, struct in_addr *out);        /* "a.b.c.d": 1, else 0 */
unsigned int inet_addr(const char *s);                     /* network order, INADDR_NONE if not an address */
char *inet_ntoa_r(unsigned int addr, char *buf);           /* network order -> "a.b.c.d" in buf (16 bytes) */
const char *inet_ntoa(struct in_addr a);                   /* (a static buffer) */
/* fill a sockaddr_in: addr in network order, port in host order; its length */
socklen_t inet_sockaddr(struct sockaddr_in *a, unsigned int addr, int port);
/* look up a host name: up to max IPv4 addresses (network order) in out;
 * returns how many (>= 1), E_NOENT (no such name), E_TIMEOUT (no answer
 * within timeout_ms; <= 0 = 10 s), E_INVAL (not a valid name), E_NODEV (no
 * DNS server), E_IO (the servers failed). "localhost" and "a.b.c.d" are
 * answered at once; answers are cached. */
int  resolve(const char *host, unsigned int *out, int max, int timeout_ms);
/* a TCP connection to host:port (a name or an address), trying each
 * address; the socket, or an error */
int  inet_connect(const char *host, int port, int timeout_ms);

/* network interfaces: index 0.. (the loopback "lo" is last); E_NOENT past the end */
int  net_ifinfo(int index, hal_ifinfo_t *out);
/* change an interface's configuration (NETCFG_ADDR: DHCP or static,
 * NETCFG_DNS: the DNS servers, NETCFG_SAVE: keep it in SETTINGS.INI) */
int  net_ifconfig(const hal_ifconfig_t *cfg);
int  net_ctl(int op, const char *ifname);                  /* NETCTL_RENEW / RELEASE / DNS_FLUSH / ARP_FLUSH */
int  net_stats(hal_netstats_t *st);
int  net_conns(hal_netconn_t *out, int max);               /* TCP connections, UDP sockets: entries */

/* ---- sound (see docs/DESIGN-M7.md; the ABI is kernel/sound/sndabi.h) ----
 * A stream plays PCM through the default sound card, mixed with every other
 * stream: open it with its format (rate 8000..192000, 1 or 2 channels, 8-bit
 * unsigned or 16-bit signed samples) and a buffer length, then write whole
 * frames. snd_write waits until everything is queued (a full buffer is the
 * back-pressure), unless the stream is non-blocking (SND_NONBLOCK or
 * SNDCTL_NONBLOCK): then it takes what fits and returns E_AGAIN when nothing
 * does. poll() reports POLLOUT when a quarter of the buffer is free. close()
 * lets what is queued play out; snd_close chooses (SNDCLOSE_*). A program has
 * at most SND_PROC_STREAMS streams; they stop when it ends. E_NODEV: no
 * sound card. */
static inline int  snd_open(int rate, int channels, int bits, int buffer_ms, int flags) {
    return (int)SC5(SYS_SND_OPEN, rate, channels, bits, buffer_ms, flags);
}
static inline long snd_write(int h, const void *buf, size_t len) { return SC4(SYS_SND_WRITE, h, buf, len, 0); }
static inline long snd_write_nb(int h, const void *buf, size_t len) { return SC4(SYS_SND_WRITE, h, buf, len, SNDW_NONBLOCK); }
/* wait until all that was written has played (timeout_ms < 0: no limit); 0, E_TIMEOUT */
static inline int  snd_drain(int h, int timeout_ms) { return (int)SC2(SYS_SND_DRAIN, h, timeout_ms); }
static inline int  snd_ctl(int h, int op, int value) { return (int)SC3(SYS_SND_CTL, h, op, value); }
static inline int  snd_pause(int h, int paused) { return snd_ctl(h, SNDCTL_PAUSE, paused); }
static inline int  snd_stream_volume(int h, int percent) { return snd_ctl(h, SNDCTL_VOLUME, percent); }
static inline int  snd_discard(int h) { return snd_ctl(h, SNDCTL_DISCARD, 0); }          /* drop the queue (seek) */
static inline int  snd_status(int h, hal_sndstatus_t *st) { return (int)SC2(SYS_SND_STATUS, h, st); }
static inline int  snd_close(int h, int how) { return (int)SC2(SYS_SND_CLOSE, h, how); }
/* the master volume: volume 0..100 or -1 (keep), mute 0/1 or -1, flags
 * SNDVOL_SAVE; returns the volume | SND_MUTED. snd_master(-1, -1, 0) reads it. */
static inline int  snd_master(int volume, int mute, int flags) { return (int)SC3(SYS_SND_MASTER, volume, mute, flags); }
/* play a system sound (SNDEV_*); 1 played, 0 the sound scheme has it off */
static inline int  snd_event(int ev, int flags) { return (int)SC2(SYS_SND_EVENT, ev, flags); }
static inline int  snd_devinfo(int index, hal_snddev_t *d) { return (int)SC2(SYS_SND_DEVINFO, index, d); }
static inline int  snd_info(hal_sndinfo_t *info) { return (int)SC1(SYS_SND_INFO, info); }

#endif
