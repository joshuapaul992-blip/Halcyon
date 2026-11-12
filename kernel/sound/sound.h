/* Halcyon OS - the user side of sound (milestone 7, see docs/DESIGN-M7.md):
 * system sounds and the sound scheme (sndevent.c), the volume UI's helpers.
 * The system calls are in sndsys.c (ABI: sndabi.h); the device layer, mixer
 * and streams in snddev.h.
 *
 * Nothing here blocks the caller: sounds are loaded and settings saved by
 * the "sndevents" kernel thread, and playing a system sound only queues a
 * static sound for the audio thread. Safe to call without a sound card
 * (then nothing plays). */
#ifndef SOUND_SOUND_H
#define SOUND_SOUND_H
#include "core/types.h"
#include "core/sysabi.h"

/* play a system sound (SNDEV_*) if the scheme has it on; 1 played, 0 not
 * (scheme off, no card, file missing or still loading). force: test button. */
int  sound_event(int ev);
int  sound_event_force(int ev);
int  sound_event_would_play(int ev);
/* shutdown / restart: play SNDEV_SHUTDOWN, then later wait (at most max_ms
 * in all) until it has been heard. Called on the GUI thread on the way out. */
void sound_shutdown_begin(void);
void sound_shutdown_wait(int max_ms);

/* the scheme: system sounds on at all, and each event on or off (applied at
 * once; sound_scheme_save() writes SETTINGS.INI in the background) */
int  sound_scheme_on(void);
void sound_scheme_set_on(int on);
int  sound_event_enabled(int ev);
void sound_event_set_enabled(int ev, int on);
const char *sound_event_label(int ev);          /* "Windows... " style names: "Start Halcyon" */
const char *sound_event_key(int ev);            /* "startup" (sound.event.<key>, sound.file.<key>) */
/* the event's sound file: the scheme's choice or the default
 * (C:/HALCYON/MEDIA/...); set_file(NULL) = the default again (reloaded in
 * the background) */
void sound_event_file(int ev, char *out, int n);
void sound_event_set_file(int ev, const char *path);
int  sound_event_loaded(int ev);                /* 1 loaded, 0 not yet, < 0 the file cannot be used (E_*) */
int  sound_event_ms(int ev);                    /* its length */
void sound_scheme_save(void);
/* sound schemes ("Halcyon", "Halcyon Dusk", "Halcyon Tide"): they pick the default files;
 * set applies at once (the changed sounds reload in the background) and is
 * saved with sound_scheme_save */
int  sound_scheme_count(void);
const char *sound_scheme_name(int i);
int  sound_scheme(void);
void sound_scheme_set(int i);

/* the master volume from the GUI: applied at once, written to SETTINGS.INI
 * by the background thread about a second after the last change */
void sound_volume_set(int percent, int mute);
int  sound_tray_enabled(void);                  /* "Place volume icon in the taskbar" */
void sound_tray_set(int on);                    /* (saved with sound_scheme_save) */
int  sound_startup_pending(void);               /* the startup sound has not finished */

void app_sound(void);                           /* Sounds and Audio Devices (apps/sound.c) */

#endif
