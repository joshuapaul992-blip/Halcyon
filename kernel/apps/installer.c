/* Halcyon OS - Halcyon Setup: the installation wizard (Start menu "Install
 * Halcyon", and the welcome prompt when Halcyon starts from a USB stick).
 *
 * Pages, in the style of the Windows XP wizards:
 *   Welcome -> Choose a disk -> Where to install (the whole disk / the free
 *   space) -> Ready to install -> Installing (progress) -> Finish
 * The work itself is apps/instcore.c, on a kernel thread; the wizard only
 * watches it. See docs/DESIGN-M8-INSTALL.md. */
#include "gui/wm.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/kernel.h"
#include "core/thread.h"
#include "core/settings.h"
#include "core/efirt.h"
#include "apps/fileops.h"
#include "apps/instcore.h"

#define CW 500
#define CH 372
#define FOOT 46                         /* the button strip at the bottom */
#define BANNER_W 164                    /* welcome / finish pages */
#define HEAD_H 58                       /* interior pages */

enum { P_WELCOME, P_DISK, P_MODE, P_SUMMARY, P_PROGRESS, P_FINISH };
enum { ID_WBACK = 500, ID_WNEXT, ID_WCANCEL, ID_TITLE, ID_TEXT, ID_DISKS, ID_DISKINFO, ID_LABEL1, ID_WHOLE_TEXT,
       ID_FREE_TEXT, ID_SUMMARY, ID_STATUS, ID_BAR, ID_FILE };

typedef struct wiz {
    window_t *w;
    int page;
    inst_disk_t disks[INST_MAX_DISKS];
    int ndisks, surveyed;
    int disk;                           /* index into disks, -1 = none */
    int mode;                           /* INST_WHOLE / INST_FREE */
    inst_plan_t plan;
    inst_job_t *job;
    char text[1024];
} wiz_t;

static wiz_t *g_wiz;

static const char *page_title(int p) {
    static const char *const t[] = { "", "Choose a disk", "Where to install", "Ready to install", "Installing Halcyon", "" };
    return t[p];
}
static const char *page_sub(int p) {
    static const char *const t[] = { "", "Select the hard disk on which you want to install Halcyon.",
                                     "Choose how Halcyon uses the disk.",
                                     "Setup has the information it needs to install Halcyon.",
                                     "Please wait while Setup installs Halcyon. This takes a few minutes.", "" };
    return t[p];
}

static widget_t *W(wiz_t *z, int id) { return widget_find(z->w, id); }
static void show(wiz_t *z, int id, int on) {
    widget_t *wd = W(z, id);
    if (!wd) return;
    if (on) wd->style &= ~WS_HIDDEN; else wd->style |= WS_HIDDEN;
}
static void enable(wiz_t *z, int id, int on) {
    widget_t *wd = W(z, id);
    if (!wd) return;
    if (on) wd->style &= ~WS_DISABLED; else wd->style |= WS_DISABLED;
}
static void set_text(wiz_t *z, int id, const char *t) {
    widget_t *wd = W(z, id);
    if (wd && strcmp(wd->text, t)) widget_set_text(z->w, wd, t);
}

static inst_disk_t *cur_disk(wiz_t *z) { return z->disk >= 0 && z->disk < z->ndisks ? &z->disks[z->disk] : NULL; }

/* the radio buttons of the mode page */
static rect_t radio_rect(int i) { return mkrect(40, i ? 168 : 76, 420, 18); }

/* ---------------- painting ---------------- */

static void paint_radio(gc_t *g, rect_t r, const char *label, int on, int disabled) {
    int cx = r.x + 6, cy = r.y + r.h / 2;
    gfx_circle(g, cx, cy, 6, HEX(0x1C5180));
    gfx_circle(g, cx, cy, 5, HEX(0xFFFFFF));
    gfx_circle(g, cx, cy, 4, HEX(0xF2F2EC));
    if (on) {
        gfx_circle(g, cx, cy, 3, disabled ? HEX(0xA19F94) : HEX(0x2BAA2B));
        gfx_circle(g, cx, cy, 2, disabled ? HEX(0xB5B3A8) : HEX(0x5ACC4E));
    }
    gfx_text_box(g, &font_ui_bold, mkrect(r.x + 19, r.y, r.w - 19, r.h), label, disabled ? HEX(0xA19F94) : HEX(0x000000), 0);
}

static void wiz_paint(window_t *w, gc_t *g) {
    wiz_t *z = w->data;
    int body = CH - FOOT;
    gfx_fill(g, mkrect(0, 0, CW, CH), g_theme->face);
    if (z->page == P_WELCOME || z->page == P_FINISH) {
        /* a blue banner on the left, white on the right */
        gfx_gradient_v(g, mkrect(0, 0, BANNER_W, body), color_lighten(g_theme->title_top, 10), g_theme->title_bottom);
        gfx_gradient_h(g, mkrect(BANNER_W - 30, 0, 30, body), g_theme->title_bottom, color_lighten(g_theme->title_bottom, -25));
        for (int i = 0; i < 5; i++)        /* soft rings behind the logo */
            gfx_circle(g, BANNER_W / 2, 92, 58 - i * 9, color_lighten(g_theme->title_mid, 8 + i * 6));
        gfx_circle(g, BANNER_W / 2, 92, 30, HEX(0xFFFFFF));
        gfx_icon(g, BANNER_W / 2 - 24, 68, &icons48[ICON_LOGO]);
        gfx_text_box(g, &font_large, mkrect(0, 170, BANNER_W, 24), "Halcyon", HEX(0xFFFFFF), 1);
        gfx_text_box(g, &font_ui, mkrect(0, 196, BANNER_W, 16), "Setup", HEX(0xDCE8FF), 1);
        gfx_fill(g, mkrect(BANNER_W, 0, CW - BANNER_W, body), HEX(0xFFFFFF));
        widget_t *t = W(z, ID_TITLE);
        if (t) gfx_text_wrap(g, &font_large, mkrect(BANNER_W + 18, 20, CW - BANNER_W - 36, 56), t->text, HEX(0x000000));
    } else {
        gfx_fill(g, mkrect(0, 0, CW, HEAD_H), HEX(0xFFFFFF));
        gfx_text(g, &font_ui_bold, 20, 13, page_title(z->page), HEX(0x000000));
        gfx_text(g, &font_ui, 36, 32, page_sub(z->page), HEX(0x333333));
        gfx_icon(g, CW - 50, 12, &icons32[ICON_SETUP]);
        gfx_hline(g, 0, HEAD_H, CW, HEX(0xA0A0A0));
        gfx_hline(g, 0, HEAD_H + 1, CW, HEX(0xFFFFFF));
    }
    gfx_hline(g, 0, body, CW, HEX(0xA0A0A0));
    gfx_hline(g, 0, body + 1, CW, HEX(0xFFFFFF));
    if (z->page == P_MODE) {
        inst_disk_t *d = cur_disk(z);
        paint_radio(g, radio_rect(0), "Use the whole disk", z->mode == INST_WHOLE, 0);
        paint_radio(g, radio_rect(1), "Install in the free space", z->mode == INST_FREE, !(d && d->free_ok));
    }
    if (z->page == P_PROGRESS && z->job) {
        /* the steps, ticked off */
        int y = 160;
        for (int s = IS_PARTITION; s < IS_DONE; s++) {
            if (s == IS_BOOTCODE && !z->plan.bios) continue;
            if (s == IS_FIRMWARE && !z->plan.uefi_entry) continue;
            int done = z->job->step > s || (z->job->done && !z->job->result), now = z->job->step == s && !z->job->done;
            color_t c = done ? HEX(0x000000) : now ? HEX(0x000000) : HEX(0x8A8A8A);
            if (done) {
                for (int i = 0; i < 3; i++) {
                    gfx_line(g, 44, y + 6 + i, 47, y + 9 + i, HEX(0x21A121));
                    gfx_line(g, 47, y + 9 + i, 53, y + 3 + i, HEX(0x21A121));
                }
            } else if (now) {
                gfx_fill(g, mkrect(45, y + 5, 7, 7), g_theme->title_mid);
            }
            gfx_text(g, now ? &font_ui_bold : &font_ui, 62, y + 2, inst_step_name(s), c);
            y += 20;
        }
    }
}

/* ---------------- pages ---------------- */

static void disk_info(wiz_t *z) {
    inst_disk_t *d = cur_disk(z);
    char *t = z->text;
    t[0] = 0;
    if (!d) {
        set_text(z, ID_DISKINFO, z->ndisks ? "" :
                 "Setup did not find an internal hard disk (SATA or NVMe) to install Halcyon on.");
        return;
    }
    size_t n = 0;
    if (!d->usable) n += snprintf(t + n, sizeof z->text - n, "This disk cannot be used: %s\n\n", d->why);
    const char *kind = d->pt->kind == PT_GPT ? "GPT" : d->pt->kind == PT_MBR ? "MBR" : "";
    if (!d->nvols && d->pt->kind != PT_OTHER) n += snprintf(t + n, sizeof z->text - n, "The disk is empty.\n");
    else if (d->pt->kind == PT_OTHER) n += snprintf(t + n, sizeof z->text - n, "%s\n", d->pt->problem);
    else n += snprintf(t + n, sizeof z->text - n, "Partition table: %s\n", kind);
    for (int i = 0; i < d->nvols && n < sizeof z->text - 120; i++) {
        char line[200];
        inst_vol_line(d, &d->vols[i], line, sizeof line);
        n += snprintf(t + n, sizeof z->text - n, "    %s\n", line);
    }
    if (d->free_count) {
        char sz[32];
        format_size(d->free_count * d->bd->sector_size, sz, sizeof sz);
        n += snprintf(t + n, sizeof z->text - n, "    Unpartitioned space: %s\n", sz);
    }
    if (d->windows) snprintf(t + n, sizeof z->text - n, "\nWindows is installed on this disk.");
    set_text(z, ID_DISKINFO, t);
}

static void mode_texts(wiz_t *z) {
    inst_disk_t *d = cur_disk(z);
    if (!d) return;
    char t[600], sz[32], title[120];
    inst_disk_title(d, title, sizeof title);
    if (d->nvols)
        snprintf(t, sizeof t, "Everything on %s is erased, including its %d partition%s%s. Halcyon gets the whole disk.",
                 title, d->nvols, d->nvols == 1 ? "" : "s", d->windows ? " and Windows" : "");
    else
        snprintf(t, sizeof t, "Halcyon gets the whole of %s. There is nothing on the disk yet.", title);
    set_text(z, ID_WHOLE_TEXT, t);
    if (d->free_ok) {
        format_size(d->free_count * d->bd->sector_size, sz, sizeof sz);
        snprintf(t, sizeof t, "Halcyon goes into the %s of unpartitioned space. The partitions already on the disk "
                 "are kept as they are%s; none is shrunk or moved.", sz,
                 d->windows ? ", and Windows still starts" : "");
    } else {
        snprintf(t, sizeof t, "Not available: %s", d->free_why);
    }
    set_text(z, ID_FREE_TEXT, t);
}

static void summary(wiz_t *z) {
    inst_disk_t *d = cur_disk(z);
    inst_plan_t *p = &z->plan;
    char *t = z->text, title[120], sz[32], files[32];
    inst_disk_title(d, title, sizeof title);
    format_size(p->count * d->bd->sector_size, sz, sizeof sz);
    format_size(p->copy_bytes, files, sizeof files);
    size_t n = 0;
    n += snprintf(t + n, sizeof z->text - n, "Disk:\n    %s\n\n", title);
    if (p->mode == INST_WHOLE)
        n += snprintf(t + n, sizeof z->text - n, "What happens:\n    The whole disk is erased. A new %s partition table gets one "
                      "%s partition for Halcyon (FAT32)%s.\n\n", p->gpt ? "GPT" : "MBR", sz,
                      p->bb_count ? " and a small BIOS boot partition" : "");
    else
        n += snprintf(t + n, sizeof z->text - n, "What happens:\n    A new %s partition for Halcyon (FAT32) is made in the free "
                      "space%s. The other partitions are not changed.\n\n", sz,
                      p->bb_count ? ", with a small BIOS boot partition" : "");
    n += snprintf(t + n, sizeof z->text - n, "Starting Halcyon:\n");
    if (p->uefi_entry) n += snprintf(t + n, sizeof z->text - n, "    UEFI: 'Halcyon' is added to the firmware's boot menu.\n");
    else if (g_bootinfo.boot_type == BOOT_TYPE_UEFI) n += snprintf(t + n, sizeof z->text - n, "    UEFI: choose the disk in the firmware's boot menu.\n");
    else n += snprintf(t + n, sizeof z->text - n, "    UEFI: the disk also starts on UEFI computers.\n");
    if (p->bios)
        n += snprintf(t + n, sizeof z->text - n, "    BIOS: Halcyon's boot code is written to the disk%s.\n",
                      d->windows && p->mode == INST_FREE && !p->gpt ? "; a menu lets you choose Halcyon or Windows" : "");
    else
        n += snprintf(t + n, sizeof z->text - n, "    BIOS: %s\n", p->bios_note);
    snprintf(t + n, sizeof z->text - n, "\nFiles: %u files, %s, copied and then verified.\n\nClick Install to begin.",
             p->copy_files, files);
    set_text(z, ID_SUMMARY, t);
}

static void finish_texts(wiz_t *z) {
    inst_job_t *j = z->job;
    char *t = z->text, title[120];
    inst_disk_t *d = cur_disk(z);
    inst_disk_title(d, title, sizeof title);
    if (j && !j->result) {
        set_text(z, ID_TITLE, "Completing the Halcyon Setup Wizard");
        size_t n = snprintf(t, sizeof z->text, "Halcyon is installed on %s.\n\n", title);
        if (j->warning[0]) n += snprintf(t + n, sizeof z->text - n, "%s\n\n", j->warning);
        if (d->windows && z->plan.mode == INST_FREE)
            n += snprintf(t + n, sizeof z->text - n, "Windows stays on the disk and still starts: choose it in the %s.\n\n",
                          z->plan.gpt ? "firmware's boot menu" : "menu shown when the computer starts");
        snprintf(t + n, sizeof z->text - n, "Remove the USB stick, then click Restart to start Halcyon from the hard disk.");
    } else {
        set_text(z, ID_TITLE, "Setup could not install Halcyon");
        snprintf(t, sizeof z->text, "%s\n\n%s", j ? j->error : "", j && j->step > IS_PARTITION ?
                 "The disk now holds an incomplete installation of Halcyon. Run Setup again to install it once more." :
                 "Nothing on the disk was changed.");
    }
    set_text(z, ID_TEXT, t);
}

static void set_page(wiz_t *z, int page) {
    z->page = page;
    int banner = page == P_WELCOME || page == P_FINISH;
    show(z, ID_TITLE, 0);               /* (holds the text; drawn in the large type by wiz_paint) */
    show(z, ID_TEXT, banner);
    show(z, ID_LABEL1, page == P_DISK);
    show(z, ID_DISKS, page == P_DISK);
    show(z, ID_DISKINFO, page == P_DISK);
    show(z, ID_WHOLE_TEXT, page == P_MODE);
    show(z, ID_FREE_TEXT, page == P_MODE);
    show(z, ID_SUMMARY, page == P_SUMMARY);
    show(z, ID_STATUS, page == P_PROGRESS);
    show(z, ID_BAR, page == P_PROGRESS);
    show(z, ID_FILE, page == P_PROGRESS);
    show(z, ID_WBACK, page != P_FINISH);
    show(z, ID_WNEXT, page != P_FINISH || (z->job && !z->job->result));
    enable(z, ID_WBACK, page > P_WELCOME && page < P_PROGRESS);
    enable(z, ID_WNEXT, page != P_PROGRESS);
    enable(z, ID_WCANCEL, page != P_PROGRESS);
    set_text(z, ID_WNEXT, page == P_SUMMARY ? "Install" : page == P_FINISH ? "Restart" : "Next >");
    set_text(z, ID_WCANCEL, page == P_FINISH ? "Close" : "Cancel");
    if (page == P_WELCOME) {
        set_text(z, ID_TITLE, "Welcome to the Halcyon Setup Wizard");
        set_text(z, ID_TEXT, "This wizard installs Halcyon on a hard disk of this computer, so that it starts without "
                 "the USB stick.\n\nHalcyon can have a disk of its own, or go into free space on a disk next to "
                 "Windows. Partitions that are already on the disk are never shrunk or moved, and Windows keeps "
                 "starting.\n\nYour settings and documents on the stick are copied too.\n\nTo continue, click Next.");
    }
    if (page == P_DISK) {
        disk_info(z);
        enable(z, ID_WNEXT, cur_disk(z) && cur_disk(z)->usable);
    }
    if (page == P_MODE) mode_texts(z);
    if (page == P_SUMMARY) summary(z);
    if (page == P_FINISH) finish_texts(z);
    widget_t *next = W(z, ID_WNEXT);
    if (next && !(next->style & (WS_HIDDEN | WS_DISABLED))) z->w->focus = next;
    else if (page == P_FINISH) z->w->focus = W(z, ID_WCANCEL);
    if (page == P_DISK && z->ndisks) z->w->focus = W(z, ID_DISKS);
    wm_invalidate(z->w);
}

static void survey(wiz_t *z) {
    if (z->surveyed) inst_survey_free(z->disks, z->ndisks);
    wm_set_busy(1);
    z->ndisks = inst_survey(z->disks, INST_MAX_DISKS);
    wm_set_busy(0);
    z->surveyed = 1;
    char items[INST_MAX_DISKS][200], *ptrs[INST_MAX_DISKS];
    z->disk = -1;
    for (int i = 0; i < z->ndisks; i++) {
        char title[120];
        inst_disk_title(&z->disks[i], title, sizeof title);
        snprintf(items[i], sizeof items[i], "Disk %d: %s%s", i + 1, title,
                 z->disks[i].usable ? "" : "  (cannot be used)");
        ptrs[i] = items[i];
        if (z->disk < 0 && z->disks[i].usable) z->disk = i;
    }
    widget_t *lb = W(z, ID_DISKS);
    widget_list_set(z->w, lb, ptrs, z->ndisks);
    lb->value = z->disk;
}

static void disk_changed(window_t *w, widget_t *wd) {
    wiz_t *z = w->data;
    z->disk = wd->value;
    disk_info(z);
    enable(z, ID_WNEXT, cur_disk(z) && cur_disk(z)->usable);
    wm_invalidate(w);
}

static void thread_main(void *arg) { inst_run(arg); }

static void start_install(wiz_t *z) {
    inst_disk_t *d = cur_disk(z);
    if (z->plan.mode == INST_WHOLE) {
        char title[120], msg[1200];
        inst_disk_title(d, title, sizeof title);
        size_t n = snprintf(msg, sizeof msg, "All data on Disk %d, %s, will be erased", z->disk + 1, title);
        if (d->nvols) {
            n += snprintf(msg + n, sizeof msg - n, ":\n\n");
            for (int i = 0; i < d->nvols && i < 8; i++) {
                char line[200];
                inst_vol_line(d, &d->vols[i], line, sizeof line);
                n += snprintf(msg + n, sizeof msg - n, "%s\n", line);
            }
        } else {
            n += snprintf(msg + n, sizeof msg - n, ".\n");
        }
        snprintf(msg + n, sizeof msg - n, "\nAre you sure you want to erase this disk and install Halcyon on it?");
        if (msgbox(z->w, "Halcyon Setup", msg, ICON_WARNING, MB_YESNO) != ID_YES) return;
    }
    inst_job_t *j = kzalloc(sizeof *j);
    if (!j) { msgbox(z->w, "Halcyon Setup", "Out of memory.", ICON_ERROR, MB_OK); return; }
    j->plan = z->plan;
    z->job = j;
    set_page(z, P_PROGRESS);
    if (!thread_create("installer", thread_main, j)) inst_run(j);
}

static void go_next(window_t *w, widget_t *wd) {
    wiz_t *z = w->data;
    char why[300];
    switch (z->page) {
    case P_WELCOME:
        survey(z);
        set_page(z, P_DISK);
        break;
    case P_DISK: {
        inst_disk_t *d = cur_disk(z);
        if (!d || !d->usable) return;
        z->mode = d->free_ok && d->nvols ? INST_FREE : INST_WHOLE;
        set_page(z, P_MODE);
        break;
    }
    case P_MODE:
        wm_set_busy(1);
        if (inst_plan(cur_disk(z), z->mode, &z->plan, why, sizeof why)) {
            wm_set_busy(0);
            msgbox(w, "Halcyon Setup", why, ICON_WARNING, MB_OK);
            return;
        }
        wm_set_busy(0);
        set_page(z, P_SUMMARY);
        break;
    case P_SUMMARY:
        start_install(z);
        break;
    case P_FINISH:
        if (z->job && !z->job->result) desktop_restart();
        break;
    }
}

static void go_back(window_t *w, widget_t *wd) {
    wiz_t *z = w->data;
    if (z->page > P_WELCOME && z->page < P_PROGRESS) set_page(z, z->page - 1);
}

static void go_cancel(window_t *w, widget_t *wd) {
    wiz_t *z = w->data;
    if (z->page == P_PROGRESS) return;
    wm_close(w);
}

static int wiz_close(window_t *w) {
    wiz_t *z = w->data;
    return !(z->page == P_PROGRESS && z->job && !z->job->done);    /* not while installing */
}

static void wiz_destroy(window_t *w) {
    wiz_t *z = w->data;
    if (z->surveyed) inst_survey_free(z->disks, z->ndisks);
    if (z->job && z->job->done) kfree(z->job);      /* (a running job keeps its memory) */
    kfree(z);
    if (g_wiz == z) g_wiz = NULL;
}

static void wiz_event(window_t *w, gui_event_t *e) {
    wiz_t *z = w->data;
    if (z->page != P_MODE) return;
    inst_disk_t *d = cur_disk(z);
    int pick = 0;
    if (e->type == GE_MOUSE_DOWN) {
        if (rect_contains(radio_rect(0), e->x, e->y)) pick = INST_WHOLE;
        if (rect_contains(radio_rect(1), e->x, e->y)) pick = INST_FREE;
    }
    if (e->type == GE_KEY && e->key.pressed && (e->key.key == KEY_UP || e->key.key == KEY_DOWN))
        pick = z->mode == INST_WHOLE ? INST_FREE : INST_WHOLE;
    if (pick == INST_FREE && !(d && d->free_ok)) pick = 0;
    if (pick && pick != z->mode) { z->mode = pick; wm_invalidate(w); }
}

static void wiz_tick(window_t *w) {
    wiz_t *z = w->data;
    if (z->page != P_PROGRESS || !z->job) return;
    inst_job_t *j = z->job;
    char t[VFS_MAX_PATH + 40];
    snprintf(t, sizeof t, "%s...", j->status);
    set_text(z, ID_STATUS, t);
    int val = 0;
    /* copying is most of the work: 5% before it, 5-75% copying, 75-98% verifying */
    if (j->step < IS_COPY) val = j->step * 12;
    else if (j->step == IS_COPY) val = 50 + (int)(j->total_bytes ? j->done_bytes * 650 / j->total_bytes : 0);
    else if (j->step == IS_VERIFY) val = 700 + (int)(j->total_bytes ? j->done_bytes * 280 / j->total_bytes : 0);
    else val = 990;
    widget_t *bar = W(z, ID_BAR);
    if (bar && bar->value != val) { bar->value = MIN(val, 1000); wm_invalidate(w); }
    if (j->step == IS_COPY || j->step == IS_VERIFY) snprintf(t, sizeof t, "%s", j->current);
    else t[0] = 0;
    set_text(z, ID_FILE, t);
    static int last_step = -1;
    if (j->step != last_step) { last_step = j->step; wm_invalidate(w); }
    if (j->done) {
        if (bar) bar->value = 1000;
        set_page(z, P_FINISH);
    }
}

void app_installer_disk(const char *preselect) {
    if (g_wiz && wm_alive(g_wiz->w)) { wm_show(g_wiz->w); return; }
    wiz_t *z = kzalloc(sizeof *z);
    if (!z) return;
    z->disk = -1;
    window_t *w = wm_create("Halcyon Setup", ICON_SETUP, 0, 0, CW, CH, WF_DIALOG | WF_CENTER | WF_NO_MAXIMIZE);
    if (!w) { kfree(z); return; }
    z->w = w;
    g_wiz = z;
    w->data = z;
    w->on_paint = wiz_paint;
    w->on_event = wiz_event;
    w->on_tick = wiz_tick;
    w->on_close = wiz_close;
    w->on_destroy = wiz_destroy;
    int body = CH - FOOT;
    widget_add(w, W_LABEL, mkrect(BANNER_W + 18, 18, CW - BANNER_W - 36, 44), "", ID_TITLE)->style |= WS_HIDDEN;
    widget_add(w, W_LABEL, mkrect(BANNER_W + 18, 84, CW - BANNER_W - 36, body - 96), "", ID_TEXT)->style |= WS_WRAP;
    widget_add(w, W_LABEL, mkrect(40, 72, 420, 16), "Disks in this computer:", ID_LABEL1);
    widget_t *lb = widget_add(w, W_LISTBOX, mkrect(40, 90, 420, 4 * 18 + 2), "", ID_DISKS);
    lb->on_change = disk_changed;
    widget_add(w, W_LABEL, mkrect(40, 176, 420, body - 182), "", ID_DISKINFO)->style |= WS_WRAP;
    widget_add(w, W_LABEL, mkrect(59, 98, 400, 60), "", ID_WHOLE_TEXT)->style |= WS_WRAP;
    widget_add(w, W_LABEL, mkrect(59, 190, 400, 80), "", ID_FREE_TEXT)->style |= WS_WRAP;
    widget_add(w, W_LABEL, mkrect(40, 70, 430, body - 76), "", ID_SUMMARY)->style |= WS_WRAP;
    widget_add(w, W_LABEL, mkrect(40, 78, 420, 16), "", ID_STATUS);
    widget_add(w, W_PROGRESS, mkrect(40, 100, 420, 18), "", ID_BAR);
    widget_add(w, W_LABEL, mkrect(40, 126, 420, 16), "", ID_FILE);
    widget_t *b = widget_add(w, W_BUTTON, mkrect(CW - 262, CH - 35, 75, 23), "< Back", ID_WBACK);
    b->on_click = go_back;
    b = widget_add(w, W_BUTTON, mkrect(CW - 187, CH - 35, 75, 23), "Next >", ID_WNEXT);
    b->style |= WS_DEFAULT;
    b->on_click = go_next;
    b = widget_add(w, W_BUTTON, mkrect(CW - 95, CH - 35, 75, 23), "Cancel", ID_WCANCEL);
    b->style |= WS_CANCEL;
    b->on_click = go_cancel;
    set_page(z, P_WELCOME);
    if (preselect) {
        survey(z);
        for (int i = 0; i < z->ndisks; i++)
            if (!strcmp(z->disks[i].bd->name, preselect) || !strcmp(z->disks[i].bd->model, preselect)) z->disk = i;
        W(z, ID_DISKS)->value = z->disk;
    }
}

void app_installer(void) { app_installer_disk(NULL); }

/* ---------------- the welcome prompt ---------------- */
enum { ID_W_INSTALL = 520, ID_W_LATER, ID_W_DONTSHOW };

static void welcome_done(window_t *w) {
    widget_t *cb = widget_find(w, ID_W_DONTSHOW);
    if (cb && cb->value) {
        settings_set("install.welcome", "0");
        settings_save();
    }
}
static void welcome_install(window_t *w, widget_t *wd) {
    welcome_done(w);
    w->on_close = NULL;
    wm_destroy(w);
    app_installer();
}
static void welcome_later(window_t *w, widget_t *wd) {
    welcome_done(w);
    w->on_close = NULL;
    wm_destroy(w);
}
static int welcome_close(window_t *w) { welcome_done(w); return 1; }

static void welcome_paint(window_t *w, gc_t *g) {
    int cw = w->client->w;
    gfx_gradient_v(g, mkrect(0, 0, cw, 64), color_lighten(g_theme->title_top, 10), g_theme->title_bottom);
    gfx_icon(g, 16, 8, &icons48[ICON_LOGO]);
    gfx_text(g, &font_large, 77, 21, "Welcome to Halcyon", color_lighten(g_theme->title_bottom, -60));
    gfx_text(g, &font_large, 76, 20, "Welcome to Halcyon", HEX(0xFFFFFF));
}

void install_welcome(void) {
    window_t *w = wm_create("Welcome to Halcyon", ICON_LOGO, 0, 0, 420, 232, WF_DIALOG | WF_CENTER);
    if (!w) return;
    w->on_paint = welcome_paint;
    w->on_close = welcome_close;
    widget_add(w, W_LABEL, mkrect(20, 78, 380, 80),
               "Halcyon is running from a USB stick. You can keep using it this way, or install it on this "
               "computer's hard disk so that it starts without the stick.\n\nIt can go next to Windows: the "
               "partitions already on the disk are kept.", 0)->style |= WS_WRAP;
    widget_add(w, W_CHECKBOX, mkrect(20, 162, 220, 18), "Don't show this again", ID_W_DONTSHOW);
    widget_t *b = widget_add(w, W_BUTTON, mkrect(196, 196, 120, 23), "Install Halcyon...", ID_W_INSTALL);
    b->style |= WS_DEFAULT;
    b->on_click = welcome_install;
    b = widget_add(w, W_BUTTON, mkrect(324, 196, 80, 23), "Not now", ID_W_LATER);
    b->style |= WS_CANCEL;
    b->on_click = welcome_later;
    w->focus = widget_find(w, ID_W_INSTALL);
}

/* ---------------- after the desktop is up ---------------- */

static void test_thread(void *arg) {
    inst_job_t *j = arg;
    inst_run(j);
    if (j->result) kprintf("install test: failed: %s\n", j->error);
    else kprintf("install test: done: %u files, %u folders on %c: (%s), boot entry %d%s%s\n", j->files, j->dirs,
                 j->letter, j->part ? j->part->name : "?", j->boot_entry, j->warning[0] ? "; " : "", j->warning);
}

/* test hook: QEMU fw_cfg "opt/halcyon/installtest" = "DISK MODE" with DISK a
 * disk's name ("sata1") or model, MODE "whole" or "free" (an installation
 * as the wizard would make it, without questions) or "wizard" (the wizard
 * opens with that disk chosen, for screenshots). Never active on real
 * hardware (fw_cfg exists only under QEMU). */
static int install_test_hook(void) {
    char spec[128];
    if (!qemu_fw_file("opt/halcyon/installtest", spec, sizeof spec) || !spec[0]) return 0;
    char *mode = strchr(spec, ' ');
    if (mode) *mode++ = 0;
    if (mode && !strcmp(mode, "wizard")) { app_installer_disk(spec); return 1; }
    if (!mode || (strcmp(mode, "whole") && strcmp(mode, "free"))) { kprintf("install test: bad spec\n"); return 1; }
    static inst_disk_t disks[INST_MAX_DISKS];
    int n = inst_survey(disks, INST_MAX_DISKS), k = -1;
    for (int i = 0; i < n; i++) {
        char title[160];
        inst_disk_title(&disks[i], title, sizeof title);
        kprintf("install test: disk %s: %s%s%s, %d partitions, free space %lu sectors%s%s\n", disks[i].bd->name, title,
                disks[i].usable ? "" : ", cannot be used: ", disks[i].usable ? "" : disks[i].why, disks[i].nvols,
                disks[i].free_count, disks[i].windows ? ", Windows" : "", disks[i].free_ok ? "" : "");
        if (!strcmp(disks[i].bd->name, spec) || !strcmp(disks[i].bd->model, spec)) k = i;
    }
    char why[300];
    inst_job_t *j = kzalloc(sizeof *j);
    if (k < 0 || !j) { kprintf("install test: failed: no disk '%s'\n", spec); inst_survey_free(disks, n); return 1; }
    if (inst_plan(&disks[k], !strcmp(mode, "whole") ? INST_WHOLE : INST_FREE, &j->plan, why, sizeof why)) {
        kprintf("install test: failed: %s\n", why);
        inst_survey_free(disks, n);
        return 1;
    }
    inst_survey_free(disks, n);
    kprintf("install test: plan: %s, %s, partition %lu+%lu, BIOS boot code %s, firmware boot entry %s\n",
            j->plan.mode == INST_WHOLE ? "whole disk" : "free space", j->plan.gpt ? "GPT" : "MBR", j->plan.start,
            j->plan.count, j->plan.bios ? "yes" : "no", j->plan.uefi_entry ? "yes" : "no");
    if (!thread_create("installer", test_thread, j)) test_thread(j);
    return 1;
}

static int booted_from_removable(void) {
    for (int i = 0; i < vfs_volume_count(); i++) {
        volume_t *v = vfs_volume(i);
        if (!v->is_boot || !v->bd) continue;
        blockdev_t *d = v->bd->parent ? v->bd->parent : v->bd;
        return d->removable || d->type == BDEV_DISK_USB || d->type == BDEV_DISK_CD;
    }
    return 0;
}

static int internal_disk_present(void) {
    for (blockdev_t *b = blockdev_first(); b; b = b->next)
        if (!b->parent && b->present && !b->removable && (b->type == BDEV_DISK_SATA || b->type == BDEV_DISK_NVME)) return 1;
    return 0;
}

/* called once the desktop is up (gui_main) */
void installer_boot_hook(void) {
    efi_log_boot_entry();
    if (install_test_hook()) return;
    char nw[8];
    if (qemu_fw_file("opt/halcyon/nowelcome", nw, sizeof nw)) return;   /* (automated tests) */
    if (!booted_from_removable() || !internal_disk_present() || !settings_get_int("install.welcome", 1)) return;
    install_welcome();
}
