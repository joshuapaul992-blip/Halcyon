/* Halcyon OS - display timings and mode lists (docs/DESIGN-M10-DISPLAY.md).
 *
 * A mode is a size with a full timing, as a modesetting driver programs it
 * into a CRTC: pixel clock, active / sync start / sync end / total for both
 * directions, sync polarities. Timings come from the monitor's EDID
 * (detailed timing descriptors in the base block and in CEA-861 extension
 * blocks, DisplayID type I and VII timings, standard and established
 * timings, CEA short video descriptors), from VESA DMT (a table of the
 * common ones) or from the CVT reduced blanking formula. modes_build()
 * turns an EDID and a driver's limits into the list Display Properties
 * shows: one entry per size, or with mode_limits.rates one per size and
 * refresh rate. No other kernel dependency: tested on the host
 * (tests/host/modetest.c). */
#ifndef MODES_H
#define MODES_H
#include "core/types.h"

/* display_mode.flags */
#define DT_HSYNC_POS  0x0001   /* positive horizontal sync */
#define DT_VSYNC_POS  0x0002   /* positive vertical sync */
#define DT_INTERLACE  0x0004   /* interlaced (h is the frame height) */
#define DT_PREFERRED  0x0008   /* the monitor's preferred (native) timing */
#define DT_EDID       0x0010   /* listed by the monitor */
#define DT_DRIVER     0x0020   /* offered by the driver (virtual hardware, no EDID) */
#define DT_DMT        0x0040   /* timing from the VESA DMT table */
#define DT_CVT        0x0080   /* timing from the CVT reduced blanking formula */
#define DT_DETAILED   0x0100   /* timing from a detailed timing descriptor */

typedef struct display_timing {
    u32 clock_khz;                         /* pixel clock; 0 = unknown (firmware modes) */
    u16 hactive, hsync_start, hsync_end, htotal;
    u16 vactive, vsync_start, vsync_end, vtotal;
} display_timing_t;

typedef struct display_mode {
    int w, h;                  /* active size */
    int hz;                    /* refresh rate, rounded (0 = unknown) */
    u32 flags;                 /* DT_* */
    display_timing_t t;
} display_mode_t;

u32  mode_refresh_mhz(const display_timing_t *t);       /* milli-Hz, 0 if unknown */
void mode_set_timing(display_mode_t *m, const display_timing_t *t, u32 flags);
int  mode_dmt(int w, int h, int hz, display_mode_t *out);   /* 1 if the DMT subset has it */
void mode_cvt_rb(int w, int h, int hz, display_mode_t *out);   /* CVT reduced blanking v1 */
/* one detailed timing descriptor (18 bytes); 0 if it is not a timing */
int  edid_detailed(const u8 *p, display_mode_t *out);
/* every timing the monitor lists (base block, CEA-861 and DisplayID
 * extensions), flagged DT_EDID; the first detailed timing is DT_PREFERRED;
 * returns the count */
int  edid_timings(const u8 *e, int size, display_mode_t *out, int max);

typedef struct mode_limits {
    int min_w, min_h;          /* 0: 640 x 480 */
    int max_w, max_h;          /* 0: no limit */
    u32 max_clock_khz;         /* 0: no limit */
    u64 vram;                  /* bytes for a head's scanout buffers; 0: no limit */
    int buffers;               /* scanout buffers a head uses (2 with page flips) */
    int virtual_sizes;         /* virtual hardware: also the common sizes, with CVT timings */
    int rates;                 /* one entry per size and refresh rate (the driver programs each
                                  timing as it is); 0: one per size */
    int (*valid)(void *ctx, const display_mode_t *m);   /* the driver's own check, or NULL */
    void *ctx;
} mode_limits_t;

/* The mode list for a connector: every timing of the EDID (and with
 * virtual_sizes the common sizes, at 60 Hz where DMT has it; pref_w x
 * pref_h, if given, counts as the preferred size when the EDID has none)
 * within the limits.
 * Without lim->rates one per size (the preferred timing, else the EDID's
 * nearest to 60 Hz, else DMT, else CVT); with it one per size and refresh
 * rate rounded to whole Hertz (the preferred timing, else the monitor's
 * detailed timing, else another it lists, else DMT, else CVT); a preferred
 * timing the limits leave out then leaves its size recommended (the
 * highest rate left there is DT_PREFERRED). Interlaced timings are left
 * out. Sorted by width, height, then rate. Returns the count. */
int  modes_build(const u8 *edid, int edid_len, const mode_limits_t *lim, int pref_w, int pref_h,
                 display_mode_t *out, int max);
/* the mode to use at w x h: the one at the preferred mode's refresh rate if
 * the list has it at that size, else the highest rate there; -1 if the
 * size is not in the list */
int  modes_find(const display_mode_t *m, int n, int w, int h);
/* w x h at exactly hz (hz 0: modes_find); -1 if not in the list */
int  modes_find_rate(const display_mode_t *m, int n, int w, int h, int hz);
/* a saved mode at the next start: w x h at hz (0: not saved) if listed,
 * else modes_find's choice at that size with *fell_back set; -1 if the
 * size is not in the list */
int  modes_saved(const display_mode_t *m, int n, int w, int h, int hz, int *fell_back);
/* the refresh rates listed at w x h, lowest first (at most max); the count */
int  modes_rates(const display_mode_t *m, int n, int w, int h, int *hz, int max);
int  modes_preferred(const display_mode_t *m, int n);              /* index or -1 */
/* the mode to start with when nothing was chosen: the preferred one if at
 * most cap_w x cap_h (the UI has no scaling: a 4K panel gets 1920 x 1080),
 * else an exact fraction of it, else the largest of its shape under the
 * cap; without a preferred mode the largest up to 1024 x 768 (at the rate
 * modes_find picks for that size). -1 if n == 0 */
int  modes_default(const display_mode_t *m, int n, int cap_w, int cap_h);
/* "1920 by 1080 pixels, 60 Hertz" style description of a timing for logs:
 * "1920x1080@60 148.50 MHz 1920 2008 2052 2200 / 1080 1084 1089 1125 +h +v" */
void mode_describe(const display_mode_t *m, char *out, int n);

#endif
