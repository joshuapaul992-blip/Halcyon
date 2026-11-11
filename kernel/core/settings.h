/* Halcyon OS - settings that survive a reboot: C:\HALCYON\SETTINGS.INI on
 * the boot volume, "key=value" lines (UTF-8, '#' or ';' starts a comment).
 * Keys are dotted names such as "display.width" or "keyboard.layouts".
 * The loaders read the "display.*" keys too (they choose the video mode). */
#ifndef SETTINGS_H
#define SETTINGS_H
#include "types.h"

void settings_load(void);                         /* after the boot volume is mounted */
const char *settings_get(const char *key, const char *def);
int  settings_get_int(const char *key, int def);
void settings_set(const char *key, const char *value);   /* NULL removes the key */
void settings_set_int(const char *key, int value);
int  settings_save(void);                         /* 0, or an E_* error (no boot volume...) */

#endif
