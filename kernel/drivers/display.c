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
static int init_done;                          /* display_init finished (later registrations: nv_init) */
static int fb_in_bars(const pci_dev_t *d, u64 addr);

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

void display_set_source(const u32 *px, int w, int h, int stride) {
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

static void head_commit(display_head_t *h) {
    display_adapter_t *a = h->adapter;
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
    display_fb_t *fb = &h->fb[back];
    for (int i = 0; i < h->stale_n[back]; i++) {
        drect_t r = { h->stale[back][i].x, h->stale[back][i].y, h->stale[back][i].w, h->stale[back][i].h };
        copy_rect(h, fb, r);
        if (a->ops->flush) a->ops->flush(h, fb, r.x, r.y, r.w, r.h);
    }
    h->stale_n[back] = 0;
    h->frames++;
    h->flip_pending = 1;
    h->flip_ms = time_ms();
    if (a->ops->flip(h, fb)) {
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

void display_commit(void) {
    display_head_t *hs[DISPLAY_MAX_OUTPUTS];
    int n = output_heads(hs, DISPLAY_MAX_OUTPUTS);
    for (int i = 0; i < n; i++) head_commit(hs[i]);
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
                for (int k = 0; k < a->nheads; k++) pending |= a->heads[k].flip_pending;
            }
            if (!pending || time_ms() > end) break;
            cpu_relax();
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
                o->x = conn->head->x; o->y = conn->head->y;
            } else {
                int d = modes_default(conn->modes, conn->nmodes, 2560, 1600);
                if (d >= 0) { o->w = conn->modes[d].w; o->h = conn->modes[d].h; }
                o->x = conn_get(conn, "x", 100000); o->y = conn_get(conn, "y", 0);
            }
        }
    }
    c->primary = primary;
}

int display_conf_live(const display_conf_t *c) {
    for (int i = 0; i < c->n; i++) {
        const display_conf_out_t *o = &c->out[i];
        display_connector_t *conn = o->conn;
        int same = (conn->head != NULL) == (o->attach != 0) &&
                   (!o->attach || (conn->head->mode.w == o->w && conn->head->mode.h == o->h));
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
            /* start black (the desktop is drawn over it at once) */
            if (nfb[got].px) for (int y = 0; y < nfb[got].h; y++) memset((u8 *)nfb[got].px + (u64)y * nfb[got].pitch, 0, (size_t)nfb[got].w * (nfb[got].bpp / 8));
        }
        if (r && got == 0) return r;
        r = 0;
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
            int m = modes_find(o->conn->modes, o->conn->nmodes, o->w, o->h);
            if (m < 0) { kprintf("display: %s: no mode %dx%d\n", o->conn->name, o->w, o->h); continue; }
            display_head_t *h = pick_head(o->conn, taken);
            if (!h) { kprintf("display: %s: no free head\n", o->conn->name); continue; }
            taken[o->conn->adapter - adapter_store] |= 1u << h->index;
            assign[i] = h;
            mode[i] = &o->conn->modes[m];
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
        if (h->conn == conn && h->mode.w == m->w && h->mode.h == m->h && h->nfb) continue;
        int r = head_set(h, conn, m);
        if (r) {
            kprintf("display: %s: %dx%d failed (%s)\n", conn->name, m->w, m->h, strerror(r));
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
    if (a->ops->restore) a->ops->restore(a);
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

/* the configuration an adapter starts with: SETTINGS.INI, else defaults */
static void conf_for_start(display_conf_t *cf, display_adapter_t *a, int place_x) {
    for (int k = 0; k < a->nconns && cf->n < (int)ARRAY_SIZE(cf->out); k++) {
        display_connector_t *c = &a->conns[k];
        display_conf_out_t *o = &cf->out[cf->n++];
        memset(o, 0, sizeof *o);
        o->conn = c;
        o->attach = c->status != CONN_DISCONNECTED && c->nmodes > 0 && conn_get(c, "attach", 1);
        int w = conn_get(c, "width", 0), h = conn_get(c, "height", 0), m = -1;
        if (w && h) m = modes_find(c->modes, c->nmodes, w, h);
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
        if (m >= 0) { o->w = c->modes[m].w; o->h = c->modes[m].h; }
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
        char mon[64];
        edid_describe(&outs[i].conn->monitor, mon, sizeof mon);
        kprintf("display: monitor %d: %s (%s) %dx%d at %d,%d on %s%s\n", outs[i].conn->number, outs[i].conn->name, mon,
                outs[i].w, outs[i].h, outs[i].x, outs[i].y, outs[i].conn->adapter->name, outs[i].primary ? ", primary" : "");
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

/* "DP-1 1920x1080 fc;HDMI-1 1280x1024;": each output's mode, and whether its
 * head flips pages (f) and draws the hardware cursor (c), so a driver that
 * gains these asks once more */
static void guard_signature(display_adapter_t *a, char *out, int n) {
    out[0] = 0;
    for (int k = 0; k < a->nconns; k++) {
        display_connector_t *c = &a->conns[k];
        if (!c->head) continue;
        char one[48];
        u32 caps = c->head->caps;
        snprintf(one, sizeof one, "%s %dx%d%s%s%s;", c->name, c->head->mode.w, c->head->mode.h,
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
        int w = conn_get(c, "width", 0), h = conn_get(c, "height", 0), m = -1;
        /* a saved mode is kept; otherwise the monitor's (the VM window's) size is followed */
        if (w && h && !pref_changed) m = modes_find(c->modes, c->nmodes, w, h);
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
    o->w = w; o->h = h;
    int r = display_set_conf(&cf);
    if (!r) kprintf("display: mode %dx%d\n", w, h);
    return r;
}
int display_save_mode(int w, int h) {
    display_conf_t cf;
    display_get_conf(&cf);
    display_conf_out_t *o = pc() ? conf_find(&cf, pc()) : NULL;
    if (o) { o->w = w; o->h = h; }
    settings_set_int("display.width", w);
    settings_set_int("display.height", h);
    return o ? display_save_conf(&cf) : settings_save();
}
