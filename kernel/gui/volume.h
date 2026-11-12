/* Halcyon OS - the volume UI (gui/volume.c): the speaker in the taskbar's
 * tray with its popup slider and tooltip, the multimedia volume keys and
 * their on-screen indicator, and a trackbar control shared with Sounds and
 * Audio Devices (apps/sound.c). */
#ifndef GUI_VOLUME_H
#define GUI_VOLUME_H
#include "wm.h"

/* the tray icon (desktop.c draws the tray; x, y: the icon's top left) */
int  volume_tray_visible(void);                 /* a sound card, and the icon is wanted */
void volume_tray_paint(gc_t *g, int x, int y, int hover);
void volume_tray_click(rect_t icon, int dbl);   /* screen rectangle of the icon */
void volume_tray_hover(rect_t icon, int on);    /* the pointer rests on it: tooltip */
void volume_tick(void);                         /* shell_tick: state changes, the indicator fading */
/* wm: Mute / Volume Up / Volume Down (HID 0x7F-0x81): 1 if used (also on release) */
int  volume_key(u8 key, int pressed);
/* wm: drawn over everything (the on-screen indicator, the tray tooltip) */
void gui_overlay_paint(gc_t *g, rect_t clip);

/* a trackbar: value 0..100 along r (vertical: 100 at the top) */
void slider_paint(gc_t *g, rect_t r, int vertical, int value, int hot, int disabled);
int  slider_value_at(rect_t r, int vertical, int x, int y);
rect_t slider_thumb(rect_t r, int vertical, int value);

#endif
