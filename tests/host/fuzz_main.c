/* Replay driver for the storage stack fuzz harness (fuzz_fs.c), for builds
 * without libFuzzer: runs every file given on the command line (directories
 * are expanded one level) through the harness. A bug aborts the process;
 * the input being run is printed first.
 *
 *   fuzz_replay [-v] FILE|DIR...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "fuzz_fs.h"

static int g_runs;

static void run_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(n > 0 ? (size_t)n : 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "short read %s\n", path); exit(2); }
    fclose(f);
    if (fuzz_verbose) fprintf(stderr, "run %s (%ld bytes)\n", path, n);
    else fprintf(stderr, "%s\n", path);    /* so a crash names its input */
    fuzz_fs_run(b, (size_t)n);
    free(b);
    g_runs++;
}

static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int main(int argc, char **argv) {
    int first = 1;
    if (argc > 1 && !strcmp(argv[1], "-v")) { fuzz_verbose = 1; first = 2; }
    for (int i = first; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st)) { fprintf(stderr, "cannot stat %s\n", argv[i]); return 2; }
        if (!S_ISDIR(st.st_mode)) { run_file(argv[i]); continue; }
        DIR *d = opendir(argv[i]);
        struct dirent *de;
        char **names = NULL;
        int n = 0;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            names = realloc(names, sizeof *names * (n + 1));
            names[n] = malloc(strlen(argv[i]) + strlen(de->d_name) + 2);
            sprintf(names[n++], "%s/%s", argv[i], de->d_name);
        }
        closedir(d);
        if (n) qsort(names, n, sizeof *names, cmp);
        for (int k = 0; k < n; k++) { run_file(names[k]); free(names[k]); }
        free(names);
    }
    printf("fuzz_replay: %d input(s), no bugs\n", g_runs);
    return 0;
}
