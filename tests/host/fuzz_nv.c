/* Fuzz target: the NVIDIA video BIOS parser (kernel/drivers/nv/vbios.c)
 * and the EDID timing parser (kernel/drivers/edid.c) on arbitrary bytes.
 * The first byte picks a chipset (some rules depend on it).
 *
 * libFuzzer (clang, -DFUZZ_LIBFUZZER):
 *   make -C tests/host fuzz-nv-run FUZZ_TIME=600
 * replay with gcc + ASan/UBSan (seeds from nvtest --seeds, plus any files):
 *   make -C tests/host fuzz-nv-regress
 *   build/hosttest/fuzz_nv_replay FILE|DIR...
 */
#include <stdio.h>
#include <stdlib.h>
#include "core/lib.h"
#include "drivers/nv/nvhw.h"
#include "drivers/edid.h"

const char *strerror(int e) { static char b[16]; snprintf(b, sizeof b, "E%d", e); return b; }

static const u16 chipsets[8] = { 0x00, 0x05, 0x11, 0x17, 0x40, 0x50, 0xc4, 0x134 };

int LLVMFuzzerTestOneInput(const u8 *data, size_t size);
int LLVMFuzzerTestOneInput(const u8 *data, size_t size) {
    static nv_bios_t b;
    if (!size) return 0;
    int cs = chipsets[data[0] & 7];
    /* the parser gets an exact-size heap copy, so ASan sees any overread */
    u8 *copy = malloc(size - 1 ? size - 1 : 1);
    memcpy(copy, data + 1, size - 1);
    nv_bios_parse(copy, (u32)(size - 1), cs, &b);
    if (b.noutputs > NV_DCB_MAX || b.nccb > NV_CCB_MAX || b.nconn > NV_CONN_MAX || b.ngpio > NV_GPIO_MAX || b.ntokens > NV_BIT_MAX)
        abort();
    /* every lookup the kernel makes afterwards */
    for (int i = 0; i < b.ntokens; i++) nv_bios_token(&b, b.tok[i].id);
    for (int i = 0; i < b.nconn; i++) nv_conn_type_str(b.conn[i].type);
    for (int i = 0; i < b.ngpio; i++) nv_gpio_func_str(b.gpio[i].func);
    for (u32 i = 0; i < 64; i++) nv_rom_rd32(&b.rom, b.dcb_offset + i * 4, NULL);
    edid_timing_t t[16];
    edid_timings(copy, (int)(size - 1), t, 16);
    edid_info_t ei;
    edid_parse(copy, (int)(size - 1), &ei);
    free(copy);
    return 0;
}

#ifndef FUZZ_LIBFUZZER

#include <dirent.h>
#include <sys/stat.h>

static int runs;

static void run_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *b = malloc(n > 0 ? (size_t)n : 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "short read %s\n", path); exit(2); }
    fclose(f);
    /* each seed with every chipset rule */
    u8 *c = malloc((size_t)n + 1);
    for (int k = 0; k < 8; k++) {
        c[0] = (u8)k;
        memcpy(c + 1, b, (size_t)n);
        LLVMFuzzerTestOneInput(c, (size_t)n + 1);
        runs++;
    }
    free(c);
    free(b);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st)) { fprintf(stderr, "cannot stat %s\n", argv[i]); return 2; }
        if (!S_ISDIR(st.st_mode)) { run_file(argv[i]); continue; }
        DIR *d = opendir(argv[i]);
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            char p[1024];
            snprintf(p, sizeof p, "%s/%s", argv[i], de->d_name);
            run_file(p);
        }
        closedir(d);
    }
    printf("fuzz_nv_replay: %d runs, no bugs\n", runs);
    return 0;
}
#endif
