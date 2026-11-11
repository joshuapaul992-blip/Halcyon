#include "lib.h"
#include "io.h"
#include "mm.h"

void *memcpy(void *d, const void *s, size_t n) {
    void *r = d;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
    return r;
}
void *memmove(void *d, const void *s, size_t n) {
    u8 *a = d; const u8 *b = s;
    if (a == b || n == 0) return d;
    if (a < b || a >= b + n) return memcpy(d, s, n);
    /* overlapping, copy backwards */
    a += n - 1; b += n - 1;
    __asm__ volatile("std; rep movsb; cld" : "+D"(a), "+S"(b), "+c"(n) :: "memory");
    return d;
}
void *memset(void *d, int c, size_t n) {
    void *r = d;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}
int memcmp(const void *a, const void *b, size_t n) {
    const u8 *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
size_t strnlen(const char *s, size_t max) { size_t n = 0; while (n < max && s[n]) n++; return n; }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (u8)*a - (u8)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return (u8)*a - (u8)*b;
        if (!*a) return 0;
    }
    return 0;
}
int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isprint(int c) { return c >= 32 && c < 127; }
int strcasecmp(const char *a, const char *b) {
    while (*a && tolower(*a) == tolower(*b)) { a++; b++; }
    return tolower((u8)*a) - tolower((u8)*b);
}
int strncasecmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        int x = tolower((u8)*a), y = tolower((u8)*b);
        if (x != y) return x - y;
        if (!x) return 0;
    }
    return 0;
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) ; return r; }
size_t strlcpy(char *d, const char *s, size_t size) {
    size_t n = strlen(s);
    if (size) {
        size_t c = n >= size ? size - 1 : n;
        memcpy(d, s, c);
        d[c] = 0;
    }
    return n;
}
size_t strlcat(char *d, const char *s, size_t size) {
    size_t dl = strnlen(d, size);
    if (dl == size) return size + strlen(s);
    return dl + strlcpy(d + dl, s, size - dl);
}
char *strchr(const char *s, int c) {
    for (; *s; s++) if (*s == (char)c) return (char *)s;
    return c == 0 ? (char *)s : NULL;
}
char *strrchr(const char *s, int c) {
    const char *r = NULL;
    for (; *s; s++) if (*s == (char)c) r = s;
    return c == 0 ? (char *)s : (char *)r;
}
char *strstr(const char *h, const char *n) {
    size_t nl = strlen(n);
    if (!nl) return (char *)h;
    for (; *h; h++) if (!strncmp(h, n, nl)) return (char *)h;
    return NULL;
}
char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = kmalloc(n);
    if (d) memcpy(d, s, n);
    return d;
}
long strtol(const char *s, char **end, int base) {
    long v = 0; int neg = 0;
    while (isspace(*s)) s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
    if (base == 0) base = 10;
    for (;;) {
        int d;
        if (isdigit(*s)) d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d; s++;
    }
    if (end) *end = (char *)s;
    return neg ? -v : v;
}
int atoi(const char *s) { return (int)strtol(s, NULL, 10); }

const char *strerror(int e) {
    switch (e) {
    case E_OK: return "Success";
    case E_IO: return "I/O error";
    case E_NOMEM: return "Out of memory";
    case E_NOENT: return "File or directory not found";
    case E_EXIST: return "Already exists";
    case E_NOTDIR: return "Not a directory";
    case E_ISDIR: return "Is a directory";
    case E_NOSPC: return "Disk is full";
    case E_INVAL: return "Invalid argument";
    case E_TIMEOUT: return "Timed out";
    case E_STALL: return "Device stalled";
    case E_NOTSUP: return "Not supported";
    case E_AGAIN: return "Try again";
    case E_NOTEMPTY: return "Directory is not empty";
    case E_ROFS: return "Read-only file system";
    case E_NODEV: return "No such device";
    case E_BUSY: return "Device busy";
    case E_NAMETOOLONG: return "Name too long";
    }
    return "Unknown error";
}

/* ---------------- vsnprintf ---------------- */
struct outbuf { char *buf; size_t size, pos; };
static void out_c(struct outbuf *o, char c) {
    if (o->pos + 1 < o->size) o->buf[o->pos] = c;
    o->pos++;
}
static void out_num(struct outbuf *o, u64 v, int base, int neg, int width, int zero, int left, int upper, int prec) {
    char tmp[32]; int n = 0;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0 && prec != 0) tmp[n++] = '0';
    while (v) { tmp[n++] = dig[v % base]; v /= base; }
    while (n < prec) tmp[n++] = '0';
    int len = n + neg;
    if (!left && !zero) while (width-- > len) out_c(o, ' ');
    if (neg) out_c(o, '-');
    if (!left && zero) while (width-- > len) out_c(o, '0');
    while (n) out_c(o, tmp[--n]);
    if (left) while (width-- > len) out_c(o, ' ');
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    struct outbuf o = { buf, size, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { out_c(&o, *fmt); continue; }
        fmt++;
        int zero = 0, left = 0, width = 0, lng = 0, prec = -1, plus = 0;
        for (;; fmt++) {
            if (*fmt == '0') zero = 1;
            else if (*fmt == '-') left = 1;
            else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ' || *fmt == '#') ;
            else break;
        }
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        else while (isdigit(*fmt)) width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            fmt++; prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (isdigit(*fmt)) prec = prec * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l') { lng++; fmt++; }
        if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') { lng = 2; fmt++; }
        if (*fmt == 'h') { fmt++; if (*fmt == 'h') fmt++; }
        switch (*fmt) {
        case 'd': case 'i': {
            i64 v = lng ? va_arg(ap, i64) : va_arg(ap, int);
            if (plus && v >= 0) out_c(&o, '+');
            out_num(&o, v < 0 ? -(u64)v : (u64)v, 10, v < 0, width, zero, left, 0, prec);
            break;
        }
        case 'u': out_num(&o, lng ? va_arg(ap, u64) : va_arg(ap, unsigned), 10, 0, width, zero, left, 0, prec); break;
        case 'x': out_num(&o, lng ? va_arg(ap, u64) : va_arg(ap, unsigned), 16, 0, width, zero, left, 0, prec); break;
        case 'X': out_num(&o, lng ? va_arg(ap, u64) : va_arg(ap, unsigned), 16, 0, width, zero, left, 1, prec); break;
        case 'o': out_num(&o, lng ? va_arg(ap, u64) : va_arg(ap, unsigned), 8, 0, width, zero, left, 0, prec); break;
        case 'p': out_c(&o, '0'); out_c(&o, 'x'); out_num(&o, (u64)va_arg(ap, void *), 16, 0, width, 1, 0, 0, -1); break;
        case 'c': out_c(&o, (char)va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = (int)(prec >= 0 ? strnlen(s, prec) : strlen(s));
            if (!left) while (width-- > len) out_c(&o, ' ');
            for (int i = 0; i < len; i++) out_c(&o, s[i]);
            if (left) while (width-- > len) out_c(&o, ' ');
            break;
        }
        case 'f': {
            double v = va_arg(ap, double);
            if (prec < 0) prec = 6;
            int neg = v < 0; if (neg) v = -v;
            double r = 0.5; for (int i = 0; i < prec; i++) r /= 10;
            v += r;
            u64 ip = (u64)v; double fp = v - (double)ip;
            char tmp[64]; int n = 0;
            if (neg) tmp[n++] = '-';
            char ib[24]; int in = 0;
            if (!ip) ib[in++] = '0';
            while (ip) { ib[in++] = '0' + ip % 10; ip /= 10; }
            while (in) tmp[n++] = ib[--in];
            if (prec) {
                tmp[n++] = '.';
                for (int i = 0; i < prec && n < 60; i++) { fp *= 10; int d = (int)fp; tmp[n++] = '0' + d; fp -= d; }
            }
            if (!left) while (width-- > n) out_c(&o, zero ? '0' : ' ');
            for (int i = 0; i < n; i++) out_c(&o, tmp[i]);
            if (left) while (width-- > n) out_c(&o, ' ');
            break;
        }
        case '%': out_c(&o, '%'); break;
        case 0: fmt--; break;
        default: out_c(&o, '%'); out_c(&o, *fmt); break;
        }
    }
    if (o.size) o.buf[o.pos < o.size ? o.pos : o.size - 1] = 0;
    return (int)o.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

/* ---------------- serial + log ---------------- */
#define COM1 0x3F8
static int serial_ok;
void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01);   /* 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
    serial_ok = inb(COM1 + 5) != 0xFF;
}
void serial_putc(char c) {
    if (!serial_ok) return;
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++) ;
    outb(COM1, (u8)c);
}

#define KLOG_SIZE (128 * 1024)
static char klog[KLOG_SIZE];
static size_t klog_head, klog_len;

static void klog_putc(char c) {
    klog[klog_head] = c;
    klog_head = (klog_head + 1) % KLOG_SIZE;
    if (klog_len < KLOG_SIZE) klog_len++;
}
size_t klog_size(void) { return klog_len; }
size_t klog_read(char *buf, size_t size) {
    size_t start = (klog_head + KLOG_SIZE - klog_len) % KLOG_SIZE;
    size_t n = klog_len < size ? klog_len : size;
    /* return the most recent n bytes */
    start = (start + (klog_len - n)) % KLOG_SIZE;
    for (size_t i = 0; i < n; i++) buf[i] = klog[(start + i) % KLOG_SIZE];
    return n;
}

void (*klog_hook)(const char *s, int n);

void kvprintf(const char *fmt, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    if (klog_hook) klog_hook(buf, n);
    for (int i = 0; i < n; i++) {
        if (buf[i] == '\n') serial_putc('\r');
        serial_putc(buf[i]);
        klog_putc(buf[i]);
    }
}
void kprintf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}
