/* Host shim: the few kernel services the storage stack uses, implemented
 * on top of libc so the kernel/fs sources can be compiled and tested unmodified. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include "core/types.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"

struct bootinfo g_bootinfo;
int shim_verbose;
int shim_kprintf_format;    /* format messages even when not printing them (fuzzing) */
size_t shim_live_allocs;

void *kmalloc(size_t n) { void *p = malloc(n ? n : 1); if (p) shim_live_allocs++; return p; }
void *kzalloc(size_t n) { void *p = calloc(1, n ? n : 1); if (p) shim_live_allocs++; return p; }
void *krealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (q && !p) shim_live_allocs++;
    return q;
}
void kfree(void *p) { if (p) { shim_live_allocs--; free(p); } }

void kprintf(const char *fmt, ...) {
    va_list ap;
    if (!shim_verbose) {
        if (shim_kprintf_format) {
            char buf[512];
            va_start(ap, fmt);
            vsnprintf(buf, sizeof buf, fmt, ap);
            va_end(ap);
        }
        return;
    }
    va_start(ap, fmt);
    fputs("    [k] ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void panic(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("PANIC: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    abort();
}

void rtc_read(struct rtc_time *t) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    t->year = tm.tm_year + 1900;
    t->month = tm.tm_mon + 1;
    t->day = tm.tm_mday;
    t->hour = tm.tm_hour;
    t->minute = tm.tm_min;
    t->second = tm.tm_sec;
    t->weekday = tm.tm_wday;
}
