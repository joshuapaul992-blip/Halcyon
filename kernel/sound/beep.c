/* Halcyon OS - beeps (message boxes, the Command Prompt's BEL).
 *
 * snd_beep(hz, ms): with a sound card, a sine tone (with 5 ms fades, so it
 * does not click) is played through the mixer as an ordinary stream, so it
 * follows the master volume and mute like every other sound. The stream is
 * written in one go and finished; it is kept until the next beep, which
 * closes it (cutting it short if it still plays) and opens its own, so a
 * burst of beeps (BEL characters) never piles up streams. Without a sound
 * card, the PC speaker: PIT channel 2 as a
 * square wave generator (mode 3) gated onto the speaker through port 0x61.
 * A small kernel thread ("beep", started on the first speaker beep) switches
 * it off when the time is up, so the caller never waits. A beep that comes
 * while another one sounds replaces it.
 *
 * Test hook: QEMU fw_cfg opt/halcyon/soundtest = "beep" (instead of the
 * sound test's length): once the desktop is drawn, an 880 Hz beep of 300 ms,
 * and 4 s later (the sound device has been stopped by then and must start
 * again) a 660 Hz beep of 200 ms; boottest.py records and checks both. */
#include "sound/snddev.h"
#include "core/kernel.h"
#include "core/lib.h"
#include "core/io.h"
#include "core/thread.h"
#include "core/kmath.h"

#define PIT_HZ 1193182
#define BEEP_RATE 44100
#define FADE_MS 5

/* ---------------- through the mixer ---------------- */
static snd_stream_t *beep_s;            /* the last beep (finished: plays out, then idles) */

static int mixer_beep(int hz, int ms) {
    int frames = (int)((u64)ms * BEEP_RATE / 1000);
    if (beep_s) { snd_stream_close(beep_s, 0); beep_s = NULL; }
    snd_stream_t *s = snd_stream_open(BEEP_RATE, 1, SND_S16, ms + 20, "beep", 0);
    if (!s) return E_BUSY;
    int fade = BEEP_RATE * FADE_MS / 1000;
    if (fade > frames / 2) fade = frames / 2;
    double step = 2 * K_PI * hz / BEEP_RATE;
    i16 buf[256];
    for (int done = 0; done < frames;) {
        int n = MIN(frames - done, (int)ARRAY_SIZE(buf));
        for (int i = 0; i < n; i++) {
            int k = done + i;
            double v = 12000 * k_sin(step * (k % BEEP_RATE));
            if (k < fade) v = v * k / fade;
            else if (frames - k < fade) v = v * (frames - k) / fade;
            buf[i] = (i16)v;
        }
        if (snd_stream_write(s, buf, (u64)n * 2) != n * 2) break;   /* (the ring holds it all) */
        done += n;
    }
    snd_stream_finish(s);
    beep_s = s;
    return 0;
}

/* ---------------- the PC speaker ---------------- */
static thread_t *spk_thread;
static volatile u64 spk_off_ms;         /* when to switch off; 0: off */
static volatile u32 spk_gen;            /* a new beep started */

static void speaker_off(void) { outb(0x61, (u8)(inb(0x61) & 0xFC)); }

static int spk_changed(void *arg) { return *(volatile u32 *)arg != spk_gen; }

static void spk_main(void *arg) {
    (void)arg;
    for (;;) {
        u32 seen = spk_gen;
        u64 off = spk_off_ms;
        if (off && time_ms() >= off) {
            speaker_off();
            spk_off_ms = off = 0;
        }
        thread_wait(spk_changed, (void *)&seen, off ? time_ms_to_tsc(off) : ~0ull);
    }
}

static int speaker_beep(int hz, int ms) {
    if (!spk_thread) {
        spk_thread = thread_create("beep", spk_main, NULL);
        if (!spk_thread) return E_NOMEM;
    }
    u32 div = (u32)(PIT_HZ / hz);
    div = CLAMP(div, 2u, 65535u);
    outb(0x43, 0xB6);                   /* channel 2, lobyte/hibyte, mode 3 (square wave), binary */
    outb(0x42, (u8)div);
    outb(0x42, (u8)(div >> 8));
    outb(0x61, (u8)((inb(0x61) & 0x0F) | 0x03));    /* gate on, speaker data on */
    spk_off_ms = time_ms() + (u64)ms;
    spk_gen++;
    return 0;
}

int snd_beep(int hz, int ms) {
    hz = CLAMP(hz, 20, 20000);
    ms = CLAMP(ms, 1, 2000);
    if (snd_default()) return mixer_beep(hz, ms);
    return speaker_beep(hz, ms);
}

/* ---------------- test hook ---------------- */
extern volatile int gui_desktop_ready __attribute__((weak));

static int desktop_ready(void *arg) { (void)arg; return !&gui_desktop_ready || gui_desktop_ready; }

static void beep_test(void *arg) {
    (void)arg;
    thread_wait(desktop_ready, NULL, ~0ull);
    const char *how = snd_default() ? snd_default()->name : "PC speaker";
    int r1 = snd_beep(880, 300);
    thread_sleep_ms(4300);
    int r2 = snd_beep(660, 200);
    thread_sleep_ms(1000);
    snddev_t *d = snd_default();
    kprintf("sound: beep test: done through %s (%d, %d), device underruns %u\n", how, r1, r2,
            d ? (unsigned)d->underruns : 0);
    thread_exit();
}

void beep_init(void) {
    char tb[16];
    if (qemu_fw_file("opt/halcyon/soundtest", tb, sizeof tb) && !strcmp(tb, "beep"))
        thread_create("beep test", beep_test, NULL);
}
