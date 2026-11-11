/* Halcyon user-mode C library subset */
#include "hal.h"

/* ---------------- startup ---------------- */
int main(int argc, char **argv);

#define MAX_ARGS 32
void __hal_start(const char *cmdline) {
    static char line[512];
    static char *argv[MAX_ARGS + 1];
    int argc = 0;
    strlcpy(line, cmdline ? cmdline : "", sizeof line);
    char *p = line;
    while (*p && argc < MAX_ARGS) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '"') {                       /* "quoted argument" */
            argv[argc++] = ++p;
            while (*p && *p != '"') p++;
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ') p++;
        }
        if (*p) *p++ = 0;
    }
    argv[argc] = 0;
    exit(main(argc, argv));
}

__attribute__((noreturn)) void exit(int code) {
    SC1(SYS_EXIT, code);
    for (;;) ;
}
int spawn(const char *path, const char *args) { return (int)SC2(SYS_SPAWN, path, args); }
int wait(int pid) { return (int)SC1(SYS_WAIT, pid); }

const char *hal_strerror(int e) {
    switch (e) {
    case 0: return "Success";
    case E_IO: return "I/O error";
    case E_NOMEM: return "Out of memory";
    case E_NOENT: return "File or directory not found";
    case E_EXIST: return "Already exists";
    case E_NOTDIR: return "Not a directory";
    case E_ISDIR: return "Is a directory";
    case E_NOSPC: return "Disk is full";
    case E_INVAL: return "Invalid argument";
    case E_NOTSUP: return "Not supported";
    case E_NOTEMPTY: return "Directory is not empty";
    case E_ROFS: return "Read-only file system";
    case E_BUSY: return "Busy";
    case E_CANCELED: return "Canceled";
    case E_PERM: return "Access denied";
    case E_TIMEOUT: return "Timed out";
    case E_AGAIN: return "Try again";
    case E_NAMETOOLONG: return "Name too long";
    case E_ADDRINUSE: return "Address already in use";
    case E_NOTCONN: return "Not connected";
    case E_ISCONN: return "Already connected";
    case E_CONNREFUSED: return "Connection refused";
    case E_CONNRESET: return "Connection reset";
    case E_PIPE: return "Connection closed";
    case E_AFNOSUPPORT: return "Address family not supported";
    case E_MSGSIZE: return "Message too long";
    case E_NOTSOCK: return "Not a socket";
    case E_INPROGRESS: return "Operation in progress";
    }
    return "Error";
}

/* ---------------- strings ---------------- */
void *memcpy(void *d, const void *s, size_t n) { char *a = d; const char *b = s; while (n--) *a++ = *b++; return d; }
void *memmove(void *d, const void *s, size_t n) {
    char *a = d; const char *b = s;
    if (a < b) while (n--) *a++ = *b++;
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
void *memset(void *d, int c, size_t n) { char *a = d; while (n--) *a++ = (char)c; return d; }
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return (unsigned char)*a - (unsigned char)*b; }
int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) { if (*a != *b) return (unsigned char)*a - (unsigned char)*b; if (!*a) return 0; }
    return 0;
}
int toupper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
int tolower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
int strcasecmp(const char *a, const char *b) {
    while (*a && toupper((unsigned char)*a) == toupper((unsigned char)*b)) a++, b++;
    return toupper((unsigned char)*a) - toupper((unsigned char)*b);
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) ; return r; }
char *strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
size_t strlcpy(char *d, const char *s, size_t n) {
    size_t l = strlen(s);
    if (n) { size_t c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = 0; }
    return l;
}
size_t strlcat(char *d, const char *s, size_t n) {
    size_t dl = strlen(d);
    if (dl >= n) return dl + strlen(s);
    return dl + strlcpy(d + dl, s, n - dl);
}
char *strchr(const char *s, int c) { for (; *s; s++) if (*s == (char)c) return (char *)s; return c ? 0 : (char *)s; }
char *strrchr(const char *s, int c) { const char *r = 0; for (; *s; s++) if (*s == (char)c) r = s; return c ? (char *)r : (char *)s; }
char *strstr(const char *h, const char *n) {
    size_t l = strlen(n);
    for (; *h; h++) if (!strncmp(h, n, l)) return (char *)h;
    return l ? 0 : (char *)h;
}
char *strdup(const char *s) { size_t l = strlen(s) + 1; char *d = malloc(l); if (d) memcpy(d, s, l); return d; }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isalpha(int c) { return (c | 32) >= 'a' && (c | 32) <= 'z'; }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int abs(int v) { return v < 0 ? -v : v; }
long strtol(const char *s, char **end, int base) {
    while (isspace((unsigned char)*s)) s++;
    int neg = 0;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] | 32) == 'x') { s += 2; base = 16; }
    else if (base == 0) base = s[0] == '0' ? 8 : 10;
    long v = 0;
    for (;; s++) {
        int d = isdigit((unsigned char)*s) ? *s - '0' : isalpha((unsigned char)*s) ? (*s | 32) - 'a' + 10 : 99;
        if (d >= base) break;
        v = v * base + d;
    }
    if (end) *end = (char *)s;
    return neg ? -v : v;
}
int atoi(const char *s) { return (int)strtol(s, 0, 10); }

static unsigned rnd = 12345;
unsigned rand(void) { rnd = rnd * 1103515245u + 12345u; return (rnd >> 16) & 0x7FFF; }
void srand(unsigned s) { rnd = s; }

/* ---------------- heap: first fit over sbrk ---------------- */
typedef struct blk { size_t size; int free; struct blk *next; } blk_t;   /* size: payload bytes */
static blk_t *heap_head;

void *sbrk(long incr) {
    long r = SC1(SYS_SBRK, incr);
    return r < 0 ? (void *)-1 : (void *)r;
}

void *malloc(size_t n) {
    if (!n) n = 1;
    n = (n + 15) & ~(size_t)15;
    blk_t *last = 0;
    for (blk_t *b = heap_head; b; last = b, b = b->next) {
        if (!b->free || b->size < n) continue;
        if (b->size >= n + sizeof(blk_t) + 32) {
            blk_t *t = (blk_t *)((char *)(b + 1) + n);
            t->size = b->size - n - sizeof(blk_t);
            t->free = 1;
            t->next = b->next;
            b->next = t;
            b->size = n;
        }
        b->free = 0;
        return b + 1;
    }
    size_t grow = n + sizeof(blk_t);
    if (grow < 64 * 1024) grow = 64 * 1024;
    blk_t *b = sbrk((long)grow);
    if (b == (void *)-1) return 0;
    b->size = grow - sizeof(blk_t);
    b->free = 1;
    b->next = 0;
    if (last) last->next = b; else heap_head = b;
    return malloc(n);
}

void free(void *p) {
    if (!p) return;
    blk_t *b = (blk_t *)p - 1;
    b->free = 1;
    /* merge with following free blocks that are adjacent in memory */
    while (b->next && b->next->free && (char *)(b + 1) + b->size == (char *)b->next) {
        b->size += sizeof(blk_t) + b->next->size;
        b->next = b->next->next;
    }
}

void *calloc(size_t n, size_t m) {
    size_t t = n * m;
    if (m && t / m != n) return 0;
    void *p = malloc(t);
    if (p) memset(p, 0, t);
    return p;
}

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    blk_t *b = (blk_t *)p - 1;
    if (b->size >= n) return p;
    void *q = malloc(n);
    if (!q) return 0;
    memcpy(q, p, b->size);
    free(p);
    return q;
}

/* ---------------- formatted output ---------------- */
typedef struct { char *buf; size_t n, pos; } out_t;
static void emit(out_t *o, char c) { if (o->pos + 1 < o->n) o->buf[o->pos] = c; o->pos++; }

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    out_t o = { buf, n, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { emit(&o, *fmt); continue; }
        fmt++;
        int left = 0, zero = 0, plus = 0, width = 0, prec = -1, lng = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ') ;
            else break;
        }
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        else while (isdigit((unsigned char)*fmt)) width = width * 10 + *fmt++ - '0';
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (isdigit((unsigned char)*fmt)) prec = prec * 10 + *fmt++ - '0';
        }
        while (*fmt == 'l' || *fmt == 'z' || *fmt == 'h') { if (*fmt != 'h') lng = 1; fmt++; }
        char tmp[32];
        const char *s = tmp;
        int len = 0, neg = 0;
        unsigned long long u = 0;
        switch (*fmt) {
        case 'd': case 'i': {
            long long v = lng ? va_arg(ap, long long) : va_arg(ap, int);
            neg = v < 0;
            u = neg ? 0ull - (unsigned long long)v : (unsigned long long)v;
            goto num10;
        }
        case 'u': u = lng ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned); goto num10;
        num10: {
            char *e = tmp + sizeof tmp; char *q = e;
            do { *--q = (char)('0' + u % 10); u /= 10; } while (u);
            if (neg) *--q = '-'; else if (plus) *--q = '+';
            s = q; len = (int)(e - q);
            break;
        }
        case 'x': case 'X': case 'p': {
            if (*fmt == 'p') { u = (unsigned long long)(uintptr_t)va_arg(ap, void *); emit(&o, '0'); emit(&o, 'x'); }
            else u = lng ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
            const char *dig = *fmt == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            char *e = tmp + sizeof tmp; char *q = e;
            do { *--q = dig[u & 15]; u >>= 4; } while (u);
            s = q; len = (int)(e - q);
            break;
        }
        case 'c': tmp[0] = (char)va_arg(ap, int); len = 1; break;
        case 's': s = va_arg(ap, const char *); if (!s) s = "(null)"; len = (int)strlen(s); if (prec >= 0 && len > prec) len = prec; break;
        case '%': tmp[0] = '%'; len = 1; break;
        default: tmp[0] = '%'; tmp[1] = *fmt; len = 2; if (!*fmt) fmt--; break;
        }
        int pad = width > len ? width - len : 0;
        if (!left) while (pad-- > 0) emit(&o, zero && *fmt != 's' ? '0' : ' ');
        for (int i = 0; i < len; i++) emit(&o, s[i]);
        if (left) while (pad-- > 0) emit(&o, ' ');
    }
    if (n) buf[o.pos < n ? o.pos : n - 1] = 0;
    return (int)o.pos;
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int printf(const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    write(1, buf, (size_t)(r < (int)sizeof buf ? r : (int)sizeof buf - 1));
    return r;
}
int puts(const char *s) { write(1, s, strlen(s)); write(1, "\n", 1); return 0; }
int putchar(int c) { char ch = (char)c; write(1, &ch, 1); return c; }

int getline(char *buf, int n) {
    long r = read(0, buf, (size_t)(n > 1 ? n - 1 : 0));
    if (r < 0) return -1;
    if (r == 0) return -1;
    buf[r] = 0;
    if (r && buf[r - 1] == '\n') buf[--r] = 0;
    return (int)r;
}

/* ---------------- whole files ---------------- */
int read_file(const char *path, char **data, long *size) {
    int fd = open(path, O_READ);
    if (fd < 0) return fd;
    long n = fsize(fd);
    char *b = n >= 0 ? malloc((size_t)n + 1) : 0;
    if (!b) { close(fd); return E_NOMEM; }
    long got = read(fd, b, (size_t)n);
    close(fd);
    if (got < 0) { free(b); return (int)got; }
    b[got] = 0;
    *data = b;
    if (size) *size = got;
    return 0;
}

int write_file(const char *path, const void *data, long size) {
    int fd = open(path, O_WRITE | O_CREATE | O_TRUNC);
    if (fd < 0) return fd;
    long w = write(fd, data, (size_t)size);
    close(fd);
    return w == size ? 0 : (w < 0 ? (int)w : E_NOSPC);
}
