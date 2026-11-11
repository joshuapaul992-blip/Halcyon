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
#define E_TIMEOUT -9
#define E_AGAIN   -12
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

/* ---- the shell ---- */
static inline int shell_open(const char *path) { return (int)SC1(SYS_SHELL_OPEN, path); }   /* like a double click */
static inline int shell_format(char drive) { return (int)SC1(SYS_SHELL_FORMAT, drive); }    /* the user decides in it */
static inline int eject(char drive) { return (int)SC1(SYS_EJECT, drive); }                  /* CD/DVD drives */

/* ---- files (fd 0/1/2: console) ---- */
static inline int open(const char *p, int flags) { return (int)SC2(SYS_OPEN, p, flags); }
static inline int close(int fd) { return (int)SC1(SYS_CLOSE, fd); }
static inline long read(int fd, void *b, size_t n) { return SC3(SYS_READ, fd, b, n); }
static inline long write(int fd, const void *b, size_t n) { return SC3(SYS_WRITE, fd, b, n); }
static inline long seek(int fd, long off, int whence) { return SC3(SYS_SEEK, fd, off, whence); }
static inline long fsize(int fd) { return SC1(SYS_FSIZE, fd); }
static inline int stat(const char *p, hal_dirent_t *d) { return (int)SC2(SYS_STAT, p, d); }
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

#endif
