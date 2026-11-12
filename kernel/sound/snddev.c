/* Halcyon OS - sound devices, streams, the mixer and the audio thread */
#include "snddev.h"
#include "core/kernel.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/irq.h"
#include "core/thread.h"
#include "core/settings.h"
#include "core/kmath.h"

static snddev_t *devs[SNDDEV_MAX];
static int ndevs, def_dev = -1;
static volatile int kicked;
static u32 wait_gen, gen;
static int master = 80, muted;
static thread_t *audio_thread;

#define IDLE_STOP_MS 3000           /* stop the device after this long with nothing to play */

struct snd_stream {
    int rate, channels, bits;
    i16 *ring;                      /* i16 stereo frames at the stream's rate */
    u32 cap;                        /* frames */
    u64 written, read;              /* frame counters */
    u64 frac;                       /* 32.32 fraction of the next frame to play */
    u64 step;                       /* 32.32 stream frames per device frame */
    int step_rate;                  /* the device rate step was computed for */
    int gain;                       /* 0..65536 */
    int eod;                        /* end of data: no more writes; the last frames play out */
    int autofree;                   /* free it once it has played out (closed with drain) */
    int paused, closed;
    const u8 *ext;                  /* snd_play_static: the sound itself (no ring) */
    u64 ext_frames;
    u64 played, underruns;
    int starved;                    /* ran dry last time (counted once per dry spell) */
    u32 owner;
    char name[32];
    snd_stream_t *next;
};

static snd_stream_t *streams;
static int nstreams;

int snddev_count(void) { return ndevs; }
snddev_t *snddev_get(int i) { return i >= 0 && i < ndevs ? devs[i] : NULL; }
snddev_t *snd_default(void) { return def_dev >= 0 ? devs[def_dev] : NULL; }
u32 snd_generation(void) { return gen; }
void snd_kick(void) { kicked = 1; }

int snddev_register(snddev_t *d) {
    if (ndevs >= SNDDEV_MAX) return E_BUSY;
    snprintf(d->name, sizeof d->name, "snd%d", ndevs);
    if (!d->channels) d->channels = 2;
    if (!d->rate) d->rate = 48000;
    if (!d->period_frames) d->period_frames = d->rate / 100;
    devs[ndevs++] = d;
    if (def_dev < 0) def_dev = ndevs - 1;
    gen++;
    kprintf("sound: %s: %s (%s), %d Hz, %s\n", d->name, d->desc, d->driver, d->rate,
            d->output[0] ? d->output : "output");
    return 0;
}

/* ---------------- master volume ---------------- */
int snd_volume(void) { return master; }
int snd_muted(void) { return muted; }

/* the master volume as a gain 0..65536, squared so that it feels like a
 * fader: -6 dB at 70 %, -12 dB at 50 %, -40 dB at 10 % */
static int master_gain(void) {
    if (muted || master <= 0) return 0;
    return master * master * 65536 / 10000;
}

static void apply_hw_volume(snddev_t *d) {
    if (!d->set_volume) return;
    if (d->hw_volume) d->set_volume(d, master, muted);
    else d->set_volume(d, 100, 0);
}

void snd_set_volume(int percent, int mute, int save) {
    master = CLAMP(percent, 0, 100);
    muted = mute ? 1 : 0;
    for (int i = 0; i < ndevs; i++) if (devs[i]->running) apply_hw_volume(devs[i]);
    gen++;
    if (save) {
        settings_set_int("sound.volume", master);
        settings_set_int("sound.mute", muted);
        settings_save();
    }
}

/* ---------------- streams ---------------- */
static int frame_bytes(const snd_stream_t *s) { return s->channels * (s->bits / 8); }

snd_stream_t *snd_stream_open(int rate, int channels, int bits, int buffer_ms, const char *name, u32 owner_pid) {
    if (rate < 8000 || rate > 192000 || (channels != 1 && channels != 2) || (bits != SND_U8 && bits != SND_S16))
        return NULL;
    if (nstreams >= SND_MAX_STREAMS) return NULL;
    if (!buffer_ms) buffer_ms = 250;
    buffer_ms = CLAMP(buffer_ms, 20, 2000);
    snd_stream_t *s = kzalloc(sizeof *s);
    if (!s) return NULL;
    s->rate = rate; s->channels = channels; s->bits = bits;
    s->cap = (u32)((u64)rate * buffer_ms / 1000) + 2;
    s->ring = kmalloc((u64)s->cap * 4);
    if (!s->ring) { kfree(s); return NULL; }
    s->gain = 65536;
    s->owner = owner_pid;
    strlcpy(s->name, name ? name : "", sizeof s->name);
    s->next = streams;
    streams = s;
    nstreams++;
    return s;
}

static i16 sample_in(const u8 *p, int bits) {
    if (bits == SND_U8) return (i16)((p[0] - 128) << 8);
    return (i16)(p[0] | p[1] << 8);
}

u64 snd_stream_space(snd_stream_t *s) {
    if (!s || s->ext || s->closed || s->eod) return 0;
    u64 used = s->written - s->read;
    u64 free = used + 1 < s->cap ? s->cap - 1 - used : 0;
    return free * (u64)frame_bytes(s);
}

long snd_stream_write(snd_stream_t *s, const void *data, u64 len) {
    if (!s || s->ext || s->closed || s->eod) return E_INVAL;
    int fb = frame_bytes(s);
    u64 n = MIN(len / fb, snd_stream_space(s) / fb);
    const u8 *p = data;
    for (u64 i = 0; i < n; i++, p += fb) {
        i16 *f = &s->ring[((s->written + i) % s->cap) * 2];
        i16 l = sample_in(p, s->bits);
        f[0] = l;
        f[1] = s->channels == 2 ? sample_in(p + fb / 2, s->bits) : l;
    }
    s->written += n;
    if (n) { s->starved = 0; kicked = 1; }
    return (long)(n * fb);
}

u64 snd_stream_queued(snd_stream_t *s) {
    if (!s) return 0;
    return s->ext ? s->ext_frames - s->read : s->written - s->read;
}
void snd_stream_finish(snd_stream_t *s) {
    if (s && !s->eod) { s->eod = 1; kicked = 1; }
}
u64 snd_stream_played(snd_stream_t *s) { return s ? s->played : 0; }
u64 snd_stream_underruns(snd_stream_t *s) { return s ? s->underruns : 0; }
void snd_stream_set_volume(snd_stream_t *s, int percent) {
    if (s) s->gain = CLAMP(percent, 0, 100) * 65536 / 100;
}
void snd_stream_pause(snd_stream_t *s, int paused) {
    if (s) { s->paused = paused ? 1 : 0; if (!paused) kicked = 1; }
}

static void stream_free(snd_stream_t *s) {
    for (snd_stream_t **pp = &streams; *pp; pp = &(*pp)->next)
        if (*pp == s) { *pp = s->next; break; }
    nstreams--;
    kfree(s->ring);
    kfree(s);
}

void snd_stream_close(snd_stream_t *s, int drain) {
    if (!s) return;
    if (drain && snd_stream_queued(s) && !s->paused) { s->eod = 1; s->autofree = 1; s->owner = 0; kicked = 1; return; }
    s->closed = 1;                  /* freed by the mixer side (never while it mixes: same lock) */
    stream_free(s);
}

void snd_release_pid(u32 pid) {
    for (snd_stream_t *s = streams, *n; s; s = n) {
        n = s->next;
        if (s->owner == pid && pid) stream_free(s);
    }
}

int snd_play_static(const void *data, u64 frames, int rate, int channels, int bits, int percent, const char *name) {
    if (!data || !frames || rate < 8000 || rate > 192000 || (channels != 1 && channels != 2) ||
        (bits != SND_U8 && bits != SND_S16))
        return E_INVAL;
    if (nstreams >= SND_MAX_STREAMS) return E_BUSY;
    if (!ndevs) return E_NODEV;
    snd_stream_t *s = kzalloc(sizeof *s);
    if (!s) return E_NOMEM;
    s->rate = rate; s->channels = channels; s->bits = bits;
    s->ext = data; s->ext_frames = frames;
    s->gain = CLAMP(percent, 0, 100) * 65536 / 100;
    s->eod = 1;
    s->autofree = 1;
    strlcpy(s->name, name ? name : "", sizeof s->name);
    s->next = streams;
    streams = s;
    nstreams++;
    kicked = 1;
    return 0;
}

/* frame k of a stream as i16 stereo */
static inline void frame_at(const snd_stream_t *s, u64 k, int *l, int *r) {
    if (s->ext) {
        int fb = frame_bytes(s);
        const u8 *p = s->ext + k * fb;
        *l = sample_in(p, s->bits);
        *r = s->channels == 2 ? sample_in(p + fb / 2, s->bits) : *l;
    } else {
        const i16 *f = &s->ring[(k % s->cap) * 2];
        *l = f[0]; *r = f[1];
    }
}

/* ---------------- the mixer ---------------- */
static i32 *mixbuf;
static int mixcap;

static int streams_playing(void) {
    for (snd_stream_t *s = streams; s; s = s->next)
        if (!s->paused && snd_stream_queued(s)) return 1;
    return 0;
}

void snd_mix(snddev_t *d, i16 *out, int frames) {
    int ch = d->channels;
    if (frames <= 0) return;
    if (frames > mixcap) {
        kfree(mixbuf);
        mixbuf = kmalloc((u64)frames * 2 * sizeof(i32));
        mixcap = mixbuf ? frames : 0;
        if (!mixbuf) { memset(out, 0, (u64)frames * ch * 2); return; }
    }
    memset(mixbuf, 0, (u64)frames * 2 * sizeof(i32));
    int any = 0;
    for (snd_stream_t *s = streams, *n; s; s = n) {
        n = s->next;
        if (s->paused) continue;
        if (s->step_rate != d->rate) {
            s->step = ((u64)s->rate << 32) / (u64)d->rate;
            s->step_rate = d->rate;
        }
        u64 avail = snd_stream_queued(s);
        u64 pos = s->frac;
        int i = 0;
        for (; i < frames; i++) {
            u64 idx = pos >> 32;
            if (idx >= avail) break;
            int l0, r0, l1, r1;
            frame_at(s, s->read + idx, &l0, &r0);
            if (idx + 1 < avail) frame_at(s, s->read + idx + 1, &l1, &r1);
            else if (s->eod) { l1 = l0; r1 = r0; }
            else break;                         /* wait for the next frame to interpolate */
            i32 f = (i32)((pos >> 16) & 0xFFFF);
            i32 l = l0 + (((l1 - l0) * f) >> 16), r = r0 + (((r1 - r0) * f) >> 16);
            mixbuf[2 * i] += (l * s->gain) >> 16;
            mixbuf[2 * i + 1] += (r * s->gain) >> 16;
            pos += s->step;
        }
        if (i) any = 1;
        u64 used = MIN(pos >> 32, avail);
        s->read += used;
        s->frac = pos - (used << 32);
        s->played += used;
        if (i < frames && !s->eod) {
            if (!s->starved && s->played) { s->underruns++; s->starved = 1; }
        }
        if (s->autofree && !snd_stream_queued(s)) stream_free(s);
    }
    int g = d->hw_volume && d->set_volume ? 65536 : master_gain();
    for (int i = 0; i < frames; i++) {
        i32 l = any ? (i32)(((i64)mixbuf[2 * i] * g) >> 16) : 0;
        i32 r = any ? (i32)(((i64)mixbuf[2 * i + 1] * g) >> 16) : 0;
        l = CLAMP(l, -32768, 32767);
        r = CLAMP(r, -32768, 32767);
        out[i * ch] = (i16)l;
        if (ch > 1) out[i * ch + 1] = (i16)r;
        for (int c = 2; c < ch; c++) out[i * ch + c] = 0;
    }
    d->frames += frames;
}

/* ---------------- test hook: QEMU fw_cfg "opt/halcyon/soundtest" ----------------
 * The value is a length in ms (empty: 2000). Once the desktop is drawn, the
 * audio thread plays a 440 Hz sine at 44100 Hz (resampled to the device
 * rate) through an ordinary stream for that long; when it has drained, a
 * 1 kHz triangle at 22050 Hz, mono, as a static sound for 0.25 s; then it
 * logs a summary. boottest.py records QEMU's output to a WAV file and checks
 * both. Never active on real hardware (fw_cfg exists only under QEMU). */
extern volatile int gui_desktop_ready __attribute__((weak));
/* the startup sound (sndevent.c) plays first: the test waits until it has
 * finished and nothing else plays, so the recording holds it and then the
 * tone and the beep, apart (at most 20 s, in case it never ends) */
extern volatile int sound_startup_busy __attribute__((weak));
enum { TS_OFF, TS_WAIT, TS_TONE, TS_BEEP, TS_DONE };
static int test_state;
static u32 test_ms;
static snd_stream_t *test_s;
static u64 test_t0, test_frames, test_sent, test_under, test_tone_ms;
static i16 *test_beep;

static void test_fill(void) {
    i16 buf[512 * 2];
    while (test_sent < test_frames) {
        u64 space = snd_stream_space(test_s) / 4;
        if (!space) break;
        int n = (int)MIN(MIN(space, (u64)512), test_frames - test_sent);
        for (int i = 0; i < n; i++) {
            u64 k = test_sent + i;
            u64 cyc = k * 440 % 44100;                  /* phase in 1/44100 turns, exact */
            int v = (int)(16000 * k_sin(2 * K_PI * (double)cyc / 44100.0));
            buf[2 * i] = (i16)v;
            buf[2 * i + 1] = (i16)v;
        }
        snd_stream_write(test_s, buf, (u64)n * 4);
        test_sent += n;
    }
}

static void test_poll(void) {
    switch (test_state) {
    case TS_WAIT:
        if (&gui_desktop_ready && !gui_desktop_ready) return;
        if (!test_t0) test_t0 = time_ms();
        if (time_ms() - test_t0 < 20000 && ((&sound_startup_busy && sound_startup_busy) || streams_playing())) return;
        test_s = snd_stream_open(44100, 2, SND_S16, 200, "sound test", 0);
        if (!test_s) { kprintf("sound: test: cannot open a stream\n"); test_state = TS_DONE; return; }
        test_frames = (u64)test_ms * 44100 / 1000;
        test_t0 = time_ms();
        kprintf("sound: test: 440 Hz for %u ms\n", test_ms);
        test_state = TS_TONE;
        /* fall through */
    case TS_TONE:
        test_fill();
        if (test_sent < test_frames) return;
        snd_stream_finish(test_s);
        if (snd_stream_queued(test_s)) return;
        test_under = snd_stream_underruns(test_s);
        test_tone_ms = time_ms() - test_t0;
        snd_stream_close(test_s, 0);
        test_s = NULL;
        test_beep = kmalloc(22050 / 4 * 2);
        if (test_beep) {
            for (int i = 0; i < 22050 / 4; i++) {
                u32 ph = (u32)(((u64)i * 1000 * 65536) / 22050) & 0xFFFF;
                int tri = ph < 32768 ? (int)ph - 16384 : 49152 - (int)ph;      /* triangle, +-16384 */
                test_beep[i] = (i16)(tri / 2);
            }
            snd_play_static(test_beep, 22050 / 4, 22050, 1, SND_S16, 100, "sound test beep");
        }
        test_t0 = time_ms();
        test_state = TS_BEEP;
        return;
    case TS_BEEP:
        if (streams_playing() && time_ms() - test_t0 < 5000) return;
        {
            snddev_t *d = snd_default();
            kprintf("sound: test: done: tone played in %u ms, stream underruns %u, device %s underruns %u, irq %s\n",
                    (unsigned)test_tone_ms, (unsigned)test_under, d ? d->name : "-",
                    d ? (unsigned)d->underruns : 0, d ? (d->irq_live ? "live" : "polled") : "-");
        }
        test_state = TS_DONE;
        return;
    }
}

/* ---------------- the audio thread ---------------- */
static int audio_wanted(void *arg) { (void)arg; return kicked || irq_generation() != wait_gen; }

static void audio_main(void *arg) {
    (void)arg;
    char tb[16];
    if (qemu_fw_file("opt/halcyon/soundtest", tb, sizeof tb)) {
        test_ms = tb[0] ? (u32)strtol(tb, NULL, 0) : 2000;
        test_ms = MIN(test_ms, 60000u);
        if (test_ms) test_state = TS_WAIT;
    }
    for (;;) {
        wait_gen = irq_generation();
        kicked = 0;
        u64 now = time_ms();
        test_poll();
        snddev_t *d = snd_default();
        int playing = streams_playing();
        u64 next = now + 1000;
        if (d) {
            if (playing && !d->running) {
                apply_hw_volume(d);
                int r = d->start(d);
                if (r) {
                    kprintf("sound: %s: cannot start (%s)\n", d->name, strerror(r));
                    /* drop what was queued rather than spin */
                    for (snd_stream_t *s = streams, *n; s; s = n) { n = s->next; if (s->autofree) stream_free(s); }
                } else {
                    d->running = 1;
                    d->started_ms = now;
                }
            }
            if (d->running) {
                d->poll(d);
                if (playing || streams_playing()) d->idle_since = 0;
                else if (!d->idle_since) d->idle_since = now;
                else if (now - d->idle_since > IDLE_STOP_MS) {
                    d->stop(d);
                    d->running = 0;
                    d->idle_since = 0;
                }
                if (d->running) {
                    int period_ms = MAX(1, d->period_frames * 1000 / d->rate);
                    /* with a working interrupt the period interrupt wakes us;
                     * the timer is a backstop. Polled: twice a period. */
                    next = now + (d->irq_live ? (u64)period_ms * 2 : (u64)MAX(1, period_ms / 2));
                }
            }
        }
        if (test_state != TS_OFF && test_state != TS_DONE) next = MIN(next, now + 5);
        if (kicked) continue;
        thread_wait(audio_wanted, NULL, time_ms_to_tsc(next));
    }
}

void sound_init(void) {
    master = CLAMP(settings_get_int("sound.volume", 80), 0, 100);
    muted = settings_get_int("sound.mute", 0) ? 1 : 0;
    if (hda_init) hda_init();
    if (ac97_init) ac97_init();
    if (es1370_init) es1370_init();
    if (virtio_snd_init) virtio_snd_init();
    if (sb16_init) sb16_init();
    if (!ndevs) { kprintf("sound: no sound card\n"); return; }
    audio_thread = thread_create("audio", audio_main, NULL);
    if (!audio_thread) kprintf("sound: cannot start the audio thread\n");
}
