/* Halcyon OS - display adapters and screen modes.
 *
 * Every machine has the firmware's frame buffer (VBE or GOP): its modes are
 * the ones the loader listed, and a change takes effect at the next start (the
 * loader sets it). An adapter driver that can program the hardware (the
 * Bochs/QEMU/VirtualBox adapter today, NVIDIA/Intel/AMD later) switches at
 * once. g_bootinfo.fb_* always describe the mode in use. */
#ifndef DISPLAY_H
#define DISPLAY_H
#include "core/types.h"
#include "drivers/edid.h"

typedef struct display_mode { int w, h; } display_mode_t;

void display_init(void);                 /* after PCI */
const char *display_adapter(void);       /* "QEMU/Bochs display adapter" */
int  display_live(void);                 /* 1: modes change at once; 0: at the next start */
int  display_mode_count(void);
display_mode_t display_mode(int i);      /* sorted by width, then height */
int  display_find(int w, int h);         /* index or -1 */
const edid_info_t *display_monitor(void);
int  display_native(int *w, int *h);     /* the monitor's native mode, if offered */

/* Switch now (live adapters). Returns 0 and calls display_changed_hook, or
 * an E_* error. */
int  display_set_mode(int w, int h);
extern int (*display_changed_hook)(void);     /* the GUI resizes the desktop; 0 or E_NOMEM */

void display_apply_saved(void);          /* gui_main, before the screen is set up */

/* Remember w x h for the next start (SETTINGS.INI). */
int  display_save_mode(int w, int h);

#endif
