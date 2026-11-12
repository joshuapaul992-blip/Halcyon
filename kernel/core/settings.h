/* Halcyon OS - settings that survive a reboot: C:\HALCYON\SETTINGS.INI on
 * the boot volume, "key=value" lines (UTF-8, '#' or ';' starts a comment).
 * Keys are dotted names such as "display.width" or "keyboard.layouts".
 * The loaders read the "display.*" keys too (they choose the video mode),
 * from the first 8 KiB of the file: the file is written with them first.
 *
 * Saving is atomic (see docs/DESIGN-M8-SCREENSAVER-SETTINGS.md): the new
 * file is written as SETTINGS.TMP, flushed and read back, the old one
 * becomes SETTINGS.BAK, then SETTINGS.TMP is renamed to SETTINGS.INI. The
 * last line is an end marker ("# end of settings"): at boot a file without
 * it counts as cut short, and the best complete copy (INI, TMP, BAK) wins. */
#ifndef SETTINGS_H
#define SETTINGS_H
#include "types.h"

#define SETTINGS_KEY_MAX 64       /* bytes with the NUL */
#define SETTINGS_VAL_MAX 600      /* bytes with the NUL (a path fits) */

void settings_load(void);                         /* after the boot volume is mounted */
const char *settings_get(const char *key, const char *def);
int  settings_get_int(const char *key, int def);
void settings_set(const char *key, const char *value);   /* NULL removes the key */
void settings_set_int(const char *key, int value);
int  settings_save(void);                         /* now: 0, or an E_* error (no boot volume...) */
/* save a moment later, coalescing changes that come in quick succession
 * (window moves, slider drags); settings_tick() does it, settings_flush()
 * at once (shutdown, restart) */
void settings_save_later(void);
void settings_tick(void);                         /* from the GUI thread, about once a second */
int  settings_flush(void);                        /* a pending save now; 0 if none */
int  settings_pending(void);

/* settings of a user program (SYS_SETTING_GET / SYS_SETTING_SET): a program
 * may read any key and write the keys of its own name, "paint.*" for
 * PAINT.EXE, "marquee.*" for MARQUEE.SCR. 0, E_PERM, E_INVAL, E_NOSPC. */
int  settings_user_set(const char *prog, const char *key, const char *value);
/* "PAINT.EXE" -> "paint" (the namespace of a program's keys) */
void settings_prog_prefix(const char *prog, char *out, int n);

/* iterate (index 0..): 0 at the end; for System Information and tests */
int  settings_entry(int i, const char **key, const char **value);
int  settings_count(void);
void settings_clear(void);                        /* tests */

#endif
