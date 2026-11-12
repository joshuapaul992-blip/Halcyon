/* Halcyon OS - sound (milestone 7): output devices, the mixer, streams.
 *
 * A sound card driver fills in a snddev_t and registers it. The "audio"
 * kernel thread owns all sound work. While something plays, it starts the
 * default device and calls its poll() whenever the device may need data; the
 * driver keeps a ring of a few periods ahead of the hardware and fills each
 * free period with snd_mix(), which mixes every playing stream into it. When
 * nothing has played for a while the device is stopped again, so an idle
 * system takes no sound interrupts at all. Interrupts are only wake-ups (as
 * everywhere in Halcyon, DESIGN-M2): drivers attach theirs with irq_attach()
 * and must work polled too; the thread then wakes every half period.
 *
 * Device format: signed 16-bit little-endian, interleaved, d->channels
 * channels (2 for now) at d->rate frames a second (48000 where the hardware
 * allows). Streams have their own rate (8000..192000), 1 or 2 channels and
 * 8-bit unsigned or 16-bit signed samples; the mixer converts and resamples
 * (linear interpolation) while mixing, applies each stream's volume and the
 * master volume, and clips.
 *
 * Everything runs under the big kernel lock. */
#ifndef SOUND_SNDDEV_H
#define SOUND_SNDDEV_H
#include "core/types.h"

struct pci_dev;
struct irq_handle;

#define SNDDEV_MAX 8
#define SND_MAX_STREAMS 32

typedef struct snddev snddev_t;
struct snddev {
    char name[8];                   /* "snd0" (set by snddev_register) */
    char desc[64];                  /* "Intel 82801AA AC'97 Audio" */
    const char *driver;             /* "ac97" */
    char output[32];                /* where the sound goes: "Line out", "Speakers", "Headphones" */
    int rate;                       /* frames a second the device plays (driver sets it) */
    int channels;                   /* 2 */
    int period_frames;              /* frames per period (about 10 ms; driver sets it) */
    /* start playing: the driver primes its ring (snd_mix for each period it
     * fills) and starts the DMA. 0 or E_*. */
    int  (*start)(snddev_t *d);
    /* stop the DMA (and leave the device quiet) */
    void (*stop)(snddev_t *d);
    /* refill: for every period the hardware has finished, snd_mix() the next
     * one into it; notice underruns (the hardware ran out) and recover from
     * them. Called by the audio thread only, while started. */
    void (*poll)(snddev_t *d);
    /* hardware output volume 0..100 and mute (may be NULL: the mixer then
     * scales the samples). Drivers that have it still get 100 here unless
     * they do the master volume themselves (hw_volume = 1). */
    int  (*set_volume)(snddev_t *d, int percent, int mute);
    int hw_volume;                  /* the driver applies the master volume in set_volume */
    void *priv;                     /* driver */
    int irq_live;                   /* the driver's interrupt works (else the thread polls) */
    struct pci_dev *pci;            /* System Information; may be NULL */
    struct irq_handle *irq;
    /* kept by the core */
    int running;
    u64 frames;                     /* frames mixed for this device */
    u64 underruns;                  /* the driver counts these (the hardware ran dry) */
    u64 started_ms, idle_since;
};

int  snddev_register(snddev_t *d);          /* names it sndN; 0 or E_BUSY */
int  snddev_count(void);
snddev_t *snddev_get(int i);
snddev_t *snd_default(void);                /* the device that plays, or NULL */
/* driver -> core: mix the next `frames` frames for d into out (d->channels
 * interleaved i16); silence when nothing plays */
void snd_mix(snddev_t *d, i16 *out, int frames);
void snd_kick(void);                        /* wake the audio thread (data queued, settings changed) */
void sound_init(void);                      /* kmain: probe drivers, start the thread */

/* ---------------- streams (kernel API; the system calls wrap it) ---------------- */
typedef struct snd_stream snd_stream_t;
enum { SND_U8 = 8, SND_S16 = 16 };

/* a stream of rate frames/s, 1 or 2 channels, SND_U8 or SND_S16 samples,
 * buffering about buffer_ms (0 = 250 ms; 20..2000) */
snd_stream_t *snd_stream_open(int rate, int channels, int bits, int buffer_ms, const char *name, u32 owner_pid);
/* copy up to len bytes of whole frames in; returns the bytes taken (0 when
 * full). Never blocks. */
long snd_stream_write(snd_stream_t *s, const void *data, u64 len);
u64  snd_stream_space(snd_stream_t *s);     /* bytes that would be taken now */
u64  snd_stream_queued(snd_stream_t *s);    /* frames still to play */
/* no more data will come: the last frames play out (the mixer otherwise
 * keeps one frame back to interpolate with the next); writes are refused */
void snd_stream_finish(snd_stream_t *s);
/* end the stream: drain = play what is queued first (the stream frees
 * itself afterwards), else stop at once */
void snd_stream_close(snd_stream_t *s, int drain);
void snd_stream_set_volume(snd_stream_t *s, int percent);    /* 0..100 (100 = as recorded) */
void snd_stream_pause(snd_stream_t *s, int paused);
u64  snd_stream_played(snd_stream_t *s);    /* frames of the stream played so far */
u64  snd_stream_underruns(snd_stream_t *s); /* times it ran dry while playing */
/* play a sound held in memory that stays valid until it ends (system
 * sounds); fire and forget. 0 or E_*. */
int  snd_play_static(const void *data, u64 frames, int rate, int channels, int bits, int percent, const char *name);
void snd_release_pid(u32 pid);              /* a program ended: close its streams at once */

/* master volume 0..100 and mute (sound.volume / sound.mute in SETTINGS.INI) */
int  snd_volume(void);
int  snd_muted(void);
void snd_set_volume(int percent, int mute, int save);
u32  snd_generation(void);                  /* changes with the volume, mute or the devices */

/* a beep of hz (20..20000) for ms (1..2000): a tone through the mixer when
 * there is a sound card, else the PC speaker; returns at once (0 or E_*) */
int  snd_beep(int hz, int ms);
void beep_init(void) __attribute__((weak));    /* sound_init: the beep test hook */

/* drivers (weak: probe the PCI bus, register what they find) */
void hda_init(void) __attribute__((weak));
void ac97_init(void) __attribute__((weak));
void es1370_init(void) __attribute__((weak));
void virtio_snd_init(void) __attribute__((weak));
void sb16_init(void) __attribute__((weak));

/* the user-side sound layer (system calls, system sounds; weak) */
void sound_settings_loaded(void) __attribute__((weak));   /* kmain: SETTINGS.INI has been read */

#endif
