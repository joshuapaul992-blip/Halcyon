#ifndef TYPES_H
#define TYPES_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;
typedef uintptr_t uptr;

#define PACKED __attribute__((packed))
#define ALIGNED(x) __attribute__((aligned(x)))
#define UNUSED __attribute__((unused))
#define NORETURN __attribute__((noreturn))

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((__typeof__(x))(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((__typeof__(x))(a) - 1))

/* generic error codes (negative return values) */
#define E_OK       0
#define E_IO      -1
#define E_NOMEM   -2
#define E_NOENT   -3
#define E_EXIST   -4
#define E_NOTDIR  -5
#define E_ISDIR   -6
#define E_NOSPC   -7
#define E_INVAL   -8
#define E_TIMEOUT -9
#define E_STALL   -10
#define E_NOTSUP  -11
#define E_AGAIN   -12
#define E_NOTEMPTY -13
#define E_ROFS    -14
#define E_NODEV   -15
#define E_BUSY    -16
#define E_NAMETOOLONG -17

const char *strerror(int err);

#endif
