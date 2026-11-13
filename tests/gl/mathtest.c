/* Host test of Halcyon's float math (userland/lib/math.c), with ASan and
 * UBSan: every function against the host's double-precision library over
 * the ranges graphics uses (random and edge values), measured in units in
 * the last place (ulp) of the float result; the exact ones (floorf,
 * fmodf...) must be exact, and the special values (NaN, infinities, signed
 * zeros, domain errors) must come out as C says.
 *   mathtest [-v]        prints the worst error of each function */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* math.c's functions under other names, next to the host's */
#define sqrtf hm_sqrtf
#define sqrt hm_sqrt
#define fabsf hm_fabsf
#define fabs hm_fabs
#define floorf hm_floorf
#define floor hm_floor
#define ceilf hm_ceilf
#define ceil hm_ceil
#define truncf hm_truncf
#define roundf hm_roundf
#define lroundf hm_lroundf
#define fmodf hm_fmodf
#define copysignf hm_copysignf
#define fminf hm_fminf
#define fmaxf hm_fmaxf
#define ldexpf hm_ldexpf
#define frexpf hm_frexpf
#define hypotf hm_hypotf
#define sinf hm_sinf
#define cosf hm_cosf
#define sincosf hm_sincosf
#define tanf hm_tanf
#define asinf hm_asinf
#define acosf hm_acosf
#define atanf hm_atanf
#define atan2f hm_atan2f
#define expf hm_expf
#define exp2f hm_exp2f
#define logf hm_logf
#define log2f hm_log2f
#define log10f hm_log10f
#define powf hm_powf
#define cbrtf hm_cbrtf
#define sinhf hm_sinhf
#define coshf hm_coshf
#define tanhf hm_tanhf
#include "../../userland/lib/math.c"

static int checks, fails, verbose;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static double urand(void) {           /* [0, 1) */
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (double)(rng >> 11) * 0x1p-53;
}
static float frange(double lo, double hi) { return (float)(lo + (hi - lo) * urand()); }
/* log-uniform magnitude, random sign */
static float flog(double lo, double hi, int sign) {
    float v = (float)exp(log(lo) + (log(hi) - log(lo)) * urand());
    return sign && urand() < 0.5 ? -v : v;
}

/* error of r in ulps of the float nearest to ref */
static double ulps(float r, double ref) {
    if (isnan(ref)) return isnan(r) ? 0 : 1e9;
    if (isinf(ref)) return r == (float)ref ? 0 : 1e9;
    if ((float)ref == r) return 0;
    if (isinf(r) || isnan(r)) return 1e9;
    int e;
    frexp(ref == 0 ? 0x1p-149 : ref, &e);
    double ulp = ldexp(1.0, e - 24);
    if (ulp < 0x1p-149) ulp = 0x1p-149;
    return __builtin_fabs((double)r - ref) / ulp;
}

typedef struct { const char *name; double worst; float at; float at2; int n; } stat_t;
static void track(stat_t *s, float r, double ref, float x, float y) {
    double u = ulps(r, ref);
    s->n++;
    if (u > s->worst) { s->worst = u; s->at = x; s->at2 = y; }
}
static void report(stat_t *s, double limit) {
    if (verbose) printf("  %-8s %8d values, worst %.3f ulp (at %.9g, %.9g)\n", s->name, s->n, s->worst, s->at, s->at2);
    CHECK(s->worst <= limit, "%s: %.3f ulp at %.9g, %.9g (limit %.1f)", s->name, s->worst, s->at, s->at2, limit);
}

#define N 200000
#define UNARY(fn, ref, gen, limit) do { \
        stat_t s = { #fn, 0, 0, 0, 0 }; \
        for (int i = 0; i < N; i++) { float x = gen; track(&s, hm_##fn(x), ref((double)x), x, 0); } \
        report(&s, limit); \
    } while (0)

static double ref_exp2(double x) { return exp2(x); }
static double sqrt_ref(double x) { return __builtin_sqrt(x); }

static void accuracy(void) {
    /* the measure itself */
    CHECK(ulps(nextafterf(1, 2), 1.0) == 1 && ulps(1, 1.0) == 0 && ulps(nextafterf(-3, 0), -3.0) == 1 && ulps(1.5f, 1.25) == 2097152, "ulp measure");
    UNARY(sinf, sin, frange(-7, 7), 1.0);
    UNARY(sinf, sin, flog(1e-6, 1e5, 1), 1.0);
    UNARY(cosf, cos, frange(-7, 7), 1.0);
    UNARY(cosf, cos, flog(1e-6, 1e5, 1), 1.0);
    UNARY(sinf, sin, flog(1e5, 3e38, 1), 1.0);            /* the long reduction */
    UNARY(cosf, cos, flog(1e5, 3e38, 1), 1.0);
    UNARY(tanf, tan, frange(-1.55, 1.55), 1.0);
    UNARY(tanf, tan, flog(1e-6, 1e4, 1), 1.0);
    UNARY(asinf, asin, frange(-1, 1), 1.0);
    UNARY(acosf, acos, frange(-1, 1), 1.0);
    UNARY(atanf, atan, flog(1e-8, 1e8, 1), 1.0);
    UNARY(expf, exp, frange(-103, 88.7), 1.0);
    UNARY(expf, exp, frange(-2, 2), 1.0);
    UNARY(exp2f, ref_exp2, frange(-149, 127.9), 1.0);
    UNARY(logf, log, flog(1e-38, 3e38, 0), 1.0);
    UNARY(logf, log, frange(0.5, 2), 1.0);
    UNARY(log2f, log2, flog(1e-38, 3e38, 0), 1.0);
    UNARY(log10f, log10, flog(1e-38, 3e38, 0), 1.0);
    UNARY(sqrtf, sqrt_ref, flog(1e-38, 3e38, 0), 0.5);
    UNARY(cbrtf, cbrt, flog(1e-30, 1e30, 1), 1.0);
    UNARY(sinhf, sinh, frange(-80, 80), 1.0);
    UNARY(coshf, cosh, frange(-80, 80), 1.0);
    UNARY(tanhf, tanh, frange(-12, 12), 1.0);
    {   /* atan2 in all four quadrants */
        stat_t s = { "atan2f", 0, 0, 0, 0 };
        for (int i = 0; i < N; i++) {
            float y = flog(1e-6, 1e6, 1), x = flog(1e-6, 1e6, 1);
            track(&s, hm_atan2f(y, x), atan2(y, x), y, x);
        }
        report(&s, 1.0);
    }
    {   /* pow: bases and exponents of lighting (shininess), gamma, fog */
        stat_t s = { "powf", 0, 0, 0, 0 };
        for (int i = 0; i < N; i++) {
            float x = frange(0, 1), y = frange(0, 128);
            track(&s, hm_powf(x, y), pow(x, y), x, y);
            x = flog(1e-10, 1e10, 0); y = frange(-3.5, 3.5);
            track(&s, hm_powf(x, y), pow(x, y), x, y);
            x = (float)(int)frange(-30, 30); y = (float)(int)frange(-20, 20);
            track(&s, hm_powf(x, y), pow(x, y), x, y);
        }
        report(&s, 1.0);
    }
    {   /* hypot */
        stat_t s = { "hypotf", 0, 0, 0, 0 };
        for (int i = 0; i < N; i++) {
            float x = flog(1e-15, 1e15, 1), y = flog(1e-15, 1e15, 1);
            track(&s, hm_hypotf(x, y), hypot(x, y), x, y);
        }
        report(&s, 1.0);
    }
}

/* exact functions: bit for bit what the host's give */
static int same(float a, float b) {
    if (isnan(a) && isnan(b)) return 1;
    return memcmp(&a, &b, sizeof a) == 0;
}
static void exact(void) {
    int bad = 0, n = 0;
    float bad_x = 0;
    for (int i = 0; i < N * 2; i++) {
        float x = i < 1000 ? (float)(i - 500) * 0.25f : i < 2000 ? flog(1e6, 3e38, 1) : flog(1e-7, 1e7, 1);
        float y = flog(1e-5, 1e5, 1);
        n++;
        if (!same(hm_floorf(x), __builtin_floorf(x)) || !same(hm_ceilf(x), __builtin_ceilf(x)) ||
            !same(hm_truncf(x), __builtin_truncf(x)) || !same(hm_roundf(x), __builtin_roundf(x)) ||
            !same(hm_fabsf(x), __builtin_fabsf(x)) || !same(hm_fmodf(x, y), __builtin_fmodf(x, y)) ||
            hm_floor((double)x * 1.5) != __builtin_floor((double)x * 1.5) ||
            hm_ceil((double)x * 1.5) != __builtin_ceil((double)x * 1.5)) {
            if (!bad) bad_x = x;
            bad++;
        }
        int e1, e2;
        float m1 = hm_frexpf(x, &e1), m2 = __builtin_frexpf(x, &e2);
        if (!same(m1, m2) || e1 != e2 || !same(hm_ldexpf(m1, e1), x)) { if (!bad) bad_x = x; bad++; }
    }
    if (verbose) printf("  exact    %8d values: floor ceil trunc round fabs fmod frexp ldexp\n", n);
    CHECK(!bad, "%d exact results differ (first at %.9g)", bad, bad_x);
    /* fmod with huge quotients, both signs */
    CHECK(same(hm_fmodf(3e38f, 7.0f), __builtin_fmodf(3e38f, 7.0f)), "fmodf(3e38, 7)");
    CHECK(same(hm_fmodf(-1e30f, 0.3f), __builtin_fmodf(-1e30f, 0.3f)), "fmodf(-1e30, 0.3)");
    CHECK(same(hm_fmodf(5.5f, -2.0f), 1.5f), "fmodf(5.5, -2)");
    CHECK(same(hm_floorf(-0.0f), -0.0f) && same(hm_ceilf(-0.5f), -0.0f) && same(hm_roundf(-0.4f), -0.0f), "signed zeros");
    CHECK(hm_roundf(2.5f) == 3.0f && hm_roundf(-2.5f) == -3.0f && hm_lroundf(-7.5f) == -8, "round halfway away from zero");
    CHECK(hm_fminf(NAN, 2) == 2 && hm_fmaxf(1, NAN) == 1 && hm_fminf(-1, 3) == -1 && hm_fmaxf(-1, 3) == 3, "fmin / fmax");
    CHECK(same(hm_copysignf(3, -0.0f), -3.0f) && same(hm_copysignf(-2, 1), 2.0f), "copysignf");
}

static void special(void) {
    CHECK(isnan(hm_sinf(INFINITY)) && isnan(hm_cosf(-INFINITY)) && isnan(hm_tanf(NAN)), "sin / cos / tan of inf, NaN");
    CHECK(same(hm_sinf(-0.0f), -0.0f) && hm_cosf(0) == 1, "sin(-0), cos(0)");
    CHECK(isnan(hm_asinf(1.5f)) && isnan(hm_acosf(-1.01f)), "asin / acos domain");
    CHECK(hm_asinf(1) == (float)M_PI_2 && hm_acosf(-1) == (float)M_PI, "asin(1), acos(-1)");
    CHECK(hm_atanf(INFINITY) == (float)M_PI_2 && hm_atanf(-INFINITY) == -(float)M_PI_2, "atan(inf)");
    CHECK(hm_atan2f(0, -1) == (float)M_PI && hm_atan2f(-0.0f, -1) == -(float)M_PI && same(hm_atan2f(-0.0f, 1), -0.0f), "atan2 on the axes");
    CHECK(hm_atan2f(1, 0) == (float)M_PI_2 && hm_atan2f(-1, 0) == -(float)M_PI_2 && hm_atan2f(INFINITY, INFINITY) == (float)M_PI_4, "atan2 at x = 0, inf");
    CHECK(hm_expf(100) == INFINITY && hm_expf(-200) == 0 && hm_expf(0) == 1 && isnan(hm_expf(NAN)), "expf limits");
    CHECK(hm_expf(-INFINITY) == 0 && hm_expf(INFINITY) == INFINITY, "expf(+-inf)");
    CHECK(hm_logf(0) == -INFINITY && isnan(hm_logf(-1)) && hm_logf(1) == 0 && hm_logf(INFINITY) == INFINITY, "logf limits");
    CHECK(hm_log2f(1024) == 10 && hm_log10f(1000) == 3 && hm_log2f(0x1p-149f) == -149, "exact logarithms");
    CHECK(hm_powf(0, 0) == 1 && hm_powf(NAN, 0) == 1 && hm_powf(1, NAN) == 1, "pow(x, 0), pow(1, y)");
    CHECK(isnan(hm_powf(-2, 0.5f)) && hm_powf(-2, 3) == -8 && hm_powf(-2, 2) == 4, "pow of negative bases");
    CHECK(hm_powf(0, -1) == INFINITY && same(hm_powf(-0.0f, 3), -0.0f) && hm_powf(-0.0f, -3) == -INFINITY, "pow of zero");
    CHECK(hm_powf(2, 128) == INFINITY && hm_powf(2, -150) == 0 && hm_powf(0.5f, INFINITY) == 0 && hm_powf(2, -INFINITY) == 0, "pow overflow");
    CHECK(hm_powf(2, 10) == 1024 && hm_powf(10, 3) == 1000 && hm_powf(4, 0.5f) == 2, "exact powers");
    CHECK(hm_sqrtf(4) == 2 && isnan(hm_sqrtf(-1)) && hm_sqrtf(INFINITY) == INFINITY && hm_sqrt(2.25) == 1.5, "sqrt");
    CHECK(hm_tanhf(50) == 1 && hm_tanhf(-50) == -1 && hm_sinhf(100) == INFINITY && hm_coshf(-100) == INFINITY, "hyperbolic limits");
    CHECK(hm_cbrtf(27) == 3 && hm_cbrtf(-8) == -2 && hm_cbrtf(0) == 0, "cbrt");
    float s, c;
    hm_sincosf(1.0f, &s, &c);
    CHECK(s == hm_sinf(1.0f) && c == hm_cosf(1.0f), "sincosf = sinf, cosf");
    CHECK(isfinite(hm_sinf(1e30f)) && hm_fabsf(hm_sinf(1e30f)) <= 1 && isfinite(hm_cosf(3e38f)), "huge arguments stay finite");
}

int main(int argc, char **argv) {
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    accuracy();
    exact();
    special();
    printf("math: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
