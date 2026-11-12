/* Halcyon OS - the display core (see display.h and docs/DESIGN-M10-DISPLAY.md):
 * the adapter registry, the firmware frame buffer adapter, mode lists,
 * configurations (from SETTINGS.INI, Display Properties and hotplug), the
 * desktop's presentation to each head (copies, flushes, page flips at
 * vertical blank), the hardware cursor and the fallback to the firmware
 * frame buffer. Everything but display_init runs on the GUI thread. */
#include "display.h"
#include "pci.h"
#include "gui/layout.h"
#include "core/lib.h"
#include "core/io.h"
#include "core/irq.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/settings.h"

/* native drivers (their probes register adapters); weak: a build without one still links */
void bochs_display_probe(void) __attribute__((weak));
void virtio_gpu_probe(void) __attribute__((weak));
void nv_display_probe(void) __attribute__((weak));
/* the shell's notification balloon and the boot log (weak: host builds) */
void shell_balloon(const char *title, const char *text) __attribute__((weak));
int  bootlog_save(const char *reason) __attribute__((weak));

static display_adapter_t adapter_store[DISPLAY_MAX_ADAPTERS];
static int nadapters;
static display_adapter_t *fwa;                 /* the firmware frame buffer */
static display_connector_t *primary;
static int started;
int (*display_changed_hook)(void);

static const u32 *src_px;                      /* the desktop surface */
static int src_w, src_h, src_stride;

static struct {                                /* g_bootinfo's frame buffer as the loader left it */
    u64 addr; u32 w, h, pitch, bpp;
    u8 rp, gp, bp;
} fwfb;

static char fail_hook[48];                     /* test hook: "<driver>:<step>" */
static char guard_hook[24];                    /* test hook: a driver to guard (opt/halcyon/displayguard) */
static char accel_hook[32];                    /* test hook: software acceleration for a driver (opt/halcyon/displayaccel) */
static int init_done;                          /* display_init finished (later registrations: nv_init) */
static int fb_in_bars(const pci_dev_t *d, u64 addr);
static void accel_head_drop(display_head_t *h);

/* ---------------- registry ---------------- */
display_adapter_t *display_register(const char *driver, const char *name, const display_ops_t *ops, void *priv,
                                    pci_dev_t *pci) {
    if (nadapters >= DISPLAY_MAX_ADAPTERS) return NULL;
    display_adapter_t *a = &adapter_store[nadapters++];
    memset(a, 0, sizeof *a);
    a->driver = driver;
    strlcpy(a->name, name, sizeof a->name);
    a->ops = ops;
    a->priv = priv;
    a->pci = pci;
    a->state = ADAPTER_PROBED;
    if (init_done) {
        /* registered after display_init (the NVIDIA driver, from nv_init) */
        if (fb_in_bars(pci, fwfb.addr)) a->owns_fw_fb = 1;
        kprintf("display: found %s (%s)%s\n", a->name, a->driver, a->owns_fw_fb ? ", shows the firmware frame buffer" : "");
    }
    return a;
}

display_head_t *display_add_head(display_adapter_t *a, u32 caps, int cursor_size) {
    if (a->nheads >= DISPLAY_MAX_HEADS) return NULL;
    display_head_t *h = &a->heads[a->nheads];
    memset(h, 0, sizeof *h);
    h->index = a->nheads++;
    h->caps = caps;
    h->cursor_size = cursor_size;
    h->adapter = a;
    return h;
}

display_connector_t *display_add_connector(display_adapter_t *a, const char *name, int type, u32 possible_heads) {
    if (a->nconns >= DISPLAY_MAX_CONNECTORS) return NULL;
    display_connector_t *c = &a->conns[a->nconns];
    memset(c, 0, sizeof *c);
    c->index = a->nconns++;
    strlcpy(c->name, name, sizeof c->name);
    c->type = type;
    c->possible_heads = possible_heads;
    c->adapter = a;
    return c;
}

void display_connector_update(display_connector_t *c, int status, const u8 *edid, int edid_len, int pref_w, int pref_h) {
    c->status = status;
    if (edid_len > DISPLAY_EDID_MAX) edid_len = DISPLAY_EDID_MAX;
    if (!edid || edid_len < 128) edid_len = 0;
    if (edid_len) memcpy(c->edid, edid, (size_t)edid_len);
    c->edid_len = edid_len;
    c->pref_w = pref_w;
    c->pref_h = pref_h;
    edid_info_t ei;
    if (edid_len && edid_parse(c->edid, edid_len, &ei)) c->monitor = ei;
    else memset(&c->monitor, 0, sizeof c->monitor);
}

void display_hotplug(display_adapter_t *a) { a->hotplug = 1; }

int display_test_fail(display_adapter_t *a, const char *step) {
    if (!fail_hook[0]) return 0;
    char want[48];
    snprintf(want, sizeof want, "%s:%s", a->driver, step);
    if (strcasecmp(want, fail_hook)) return 0;
    kprintf("display: %s: %s fails (test hook)\n", a->driver, step);
    return 1;
}

int display_adapter_count(void) { return nadapters; }
display_adapter_t *display_adapter_at(int i) { return i >= 0 && i < nadapters ? &adapter_store[i] : NULL; }

/* adapters whose connectors are shown: started (or the firmware one in use) */
static int adapter_shown(display_adapter_t *a) { return a->state == ADAPTER_ACTIVE; }

int display_connector_count(void) {
    int n = 0;
    for (int i = 0; i < nadapters; i++) if (adapter_shown(&adapter_store[i])) n += adapter_store[i].nconns;
    return n;
}
display_connector_t *display_connector_at(int k) {
    for (int i = 0; i < nadapters; i++) {
        display_adapter_t *a = &adapter_store[i];
        if (!adapter_shown(a)) continue;
        if (k < a->nconns) return &a->conns[k];
        k -= a->nconns;
    }
    return NULL;
}
static void number_connectors(void) {
    for (int i = 0, n = display_connector_count(); i < n; i++) display_connector_at(i)->number = i + 1;
}
display_connector_t *display_primary(void) { return primary; }

/* ---------------- outputs ---------------- */
static int output_heads(display_head_t **out, int max) {
    int n = 0;
    if (primary && primary->head) out[n++] = primary->head;
    for (int i = 0; i < nadapters; i++) {
        display_adapter_t *a = &adapter_store[i];
        if (a->state != ADAPTER_ACTIVE) continue;
        for (int k = 0; k < a->nheads && n < max; k++) {
            display_head_t *h = &a->heads[k];
            if (h->conn && h->conn != primary) out[n++] = h;
        }
    }
    return n;
}

int display_outputs(display_output_t *out, int max) {
    display_head_t *hs[DISPLAY_MAX_OUTPUTS];
    int n = output_heads(hs, MIN(max, DISPLAY_MAX_OUTPUTS));
    for (int i = 0; i < n; i++) {
        out[i].conn = hs[i]->conn;
        out[i].head = hs[i];
        out[i].x = hs[i]->x;
        out[i].y = hs[i]->y;
        out[i].w = hs[i]->mode.w;
        out[i].h = hs[i]->mode.h;
        out[i].primary = hs[i]->conn == primary;
    }
    return n;
}

int display_desktop_size(int *w, int *h) {
    display_head_t *hs[DISPLAY_MAX_OUTPUTS];
    int n = output_heads(hs, DISPLAY_MAX_OUTPUTS), bw = 0, bh = 0;
    for (int i = 0; i < n; i++) {
        bw = MAX(bw, hs[i]->x + hs[i]->mode.w);
        bh = MAX(bh, hs[i]->y + hs[i]->mode.h);
    }
    if (!n) { bw = (int)g_bootinfo.fb_width; bh = (int)g_bootinfo.fb_height; }
    *w = bw; *h = bh;
    return n;
}

/* ---------------- buffers ---------------- */
static void fb_from_bootinfo(display_fb_t *fb) {
    memset(fb, 0, sizeof *fb);
    fb->px = (void *)(uptr)fwfb.addr;
    fb->phys = fwfb.addr;
    fb->w = (int)fwfb.w;
    fb->h = (int)fwfb.h;
    fb->pitch = fwfb.pitch;
    fb->bpp = (u8)fwfb.bpp;
    fb->rpos = fwfb.rp; fb->gpos = fwfb.gp; fb->bpos = fwfb.bp;
}

/* point g_bootinfo.fb_* (the panic screen, About, System Information) at fb */
static void bootinfo_from_fb(const display_fb_t *fb) {
    g_bootinfo.fb_addr = fb->phys ? fb->phys : (u64)(uptr)fb->px;
    g_bootinfo.fb_width = (u32)fb->w;
    g_bootinfo.fb_height = (u32)fb->h;
    g_bootinfo.fb_pitch = fb->pitch;
    g_bootinfo.fb_bpp = fb->bpp;
    g_bootinfo.fb_red_pos = fb->rpos; g_bootinfo.fb_green_pos = fb->gpos; g_bootinfo.fb_blue_pos = fb->bpos;
    g_bootinfo.fb_red_size = g_bootinfo.fb_green_size = g_bootinfo.fb_blue_size = 8;
}

/* write-combining for buffers inside a display BAR (never other device memory) */
static void enable_wc(const display_fb_t *fb) {
    if (!fb->phys) return;
    u64 size = (u64)fb->pitch * (u64)fb->h;
    u64 a = fb->phys & ~0x1FFFFFull, e = (fb->phys + size + 0x1FFFFF) & ~0x1FFFFFull;
    for (int i = 0; i < pci_count(); i++) {
        pci_dev_t *d = pci_get(i);
        if (d->class_code != 0x03) continue;
        for (int b = 0; b < 6; b++) {
            pci_bar_t *bar = &d->bar[b];
            if (bar->is_io || !bar->addr || bar->addr > a || bar->addr + bar->size < e) continue;
            if (mm_set_write_combining(a, e - a)) {
                kprintf("display: frame buffer %llx-%llx write-combining\n", (unsigned long long)a, (unsigned long long)e - 1);
                return;
            }
        }
    }
    if (fb->phys == fwfb.addr)
        kprintf("display: frame buffer stays uncached (no PAT, or not inside a display BAR)\n");
}

static int fb_in_bars(const pci_dev_t *d, u64 addr) {
    if (!d || !addr) return 0;
    for (int b = 0; b < 6; b++)
        if (!d->bar[b].is_io && d->bar[b].addr && addr >= d->bar[b].addr && addr < d->bar[b].addr + d->bar[b].size) return 1;
    return 0;
}

static void head_free_fbs(display_head_t *h) {
    display_adapter_t *a = h->adapter;
    accel_head_drop(h);                        /* nothing may still write into them */
    for (int i = 0; i < h->nfb; i++)
        if (a->ops->fb_free && a != fwa) a->ops->fb_free(a, &h->fb[i]);
    memset(h->fb, 0, sizeof h->fb);
    h->nfb = 0;
    h->front = 0;
    h->flip_pending = 0;
    h->stale_n[0] = h->stale_n[1] = 0;
    h->dirty_n = 0;
}

/* ---------------- copies from the desktop ---------------- */
typedef struct { int x, y, w, h; } drect_t;

static void rect_add(drect_t *list, int *n, int max, drect_t r) {
    for (int i = 0; i < *n; i++) {
        drect_t a = list[i];
        int x0 = MIN(a.x, r.x), y0 = MIN(a.y, r.y), x1 = MAX(a.x + a.w, r.x + r.w), y1 = MAX(a.y + a.h, r.y + r.h);
        long u = (long)(x1 - x0) * (y1 - y0);
        if (u <= (long)a.w * a.h + (long)r.w * r.h + 4096) { list[i] = (drect_t){ x0, y0, x1 - x0, y1 - y0 }; return; }
    }
    if (*n < max) { list[(*n)++] = r; return; }
    drect_t u = list[0];
    for (int i = 1; i < *n; i++) {
        int x0 = MIN(u.x, list[i].x), y0 = MIN(u.y, list[i].y);
        int x1 = MAX(u.x + u.w, list[i].x + list[i].w), y1 = MAX(u.y + u.h, list[i].y + list[i].h);
        u = (drect_t){ x0, y0, x1 - x0, y1 - y0 };
    }
    int x0 = MIN(u.x, r.x), y0 = MIN(u.y, r.y), x1 = MAX(u.x + u.w, r.x + r.w), y1 = MAX(u.y + u.h, r.y + r.h);
    list[0] = (drect_t){ x0, y0, x1 - x0, y1 - y0 };
    *n = 1;
}

/* copy head rectangle r (head coordinates) from the desktop into fb */
static void copy_rect(display_head_t *h, display_fb_t *fb, drect_t r) {
    if (!src_px || !fb->px) return;
    int sx = h->x + r.x, sy = h->y + r.y;
    if (sx < 0 || sy < 0 || sx + r.w > src_w || sy + r.h > src_h || r.x + r.w > fb->w || r.y + r.h > fb->h) {
        /* clip to the desktop and the buffer */
        int x0 = MAX(r.x, MAX(-h->x, 0)), y0 = MAX(r.y, MAX(-h->y, 0));
        int x1 = MIN(MIN(r.x + r.w, fb->w), src_w - h->x), y1 = MIN(MIN(r.y + r.h, fb->h), src_h - h->y);
        if (x1 <= x0 || y1 <= y0) return;
        r = (drect_t){ x0, y0, x1 - x0, y1 - y0 };
        sx = h->x + r.x; sy = h->y + r.y;
    }
    u8 *base = fb->px;
    int native = fb->bpp == 32 && fb->rpos == 16 && fb->gpos == 8 && fb->bpos == 0;
    for (int y = 0; y < r.h; y++) {
        const u32 *s = src_px + (u64)(sy + y) * src_stride + sx;
        u8 *row = base + (u64)(r.y + y) * fb->pitch;
        if (native) {
            memcpy(row + (u64)r.x * 4, s, (size_t)r.w * 4);
        } else if (fb->bpp == 32) {
            u32 *d = (u32 *)row + r.x;
            for (int x = 0; x < r.w; x++) {
                u32 c = s[x];
                d[x] = (((c >> 16) & 255) << fb->rpos) | (((c >> 8) & 255) << fb->gpos) | ((c & 255) << fb->bpos);
            }
        } else {
            u8 *d = row + (u64)r.x * 3;
            for (int x = 0; x < r.w; x++) {
                u32 c = s[x];
                u32 v = (((c >> 16) & 255) << fb->rpos) | (((c >> 8) & 255) << fb->gpos) | ((c & 255) << fb->bpos);
                d[x * 3] = (u8)v; d[x * 3 + 1] = (u8)(v >> 8); d[x * 3 + 2] = (u8)(v >> 16);
            }
        }
    }
}

/* ---------------- acceleration (M10e) ----------------
 * The driver's queued operations go out in display_commit (one submit per
 * adapter); a head whose writes are in flight flushes or flips only when
 * their fence is done (head_commit checks it, display_service calls it
 * again). Operations of one adapter complete in the order they were
 * queued. Anything that fails gives acceleration up for the session. */

static int accel_has(const display_adapter_t *a, u32 caps) { return a->accel_ops && (a->accel & caps) == caps; }

/* a buffer the device writes as the core would (XRGB, 32 bits) */
static int accel_fb_ok(const display_fb_t *fb) {
    return fb->px && fb->bpp == 32 && fb->rpos == 16 && fb->gpos == 8 && fb->bpos == 0;
}

static void head_all_stale(display_head_t *h) {
    drect_t all = { 0, 0, h->mode.w, h->mode.h };
    if (!h->nfb || !h->mode.w) return;
    if (h->nfb == 2 && (h->caps & HEAD_CAP_FLIP)) {
        for (int b = 0; b < 2; b++) { h->stale_n[b] = 0; rect_add((drect_t *)h->stale[b], &h->stale_n[b], 8, all); }
    } else {
        int f = h->front;
        h->stale_n[0] = h->stale_n[1] = 0;
        copy_rect(h, &h->fb[f], all);
        if (h->adapter->ops->flush) { h->dirty_n = 0; rect_add((drect_t *)h->dirty, &h->dirty_n, 8, all); }
    }
}

static void accel_guard_key(const display_adapter_t *a, char *k, int n) { snprintf(k, (size_t)n, "display.%s.accel.guard", a->driver); }

/* acceleration ends for the session: the driver stops, the CPU redraws every buffer */
static void accel_fail(display_adapter_t *a, const char *why_in) {
    if (!a->accel_ops) return;
    char why[96];
    strlcpy(why, why_in ? why_in : "?", sizeof why);       /* (it may be a->accel_why itself) */
    int was = a->accel || a->accel_last != a->accel_seen;
    a->accel = 0;
    if (was || !a->accel_why[0]) strlcpy(a->accel_why, why, sizeof a->accel_why);
    if (was) {
        kprintf("display: %s: acceleration off for this session: %s; the CPU draws\n", a->name, why);
        if (a->accel_ops->stop) a->accel_ops->stop(a, why);
        if (a->accel_guard_armed)
            kprintf("display: %s: the acceleration marker stays (display.%s.accel.guard): acceleration stays off at the next start\n",
                    a->driver, a->driver);
        a->accel_guard_clear_ms = 0;
    }
    a->accel_last = a->accel_seen = 0;
    a->accel_src = NULL;
    for (int k = 0; k < a->nheads; k++) {
        display_head_t *h = &a->heads[k];
        int busy = h->acc_queued || h->acc_fence || h->acc_flip;
        h->acc_queued = h->acc_flip = h->acc_rn = 0;
        h->acc_fence = 0;
        if (h->conn && (busy || was)) head_all_stale(h);
    }
}

void display_accel_lost(display_adapter_t *a, const char *why) { accel_fail(a, why); }

/* a fence was submitted: the clock of the 250 ms bound starts when nothing older is outstanding */
static void accel_note(display_adapter_t *a, u32 f) {
    if (a->accel_seen == a->accel_last) a->accel_last_ms = time_ms();
    a->accel_last = f;
}

/* poll a fence: 1 done, 0 not yet (the idle loop is asked to come back),
 * E_* acceleration failed (accel_fail has run) */
static int accel_check(display_adapter_t *a, u32 fence) {
    if (!a->accel_ops || !fence) return 1;
    int r = a->accel_ops->done(a, fence);
    if (r == 1) {
        if ((i32)(fence - a->accel_seen) > 0) { a->accel_seen = fence; a->accel_last_ms = time_ms(); }
        return 1;
    }
    if (r < 0) { accel_fail(a, a->accel_why[0] ? a->accel_why : "the device reported an error"); return r; }
    if (time_ms() - a->accel_last_ms > DISPLAY_ACCEL_TIMEOUT_MS) {
        char w[64];
        snprintf(w, sizeof w, "a fence did not complete in %d ms", DISPLAY_ACCEL_TIMEOUT_MS);
        accel_fail(a, w);
        return E_TIMEOUT;
    }
    idle_wake_at(time_ms() + 1);
    return 0;
}

/* wait (bounded) until fence is done; 0 or E_* (acceleration failed) */
static int accel_wait(display_adapter_t *a, u32 fence) {
    u64 end = time_ms() + DISPLAY_ACCEL_TIMEOUT_MS;
    for (;;) {
        if (!a->accel_ops || !fence) return 0;
        if (!a->accel && !a->accel_last) return E_IO;
        int r = accel_check(a, fence);
        if (r) return r > 0 ? 0 : r;
        if (time_ms() >= end) {
            accel_fail(a, "a fence did not complete in time");
            return E_TIMEOUT;
        }
        cpu_relax();
    }
}

static void accel_drain(display_adapter_t *a) {
    if (a->accel_ops && a->accel_last && a->accel_last != a->accel_seen) accel_wait(a, a->accel_last);
}

/* a head's buffers go away (mode set, head off, failure): its writes first */
static void accel_head_drop(display_head_t *h) {
    display_adapter_t *a = h->adapter;
    if (!a || !a->accel_ops) return;
    if (h->acc_queued || h->acc_fence) {
        if (h->acc_queued && a->accel) {
            u32 f = a->accel_ops->submit(a);
            if (f) { accel_note(a, f); for (int k = 0; k < a->nheads; k++) if (a->heads[k].acc_queued) { a->heads[k].acc_queued = 0; a->heads[k].acc_fence = f; } }
        }
        accel_drain(a);
    }
    h->acc_queued = h->acc_flip = h->acc_rn = 0;
    h->acc_fence = 0;
}

/* the desktop surface mapped for the device (once per surface) */
static int accel_src(display_adapter_t *a) {
    if (!src_px || !a->accel_ops->source) return 0;
    u64 bytes = (u64)src_stride * 4 * (u64)src_h;
    if (a->accel_src == src_px && a->accel_src_bytes == bytes) return a->accel_src_ok;
    accel_drain(a);
    if (!a->accel) return 0;
    a->accel_src = src_px;
    a->accel_src_bytes = bytes;
    int r = a->accel_ops->source(a, src_px, bytes);
    a->accel_src_ok = !r;
    if (r) kprintf("display: %s: the desktop surface (%llu KiB) is not readable by the device (%s): uploads by the CPU\n", a->name,
                   (unsigned long long)(bytes >> 10), strerror(r));
    return a->accel_src_ok;
}

static void acc_frame(display_head_t *h) {
    display_adapter_t *a = h->adapter;
    if (++h->acc_frames == 1 || h->acc_frames == 100)
        kprintf("display: %s: %u updates presented through acceleration (%u uploads, %u copies, %u fills so far)\n", h->conn ? h->conn->name : "?",
                h->acc_frames, a->accel_uploads, a->accel_copies, a->accel_fills);
}

/* queue the upload of head rectangle r into fb; 1 queued, 0 the CPU must do it */
static int accel_upload(display_head_t *h, display_fb_t *fb, drect_t r) {
    display_adapter_t *a = h->adapter;
    int sx = h->x + r.x, sy = h->y + r.y;
    if (sx < 0 || sy < 0 || sx + r.w > src_w || sy + r.h > src_h || r.x + r.w > fb->w || r.y + r.h > fb->h) {
        int x0 = MAX(r.x, MAX(-h->x, 0)), y0 = MAX(r.y, MAX(-h->y, 0));
        int x1 = MIN(MIN(r.x + r.w, fb->w), src_w - h->x), y1 = MIN(MIN(r.y + r.h, fb->h), src_h - h->y);
        if (x1 <= x0 || y1 <= y0) return 1;                     /* nothing of it is visible */
        r = (drect_t){ x0, y0, x1 - x0, y1 - y0 };
        sx = h->x + r.x; sy = h->y + r.y;
    }
    int e = a->accel_ops->upload(a, fb, r.x, r.y, r.w, r.h, ((u64)sy * (u64)src_stride + (u64)sx) * 4, (u32)src_stride * 4);
    if (!e) { a->accel_uploads++; return 1; }
    a->accel_cpu++;
    if (e != E_NOSPC) {
        char w[64];
        snprintf(w, sizeof w, "an upload could not be queued (%s)", strerror(e));
        accel_fail(a, w);
    }
    return 0;
}

/* the flip's back buffer brought up to date by the device: what the front
 * buffer has (every pixel outside stale[front] is current there) copied
 * from it, then what neither has uploaded from the desktop. 1 queued (the
 * flip follows when they are done), 0 the CPU does it. */
static int accel_flip_prepare(display_head_t *h, int back) {
    display_adapter_t *a = h->adapter;
    int f = !back;
    display_fb_t *fb = &h->fb[back], *fr = &h->fb[f];
    if (!accel_has(a, ACCEL_UPLOAD) || !accel_fb_ok(fb) || !accel_src(a) || !a->accel) return 0;
    int all_stale = h->stale_n[f] == 1 && h->stale[f][0].x <= 0 && h->stale[f][0].y <= 0 && h->stale[f][0].w >= h->mode.w &&
                    h->stale[f][0].h >= h->mode.h;
    int use_copy = accel_has(a, ACCEL_COPY) && accel_fb_ok(fr) && !all_stale && fr->w == fb->w && fr->h == fb->h;
    int ok = 1;
    h->acc_rn = 0;
    if (use_copy)
        for (int i = 0; i < h->stale_n[back] && ok; i++) {
            drect_t r = { h->stale[back][i].x, h->stale[back][i].y, h->stale[back][i].w, h->stale[back][i].h };
            int x0 = MAX(r.x, 0), y0 = MAX(r.y, 0), x1 = MIN(r.x + r.w, fb->w), y1 = MIN(r.y + r.h, fb->h);
            if (x1 <= x0 || y1 <= y0) continue;
            int e = a->accel_ops->copy(a, fb, x0, y0, fr, x0, y0, x1 - x0, y1 - y0);
            if (e) {
                ok = 0;
                if (e != E_NOSPC) { char w[64]; snprintf(w, sizeof w, "a copy could not be queued (%s)", strerror(e)); accel_fail(a, w); }
                break;
            }
            a->accel_copies++;
            rect_add((drect_t *)h->acc_r, &h->acc_rn, 8, (drect_t){ x0, y0, x1 - x0, y1 - y0 });
        }
    int which = use_copy ? f : back;
    for (int i = 0; i < h->stale_n[which] && ok; i++) {
        drect_t r = { h->stale[which][i].x, h->stale[which][i].y, h->stale[which][i].w, h->stale[which][i].h };
        if (!accel_upload(h, fb, r)) ok = 0;
        else rect_add((drect_t *)h->acc_r, &h->acc_rn, 8, r);
    }
    if (!ok) {
        /* part of it may be queued: let it finish, then the CPU does all of it */
        if (a->accel) {
            u32 fe = a->accel_ops->submit(a);
            if (fe) { accel_note(a, fe); accel_wait(a, fe); }
        }
        h->acc_rn = 0;
        return 0;
    }
    h->stale_n[back] = 0;
    h->frames++;
    acc_frame(h);
    h->acc_queued = 1;
    h->acc_flip = 1;
    h->acc_back = back;
    return 1;
}

/* the queued operations of every adapter go out */
static void accel_submit_all(void) {
    for (int i = 0; i < nadapters; i++) {
        display_adapter_t *a = &adapter_store[i];
        if (!a->accel_ops || a->state != ADAPTER_ACTIVE) continue;
        int any = 0;
        for (int k = 0; k < a->nheads; k++) any |= a->heads[k].acc_queued;
        if (!any) continue;
        u32 f = a->accel ? a->accel_ops->submit(a) : 0;
        if (!f) { accel_fail(a, "the queued operations could not be sent"); continue; }
        accel_note(a, f);
        for (int k = 0; k < a->nheads; k++)
            if (a->heads[k].acc_queued) { a->heads[k].acc_queued = 0; a->heads[k].acc_fence = f; }
    }
}

static void accel_flush_rects(display_head_t *h) {
    if (h->adapter->ops->flush)
        for (int i = 0; i < h->acc_rn; i++) h->adapter->ops->flush(h, &h->fb[h->acc_back], h->acc_r[i].x, h->acc_r[i].y, h->acc_r[i].w, h->acc_r[i].h);
    h->acc_rn = 0;
}

void display_set_source(const u32 *px, int w, int h, int stride) {
    /* uploads in flight read the old surface (freed after this): they finish first */
    for (int i = 0; i < nadapters; i++) accel_drain(&adapter_store[i]);
    src_px = px; src_w = w; src_h = h; src_stride = stride;
}

void display_present(int x, int y, int w, int hgt) {
    display_head_t *hs[DISPLAY_MAX_OUTPUTS];
    int n = output_heads(hs, DISPLAY_MAX_OUTPUTS);
    for (int i = 0; i < n; i++) {
        display_head_t *h = hs[i];
        int x0 = MAX(x, h->x), y0 = MAX(y, h->y);
        int x1 = MIN(x + w, h->x + h->mode.w), y1 = MIN(y + hgt, h->y + h->mode.h);
        if (x1 <= x0 || y1 <= y0 || !h->nfb) continue;
        drect_t r = { x0 - h->x, y0 - h->y, x1 - x0, y1 - y0 };
        if (h->nfb == 2 && (h->caps & HEAD_CAP_FLIP)) {
            for (int b = 0; b < 2; b++) rect_add((drect_t *)h->stale[b], &h->stale_n[b], 8, r);
            continue;
        }
        display_adapter_t *a = h->adapter;
        display_fb_t *ffb = &h->fb[h->front];
        if (accel_has(a, ACCEL_UPLOAD) && accel_fb_ok(ffb) && accel_src(a) && accel_has(a, ACCEL_UPLOAD) && accel_upload(h, ffb, r)) {
            /* queued: flushed once its fence is done (head_commit) */
            h->acc_queued = 1;
            acc_frame(h);
            if (a->ops->flush) rect_add((drect_t *)h->dirty, &h->dirty_n, 8, r);
            else h->frames++;
            continue;
        }
        copy_rect(h, &h->fb[h->front], r);
        if (h->adapter->ops->flush) rect_add((drect_t *)h->dirty, &h->dirty_n, 8, r);
        else h->frames++;
    }
}

/* flips were given up on a double-buffered head (a failed flip, one that
 * never completed): the buffer on screen gets what it lacks, then the head
 * is single buffered on it */
static void flips_gone(display_head_t *h) {
    int f = h->front;
    for (int i = 0; i < h->stale_n[f]; i++) {
        drect_t r = { h->stale[f][i].x, h->stale[f][i].y, h->stale[f][i].w, h->stale[f][i].h };
        copy_rect(h, &h->fb[f], r);
        if (h->adapter->ops->flush) rect_add((drect_t *)h->dirty, &h->dirty_n, 8, r);
    }
    h->stale_n[0] = h->stale_n[1] = 0;
}

static void head_commit(display_head_t *h);

/* queue the page flip to fb[back] (its contents are complete) */
static void head_flip(display_head_t *h, int back) {
    display_adapter_t *a = h->adapter;
    h->flip_pending = 1;
    h->flip_ms = time_ms();
    if (a->ops->flip(h, &h->fb[back])) {
        /* the flip failed: nothing changed on screen, so the front buffer
         * stays; it gets what the back one just got (single buffering from now on) */
        kprintf("display: %s: page flip failed, single buffering\n", h->conn ? h->conn->name : "?");
        h->flip_pending = 0;
        h->caps &= ~HEAD_CAP_FLIP;
        drect_t all = { 0, 0, h->mode.w, h->mode.h };
        rect_add((drect_t *)h->stale[h->front], &h->stale_n[h->front], 8, all);
        head_commit(h);
    }
}

static void head_commit(display_head_t *h) {
    display_adapter_t *a = h->adapter;
    /* acceleration in flight: the flush or the flip waits for its fence */
    if (h->acc_queued) return;                    /* (display_commit submits, then comes back) */
    if (h->acc_fence) {
        int d = accel_check(a, h->acc_fence);
        if (!d) return;
        if (d > 0) {
            h->acc_fence = 0;
            if (h->acc_flip) {
                h->acc_flip = 0;
                accel_flush_rects(h);
                if (!h->flip_pending && h->nfb == 2) {
                    if (h->caps & HEAD_CAP_FLIP) { head_flip(h, h->acc_back); return; }
                    /* flips ended meanwhile: the back buffer is complete, the front one is redrawn */
                    drect_t all = { 0, 0, h->mode.w, h->mode.h };
                    rect_add((drect_t *)h->stale[h->front], &h->stale_n[h->front], 8, all);
                }
            }
        }
        /* (d < 0: accel_fail made everything stale; the CPU paths below) */
    }
    if (h->nfb == 2 && !(h->caps & HEAD_CAP_FLIP)) {
        if (h->flip_pending) return;
        flips_gone(h);
        if (!h->dirty_n) return;
        for (int i = 0; i < h->dirty_n; i++)
            a->ops->flush(h, &h->fb[h->front], h->dirty[i].x, h->dirty[i].y, h->dirty[i].w, h->dirty[i].h);
        h->dirty_n = 0;
        h->frames++;
        return;
    }
    if (h->nfb == 1) {
        if (!h->dirty_n) return;
        for (int i = 0; i < h->dirty_n; i++)
            a->ops->flush(h, &h->fb[h->front], h->dirty[i].x, h->dirty[i].y, h->dirty[i].w, h->dirty[i].h);
        h->dirty_n = 0;
        h->frames++;
        return;
    }
    if (h->flip_pending) return;
    int back = !h->front;
    if (!h->stale_n[back]) return;
    if (accel_flip_prepare(h, back)) return;      /* the device brings it up to date; the flip waits for it */
    display_fb_t *fb = &h->fb[back];
    for (int i = 0; i < h->stale_n[back]; i++) {
        drect_t r = { h->stale[back][i].x, h->stale[back][i].y, h->stale[back][i].w, h->stale[back][i].h };
        copy_rect(h, fb, r);
        if (a->ops->flush) a->ops->flush(h, fb, r.x, r.y, r.w, r.h);
    }
    h->stale_n[back] = 0;
    h->frames++;
    head_flip(h, back);
}

void display_commit(void) {
    display_head_t *hs[DISPLAY_MAX_OUTPUTS];
    int n = output_heads(hs, DISPLAY_MAX_OUTPUTS), acc = 0;
    for (int i = 0; i < n; i++) head_commit(hs[i]);
    for (int i = 0; i < n; i++) acc |= hs[i]->acc_queued;
    if (!acc) return;
    accel_submit_all();
    /* a device that is already done (or failed): flush / flip now */
    for (int i = 0; i < n; i++) if (hs[i]->acc_fence || hs[i]->dirty_n) head_commit(hs[i]);
}

void display_sync(void) {
    for (int round = 0; round < 3; round++) {
        display_commit();
        u64 end = time_ms() + 100;
        for (;;) {
            int pending = 0;
            for (int i = 0; i < nadapters; i++) {
                display_adapter_t *a = &adapter_store[i];
                if (a->state != ADAPTER_ACTIVE) continue;
                if (a->ops->poll) a->ops->poll(a);
                for (int k = 0; k < a->nheads; k++)
                    pending |= a->heads[k].flip_pending | a->heads[k].acc_flip | (a->heads[k].acc_fence != 0);
            }
            if (!pending || time_ms() > end) break;
            display_commit();                      /* (finishes accelerated frames: their flush / flip) */
            cpu_relax();
        }
    }
    /* a clean shutdown: acceleration worked this far, its marker goes */
    for (int i = 0; i < nadapters; i++) {
        display_adapter_t *a = &adapter_store[i];
        if (a->accel_guard_armed && a->accel) {
            char k[SETTINGS_KEY_MAX];
            accel_guard_key(a, k, sizeof k);
            settings_set(k, NULL);
            settings_save();
            a->accel_guard_armed = 0;
            a->accel_guard_clear_ms = 0;
            kprintf("display: %s: acceleration marker cleared (shutting down)\n", a->driver);
        }
    }
}

void display_flip_done(display_head_t *h) {
    if (!h->flip_pending) return;
    h->flip_pending = 0;
    h->front = !h->front;
    h->flips++;
    u32 waited = (u32)(time_ms() - h->flip_ms);
    if (waited > h->flip_wait_max_ms) h->flip_wait_max_ms = waited;
    if (h->flips == 1 || h->flips == 100)
        kprintf("display: %s: %u page flips completed at vertical blank (longest wait %u ms)\n",
                h->conn ? h->conn->name : "?", h->flips, h->flip_wait_max_ms);
    if (h->conn == primary) bootinfo_from_fb(&h->fb[h->front]);
}

/* ---------------- the hardware cursor ---------------- */
static u32 cur_img[64 * 64];
static int cur_hx, cur_hy, cur_set, cur_x, cur_y, cur_vis;
static int hw_cursor_ok, hw_cursor_logged;

int display_hw_cursor(void) {
    display_head_t *hs[DISPLAY_MAX_OUTPUTS];
    int n = output_heads(hs, DISPLAY_MAX_OUTPUTS);
    if (!n || !hw_cursor_ok) return 0;
    for (int i = 0; i < n; i++)
        if (!(hs[i]->caps & HEAD_CAP_CURSOR) || !hs[i]->adapter->ops->cursor_image) return 0;
    return 1;
}

static int upload_cursor(display_head_t *h) {
    int sz = h->cursor_size > 0 && h->cursor_size <= 64 ? h->cursor_size : 64;
    static u32 img[64 * 64];
    for (int y = 0; y < sz; y++) memcpy(img + y * sz, cur_img + y * 64, (size_t)sz * 4);
    return h->adapter->ops->cursor_image(h, img, sz, cur_hx, cur_hy);
}

static void cursor_all(void) {
    display_head_t *hs[DISPLAY_MAX_OUTPUTS];
    int n = output_heads(hs, DISPLAY_MAX_OUTPUTS), all = n > 0, failed = 0;
    for (int i = 0; i < n; i++) if (!(hs[i]->caps & HEAD_CAP_CURSOR) || !hs[i]->adapter->ops->cursor_image) all = 0;
    hw_cursor_ok = 0;
    if (!all || !cur_set || !settings_get_int("display.hwcursor", 1)) return;
    for (int i = 0; i < n; i++) {
        hs[i]->cursor_on = 0;
        if (upload_cursor(hs[i])) failed = 1;
    }
    hw_cursor_ok = !failed;
    if (!hw_cursor_logged || failed) {
        char names[96] = "";
        for (int i = 0; i < n; i++) {
            if (i) strlcat(names, ", ", sizeof names);
            strlcat(names, hs[i]->conn->name, sizeof names);
        }
        kprintf("display: hardware cursor %s on %s (%dx%d)\n", failed ? "failed" : "in use", names,
                hs[0]->cursor_size, hs[0]->cursor_size);
        hw_cursor_logged = 1;
    }
}

void display_cursor_image(const u32 *argb64, int hot_x, int hot_y) {
    memcpy(cur_img, argb64, sizeof cur_img);
    cur_hx = hot_x; cur_hy = hot_y;
    int first = !cur_set;
    cur_set = 1;
    if (first) { cursor_all(); return; }
    if (!display_hw_cursor()) return;
    display_head_t *hs[DISPLAY_MAX_OUTPUTS];
    int n = output_heads(hs, DISPLAY_MAX_OUTPUTS);
    for (int i = 0; i < n; i++) upload_cursor(hs[i]);
}

void display_cursor_move(int x, int y, int visible) {
    cur_x = x; cur_y = y; cur_vis = visible;
    if (!display_hw_cursor()) return;
    display_head_t *hs[DISPLAY_MAX_OUTPUTS];
    int n = output_heads(hs, DISPLAY_MAX_OUTPUTS);
    display_head_t *on = NULL;
    for (int i = 0; i < n && !on; i++)
        if (x >= hs[i]->x && y >= hs[i]->y && x < hs[i]->x + hs[i]->mode.w && y < hs[i]->y + hs[i]->mode.h) on = hs[i];
    for (int i = 0; i < n; i++) {
        display_head_t *h = hs[i];
        if (h == on) {
            h->adapter->ops->cursor_move(h, x - h->x, y - h->y, visible);
            h->cursor_on = visible;
        } else if (h->cursor_on) {
            h->adapter->ops->cursor_move(h, 0, 0, 0);
            h->cursor_on = 0;
        }
    }
}

/* ---------------- mode lists ---------------- */
static display_connector_t *valid_conn;
static int valid_cb(void *ctx, const display_mode_t *m) {
    display_adapter_t *a = ctx;
    return a->ops->mode_valid ? a->ops->mode_valid(a, valid_conn, m) : 1;
}

static void fw_add_mode(display_connector_t *c, int w, int h) {
    if (c->nmodes >= DISPLAY_MAX_MODES || w < 640 || h < 480 || modes_find(c->modes, c->nmodes, w, h) >= 0) return;
    int i = c->nmodes++;
    while (i > 0 && (c->modes[i - 1].w > w || (c->modes[i - 1].w == w && c->modes[i - 1].h > h))) {
        c->modes[i] = c->modes[i - 1];
        i--;
    }
    memset(&c->modes[i], 0, sizeof c->modes[i]);
    c->modes[i].w = w;
    c->modes[i].h = h;
    if (c->monitor.valid && c->monitor.native_w == w && c->monitor.native_h == h) {
        c->modes[i].flags |= DT_PREFERRED;
        c->modes[i].hz = c->monitor.native_hz;
    }
}

static int use_flips(display_head_t *h) {
    return (h->caps & HEAD_CAP_FLIP) && h->adapter->ops->flip && settings_get_int("display.flip", 1);
}

static void build_modes(display_connector_t *c) {
    display_adapter_t *a = c->adapter;
    c->nmodes = 0;
    if (a == fwa) {
        for (u32 i = 0; i < g_bootinfo.mode_count && i < BOOTINFO_MAX_MODES; i++)
            fw_add_mode(c, g_bootinfo.modes[i].width, g_bootinfo.modes[i].height);
        fw_add_mode(c, (int)fwfb.w, (int)fwfb.h);
        return;
    }
    int flips = 0;
    for (int k = 0; k < a->nheads; k++) if ((c->possible_heads & (1u << k)) && use_flips(&a->heads[k])) flips = 1;
    mode_limits_t lim = { 0 };
    lim.max_w = a->max_w;
    lim.max_h = a->max_h;
    lim.max_clock_khz = a->max_clock_khz;
    lim.vram = a->vram;
    lim.buffers = flips ? 2 : 1;
    lim.virtual_sizes = a->virtual_sizes;
    lim.rates = a->rates;
    lim.valid = valid_cb;
    lim.ctx = a;
    valid_conn = c;
    c->nmodes = modes_build(c->edid, c->edid_len, &lim, c->pref_w, c->pref_h, c->modes, DISPLAY_MAX_MODES);
}

/* ---------------- settings ---------------- */
static void conn_key(const display_connector_t *c, const char *what, char *out, int n) {
    int o = snprintf(out, (size_t)n, "display.");
    for (const char *s = c->name; *s && o < n - 1; s++)
        if (isalnum((u8)*s) || *s == '-' || *s == '_') out[o++] = (char)tolower((u8)*s);
    out[o] = 0;
    snprintf(out + o, (size_t)(n - o), ".%s", what);
}
static int conn_get(const display_connector_t *c, const char *what, int def) {
    char k[SETTINGS_KEY_MAX];
    conn_key(c, what, k, sizeof k);
    return settings_get_int(k, def);
}
static int conn_has(const display_connector_t *c, const char *what) {
    char k[SETTINGS_KEY_MAX];
    conn_key(c, what, k, sizeof k);
    return settings_get(k, NULL) != NULL;
}

static int name_in_list(const char *list, const char *name) {
    int n = (int)strlen(name);
    for (const char *p = list; p && *p;) {
        while (*p == ' ' || *p == ',') p++;
        const char *e = p;
        while (*e && *e != ',' && *e != ' ') e++;
        if (e - p == n && !strncasecmp(p, name, (size_t)n)) return 1;
        p = e;
    }
    return 0;
}

/* ---------------- configurations ---------------- */
static display_conf_out_t *conf_find(display_conf_t *c, const display_connector_t *conn) {
    for (int i = 0; i < c->n; i++) if (c->out[i].conn == conn) return &c->out[i];
    return NULL;
}

void display_get_conf(display_conf_t *c) {
    memset(c, 0, sizeof *c);
    for (int i = 0; i < nadapters; i++) {
        display_adapter_t *a = &adapter_store[i];
        if (a->state != ADAPTER_ACTIVE) continue;
        for (int k = 0; k < a->nconns && c->n < (int)ARRAY_SIZE(c->out); k++) {
            display_connector_t *conn = &a->conns[k];
            display_conf_out_t *o = &c->out[c->n++];
            o->conn = conn;
            o->attach = conn->head != NULL;
            if (conn->head) {
                o->w = conn->head->mode.w; o->h = conn->head->mode.h;
                o->hz = conn->head->mode.hz;
                o->x = conn->head->x; o->y = conn->head->y;
            } else {
                int d = modes_default(conn->modes, conn->nmodes, 2560, 1600);
                if (d >= 0) { o->w = conn->modes[d].w; o->h = conn->modes[d].h; o->hz = conn->modes[d].hz; }
                o->x = conn_get(conn, "x", 100000); o->y = conn_get(conn, "y", 0);
            }
        }
    }
    c->primary = primary;
}

const display_mode_t *display_conf_mode(const display_conf_out_t *o) {
    display_connector_t *c = o->conn;
    int m = modes_find_rate(c->modes, c->nmodes, o->w, o->h, c->adapter->rates ? o->hz : 0);
    if (m < 0) m = modes_find(c->modes, c->nmodes, o->w, o->h);
    return m >= 0 ? &c->modes[m] : NULL;
}

/* the same mode (size, refresh rate and timing) */
static int same_mode(const display_mode_t *a, const display_mode_t *b) {
    return a->w == b->w && a->h == b->h && a->hz == b->hz && !memcmp(&a->t, &b->t, sizeof a->t) &&
           (a->flags & (DT_HSYNC_POS | DT_VSYNC_POS | DT_INTERLACE)) == (b->flags & (DT_HSYNC_POS | DT_VSYNC_POS | DT_INTERLACE));
}

int display_conf_live(const display_conf_t *c) {
    for (int i = 0; i < c->n; i++) {
        const display_conf_out_t *o = &c->out[i];
        display_connector_t *conn = o->conn;
        const display_mode_t *m = o->attach ? display_conf_mode(o) : NULL;
        int same = (conn->head != NULL) == (o->attach != 0) && (!o->attach || (m && same_mode(&conn->head->mode, m)));
        if (!same && !conn->adapter->live) return 0;
    }
    return 1;
}

/* a free head for connector c (keeping the one it has) */
static display_head_t *pick_head(display_connector_t *c, u32 *taken) {
    display_adapter_t *a = c->adapter;
    if (c->head && !(taken[a - adapter_store] & (1u << c->head->index))) return c->head;
    for (int k = 0; k < a->nheads; k++)
        if ((c->possible_heads & (1u << k)) && !(taken[a - adapter_store] & (1u << k))) return &a->heads[k];
    return NULL;
}

/* switch head h to connector c with mode m (new buffers); 0 or E_* */
static int head_set(display_head_t *h, display_connector_t *c, const display_mode_t *m) {
    display_adapter_t *a = h->adapter;
    display_fb_t nfb[2];
    int want = use_flips(h) ? 2 : 1, got = 0, r = 0;
    memset(nfb, 0, sizeof nfb);
    if (a == fwa) {
        if (m->w != (int)fwfb.w || m->h != (int)fwfb.h) return E_NOTSUP;
        fb_from_bootinfo(&nfb[0]);
        got = 1;
    } else {
        for (; got < want; got++) {
            if (display_test_fail(a, "fb")) r = E_NOMEM;
            else r = a->ops->fb_alloc ? a->ops->fb_alloc(a, m->w, m->h, &nfb[got]) : E_NOTSUP;
            if (r) break;
        }
        if (r && got == 0) return r;
        r = 0;
        /* start black (the desktop is drawn over it at once): by the device if it can */
        int filled = 0;
        if (accel_has(a, ACCEL_FILL)) {
            accel_drain(a);
            int ok = accel_has(a, ACCEL_FILL), queued = 0;
            for (int i = 0; i < got && ok; i++) {
                if (!accel_fb_ok(&nfb[i]) || a->accel_ops->fill(a, &nfb[i], 0, 0, nfb[i].w, nfb[i].h, 0)) ok = 0;
                else queued++;
            }
            u32 f = queued ? a->accel_ops->submit(a) : 0;
            if (f) {
                accel_note(a, f);
                if (!accel_wait(a, f) && ok) { filled = 1; a->accel_fills += (u32)got; }
            } else if (queued) {
                accel_fail(a, "a fill could not be sent");
            }
        }
        for (int i = 0; i < got && !filled; i++)
            if (nfb[i].px) for (int y = 0; y < nfb[i].h; y++) memset((u8 *)nfb[i].px + (u64)y * nfb[i].pitch, 0, (size_t)nfb[i].w * (nfb[i].bpp / 8));
    }
    /* an unfinished flip on the old buffers: wait for it (bounded) */
    for (u64 end = time_ms() + 100; h->flip_pending && time_ms() < end;) {
        if (a->ops->poll) a->ops->poll(a);
        cpu_relax();
    }
    h->flip_pending = 0;
    r = a->ops->set_mode(h, c, m, &nfb[0]);
    if (!r && display_test_fail(a, "setmode")) r = E_IO;
    if (r) {
        for (int i = 0; i < got; i++) if (a->ops->fb_free && a != fwa) a->ops->fb_free(a, &nfb[i]);
        return r;
    }
    head_free_fbs(h);
    memcpy(h->fb, nfb, sizeof nfb);
    h->nfb = got;
    h->front = 0;
    h->mode = *m;
    h->conn = c;
    c->head = h;
    enable_wc(&h->fb[0]);
    if (got == 2) enable_wc(&h->fb[1]);
    /* everything is stale in the new buffers */
    drect_t all = { 0, 0, m->w, m->h };
    if (got == 2) { for (int b = 0; b < 2; b++) { h->stale_n[b] = 0; rect_add((drect_t *)h->stale[b], &h->stale_n[b], 8, all); } }
    else { h->dirty_n = 0; copy_rect(h, &h->fb[0], all); if (a->ops->flush) rect_add((drect_t *)h->dirty, &h->dirty_n, 8, all); }
    char d[128];
    mode_describe(m, d, sizeof d);
    kprintf("display: %s: %s on head %d (%s, %d buffer%s)\n", c->name, d, h->index, a->name, got, got > 1 ? "s, page flips" : "");
    return 0;
}

static void head_off(display_head_t *h) {
    display_adapter_t *a = h->adapter;
    if (!h->conn) return;
    if (h->cursor_on && a->ops->cursor_move) a->ops->cursor_move(h, 0, 0, 0);
    h->cursor_on = 0;
    if (a != fwa) a->ops->set_mode(h, h->conn, NULL, NULL);
    kprintf("display: %s: off\n", h->conn->name);
    h->conn->head = NULL;
    h->conn = NULL;
    head_free_fbs(h);
}

/* apply c (already checked); 0 or E_* (partly applied: the caller goes back) */
static int apply_raw(const display_conf_t *cf) {
    display_conf_t c = *cf;
    u32 taken[DISPLAY_MAX_ADAPTERS] = { 0 };
    display_head_t *assign[ARRAY_SIZE(c.out)];
    const display_mode_t *mode[ARRAY_SIZE(c.out)];
    rect_t place[ARRAY_SIZE(c.out)];
    int nplace = 0, pidx = -1, idx[ARRAY_SIZE(c.out)];
    /* the primary first, so that it keeps its head */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < c.n; i++) {
            display_conf_out_t *o = &c.out[i];
            if ((pass == 0) != (o->conn == c.primary)) continue;
            assign[i] = NULL;
            mode[i] = NULL;
            if (!o->attach || o->conn->status == CONN_DISCONNECTED) continue;
            const display_mode_t *m = display_conf_mode(o);
            if (!m) { kprintf("display: %s: no mode %dx%d\n", o->conn->name, o->w, o->h); continue; }
            display_head_t *h = pick_head(o->conn, taken);
            if (!h) { kprintf("display: %s: no free head\n", o->conn->name); continue; }
            taken[o->conn->adapter - adapter_store] |= 1u << h->index;
            assign[i] = h;
            mode[i] = m;
        }
    for (int i = 0; i < c.n; i++)
        if (assign[i] && nplace < LAYOUT_MAX) {
            if (c.out[i].conn == c.primary) pidx = nplace;
            idx[nplace] = i;
            place[nplace++] = mkrect(c.out[i].x, c.out[i].y, mode[i]->w, mode[i]->h);
        }
    if (!nplace) return E_INVAL;
    if (pidx < 0) { pidx = 0; c.primary = c.out[idx[0]].conn; }
    layout_normalize(place, nplace, pidx, NULL, NULL);
    /* heads that go off or change connector: off first (bandwidth, routing) */
    for (int i = 0; i < nadapters; i++) {
        display_adapter_t *a = &adapter_store[i];
        if (a->state != ADAPTER_ACTIVE) continue;
        for (int k = 0; k < a->nheads; k++) {
            display_head_t *h = &a->heads[k];
            if (!h->conn) continue;
            int keep = 0;
            for (int j = 0; j < c.n; j++) if (assign[j] == h && c.out[j].conn == h->conn) keep = 1;
            if (!keep) head_off(h);
        }
    }
    int err = 0;
    for (int p = 0; p < nplace; p++) {
        int i = idx[p];
        display_head_t *h = assign[i];
        display_connector_t *conn = c.out[i].conn;
        const display_mode_t *m = mode[i];
        h->x = place[p].x;
        h->y = place[p].y;
        if (h->conn == conn && same_mode(&h->mode, m) && h->nfb) continue;
        int r = head_set(h, conn, m);
        if (r) {
            kprintf("display: %s: %dx%d at %d Hz failed (%s)\n", conn->name, m->w, m->h, m->hz, strerror(r));
            err = r;
            if (h->conn == conn) head_off(h);
        }
    }
    primary = c.primary && c.primary->head ? c.primary : NULL;
    if (!primary)
        for (int p = 0; p < nplace && !primary; p++) if (c.out[idx[p]].conn->head) primary = c.out[idx[p]].conn;
    if (!primary) return err ? err : E_IO;
    bootinfo_from_fb(&primary->head->fb[primary->head->front]);
    cursor_all();
    if (cur_set) display_cursor_move(cur_x, cur_y, cur_vis);
    return err;
}

/* the firmware frame buffer as the only output (after a native adapter failed) */
static void firmware_back(void) {
    if (!fwa) return;
    display_head_t *h = &fwa->heads[0];
    display_connector_t *c = &fwa->conns[0];
    fwa->state = ADAPTER_ACTIVE;
    if (!h->conn) {
        int m = modes_find(c->modes, c->nmodes, (int)fwfb.w, (int)fwfb.h);
        h->x = h->y = 0;
        if (m >= 0) head_set(h, c, &c->modes[m]);
    }
    primary = c;
    bootinfo_from_fb(&h->fb[0]);
    number_connectors();
    cursor_all();
}

/* a native adapter failed: give its device back, carry on without it */
static void adapter_fail(display_adapter_t *a, const char *why_in) {
    char why[sizeof a->why];
    strlcpy(why, why_in, sizeof why);
    for (int k = 0; k < a->nheads; k++) {
        display_head_t *h = &a->heads[k];
        if (h->conn) { h->conn->head = NULL; h->conn = NULL; }
        head_free_fbs(h);
    }
    if (a->ops->restore) a->ops->restore(a);       /* (a driver's acceleration stops with it) */
    if (a->accel) { a->accel = 0; strlcpy(a->accel_why, "the driver was given up", sizeof a->accel_why); }
    a->state = ADAPTER_FAILED;
    strlcpy(a->why, why, sizeof a->why);
    kprintf("display: %s (%s) failed: %s; the firmware frame buffer is used instead\n", a->name, a->driver, why);
    int other = 0;
    for (int i = 0; i < nadapters; i++)
        if (&adapter_store[i] != fwa && adapter_store[i].state == ADAPTER_ACTIVE) other = 1;
    if (primary && primary->adapter == a) primary = NULL;
    if (a->owns_fw_fb || !other || !primary) firmware_back();
}

static void log_outputs(void);
int display_set_conf(const display_conf_t *c) {
    display_conf_t old;
    display_get_conf(&old);
    int r = apply_raw(c);
    if (!r && display_changed_hook && started) r = display_changed_hook();
    if (!r) { number_connectors(); log_outputs(); return 0; }
    kprintf("display: the new configuration failed (%s): going back\n", strerror(r));
    int r2 = apply_raw(&old);
    if (r2) {
        /* even the old one does not work any more: the firmware frame buffer */
        for (int i = 0; i < nadapters; i++)
            if (&adapter_store[i] != fwa && adapter_store[i].state == ADAPTER_ACTIVE) adapter_fail(&adapter_store[i], "mode set failed");
    }
    number_connectors();
    if (display_changed_hook && started) display_changed_hook();
    return r;
}

int display_save_conf(const display_conf_t *c) {
    char k[SETTINGS_KEY_MAX];
    for (int i = 0; i < c->n; i++) {
        const display_conf_out_t *o = &c->out[i];
        conn_key(o->conn, "attach", k, sizeof k); settings_set_int(k, o->attach ? 1 : 0);
        if (o->w && o->h) {
            conn_key(o->conn, "width", k, sizeof k); settings_set_int(k, o->w);
            conn_key(o->conn, "height", k, sizeof k); settings_set_int(k, o->h);
            /* the refresh rate, where the adapter offers a choice */
            conn_key(o->conn, "hz", k, sizeof k);
            if (o->conn->adapter->rates && o->hz > 0) settings_set_int(k, o->hz);
            else settings_set(k, NULL);
        }
        display_connector_t *conn = o->conn;
        if (conn->head) {
            conn_key(o->conn, "x", k, sizeof k); settings_set_int(k, conn->head->x);
            conn_key(o->conn, "y", k, sizeof k); settings_set_int(k, conn->head->y);
        } else {
            conn_key(o->conn, "x", k, sizeof k); settings_set_int(k, o->x);
            conn_key(o->conn, "y", k, sizeof k); settings_set_int(k, o->y);
        }
        if (o->conn == c->primary && o->w && o->h) {
            /* the loaders choose the firmware's mode from these */
            settings_set_int("display.width", o->w);
            settings_set_int("display.height", o->h);
        }
    }
    if (c->primary) settings_set("display.primary", c->primary->name);
    return settings_save();
}

/* the mode saved for a connector (display.<connector>.width, height and,
 * where the adapter offers a choice, hz); -1 if none is saved or its size is
 * not offered. A saved refresh rate that is not offered any more (another
 * monitor, a slower link) gives modes_find's rate at that size: the
 * monitor's preferred one, else the highest; BOOT.LOG says so. */
static int rate_fell;                          /* (display_start saves BOOT.LOG then) */
static int saved_mode(display_connector_t *c) {
    int w = conn_get(c, "width", 0), h = conn_get(c, "height", 0), fell = 0;
    if (!w || !h) return -1;
    int hz = c->adapter->rates ? conn_get(c, "hz", 0) : 0;
    int m = modes_saved(c->modes, c->nmodes, w, h, hz, &fell);
    if (m >= 0 && fell) {
        int p = modes_preferred(c->modes, c->nmodes);
        rate_fell = 1;
        kprintf("display: %s: %d Hz at %dx%d is not offered any more: %d Hz instead (%s)\n", c->name, hz, w, h, c->modes[m].hz,
                p >= 0 && c->modes[p].hz == c->modes[m].hz ? "the monitor's preferred rate" : "the highest rate at that size");
    }
    return m;
}

/* the configuration an adapter starts with: SETTINGS.INI, else defaults */
static void conf_for_start(display_conf_t *cf, display_adapter_t *a, int place_x) {
    for (int k = 0; k < a->nconns && cf->n < (int)ARRAY_SIZE(cf->out); k++) {
        display_connector_t *c = &a->conns[k];
        display_conf_out_t *o = &cf->out[cf->n++];
        memset(o, 0, sizeof *o);
        o->conn = c;
        o->attach = c->status != CONN_DISCONNECTED && c->nmodes > 0 && conn_get(c, "attach", 1);
        int w, h, m = saved_mode(c);
        const char *pn = settings_get("display.primary", NULL);
        int maybe_primary = (pn && !strcasecmp(pn, c->name)) || (!pn && a->owns_fw_fb && k == 0);
        if (m < 0 && maybe_primary) {
            /* the older keys (and the loaders): display.width / height */
            w = settings_get_int("display.width", 0); h = settings_get_int("display.height", 0);
            if (w && h) m = modes_find(c->modes, c->nmodes, w, h);
        }
        /* the mode the loader chose (its policy: a setting, the EDID it got,
         * its default): no switch at boot */
        if (m < 0 && a->owns_fw_fb && maybe_primary)
            m = modes_find(c->modes, c->nmodes, (int)fwfb.w, (int)fwfb.h);
        if (m < 0) m = modes_default(c->modes, c->nmodes, 2560, 1600);
        if (m >= 0) { o->w = c->modes[m].w; o->h = c->modes[m].h; o->hz = c->modes[m].hz; }
        else o->attach = 0;
        o->x = conn_has(c, "x") ? conn_get(c, "x", 0) : place_x;
        o->y = conn_get(c, "y", 0);
        if (o->attach) place_x += o->w;
        if (maybe_primary && o->attach && !cf->primary) cf->primary = c;
    }
}

static void log_connectors(display_adapter_t *a) {
    for (int k = 0; k < a->nconns; k++) {
        display_connector_t *c = &a->conns[k];
        char mon[64];
        edid_describe(&c->monitor, mon, sizeof mon);
        int p = modes_preferred(c->modes, c->nmodes);
        kprintf("display: %s %s: %s, %s%s, %d modes%s%s\n", a->driver, c->name,
                c->status == CONN_CONNECTED ? "connected" : c->status == CONN_MAYBE ? "maybe connected" : "disconnected",
                mon, c->edid_len ? "" : " (no EDID)", c->nmodes, p >= 0 ? ", preferred " : "", "");
        if (p >= 0) {
            char d[128];
            mode_describe(&c->modes[p], d, sizeof d);
            kprintf("display:   preferred %s\n", d);
        }
        /* the sizes with a choice of refresh rates */
        for (int i = 0, lines = 0; a->rates && i < c->nmodes && lines < 12; i++) {
            if (i && c->modes[i - 1].w == c->modes[i].w && c->modes[i - 1].h == c->modes[i].h) continue;
            int hz[16], n = modes_rates(c->modes, c->nmodes, c->modes[i].w, c->modes[i].h, hz, 16);
            if (n < 2) continue;
            char r[96] = "";
            for (int j = 0; j < n; j++) snprintf(r + strlen(r), sizeof r - strlen(r), "%s%d", j ? ", " : "", hz[j]);
            kprintf("display:   %dx%d at %s Hz\n", c->modes[i].w, c->modes[i].h, r);
            lines++;
        }
    }
}

static int adapter_start(display_adapter_t *a, display_conf_t *cf, int *place_x) {
    if (display_test_fail(a, "enable")) return E_IO;
    if (a->ops->enable) {
        int r = a->ops->enable(a);
        if (r) return r;
    }
    a->ops->detect(a);
    for (int k = 0; k < a->nconns; k++) {
        build_modes(&a->conns[k]);
        a->conns[k].head = NULL;
    }
    log_connectors(a);
    int before = cf->n;
    conf_for_start(cf, a, *place_x);
    for (int i = before; i < cf->n; i++) if (cf->out[i].attach) *place_x += cf->out[i].w;
    return 0;
}

static void log_outputs(void) {
    display_output_t outs[DISPLAY_MAX_OUTPUTS];
    int n = display_outputs(outs, DISPLAY_MAX_OUTPUTS), bw, bh;
    display_desktop_size(&bw, &bh);
    for (int i = 0; i < n; i++) {
        char mon[64], hz[16] = "";
        edid_describe(&outs[i].conn->monitor, mon, sizeof mon);
        /* the refresh rate where it is the monitor's (a driver that programs the timing) */
        if (outs[i].conn->adapter->rates && outs[i].head->mode.hz) snprintf(hz, sizeof hz, "@%d", outs[i].head->mode.hz);
        kprintf("display: monitor %d: %s (%s) %dx%d%s at %d,%d on %s%s\n", outs[i].conn->number, outs[i].conn->name, mon,
                outs[i].w, outs[i].h, hz, outs[i].x, outs[i].y, outs[i].conn->adapter->name, outs[i].primary ? ", primary" : "");
    }
    kprintf("display: desktop %dx%d on %d monitor%s\n", bw, bh, n, n == 1 ? "" : "s");
}

/* ---------------- the start guard ---------------- */
static void guard_key(const char *driver, const char *what, char *k, int n) { snprintf(k, (size_t)n, "display.%s.%s", driver, what); }

static void balloon(const char *title, const char *text) { if (shell_balloon) shell_balloon(title, text); }

/* a marker left by the last boot: the driver stays off */
static int guard_blocks(display_adapter_t *a) {
    char k[SETTINGS_KEY_MAX];
    guard_key(a->driver, "guard", k, sizeof k);
    const char *v = settings_get(k, NULL);
    if (!v || !v[0] || !strcmp(v, "0")) return 0;
    int fresh = !strcmp(v, "start");
    if (fresh) {
        settings_set(k, "blocked");
        settings_save();
    }
    a->state = ADAPTER_DISABLED;
    strlcpy(a->why, "turned off: its last start did not finish (Display Properties turns it on again)", sizeof a->why);
    kprintf("display: %s (%s) not started: %s (%s); the firmware frame buffer is used\n", a->name, a->driver,
            fresh ? "its start at the last boot did not finish" : "it stays off after a start that did not finish", k);
    char t[256];
    snprintf(t, sizeof t, "Halcyon's driver for the %s did not finish starting last time, so the basic display is used. "
             "You can turn it on again in Display Properties.", a->name);
    balloon("Display driver turned off", t);
    return 1;
}

int display_guard_begin(display_adapter_t *a) {
    if (!a->guarded || a->guard_armed) return 0;
    char k[SETTINGS_KEY_MAX];
    guard_key(a->driver, "guard", k, sizeof k);
    settings_set(k, "start");
    int r = settings_save();
    if (r) {
        settings_set(k, NULL);
        kprintf("display: %s: the start marker cannot be saved (%s)\n", a->driver, strerror(r));
        return r;
    }
    a->guard_armed = 1;
    kprintf("display: %s: start marker saved (%s=start)\n", a->driver, k);
    if (bootlog_save) bootlog_save("display driver starting");
    return 0;
}

void display_guard_checkpoint(display_adapter_t *a, const char *what) {
    if (a->guard_armed && bootlog_save) bootlog_save(what);
}

static void guard_clear(display_adapter_t *a, const char *why) {
    char k[SETTINGS_KEY_MAX];
    guard_key(a->driver, "guard", k, sizeof k);
    settings_set(k, NULL);
    settings_save();
    a->guard_armed = 0;
    a->guard_clear_ms = 0;
    a->guard_confirm = 0;
    kprintf("display: %s: start marker cleared (%s)\n", a->driver, why);
    if (bootlog_save) bootlog_save("display driver running");
}

/* "DP-1 2560x1440@165 fc;HDMI-1 1280x1024@60;": each output's mode and
 * refresh rate, and whether its head flips pages (f) and draws the
 * hardware cursor (c), so a new rate, or a driver that gains these, asks
 * once more */
static void guard_signature(display_adapter_t *a, char *out, int n) {
    out[0] = 0;
    for (int k = 0; k < a->nconns; k++) {
        display_connector_t *c = &a->conns[k];
        if (!c->head) continue;
        char one[48];
        u32 caps = c->head->caps;
        snprintf(one, sizeof one, "%s %dx%d@%d%s%s%s;", c->name, c->head->mode.w, c->head->mode.h, c->head->mode.hz,
                 caps & (HEAD_CAP_FLIP | HEAD_CAP_CURSOR) ? " " : "", caps & HEAD_CAP_FLIP ? "f" : "", caps & HEAD_CAP_CURSOR ? "c" : "");
        strlcat(out, one, (size_t)n);
    }
}

/* display_start is done: what the marker of an adapter that started needs */
static void guard_started(display_adapter_t *a) {
    if (!a->guard_armed) return;
    if (a->state != ADAPTER_ACTIVE) {
        kprintf("display: %s: the start marker stays (the driver failed after touching the device): it stays off at the next start\n",
                a->driver);
        if (bootlog_save) bootlog_save("display driver failed");
        return;
    }
    char k[SETTINGS_KEY_MAX], sig[SETTINGS_VAL_MAX];
    guard_key(a->driver, "confirmed", k, sizeof k);
    guard_signature(a, sig, sizeof sig);
    const char *v = settings_get(k, "");
    if (sig[0] && !strcmp(v, sig)) {
        a->guard_clear_ms = time_ms() + 10000;
        kprintf("display: %s: configuration confirmed before (%s)\n", a->driver, sig);
    } else {
        a->guard_confirm = 1;
        kprintf("display: %s: new configuration (%s): the picture will be confirmed\n", a->driver, sig);
    }
}

const char *display_guard_state(const char *driver) {
    for (int i = 0; i < nadapters; i++) {
        display_adapter_t *a = &adapter_store[i];
        if (a->guarded && !strcmp(a->driver, driver) && (a->state == ADAPTER_DISABLED || a->state == ADAPTER_FAILED) &&
            !strncmp(a->why, "turned off", 10))
            return a->why;
    }
    return NULL;
}

void display_guard_reenable(const char *driver) {
    char k[SETTINGS_KEY_MAX];
    guard_key(driver, "guard", k, sizeof k);
    settings_set(k, NULL);
    settings_save();
    kprintf("display: %s: turned on again (the next start uses it)\n", driver);
}

display_adapter_t *display_guard_confirm_needed(void) {
    for (int i = 0; i < nadapters; i++)
        if (adapter_store[i].guard_confirm && adapter_store[i].state == ADAPTER_ACTIVE) return &adapter_store[i];
    return NULL;
}

void display_guard_confirm(display_adapter_t *a, int yes) {
    if (!a || !a->guard_confirm) return;
    a->guard_confirm = 0;
    if (yes && a->state == ADAPTER_ACTIVE) {
        char k[SETTINGS_KEY_MAX], sig[SETTINGS_VAL_MAX];
        guard_key(a->driver, "confirmed", k, sizeof k);
        guard_signature(a, sig, sizeof sig);
        settings_set(k, sig);
        guard_clear(a, "the picture was confirmed");
        return;
    }
    kprintf("display: %s: the picture was not confirmed\n", a->driver);
    adapter_fail(a, "turned off: nobody confirmed the picture (Display Properties turns it on again)");
    number_connectors();
    if (display_changed_hook && started) display_changed_hook();
    log_outputs();
    if (bootlog_save) bootlog_save("display driver not confirmed");
    char t[256];
    snprintf(t, sizeof t, "Nobody confirmed the picture of Halcyon's driver for the %s, so the basic display is used again. "
             "You can turn the driver on again in Display Properties.", a->name);
    balloon("Display driver turned off", t);
}

/* ---------------- acceleration: its guard, state, speed test ---------------- */

int display_guard_settled(display_adapter_t *a) {
    return a->state == ADAPTER_ACTIVE && started && !a->guard_armed && !a->guard_confirm;
}

int display_accel_guard_begin(display_adapter_t *a) {
    char k[SETTINGS_KEY_MAX];
    accel_guard_key(a, k, sizeof k);
    const char *v = settings_get(k, NULL);
    if (v && v[0] && strcmp(v, "0")) {
        int fresh = !strcmp(v, "start");
        if (fresh) { settings_set(k, "blocked"); settings_save(); }
        snprintf(a->accel_why, sizeof a->accel_why, "blocked: its start did not finish before (remove %s from SETTINGS.INI)", k);
        kprintf("display: %s: acceleration not started: %s (%s=%s)\n", a->driver,
                fresh ? "its bring-up at the last boot did not finish" : "it stays off after a bring-up that did not finish", k,
                fresh ? "start" : v);
        return E_PERM;
    }
    settings_set(k, "start");
    int r = settings_save();
    if (r) {
        settings_set(k, NULL);
        snprintf(a->accel_why, sizeof a->accel_why, "its start marker cannot be saved (%s)", strerror(r));
        kprintf("display: %s: the acceleration marker cannot be saved (%s): not starting it\n", a->driver, strerror(r));
        return r;
    }
    a->accel_guard_armed = 1;
    kprintf("display: %s: acceleration marker saved (%s=start)\n", a->driver, k);
    if (bootlog_save) bootlog_save("display acceleration starting");
    return 0;
}

static void accel_guard_clear(display_adapter_t *a) {
    char k[SETTINGS_KEY_MAX];
    accel_guard_key(a, k, sizeof k);
    settings_set(k, NULL);
    settings_save();
    a->accel_guard_armed = 0;
    a->accel_guard_clear_ms = 0;
    kprintf("display: %s: acceleration marker cleared (it ran for 10 seconds)\n", a->driver);
    if (bootlog_save) bootlog_save("display acceleration running");
}

void display_accel_ready(display_adapter_t *a, u32 caps, const char *name) {
    a->accel = caps;
    a->accel_why[0] = 0;
    a->accel_src = NULL;
    a->accel_last = a->accel_seen = 0;
    strlcpy(a->accel_name, name ? name : "", sizeof a->accel_name);
    kprintf("display: %s: acceleration in use: %s (%s%s%s)\n", a->name, a->accel_name, caps & ACCEL_UPLOAD ? "uploads" : "",
            caps & ACCEL_COPY ? ", copies" : "", caps & ACCEL_FILL ? ", fills" : "");
    if (a->accel_guard_armed) a->accel_guard_clear_ms = time_ms() + 10000;
}

const char *display_accel_state(display_adapter_t *a, char *buf, int n) {
    if (!a->accel_ops) snprintf(buf, (size_t)n, "none");
    else if (a->accel)
        snprintf(buf, (size_t)n, "in use: %s; %u uploads, %u copies, %u fills, %u rectangles by the CPU", a->accel_name, a->accel_uploads,
                 a->accel_copies, a->accel_fills, a->accel_cpu);
    else snprintf(buf, (size_t)n, "off: %s", a->accel_why[0] ? a->accel_why : "not started");
    return buf;
}

int display_accel_bench(int test, u64 *bytes, u64 *us, u64 *cpu_us, u32 *passes, char *why, int whyn) {
    *bytes = *us = *cpu_us = 0;
    *passes = 0;
    display_head_t *h = primary ? primary->head : NULL;
    if (!h || !h->nfb) { snprintf(why, (size_t)whyn, "no monitor on the desktop"); return E_NOTSUP; }
    display_adapter_t *a = h->adapter;
    u32 need = test == 0 ? ACCEL_FILL : test == 1 ? ACCEL_UPLOAD : ACCEL_COPY;
    if (!accel_has(a, need)) {
        char st[128];
        snprintf(why, (size_t)whyn, "%s: %s", a->name, a->accel_ops ? display_accel_state(a, st, sizeof st) : "no acceleration");
        return E_NOTSUP;
    }
    display_fb_t *fb = &h->fb[h->front];
    if (!accel_fb_ok(fb) || (test == 1 && !accel_src(a))) { snprintf(why, (size_t)whyn, "the buffer or the desktop surface cannot be used"); return E_NOTSUP; }
    accel_drain(a);
    int w = fb->w, hh = fb->h, half = fb->h / 2;
    if (test == 1) { w = MIN(w, src_w - h->x); hh = MIN(hh, src_h - h->y); }
    u64 t0 = time_us(), now = t0, cpu = 0, b = 0;
    u32 n = 0;
    do {
        u64 c0 = time_us();
        int e;
        if (test == 0) { e = a->accel_ops->fill(a, fb, 0, 0, w, hh, (n & 1) ? 0x00404040 : 0x00303030); b += (u64)w * 4 * hh; }
        else if (test == 1) { e = a->accel_ops->upload(a, fb, 0, 0, w, hh, ((u64)h->y * (u64)src_stride + (u64)h->x) * 4, (u32)src_stride * 4); b += (u64)w * 4 * hh; }
        else {
            e = (n & 1) ? a->accel_ops->copy(a, fb, 0, 0, fb, 0, half, w, half) : a->accel_ops->copy(a, fb, 0, half, fb, 0, 0, w, half);
            b += (u64)w * 4 * half;
        }
        u32 f = e ? 0 : a->accel_ops->submit(a);
        cpu += time_us() - c0;
        if (!f) { snprintf(why, (size_t)whyn, "the operation could not be queued (%s)", strerror(e ? e : E_IO)); return E_IO; }
        accel_note(a, f);
        if (accel_wait(a, f)) { snprintf(why, (size_t)whyn, "it failed: %s", a->accel_why); return E_IO; }
        n++;
        now = time_us();
    } while (now - t0 < 200000 && n < 10000);
    *bytes = b;
    *us = now - t0;
    *cpu_us = cpu;
    *passes = n;
    return 0;
}

/* ---------------- acceleration: the software test hook ----------------
 * opt/halcyon/displayaccel = "<driver>" (or "<driver>:hang") gives an
 * adapter without acceleration of its own operations done by the CPU, but
 * asynchronously (2 ms after submit, when done() is asked), so the queueing,
 * the fences, the flush / flip ordering and the fallback run on the real
 * presentation path in QEMU. ":hang" stops completing after 50 fences. */
typedef struct soft_op {
    u8 kind;                                   /* 1 upload, 2 copy, 3 fill */
    display_fb_t dst, src;
    int x, y, sx, sy, w, h;
    u64 src_off;
    u32 src_pitch, color;
} soft_op_t;
static struct {
    soft_op_t q[128];
    int nq, nfl;                               /* queued; submitted (the first nfl of q are in flight) */
    u32 seq, done;
    u64 due_ms;
    int hang;
    const u8 *src;
    u64 src_bytes;
} soft;

static int soft_source(display_adapter_t *a, const void *px, u64 bytes) { (void)a; soft.src = px; soft.src_bytes = bytes; return 0; }
static int soft_rect_ok(const display_fb_t *fb, int x, int y, int w, int h) { return x >= 0 && y >= 0 && w > 0 && h > 0 && x + w <= fb->w && y + h <= fb->h; }
static int soft_queue(soft_op_t *o) {
    if (soft.nq >= (int)ARRAY_SIZE(soft.q)) return E_NOSPC;
    soft.q[soft.nq++] = *o;
    return 0;
}
static int soft_upload(display_adapter_t *a, display_fb_t *dst, int x, int y, int w, int h, u64 src_off, u32 src_pitch) {
    (void)a;
    if (!soft.src || !soft_rect_ok(dst, x, y, w, h) || src_off + (u64)(h - 1) * src_pitch + (u64)w * 4 > soft.src_bytes) return E_INVAL;
    soft_op_t o = { .kind = 1, .dst = *dst, .x = x, .y = y, .w = w, .h = h, .src_off = src_off, .src_pitch = src_pitch };
    return soft_queue(&o);
}
static int soft_copy(display_adapter_t *a, display_fb_t *dst, int dx, int dy, display_fb_t *src, int sx, int sy, int w, int h) {
    (void)a;
    if (!soft_rect_ok(dst, dx, dy, w, h) || !soft_rect_ok(src, sx, sy, w, h)) return E_INVAL;
    if (dst->px == src->px && dx < sx + w && sx < dx + w && dy < sy + h && sy < dy + h) return E_INVAL;   /* overlapping */
    soft_op_t o = { .kind = 2, .dst = *dst, .src = *src, .x = dx, .y = dy, .sx = sx, .sy = sy, .w = w, .h = h };
    return soft_queue(&o);
}
static int soft_fill(display_adapter_t *a, display_fb_t *dst, int x, int y, int w, int h, u32 c) {
    (void)a;
    if (!soft_rect_ok(dst, x, y, w, h)) return E_INVAL;
    soft_op_t o = { .kind = 3, .dst = *dst, .x = x, .y = y, .w = w, .h = h, .color = c };
    return soft_queue(&o);
}
static u32 soft_submit(display_adapter_t *a) {
    (void)a;
    if (soft.nq > soft.nfl) {
        soft.nfl = soft.nq;
        soft.seq++;
        soft.due_ms = time_ms() + 2;
    }
    return soft.seq;
}
static void soft_run(void) {
    for (int i = 0; i < soft.nfl; i++) {
        soft_op_t *o = &soft.q[i];
        for (int j = 0; j < o->h; j++) {
            u32 *d = (u32 *)((u8 *)o->dst.px + (u64)(o->y + j) * o->dst.pitch) + o->x;
            if (o->kind == 1) memcpy(d, soft.src + o->src_off + (u64)j * o->src_pitch, (size_t)o->w * 4);
            else if (o->kind == 2) memcpy(d, (u32 *)((u8 *)o->src.px + (u64)(o->sy + j) * o->src.pitch) + o->sx, (size_t)o->w * 4);
            else for (int k = 0; k < o->w; k++) d[k] = o->color;
        }
    }
    memmove(soft.q, soft.q + soft.nfl, (size_t)(soft.nq - soft.nfl) * sizeof soft.q[0]);
    soft.nq -= soft.nfl;
    soft.nfl = 0;
    soft.done = soft.seq;
}
static int soft_done(display_adapter_t *a, u32 fence) {
    (void)a;
    if ((i32)(soft.done - fence) >= 0) return 1;
    if (soft.hang && soft.seq > 50) return 0;            /* (the test of the fallback: never again) */
    if (time_ms() < soft.due_ms) return 0;
    soft_run();
    return (i32)(soft.done - fence) >= 0;
}
static void soft_stop(display_adapter_t *a, const char *why) {
    (void)a; (void)why;
    soft.nq = soft.nfl = 0;
    soft.done = soft.seq;
}
static const display_accel_ops_t soft_ops = { soft_source, soft_upload, soft_copy, soft_fill, soft_submit, soft_done, soft_stop };

static void soft_accel_install(display_adapter_t *a) {
    if (!accel_hook[0] || a->accel_ops) return;
    char name[32];
    strlcpy(name, accel_hook, sizeof name);
    char *colon = strchr(name, ':');
    if (colon) *colon = 0;
    if (strcasecmp(name, a->driver)) return;
    memset(&soft, 0, sizeof soft);
    soft.hang = colon && !strcmp(colon + 1, "hang");
    a->accel_ops = &soft_ops;
    a->accel_priv = &soft;
    display_accel_ready(a, ACCEL_UPLOAD | ACCEL_COPY | ACCEL_FILL, soft.hang ? "software (test hook, hangs after 50 fences)" : "software (test hook)");
}

/* ---------------- start ---------------- */
void display_start(void) {
    if (started) return;
    const char *dis = settings_get("display.disable", "");
    display_conf_t cf;
    memset(&cf, 0, sizeof cf);
    /* the firmware's output first (it stays unless a native adapter takes its device over) */
    display_get_conf(&cf);
    int place_x = 0;
    for (int i = 0; i < cf.n; i++) if (cf.out[i].attach) place_x = MAX(place_x, cf.out[i].x + cf.out[i].w);
    int started_any = 0, owner_started = 0;
    for (int i = 0; i < nadapters; i++) {
        display_adapter_t *a = &adapter_store[i];
        if (a == fwa || a->state != ADAPTER_PROBED) continue;
        if (name_in_list(dis, a->driver)) {
            a->state = ADAPTER_DISABLED;
            strlcpy(a->why, "disabled in SETTINGS.INI (display.disable)", sizeof a->why);
            kprintf("display: %s (%s) not started: %s\n", a->name, a->driver, a->why);
            continue;
        }
        int before = cf.n, px = place_x;
        if (guard_hook[0] && !strcasecmp(guard_hook, a->driver) && !a->guarded) {
            /* (the guard on a driver that does not use it itself: marker saved before enable) */
            a->guarded = 1;
            kprintf("display: %s: guarded (test hook)\n", a->driver);
            if (!guard_blocks(a) && display_guard_begin(a)) {
                a->state = ADAPTER_DISABLED;
                strlcpy(a->why, "the start marker cannot be saved", sizeof a->why);
                continue;
            }
            if (a->state == ADAPTER_DISABLED) continue;
        }
        if (a->guarded && !a->guard_armed && guard_blocks(a)) continue;
        int r = adapter_start(a, &cf, &px);
        if (r) {
            cf.n = before;
            adapter_fail(a, a->why[0] ? a->why : r == E_IO ? "the device did not start" : strerror(r));
            continue;
        }
        a->state = ADAPTER_ACTIVE;
        soft_accel_install(a);
        int attached = 0;
        for (int k = before; k < cf.n; k++) attached |= cf.out[k].attach;
        if (!attached) {
            kprintf("display: %s: no monitor connected\n", a->name);
            if (a->ops->restore) a->ops->restore(a);
            a->state = ADAPTER_FAILED;
            strlcpy(a->why, "no monitor connected", sizeof a->why);
            cf.n = before;
            continue;
        }
        place_x = px;
        started_any = 1;
        if (a->owns_fw_fb) owner_started = 1;
    }
    if (owner_started && fwa) {
        /* the firmware's frame buffer is that device's: it is not an output of its own */
        display_conf_out_t *o = conf_find(&cf, &fwa->conns[0]);
        if (o) o->attach = 0;
        if (cf.primary == &fwa->conns[0]) cf.primary = NULL;
    }
    if (!cf.primary || !conf_find(&cf, cf.primary) || !conf_find(&cf, cf.primary)->attach) {
        cf.primary = NULL;
        const char *pn = settings_get("display.primary", NULL);
        for (int i = 0; i < cf.n && !cf.primary; i++) if (cf.out[i].attach && pn && !strcasecmp(pn, cf.out[i].conn->name)) cf.primary = cf.out[i].conn;
        for (int i = 0; i < cf.n && !cf.primary; i++) if (cf.out[i].attach && cf.out[i].conn->adapter != fwa) cf.primary = cf.out[i].conn;
        for (int i = 0; i < cf.n && !cf.primary; i++) if (cf.out[i].attach) cf.primary = cf.out[i].conn;
    }
    int r = started_any ? apply_raw(&cf) : 0;
    if (r) {
        for (int i = 0; i < nadapters; i++) {
            display_adapter_t *a = &adapter_store[i];
            if (a == fwa || a->state != ADAPTER_ACTIVE) continue;
            int any = 0;
            for (int k = 0; k < a->nconns; k++) if (a->conns[k].head) any = 1;
            if (!any) adapter_fail(a, "no mode could be set");
        }
    }
    if (owner_started && fwa) {
        int owner_on = 0;
        for (int i = 0; i < nadapters; i++)
            if (adapter_store[i].owns_fw_fb && adapter_store[i].state == ADAPTER_ACTIVE) owner_on = 1;
        if (owner_on) {
            if (fwa->heads[0].conn) head_off(&fwa->heads[0]);
            fwa->state = ADAPTER_RETIRED;
        }
    }
    if (!primary) firmware_back();
    started = 1;
    number_connectors();
    if (fwa && fwa->state == ADAPTER_ACTIVE && primary == &fwa->conns[0] && !started_any)
        kprintf("display: %s, %d modes (changes apply at the next start)\n", fwa->name, fwa->conns[0].nmodes);
    log_outputs();
    for (int i = 0; i < nadapters; i++) guard_started(&adapter_store[i]);
    /* (BOOT.LOG was saved before the display started: the reason goes in now) */
    if (rate_fell && bootlog_save) bootlog_save("a saved refresh rate is not offered");
}

void display_apply_saved(void) { display_start(); }

/* a connector's status or preferred size changed (hotplug): follow it */
static void hotplug_reconfigure(display_adapter_t *a) {
    int was[DISPLAY_MAX_CONNECTORS], wpw[DISPLAY_MAX_CONNECTORS], wph[DISPLAY_MAX_CONNECTORS];
    for (int k = 0; k < a->nconns; k++) {
        was[k] = a->conns[k].status; wpw[k] = a->conns[k].pref_w; wph[k] = a->conns[k].pref_h;
    }
    u8 old_edid_sum[DISPLAY_MAX_CONNECTORS];
    for (int k = 0; k < a->nconns; k++) {
        u8 s = 0;
        for (int i = 0; i < a->conns[k].edid_len; i++) s = (u8)(s * 31 + a->conns[k].edid[i]);
        old_edid_sum[k] = s;
    }
    a->ops->detect(a);
    display_conf_t cf;
    display_get_conf(&cf);
    int changed = 0;
    rect_t bounds = mkrect(0, 0, 0, 0);
    for (int i = 0; i < cf.n; i++) if (cf.out[i].attach) bounds = rect_union(bounds, mkrect(cf.out[i].x, cf.out[i].y, cf.out[i].w, cf.out[i].h));
    for (int k = 0; k < a->nconns; k++) {
        display_connector_t *c = &a->conns[k];
        u8 s = 0;
        for (int i = 0; i < c->edid_len; i++) s = (u8)(s * 31 + c->edid[i]);
        int pref_changed = c->pref_w != wpw[k] || c->pref_h != wph[k] || s != old_edid_sum[k];
        if (c->status == was[k] && !pref_changed) continue;
        build_modes(c);
        display_conf_out_t *o = conf_find(&cf, c);
        if (!o) continue;
        changed = 1;
        if (c->status == CONN_DISCONNECTED) {
            kprintf("display: %s: monitor disconnected\n", c->name);
            o->attach = 0;
            continue;
        }
        /* a saved mode (and rate) is kept; otherwise the monitor's (the VM window's) size is followed */
        int m = pref_changed ? -1 : saved_mode(c);
        if (m < 0) m = modes_default(c->modes, c->nmodes, 2560, 1600);
        if (m < 0) { o->attach = 0; continue; }
        if (was[k] == CONN_DISCONNECTED || !c->head) {
            o->attach = conn_get(c, "attach", 1);
            o->x = conn_has(c, "x") ? conn_get(c, "x", 0) : bounds.x + bounds.w;
            o->y = conn_get(c, "y", 0);
            kprintf("display: %s: monitor connected (%dx%d)\n", c->name, c->modes[m].w, c->modes[m].h);
        } else {
            kprintf("display: %s: monitor changed (%dx%d)\n", c->name, c->modes[m].w, c->modes[m].h);
        }
        o->w = c->modes[m].w;
        o->h = c->modes[m].h;
        o->hz = c->modes[m].hz;
    }
    if (!changed) return;
    if (cf.primary && conf_find(&cf, cf.primary) && !conf_find(&cf, cf.primary)->attach) cf.primary = NULL;
    for (int i = 0; i < cf.n && !cf.primary; i++) if (cf.out[i].attach) cf.primary = cf.out[i].conn;
    if (!cf.primary) { kprintf("display: no monitor left connected: the desktop stays as it is\n"); return; }
    int r = display_set_conf(&cf);
    int bw, bh;
    display_desktop_size(&bw, &bh);
    kprintf("display: hotplug: %s (desktop %dx%d)\n", r ? strerror(r) : "reconfigured", bw, bh);
}

int display_service(void) {
    int work = 0;
    for (int i = 0; i < nadapters; i++) {
        display_adapter_t *a = &adapter_store[i];
        if (a->state != ADAPTER_ACTIVE) continue;
        if (a->guard_clear_ms && time_ms() >= a->guard_clear_ms) guard_clear(a, "running for 10 seconds with a confirmed configuration");
        if (a->accel_guard_clear_ms && time_ms() >= a->accel_guard_clear_ms) accel_guard_clear(a);
        u32 before = 0;
        for (int k = 0; k < a->nheads; k++) before += a->heads[k].flips;
        if (a->ops->poll) a->ops->poll(a);
        u32 after = 0;
        for (int k = 0; k < a->nheads; k++) after += a->heads[k].flips;
        if (after != before) work = 1;
        if (a->hotplug && started) {
            a->hotplug = 0;
            hotplug_reconfigure(a);
            work = 1;
        }
    }
    display_commit();
    /* a flip in flight: be back for its vertical blank */
    for (int i = 0; i < nadapters; i++) {
        display_adapter_t *a = &adapter_store[i];
        if (a->state != ADAPTER_ACTIVE) continue;
        for (int k = 0; k < a->nheads; k++) {
            display_head_t *h = &a->heads[k];
            if (!h->flip_pending) continue;
            if (time_ms() - h->flip_ms > 250) {
                /* the vertical blank never came: show the buffer anyway */
                kprintf("display: %s: flip not completed after 250 ms, single buffering\n", h->conn ? h->conn->name : "?");
                h->caps &= ~HEAD_CAP_FLIP;
                display_flip_done(h);
            }
        }
    }
    return work;
}

/* ---------------- the firmware frame buffer adapter ---------------- */
static void fw_detect(display_adapter_t *a) {
    display_connector_t *c = &a->conns[0];
    display_connector_update(c, CONN_CONNECTED, g_bootinfo.edid_size ? g_bootinfo.edid : NULL, (int)g_bootinfo.edid_size, 0, 0);
}
static int fw_set_mode(display_head_t *h, display_connector_t *c, const display_mode_t *m, display_fb_t *fb) {
    if (!m) return 0;
    return m->w == (int)fwfb.w && m->h == (int)fwfb.h ? 0 : E_NOTSUP;
}
static const display_ops_t fw_ops = { .detect = fw_detect, .set_mode = fw_set_mode };

static pci_dev_t *fw_fb_device(void) {
    for (int i = 0; i < pci_count(); i++) {
        pci_dev_t *d = pci_get(i);
        if (d->class_code == 0x03 && fb_in_bars(d, fwfb.addr)) return d;
    }
    return NULL;
}

void display_init(void) {
    fwfb.addr = g_bootinfo.fb_addr; fwfb.w = g_bootinfo.fb_width; fwfb.h = g_bootinfo.fb_height;
    fwfb.pitch = g_bootinfo.fb_pitch; fwfb.bpp = g_bootinfo.fb_bpp;
    fwfb.rp = g_bootinfo.fb_red_pos; fwfb.gp = g_bootinfo.fb_green_pos; fwfb.bp = g_bootinfo.fb_blue_pos;
    if (!qemu_fw_file("opt/halcyon/displayfail", fail_hook, sizeof fail_hook)) fail_hook[0] = 0;
    if (!qemu_fw_file("opt/halcyon/displayguard", guard_hook, sizeof guard_hook)) guard_hook[0] = 0;
    if (!qemu_fw_file("opt/halcyon/displayaccel", accel_hook, sizeof accel_hook)) accel_hook[0] = 0;
    fwa = display_register("firmware", "Firmware frame buffer (VESA/GOP)", &fw_ops, NULL, fw_fb_device());
    display_head_t *h = display_add_head(fwa, 0, 0);
    display_connector_t *c = display_add_connector(fwa, "Default", CONN_FIRMWARE, 1);
    fw_detect(fwa);
    build_modes(c);
    fwa->state = ADAPTER_ACTIVE;
    int m = modes_find(c->modes, c->nmodes, (int)fwfb.w, (int)fwfb.h);
    if (m >= 0) head_set(h, c, &c->modes[m]);
    primary = c;
    number_connectors();
    if (g_bootinfo.boot_flags & (BOOTF_SAFE_VIDEO | BOOTF_NO_DISPLAY)) {
        kprintf("display: native display drivers off (boot option)\n");
        init_done = 1;
        return;
    }
    if (bochs_display_probe) bochs_display_probe();
    if (virtio_gpu_probe) virtio_gpu_probe();
    if (nv_display_probe) nv_display_probe();
    for (int i = 0; i < nadapters; i++) {
        display_adapter_t *a = &adapter_store[i];
        if (a == fwa) continue;
        if (fb_in_bars(a->pci, fwfb.addr)) a->owns_fw_fb = 1;
        kprintf("display: found %s (%s)%s\n", a->name, a->driver, a->owns_fw_fb ? ", shows the firmware frame buffer" : "");
    }
    init_done = 1;
}

/* ---------------- after a fatal error ---------------- */
void display_panic(int phase) {
    if (!primary || !primary->head || !primary->head->nfb) return;
    display_head_t *h = primary->head;
    display_fb_t *fb = &h->fb[h->front];
    if (phase == 0) { bootinfo_from_fb(fb); return; }
    if (h->adapter->ops->panic_flush) h->adapter->ops->panic_flush(h, fb);
}

/* ---------------- the primary monitor (older callers) ---------------- */
static display_connector_t *pc(void) { return primary ? primary : fwa ? &fwa->conns[0] : NULL; }
const char *display_adapter(void) { return pc() ? pc()->adapter->name : "none"; }
int display_live(void) { return pc() && pc()->adapter->live; }
int display_mode_count(void) { return pc() ? pc()->nmodes : 0; }
display_mode_t display_mode(int i) {
    display_mode_t z;
    memset(&z, 0, sizeof z);
    return pc() && i >= 0 && i < pc()->nmodes ? pc()->modes[i] : z;
}
int display_find(int w, int h) { return pc() ? modes_find(pc()->modes, pc()->nmodes, w, h) : -1; }
const edid_info_t *display_monitor(void) {
    static edid_info_t none;
    return pc() ? &pc()->monitor : &none;
}
int display_native(int *w, int *h) {
    const edid_info_t *m = display_monitor();
    if (!m->valid || !m->native_w || display_find(m->native_w, m->native_h) < 0) return 0;
    *w = m->native_w;
    *h = m->native_h;
    return 1;
}
int display_set_mode(int w, int h) {
    display_connector_t *c = pc();
    if (!c || !c->adapter->live) return E_NOTSUP;
    if (display_find(w, h) < 0) return E_INVAL;
    if (c->head && c->head->mode.w == w && c->head->mode.h == h) return 0;
    display_conf_t cf;
    display_get_conf(&cf);
    display_conf_out_t *o = conf_find(&cf, c);
    if (!o) return E_INVAL;
    o->w = w; o->h = h; o->hz = 0;
    int r = display_set_conf(&cf);
    if (!r) kprintf("display: mode %dx%d\n", w, h);
    return r;
}
int display_save_mode(int w, int h) {
    display_conf_t cf;
    display_get_conf(&cf);
    display_conf_out_t *o = pc() ? conf_find(&cf, pc()) : NULL;
    if (o) { o->w = w; o->h = h; o->hz = 0; }
    settings_set_int("display.width", w);
    settings_set_int("display.height", h);
    return o ? display_save_conf(&cf) : settings_save();
}
