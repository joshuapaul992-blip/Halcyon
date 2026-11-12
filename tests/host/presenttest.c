/* Host test: the display core's presentation with acceleration (M10e,
 * kernel/drivers/display.c compiled unmodified with modes.c, edid.c and
 * layout.c), built with ASan/UBSan, against a fake adapter whose copy
 * engine completes its work asynchronously on simulated time.
 *
 *   - the flip-ordering rule: a page flip is never queued, and a buffer
 *     never flushed, while an operation writing into it is still in
 *     flight; no buffer is freed and no desktop surface replaced while an
 *     operation still reads or writes it
 *   - pixels: after thousands of random rectangles drawn and presented on
 *     double-buffered heads (with the copy engine bringing the back buffer
 *     up to date from the front one) and on a single-buffered head of a
 *     device that needs flushes, every buffer on screen equals the desktop
 *   - the fallback: a copy engine that stops completing (the 250 ms bound),
 *     reports an error, refuses operations, or runs out of queue space —
 *     acceleration ends for the session (the driver is told once), the CPU
 *     redraws and the screen is still exactly the desktop
 *   - mode changes and a new desktop surface with work in flight, two
 *     heads on one adapter, the speed test, the acceleration start guard,
 *     and the software test hook QEMU uses (opt/halcyon/displayaccel)
 *
 *   presenttest [-v]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "core/lib.h"
#include "core/kernel.h"
#include "core/settings.h"
#include "drivers/display.h"
#include "drivers/pci.h"

static int checks, failed, verbose;
#define CHECK(c, ...) do { checks++; if (!(c)) { failed++; printf("presenttest: FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static u64 rs = 0x2545F4914F6CDD1Dull;
static u32 rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (u32)rs; }

/* ---------------- the kernel services display.c uses ---------------- */

struct bootinfo g_bootinfo;
static u64 now_us;
u64 time_us(void) { now_us += 20; return now_us; }
u64 time_ms(void) { now_us += 20; return now_us / 1000; }
static void advance_ms(u32 ms) { now_us += (u64)ms * 1000; }
void idle_wake_at(u64 ms) { (void)ms; }
int mm_set_write_combining(u64 phys, u64 size) { (void)phys; (void)size; return 0; }
int pci_count(void) { return 0; }
pci_dev_t *pci_get(int i) { (void)i; return NULL; }
const char *strerror(int e) { static char b[16]; snprintf(b, sizeof b, "E%d", e); return b; }
static char hook_accel[32];
int qemu_fw_file(const char *name, char *buf, int n) {
    if (!strcmp(name, "opt/halcyon/displayaccel") && hook_accel[0]) { strlcpy(buf, hook_accel, (size_t)n); return 1; }
    return 0;
}
static char klog[1 << 16];
static int kloglen;
void kprintf(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (verbose) printf("    [k] %s", line);
    int l = (int)strlen(line);
    if (kloglen + l < (int)sizeof klog - 1) { memcpy(klog + kloglen, line, (size_t)l); kloglen += l; klog[kloglen] = 0; }
}
static int logged(const char *s) { return strstr(klog, s) != NULL; }

static struct { char k[64], v[128]; } set[64];
static int nset, saves;
const char *settings_get(const char *key, const char *def) {
    for (int i = 0; i < nset; i++) if (!strcmp(set[i].k, key)) return set[i].v;
    return def;
}
int settings_get_int(const char *key, int def) { const char *v = settings_get(key, NULL); return v ? atoi(v) : def; }
void settings_set(const char *key, const char *value) {
    for (int i = 0; i < nset; i++)
        if (!strcmp(set[i].k, key)) {
            if (!value) { set[i] = set[--nset]; return; }
            strlcpy(set[i].v, value, sizeof set[i].v);
            return;
        }
    if (value && nset < 64) { strlcpy(set[nset].k, key, sizeof set[0].k); strlcpy(set[nset].v, value, sizeof set[0].v); nset++; }
}
void settings_set_int(const char *key, int value) { char b[16]; snprintf(b, sizeof b, "%d", value); settings_set(key, b); }
int settings_save(void) { saves++; return 0; }

/* ---------------- the fake device ---------------- */

typedef struct fop {
    int kind;                   /* 1 upload, 2 copy, 3 fill */
    u32 *dst, *src;
    u32 dpitch, spitch;         /* bytes */
    int x, y, w, h, sx, sy;
    u64 src_off;
    u32 color, fence;
} fop_t;

static struct {
    fop_t op[4096];
    int n;                      /* queued (fence 0) and in flight */
    u32 seq, done;
    u64 due[1 << 16];           /* per fence: when it completes (us) */
    u32 delay_us;
    int hang_after;             /* fences: never more after this many (0: no) */
    int error_after;            /* done() reports an error after this many fences */
    int refuse;                 /* operations fail with E_IO */
    int nospc_every;            /* every nth operation reports E_NOSPC */
    int opcount;
    const u8 *src;
    u64 src_bytes;
    int stops;
    int violations;
    char vwhy[128];
    u32 uploads, copies, fills, flushes, flips;
    int flushing;               /* the device needs flushes (single-buffered checks) */
} E;

static void violation(const char *fmt, ...) {
    E.violations++;
    if (E.vwhy[0]) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.vwhy, sizeof E.vwhy, fmt, ap);
    va_end(ap);
}

/* is any operation not yet complete touching buffer px (as destination; also as source when src) */
static int busy_with(const void *px, int src_too, int x, int y, int w, int h) {
    for (int i = 0; i < E.n; i++) {
        fop_t *o = &E.op[i];
        if (o->dst == px && (w <= 0 || (o->x < x + w && x < o->x + o->w && o->y < y + h && y < o->y + o->h))) return 1;
        if (src_too && o->kind == 2 && o->src == px) return 1;
    }
    return 0;
}

static void run_ops(u32 upto) {
    int k = 0;
    for (int i = 0; i < E.n; i++) {
        fop_t *o = &E.op[i];
        if (!o->fence || (i32)(o->fence - upto) > 0) { E.op[k++] = *o; continue; }
        for (int j = 0; j < o->h; j++) {
            u32 *d = (u32 *)((u8 *)o->dst + (u64)(o->y + j) * o->dpitch) + o->x;
            if (o->kind == 1) memcpy(d, E.src + o->src_off + (u64)j * o->spitch, (size_t)o->w * 4);
            else if (o->kind == 2) memcpy(d, (u32 *)((u8 *)o->src + (u64)(o->sy + j) * o->spitch) + o->sx, (size_t)o->w * 4);
            else for (int i2 = 0; i2 < o->w; i2++) d[i2] = o->color;
        }
    }
    E.n = k;
    E.done = upto;
}

static int f_queue(fop_t *o) {
    E.opcount++;
    if (E.refuse) return E_IO;
    if (E.nospc_every && E.opcount % E.nospc_every == 0) return E_NOSPC;
    if (E.n >= (int)ARRAY_SIZE(E.op)) return E_NOSPC;
    o->fence = 0;
    E.op[E.n++] = *o;
    return 0;
}
static int a_source(display_adapter_t *a, const void *px, u64 bytes) {
    (void)a;
    for (int i = 0; i < E.n; i++) if (E.op[i].kind == 1) violation("the source changed with uploads in flight");
    E.src = px;
    E.src_bytes = bytes;
    return 0;
}
static int a_upload(display_adapter_t *a, display_fb_t *dst, int x, int y, int w, int h, u64 off, u32 pitch) {
    (void)a;
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > dst->w || y + h > dst->h || off + (u64)(h - 1) * pitch + (u64)w * 4 > E.src_bytes) {
        violation("upload outside: %d,%d %dx%d of %dx%d, off %llu", x, y, w, h, dst->w, dst->h, (unsigned long long)off);
        return E_INVAL;
    }
    fop_t o = { 1, dst->px, NULL, dst->pitch, pitch, x, y, w, h, 0, 0, off, 0, 0 };
    int r = f_queue(&o);
    if (!r) E.uploads++;
    return r;
}
static int a_copy(display_adapter_t *a, display_fb_t *dst, int dx, int dy, display_fb_t *src, int sx, int sy, int w, int h) {
    (void)a;
    if (dx < 0 || dy < 0 || sx < 0 || sy < 0 || dx + w > dst->w || dy + h > dst->h || sx + w > src->w || sy + h > src->h) {
        violation("copy outside");
        return E_INVAL;
    }
    if (busy_with(src->px, 0, sx, sy, w, h) && src->px != dst->px) violation("copy from a buffer with writes in flight");
    fop_t o = { 2, dst->px, src->px, dst->pitch, src->pitch, dx, dy, w, h, sx, sy, 0, 0, 0 };
    int r = f_queue(&o);
    if (!r) E.copies++;
    return r;
}
static int a_fill(display_adapter_t *a, display_fb_t *dst, int x, int y, int w, int h, u32 c) {
    (void)a;
    if (x < 0 || y < 0 || x + w > dst->w || y + h > dst->h) { violation("fill outside"); return E_INVAL; }
    fop_t o = { 3, dst->px, NULL, dst->pitch, 0, x, y, w, h, 0, 0, 0, c, 0 };
    int r = f_queue(&o);
    if (!r) E.fills++;
    return r;
}
static u32 a_submit(display_adapter_t *a) {
    (void)a;
    int any = 0;
    for (int i = 0; i < E.n; i++) if (!E.op[i].fence) any = 1;
    if (!any) return E.seq;
    E.seq++;
    for (int i = 0; i < E.n; i++) if (!E.op[i].fence) E.op[i].fence = E.seq;
    E.due[E.seq & 0xFFFF] = now_us + E.delay_us;
    return E.seq;
}
static int a_done(display_adapter_t *a, u32 fence) {
    (void)a;
    if ((i32)(E.done - fence) >= 0) return 1;
    if (E.error_after && (int)fence > E.error_after) return E_IO;
    u32 upto = E.done;
    while ((i32)(E.seq - upto) > 0 && now_us >= E.due[(upto + 1) & 0xFFFF] && !(E.hang_after && (int)upto + 1 > E.hang_after)) upto++;
    if (upto != E.done) run_ops(upto);
    return (i32)(E.done - fence) >= 0;
}
static void a_stop(display_adapter_t *a, const char *why) { (void)a; (void)why; E.stops++; E.n = 0; E.done = E.seq; }
static const display_accel_ops_t accel_ops = { a_source, a_upload, a_copy, a_fill, a_submit, a_done, a_stop };

/* the adapter */
static int flip_time_ms = 16;
static struct { display_fb_t *pending; u64 at; } flipq[4];

static int f_enable(display_adapter_t *a) { (void)a; return 0; }
static void f_detect(display_adapter_t *a) {
    for (int k = 0; k < a->nconns; k++) display_connector_update(&a->conns[k], CONN_CONNECTED, NULL, 0, k ? 640 : 800, k ? 480 : 600);
}
static int f_fb_alloc(display_adapter_t *a, int w, int h, display_fb_t *fb) {
    (void)a;
    memset(fb, 0, sizeof *fb);
    fb->pitch = (u32)(w * 4 + 64);
    fb->px = malloc((size_t)fb->pitch * (size_t)h);
    memset(fb->px, 0x5A, (size_t)fb->pitch * (size_t)h);
    fb->w = w; fb->h = h; fb->bpp = 32; fb->rpos = 16; fb->gpos = 8; fb->bpos = 0;
    return 0;
}
static void f_fb_free(display_adapter_t *a, display_fb_t *fb) {
    (void)a;
    if (busy_with(fb->px, 1, 0, 0, 0, 0)) violation("a buffer freed with operations in flight");
    free(fb->px);
    fb->px = NULL;
}
static int f_set_mode(display_head_t *h, display_connector_t *c, const display_mode_t *m, display_fb_t *fb) {
    (void)c; (void)m; (void)fb;
    flipq[h->index].pending = NULL;
    return 0;
}
static int f_flip(display_head_t *h, display_fb_t *fb) {
    if (busy_with(fb->px, 0, 0, 0, 0, 0)) violation("a flip queued while writes into its buffer are in flight");
    flipq[h->index].pending = fb;
    flipq[h->index].at = now_us / 1000 + (u64)flip_time_ms;
    E.flips++;
    return 0;
}
static void f_flush(display_head_t *h, display_fb_t *fb, int x, int y, int w, int hh) {
    (void)h;
    if (busy_with(fb->px, 0, x, y, w, hh)) violation("a flush of a rectangle with writes in flight");
    E.flushes++;
}
static void f_poll(display_adapter_t *a) {
    for (int k = 0; k < a->nheads; k++)
        if (flipq[k].pending && now_us / 1000 >= flipq[k].at) { flipq[k].pending = NULL; display_flip_done(&a->heads[k]); }
}
static const display_ops_t ops_flip = { .enable = f_enable, .detect = f_detect, .fb_alloc = f_fb_alloc, .fb_free = f_fb_free,
                                        .set_mode = f_set_mode, .flip = f_flip, .poll = f_poll };
static const display_ops_t ops_flush = { .enable = f_enable, .detect = f_detect, .fb_alloc = f_fb_alloc, .fb_free = f_fb_free,
                                         .set_mode = f_set_mode, .flush = f_flush, .poll = f_poll };

/* ---------------- the desktop ---------------- */

static u32 *desk;
static int dw, dh;
static u32 fwmem[640 * 480];

static void new_desktop(void) {
    int w, h;
    display_desktop_size(&w, &h);
    u32 *n = malloc((size_t)w * (size_t)h * 4);
    for (int i = 0; i < w * h; i++) n[i] = rnd() & 0xFFFFFF;
    display_set_source(n, w, h, w);
    /* (nothing may read the old one any more: the core drained) */
    for (int i = 0; i < E.n; i++) if (E.op[i].kind == 1) violation("uploads in flight after the source changed");
    free(desk);
    desk = n; dw = w; dh = h;
    display_present(0, 0, w, h);
}

static void draw_some(int n) {
    for (int i = 0; i < n; i++) {
        int w = 1 + (int)(rnd() % 200), h = 1 + (int)(rnd() % 120), x = (int)(rnd() % (u32)dw) - 20, y = (int)(rnd() % (u32)dh) - 20;
        u32 c = rnd() & 0xFFFFFF;
        int x0 = MAX(x, 0), y0 = MAX(y, 0), x1 = MIN(x + w, dw), y1 = MIN(y + h, dh);
        if (x1 <= x0 || y1 <= y0) continue;
        for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) desk[yy * dw + xx] = c ^ (u32)(xx * 7);
        display_present(x0, y0, x1 - x0, y1 - y0);
    }
}

/* every head's buffer on screen equals its part of the desktop */
static int screens_match(const char *what) {
    display_output_t o[8];
    int n = display_outputs(o, 8), bad = 0;
    for (int i = 0; i < n; i++) {
        display_head_t *h = o[i].head;
        display_fb_t *fb = &h->fb[h->front];
        for (int y = 0; y < o[i].h && !bad; y++)
            for (int x = 0; x < o[i].w; x++) {
                u32 want = desk[(o[i].y + y) * dw + o[i].x + x], got = ((u32 *)((u8 *)fb->px + (u64)y * fb->pitch))[x];
                if (want != got) {
                    printf("presenttest: %s: %s pixel %d,%d is %06x, the desktop %06x\n", what, o[i].conn->name, x, y, got, want);
                    bad = 1;
                    break;
                }
            }
    }
    return !bad;
}

static void service(int steps) {
    for (int i = 0; i < steps; i++) {
        advance_ms(1 + rnd() % 4);
        display_service();
    }
}

static display_adapter_t *A;

static void setup(const display_ops_t *ops, int nheads, int with_accel, u32 delay_us) {
    memset(&E, 0, sizeof E);
    E.delay_us = delay_us;
    memset(&g_bootinfo, 0, sizeof g_bootinfo);
    g_bootinfo.fb_addr = (u64)(uintptr_t)fwmem;
    g_bootinfo.fb_width = 640; g_bootinfo.fb_height = 480; g_bootinfo.fb_pitch = 640 * 4; g_bootinfo.fb_bpp = 32;
    g_bootinfo.fb_red_pos = 16; g_bootinfo.fb_green_pos = 8; g_bootinfo.fb_blue_pos = 0;
}

/* ======================================================================
 * the tests
 * ====================================================================== */

/* display.c keeps its state in statics: the test runs one scenario per
 * process (fork), each from a fresh display_init */
#include <unistd.h>
#include <sys/wait.h>

static void boot(const display_ops_t *ops, int nheads, int with_accel, u32 delay_us, const char *driver) {
    setup(ops, nheads, with_accel, delay_us);
    display_init();
    A = display_register(driver, "Fake GPU", ops, NULL, NULL);
    A->live = 1;
    A->owns_fw_fb = 1;
    A->virtual_sizes = 1;
    A->max_w = 4096; A->max_h = 4096;
    for (int k = 0; k < nheads; k++) display_add_head(A, ops->flip ? HEAD_CAP_FLIP : 0, 0);
    for (int k = 0; k < nheads; k++) { char n[16]; snprintf(n, sizeof n, "Fake-%d", k + 1); display_add_connector(A, n, CONN_DP, 1u << k); }
    if (with_accel) { A->accel_ops = &accel_ops; display_accel_ready(A, ACCEL_UPLOAD | ACCEL_COPY | ACCEL_FILL, "fake copy engine"); }
    display_start();
    new_desktop();
}

static void scenario_flip(void) {
    boot(&ops_flip, 2, 1, 3000, "fake");
    CHECK(display_outputs((display_output_t[8]){ 0 }, 8) == 2, "two outputs");
    for (int round = 0; round < 300; round++) {
        draw_some(1 + (int)(rnd() % 6));
        display_commit();
        service((int)(rnd() % 3));
    }
    display_sync();
    service(40);
    display_sync();
    CHECK(screens_match("flips"), "flips: the screens equal the desktop");
    CHECK(!E.violations, "flips: %d ordering violations (%s)", E.violations, E.vwhy);
    CHECK(E.flips > 50 && E.copies > 50 && E.uploads > 100, "flips: %u flips, %u copies, %u uploads", E.flips, E.copies, E.uploads);
    CHECK(A->accel && !E.stops, "flips: acceleration still on");
    /* a mode change with work in flight: the old buffers go only when nothing uses them */
    draw_some(8);
    display_commit();
    display_conf_t cf;
    display_get_conf(&cf);
    for (int i = 0; i < cf.n; i++) if (cf.out[i].attach && i == 0) { cf.out[i].w = 1024; cf.out[i].h = 768; }
    CHECK(display_set_conf(&cf) == 0, "mode change");
    new_desktop();
    for (int round = 0; round < 50; round++) { draw_some(4); display_commit(); service(2); }
    display_sync(); service(30); display_sync();
    CHECK(screens_match("after the mode change"), "mode change: screens");
    CHECK(!E.violations, "mode change: %d violations (%s)", E.violations, E.vwhy);
    CHECK(E.fills >= 2, "new buffers filled by the device (%u fills)", E.fills);
    /* the speed test */
    u64 b, us, cpu;
    u32 n;
    char why[128];
    CHECK(display_accel_bench(0, &b, &us, &cpu, &n, why, sizeof why) == 0 && n > 0 && b > 0 && cpu < us, "bench fill: %s", why);
    CHECK(display_accel_bench(1, &b, &us, &cpu, &n, why, sizeof why) == 0 && n > 0, "bench upload: %s", why);
    CHECK(display_accel_bench(2, &b, &us, &cpu, &n, why, sizeof why) == 0 && n > 0, "bench copy: %s", why);
    CHECK(!E.violations, "bench: %s", E.vwhy);
    char st[160];
    display_accel_state(A, st, sizeof st);
    CHECK(strstr(st, "in use: fake copy engine"), "state: %s", st);
}

static void scenario_flush(void) {
    boot(&ops_flush, 1, 1, 2000, "fake");
    for (int round = 0; round < 400; round++) {
        draw_some(1 + (int)(rnd() % 5));
        display_commit();
        service((int)(rnd() % 2));
    }
    display_sync(); service(20); display_sync();
    CHECK(screens_match("single-buffered"), "single-buffered: the screen equals the desktop");
    CHECK(!E.violations, "single-buffered: %d violations (%s)", E.violations, E.vwhy);
    CHECK(E.uploads > 300 && E.flushes > 100, "single-buffered: %u uploads, %u flushes", E.uploads, E.flushes);
}

/* the copy engine fails in some way part of the way through */
static void scenario_fail(int how) {
    boot(how == 3 ? &ops_flush : &ops_flip, how == 3 ? 1 : 2, 1, 2500, "fake");
    for (int round = 0; round < 200; round++) {
        if (round == 60) {
            if (how == 0) E.hang_after = (int)E.seq + 1;
            if (how == 1) E.error_after = (int)E.seq + 1;
            if (how == 2) E.refuse = 1;
            if (how == 3) E.hang_after = (int)E.seq + 1;
            if (how == 4) E.nospc_every = 3;
        }
        draw_some(1 + (int)(rnd() % 5));
        display_commit();
        service(1 + (int)(rnd() % 2));
    }
    advance_ms(300);
    service(60);
    display_sync(); service(30); display_sync();
    static const char *names[] = { "hang", "error", "refused", "hang (single-buffered)", "no room" };
    CHECK(screens_match(names[how]), "%s: the screen equals the desktop", names[how]);
    CHECK(!E.violations, "%s: %d violations (%s)", names[how], E.violations, E.vwhy);
    if (how == 4) {
        CHECK(A->accel && E.stops == 0 && A->accel_cpu > 0, "no room: acceleration stays, %u rectangles by the CPU (%s)", A->accel_cpu, A->accel_why);
    } else {
        CHECK(!A->accel && E.stops == 1, "%s: acceleration off, the driver stopped once (%d)", names[how], E.stops);
        CHECK(logged("acceleration off for this session"), "%s: logged", names[how]);
        u32 before = E.uploads;
        draw_some(10);
        display_sync(); service(30); display_sync();
        CHECK(E.uploads == before && screens_match("after the failure"), "%s: the CPU presents afterwards", names[how]);
        char st[160];
        display_accel_state(A, st, sizeof st);
        CHECK(!strncmp(st, "off: ", 5), "state: %s", st);
    }
}

static void scenario_guard(void) {
    nset = 0;
    boot(&ops_flip, 1, 0, 1000, "fake");
    CHECK(display_guard_settled(A), "settled (not guarded)");
    CHECK(display_accel_guard_begin(A) == 0 && !strcmp(settings_get("display.fake.accel.guard", ""), "start"), "marker saved");
    A->accel_ops = &accel_ops;
    display_accel_ready(A, ACCEL_UPLOAD | ACCEL_COPY | ACCEL_FILL, "fake");
    service(5);
    CHECK(settings_get("display.fake.accel.guard", NULL), "the marker stays the first seconds");
    advance_ms(10500);
    service(2);
    CHECK(!settings_get("display.fake.accel.guard", NULL), "the marker goes after 10 s");
    /* a marker from the last boot: blocked */
    settings_set("display.fake.accel.guard", "start");
    CHECK(display_accel_guard_begin(A) == E_PERM && !strcmp(settings_get("display.fake.accel.guard", ""), "blocked") &&
          strstr(A->accel_why, "blocked"), "blocked: %s", A->accel_why);
    CHECK(display_accel_guard_begin(A) == E_PERM && !strcmp(settings_get("display.fake.accel.guard", ""), "blocked"), "stays blocked");
    /* a failure while the marker is armed keeps it */
    settings_set("display.fake.accel.guard", NULL);
    display_accel_guard_begin(A);
    display_accel_ready(A, ACCEL_UPLOAD, "fake");
    display_accel_lost(A, "test failure");
    advance_ms(11000);
    service(2);
    CHECK(!strcmp(settings_get("display.fake.accel.guard", ""), "start") && logged("acceleration stays off at the next start"), "kept after a failure");
    /* a clean shutdown with acceleration working clears it */
    settings_set("display.fake.accel.guard", NULL);
    display_accel_guard_begin(A);
    display_accel_ready(A, ACCEL_UPLOAD | ACCEL_COPY, "fake");
    display_sync();
    CHECK(!settings_get("display.fake.accel.guard", NULL), "cleared at shutdown");
}

static void scenario_softhook(int hang) {
    strlcpy(hook_accel, hang ? "fake2:hang" : "fake2", sizeof hook_accel);
    boot(&ops_flip, 2, 0, 0, "fake2");
    CHECK(A->accel_ops && A->accel == (ACCEL_UPLOAD | ACCEL_COPY | ACCEL_FILL), "software hook installed");
    for (int round = 0; round < 250; round++) {
        draw_some(1 + (int)(rnd() % 4));
        display_commit();
        service(1 + (int)(rnd() % 3));
    }
    advance_ms(300);
    service(40);
    display_sync(); service(30); display_sync();
    CHECK(screens_match(hang ? "software hook, hang" : "software hook"), "software hook%s: screens", hang ? " (hang)" : "");
    CHECK(hang ? !A->accel : (A->accel && A->accel_uploads > 100 && A->accel_copies > 50), "software hook%s: %u uploads, %u copies", hang ? " (hang)" : "",
          A->accel_uploads, A->accel_copies);
    hook_accel[0] = 0;
}

#include <sys/mman.h>
static int *shared;                             /* checks and failures of the scenarios (each runs in a child) */
static int run(const char *name, void (*fn)(int), int arg) {
    fflush(stdout);
    pid_t p = fork();
    if (p == 0) {
        if (verbose) printf("  scenario %s %d\n", name, arg);
        fn(arg);
        shared[0] += checks;
        shared[1] += failed;
        fflush(stdout);
        _exit(failed ? 1 : 0);
    }
    int st;
    waitpid(p, &st, 0);
    if (!WIFEXITED(st)) { printf("presenttest: scenario %s %d crashed\n", name, arg); return 1; }
    return WEXITSTATUS(st);
}
static void s_flip(int a) { (void)a; scenario_flip(); }
static void s_flush(int a) { (void)a; scenario_flush(); }
static void s_fail(int a) { scenario_fail(a); }
static void s_guard(int a) { (void)a; scenario_guard(); }
static void s_soft(int a) { scenario_softhook(a); }

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "-v")) verbose = 1;
    shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) return 2;
    int bad = 0;
    bad |= run("flip", s_flip, 0);
    bad |= run("flush", s_flush, 0);
    for (int i = 0; i < 5; i++) bad |= run("fail", s_fail, i);
    bad |= run("guard", s_guard, 0);
    bad |= run("soft", s_soft, 0);
    bad |= run("soft-hang", s_soft, 1);
    if (shared[1] || bad) printf("presenttest: %d of %d checks FAILED\n", shared[1] ? shared[1] : 1, shared[0]);
    else printf("presenttest: all %d checks passed (flip ordering, pixel-checked presentation with acceleration on double- and single-buffered heads, "
                "hangs / errors / refusals / full queues falling back to the CPU, mode changes and new desktop surfaces with work in flight, the "
                "speed test, the acceleration guard, the software test hook)\n", shared[0]);
    return shared[1] || bad;
}
