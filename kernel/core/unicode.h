#ifndef UNICODE_H
#define UNICODE_H
/* Halcyon OS - Unicode helpers.
 *
 * All text in the kernel (file names, the GUI, files opened in Notepad) is
 * UTF-8. Decoding is lenient: a byte that does not start a valid UTF-8
 * sequence decodes as the Latin-1 character of the same value, so legacy
 * 8-bit text stays readable and every byte string can be displayed. */
#include "types.h"

#define UC_REPLACEMENT 0xFFFDu

/* decode the character at *s and advance; returns 0 at the terminating NUL
 * (without advancing) */
u32 utf8_next(const char **s);
/* the same within a length-limited buffer: decodes s[*pos..len) */
u32 utf8_get(const char *s, int len, int *pos);
/* encode cp (surrogates and > 0x10FFFF become U+FFFD); returns 1..4 bytes */
int utf8_put(u32 cp, char *out);
/* strictly valid UTF-8 (no overlong forms, surrogates or > 0x10FFFF) */
int utf8_valid(const char *s);
/* characters in the first n bytes of s (or up to the NUL) */
int utf8_count(const char *s, int n);
/* byte offset of character number ncp (clamped to the string end) */
int utf8_offset(const char *s, int ncp);
/* cursor movement inside s[0..len): start of the previous / next character */
int utf8_prev(const char *s, int pos);
int utf8_after(const char *s, int len, int pos);

/* simple uppercase mapping (ASCII, Latin-1, Latin Extended-A, Greek,
 * Cyrillic, full-width Latin); other characters map to themselves */
u32 uc_upper(u32 cp);
/* case-insensitive comparison of two UTF-8 strings */
int utf8_casecmp(const char *a, const char *b);

/* UTF-16 (little-endian code units) <-> UTF-8.
 * utf16_to_utf8: n units (stops early at 0); returns the byte length or -1
 * if the result does not fit in outsz-1 bytes or holds an unpaired
 * surrogate (strict) / a character < 0x20.
 * utf8_to_utf16: returns the unit count or -1 if it exceeds max units. */
int utf16_to_utf8(const u16 *in, int n, char *out, int outsz);
int utf8_to_utf16(const char *in, u16 *out, int max);

/* IBM code page 437 (the usual OEM code page of FAT short names) */
u32 cp437_to_uc(u8 c);

#endif
