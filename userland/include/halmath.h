/* Halcyon float math (userland/lib/math.c, in libm.a: linked into every
 * program, taken only by the programs that call it). hal.h includes this.
 *
 * The float functions use the C names and are computed in double precision
 * inside, so they are correctly rounded or within one unit in the last place
 * (ulp) of the exact result over the ranges graphics and games use; tests/gl
 * (mathtest) checks that against the host's double-precision library.
 * sinf / cosf / tanf reduce their argument with a three-part pi/2: within
 * 1 ulp for |x| < 2^20, less accurate (but finite) beyond.
 *
 * No errno and no floating-point exceptions are raised on purpose; domain
 * errors give NaN, overflow gives +-INFINITY, as C's functions do.
 *
 * The file includes nothing but <stdint.h>, so it also builds on the host. */
#ifndef HALMATH_H
#define HALMATH_H
#include <stdint.h>

#ifndef M_PI
#define M_E        2.71828182845904523536
#define M_LOG2E    1.44269504088896340736
#define M_LOG10E   0.434294481903251827651
#define M_LN2      0.693147180559945309417
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.785398163397448309616
#define M_1_PI     0.318309886183790671538
#define M_2_PI     0.636619772367581343076
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.707106781186547524401
#endif
#ifndef INFINITY
#define INFINITY  (__builtin_inff())
#define NAN       (__builtin_nanf(""))
#define HUGE_VALF (__builtin_inff())
#define HUGE_VAL  (__builtin_inf())
#endif
#ifndef isnan
#define isnan(x)    __builtin_isnan(x)
#define isinf(x)    __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x)  __builtin_signbit(x)
#endif

/* exact (one instruction or bit operations) */
float sqrtf(float x);
float fabsf(float x);
float floorf(float x);
float ceilf(float x);
float truncf(float x);
float roundf(float x);                /* halfway cases away from zero */
long  lroundf(float x);
float fmodf(float x, float y);        /* exact, the sign of x */
float copysignf(float x, float y);
float fminf(float a, float b);
float fmaxf(float a, float b);
float ldexpf(float x, int e);
float frexpf(float x, int *e);        /* x = m * 2^e, 0.5 <= |m| < 1 */
float hypotf(float x, float y);
double sqrt(double x);
double fabs(double x);
double floor(double x);
double ceil(double x);

/* within 1 ulp (see above) */
float sinf(float x);
float cosf(float x);
void  sincosf(float x, float *s, float *c);
float tanf(float x);
float asinf(float x);
float acosf(float x);
float atanf(float x);
float atan2f(float y, float x);
float expf(float x);
float exp2f(float x);
float logf(float x);
float log2f(float x);
float log10f(float x);
float powf(float x, float y);
float cbrtf(float x);
float sinhf(float x);
float coshf(float x);
float tanhf(float x);

#endif
