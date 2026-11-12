/* Halcyon OS - screen savers (see docs/DESIGN-M8-SCREENSAVER-SETTINGS.md).
 *
 * The window manager hands every input event to saver_input() first, which
 * keeps the idle time. After screensaver.wait minutes without input (keys,
 * buttons, the wheel, or the pointer moving more than JITTER pixels) the
 * chosen screen saver, C:\PROGRAMS\<screensaver.file>, starts with "/s": a
 * black cover window goes up at once (it also is the blank screen if the
 * program cannot run), the pointer is hidden, and the program draws in a
 * WIN_FULLSCREEN window that the window manager keeps above everything
 * (WF_SAVER). The first key, click, wheel turn or real pointer movement ends
 * it: the saver's windows are taken down at once and its program is ended,
 * and that event (and the release of the key or buttons involved) never
 * reaches the windows underneath.
 *
 * It does not start while a program shows a full-screen window of its own
 * (a video) or keeps the computer awake (SYS_KEEP_AWAKE).
 *
 * Display Properties shows a saver in a preview box: saver_preview_open()
 * gives a handle, the program runs with "/p <handle>" and its WIN_PREVIEW
 * window is drawn inside the box (WF_EMBED). */
#include "wm.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/proc.h"
#include "core/settings.h"
#include "fs/vfs.h"

#define JITTER 6                 /* pointer movement that does not count as input (pixels) */
#define MOTION_GRACE_MS 800      /* after a start, motion alone does not end it */
#define DEFAULT_FILE "LOGO.SCR"
#define DEFAULT_WAIT 10          /* minutes */
#define MAX_AWAKE 16
#define MAX_PREVIEW 4

static struct {
    int active;
    u32 pid;
    window_t *cover;
    u64 start_ms;
    char file[64];
    int shown;                   /* the program's window was seen */
} ss;

static u64 last_input;
static u64 wait_ms;              /* 0: never */
static char cfg_file[64];
static int test_wait_s;          /* fw_cfg test hook: seconds (0 = off) */
static char test_file[64];

/* pointer position as the device reports it (relative mice: summed) */
static int px, py, ax, ay;
static u8 last_buttons, swallow_buttons, swallow_key;

static u32 awake[MAX_AWAKE];

typedef struct { int handle; window_t *host; rect_t r; u32 pid; } pslot_t;
static pslot_t previews[MAX_PREVIEW];

/* ---------------- settings ---------------- */
void saver_reload(void) {
    const char *f = settings_get("screensaver.file", DEFAULT_FILE);
    strlcpy(cfg_file, f, sizeof cfg_file);
    int m = settings_get_int("screensaver.wait", DEFAULT_WAIT);
    wait_ms = (u64)CLAMP(m, 1, 999) * 60000;
    if (!cfg_file[0]) wait_ms = 0;               /* (None) */
    if (test_wait_s) {
        wait_ms = (u64)test_wait_s * 1000;
        if (test_file[0]) strlcpy(cfg_file, test_file, sizeof cfg_file);
    }
}

void saver_init(void) {
    /* test hook: QEMU fw_cfg "opt/halcyon/screensaver" = "SECONDS[:FILE.SCR]" */
    char t[96] = "";
    /* (or "display" / "mouse": open Display Properties on the Screen Saver
     * tab, or Mouse Properties, for screenshots) */
    if (qemu_fw_file("opt/halcyon/screensaver", t, sizeof t) && !strcmp(t, "display")) app_display_saver();
    else if (!strcmp(t, "mouse")) mouse_dialog();
    else if (t[0]) {
        test_wait_s = CLAMP(atoi(t), 1, 3600);
        char *c = strchr(t, ':');
        if (c) strlcpy(test_file, c + 1, sizeof test_file);
        kprintf("screensaver: test hook: %s after %d s\n", test_file[0] ? test_file : "the chosen one", test_wait_s);
    }
    saver_reload();
    last_input = time_ms();
}

/* ---------------- the session ---------------- */
int saver_active(void) { return ss.active; }
int saver_is_saver_pid(u32 pid) { return ss.active && pid && pid == ss.pid; }

static void cover_paint(window_t *w, gc_t *g) { gfx_fill(g, mkrect(0, 0, w->client->w, w->client->h), HEX(0x000000)); }

void saver_start(const char *file) {
    if (ss.active) return;
    menu_close_all();
    memset(&ss, 0, sizeof ss);
    strlcpy(ss.file, file ? file : "", sizeof ss.file);
    ss.cover = wm_create("", -1, 0, 0, screen_w(), screen_h(), WF_FULLSCREEN | WF_TOPMOST | WF_NOSHADOW | WF_SAVER);
    if (ss.cover) ss.cover->on_paint = cover_paint;
    ss.active = 1;
    ss.start_ms = time_ms();
    swallow_buttons = 0;
    swallow_key = 0;
    ax = px; ay = py;
    if (ss.file[0]) {
        char path[VFS_MAX_PATH];
        snprintf(path, sizeof path, strchr(ss.file, ':') ? "%s" : "C:/PROGRAMS/%s", ss.file);
        int pid = proc_spawn(path, "/s", NULL, NULL);
        if (pid > 0) ss.pid = (u32)pid;
        kprintf("screensaver: started %s (%s)\n", ss.file, pid > 0 ? "running" : strerror(pid));
    } else {
        kprintf("screensaver: started (blank screen)\n");
    }
    wm_cursor_changed();
    gui_full_redraw();
}

void saver_stop(const char *why) {
    if (!ss.active) return;
    /* the saver's windows go now (its program may take a moment to end) */
    for (;;) {
        window_t *s = NULL;
        for (window_t *w = wm_first(); w; w = w->next) if (w->flags & WF_SAVER) { s = w; break; }
        if (!s) break;
        wm_destroy(s);
    }
    if (ss.pid && proc_alive(ss.pid)) proc_kill(ss.pid, NULL);
    ss.active = 0;
    ss.pid = 0;
    ss.cover = NULL;
    last_input = time_ms();
    kprintf("screensaver: stopped (%s)\n", why);
    gui_full_redraw();
}

/* a program in full screen (a video), or one keeping the computer awake */
static int suppressed(void) {
    for (window_t *w = wm_first(); w; w = w->next)
        if ((w->flags & WF_FULLSCREEN) && !(w->flags & WF_SAVER) && w->state != WS_MINIMIZED) return 1;
    for (int i = 0; i < MAX_AWAKE; i++) {
        if (!awake[i]) continue;
        if (proc_alive(awake[i])) return 1;
        awake[i] = 0;
    }
    return 0;
}

int saver_keep_awake(u32 pid, int on) {
    int free_i = -1;
    for (int i = 0; i < MAX_AWAKE; i++) {
        if (awake[i] == pid) { if (!on) awake[i] = 0; return 0; }
        if (!awake[i] || !proc_alive(awake[i])) free_i = i;
    }
    if (!on) return 0;
    if (free_i < 0) return E_NOSPC;
    awake[free_i] = pid;
    last_input = time_ms();
    return 0;
}

void saver_tick(void) {
    u64 now = time_ms();
    /* previews whose box went away */
    for (int i = 0; i < MAX_PREVIEW; i++)
        if (previews[i].handle && !wm_alive(previews[i].host)) saver_preview_close(previews[i].handle);
    if (ss.active) {
        if (!ss.shown) {
            for (window_t *w = wm_first(); w; w = w->next)
                if ((w->flags & WF_SAVER) && w != ss.cover) {
                    ss.shown = 1;
                    kprintf("screensaver: %s is showing (%dx%d)\n", ss.file, w->frame.w, w->frame.h);
                }
        }
        if (ss.pid && !proc_alive(ss.pid)) ss.pid = 0;   /* the cover stays: a blank screen until input */
        return;
    }
    if (!wait_ms) return;
    if (suppressed()) { last_input = now; return; }
    if (now - last_input >= wait_ms) saver_start(cfg_file);
}

/* every input event, before the window manager sees it */
int saver_input(const input_event_t *ev) {
    u64 now = time_ms();
    if (ev->type == EV_KEY) {
        if (ss.active) {
            if (ev->pressed) { swallow_key = ev->key; saver_stop("key"); }
            return 1;                        /* releases too: none reach a window */
        }
        if (!ev->pressed && swallow_key && ev->key == swallow_key) { swallow_key = 0; return 1; }
        last_input = now;
        return 0;
    }
    /* the pointer: movement beyond the jitter is input */
    if (ev->absolute) { px = ev->x * screen_w() / 32768; py = ev->y * screen_h() / 32768; }
    else { px += ev->dx; py += ev->dy; }
    int moved = px - ax > JITTER || ax - px > JITTER || py - ay > JITTER || ay - py > JITTER;
    if (moved) { ax = px; ay = py; }
    u8 pressed = (u8)(ev->buttons & ~last_buttons);
    last_buttons = ev->buttons;
    if (ss.active) {
        if (pressed || ev->dz || (moved && now - ss.start_ms >= MOTION_GRACE_MS)) {
            swallow_buttons = ev->buttons;   /* and their release */
            saver_stop(pressed ? "click" : ev->dz ? "wheel" : "mouse moved");
        }
        return 1;
    }
    if (swallow_buttons) {
        if (!(ev->buttons & swallow_buttons)) swallow_buttons = 0;
        return 1;
    }
    if (moved || pressed || ev->dz || ev->buttons) last_input = now;
    return 0;
}

/* ---------------- previews ---------------- */
int saver_preview_open(window_t *host, rect_t r) {
    for (int i = 0; i < MAX_PREVIEW; i++) {
        if (previews[i].handle) continue;
        int h = (int)(random_u32() & 0x3FFFFFFF) | 0x10000;
        previews[i] = (pslot_t){ h, host, r, 0 };
        return h;
    }
    return E_NOSPC;
}

void saver_preview_set_pid(int handle, u32 pid) {
    for (int i = 0; i < MAX_PREVIEW; i++) if (previews[i].handle == handle) previews[i].pid = pid;
}

void saver_preview_close(int handle) {
    for (int i = 0; i < MAX_PREVIEW; i++) {
        pslot_t *s = &previews[i];
        if (s->handle != handle) continue;
        if (wm_alive(s->host)) {
            for (;;) {
                window_t *e = NULL;
                for (window_t *w = wm_first(); w; w = w->next) if ((w->flags & WF_EMBED) && w->owner == s->host) { e = w; break; }
                if (!e) break;
                wm_destroy(e);
            }
            wm_invalidate(s->host);
        }
        if (s->pid && proc_alive(s->pid)) proc_kill(s->pid, NULL);
        memset(s, 0, sizeof *s);
    }
}

/* for SYS_WIN_CREATE with WIN_PREVIEW: only the program started for the box */
window_t *saver_preview_host(int handle, u32 pid, rect_t *r) {
    for (int i = 0; i < MAX_PREVIEW; i++) {
        pslot_t *s = &previews[i];
        if (s->handle && s->handle == handle && s->pid == pid && wm_alive(s->host)) { *r = s->r; return s->host; }
    }
    return NULL;
}

/* ---------------- the installed screen savers ----------------
 * A .SCR file is an ordinary program. Its display name is an ELF note
 * (owner "Halcyon", type 1, the UTF-8 name: userland/savers/saver.h); a file
 * without one shows its file name. */
#define NT_HALCYON_NAME 1

static int note_name(const u8 *d, u32 n, char *out, int outsz) {
    u32 o = 0;
    while (o + 12 <= n) {
        u32 namesz = d[o] | d[o + 1] << 8 | d[o + 2] << 16 | (u32)d[o + 3] << 24;
        u32 descsz = d[o + 4] | d[o + 5] << 8 | d[o + 6] << 16 | (u32)d[o + 7] << 24;
        u32 type = d[o + 8] | d[o + 9] << 8 | d[o + 10] << 16 | (u32)d[o + 11] << 24;
        u32 np = (namesz + 3) & ~3u, dp = (descsz + 3) & ~3u;
        if (namesz > 64 || descsz > 256 || o + 12 + np + dp > n) return 0;
        const u8 *nm = d + o + 12, *desc = nm + np;
        if (type == NT_HALCYON_NAME && namesz == 8 && !memcmp(nm, "Halcyon", 8) && descsz) {
            int k = 0;
            for (u32 i = 0; i < descsz && desc[i] && k < outsz - 1; i++) out[k++] = (char)desc[i];
            out[k] = 0;
            return k > 0;
        }
        o += 12 + np + dp;
    }
    return 0;
}

static u64 rd64(const u8 *p) { u64 v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }
static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

/* the program's display name from its PT_NOTE segments */
static int scr_name(const char *path, char *out, int outsz) {
    vfs_file_t *f;
    if (vfs_open(path, VFS_O_READ, &f)) return 0;
    u8 eh[64];
    int ok = 0;
    if (vfs_read(f, eh, 64) == 64 && !memcmp(eh, "\x7F" "ELF", 4) && eh[4] == 2) {
        u64 phoff = rd64(eh + 32);
        int phnum = eh[56] | eh[57] << 8, phent = eh[54] | eh[55] << 8;
        for (int i = 0; i < phnum && i < 32 && phent == 56 && !ok; i++) {
            u8 ph[56];
            if (vfs_seek(f, phoff + (u64)i * 56) || vfs_read(f, ph, 56) != 56) break;
            if (rd32(ph) != 4) continue;             /* PT_NOTE */
            u64 off = rd64(ph + 8), sz = rd64(ph + 32);
            if (!sz || sz > 4096) continue;
            u8 *d = kmalloc((size_t)sz);
            if (!d) break;
            if (!vfs_seek(f, off) && vfs_read(f, d, (u32)sz) == (int)sz) ok = note_name(d, (u32)sz, out, outsz);
            kfree(d);
        }
    }
    vfs_close(f);
    return ok;
}

int saver_list(saver_info_t *out, int max) {
    vfs_dirent_t *ents;
    int n = vfs_list("C:/PROGRAMS", &ents), k = 0;
    if (n < 0) return 0;
    for (int i = 0; i < n && k < max; i++) {
        const char *dot = strrchr(ents[i].name, '.');
        if (ents[i].is_dir || !dot || strcasecmp(dot, ".scr")) continue;
        saver_info_t *s = &out[k];
        strlcpy(s->file, ents[i].name, sizeof s->file);
        for (char *c = s->file; *c; c++) *c = (char)toupper((u8)*c);
        char path[VFS_MAX_PATH];
        snprintf(path, sizeof path, "C:/PROGRAMS/%s", ents[i].name);
        if (!scr_name(path, s->name, sizeof s->name)) {
            int len = (int)(dot - ents[i].name);
            snprintf(s->name, sizeof s->name, "%.*s", len, ents[i].name);
        }
        k++;
    }
    kfree(ents);
    /* by name */
    for (int i = 1; i < k; i++)
        for (int j = i; j > 0 && strcasecmp(out[j].name, out[j - 1].name) < 0; j--) {
            saver_info_t t = out[j]; out[j] = out[j - 1]; out[j - 1] = t;
        }
    return k;
}
