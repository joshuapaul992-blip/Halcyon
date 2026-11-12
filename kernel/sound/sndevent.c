/* Halcyon OS - system sounds and the sound scheme (see sound.h and
 * docs/DESIGN-M7.md).
 *
 * Each event (startup, shutdown, message boxes, balloons, USB devices...)
 * has a WAV file, C:\HALCYON\MEDIA\<NAME>.WAV unless the scheme names
 * another (sound.file.<key>). The "sndevents" kernel thread loads them all
 * into memory at boot (and again when the scheme changes), plays the
 * startup sound once the desktop is up, and writes SETTINGS.INI for the
 * volume and the scheme, so neither the GUI thread nor a program waits for
 * the disk because of sound. Playing an event only queues a static sound
 * (snd_play_static) for the audio thread. Replaced sounds are freed only
 * after they can no longer be playing.
 *
 * Settings: sound.sounds (0 = no system sounds), sound.event.<key> (0 =
 * this one off), sound.file.<key> (another WAV file), sound.tray (the volume
 * icon), and the base layer's sound.volume / sound.mute. */
#include "sound/snddev.h"
#include "sound/sound.h"
#include "core/kernel.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/thread.h"
#include "core/settings.h"
#include "fs/vfs.h"

extern volatile int gui_desktop_ready __attribute__((weak));

#define MAX_FILE (8u << 20)         /* larger WAV files are not used as system sounds */
#define REPEAT_MS 150               /* the same sound again within this is skipped */
#define QUIET_MS 10000              /* no device / balloon sounds this soon after the desktop came up */

static const struct { const char *key, *label, *file; } evinfo[SNDEV_COUNT] = {
    { "startup", "Start Halcyon", "STARTUP.WAV" },
    { "shutdown", "Exit Halcyon", "SHUTDOWN.WAV" },
    { "error", "Error", "ERROR.WAV" },
    { "warning", "Warning", "WARNING.WAV" },
    { "info", "Information", "INFO.WAV" },
    { "question", "Question", "QUESTION.WAV" },
    { "notify", "Notification", "NOTIFY.WAV" },
    { "devadd", "Device Connect", "DEVADD.WAV" },
    { "devremove", "Device Disconnect", "DEVREM.WAV" },
    { "default", "Default Sound", "DEFAULT.WAV" },
};

typedef struct sysnd {
    u8 *buf;                        /* the whole file (the samples point into it) */
    const u8 *pcm;
    u64 frames;
    int rate, channels, bits;
    int state;                      /* 0 not loaded (yet), 1 ready, < 0 unusable (E_*) */
    u64 last_ms;
} sysnd_t;

typedef struct retired { u8 *buf; u64 free_at; struct retired *next; } retired_t;

static sysnd_t evs[SNDEV_COUNT];
static int enabled[SNDEV_COUNT];
static int scheme_on = 1, tray_on = 1;
static retired_t *retired;
static thread_t *worker;
static volatile u32 req_gen, reload_mask;
static int save_pending;
static u64 save_at;
static u64 desktop_ms;              /* when the worker saw the desktop come up */
static u64 last_device_ms;
static u64 shutdown_end, shutdown_t0;
volatile int sound_startup_busy;    /* the startup sound has not finished (snddev.c's test hook waits) */
enum { SS_LOADING, SS_WAIT_DESKTOP, SS_PLAYING, SS_DONE };
static int startup_state = SS_DONE;
static u64 startup_end;

/* ---------------- WAV files ---------------- */
static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }

/* PCM 8/16-bit, mono or stereo (what the mixer plays directly). 0 or E_* */
static int wav_parse(u8 *buf, u32 size, sysnd_t *s) {
    if (size < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) return E_INVAL;
    int have_fmt = 0;
    u32 pos = 12;
    while (pos + 8 <= size) {
        u32 len = rd32(buf + pos + 4);
        const u8 *c = buf + pos + 8;
        u32 avail = size - pos - 8;
        if (!memcmp(buf + pos, "fmt ", 4)) {
            if (len < 16 || len > avail) return E_INVAL;
            u16 tag = rd16(c);
            if (tag == 0xFFFE && len >= 40) tag = rd16(c + 24);     /* WAVE_FORMAT_EXTENSIBLE: the subformat */
            s->channels = rd16(c + 2);
            s->rate = (int)rd32(c + 4);
            s->bits = rd16(c + 14);
            if (tag != 1 || (s->channels != 1 && s->channels != 2) || (s->bits != 8 && s->bits != 16) ||
                s->rate < SND_RATE_MIN || s->rate > SND_RATE_MAX || rd16(c + 12) != s->channels * s->bits / 8)
                return E_NOTSUP;
            have_fmt = 1;
        } else if (!memcmp(buf + pos, "data", 4)) {
            if (!have_fmt) return E_INVAL;
            u32 n = MIN(len, avail);                 /* a truncated file plays what it has */
            s->pcm = c;
            s->frames = n / (u32)(s->channels * s->bits / 8);
            return s->frames ? 0 : E_INVAL;
        }
        if (len > avail) break;
        pos += 8 + len + (len & 1);
    }
    return E_INVAL;
}

static int ms_of(const sysnd_t *s) { return s->state == 1 ? (int)(s->frames * 1000 / (u64)s->rate) : 0; }

static void retire(u8 *buf, u64 after_ms) {
    if (!buf) return;
    retired_t *r = kzalloc(sizeof *r);
    if (!r) return;                 /* (leak rather than free a sound that may be playing) */
    r->buf = buf;
    r->free_at = time_ms() + after_ms + 1000;
    r->next = retired;
    retired = r;
}

static void boot_media(char *out, int n, const char *file) {
    char letter = 'C';
    for (int i = 0; i < vfs_volume_count(); i++)
        if (vfs_volume(i)->is_boot) { letter = vfs_volume(i)->letter; break; }
    snprintf(out, n, "%c:/HALCYON/MEDIA/%s", letter, file);
}

void sound_event_file(int ev, char *out, int n) {
    if (ev < 0 || ev >= SNDEV_COUNT) { if (n) out[0] = 0; return; }
    char key[48];
    snprintf(key, sizeof key, "sound.file.%s", evinfo[ev].key);
    const char *f = settings_get(key, NULL);
    if (f && *f) strlcpy(out, f, n);
    else boot_media(out, n, evinfo[ev].file);
}

/* (the worker thread; blocks on the disk) */
static void load(int ev) {
    char path[VFS_MAX_PATH];
    sound_event_file(ev, path, sizeof path);
    sysnd_t n;
    memset(&n, 0, sizeof n);
    vfs_dirent_t st;
    int r = vfs_stat(path, &st);
    if (!r && (st.is_dir || st.size > MAX_FILE)) r = E_NOTSUP;
    char *data = NULL;
    u32 size = 0;
    if (!r) r = vfs_read_file(path, &data, &size) < 0 ? E_NOENT : 0;
    if (!r) {
        r = wav_parse((u8 *)data, size, &n);
        if (r) { kfree(data); data = NULL; }
    }
    sysnd_t *s = &evs[ev];
    retire(s->buf, (u64)ms_of(s));
    if (r) {
        memset(s, 0, sizeof *s);
        s->state = r;
        kprintf("sound: %s: %s: %s\n", evinfo[ev].key, path, strerror(r));
        return;
    }
    n.buf = (u8 *)data;
    n.state = 1;
    *s = n;
}

/* ---------------- playing ---------------- */
static int play(int ev) {
    sysnd_t *s = &evs[ev];
    if (!snddev_count() || s->state != 1) return 0;
    u64 now = time_ms();
    if (s->last_ms && now - s->last_ms < REPEAT_MS) return 0;
    if (snd_play_static(s->pcm, s->frames, s->rate, s->channels, s->bits == 8 ? SND_U8 : SND_S16, 100,
                        evinfo[ev].label))
        return 0;
    s->last_ms = now;
    if (ev == SNDEV_DEVICE_ADD || ev == SNDEV_DEVICE_REMOVE) last_device_ms = now;
    return 1;
}

int sound_event_would_play(int ev) {
    if (ev < 0 || ev >= SNDEV_COUNT || !scheme_on || !enabled[ev] || evs[ev].state != 1 || !snddev_count()) return 0;
    if (ev == SNDEV_DEVICE_ADD || ev == SNDEV_DEVICE_REMOVE || ev == SNDEV_NOTIFY) {
        u64 now = time_ms();
        if (!desktop_ms || now - desktop_ms < QUIET_MS) return 0;   /* devices found while booting */
        /* a balloon about a device that just made its own sound stays quiet */
        if (ev == SNDEV_NOTIFY && last_device_ms && now - last_device_ms < 2000) return 0;
    }
    return 1;
}

/* Without a sound card, the message box sounds and the default sound are
 * a beep (sound/beep.c's snd_beep: the PC speaker), sent from the worker
 * thread since a beep may take its time. (Declared under another name, so
 * that a different prototype of snd_beep in a header cannot conflict.) */
void sound_beep_hook(int hz, int ms) __asm__("snd_beep") __attribute__((weak));
static volatile int beep_ev = -1;
static void wake(void);

int sound_event(int ev) {
    if (!snddev_count()) {
        if (!sound_beep_hook || !worker || ev < 0 || ev >= SNDEV_COUNT || !scheme_on || !enabled[ev]) return 0;
        if (ev != SNDEV_ERROR && ev != SNDEV_WARNING && ev != SNDEV_INFO && ev != SNDEV_QUESTION && ev != SNDEV_DEFAULT) return 0;
        beep_ev = ev;
        wake();
        return 1;
    }
    return sound_event_would_play(ev) ? play(ev) : 0;
}
int sound_event_force(int ev) { return ev >= 0 && ev < SNDEV_COUNT ? play(ev) : 0; }

void sound_shutdown_begin(void) {
    shutdown_t0 = time_ms();
    shutdown_end = sound_event(SNDEV_SHUTDOWN) ? shutdown_t0 + (u64)ms_of(&evs[SNDEV_SHUTDOWN]) + 150 : 0;
}

void sound_shutdown_wait(int max_ms) {
    u64 end = MIN(shutdown_end, shutdown_t0 + (u64)max_ms);
    while (time_ms() < end) thread_sleep_ms(20);
}

/* ---------------- the scheme ---------------- */
int sound_scheme_on(void) { return scheme_on; }
void sound_scheme_set_on(int on) { scheme_on = on ? 1 : 0; }
int sound_event_enabled(int ev) { return ev >= 0 && ev < SNDEV_COUNT && enabled[ev]; }
void sound_event_set_enabled(int ev, int on) { if (ev >= 0 && ev < SNDEV_COUNT) enabled[ev] = on ? 1 : 0; }
const char *sound_event_label(int ev) { return ev >= 0 && ev < SNDEV_COUNT ? evinfo[ev].label : ""; }
const char *sound_event_key(int ev) { return ev >= 0 && ev < SNDEV_COUNT ? evinfo[ev].key : ""; }
int sound_event_loaded(int ev) { return ev >= 0 && ev < SNDEV_COUNT ? evs[ev].state : E_INVAL; }
int sound_event_ms(int ev) { return ev >= 0 && ev < SNDEV_COUNT ? ms_of(&evs[ev]) : 0; }
int sound_tray_enabled(void) { return tray_on; }
void sound_tray_set(int on) { tray_on = on ? 1 : 0; }
int sound_startup_pending(void) { return sound_startup_busy; }

static void wake(void) { req_gen++; }

void sound_event_set_file(int ev, const char *path) {
    if (ev < 0 || ev >= SNDEV_COUNT) return;
    char key[48];
    snprintf(key, sizeof key, "sound.file.%s", evinfo[ev].key);
    settings_set(key, path && *path ? path : NULL);
    evs[ev].state = evs[ev].state == 1 ? 1 : 0;    /* (the old sound plays until the new one is in) */
    reload_mask |= 1u << ev;
    wake();
}

static void request_save(u64 delay_ms) {
    u64 at = time_ms() + delay_ms;
    if (!save_pending || at > save_at) save_at = at;
    save_pending = 1;
    wake();
}

void sound_scheme_save(void) {
    char key[48];
    settings_set_int("sound.sounds", scheme_on);
    settings_set_int("sound.tray", tray_on);
    for (int i = 0; i < SNDEV_COUNT; i++) {
        snprintf(key, sizeof key, "sound.event.%s", evinfo[i].key);
        settings_set(key, enabled[i] ? NULL : "0");     /* on is the default */
    }
    if (worker) request_save(0);
    else settings_save();
}

void sound_volume_set(int percent, int mute) {
    snd_set_volume(percent, mute, 0);
    settings_set_int("sound.volume", snd_volume());
    settings_set_int("sound.mute", snd_muted());
    if (worker) request_save(1000);             /* debounced: key repeat, slider drags */
    else settings_save();
}

/* ---------------- the worker thread ---------------- */
static u32 seen_gen;
static int worker_wanted(void *a) {
    (void)a;
    return req_gen != seen_gen || (startup_state == SS_WAIT_DESKTOP && &gui_desktop_ready && gui_desktop_ready);
}

static void worker_main(void *arg) {
    (void)arg;
    for (;;) {
        seen_gen = req_gen;
        if (beep_ev >= 0 && sound_beep_hook) {
            int ev = beep_ev;
            beep_ev = -1;
            sound_beep_hook(ev == SNDEV_ERROR ? 440 : ev == SNDEV_WARNING ? 660 : ev == SNDEV_DEFAULT ? 1000 : 880,
                     ev == SNDEV_ERROR ? 250 : ev == SNDEV_WARNING ? 150 : ev == SNDEV_DEFAULT ? 60 : 100);
        }
        u32 m = reload_mask;
        reload_mask = 0;
        for (int i = 0; i < SNDEV_COUNT; i++) if (m & (1u << i)) load(i);
        u64 now = time_ms();
        if (startup_state == SS_LOADING) startup_state = SS_WAIT_DESKTOP;
        if (!desktop_ms && &gui_desktop_ready && gui_desktop_ready) desktop_ms = now;
        if (startup_state == SS_WAIT_DESKTOP && (!&gui_desktop_ready || gui_desktop_ready)) {
            desktop_ms = now;
            if (sound_event(SNDEV_STARTUP)) {
                startup_end = now + (u64)ms_of(&evs[SNDEV_STARTUP]) + 200;
                startup_state = SS_PLAYING;
                kprintf("sound: startup sound (%u ms)\n", (unsigned)ms_of(&evs[SNDEV_STARTUP]));
            } else {
                startup_state = SS_DONE;
                sound_startup_busy = 0;
            }
        }
        if (startup_state == SS_PLAYING && now >= startup_end) {
            startup_state = SS_DONE;
            sound_startup_busy = 0;
        }
        if (save_pending && now >= save_at) {
            save_pending = 0;
            int r = settings_save();
            if (r) kprintf("sound: settings not saved (%s)\n", strerror(r));
            now = time_ms();
        }
        u64 next = ~0ull;
        for (retired_t **pp = &retired; *pp;) {
            retired_t *r = *pp;
            if (now >= r->free_at) { *pp = r->next; kfree(r->buf); kfree(r); continue; }
            next = MIN(next, r->free_at);
            pp = &r->next;
        }
        if (save_pending) next = MIN(next, save_at);
        if (startup_state == SS_PLAYING) next = MIN(next, startup_end);
        if (req_gen != seen_gen) continue;
        thread_wait(worker_wanted, NULL, next == ~0ull ? ~0ull : time_ms_to_tsc(next));
    }
}

/* kmain, after sound_init and SETTINGS.INI (snddev.h) */
void sound_settings_loaded(void) {
    char key[48];
    scheme_on = settings_get_int("sound.sounds", 1) ? 1 : 0;
    tray_on = settings_get_int("sound.tray", 1) ? 1 : 0;
    for (int i = 0; i < SNDEV_COUNT; i++) {
        snprintf(key, sizeof key, "sound.event.%s", evinfo[i].key);
        enabled[i] = settings_get_int(key, 1) ? 1 : 0;
    }
    /* (also without a sound card: settings, and beeps on the PC speaker) */
    if (snddev_count()) {
        reload_mask = (1u << SNDEV_COUNT) - 1;
        sound_startup_busy = 1;
        startup_state = SS_LOADING;
    }
    worker = thread_create("sndevents", worker_main, NULL);
    if (!worker) { sound_startup_busy = 0; startup_state = SS_DONE; kprintf("sound: cannot start the sndevents thread\n"); }
}
