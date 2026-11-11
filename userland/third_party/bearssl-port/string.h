/* Halcyon: the part of <string.h> BearSSL uses, from the user-mode C library
 * (userland/lib/libc.c). BearSSL is compiled for Halcyon with -nostdinc, so
 * these headers and the compiler's own freestanding ones (stddef.h,
 * stdint.h, limits.h) are all it sees. */
#ifndef HALCYON_BEARSSL_STRING_H
#define HALCYON_BEARSSL_STRING_H
#include <stddef.h>
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
#endif
