/* Halcyon OS - the ring of periods of a sound card whose DMA loops over one
 * buffer by itself (ES1370 DAC2, Sound Blaster 16 auto-init DMA).
 *
 * The buffer holds nper periods of period frames (16-bit stereo). start
 * fills all of them and starts the hardware; poll reads the hardware's
 * position (in frames from the start of the buffer) and mixes the next
 * period into every period the hardware has left since the last poll. The
 * period being played is never touched, so nper - 1 periods are always
 * ahead of the hardware. The hardware never stops by itself: if the audio
 * thread was too late, it plays a period it already played (an underrun).
 * That is noticed from the time since the last poll: once that covers the
 * rest of the ring, the hardware has wrapped into stale periods. The ring is
 * then refilled from the hardware's position on. A device that reads ahead
 * in bursts (QEMU's SB16 takes about 40 ms at a time) can reach a stale
 * period before that, so such a device needs a ring well longer than its
 * bursts. Used under the big kernel lock by the audio thread only. */
#ifndef SOUND_LOOPRING_H
#define SOUND_LOOPRING_H
#include "sound/snddev.h"
#include "core/kernel.h"

typedef struct loopring {
    u8 *buf;                        /* nper * period * 4 bytes, DMA memory */
    int nper, period;               /* periods, frames per period */
    int cur;                        /* the period the hardware was in at the last poll */
    u32 pos;                        /* and its position (frames) */
    u64 last_ms;                    /* time of the last poll */
} loopring_t;

static inline i16 *lr_period(loopring_t *r, int i) { return (i16 *)(r->buf + (u64)i * r->period * 4); }

/* fill every period; the hardware will start at frame 0 */
static inline void lr_prime(snddev_t *sd, loopring_t *r) {
    for (int i = 0; i < r->nper; i++) snd_mix(sd, lr_period(r, i), r->period);
    r->cur = 0;
    r->pos = 0;
    r->last_ms = time_ms();
}

/* the hardware is at frame pos: refill what it has finished. Returns the
 * number of periods filled (0: nothing to do). */
static inline int lr_update(snddev_t *sd, loopring_t *r, u32 pos) {
    u32 ring = (u32)(r->nper * r->period);
    u64 now = time_ms();
    if (pos >= ring) pos = ring - 1;
    int cur = (int)(pos / (u32)r->period);
    /* frames the hardware may have played since the last poll, against
     * those that were still fresh then */
    u64 elapsed = (now - r->last_ms) * (u64)sd->rate / 1000;
    u32 fresh = ring - r->pos % (u32)r->period;
    int n = 0;
    if (elapsed >= fresh) {
        sd->underruns++;
        for (int i = 1; i < r->nper; i++, n++) snd_mix(sd, lr_period(r, (cur + i) % r->nper), r->period);
    } else {
        for (int i = r->cur; i != cur; i = (i + 1) % r->nper, n++) snd_mix(sd, lr_period(r, i), r->period);
    }
    r->cur = cur;
    r->pos = pos;
    r->last_ms = now;
    return n;
}

#endif
