/* NVREPORT.EXE - saves what Halcyon found out about the NVIDIA graphics
 * card (M10a) in C:\HALCYON\NVREPORT, for writing the native driver (M10b):
 *
 *   REPORT.TXT       the card, the video BIOS and its tables, the
 *                    connectors and monitors, a read-only dump of the display
 *                    registers, the frame buffer speed test
 *   VBIOS_*.ROM      the video BIOS as read from each place
 *   EDIDn.BIN        each monitor's EDID (n: the connector)
 *   README.TXT       what these are and what to send back
 *
 *   nvreport [/nospeed] [/quiet]
 *
 * /nospeed skips the speed test (it draws grey frames for about a second),
 * /quiet the message boxes. Works without an NVIDIA card too (the report
 * then lists the display adapters and the speed test). */
#include "hal.h"

#define DIR "C:/HALCYON/NVREPORT"

static int quiet, files, failed;
static char *out;                       /* REPORT.TXT */
static long outn, outcap;

static void put(const char *s, long n) {
    if (outn + n + 1 > outcap) {
        long cap = outcap ? outcap * 2 : 65536;
        while (cap < outn + n + 1) cap *= 2;
        char *nb = realloc(out, (size_t)cap);
        if (!nb) return;
        out = nb;
        outcap = cap;
    }
    memcpy(out + outn, s, (size_t)n);
    outn += n;
    out[outn] = 0;
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof b - 1) n = sizeof b - 1;
    if (n > 0) put(b, n);
}

/* LF -> CRLF, so it opens nicely in any editor */
static int save_text(const char *name, const char *text, long n) {
    char *b = malloc((size_t)n * 2 + 1);
    if (!b) return -1;
    long o = 0;
    for (long i = 0; i < n; i++) {
        if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) b[o++] = '\r';
        b[o++] = text[i];
    }
    char path[128];
    snprintf(path, sizeof path, "%s/%s", DIR, name);
    int r = write_file(path, b, o);
    free(b);
    if (r < 0) { printf("nvreport: cannot write %s (error %d)\n", path, r); failed++; return r; }
    files++;
    return 0;
}

static int save_bin(const char *name, const void *data, long n) {
    char path[128];
    snprintf(path, sizeof path, "%s/%s", DIR, name);
    int r = write_file(path, data, n);
    if (r < 0) { printf("nvreport: cannot write %s (error %d)\n", path, r); failed++; return r; }
    files++;
    printf("  %s (%ld bytes)\n", name, n);
    return 0;
}

/* the files an earlier run left (only the kinds this program writes) */
static void clean(void) {
    hal_dirent_t e[64];
    int n = readdir(DIR, e, 64);
    for (int i = 0; i < n; i++) {
        if (e[i].is_dir) continue;
        const char *dot = strrchr(e[i].name, '.');
        if (!dot || (strcasecmp(dot, ".ROM") && strcasecmp(dot, ".BIN") && strcasecmp(dot, ".TXT"))) continue;
        char path[160];
        snprintf(path, sizeof path, "%s/%s", DIR, e[i].name);
        unlink(path);
    }
}

static long text_of(int gpu, char **t) {
    long cap = 256 * 1024;
    *t = malloc((size_t)cap + 1);
    if (!*t) return -1;
    long n = nv_query(NVQ_REPORT, gpu, *t, (size_t)cap);
    if (n < 0) return n;
    if (n > cap) n = cap;
    (*t)[n] = 0;
    return n;
}

static void card(int g) {
    char *t;
    long n = text_of(g, &t);
    say("\n");
    if (n > 0) put(t, n);
    else say("Card %d: no report (error %ld)\n", g, n);
    free(t);
    char pre[16] = "";
    if (g) snprintf(pre, sizeof pre, "CARD%d_", g);
    unsigned char *buf = malloc(1 << 20);
    if (!buf) return;
    for (int s = 0; s < 16; s++) {
        char nm[16];
        long r = nv_query(NVQ_ROMNAME, g << 8 | s, nm, sizeof nm);
        if (r < 0) break;
        long sz = nv_query(NVQ_ROM, g << 8 | s, buf, 1 << 20);
        if (sz <= 0) continue;
        char fn[64];
        snprintf(fn, sizeof fn, "%sVBIOS_%s.ROM", pre, nm);
        save_bin(fn, buf, sz > (1 << 20) ? (1 << 20) : sz);
    }
    for (int c = 0; c < 32; c++) {
        long r = nv_query(NVQ_EDID, g << 8 | c, buf, 4096);
        if (r == E_INVAL) break;
        if (r <= 0) continue;
        char fn[64];
        snprintf(fn, sizeof fn, "%sEDID%ld.BIN", pre, r >> 16);
        save_bin(fn, buf, r & 0xFFFF);
    }
    free(buf);
}

static const char *ctype(int t) {
    switch (t) {
    case 0: return "uncached";
    case 1: return "write-combining";
    case 4: return "write-through";
    case 6: return "write-back (made uncached by the MTRRs, most likely)";
    case 7: return "uncached (UC-)";
    }
    return "unknown";
}

static void rate(const hal_nvfbtest_t *t, unsigned long long *mbs10, unsigned long long *fps10, unsigned *cpu) {
    unsigned long long us = t->us ? t->us : 1;
    *mbs10 = t->bytes * 10 / us;                                   /* bytes/us = MB/s */
    *fps10 = t->frame_bytes ? t->bytes * 10000000ull / t->frame_bytes / us : 0;
    *cpu = (unsigned)(t->cpu_us * 100 / us);
}

static void speed(void) {
    hal_nvfbtest_t t[NV_FBTESTS];
    int ok[NV_FBTESTS], n = 0;
    for (int i = 0; i < NV_FBTESTS; i++) {
        memset(&t[i], 0, sizeof t[i]);
        long r = nv_query(NVQ_FBTEST, i, &t[i], sizeof t[i]);
        ok[i] = r >= 0;
        if (r < 0 && i < NV_FBTESTS_CPU) { say("  test %d: error %ld\n", i, r); break; }
        n++;
    }
    if (!n) return;
    say("  Mode %ux%u, %u bits per pixel, pitch %u bytes: %llu KiB per frame. Frame buffer page attribute: %s.\n",
        t[0].width, t[0].height, t[0].bpp, t[0].pitch, t[0].frame_bytes >> 10, ctype(t[0].cache_type));
    say("\n  %-40s %10s %12s %8s %9s\n", "test", "MB/s", "frames/s", "ms", "CPU busy");
    for (int i = 0; i < n; i++) {
        unsigned long long mbs10, fps10;
        unsigned cpu;
        if (!ok[i]) {
            say("  %-40s not available: %s\n", t[i].name, t[i].note[0] ? t[i].note : "?");
            printf("  %-40s not available\n", t[i].name);
            continue;
        }
        rate(&t[i], &mbs10, &fps10, &cpu);
        say("  %-40s %8llu.%llu %10llu.%llu %8llu %8u %%\n", t[i].name, mbs10 / 10, mbs10 % 10, fps10 / 10, fps10 % 10, t[i].us / 1000, cpu);
        printf("  %-40s %6llu.%llu MB/s, %llu.%llu frames/s, CPU %u %%\n", t[i].name, mbs10 / 10, mbs10 % 10, fps10 / 10, fps10 % 10, cpu);
    }
    if (n > NV_FBTESTS_CPU) {
        /* side by side: the CPU's way and the copy engine's way of the same work */
        static const struct { int cpu, eng; const char *what; } pair[] = {
            { 0, 6, "fill the screen" }, { 2, 7, "RAM to screen (the compositor)" }, { 3, 8, "screen to screen" } };
        say("\n  %-36s %12s %10s  %17s %10s\n", "Side by side (M10e)", "CPU MB/s", "CPU busy", "copy engine MB/s", "CPU busy");
        for (int k = 0; k < 3; k++) {
            unsigned long long m1, f1, m2, f2;
            unsigned c1, c2;
            rate(&t[pair[k].cpu], &m1, &f1, &c1);
            if (ok[pair[k].eng]) {
                rate(&t[pair[k].eng], &m2, &f2, &c2);
                say("  %-36s %10llu.%llu %8u %%  %15llu.%llu %8u %%\n", pair[k].what, m1 / 10, m1 % 10, c1, m2 / 10, m2 % 10, c2);
            } else {
                say("  %-36s %10llu.%llu %8u %%  %20s\n", pair[k].what, m1 / 10, m1 % 10, c1, "not available");
            }
        }
        if (ok[6] || ok[7] || ok[8]) say("  Copy engine: %s\n", t[ok[7] ? 7 : ok[6] ? 6 : 8].note);
        else say("  Copy engine: %s\n", t[6].note[0] ? t[6].note : "not available");
    }
    say("\n  frames/s: how many whole screens that speed moves in a second. MB = 1,000,000 bytes.\n"
        "  CPU busy: the share of the time the processor worked. A copy engine works by itself: the\n"
        "  processor only queues the work (the test waits for it; the desktop does not).\n"
        "  The desktop draws into memory and copies the changed parts to the screen (\"RAM to screen\");\n"
        "  it never reads the screen. With the copy engine (M10e) those copies, the page flips'\n"
        "  catching up between the two buffers and new buffers' fills are done by the GPU.\n");
}

static const char readme[] =
    "NVREPORT - what is in this folder\n"
    "=================================\n"
    "\n"
    "Halcyon's NVREPORT.EXE wrote these files to help write the native NVIDIA\n"
    "display driver (milestone M10b). Please send all of them back (zip the\n"
    "NVREPORT folder).\n"
    "\n"
    "  REPORT.TXT     everything found: the card and its chip, the video memory,\n"
    "                 the video BIOS and the tables in it (outputs, I2C ports,\n"
    "                 connectors, GPIOs), the monitors' EDIDs decoded, a dump of\n"
    "                 the display registers (only read, never written) and the\n"
    "                 frame buffer speed test\n"
    "  VBIOS_*.ROM    the video BIOS exactly as read from each place it can be:\n"
    "                 PRAMIN (the copy the GPU keeps in its memory), PROM (the\n"
    "                 flash chip on the card), ROMBAR (the PCI expansion ROM),\n"
    "                 SHADOW (the copy at C0000 when started by a PC BIOS)\n"
    "  EDIDn.BIN      the EDID of the monitor on connector n\n"
    "\n"
    "Privacy: the files describe the graphics card and the monitors only. There\n"
    "are no file names, no user names and nothing from your disks in them. An\n"
    "EDID holds the monitor's model and serial number, as every monitor reports\n"
    "it to any computer; leave the EDID files out if you prefer.\n"
    "\n"
    "If NVREPORT or the start of Halcyon misbehaves on this card, put the line\n"
    "nv.disable=1 into \\HALCYON\\SETTINGS.INI and send \\HALCYON\\BOOT.LOG\n"
    "(its lines start with \"nv\").\n";

int main(int argc, char **argv) {
    int nospeed = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcasecmp(argv[i], "/nospeed")) nospeed = 1;
        else if (!strcasecmp(argv[i], "/quiet")) quiet = 1;
        else { printf("usage: nvreport [/nospeed] [/quiet]\n"); return 1; }
    }
    printf("NVREPORT: NVIDIA graphics report\n");
    mkdir("C:/HALCYON");
    mkdir(DIR);
    clean();
    hal_sysinfo_t si;
    sysinfo(&si);
    int gpus = (int)nv_query(NVQ_COUNT, 0, NULL, 0);
    if (gpus < 0) gpus = 0;
    say("NVIDIA graphics report (NVREPORT.EXE, Halcyon %s)\n", si.version);
    say("=====================================================\n");
    say("For writing Halcyon's native NVIDIA driver (M10b). Only the graphics card and\n"
        "the monitors are described: no file names, no user data. README.TXT lists the files.\n\n");
    char *t;
    long n = text_of(-1, &t);
    if (n > 0) put(t, n);
    free(t);
    if (!gpus) say("\nNo NVIDIA graphics card was identified (or nv.disable=1 is set in SETTINGS.INI).\n");
    printf("%d NVIDIA card%s; writing %s\n", gpus, gpus == 1 ? "" : "s", DIR);
    for (int g = 0; g < gpus; g++) card(g);
    say("\nFrame buffer speed: the CPU and, with M10e, the copy engine\n");
    if (nospeed) say("  skipped (/nospeed)\n");
    else {
        if (!quiet && msgbox(0, "NVREPORT", "NVREPORT now measures how fast the screen can be drawn.\n\n"
                             "The screen flickers grey for about a second, then the desktop comes back.",
                             MBX_ICON_INFO, MBX_OKCANCEL) != IDR_OK) {
            say("  skipped (cancelled)\n");
        } else {
            printf("measuring the frame buffer (the screen flickers grey for about a second)...\n");
            speed();
        }
    }
    save_text("REPORT.TXT", out ? out : "", outn);
    save_text("README.TXT", readme, (long)strlen(readme));
    printf("  REPORT.TXT (%ld bytes), README.TXT\n", outn);
    if (failed) printf("nvreport: %d file%s could not be written\n", failed, failed == 1 ? "" : "s");
    else printf("nvreport: all %d files written to %s (%d NVIDIA card%s)\n", files, DIR, gpus, gpus == 1 ? "" : "s");
    if (!quiet) {
        char m[300];
        snprintf(m, sizeof m, failed ? "%d files could not be written to C:\\HALCYON\\NVREPORT." :
                 "The report is in C:\\HALCYON\\NVREPORT (%d files).\n\nPlease send the whole folder back; README.TXT says what is in it.",
                 failed ? failed : files);
        msgbox(0, "NVREPORT", m, failed ? MBX_ICON_WARNING : MBX_ICON_INFO, MBX_OK);
    }
    return failed ? 1 : 0;
}
