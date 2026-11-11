/* Halcyon OS - small floating point math library (no libm in the kernel) */
#ifndef KMATH_H
#define KMATH_H

#define K_PI 3.14159265358979323846

static inline double k_sqrt(double x) { double r; __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x)); return r; }
static inline double k_fabs(double x) { return x < 0 ? -x : x; }
static inline double k_floor(double x) {
    long long i = (long long)x;
    return (double)(x < (double)i ? i - 1 : i);
}
static inline double k_fmod(double x, double y) { return x - k_floor(x / y) * y; }

static inline double k_sin(double x) {
    /* range reduce to [-pi, pi] then a 9th-order minimax-ish polynomial */
    x = k_fmod(x + K_PI, 2 * K_PI) - K_PI;
    /* reduce further to [-pi/2, pi/2] */
    if (x > K_PI / 2) x = K_PI - x;
    else if (x < -K_PI / 2) x = -K_PI - x;
    double x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110)))));
}
static inline double k_cos(double x) { return k_sin(x + K_PI / 2); }

static inline double k_exp(double x) {
    /* e^x = 2^(x/ln2); split integer and fraction */
    if (x < -700) return 0;
    if (x > 700) x = 700;
    double t = x * 1.4426950408889634;
    double fi = k_floor(t);
    double f = (t - fi) * 0.6931471805599453;
    double r = 1, term = 1;
    for (int i = 1; i < 14; i++) { term *= f / i; r += term; }
    long long e = (long long)fi;
    double p = 1;
    double b = e >= 0 ? 2.0 : 0.5;
    if (e < 0) e = -e;
    while (e) { if (e & 1) p *= b; b *= b; e >>= 1; }
    return r * p;
}
static inline double k_log(double x) {
    if (x <= 0) return -1e300;
    int e = 0;
    while (x > 2) { x /= 2; e++; }
    while (x < 1) { x *= 2; e--; }
    /* ln(x) for x in [1,2): use atanh series */
    double y = (x - 1) / (x + 1), y2 = y * y, s = 0, t = y;
    for (int i = 1; i < 40; i += 2) { s += t / i; t *= y2; }
    return 2 * s + e * 0.6931471805599453;
}
static inline double k_pow(double b, double e) {
    if (b == 0) return 0;
    if (e == (double)(long long)e && k_fabs(e) < 64) {
        long long n = (long long)e; int neg = n < 0; if (neg) n = -n;
        double r = 1, x = b;
        while (n) { if (n & 1) r *= x; x *= x; n >>= 1; }
        return neg ? 1 / r : r;
    }
    if (b < 0) return 0;
    return k_exp(e * k_log(b));
}

#endif
