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
static inline int kill(int pid) { return (int)SC1(SYS_KILL, pid); }
static inline int getcwd(char *buf, size_t n) { return (int)SC2(SYS_GETCWD, buf, n); }
static inline int chdir(const char *p) { return (int)SC1(SYS_CHDIR, p); }
static inline int proclist(hal_procinfo_t *buf, int max) { return (int)SC2(SYS_PROCLIST, buf, max); }
static inline int sysinfo(hal_sysinfo_t *si) { return (int)SC1(SYS_SYSINFO, si); }
void *sbrk(long incr);

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
void clipboard_set(const char *text);
int  clipboard_get(char *buf, int n);

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
 * messages (up to 64 KiB) with boundaries kept. AF_INET comes with
 * networking.
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

#endif
