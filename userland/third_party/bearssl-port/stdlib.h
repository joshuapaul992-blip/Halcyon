/* Halcyon: BearSSL includes <stdlib.h> in a few files (debug leftovers and
 * the compiler's intrinsics headers); this is all they get. */
#ifndef HALCYON_BEARSSL_STDLIB_H
#define HALCYON_BEARSSL_STDLIB_H
#include <stddef.h>
void *malloc(size_t n);
void  free(void *p);
#endif
