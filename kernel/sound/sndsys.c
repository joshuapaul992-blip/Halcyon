/* Halcyon OS - the sound system calls (140-151, ABI in sndabi.h; see
 * docs/DESIGN-M7.md): programs' streams as H_SOUND handles over the
 * kernel's streams (snddev.h), the master volume, system sounds, device
 * information, and poll() support.
 *
 * A handle holds a ustream_t, which owns one kernel stream. Draining and
 * discarding replace the kernel stream with a fresh one (a kernel stream
 * that was told "no more data" refuses writes for good), so a program's
 * stream stays usable after drain() and a seek. Waits (a full buffer,
 * drain) use proc_wait on a generation that moves whenever the mixer has
 * run or a stream changed, so they end when the program is being ended. A
 * ustream is reference counted: another thread of the program may close
 * the handle while one waits in it. */
#include "sound/snddev.h"
#include "sound/sound.h"
#include "core/kernel.h"
#include "core/lib.h"
#include "core/mm.h"
#include "core/proc.h"

typedef struct ustream {
    snd_stream_t *s;                /* NULL after a failed replacement (writes then retry) */
    u32 pid;
    char name[32];
    int rate, channels, bits, buffer_ms, fb;
    int volume, paused, nonblock;
    int draining;                   /* s was told "no more data": replace it once it has played out */
    int refs, closed;
    u64 written;                    /* frames accepted */
    u64 base_played, base_under;    /* of the kernel streams it had before */
    struct ustream *next;
} ustream_t;

static ustream_t *ustreams;
static int nuser;                   /* open (not closed) ustreams */
static u32 sgen;                    /* bumped by every change made here */
static u8 kbuf[16384];              /* copy buffer (used between blocking points only) */

/* moves whenever a stream may have room or may have played out: each mix of
 * a running device, and every change made here */
u32 sound_wait_gen(void) {
    u32 g = sgen;
    for (int i = 0; i < snddev_count(); i++) g += (u32)snddev_get(i)->frames;
    return g;
}

typedef struct { u32 g; } gwait_t;
static int gen_moved(void *a) { return sound_wait_gen() != ((gwait_t *)a)->g; }

/* wait for the generation to move: 0, E_TIMEOUT, E_CANCELED */
static int wait_change(u64 deadline_tsc) {
    gwait_t w = { sound_wait_gen() };
    int r = proc_wait(gen_moved, &w, deadline_tsc);
    if (r < 0) return r;
    return r ? 0 : E_TIMEOUT;
}

static int proc_count(u32 pid) {
    int n = 0;
    for (ustream_t *u = ustreams; u; u = u->next) if (!u->closed && u->pid == pid) n++;
    return n;
}

static void unref(ustream_t *u) {
    if (--u->refs > 0) return;
    for (ustream_t **pp = &ustreams; *pp; pp = &(*pp)->next)
        if (*pp == u) { *pp = u->next; break; }
    kfree(u);
}

/* a fresh kernel stream in place of the current one (keeps volume and pause) */
static int replace(ustream_t *u) {
    if (u->s) {
        u->base_played += snd_stream_played(u->s);
        u->base_under += snd_stream_underruns(u->s);
        snd_stream_close(u->s, 0);
        u->s = NULL;
    }
    u->draining = 0;
    sgen++;
    u->s = snd_stream_open(u->rate, u->channels, u->bits, u->buffer_ms, u->name, u->pid);
    if (!u->s) return E_BUSY;
    snd_stream_set_volume(u->s, u->volume);
    snd_stream_pause(u->s, u->paused);
    return 0;
}

/* the stream ready for writing: replaced if a drain has finished or a
 * replacement failed. 0, E_AGAIN (still draining) or E_BUSY. */
static int ready(ustream_t *u) {
    if (!u->s) return replace(u);
    if (u->draining) return snd_stream_queued(u->s) ? E_AGAIN : replace(u);
    return 0;
}

static u64 space_bytes(ustream_t *u) {
    if (ready(u)) return 0;
    return snd_stream_space(u->s);
}

/* the program's stream behind a handle */
static ustream_t *get(proc_t *p, int h) {
    ustream_t *u = handle_get(p, h, H_SOUND);
    return u && !u->closed ? u : NULL;
}

static void stream_end(proc_t *p, int h, ustream_t *u, int how) {
    handle_free(p, h);
    if (u->s) snd_stream_close(u->s, how == SNDCLOSE_DRAIN);
    u->s = NULL;
    u->closed = 1;
    nuser--;
    sgen++;
    unref(u);
}

/* ---------------- the calls ---------------- */

static long sys_open(proc_t *p, long rate, long ch, long bits, long buffer_ms, u64 flags) {
    if ((flags & ~(u64)SND_NONBLOCK) || rate < SND_RATE_MIN || rate > SND_RATE_MAX || (ch != 1 && ch != 2) ||
        (bits != 8 && bits != 16) || buffer_ms < 0 || buffer_ms > 100000)
        return E_INVAL;
    if (!snddev_count()) return E_NODEV;
    if (proc_count(p->pid) >= SND_PROC_STREAMS || nuser >= SND_USER_STREAMS) return E_BUSY;
    ustream_t *u = kzalloc(sizeof *u);
    if (!u) return E_NOMEM;
    u->pid = p->pid;
    strlcpy(u->name, p->name, sizeof u->name);
    u->rate = (int)rate; u->channels = (int)ch; u->bits = (int)bits;
    u->buffer_ms = buffer_ms ? CLAMP((int)buffer_ms, SND_BUFFER_MIN, SND_BUFFER_MAX) : 250;
    u->fb = u->channels * u->bits / 8;
    u->volume = 100;
    u->nonblock = (flags & SND_NONBLOCK) != 0;
    u->s = snd_stream_open(u->rate, u->channels, u->bits, u->buffer_ms, u->name, u->pid);
    if (!u->s) { kfree(u); return E_BUSY; }
    int h = handle_alloc(p, H_SOUND, u);
    if (h < 0) { snd_stream_close(u->s, 0); kfree(u); return h; }
    u->refs = 1;
    u->next = ustreams;
    ustreams = u;
    nuser++;
    sgen++;
    return h;
}

static long us_write(ustream_t *u, u64 ubuf, u64 len, int nb) {
    if (len % (u64)u->fb) return E_INVAL;
    u->refs++;
    u64 done = 0;
    long r = 0;
    while (done < len) {
        if (u->closed) { r = E_CANCELED; break; }
        int k = ready(u);
        u64 space = k ? 0 : snd_stream_space(u->s);
        if (k == E_BUSY && nb) { r = k; break; }
        if (!space) {
            if (nb) { r = E_AGAIN; break; }
            r = wait_change(~0ull);           /* room, the drain's end, a close, or the program ends */
            if (r) break;
            continue;
        }
        u64 n = MIN(MIN(space, len - done), (u64)sizeof kbuf);
        n -= n % (u64)u->fb;
        if (copyin(kbuf, ubuf + done, n)) { r = E_INVAL; break; }
        long w = snd_stream_write(u->s, kbuf, n);
        if (w <= 0) { r = w ? w : E_AGAIN; break; }
        done += (u64)w;
        u->written += (u64)w / (u64)u->fb;
    }
    unref(u);
    return done ? (long)done : r;
}

static long us_drain(ustream_t *u, long timeout_ms) {
    if (!u->s) return 0;
    u->refs++;
    snd_stream_finish(u->s);
    u->draining = 1;
    sgen++;
    u64 dl = timeout_ms < 0 ? ~0ull : time_ms_to_tsc(time_ms() + (u64)MIN(timeout_ms, 86400000L));
    long r = 0;
    while (!u->closed && u->s && u->draining && snd_stream_queued(u->s)) {
        r = wait_change(dl);
        if (r) break;
    }
    if (!r && !u->closed && u->draining) replace(u);    /* empty again and writable */
    if (u->closed && !r) r = E_CANCELED;
    unref(u);
    return r;
}

static long sys_ctl(ustream_t *u, long op, long v) {
    switch (op) {
    case SNDCTL_PAUSE:
        if (v != 0 && v != 1) return E_INVAL;
        u->paused = (int)v;
        if (u->s) snd_stream_pause(u->s, u->paused);
        break;
    case SNDCTL_VOLUME:
        if (v < 0 || v > 100) return E_INVAL;
        u->volume = (int)v;
        if (u->s) snd_stream_set_volume(u->s, u->volume);
        break;
    case SNDCTL_NONBLOCK:
        if (v != 0 && v != 1) return E_INVAL;
        u->nonblock = (int)v;
        break;
    case SNDCTL_DISCARD:
        if (replace(u)) return E_BUSY;
        break;
    default:
        return E_INVAL;
    }
    sgen++;
    return 0;
}

static int period_ms(void) {
    snddev_t *d = snd_default();
    return d && d->rate ? MAX(1, d->period_frames * 1000 / d->rate) : 10;
}

static long sys_status(ustream_t *u, u64 ust) {
    hal_sndstatus_t st;
    memset(&st, 0, sizeof st);
    if (u->draining) ready(u);               /* a finished drain counts as empty */
    st.written = u->written;
    st.played = u->base_played + (u->s ? snd_stream_played(u->s) : 0);
    st.queued = u->s ? snd_stream_queued(u->s) : 0;
    st.underruns = u->base_under + (u->s ? snd_stream_underruns(u->s) : 0);
    st.rate = (unsigned)u->rate;
    st.channels = (unsigned)u->channels;
    st.bits = (unsigned)u->bits;
    st.buffer_frames = (unsigned)((u64)u->rate * (u64)u->buffer_ms / 1000 + 1);
    st.space = (unsigned)MIN(space_bytes(u), 0xFFFFFFFFull);
    st.volume = u->volume;
    st.paused = u->paused;
    st.nonblock = u->nonblock;
    st.period_ms = period_ms();
    /* the stream's queue, then about two periods in the device */
    st.latency_ms = (int)(st.queued * 1000 / (u64)u->rate) + 2 * st.period_ms;
    return copyout(ust, &st, sizeof st) ? E_INVAL : 0;
}

static long sys_close(proc_t *p, int h, ustream_t *u, long how) {
    if (how != SNDCLOSE_NOW && how != SNDCLOSE_DRAIN && how != SNDCLOSE_WAIT) return E_INVAL;
    if (how == SNDCLOSE_WAIT) {
        long r = us_drain(u, -1);
        if (r) return r;                      /* ended meanwhile (or closed by another thread) */
        if (get(p, h) != u) return 0;
        how = SNDCLOSE_NOW;
    }
    stream_end(p, h, u, (int)how);
    return 0;
}

static long sys_master(long vol, long mute, u64 flags) {
    if (vol < -1 || vol > 100 || mute < -1 || mute > 1 || (flags & ~(u64)SNDVOL_SAVE)) return E_INVAL;
    int v = vol < 0 ? snd_volume() : (int)vol;
    int m = mute < 0 ? snd_muted() : (int)mute;
    if (vol >= 0 || mute >= 0 || (flags & SNDVOL_SAVE)) snd_set_volume(v, m, (flags & SNDVOL_SAVE) != 0);
    return snd_volume() | (snd_muted() ? SND_MUTED : 0);
}

static long sys_event(long ev, u64 flags) {
    if (ev < 0 || ev >= SNDEV_COUNT || (flags & ~(u64)(SNDEVF_FORCE | SNDEVF_QUERY))) return E_INVAL;
    if (!snddev_count()) return E_NODEV;
    if (flags & SNDEVF_QUERY) return (flags & SNDEVF_FORCE) ? sound_event_loaded((int)ev) == 1 : sound_event_would_play((int)ev);
    return (flags & SNDEVF_FORCE) ? sound_event_force((int)ev) : sound_event((int)ev);
}

static long sys_devinfo(long idx, u64 uout) {
    snddev_t *d = idx >= 0 ? snddev_get((int)idx) : NULL;
    if (!d) return E_NOENT;
    hal_snddev_t o;
    memset(&o, 0, sizeof o);
    strlcpy(o.name, d->name, sizeof o.name);
    strlcpy(o.desc, d->desc, sizeof o.desc);
    strlcpy(o.driver, d->driver ? d->driver : "", sizeof o.driver);
    strlcpy(o.output, d->output, sizeof o.output);
    o.rate = d->rate;
    o.channels = d->channels;
    o.period_frames = d->period_frames;
    o.running = d->running;
    o.is_default = d == snd_default();
    o.irq_live = d->irq_live;
    o.frames = d->frames;
    o.underruns = d->underruns;
    return copyout(uout, &o, sizeof o) ? E_INVAL : 0;
}

static long sys_info(proc_t *p, u64 uout) {
    hal_sndinfo_t o;
    memset(&o, 0, sizeof o);
    o.devices = snddev_count();
    o.volume = snd_volume();
    o.muted = snd_muted();
    o.sounds_on = sound_scheme_on();
    o.streams = nuser;
    o.max_streams = SND_USER_STREAMS;
    o.proc_streams = proc_count(p->pid);
    o.max_proc_streams = SND_PROC_STREAMS;
    o.startup_pending = sound_startup_pending();
    o.generation = snd_generation();
    return copyout(uout, &o, sizeof o) ? E_INVAL : 0;
}

/* ---------------- handles (core/syscall.c, core/proc.c, net/socket.c) ---------------- */

int sound_handle_close(proc_t *p, int h) {
    ustream_t *u = get(p, h);
    if (!u) return E_INVAL;
    stream_end(p, h, u, SNDCLOSE_DRAIN);
    return 0;
}

long sound_handle_write(proc_t *p, int h, u64 buf, u64 len) {
    ustream_t *u = get(p, h);
    if (!u) return E_INVAL;
    return us_write(u, buf, len, u->nonblock);
}

/* POLLOUT when a quarter of the buffer is free (or what is left of it) */
int sound_handle_poll(proc_t *p, int h) {
    ustream_t *u = get(p, h);
    if (!u) return POLLNVAL;
    u64 space = space_bytes(u);
    u64 quarter = ((u64)u->rate * (u64)u->buffer_ms / 4000) * (u64)u->fb;
    return space >= MAX(quarter, (u64)u->fb) ? POLLOUT : 0;
}

void sound_release_all(proc_t *p) {
    for (int i = 0; i < PROC_HANDLES; i++) {
        if (p->handles[i].type != H_SOUND) continue;
        ustream_t *u = p->handles[i].obj;
        if (u && !u->closed) stream_end(p, i, u, SNDCLOSE_NOW);
        else handle_free(p, i);
    }
}

/* ---------------- dispatcher (core/syscall.c) ---------------- */

long sound_syscall(int nr, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6) {
    (void)a6;
    proc_t *p = proc_current();
    if (!p) return E_INVAL;
    switch (nr) {
    case SYS_SND_OPEN: return sys_open(p, (long)(int)a1, (long)(int)a2, (long)(int)a3, (long)(int)a4, a5);
    case SYS_SND_MASTER: return sys_master((long)(int)a1, (long)(int)a2, a3);
    case SYS_SND_EVENT: return sys_event((long)(int)a1, a2);
    case SYS_SND_DEVINFO: return sys_devinfo((long)(int)a1, a2);
    case SYS_SND_INFO: return sys_info(p, a1);
    }
    if (nr < SYS_SND_WRITE || nr > SYS_SND_CLOSE) return E_NOTSUP;
    int h = (int)a1;
    ustream_t *u = get(p, h);
    if (!u) return E_INVAL;
    switch (nr) {
    case SYS_SND_WRITE:
        if (a4 & ~(u64)SNDW_NONBLOCK) return E_INVAL;
        return us_write(u, a2, a3, u->nonblock || (a4 & SNDW_NONBLOCK));
    case SYS_SND_DRAIN: return us_drain(u, (long)(int)a2);
    case SYS_SND_CTL: return sys_ctl(u, (long)(int)a2, (long)(int)a3);
    case SYS_SND_STATUS: return sys_status(u, a2);
    case SYS_SND_CLOSE: return sys_close(p, h, u, (long)(int)a2);
    }
    return E_NOTSUP;
}
