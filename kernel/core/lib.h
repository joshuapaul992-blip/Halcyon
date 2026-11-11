#ifndef LIB_H
#define LIB_H
#include "types.h"

void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
size_t strlcpy(char *d, const char *s, size_t size);   /* always NUL-terminates */
size_t strlcat(char *d, const char *s, size_t size);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *h, const char *n);
char *strdup(const char *s);
int toupper(int c);
int tolower(int c);
int isdigit(int c);
int isspace(int c);
int isalpha(int c);
int isalnum(int c);
int isprint(int c);
long strtol(const char *s, char **end, int base);
int atoi(const char *s);

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* kernel log: goes to COM1 and an in-memory ring buffer */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kvprintf(const char *fmt, va_list ap);
/* copy of the log ring buffer (oldest first); returns length */
size_t klog_read(char *buf, size_t size);
size_t klog_size(void);
/* optional observer of every kprintf (verbose boot console) */
extern void (*klog_hook)(const char *s, int n);

NORETURN void panic(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define ASSERT(c) do { if (!(c)) panic("assertion failed: %s (%s:%d)", #c, __FILE__, __LINE__); } while (0)

void serial_init(void);
void serial_putc(char c);

#endif
