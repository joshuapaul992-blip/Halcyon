/* SNDTEST.EXE - checks the sound system calls: formats and bad arguments,
 * handles, blocking and non-blocking writes, poll, drain timing, pause,
 * discard, volumes, closing, the per-program and system-wide stream limits,
 * copies of itself that end with open streams or are ended while they wait
 * in a write, the master volume and system sound events; then it plays a
 * known signal through the system calls: a 660 Hz sine as 8-bit mono at
 * 32 kHz for 1 s, after the startup sound has finished. Run by the
 * automated QEMU tests (the `audio` variant, which records the sound card
 * and looks for the signal) and by hand ("sndtest" in the Command Prompt).
 * Plays only silence apart from the signal. Prints "sndtest: all N checks
 * passed" or the failures. */
#include "hal.h"

static int checks, failed;
#define CHECK(c, ...) do { checks++; if (!(c)) { failed++; printf("sndtest: FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define EXE "C:/PROGRAMS/SNDTEST.EXE"
#define SIG_RATE 32000
#define SIG_HZ 660

static unsigned char silence[48000 * 4];          /* 16-bit zeros; for 8-bit streams 0x80 below */
static unsigned char silence8[48000];

static int streams_now(void) {
    hal_sndinfo_t in;
    return snd_info(&in) == 0 ? in.streams : -1;
}

static int wait_streams(int want, int ms) {
    uint64_t t0 = time_ms();
    while (streams_now() != want && time_ms() - t0 < (uint64_t)ms) sleep_ms(20);
    return streams_now();
}

/* sin(2 pi t), t in turns (no math library here) */
static double sine(double t) {
    t -= (double)(long)t;
    if (t < 0) t += 1;
    double x = (t > 0.5 ? t - 1 : t) * 6.283185307179586;   /* -pi..pi */
    double x2 = x * x, r = x, term = x;
    for (int k = 1; k < 9; k++) { term *= -x2 / ((2 * k) * (2 * k + 1)); r += term; }
    return r;
}

/* ================= child programs ================= */
static int child(const char *mode, int n) {
    if (!strcmp(mode, "child-open")) {          /* open streams, queue a little, end without closing */
        for (int i = 0; i < 3; i++) {
            int h = snd_open(22050, 2, 16, 200, SND_NONBLOCK);
            if (h < 0) return 10 + i;
            if (snd_write(h, silence, 22050 / 10 * 4) <= 0) return 20 + i;
        }
        return 0;
    }
    if (!strcmp(mode, "child-block")) {         /* wait in a write that can never finish */
        int h = snd_open(8000, 1, 16, 50, 0);
        if (h < 0) return 30;
        snd_pause(h, 1);
        snd_write(h, silence, sizeof silence);   /* blocks: the stream is paused */
        return 31;
    }
    if (!strcmp(mode, "child-hold")) {          /* hold n streams until ended */
        for (int i = 0; i < n; i++) if (snd_open(8000, 1, 8, 50, 0) < 0) return 40 + i;
        for (;;) sleep_ms(1000);
    }
    return 99;
}

/* ================= the checks ================= */
static void check_info(void) {
    hal_snddev_t d;
    hal_sndinfo_t in;
    CHECK(snd_devinfo(0, &d) == 0 && !strcmp(d.name, "snd0") && d.desc[0] && d.driver[0] && d.rate >= 8000 && d.channels >= 1 &&
          d.is_default, "device 0: %s %s %s %d Hz", d.name, d.desc, d.driver, d.rate);
    CHECK(snd_devinfo(-1, &d) == E_NOENT && snd_devinfo(8, &d) == E_NOENT && snd_devinfo(1 << 30, &d) == E_NOENT, "devinfo past the end");
    CHECK(snd_devinfo(0, (hal_snddev_t *)16) == E_INVAL, "devinfo to a bad pointer");
    CHECK(snd_info((hal_sndinfo_t *)0x1000) == E_INVAL, "info to a bad pointer");
    CHECK(snd_info(&in) == 0 && in.devices >= 1 && in.max_proc_streams == SND_PROC_STREAMS && in.max_streams == SND_USER_STREAMS &&
          in.proc_streams == 0 && in.volume >= 0 && in.volume <= 100, "info: %d devices, %d/%d streams", in.devices, in.proc_streams,
          in.max_proc_streams);
}

static void check_args(void) {
    struct { int rate, ch, bits, ms, flags; } bad[] = {
        { 7999, 1, 16, 100, 0 }, { 192001, 2, 16, 100, 0 }, { 0, 1, 16, 100, 0 }, { -44100, 1, 16, 100, 0 },
        { 44100, 0, 16, 100, 0 }, { 44100, 3, 16, 100, 0 }, { 44100, -1, 16, 100, 0 }, { 44100, 1, 0, 100, 0 },
        { 44100, 1, 12, 100, 0 }, { 44100, 1, 24, 100, 0 }, { 44100, 1, 32, 100, 0 }, { 44100, 1, 16, -1, 0 },
        { 44100, 1, 16, 100, 0x80 },
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int r = snd_open(bad[i].rate, bad[i].ch, bad[i].bits, bad[i].ms, bad[i].flags);
        CHECK(r == E_INVAL, "open(%d, %d, %d, %d, %#x) = %d", bad[i].rate, bad[i].ch, bad[i].bits, bad[i].ms, bad[i].flags, r);
        if (r >= 0) close(r);
    }
    int h = snd_open(22050, 2, 16, 100, 0);
    CHECK(h >= 3, "open 22050 Hz stereo 16-bit: %d", h);
    if (h < 0) return;
    hal_sndstatus_t st;
    CHECK(snd_status(h, &st) == 0 && st.rate == 22050 && st.channels == 2 && st.bits == 16 && st.buffer_frames == 2206 &&
          st.space == st.buffer_frames * 4 && !st.written && !st.played && !st.queued && st.volume == 100 && !st.paused &&
          !st.nonblock && st.period_ms > 0, "fresh status: buffer %u frames, space %u", st.buffer_frames, st.space);
    char buf[16] = { 0 };
    CHECK(snd_write(h, buf, 3) == E_INVAL, "a partial frame");
    CHECK(snd_write(h, (void *)0x1000, 4) == E_INVAL, "write from a bad pointer");
    CHECK(snd_write(h, buf, 0) == 0, "write of nothing");
    CHECK(SC4(SYS_SND_WRITE, h, buf, 4, 0x10) == E_INVAL, "unknown write flag");
    CHECK(snd_ctl(h, 99, 0) == E_INVAL && snd_ctl(h, SNDCTL_VOLUME, 101) == E_INVAL && snd_ctl(h, SNDCTL_VOLUME, -1) == E_INVAL &&
          snd_ctl(h, SNDCTL_PAUSE, 2) == E_INVAL && snd_ctl(h, SNDCTL_NONBLOCK, -1) == E_INVAL, "bad controls");
    CHECK(snd_status(h, (hal_sndstatus_t *)0x10) == E_INVAL, "status to a bad pointer");
    CHECK(snd_close(h, 7) == E_INVAL && snd_drain(99, 0) == E_INVAL, "bad close / handle");
    CHECK(snd_stream_volume(h, 50) == 0 && snd_status(h, &st) == 0 && st.volume == 50, "stream volume 50");
    /* not sound handles */
    int fh = open("C:/HALCYON/MEDIA/DEFAULT.WAV", O_READ);
    CHECK(snd_write(99, buf, 4) == E_INVAL && snd_write(1, buf, 4) == E_INVAL && snd_write(-1, buf, 4) == E_INVAL &&
          (fh < 0 || (snd_write(fh, buf, 4) == E_INVAL && snd_status(fh, &st) == E_INVAL && snd_close(fh, 0) == E_INVAL)),
          "sound calls on other handles");
    if (fh >= 0) close(fh);
    CHECK(write(h, buf, 8) == 8 && snd_status(h, &st) == 0 && st.written == 2, "write() on a stream");
    CHECK(close(h) == 0 && snd_status(h, &st) == E_INVAL && close(h) == E_INVAL, "close() a stream, then it is gone");
}

static void check_nonblock_poll(void) {
    int h = snd_open(22050, 2, 16, 100, SND_NONBLOCK);
    hal_sndstatus_t st;
    CHECK(h >= 0 && snd_status(h, &st) == 0 && st.nonblock, "non-blocking open: %d", h);
    if (h < 0) return;
    snd_pause(h, 1);
    long r = snd_write(h, silence, 22050 * 4);
    CHECK(r == 2206 * 4, "non-blocking write takes what fits: %ld", r);
    CHECK(snd_write(h, silence, 4) == E_AGAIN, "then E_AGAIN");
    struct pollfd p = { h, POLLOUT, 0 };
    CHECK(poll(&p, 1, 0) == 0 && p.revents == 0, "poll: full, no POLLOUT");
    CHECK(snd_status(h, &st) == 0 && st.queued == 2206 && st.paused && st.space == 0 && st.latency_ms >= 100,
          "full and paused: queued %llu, latency %d ms", st.queued, st.latency_ms);
    uint64_t t0 = time_ms();
    snd_pause(h, 0);
    int n = poll(&p, 1, 3000);
    uint64_t dt = time_ms() - t0;
    CHECK(n == 1 && p.revents == POLLOUT && dt < 1000, "poll: POLLOUT after %llu ms (%d, %#x)", (unsigned long long)dt, n, p.revents);
    CHECK(snd_status(h, &st) == 0 && st.space >= 551 * 4, "a quarter is free: %u", st.space);
    CHECK(snd_discard(h) == 0 && snd_status(h, &st) == 0 && st.queued == 0 && st.space == st.buffer_frames * 4, "discard empties it");
    CHECK(SC4(SYS_SND_WRITE, h, silence, 4, 0) == 4, "a write");
    CHECK(snd_ctl(h, SNDCTL_NONBLOCK, 0) == 0 && snd_status(h, &st) == 0 && !st.nonblock, "blocking again");
    snd_close(h, SNDCLOSE_NOW);
    /* poll on a closed handle */
    p.fd = h;
    CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLNVAL), "poll on a closed stream: %#x", p.revents);
}

static void check_timing(void) {
    /* a blocking write of 500 ms into a 100 ms buffer waits about 400 ms */
    int h = snd_open(22050, 2, 16, 100, 0);
    if (h < 0) { CHECK(0, "open: %d", h); return; }
    uint64_t t0 = time_ms();
    long r = snd_write(h, silence, 11025 * 4);
    uint64_t dt = time_ms() - t0;
    CHECK(r == 11025 * 4 && dt >= 250 && dt < 2000, "blocking write of 500 ms: %ld bytes in %llu ms", r, (unsigned long long)dt);
    t0 = time_ms();
    int d = snd_drain(h, 5000);
    dt = time_ms() - t0;
    hal_sndstatus_t st;
    CHECK(d == 0 && dt < 500 && snd_status(h, &st) == 0 && st.queued == 0 && st.played == st.written && st.written == 11025 &&
          !st.underruns, "drain: %d after %llu ms, played %llu of %llu, %llu underruns", d, (unsigned long long)dt,
          st.played, st.written, st.underruns);
    /* usable again after a drain */
    CHECK(snd_write(h, silence, 2205 * 4) == 2205 * 4 && snd_status(h, &st) == 0 && st.written == 11025 + 2205, "write after drain");
    snd_close(h, SNDCLOSE_NOW);

    /* 300 ms into a 500 ms buffer: drain takes about 300 ms */
    h = snd_open(44100, 1, 16, 500, 0);
    if (h < 0) { CHECK(0, "open: %d", h); return; }
    r = snd_write(h, silence, 13230 * 2);
    t0 = time_ms();
    d = snd_drain(h, -1);
    dt = time_ms() - t0;
    CHECK(r == 13230 * 2 && d == 0 && dt >= 200 && dt < 800, "drain of 300 ms: %llu ms", (unsigned long long)dt);
    /* a drain that cannot finish (paused) times out */
    snd_write(h, silence, 4410 * 2);
    snd_pause(h, 1);
    t0 = time_ms();
    d = snd_drain(h, 200);
    dt = time_ms() - t0;
    CHECK(d == E_TIMEOUT && dt >= 150 && dt < 1000, "drain while paused: %d after %llu ms", d, (unsigned long long)dt);
    snd_close(h, SNDCLOSE_NOW);

    /* pause holds the position */
    h = snd_open(16000, 2, 8, 500, 0);
    if (h < 0) { CHECK(0, "open: %d", h); return; }
    memset(silence8, 0x80, sizeof silence8);
    snd_write(h, silence8, 6400 * 2);             /* 400 ms */
    sleep_ms(100);
    snd_pause(h, 1);
    hal_sndstatus_t a, b;
    snd_status(h, &a);
    sleep_ms(300);
    snd_status(h, &b);
    CHECK(a.paused && b.played == a.played && a.played < 6400 && b.queued == a.queued, "paused: played %llu then %llu",
          a.played, b.played);
    snd_pause(h, 0);
    t0 = time_ms();
    CHECK(snd_close(h, SNDCLOSE_WAIT) == 0 && time_ms() - t0 >= 100 && snd_status(h, &b) == E_INVAL,
          "close and wait: %llu ms", (unsigned long long)(time_ms() - t0));
}

static void check_limits(void) {
    int base = streams_now(), hs[SND_PROC_STREAMS + 1], n = 0;
    for (int i = 0; i < SND_PROC_STREAMS; i++) if ((hs[n] = snd_open(8000, 1, 8, 20, 0)) >= 0) n++;
    hal_sndinfo_t in;
    int r = snd_open(8000, 1, 8, 20, 0);
    CHECK(n == SND_PROC_STREAMS && r == E_BUSY && snd_info(&in) == 0 && in.proc_streams == SND_PROC_STREAMS &&
          in.streams == base + SND_PROC_STREAMS, "per program: %d opened, then %d", n, r);
    for (int i = 0; i < n; i++) snd_close(hs[i], i % 2 ? SNDCLOSE_NOW : SNDCLOSE_DRAIN);
    CHECK(streams_now() == base, "all closed: %d streams (%d before)", streams_now(), base);

    /* system-wide: three copies hold 8 each */
    int kids[3];
    char args[32];
    snprintf(args, sizeof args, "child-hold %d", SND_PROC_STREAMS);
    for (int i = 0; i < 3; i++) kids[i] = spawn(EXE, args);
    int now = wait_streams(base + 3 * SND_PROC_STREAMS, 5000);
    r = snd_open(8000, 1, 8, 20, 0);
    CHECK(now == SND_USER_STREAMS && r == E_BUSY, "system-wide: %d streams, then %d", now, r);
    if (r >= 0) close(r);
    for (int i = 0; i < 3; i++) if (kids[i] > 0) { kill(kids[i]); wait(kids[i]); }
    CHECK(wait_streams(base, 3000) == base, "the copies' streams are gone");

    /* a copy that ends without closing its streams */
    int pid = spawn(EXE, "child-open");
    int code = pid > 0 ? wait(pid) : pid;
    CHECK(code == 0 && wait_streams(base, 2000) == base, "a copy that ends with 3 open streams: exit %d, %d streams", code, streams_now());

    /* a copy ended while it waits in a write */
    pid = spawn(EXE, "child-block");
    now = wait_streams(base + 1, 3000);
    sleep_ms(200);
    uint64_t t0 = time_ms();
    int k = pid > 0 ? kill(pid) : pid;
    code = pid > 0 ? wait(pid) : pid;
    uint64_t dt = time_ms() - t0;
    CHECK(now == base + 1 && k == 0 && code == -1 && dt < 1500 && wait_streams(base, 2000) == base,
          "a copy ended in a blocked write: %d streams, kill %d, exit %d after %llu ms", now, k, code, (unsigned long long)dt);
}

static void check_master_events(void) {
    int m0 = snd_master(-1, -1, 0);
    CHECK(m0 >= 0 && (m0 & 0xFF) <= 100, "master: %#x", m0);
    CHECK(snd_master(37, -1, 0) == ((m0 & SND_MUTED) | 37), "master 37");
    CHECK(snd_master(-1, 1, 0) == (37 | SND_MUTED) && snd_master(-1, 0, 0) == 37, "mute, unmute");
    CHECK(snd_master(101, -1, 0) == E_INVAL && snd_master(-2, -1, 0) == E_INVAL && snd_master(-1, 2, 0) == E_INVAL &&
          snd_master(-1, -1, 4) == E_INVAL, "bad master values");
    hal_sndinfo_t in;
    CHECK(snd_info(&in) == 0 && in.volume == 37 && !in.muted, "info sees the master volume");
    snd_master(m0 & 0xFF, (m0 & SND_MUTED) != 0, 0);
    CHECK(snd_master(-1, -1, 0) == m0, "master restored");
    CHECK(snd_event(-1, 0) == E_INVAL && snd_event(SNDEV_COUNT, 0) == E_INVAL && snd_event(SNDEV_INFO, 0x10) == E_INVAL, "bad events");
    int q = snd_event(SNDEV_INFO, SNDEVF_QUERY);
    CHECK(q == 0 || q == 1, "query: %d", q);
    CHECK(snd_event(SNDEV_STARTUP, SNDEVF_QUERY | SNDEVF_FORCE) == 1, "the startup sound is loaded");
}

/* the signal: 660 Hz, 8-bit mono, 32 kHz, 1 s */
static void play_signal(void) {
    uint64_t t0 = time_ms();
    hal_sndinfo_t in;
    while (snd_info(&in) == 0 && in.startup_pending && time_ms() - t0 < 20000) sleep_ms(50);
    CHECK(!in.startup_pending, "the startup sound finished");
    sleep_ms(300);
    static unsigned char sig[SIG_RATE];
    for (int i = 0; i < SIG_RATE; i++) {
        double v = 100.0 * sine((double)((long)i * SIG_HZ % SIG_RATE) / SIG_RATE);
        sig[i] = (unsigned char)(128 + (int)(v < 0 ? v - 0.5 : v + 0.5));
    }
    int h = snd_open(SIG_RATE, 1, 8, 250, 0);
    CHECK(h >= 0, "open the signal's stream: %d", h);
    if (h < 0) return;
    t0 = time_ms();
    long r = snd_write(h, sig, sizeof sig);
    int d = snd_drain(h, 5000);
    uint64_t dt = time_ms() - t0;
    hal_sndstatus_t st;
    CHECK(r == SIG_RATE && d == 0 && snd_status(h, &st) == 0 && st.played == SIG_RATE && !st.underruns && dt >= 850 && dt < 1500,
          "signal: %ld bytes, drain %d, played %llu, %llu underruns, %llu ms", r, d, st.played, st.underruns, (unsigned long long)dt);
    snd_close(h, SNDCLOSE_NOW);
    printf("sndtest: played %d Hz, 8-bit mono, %d Hz, 1 s in %llu ms\n", SIG_HZ, SIG_RATE, (unsigned long long)dt);
}

int main(int argc, char **argv) {
    if (argc > 1) return child(argv[1], argc > 2 ? atoi(argv[2]) : 0);
    hal_sndinfo_t in;
    if (snd_info(&in) || !in.devices) {
        CHECK(snd_open(22050, 2, 16, 100, 0) == E_NODEV, "no sound card: open fails with E_NODEV");
        printf("sndtest: no sound card; stream checks skipped\n");
    } else {
        check_info();
        check_args();
        check_nonblock_poll();
        check_timing();
        check_limits();
        /* (after the startup sound: the master volume must not change while it plays) */
        uint64_t t0 = time_ms();
        while (snd_info(&in) == 0 && in.startup_pending && time_ms() - t0 < 20000) sleep_ms(50);
        check_master_events();
        play_signal();
    }
    if (failed) printf("sndtest: %d of %d checks FAILED\n", failed, checks);
    else printf("sndtest: all %d checks passed\n", checks);
    return failed;
}
