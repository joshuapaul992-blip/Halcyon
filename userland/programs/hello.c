/* HELLO.EXE - a console program: prints, reads a line, lists a directory */
#include "hal.h"

int main(int argc, char **argv) {
    printf("Hello from user mode! I am process %d, running %s.\n", getpid(), argv[0]);
    for (int i = 1; i < argc; i++) printf("  argument %d: %s\n", i, argv[i]);
    char cwd[260];
    getcwd(cwd, sizeof cwd);
    hal_dirent_t ents[64];
    int n = readdir(cwd, ents, 64);
    printf("%s has %d entries:", cwd, n);
    for (int i = 0; i < n && i < 8; i++) printf(" %s%s", ents[i].name, ents[i].is_dir ? "/" : "");
    printf("%s\n", n > 8 ? " ..." : "");
    char *big = malloc(1 << 20);
    printf("malloc(1 MiB) = %p\n", big);
    if (argc > 1 && !strcmp(argv[1], "/ask")) {
        char name[64];
        printf("What is your name? ");
        if (getline(name, sizeof name) >= 0) printf("Nice to meet you, %s.\n", name);
    }
    return 7;
}
