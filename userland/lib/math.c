/* Halcyon float math: see halmath.h.
 *
 * Every float function widens to double, works there (range reduction,
 * polynomial or series, scaling by a power of two built from bits) and
 * rounds once at the end: double's 29 extra bits keep the result within one
 * ulp of float without the careful tricks a double library needs. The
 * series are plain Taylor / atanh series over small reduced ranges, with
 * enough terms that their truncation error is below 1e-15.
 *
 * Builds unchanged for Halcyon (static library libm.a) and the host
 * (tests/gl: mathtest compares it with the host's library; libgl's host
 * build links it so that host and Halcyon render the same pictures). */
#include "halmath.h"

typedef union { float f; uint32_t u; } fbits_t;
typedef union { double d; uint64_t u; } dbits_t;

/* ---------------- exact operations ---------------- */
float sqrtf(float x) { float r; __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x)); return r; }
double sqrt(double x) { double r; __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x)); return r; }
float fabsf(float x) { fbits_t b = { x }; b.u &= 0x7FFFFFFFu; return b.f; }
double fabs(double x) { dbits_t b = { x }; b.u &= 0x7FFFFFFFFFFFFFFFull; return b.d; }
float copysignf(float x, float y) {
    fbits_t a = { x }, b = { y };
    a.u = (a.u & 0x7FFFFFFFu) | (b.u & 0x80000000u);
    return a.f;
}
static double copysign_d(double x, double y) {
    dbits_t a = { x }, b = { y };
    a.u = (a.u & 0x7FFFFFFFFFFFFFFFull) | (b.u & 0x8000000000000000ull);
    return a.d;
}

/* rounding: values of 2^23 (float) / 2^52 (double) and more, infinities and
 * NaNs are integers already; the rest fit in a long long */
float truncf(float x) {
    if (!(fabsf(x) < 8388608.0f)) return x;
    return copysignf((float)(long long)x, x);
}
float floorf(float x) {
    if (!(fabsf(x) < 8388608.0f)) return x;
    float t = (float)(long long)x;
    if (t > x) t -= 1.0f;
    return copysignf(t, x);
}
float ceilf(float x) {
    if (!(fabsf(x) < 8388608.0f)) return x;
    float t = (float)(long long)x;
    if (t < x) t += 1.0f;
    return copysignf(t, x);
}
float roundf(float x) {
    if (!(fabsf(x) < 8388608.0f)) return x;
    float t = truncf(x);
    if (fabsf(x - t) >= 0.5f) t += copysignf(1.0f, x);
    return copysignf(t, x);
}
long lroundf(float x) { return (long)roundf(x); }
double floor(double x) {
    if (!(fabs(x) < 4503599627370496.0)) return x;
    double t = (double)(long long)x;
    if (t > x) t -= 1.0;
    return copysign_d(t, x);
}
double ceil(double x) {
    if (!(fabs(x) < 4503599627370496.0)) return x;
    double t = (double)(long long)x;
    if (t < x) t += 1.0;
    return copysign_d(t, x);
}

float fminf(float a, float b) { return a != a ? b : b != b ? a : a < b ? a : b; }
float fmaxf(float a, float b) { return a != a ? b : b != b ? a : a > b ? a : b; }

/* x * 2^e for any e (double has room for every float result) */
static double scale2(double x, int e) {
    while (e > 1000) { x *= 0x1p1000; e -= 1000; }
    while (e < -1000) { x *= 0x1p-1000; e += 1000; }
    dbits_t b;
    b.u = (uint64_t)(e + 1023) << 52;
    return x * b.d;
}
float ldexpf(float x, int e) {
    if (e > 400) e = 400;                  /* (any finite float overflows by then) */
    if (e < -400) e = -400;
    return (float)scale2(x, e);
}
float frexpf(float x, int *e) {
    *e = 0;
    if (x == 0 || x != x || fabsf(x) == INFINITY) return x;
    dbits_t b = { (double)x };
    int ex = (int)((b.u >> 52) & 0x7FF);
    *e = ex - 1022;
    b.u = (b.u & ~(0x7FFull << 52)) | (1022ull << 52);
    return (float)b.d;
}

/* exact: repeated subtraction of y * 2^k (each step is exact) */
float fmodf(float x, float y) {
    if (y == 0 || x != x || y != y || fabsf(x) == INFINITY) return NAN;
    double ax = fabs((double)x), ay = fabs((double)y);
    if (ay == INFINITY) return x;
    while (ax >= ay) {
        double t = ay;
        int ex = (int)((((dbits_t){ ax }).u >> 52) & 0x7FF) - (int)((((dbits_t){ ay }).u >> 52) & 0x7FF);
        if (ex > 1) t = scale2(ay, ex - 1);
        while (t * 2 <= ax) t *= 2;
        ax -= t;
    }
    return copysignf((float)ax, x);
}

float hypotf(float x, float y) { double a = x, b = y; return (float)sqrt(a * a + b * b); }

/* ---------------- sine and cosine ---------------- */
/* pi/2 in three parts: 33 bits, 33 bits, the rest (k * part is exact for
 * |k| < 2^20) */
static const double PIO2_1 = 1.5707963267341256, PIO2_2 = 6.077100506303966e-11, PIO2_3 = 2.0222662487959506e-21;
static const double TWO_OVER_PI = 0.6366197723675814, PIO2_D_FULL = 1.5707963267948966;

/* the bits of 2/pi after the binary point, most significant first */
static const uint32_t TWO_OVER_PI_BITS[8] = {
    0xA2F9836E, 0x4E441529, 0xFC2757D1, 0xF534DDC0, 0xDB629599, 0x3C439041, 0xFE5163AB, 0xDEBBC561,
};
static uint32_t two_over_pi_32(int pos) {             /* bits pos .. pos + 31 (bit 1 = the first) */
    int i = (pos - 1) >> 5, off = (pos - 1) & 31;
    return off ? TWO_OVER_PI_BITS[i] << off | TWO_OVER_PI_BITS[i + 1] >> (32 - off) : TWO_OVER_PI_BITS[i];
}

/* r = x - k pi/2 with |r| <= pi/4; returns k mod 4. x is a float's value.
 * Up to 2^20 a three-part pi/2 is exact enough; beyond, x = M 2^E (M an
 * integer of 24 bits) is multiplied by the 96 bits of 2/pi that matter
 * (earlier bits only add multiples of 4, later ones less than 2^-70). */
static int reduce_pio2(double x, double *r) {
    if (fabs(x) <= 0.7853981633974483) { *r = x; return 0; }
    if (fabs(x) < 1048576.0) {
        double k = floor(x * TWO_OVER_PI + 0.5);
        *r = ((x - k * PIO2_1) - k * PIO2_2) - k * PIO2_3;
        return (int)(k - 4 * floor(k * 0.25));        /* k mod 4 */
    }
    dbits_t b = { fabs(x) };
    int E = (int)((b.u >> 52) & 0x7FF) - 1023 - 23;
    uint64_t M = ((b.u & ((1ull << 52) - 1)) | (1ull << 52)) >> 29;
    int i0 = E - 1 > 1 ? E - 1 : 1;
    unsigned __int128 w = (unsigned __int128)two_over_pi_32(i0) << 64 | (unsigned __int128)two_over_pi_32(i0 + 32) << 32 |
                          two_over_pi_32(i0 + 64);
    unsigned __int128 p = w * M;
    int fracbits = i0 + 95 - E;                       /* p = x 2/pi (mod 4) * 2^fracbits */
    int k = (int)(p >> fracbits) & 3;
    double f = (double)(uint64_t)((p << (128 - fracbits)) >> 64) * 0x1p-64;
    if (f >= 0.5) { f -= 1; k = (k + 1) & 3; }
    *r = f * PIO2_D_FULL;
    if (x < 0) { *r = -*r; k = (4 - k) & 3; }
    return k;
}

/* Taylor series on |r| <= pi/4 (truncation error < 5e-17) */
static double sin_poly(double r) {
    double z = r * r;
    return r + r * z * (-1.0 / 6 + z * (1.0 / 120 + z * (-1.0 / 5040 + z * (1.0 / 362880 + z * (-1.0 / 39916800 +
               z * (1.0 / 6227020800.0 + z * (-1.0 / 1307674368000.0)))))));
}
static double cos_poly(double r) {
    double z = r * r;
    return 1 + z * (-0.5 + z * (1.0 / 24 + z * (-1.0 / 720 + z * (1.0 / 40320 + z * (-1.0 / 3628800 +
               z * (1.0 / 479001600 + z * (-1.0 / 87178291200.0 + z * (1.0 / 20922789888000.0))))))));
}
static void sincos_d(double x, double *s, double *c) {
    double r;
    int q = reduce_pio2(x, &r);
    double sr = sin_poly(r), cr = cos_poly(r);
    switch (q) {
    case 0: *s = sr; *c = cr; break;
    case 1: *s = cr; *c = -sr; break;
    case 2: *s = -sr; *c = -cr; break;
    default: *s = -cr; *c = sr; break;
    }
}
float sinf(float x) {
    if (fabsf(x) == INFINITY || x != x) return NAN;
    if (fabsf(x) < 1e-4f) return x;                  /* (x^3 / 6 is below half an ulp) */
    double s, c;
    sincos_d(x, &s, &c);
    return (float)s;
}
float cosf(float x) {
    if (fabsf(x) == INFINITY || x != x) return NAN;
    double s, c;
    sincos_d(x, &s, &c);
    return (float)c;
}
void sincosf(float x, float *s, float *c) {
    if (fabsf(x) == INFINITY || x != x) { *s = *c = NAN; return; }
    double sd, cd;
    sincos_d(x, &sd, &cd);
    *s = (float)sd;
    *c = (float)cd;
}
float tanf(float x) {
    if (fabsf(x) == INFINITY || x != x) return NAN;
    double s, c;
    sincos_d(x, &s, &c);
    return (float)(s / c);
}

/* ---------------- arc tangent family ---------------- */
static const double PI_D = 3.141592653589793, PIO2_D = 1.5707963267948966, PIO6_D = 0.5235987755982989;
static const double SQRT3_D = 1.7320508075688772, TAN_PI12 = 0.2679491924311227;

/* atan for x >= 0: atan(1/x) = pi/2 - atan(x) brings x to [0, 1], then
 * atan(x) = pi/6 + atan((x sqrt3 - 1) / (sqrt3 + x)) to |x| <= tan(pi/12),
 * where the series x - x^3/3 + x^5/5 ... converges fast (13 terms) */
static double atan_pos(double x) {
    int inv = x > 1;
    if (inv) x = 1 / x;
    double off = 0;
    if (x > TAN_PI12) {
        x = (x * SQRT3_D - 1) / (SQRT3_D + x);
        off = PIO6_D;
    }
    double z = x * x, sum = 0;
    for (int k = 25; k >= 3; k -= 2) sum = z * ((((k / 2) & 1) ? -1.0 : 1.0) / k + sum);
    double r = off + x + x * sum;
    return inv ? PIO2_D - r : r;
}
static double atan_d(double x) { return signbit(x) ? -atan_pos(-x) : atan_pos(x); }

float atanf(float x) {
    if (x != x) return x;
    if (fabsf(x) == INFINITY) return copysignf((float)PIO2_D, x);
    return (float)atan_d(x);
}
static double atan2_d(double y, double x) {
    if (x == 0 && y == 0) return signbit(x) ? copysign_d(PI_D, y) : copysign_d(0.0, y);
    if (fabs(x) == INFINITY || fabs(y) == INFINITY) {
        double ax = fabs(x) == INFINITY ? copysign_d(1, x) : 0, ay = fabs(y) == INFINITY ? copysign_d(1, y) : 0;
        if (ax == 0) return copysign_d(PIO2_D, y);
        if (ay == 0) return ax > 0 ? copysign_d(0.0, y) : copysign_d(PI_D, y);
        x = ax; y = ay;
    }
    double r;
    if (fabs(y) <= fabs(x)) {
        r = atan_d(y / x);
        if (x < 0) r += y < 0 || (y == 0 && signbit(y)) ? -PI_D : PI_D;
    } else {
        r = copysign_d(PIO2_D, y) - atan_d(x / y);
    }
    return r;
}
float atan2f(float y, float x) {
    if (x != x || y != y) return NAN;
    return (float)atan2_d(y, x);
}
float asinf(float x) {
    if (!(fabsf(x) <= 1)) return NAN;
    double d = x;
    return (float)atan2_d(d, sqrt(1 - d * d));       /* (x * x is exact in double) */
}
float acosf(float x) {
    if (!(fabsf(x) <= 1)) return NAN;
    double d = x;
    return (float)atan2_d(sqrt(1 - d * d), d);
}

/* ---------------- exponentials and logarithms ---------------- */
static const double LN2_D = 0.6931471805599453, LOG2E_D = 1.4426950408889634, LOG10_2_D = 0.30102999566398120;

/* 2^t for any double t a float result can come from */
static double exp2_d(double t) {
    if (t > 1100) return INFINITY;
    if (t < -1100) return 0;
    double n = floor(t + 0.5), f = (t - n) * LN2_D;       /* |f| <= ln2 / 2 */
    double e = 1 + f * (1 + f * (0.5 + f * (1.0 / 6 + f * (1.0 / 24 + f * (1.0 / 120 + f * (1.0 / 720 +
               f * (1.0 / 5040 + f * (1.0 / 40320 + f * (1.0 / 362880 + f * (1.0 / 3628800 + f * (1.0 / 39916800)))))))))));
    return scale2(e, (int)n);
}
/* log2 of a finite positive double: x = m 2^e with m in [sqrt(1/2), sqrt(2)),
 * ln m = 2 atanh((m - 1) / (m + 1)) */
static double log2_d(double x) {
    dbits_t b = { x };
    int e = (int)((b.u >> 52) & 0x7FF);
    if (e == 0) { b.d = x * 0x1p60; e = (int)((b.u >> 52) & 0x7FF) - 60; }    /* subnormal */
    e -= 1023;
    b.u = (b.u & ~(0x7FFull << 52)) | (1023ull << 52);
    double m = b.d;
    if (m > 1.4142135623730951) { m *= 0.5; e++; }
    double s = (m - 1) / (m + 1), z = s * s;
    double ln = 2 * s * (1 + z * (1.0 / 3 + z * (1.0 / 5 + z * (1.0 / 7 + z * (1.0 / 9 + z * (1.0 / 11 +
                z * (1.0 / 13 + z * (1.0 / 15 + z * (1.0 / 17)))))))));
    return e + ln * LOG2E_D;
}

float expf(float x) {
    if (x != x) return x;
    if (x > 89) return INFINITY;
    if (x < -104) return 0;
    return (float)exp2_d((double)x * LOG2E_D);
}
float exp2f(float x) {
    if (x != x) return x;
    if (x > 129) return INFINITY;
    if (x < -150) return 0;
    return (float)exp2_d(x);
}
static float log_special(float x, int *done) {
    *done = 1;
    if (x != x) return x;
    if (x == 0) return -INFINITY;
    if (x < 0) return NAN;
    if (x == INFINITY) return x;
    *done = 0;
    return 0;
}
float logf(float x) { int d; float s = log_special(x, &d); return d ? s : (float)(log2_d(x) * LN2_D); }
float log2f(float x) { int d; float s = log_special(x, &d); return d ? s : (float)log2_d(x); }
float log10f(float x) { int d; float s = log_special(x, &d); return d ? s : (float)(log2_d(x) * LOG10_2_D); }

static int is_int_f(float y) { return truncf(y) == y; }
static int is_odd_f(float y) { return is_int_f(y) && fabsf(y) < 16777216.0f && ((long long)y & 1); }

float powf(float x, float y) {
    if (y == 0 || x == 1) return 1;
    if (x != x || y != y) return NAN;
    if (fabsf(y) == INFINITY) {
        float ax = fabsf(x);
        if (ax == 1) return 1;
        return (ax > 1) == (y > 0) ? INFINITY : 0;
    }
    if (x == 0) {
        if (y > 0) return is_odd_f(y) ? x : 0;
        return is_odd_f(y) ? copysignf(INFINITY, x) : INFINITY;
    }
    int neg = 0;
    if (x < 0) {
        if (!is_int_f(y)) return NAN;
        neg = is_odd_f(y);
        x = -x;
    }
    if (x == INFINITY) return neg ? (y > 0 ? -INFINITY : -0.0f) : (y > 0 ? INFINITY : 0);
    double r = exp2_d((double)y * log2_d(x));
    return (float)(neg ? -r : r);
}

float cbrtf(float x) {
    if (x == 0 || x != x || fabsf(x) == INFINITY) return x;
    double a = fabs((double)x), r = exp2_d(log2_d(a) / 3);
    r = r - (r * r * r - a) / (3 * r * r);                  /* one Newton step */
    return copysignf((float)r, x);
}

float sinhf(float x) {
    if (x != x) return x;
    double d = x;
    if (fabs(d) < 1e-5) return x;
    if (fabs(d) > 90) return copysignf(INFINITY, x);
    double e = exp2_d(d * LOG2E_D);
    return (float)((e - 1 / e) * 0.5);
}
float coshf(float x) {
    if (x != x) return x;
    double d = fabs((double)x);
    if (d > 90) return INFINITY;
    double e = exp2_d(d * LOG2E_D);
    return (float)((e + 1 / e) * 0.5);
}
float tanhf(float x) {
    if (x != x) return x;
    double d = x;
    if (fabs(d) < 1e-5) return x;
    if (fabs(d) > 20) return copysignf(1.0f, x);
    double e = exp2_d(2 * d * LOG2E_D);
    return (float)((e - 1) / (e + 1));
}
