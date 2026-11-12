/* Halcyon OS - sound: the part of the system call interface (sysabi.h)
 * owned by kernel/sound/, system calls 140-151. See docs/DESIGN-M7.md.
 *
 * A program plays sound through streams: open one with its format (8000 to
 * 192000 frames a second, 1 or 2 channels, 8-bit unsigned or 16-bit signed
 * little-endian samples, interleaved), write() whole frames into it, and the
 * kernel's mixer plays it on the default sound card together with every
 * other stream, converting the rate and applying the stream's volume and the
 * master volume. Streams are handles (H_SOUND): close() and poll() work on
 * them too. Errors are Halcyon's negative E_* codes. */
#ifndef SNDABI_H
#define SNDABI_H

enum {
    SYS_SND_OPEN = 140, /* (rate, channels, bits, buffer_ms (0 = 250), flags SND_NONBLOCK) -> handle */
    SYS_SND_WRITE,      /* (h, buf, len (whole frames), flags SNDW_*) -> bytes queued */
    SYS_SND_DRAIN,      /* (h, timeout_ms (< 0: no limit)) -> 0, E_TIMEOUT: wait until all is played */
    SYS_SND_CTL,        /* (h, SNDCTL_*, value) -> 0 */
    SYS_SND_STATUS,     /* (h, hal_sndstatus_t *) -> 0 */
    SYS_SND_CLOSE,      /* (h, SNDCLOSE_*) -> 0 */
    SYS_SND_MASTER,     /* (volume 0..100 or -1 = keep, mute 0/1 or -1 = keep, flags SNDVOL_SAVE)
                           -> volume | SND_MUTED if muted (the values after the call) */
    SYS_SND_EVENT,      /* (SNDEV_*, flags SNDEVF_*) -> 1 played, 0 silenced by the sound scheme */
    SYS_SND_DEVINFO,    /* (index, hal_snddev_t *) -> 0, E_NOENT past the last sound card */
    SYS_SND_INFO,       /* (hal_sndinfo_t *) -> 0: system-wide state and limits */
    /* 150-151: free (recording later) */
};

/* SYS_SND_OPEN */
#define SND_NONBLOCK    0x01    /* write() returns what fits (E_AGAIN if nothing) instead of waiting */
#define SND_RATE_MIN    8000
#define SND_RATE_MAX    192000
#define SND_BUFFER_MIN  20      /* ms; buffer_ms is clamped to this range */
#define SND_BUFFER_MAX  2000
#define SND_PROC_STREAMS 8      /* open streams per program (E_BUSY beyond) */
#define SND_USER_STREAMS 24     /* open streams of all programs together (the rest is kept for system sounds) */

/* SYS_SND_WRITE flags */
#define SNDW_NONBLOCK   0x01    /* this call does not wait */

/* SYS_SND_CTL operations */
enum { SNDCTL_PAUSE = 1,        /* value 1 pauses, 0 resumes (the stream keeps its data) */
       SNDCTL_VOLUME,           /* value 0..100: the stream's own volume (100 = as written) */
       SNDCTL_NONBLOCK,         /* value 1: writes stop waiting (as SND_NONBLOCK) */
       SNDCTL_DISCARD };        /* drop what is queued and not yet played (a seek) */

/* SYS_SND_CLOSE */
#define SNDCLOSE_NOW    0       /* stop at once, drop what is queued */
#define SNDCLOSE_DRAIN  1       /* let what is queued play out, without waiting (close() does this) */
#define SNDCLOSE_WAIT   2       /* play out what is queued and wait for it (killable) */

/* SYS_SND_MASTER */
#define SNDVOL_SAVE     0x01    /* also keep it in SETTINGS.INI (sound.volume, sound.mute) */
#define SND_MUTED       0x100

/* system sounds (SYS_SND_EVENT), C:\HALCYON\MEDIA\*.WAV unless the sound
 * scheme names another file */
enum { SNDEV_STARTUP = 0,       /* the desktop is ready */
       SNDEV_SHUTDOWN,          /* turning off or restarting */
       SNDEV_ERROR,             /* message boxes with the error icon */
       SNDEV_WARNING,
       SNDEV_INFO,
       SNDEV_QUESTION,
       SNDEV_NOTIFY,            /* a notification balloon */
       SNDEV_DEVICE_ADD,        /* a USB device was connected */
       SNDEV_DEVICE_REMOVE,     /* ... removed */
       SNDEV_DEFAULT,           /* the default sound (volume feedback) */
       SNDEV_COUNT };
#define SNDEVF_FORCE    0x01    /* play even if the scheme has it (or all sounds) off */
#define SNDEVF_QUERY    0x02    /* do not play: 1 if it would play, 0 if not */

/* SYS_SND_STATUS */
typedef struct hal_sndstatus {
    unsigned long long written;         /* frames accepted since open */
    unsigned long long played;          /* frames played since open */
    unsigned long long queued;          /* frames waiting to be played */
    unsigned long long underruns;       /* times the stream ran dry while playing */
    unsigned int rate, channels, bits;  /* as opened */
    unsigned int buffer_frames;         /* the stream's buffer */
    unsigned int space;                 /* bytes a write would take now without waiting */
    int volume;                         /* SNDCTL_VOLUME */
    int paused, nonblock;
    int latency_ms;                     /* until a frame written now is heard (estimate) */
    int period_ms;                      /* the device's period */
} hal_sndstatus_t;

/* SYS_SND_DEVINFO: one sound card */
typedef struct hal_snddev {
    char name[8];                       /* "snd0" */
    char desc[64];                      /* "Intel 82801AA AC'97 Audio" */
    char driver[16];                    /* "ac97" */
    char output[32];                    /* "Line out", "Speakers"... */
    int rate, channels, period_frames;  /* what the device plays */
    int running;                        /* playing now (it stops after a few seconds of silence) */
    int is_default;                     /* the device streams play on */
    int irq_live;                       /* its interrupt works (else it is polled) */
    unsigned long long frames;          /* frames played since boot */
    unsigned long long underruns;       /* the hardware ran dry */
} hal_snddev_t;

/* SYS_SND_INFO */
typedef struct hal_sndinfo {
    int devices;                        /* sound cards */
    int volume, muted;                  /* the master volume */
    int sounds_on;                      /* the sound scheme plays system sounds */
    int streams, max_streams;           /* programs' open streams, system-wide (SND_USER_STREAMS) */
    int proc_streams, max_proc_streams; /* the caller's (SND_PROC_STREAMS) */
    int startup_pending;                /* the startup sound has not finished yet */
    unsigned int generation;            /* changes with the volume, mute or the devices */
} hal_sndinfo_t;

#endif
