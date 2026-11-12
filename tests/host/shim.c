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

/* threads: the host tests are single threaded */
#include "core/thread.h"
void mutex_lock(mutex_t *m) { (void)m; }
int mutex_trylock(mutex_t *m) { (void)m; return 1; }
void mutex_unlock(mutex_t *m) { (void)m; }
int mutex_held(mutex_t *m) { (void)m; return 1; }

/* settings (core/settings.h): an in-memory table the tests can fill */
#include "core/settings.h"
static struct { char k[64]; char v[64]; } g_settings[32];
static int g_nsettings;

const char *settings_get(const char *key, const char *def) {
    for (int i = 0; i < g_nsettings; i++) if (!strcasecmp(g_settings[i].k, key)) return g_settings[i].v;
    return def;
}

int settings_get_int(const char *key, int def) {
    const char *v = settings_get(key, NULL);
    return v ? atoi(v) : def;
}

void settings_set(const char *key, const char *value) {
    for (int i = 0; i < g_nsettings; i++) {
        if (strcasecmp(g_settings[i].k, key)) continue;
        if (!value) { g_settings[i] = g_settings[--g_nsettings]; return; }
        strlcpy(g_settings[i].v, value, sizeof g_settings[i].v);
        return;
    }
    if (!value || g_nsettings >= 32) return;
    strlcpy(g_settings[g_nsettings].k, key, sizeof g_settings[g_nsettings].k);
    strlcpy(g_settings[g_nsettings].v, value, sizeof g_settings[g_nsettings].v);
    g_nsettings++;
}

void settings_set_int(const char *key, int value) {
    char b[16];
    snprintf(b, sizeof b, "%d", value);
    settings_set(key, b);
}
