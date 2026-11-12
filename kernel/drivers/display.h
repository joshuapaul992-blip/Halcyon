/* Halcyon OS - the display core: adapters, heads, connectors, modes,
 * scanout buffers, hardware cursors, page flips, the desktop arrangement.
 * See docs/DESIGN-M10-DISPLAY.md ("Writing a native display driver").
 *
 * Words:
 *   adapter    one display device (a PCI function) and its driver
 *   head       a scanout engine (CRTC): reads a buffer and sends a mode's
 *              timing to an output; has its own mode, buffer, cursor and
 *              vertical blank
 *   connector  a place a monitor plugs into (DP-1, HDMI-1, VGA-1,
 *              Virtual-2); has a status and an EDID; driven by one of the
 *              heads it can be routed to (possible_heads)
 *   buffer     a scanout buffer (display_fb_t): pixels the CPU writes and
 *              a head reads (VRAM, or RAM the device fetches from)
 *
 * Every machine has the firmware's frame buffer (VBE or GOP) as an adapter
 * with one head and one connector; its modes change only at the next start
 * (the loader sets them). A native driver registers its adapter from
 * display_init(); the core starts it from display_start() (gui_main, once
 * the settings are loaded), unless the F8 menu's "no display driver" flag
 * or SETTINGS.INI's display.disable names it. Any failure on the way (the
 * driver's enable, a mode set, a buffer allocation) gives the device back
 * (ops->restore) and leaves the firmware frame buffer in use, with the
 * reason in BOOT.LOG.
 *
 * The desktop: one surface in system RAM (gfx.c's g_screen, the bounding
 * box of the arrangement); each active head shows its rectangle of it. The
 * compositor draws there and reports damaged rectangles
 * (display_present); the core copies them into each head's buffer and
 * flushes them (ops->flush), or, on heads with page flips, into the back
 * buffer which it flips at the next vertical blank. */
#ifndef DISPLAY_H
#define DISPLAY_H
#include "core/types.h"
#include "drivers/edid.h"
#include "drivers/modes.h"

struct pci_dev;

#define DISPLAY_MAX_ADAPTERS   4
#define DISPLAY_MAX_HEADS      4      /* per adapter */
#define DISPLAY_MAX_CONNECTORS 8      /* per adapter */
#define DISPLAY_MAX_MODES      64     /* per connector */
#define DISPLAY_MAX_OUTPUTS    8      /* monitors on the desktop at once */
#define DISPLAY_EDID_MAX       512

/* head capabilities */
#define HEAD_CAP_CURSOR  0x01         /* hardware cursor (cursor_image / cursor_move) */
#define HEAD_CAP_FLIP    0x02         /* page flips completing at vertical blank */
#define HEAD_CAP_VBLANK  0x04         /* wait_vblank */

/* connector types and states */
enum { CONN_UNKNOWN = 0, CONN_VGA, CONN_DVI, CONN_HDMI, CONN_DP, CONN_LVDS, CONN_EDP, CONN_TV, CONN_VIRTUAL, CONN_FIRMWARE };
enum { CONN_DISCONNECTED = 0, CONN_CONNECTED, CONN_MAYBE };     /* MAYBE: cannot tell (no load detection) */

typedef struct display_adapter display_adapter_t;
typedef struct display_head display_head_t;
typedef struct display_connector display_connector_t;

/* a scanout buffer */
typedef struct display_fb {
    void *px;                  /* CPU address (write-combining VRAM or RAM) */
    u64 phys;
    int w, h;
    u32 pitch;                 /* bytes per line */
    u8 bpp;                    /* 32 (or 24 for some firmware frame buffers) */
    u8 rpos, gpos, bpos;       /* bit positions of 8-bit red, green, blue */
    u32 handle;                /* the driver's: a resource id, a VRAM offset... */
    void *priv;
} display_fb_t;

typedef struct display_ops {
    /* Take the device over from the firmware (before the first set_mode).
     * Optional. Return 0 or an E_* error (the firmware frame buffer stays). */
    int  (*enable)(display_adapter_t *a);
    /* Give the device back to the firmware's mode after a failure: show
     * g_bootinfo's frame buffer again. Optional. */
    void (*restore)(display_adapter_t *a);
    /* Read every connector's status and EDID (display_connector_update).
     * Required. Called at start and after display_hotplug(). */
    void (*detect)(display_adapter_t *a);
    /* A mode the core considers for connector c (already within the
     * adapter's limits): 1 if the hardware can drive it. Optional. */
    int  (*mode_valid)(display_adapter_t *a, display_connector_t *c, const display_mode_t *m);
    /* A w x h, 32 bpp XRGB scanout buffer. Required for live adapters. */
    int  (*fb_alloc)(display_adapter_t *a, int w, int h, display_fb_t *fb);
    void (*fb_free)(display_adapter_t *a, display_fb_t *fb);
    /* Drive connector c from head h with mode m, scanning out fb (already
     * filled by the core); m == NULL switches the head off. Required. */
    int  (*set_mode)(display_head_t *h, display_connector_t *c, const display_mode_t *m, display_fb_t *fb);
    /* The core wrote x, y, w, h of fb (device memory that needs a flush:
     * virtio's host resources). Optional. */
    void (*flush)(display_head_t *h, display_fb_t *fb, int x, int y, int w, int hgt);
    /* Show fb from the next vertical blank (HEAD_CAP_FLIP). The driver
     * calls display_flip_done(h) once the head scans it out (from poll or
     * its interrupt's wake-up). One flip at a time. */
    int  (*flip)(display_head_t *h, display_fb_t *fb);
    /* Wait (bounded, ~1 frame) for the next vertical blank (HEAD_CAP_VBLANK). */
    int  (*wait_vblank)(display_head_t *h);
    /* Hardware cursor (HEAD_CAP_CURSOR): an ARGB image of size x size
     * (h->cursor_size) with its hot spot, and its position in head
     * coordinates (may be partly off the head). */
    int  (*cursor_image)(display_head_t *h, const u32 *argb, int size, int hot_x, int hot_y);
    void (*cursor_move)(display_head_t *h, int x, int y, int visible);
    /* Main-loop work (polled, and woken by the device's interrupt):
     * completed flips, hotplug, deferred commands. Optional. */
    void (*poll)(display_adapter_t *a);
    /* After a fatal error: make what the CPU drew into fb visible, polled,
     * interrupts off. Optional (memory the head scans out directly). */
    void (*panic_flush)(display_head_t *h, display_fb_t *fb);
} display_ops_t;

struct display_head {
    /* set by the driver (display_add_head) */
    int index;                 /* within the adapter */
    u32 caps;                  /* HEAD_CAP_* */
    int cursor_size;           /* 32 or 64 with HEAD_CAP_CURSOR */
    void *priv;
    /* the core's */
    display_adapter_t *adapter;
    display_connector_t *conn; /* the connector it drives, NULL when off */
    display_mode_t mode;
    int x, y;                  /* where it sits on the desktop */
    display_fb_t fb[2];
    int nfb, front;            /* buffers (2: page flips), the one scanned out */
    int flip_pending;
    u64 flip_ms;               /* when the pending flip was queued */
    u32 flips, frames;         /* completed flips, presented frames */
    u32 flip_wait_max_ms;
    int cursor_on;             /* the hardware cursor is shown on this head */
    int stale_n[2];            /* per buffer: rectangles (head coordinates) not yet copied */
    struct { int x, y, w, h; } stale[2][8];
    int dirty_n;               /* single-buffered: rectangles to flush */
    struct { int x, y, w, h; } dirty[8];
};

struct display_connector {
    /* set by the driver */
    char name[16];             /* "DP-1", "HDMI-1", "VGA-1", "Virtual-2": the SETTINGS.INI key */
    int type;                  /* CONN_* */
    u32 possible_heads;        /* bit mask of the adapter's heads that can drive it */
    void *priv;
    /* display_connector_update */
    int status;                /* CONN_CONNECTED... */
    u8 edid[DISPLAY_EDID_MAX];
    int edid_len;
    int pref_w, pref_h;        /* the hardware's size hint (virtio display info) */
    /* the core's */
    display_adapter_t *adapter;
    int index;                 /* within the adapter */
    int number;                /* "1", "2" in Display Properties (desktop-wide, stable) */
    edid_info_t monitor;
    display_mode_t modes[DISPLAY_MAX_MODES];
    int nmodes;
    display_head_t *head;      /* driving it while it is on the desktop */
};

struct display_adapter {
    /* set by the driver (display_register) */
    char name[64];             /* "Virtio GPU", "QEMU/Bochs display adapter" */
    const char *driver;        /* "virtio-gpu", "bochs": BOOT.LOG and display.disable */
    const display_ops_t *ops;
    void *priv;
    struct pci_dev *pci;
    int live;                  /* modes switch at once (else: at the next start) */
    int owns_fw_fb;            /* this device shows the firmware frame buffer */
    int virtual_sizes;         /* virtual hardware: offer the common sizes too */
    int max_w, max_h;          /* largest mode / buffer */
    u32 max_clock_khz;         /* 0: no limit */
    u64 vram;                  /* bytes for scanout buffers (0: no limit) */
    int pitch_align;           /* informational */
    int guarded;               /* a driver that may hang the machine while it starts: the
                                  core keeps a start marker (display_guard_begin) */
    /* the core's */
    display_head_t heads[DISPLAY_MAX_HEADS];
    int nheads;
    display_connector_t conns[DISPLAY_MAX_CONNECTORS];
    int nconns;
    int state;                 /* ADAPTER_* */
    int hotplug;               /* the driver reported a change */
    char why[96];              /* why it failed or is off (a driver may set it before failing) */
    int guard_armed;           /* the start marker was saved this boot */
    int guard_confirm;         /* the GUI asks whether the picture can be seen */
    u64 guard_clear_ms;        /* the marker goes when the desktop has run this long */
};
enum { ADAPTER_PROBED = 0, ADAPTER_ACTIVE, ADAPTER_FAILED, ADAPTER_DISABLED, ADAPTER_RETIRED };

/* ---------------- for drivers ---------------- */
/* register from display_init's probe (no mode set yet); NULL if full */
display_adapter_t *display_register(const char *driver, const char *name, const display_ops_t *ops, void *priv,
                                    struct pci_dev *pci);
display_head_t *display_add_head(display_adapter_t *a, u32 caps, int cursor_size);
display_connector_t *display_add_connector(display_adapter_t *a, const char *name, int type, u32 possible_heads);
/* from ops->detect: the connector's state; edid may be NULL; pref_w/h a hint or 0 */
void display_connector_update(display_connector_t *c, int status, const u8 *edid, int edid_len, int pref_w, int pref_h);
/* something changed (hotplug interrupt, polled status): the core re-runs
 * detect and reconfigures from the main loop */
void display_hotplug(display_adapter_t *a);
/* the flip queued on h completed: its buffer is being scanned out */
void display_flip_done(display_head_t *h);
/* The start guard (drivers with a->guarded): right before the driver's
 * first register write it calls display_guard_begin, which saves
 * display.<driver>.guard=start in SETTINGS.INI and BOOT.LOG. The marker
 * is cleared once the desktop runs on the driver and its picture was
 * confirmed (display_guard_confirm, asked by the GUI for a configuration
 * not confirmed before) or, for a confirmed one, after 10 seconds. A
 * marker still there at the next start keeps the driver off ("blocked")
 * until the user turns it on again (Display Properties, or removing the
 * key). 0, or E_* when the marker cannot be saved (the driver must not go
 * on then). display_guard_checkpoint saves BOOT.LOG before a risky step. */
int  display_guard_begin(display_adapter_t *a);
void display_guard_checkpoint(display_adapter_t *a, const char *what);
/* NULL, or why the driver is off because of the guard */
const char *display_guard_state(const char *driver);
void display_guard_reenable(const char *driver);
/* the GUI, once the desktop is up: an adapter whose picture must be
 * confirmed (NULL: none); then the answer (0: no / no answer) */
display_adapter_t *display_guard_confirm_needed(void);
void display_guard_confirm(display_adapter_t *a, int yes);

/* a fw_cfg test hook asked this step to fail (tests of the fallback):
 * "opt/halcyon/displayfail" = "<driver>:<step>" (step: enable, setmode, fb) */
int  display_test_fail(display_adapter_t *a, const char *step);

/* ---------------- the desktop (GUI) ---------------- */
void display_init(void);                 /* after PCI: firmware adapter, native drivers' probes */
void display_start(void);                /* gui_main, before the screen is set up: start native adapters */
int  display_service(void);              /* each GUI step: driver polls, flips, hotplug; 1 if it did work */

/* the outputs on the desktop, primary first */
typedef struct display_output {
    display_connector_t *conn;
    display_head_t *head;
    int x, y, w, h;            /* desktop coordinates */
    int primary;
} display_output_t;
int  display_outputs(display_output_t *out, int max);
int  display_desktop_size(int *w, int *h);     /* the bounding box; 0 if no output */
/* the desktop surface the compositor draws (gfx.c registers g_screen) */
void display_set_source(const u32 *px, int w, int h, int stride);
/* the compositor changed this rectangle of the desktop */
void display_present(int x, int y, int w, int h);
/* end of a frame: flushes, and flips where they are possible now */
void display_commit(void);
/* commit and wait (bounded) until the flips are done: the screen shows the
 * desktop as it is now (shutdown, restart: the GUI loop stops) */
void display_sync(void);
extern int (*display_changed_hook)(void);      /* the arrangement or a mode changed: the GUI follows (0 or E_NOMEM) */

/* hardware cursor: on when every output's head has one */
int  display_hw_cursor(void);
void display_cursor_image(const u32 *argb64, int hot_x, int hot_y);   /* 64 x 64 ARGB */
void display_cursor_move(int x, int y, int visible);                   /* desktop coordinates */

/* every connector of the started adapters (Display Properties, System Information) */
int  display_connector_count(void);
display_connector_t *display_connector_at(int i);
display_connector_t *display_primary(void);
int  display_adapter_count(void);
display_adapter_t *display_adapter_at(int i);

/* the configuration: what each connector shows */
typedef struct display_conf_out {
    display_connector_t *conn;
    int attach;                /* part of the desktop */
    int w, h;                  /* mode */
    int x, y;                  /* place (any origin: normalized) */
} display_conf_out_t;
typedef struct display_conf {
    int n;
    display_conf_out_t out[DISPLAY_MAX_ADAPTERS * DISPLAY_MAX_CONNECTORS];
    display_connector_t *primary;
} display_conf_t;
void display_get_conf(display_conf_t *c);            /* as it is now */
int  display_set_conf(const display_conf_t *c);      /* apply live; on failure back to the old one; 0 or E_* */
int  display_save_conf(const display_conf_t *c);     /* SETTINGS.INI (display.<connector>.*) */
int  display_conf_live(const display_conf_t *c);     /* can this be applied without a restart? */

/* After a fatal error (core/panic.c): phase 0 points g_bootinfo.fb_* at
 * the primary head's visible buffer, phase 1 makes the drawing visible */
void display_panic(int phase);

/* ---------------- the primary monitor (older callers) ---------------- */
const char *display_adapter(void);       /* the primary's adapter: "QEMU/Bochs display adapter" */
int  display_live(void);                 /* 1: modes change at once; 0: at the next start */
int  display_mode_count(void);
display_mode_t display_mode(int i);      /* sorted by width, then height */
int  display_find(int w, int h);         /* index or -1 */
const edid_info_t *display_monitor(void);
int  display_native(int *w, int *h);     /* the monitor's native mode, if offered */
int  display_set_mode(int w, int h);     /* the primary, now (live adapters); 0 or E_* */
int  display_save_mode(int w, int h);    /* the primary at the next start */
void display_apply_saved(void);          /* == display_start */

#endif
