/* SELFTEST.EXE - checks the user-mode system calls and process isolation.
 * Run by the automated QEMU tests (and by hand: "selftest" in the Command
 * Prompt). Prints "selftest: all N checks passed" or the failures. */
#include "hal.h"

static int checks, failed;
#define CHECK(c, ...) do { checks++; if (!(c)) { failed++; printf("selftest: FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- threads ---- */
static hal_mutex_t lock;
static hal_cond_t cv;
static volatile int counter, ready, consumed;

static int adder(void *arg) {
    int n = (int)(long)arg;
    for (int i = 0; i < n; i++) {
        mutex_lock(&lock);
        int v = counter;
        if ((i & 63) == 0) yield();          /* force interleaving inside the lock */
        counter = v + 1;
        mutex_unlock(&lock);
    }
    return 100 + gettid();
}

static int producer(void *arg) {
    (void)arg;
    for (int i = 0; i < 50; i++) {
        mutex_lock(&lock);
        while (ready) cond_wait(&cv, &lock);
        ready = i + 1;
        cond_broadcast(&cv);
        mutex_unlock(&lock);
    }
    return 0;
}

static int tls_check(void *arg) {
    long slot[2] = { 0x1234567L + (long)arg, 0 };
    set_tls(slot);
    sleep_ms(20);                           /* other threads run and set their own */
    long v;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(v));
    return v == 0x1234567L + (long)arg;
}

static int sleeper(void *arg) { (void)arg; sleep_ms(100000); return 0; }

static int run(const char *args) {
    int pid = spawn("C:/PROGRAMS/CRASHME.EXE", args);
    if (pid < 0) return 9999;
    return wait(pid);
}

int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "shm")) {
        /* child: check what the parent wrote, answer through the same memory */
        int h = shm_open(argv[2], 0, 0);
        size_t sz = 0;
        unsigned *m = h >= 0 ? shm_map(h, &sz) : 0;
        if (!m || m[0] != 0xC0FFEEu) return 1;
        m[1] = 0xBEEFu;
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "threadexit")) {
        /* the main thread exits while another one sleeps: the program ends */
        thread_create(sleeper, 0);
        exit(42);
    }
    printf("selftest: starting (pid %d)\n", getpid());

    /* files */
    const char *dir = "C:/SELFTEST.TMP";
    mkdir(dir);
    char path[64];
    snprintf(path, sizeof path, "%s/data.bin", dir);
    static unsigned char buf[200000], back[200000];
    for (int i = 0; i < (int)sizeof buf; i++) buf[i] = (unsigned char)(i * 7 + 3);
    CHECK(write_file(path, buf, sizeof buf) == 0, "write_file");
    char *data = 0; long size = 0;
    CHECK(read_file(path, &data, &size) == 0 && size == (long)sizeof buf && !memcmp(data, buf, sizeof buf), "read back");
    free(data);
    int fd = open(path, O_READ);
    CHECK(fd >= 3, "open %d", fd);
    CHECK(seek(fd, 100000, 0) == 100000, "seek");
    CHECK(read(fd, back, 1000) == 1000 && !memcmp(back, buf + 100000, 1000), "read at offset");
    CHECK(close(fd) == 0, "close");
    hal_dirent_t st;
    CHECK(stat(path, &st) == 0 && st.size == sizeof buf && !st.is_dir, "stat");
    char path2[64];
    snprintf(path2, sizeof path2, "%s/renamed.bin", dir);
    CHECK(rename(path, path2) == 0 && stat(path, &st) == E_NOENT && stat(path2, &st) == 0, "rename");
    hal_dirent_t ents[8];
    CHECK(readdir(dir, ents, 8) == 1 && !strcmp(ents[0].name, "renamed.bin"), "readdir");
    CHECK(chdir(dir) == 0 && stat("renamed.bin", &st) == 0, "relative path after chdir");
    chdir("C:/");
    CHECK(unlink(path2) == 0 && unlink(dir) == 0 && stat(dir, &st) == E_NOENT, "unlink");
    CHECK(open("C:/no/such/file", O_READ) == E_NOENT, "missing file");

    /* bad pointers are rejected, not followed */
    CHECK(open((const char *)0x1000, O_READ) == E_INVAL, "open(bad pointer)");
    CHECK(write(1, (void *)0x100000, 16) == E_INVAL, "write(kernel pointer)");
    CHECK(stat("C:/", (hal_dirent_t *)0x200000) == E_INVAL, "stat(kernel buffer)");
    CHECK(SC3(SYS_READDIR, "C:/", 0xFFFF800000000000ull, 4) == E_INVAL, "readdir(kernel half)");
    CHECK(SC0(SYS_COUNT + 5) == E_NOTSUP, "unknown system call");

    /* memory */
    char *m = malloc(8 << 20);
    CHECK(m != 0, "malloc 8 MiB");
    if (m) { memset(m, 0x5A, 8 << 20); CHECK(m[(8 << 20) - 1] == 0x5A, "touch"); free(m); }

    /* time */
    uint64_t t0 = time_ms();
    sleep_ms(100);
    uint64_t dt = time_ms() - t0;
    CHECK(dt >= 100 && dt < 2000, "sleep 100 ms took %llu", (unsigned long long)dt);

    /* other processes: their crashes end them only */
    CHECK(run("null") == -1014, "null pointer -> page fault exit code");
    CHECK(run("kernel") == -1014, "write to kernel memory -> page fault");
    CHECK(run("divide") == -1000, "divide error");
    CHECK(run("opcode") == -1006, "invalid instruction");
    CHECK(run("privileged") == -1013, "privileged instruction -> GP fault");
    CHECK(run("stack") == -1014, "stack overflow");
    CHECK(run("badptr") == 0, "bad system call pointers return errors");
    int hello = spawn("C:/PROGRAMS/HELLO.EXE", "x");
    CHECK(hello > 0 && wait(hello) == 7, "HELLO.EXE exit code");

    /* a program spinning forever is preempted (we keep running) and can be ended */
    int loop = spawn("C:/PROGRAMS/CRASHME.EXE", "loop");
    CHECK(loop > 0, "spawn loop");
    uint64_t s0 = time_ms();
    volatile unsigned long spins = 0;
    while (time_ms() - s0 < 300) spins++;      /* we get CPU time while it spins */
    CHECK(spins > 1000, "preemption: only %lu spins", (unsigned long)spins);
    CHECK(kill(loop) == 0, "kill");
    CHECK(wait(loop) == -1, "killed exit code");

    /* threads: a contended mutex, join codes, condition variables, TLS, limits */
    counter = 0;
    int tids[8];
    for (int i = 0; i < 8; i++) tids[i] = thread_create(adder, (void *)2000L);
    int joined = 1;
    for (int i = 0; i < 8; i++) joined &= tids[i] > 1 && thread_join(tids[i]) == 100 + tids[i];
    CHECK(joined, "8 threads joined with their exit codes");
    CHECK(counter == 16000, "mutex: counter %d (expected 16000)", counter);
    ready = 0; consumed = 0;
    int pt = thread_create(producer, 0);
    for (int i = 0; i < 50; i++) {
        mutex_lock(&lock);
        while (!ready) cond_wait(&cv, &lock);
        consumed += ready;
        ready = 0;
        cond_broadcast(&cv);
        mutex_unlock(&lock);
    }
    CHECK(thread_join(pt) == 0 && consumed == 50 * 51 / 2, "condition variable hand-over: %d", consumed);
    int t1 = thread_create(tls_check, (void *)1L), t2 = thread_create(tls_check, (void *)2L);
    CHECK(thread_join(t1) == 1 && thread_join(t2) == 1, "thread-local storage (FS base) per thread");
    volatile int fw = 7;
    uint64_t f0 = time_ms();
    CHECK(futex_wait(&fw, 7, 50) == E_TIMEOUT && time_ms() - f0 >= 50, "futex timeout");
    CHECK(futex_wait(&fw, 8, 50) == E_AGAIN, "futex value changed");
    int many[40], nmany = 0, lastr = 0;
    for (int i = 0; i < 40; i++) {
        lastr = thread_create(sleeper, 0);
        if (lastr < 0) break;
        many[nmany++] = lastr;
    }
    CHECK(nmany == 31 && lastr == E_BUSY, "thread limit: %d threads, then %d", nmany, lastr);
    for (int i = 0; i < nmany; i++) thread_detach(many[i]);
    CHECK(thread_join(gettid()) == E_INVAL && thread_join(9999) == E_INVAL, "bad joins rejected");
    int te = spawn("C:/PROGRAMS/SELFTEST.EXE", "threadexit");
    CHECK(te > 0 && wait(te) == 42, "exit() from main ends the other threads too");
    CHECK(run("threadcrash") == -1014, "a crash in a second thread ends the program");

    /* shared memory between two programs */
    char shname[32];
    snprintf(shname, sizeof shname, "selftest.%d", getpid());
    int sh = shm_open(shname, 10000, SHM_CREATE | SHM_EXCL);
    size_t shsz = 0;
    unsigned *shm = sh >= 0 ? shm_map(sh, &shsz) : 0;
    CHECK(shm && shsz == 12288, "shared memory: %d, %lu bytes", sh, (unsigned long)shsz);
    if (shm) {
        shm[0] = 0xC0FFEEu;
        char a[48];
        snprintf(a, sizeof a, "shm %s", shname);
        int c = spawn("C:/PROGRAMS/SELFTEST.EXE", a);
        CHECK(c > 0 && wait(c) == 0 && shm[1] == 0xBEEFu, "another program sees and writes the same memory");
        CHECK(shm_open(shname, 100, SHM_CREATE | SHM_EXCL) == E_EXIST, "exclusive create of an existing name");
        CHECK(shm_open("bad name!", 100, SHM_CREATE) == E_INVAL, "bad shared memory name");
        CHECK(shm_unmap(shm) == 0 && shm_unmap(shm) == E_INVAL, "unmap once");
        close(sh);
    }
    CHECK(shm_open("selftest.none", 0, 0) == E_NOENT, "missing shared memory");

    hal_datetime_t dt;
    CHECK(datetime(&dt) == 0 && dt.year >= 2000 && dt.month >= 1 && dt.month <= 12, "date %d-%d-%d", dt.year, dt.month, dt.day);
    hal_volinfo_t vi;
    CHECK(volume_info('C', &vi) == 0 && vi.letter == 'C' && vi.is_boot && vi.total > vi.free_bytes && vi.free_bytes > 0,
          "volume info C: %s %llu/%llu", vi.fs, vi.free_bytes, vi.total);
    CHECK(volume_info('Q', &vi) == E_NOENT, "missing volume");

    hal_procinfo_t pl[32];
    int np = proclist(pl, 32);
    int me = 0;
    for (int i = 0; i < np; i++) if ((int)pl[i].pid == getpid() && pl[i].state == 1) me = 1;
    CHECK(me, "proclist lists this process");

    /* windows */
    int w = win_create("Self Test", 200, 100, 0);
    if (w >= 0) {
        hal_event_t ev;
        CHECK(win_get_event(&ev, 1000) == 1 && ev.type == EVT_PAINT, "first event is paint");
        gfx_fill(w, 0, 0, 200, 100, RGB(0, 128, 0));
        CHECK(gfx_text(w, 4, 4, "Self test", RGB(255, 255, 255), FONT_UI) > 4, "text");
        win_invalidate(w);
        CHECK(win_get_event(&ev, 50) == 0, "timeout with no events");
        hal_canvas_t cv2;
        CHECK(win_canvas(w, &cv2) == 0 && cv2.pixels && cv2.w == 200 && cv2.h == 100 && cv2.stride >= 200, "canvas mapped");
        if (cv2.pixels) {
            CHECK((cv2.pixels[50 * cv2.stride + 100] & 0xFFFFFF) == 0x008000, "drawing calls visible in the mapping");
            cv2.pixels[10 * cv2.stride + 10] = 0x123456;
            gfx_line(w, 0, 99, 199, 99, RGB(255, 0, 0));
            CHECK((cv2.pixels[99 * cv2.stride + 5] & 0xFFFFFF) == 0xFF0000 && cv2.pixels[10 * cv2.stride + 10] == 0x123456,
                  "direct writes and drawing calls share the pixels");
        }
        win_set_timer(w, 30);
        CHECK(win_get_event(&ev, 1000) == 1 && ev.type == EVT_TIMER, "timer event");
        /* drawing with absurd coordinates costs bounded time */
        uint64_t g0 = time_ms();
        gfx_line(w, -2000000000, -2000000000, 2000000000, 2000000000, RGB(255, 0, 0));
        gfx_circle(w, 100, 50, 2000000000, RGB(0, 0, 255), 1);
        gfx_circle(w, 100, 50, 2000000000, RGB(0, 0, 255), 0);
        CHECK(time_ms() - g0 < 2000, "huge shapes took %llu ms", (unsigned long long)(time_ms() - g0));
        win_destroy(w);
        CHECK(win_get_event(&ev, 0) == 0, "no events after destroy");

        /* canvas memory is limited per program: big windows run out with an error */
        int big[16], nbig = 0, last = 0;
        for (int i = 0; i < 16; i++) {
            last = win_create("Big", 2048, 1536, 0);
            if (last < 0) break;
            big[nbig++] = last;
        }
        CHECK(nbig >= 1 && nbig < 16 && last == E_NOMEM, "canvas quota: %d windows, then %d", nbig, last);
        for (int i = 0; i < nbig; i++) win_destroy(big[i]);
        CHECK((w = win_create("Again", 640, 480, 0)) >= 0, "window after quota released");
        if (w >= 0) win_destroy(w);

        /* a program waiting in a message box can be ended */
        int mb = spawn("C:/PROGRAMS/CRASHME.EXE", "msgbox");
        sleep_ms(300);
        uint64_t k0 = time_ms();
        CHECK(mb > 0 && kill(mb) == 0 && wait(mb) == -1 && time_ms() - k0 < 1000, "end a program in a message box");
    } else {
        printf("selftest: no desktop, window checks skipped\n");
    }

    if (failed) printf("selftest: %d of %d checks FAILED\n", failed, checks);
    else printf("selftest: all %d checks passed\n", checks);
    return failed;
}
