/* CRASHME.EXE - misbehaves on purpose to show that a program cannot take
 * the system down with it.
 *   crashme              the Crash Test window (pick a way to fail)
 *   crashme NAME         fail right away from the Command Prompt
 * NAME: null, kernel, divide, opcode, privileged, stack, loop, memory, badptr */
#include "hal.h"

static volatile int sink;

/* unbounded recursion (until the 1 MiB stack runs out) */
__attribute__((noinline)) static int recurse(int n) {
    volatile char pad[4096];
    pad[0] = (char)n;
    if (n < 0) return 0;
    return recurse(n + 1) + pad[0];
}

static void fail(const char *how) {
    if (!strcmp(how, "null")) { volatile int *p = 0; sink = *p; }
    else if (!strcmp(how, "kernel")) { volatile unsigned long *p = (unsigned long *)0x100000; *p = 0; }   /* the kernel's code */
    else if (!strcmp(how, "divide")) { volatile int a = 7, z = 0; sink = a / z; }
    else if (!strcmp(how, "opcode")) __asm__ volatile("ud2");
    else if (!strcmp(how, "privileged")) __asm__ volatile("cli; hlt");
    else if (!strcmp(how, "stack")) sink = recurse(0);
    else if (!strcmp(how, "loop")) { printf("Spinning forever: the desktop keeps working; Ctrl+C ends me.\n"); for (;;) sink++; }
    else if (!strcmp(how, "memory")) {
        long total = 0;
        for (;;) {
            char *p = malloc(16 << 20);
            if (!p) break;
            memset(p, 1, 16 << 20);
            total += 16;
        }
        printf("Allocated %ld MiB before the system said no; the rest of the system is unaffected.\n", total);
        return;
    } else if (!strcmp(how, "badptr")) {
        long r1 = write(1, (const void *)0x1000, 10);                 /* not mapped */
        long r2 = write(1, (const void *)0x100000, 10);               /* kernel memory */
        long r3 = read(0, (void *)0xFFFF800000000000ull, 10);         /* kernel half */
        printf("System calls with bad pointers return errors: %ld %ld %ld\n", r1, r2, r3);
        return;
    } else { printf("unknown test '%s'\n", how); return; }
    printf("still running?!\n");
}

static const char *const tests[] = { "null", "kernel", "divide", "opcode", "privileged", "stack", "loop", "memory", "badptr" };
static const char *const labels[] = {
    "Read a null pointer", "Write into kernel memory", "Divide by zero", "Invalid instruction",
    "Privileged instruction (CLI/HLT)", "Stack overflow", "Infinite loop", "Use up memory", "Bad system call pointers",
};
#define NT (int)(sizeof tests / sizeof tests[0])

int main(int argc, char **argv) {
    if (argc > 1) { fail(argv[1]); return 0; }
    int w = win_create("Crash Test", 330, 66 + NT * 30, WIN_NO_MAX);
    if (w < 0) { printf("Crash Test needs the desktop (%d)\n", w); return 1; }
    ctl_create(w, CTL_LABEL, 12, 10, 306, 46,
               "Each button makes this program fail in a different way. Halcyon ends the program; everything else keeps running.",
               0, CTL_WRAP);
    for (int i = 0; i < NT; i++) ctl_create(w, CTL_BUTTON, 12, 58 + i * 30, 306, 24, labels[i], 100 + i, 0);
    for (;;) {
        hal_event_t ev;
        if (!win_get_event(&ev, -1)) continue;
        if (ev.type == EVT_CLOSE) break;
        if (ev.type == EVT_COMMAND && ev.id >= 100 && ev.id < 100 + NT) fail(tests[ev.id - 100]);
    }
    return 0;
}
