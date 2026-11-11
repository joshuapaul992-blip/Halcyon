/* SELFTEST.EXE - checks the user-mode system calls and process isolation.
 * Run by the automated QEMU tests (and by hand: "selftest" in the Command
 * Prompt). Prints "selftest: all N checks passed" or the failures. */
#include "hal.h"

static int checks, failed;
#define CHECK(c, ...) do { checks++; if (!(c)) { failed++; printf("selftest: FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int run(const char *args) {
    int pid = spawn("C:/PROGRAMS/CRASHME.EXE", args);
    if (pid < 0) return 9999;
    return wait(pid);
}

int main(int argc, char **argv) {
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
        win_set_timer(w, 30);
        CHECK(win_get_event(&ev, 1000) == 1 && ev.type == EVT_TIMER, "timer event");
        win_destroy(w);
        CHECK(win_get_event(&ev, 0) == 0, "no events after destroy");
    } else {
        printf("selftest: no desktop, window checks skipped\n");
    }

    if (failed) printf("selftest: %d of %d checks FAILED\n", failed, checks);
    else printf("selftest: all %d checks passed\n", checks);
    return failed;
}
