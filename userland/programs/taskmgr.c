/* TASKMGR.EXE - Halcyon Task Manager: running programs, End Process, CPU
 * usage graph and memory. A user-mode program itself (Ctrl+Alt+Del). */
#include "hal.h"

#define W 420
#define H 430
#define ID_LIST 10
#define ID_END 11
#define ID_REFRESH 12
#define HIST 60

static int win;
static hal_procinfo_t procs[64];
static int nprocs;
static int pids[64];
static int cpu_hist[HIST], nhist;
static hal_sysinfo_t si;

static void fmt_kib(unsigned long long k, char *out, int n) {
    if (k >= 10240) snprintf(out, (size_t)n, "%llu MB", k / 1024);
    else snprintf(out, (size_t)n, "%llu KB", k);
}

static void refresh_list(void) {
    nprocs = proclist(procs, 64);
    if (nprocs < 0) nprocs = 0;
    static char rows[65][96];
    const char *items[65];
    int n = 0;
    /* the kernel itself, like XP's "System" entry */
    char mem[24];
    fmt_kib(si.heap_used_kib, mem, sizeof mem);
    snprintf(rows[n], sizeof rows[n], "System (the kernel)  -  %d threads, %s", si.nthreads, mem);
    pids[n] = 0;
    items[n] = rows[n]; n++;
    for (int i = 0; i < nprocs && n < 65; i++) {
        if (procs[i].state != 1) continue;
        char cpu[24];
        unsigned long long s = procs[i].cpu_ms / 1000;
        snprintf(cpu, sizeof cpu, "%llu:%02llu:%02llu", s / 3600, s / 60 % 60, s % 60);
        fmt_kib(procs[i].mem_kib, mem, sizeof mem);
        snprintf(rows[n], sizeof rows[n], "%s  -  PID %u, %s, CPU time %s", procs[i].name, procs[i].pid, mem, cpu);
        pids[n] = (int)procs[i].pid;
        items[n] = rows[n]; n++;
    }
    int sel = ctl_get_value(win, ID_LIST);
    ctl_list_set(win, ID_LIST, items, n);
    if (sel >= 0 && sel < n) ctl_set_value(win, ID_LIST, sel);
}

static void draw_perf(void) {
    int gx = 12, gy = 262, gw = W - 24, gh = 100;
    gfx_fill(win, 0, 240, W, 160, RGB(0xEC, 0xE9, 0xD8));
    char t[96];
    snprintf(t, sizeof t, "CPU Usage History (%d%%)", si.cpu_load);
    gfx_text(win, gx, 244, t, RGB(0, 0, 0), FONT_UI_BOLD);
    gfx_fill(win, gx, gy, gw, gh, RGB(0, 0, 0));
    for (int x = gx; x < gx + gw; x += 12) gfx_line(win, x, gy, x, gy + gh - 1, RGB(0, 0x50, 0x20));
    for (int y = gy; y < gy + gh; y += 12) gfx_line(win, gx, y, gx + gw - 1, y, RGB(0, 0x50, 0x20));
    int step = gw / (HIST - 1);
    for (int i = 1; i < nhist; i++) {
        int x0 = gx + gw - 1 - (nhist - i) * step, x1 = x0 + step;
        int y0 = gy + gh - 1 - cpu_hist[i - 1] * (gh - 2) / 100, y1 = gy + gh - 1 - cpu_hist[i] * (gh - 2) / 100;
        gfx_line(win, x0, y0, x1, y1, RGB(0, 0xFF, 0x40));
    }
    gfx_bevel(win, gx - 1, gy - 1, gw + 2, gh + 2, 0);
    char a[24], b[24], c[24];
    fmt_kib(si.ram_kib, a, sizeof a);
    fmt_kib(si.heap_used_kib, b, sizeof b);
    fmt_kib(si.heap_kib - si.heap_used_kib, c, sizeof c);
    snprintf(t, sizeof t, "Physical memory: %s    In use: %s    Available: %s", a, b, c);
    gfx_text(win, gx, gy + gh + 8, t, RGB(0, 0, 0), FONT_UI);
    unsigned long long up = si.uptime_ms / 1000;
    snprintf(t, sizeof t, "Processes: %d    Threads: %d    Up time: %llu:%02llu:%02llu", si.nprocs, si.nthreads,
             up / 3600, up / 60 % 60, up % 60);
    gfx_text(win, gx, gy + gh + 26, t, RGB(0, 0, 0), FONT_UI);
    win_invalidate(win);
}

static void tick(void) {
    sysinfo(&si);
    if (nhist == HIST) { memmove(cpu_hist, cpu_hist + 1, sizeof(int) * (HIST - 1)); nhist--; }
    cpu_hist[nhist++] = si.cpu_load;
    refresh_list();
    draw_perf();
}

int main(int argc, char **argv) {
    win = win_create("Halcyon Task Manager", W, H, WIN_NO_MAX);
    if (win < 0) { printf("Task Manager needs the desktop.\n"); return 1; }
    gfx_text(win, 12, 8, "Applications and processes running on this computer:", RGB(0, 0, 0), FONT_UI);
    ctl_create(win, CTL_LISTBOX, 12, 28, W - 24, 170, "", ID_LIST, 0);
    ctl_create(win, CTL_BUTTON, W - 12 - 100, 206, 100, 24, "End Process", ID_END, 0);
    ctl_create(win, CTL_BUTTON, W - 12 - 208, 206, 100, 24, "Refresh", ID_REFRESH, 0);
    win_set_timer(win, 1000);
    tick();
    for (;;) {
        hal_event_t ev;
        if (!win_get_event(&ev, -1)) continue;
        if (ev.type == EVT_CLOSE) break;
        if (ev.type == EVT_TIMER) tick();
        if (ev.type == EVT_KEY && ev.pressed && ev.id == KEY_F1) help_open("taskmgr");
        if (ev.type == EVT_COMMAND && ev.id == ID_REFRESH) tick();
        if (ev.type == EVT_COMMAND && ev.id == ID_END) {
            int sel = ctl_get_value(win, ID_LIST);
            int pid = sel >= 0 && sel < 65 ? pids[sel] : -1;
            if (pid == 0) { msgbox(win, "Task Manager", "The System process is the Halcyon kernel and cannot be ended.", MBX_ICON_WARNING, MBX_OK); continue; }
            if (pid < 0) continue;
            if (pid == getpid()) break;
            /* Halcyon itself asks the user before a program ends another one */
            int r = kill(pid);
            if (r < 0 && r != E_PERM) msgbox(win, "Unable to Terminate Process", hal_strerror(r), MBX_ICON_ERROR, MBX_OK);
            tick();
        }
    }
    return 0;
}
