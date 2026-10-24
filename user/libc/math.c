/* libm in pure SSE2 double arithmetic (no x87 transcendentals, which are microcoded and slow).
 *
 * Every function is built from published numerical methods: Cody-Waite and Payne-Hanek argument
 * reduction, table-driven exp/log (2^(j/64) and log(1 + i/128) as double+double pairs), and
 * near-minimax polynomials. The tables and coefficients come from math_tables.h, which
 * scripts/gen_libm_tables.py derives with arbitrary-precision arithmetic. Measured against a
 * 160-bit reference the maximum error is 0.5 to 0.8 ulp for sin, cos, tan, exp, exp2, expm1, log,
 * log2, log10, log1p, atan, atan2, asin, acos, sinh, tanh, hypot and pow (under 0.52 ulp). The
 * inverse hyperbolic functions are below 2 ulp. Results that are exactly representable (exp(0),
 * log(1), pow(2, 10), pow(10, n), log2(2^k), cbrt of a cube) come out exact. */
#include <math.h>
#include <fenv.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include "math_tables.h"

/* ---- floating-point environment (MXCSR and the x87 words together) ---- */
struct x87_env { /* fnstenv/fldenv 32-bit protected-mode layout, 28 bytes */
    unsigned short cw, r0, sw, r1, tw, r2;
    unsigned ip;
    unsigned short cs, op;
    unsigned dp;
    unsigned short ds, r3;
};
static unsigned get_mxcsr(void) {
    unsigned m;
    __asm__ volatile("stmxcsr %0" : "=m"(m));
    return m;
}
static void set_mxcsr(unsigned m) { __asm__ volatile("ldmxcsr %0" : : "m"(m)); }
static void get_x87(struct x87_env *e) { __asm__ volatile("fnstenv %0\n\tfldenv %0" : "=m"(*e)); }
static void set_x87(const struct x87_env *e) { __asm__ volatile("fldenv %0" : : "m"(*e)); }

int feclearexcept(int excepts) {
    excepts &= FE_ALL_EXCEPT;
    struct x87_env e;
    get_x87(&e);
    e.sw &= ~excepts;
    set_x87(&e);
    set_mxcsr(get_mxcsr() & ~(unsigned)excepts);
    return 0;
}
int fetestexcept(int excepts) {
    unsigned short sw;
    __asm__ volatile("fnstsw %0" : "=am"(sw));
    return (int)((sw | get_mxcsr()) & (unsigned)excepts & FE_ALL_EXCEPT);
}
int fegetexceptflag(fexcept_t *flag, int excepts) {
    *flag = (fexcept_t)fetestexcept(excepts);
    return 0;
}
int fesetexceptflag(const fexcept_t *flag, int excepts) {
    excepts &= FE_ALL_EXCEPT;
    struct x87_env e;
    get_x87(&e);
    e.sw = (unsigned short)((e.sw & ~excepts) | (*flag & excepts));
    set_x87(&e);
    set_mxcsr((get_mxcsr() & ~(unsigned)excepts) | (*flag & (unsigned)excepts));
    return 0;
}
int feraiseexcept(int excepts) {
    /* Records the flags; Nocturne programs run with exceptions masked. */
    set_mxcsr(get_mxcsr() | (unsigned)(excepts & FE_ALL_EXCEPT));
    return 0;
}
int fegetround(void) {
    unsigned short cw;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    return cw & 0xc00;
}
int fesetround(int round) {
    if (round & ~0xc00) return -1;
    unsigned short cw;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    cw = (unsigned short)((cw & ~0xc00) | round);
    __asm__ volatile("fldcw %0" : : "m"(cw));
    set_mxcsr((get_mxcsr() & ~0x6000u) | ((unsigned)round << 3));
    return 0;
}
int fegetenv(fenv_t *env) {
    struct x87_env e;
    get_x87(&e);
    env->x87_control = e.cw;
    env->x87_status = e.sw;
    env->mxcsr = get_mxcsr();
    return 0;
}
int fesetenv(const fenv_t *env) {
    struct x87_env e;
    get_x87(&e);
    if (env == FE_DFL_ENV) {
        e.cw = 0x037f; /* all masked, round to nearest, extended precision */
        e.sw &= ~FE_ALL_EXCEPT;
        set_x87(&e);
        set_mxcsr(0x1f80);
        return 0;
    }
    e.cw = env->x87_control;
    e.sw = (unsigned short)((e.sw & ~FE_ALL_EXCEPT) | (env->x87_status & FE_ALL_EXCEPT));
    set_x87(&e);
    set_mxcsr(env->mxcsr);
    return 0;
}
int feholdexcept(fenv_t *env) {
    fegetenv(env);
    struct x87_env e;
    get_x87(&e);
    e.cw |= FE_ALL_EXCEPT;
    e.sw &= ~FE_ALL_EXCEPT;
    set_x87(&e);
    set_mxcsr((env->mxcsr | (FE_ALL_EXCEPT << 7)) & ~(unsigned)FE_ALL_EXCEPT);
    return 0;
}
int feupdateenv(const fenv_t *env) {
    int raised = fetestexcept(FE_ALL_EXCEPT);
    fesetenv(env);
    if (raised) feraiseexcept(raised);
    return 0;
}

/* ---- bit access and error-free transformations ---- */
#define F64_SIGN 0x8000000000000000ULL
#define F64_FRAC 0x000fffffffffffffULL
static inline uint64_t d2u(double d) {
    union { double d; uint64_t u; } v = {d};
    return v.u;
}
static inline double u2d(uint64_t u) {
    union { double d; uint64_t u; } v = {0};
    v.u = u;
    return v.d;
}
/* 2^e for -1022 <= e <= 1023 */
static inline double pow2i(int e) { return u2d((uint64_t)(e + 1023) << 52); }

/* a + b = s + e exactly (Knuth) */
static inline void two_sum(double a, double b, double *s, double *e) {
    double x = a + b, bb = x - a;
    *s = x;
    *e = (a - (x - bb)) + (b - bb);
}
/* the same when |a| >= |b| (Dekker) */
static inline void fast_two_sum(double a, double b, double *s, double *e) {
    double x = a + b;
    *s = x;
    *e = b - (x - a);
}
/* a * b = p + e exactly, by Veltkamp splitting; needs |a|, |b| < 2^996 and no underflow */
static inline void two_product(double a, double b, double *p, double *e) {
    const double split = 134217729.0; /* 2^27 + 1 */
    double ca = split * a, cb = split * b;
    double ah = ca - (ca - a), al = a - ah;
    double bh = cb - (cb - b), bl = b - bh;
    *p = a * b;
    *e = ((ah * bh - *p) + ah * bl + al * bh) + al * bl;
}

/* x * 2^k with a single rounding, for a normal x of moderate size and |k| < 2200 */
static double scale_pow2(double x, int k) {
    if (k > 1023) {
        x *= 0x1p1023;
        k -= 1023;
        if (k > 1023) k = 1023;
    } else if (k < -1022) {
        if (k < -2044) k = -2044;
        x *= pow2i(k + 1022); /* exact: x stays normal */
        k = -1022;            /* the only rounding happens here, into the subnormal range */
    }
    return x * pow2i(k);
}

double fabs(double x) { return u2d(d2u(x) & ~F64_SIGN); }

double sqrt(double x) {
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}

double copysign(double x, double y) { return u2d((d2u(x) & ~F64_SIGN) | (d2u(y) & F64_SIGN)); }

/* ---- rounding to integral values: bit manipulation, independent of the rounding mode ---- */
double floor(double x) {
    uint64_t u = d2u(x);
    int e = (int)((u >> 52) & 0x7ff) - 1023;
    if (e >= 52) return e == 1024 ? x + x : x; /* integers, infinities; NaN is quieted */
    if (e < 0) {
        if (!(u << 1)) return x; /* +-0 */
        return (u >> 63) ? -1.0 : 0.0;
    }
    uint64_t m = (1ULL << (52 - e)) - 1;
    if (!(u & m)) return x;
    if (u >> 63) u += m; /* negative: the magnitude rounds up */
    return u2d(u & ~m);
}

double ceil(double x) {
    uint64_t u = d2u(x);
    int e = (int)((u >> 52) & 0x7ff) - 1023;
    if (e >= 52) return e == 1024 ? x + x : x;
    if (e < 0) {
        if (!(u << 1)) return x;
        return (u >> 63) ? -0.0 : 1.0;
    }
    uint64_t m = (1ULL << (52 - e)) - 1;
    if (!(u & m)) return x;
    if (!(u >> 63)) u += m;
    return u2d(u & ~m);
}

double trunc(double x) {
    uint64_t u = d2u(x);
    int e = (int)((u >> 52) & 0x7ff) - 1023;
    if (e >= 52) return e == 1024 ? x + x : x;
    if (e < 0) return u2d(u & F64_SIGN);
    return u2d(u & ~((1ULL << (52 - e)) - 1));
}

/* halfway cases away from zero */
double round(double x) {
    uint64_t u = d2u(x);
    int e = (int)((u >> 52) & 0x7ff) - 1023;
    if (e >= 52) return e == 1024 ? x + x : x;
    if (e < 0) return u2d((u & F64_SIGN) | (e == -1 ? 0x3ff0000000000000ULL : 0));
    uint64_t m = (1ULL << (52 - e)) - 1;
    u += (m + 1) >> 1;
    return u2d(u & ~m);
}

/* ---- sin, cos, tan ----
 * The argument is reduced to y0 + y1 in [-pi/4, pi/4] (y1 is the rounding tail, |y1| <= ulp(y0)/2)
 * and a multiple n of pi/2. Up to |x| < 3.2e6 a Cody-Waite reduction with pi/2 split in three
 * 32-bit chunks and a double tail is exact enough for results close to a multiple of pi/2;
 * beyond that Payne-Hanek uses 192 bits of 2/pi. */
static int rem_pio2_large(double x, double *y0, double *y1) {
    uint64_t u = d2u(x) & ~F64_SIGN;
    int q = (int)(u >> 52) - 1075; /* |x| = M * 2^q, q >= -31 here */
    uint64_t M = (u & F64_FRAC) | (1ULL << 52);
    int qe = q < 2 ? 2 : q;
    /* Only the fraction bits of 2/pi at or below 2^(2 - qe) matter modulo 4: the window starts
       at fraction bit qe - 1 (1-based) and holds 192 bits. */
    int s = qe - 2, w = s >> 6, sh = s & 63;
    uint64_t L[3];
    for (int i = 0; i < 3; i++)
        L[i] = sh ? (two_over_pi[w + i] << sh) | (two_over_pi[w + i + 1] >> (64 - sh)) : two_over_pi[w + i];
    /* (x * 2/pi) mod 4 as a 192-bit fixed-point number with 190 fraction bits. For q < 2 the
       significand carries fraction bits, so the full product is formed and shifted right. */
    unsigned __int128 t = (unsigned __int128)M * L[2];
    uint64_t r0 = (uint64_t)t;
    t = (unsigned __int128)M * L[1] + (uint64_t)(t >> 64);
    uint64_t r1 = (uint64_t)t;
    t = (unsigned __int128)M * L[0] + (uint64_t)(t >> 64);
    uint64_t r2 = (uint64_t)t, r3 = (uint64_t)(t >> 64);
    if (q < 2) { /* shift right by d = 2 - q, 1 <= d <= 33 */
        int d = 2 - q;
        r0 = (r0 >> d) | (r1 << (64 - d));
        r1 = (r1 >> d) | (r2 << (64 - d));
        r2 = (r2 >> d) | (r3 << (64 - d));
    }
    int n = (int)(r2 >> 62);
    const uint64_t mask62 = (1ULL << 62) - 1;
    uint64_t f2 = r2 & mask62, f1 = r1, f0 = r0;
    bool neg = false;
    if (f2 >> 61) { /* fraction >= 1/2: round the quotient up, the remainder turns negative */
        n++;
        neg = true;
        unsigned __int128 lo = ~(((unsigned __int128)f1 << 64) | f0) + 1;
        f2 = (~f2 + (lo == 0 ? 1 : 0)) & mask62;
        f1 = (uint64_t)(lo >> 64);
        f0 = (uint64_t)lo;
    }
    double r_hi = 0, r_lo = 0;
    if (f2 | f1 | f0) {
        int lz = f2 ? __builtin_clzll(f2) : f1 ? 64 + __builtin_clzll(f1) : 128 + __builtin_clzll(f0);
        int ws = lz >> 6, bs = lz & 63;
        uint64_t v[5] = {f2, f1, f0, 0, 0};
        uint64_t m2 = bs ? (v[ws] << bs) | (v[ws + 1] >> (64 - bs)) : v[ws];
        uint64_t m1 = bs ? (v[ws + 1] << bs) | (v[ws + 2] >> (64 - bs)) : v[ws + 1];
        /* the fraction is m * 2^(-lz-190), m = m2:m1:m0 normalized to 192 bits */
        double fh = (double)(m2 >> 11) * pow2i(-51 - lz);
        double fl = (double)(((m2 & 0x7ff) << 53) | (m1 >> 11)) * pow2i(-115 - lz);
        double p, pe;
        two_product(fh, PIO2_HI, &p, &pe);
        pe += fh * PIO2_LO + fl * PIO2_HI;
        two_sum(p, pe, &r_hi, &r_lo);
    }
    if (neg != (x < 0)) { /* the sign of x flips the whole remainder */
        r_hi = -r_hi;
        r_lo = -r_lo;
    }
    *y0 = r_hi;
    *y1 = r_lo;
    return x < 0 ? -n : n;
}

static int rem_pio2(double x, double *y0, double *y1) {
    double ax = fabs(x);
    if (ax <= 0x1.921fb54442d18p-1) { /* pi/4 */
        *y0 = x;
        *y1 = 0;
        return 0;
    }
    if (ax < 3.2e6) {
        double t = x * INV_PIO2;
        int n = (int)(t + (t < 0 ? -0.5 : 0.5));
        double fn = n;
        double a = x - fn * PIO2_1; /* exact: fn has at most 21 bits, PIO2_1 32 */
        double s1, e1, s2, e2;
        two_sum(a, -(fn * PIO2_2), &s1, &e1);
        two_sum(s1, -(fn * PIO2_3), &s2, &e2);
        two_sum(s2, (e1 + e2) - fn * PIO2_4, y0, y1);
        return n;
    }
    return rem_pio2_large(x, y0, y1);
}

/* sin(y0 + y1), with the rounding error of the result in *lo */
static inline double ksin(double y0, double y1, double *lo) {
    double z = y0 * y0;
    double p = sin_c[0] + z * (sin_c[1] + z * (sin_c[2] + z * (sin_c[3] + z * (sin_c[4] +
               z * (sin_c[5] + z * (sin_c[6] + z * sin_c[7]))))));
    double t = y0 * z * p + y1 * (1.0 - 0.5 * z);
    double hi = y0 + t;
    *lo = t - (hi - y0);
    return hi;
}

/* cos(y0 + y1), with the rounding error of the result in *lo */
static inline double kcos(double y0, double y1, double *lo) {
    double z = y0 * y0, zl = 0;
    if (fabs(y0) > 0.3) two_product(y0, y0, &z, &zl);
    double c = z * z * (cos_c[0] + z * (cos_c[1] + z * (cos_c[2] + z * (cos_c[3] + z * (cos_c[4] +
               z * cos_c[5])))));
    double hz = 0.5 * z;
    double w = 1.0 - hz;
    double tail = ((1.0 - w) - hz) + (c - (y0 * y1 + 0.5 * zl));
    double hi = w + tail;
    *lo = tail - (hi - w);
    return hi;
}

double sin(double x) {
    unsigned ex = (unsigned)(d2u(x) >> 52) & 0x7ff;
    if (ex < 0x3e5) return x; /* |x| < 2^-26, or 0 */
    if (ex == 0x7ff) return x - x;
    double y0, y1, lo;
    switch (rem_pio2(x, &y0, &y1) & 3) {
    case 0: return ksin(y0, y1, &lo);
    case 1: return kcos(y0, y1, &lo);
    case 2: return -ksin(y0, y1, &lo);
    default: return -kcos(y0, y1, &lo);
    }
}

double cos(double x) {
    unsigned ex = (unsigned)(d2u(x) >> 52) & 0x7ff;
    if (ex < 0x3e4) return 1.0; /* |x| < 2^-27 */
    if (ex == 0x7ff) return x - x;
    double y0, y1, lo;
    switch (rem_pio2(x, &y0, &y1) & 3) {
    case 0: return kcos(y0, y1, &lo);
    case 1: return -ksin(y0, y1, &lo);
    case 2: return -kcos(y0, y1, &lo);
    default: return ksin(y0, y1, &lo);
    }
}

/* (ah + al) / (bh + bl) to nearly full double precision */
static double dd_div(double ah, double al, double bh, double bl) {
    double q = ah / bh, p, pe;
    two_product(q, bh, &p, &pe);
    double r = ((ah - p) - pe) + (al - q * bl);
    return q + r / bh;
}

double tan(double x) {
    unsigned ex = (unsigned)(d2u(x) >> 52) & 0x7ff;
    if (ex < 0x3e4) return x; /* |x| < 2^-27, or 0 */
    if (ex == 0x7ff) return x - x;
    double y0, y1, sl, cl;
    int n = rem_pio2(x, &y0, &y1);
    double sh = ksin(y0, y1, &sl), ch = kcos(y0, y1, &cl);
    if (n & 1) return -dd_div(ch, cl, sh, sl);
    return dd_div(sh, sl, ch, cl);
}

/* ---- exp family ----
 * x = (64 k + j) ln2/64 + r, |r| <= ln2/128; exp(x) = 2^k * 2^(j/64) * (1 + p(r)). */
static double exp_dd(double zh, double zl) { /* exp(zh + zl), -746 < zh < 710 */
    double t = zh * INV_LN2N;
    int ki = (int)(t + (t < 0 ? -0.5 : 0.5));
    double fk = ki;
    double r = ((zh - fk * LN2N_HI) - fk * LN2N_LO) + zl;
    int j = ki & 63, k = ki >> 6;
    double p = r + r * r * (exp_c[0] + r * (exp_c[1] + r * (exp_c[2] + r * (exp_c[3] + r * exp_c[4]))));
    double th = exp_tab[2 * j], tl = exp_tab[2 * j + 1];
    return scale_pow2(th + (th * p + tl), k);
}

double exp(double x) {
    if (x != x) return x;
    if (x > 709.8) return HUGE_VAL;
    if (x < -745.2) return 0.0;
    return exp_dd(x, 0.0);
}

double exp2(double x) {
    if (x != x) return x;
    if (x >= 1025.0) return HUGE_VAL;
    if (x < -1080.0) return 0.0;
    double t = x * 64.0;
    int ki = (int)(t + (t < 0 ? -0.5 : 0.5));
    double r = (x - (double)ki * 0x1p-6) * M_LN2; /* the subtraction is exact */
    int j = ki & 63, k = ki >> 6;
    double p = r + r * r * (exp_c[0] + r * (exp_c[1] + r * (exp_c[2] + r * (exp_c[3] + r * exp_c[4]))));
    double th = exp_tab[2 * j], tl = exp_tab[2 * j + 1];
    return scale_pow2(th + (th * p + tl), k);
}

/* e^x - 1 for |x| <= 0.35 from the series with the leading terms kept apart */
static inline void expm1_dd(double x, double *hi, double *lo) {
    double a, ae, s, e;
    two_product(x, x, &a, &ae);
    double q = x * (expm1_c[1] + x * (expm1_c[2] + x * (expm1_c[3] + x * (expm1_c[4] +
               x * (expm1_c[5] + x * (expm1_c[6] + x * (expm1_c[7] + x * (expm1_c[8] +
               x * (expm1_c[9] + x * (expm1_c[10] + x * expm1_c[11]))))))))));
    double tail = a * ((expm1_c[0] - 0.5) + q); /* x^2 (E(x) - 1/2) */
    fast_two_sum(x, 0.5 * a, &s, &e);
    double t = e + 0.5 * ae + tail;
    *hi = s + t;
    *lo = t - (*hi - s);
}

/* e^x - 1 as a double+double pair (relative error about 2^-62), for 2^-54 <= |x| <= 709.8 */
static void expm1_pair(double x, double *hi, double *lo) {
    double eh, el;
    if (fabs(x) < 0.3466) {
        expm1_dd(x, hi, lo);
        return;
    }
    /* x = k ln2 + r; e^x - 1 = 2^k (1 + expm1(r)) - 1 */
    int k = (int)(x * INV_LN2 + (x < 0 ? -0.5 : 0.5));
    double fk = k;
    double rh = x - fk * LN2_HI, rl = -fk * LN2_LO;
    double t, tl;
    expm1_dd(rh, &eh, &el);
    two_sum(1.0, eh, &t, &tl);
    tl += el + rl * t;
    double s, e;
    two_sum(scale_pow2(t, k), -1.0, &s, &e);
    fast_two_sum(s, e + scale_pow2(tl, k), hi, lo);
}

double expm1(double x) {
    if (x != x || x == 0) return x;
    if (fabs(x) < 0x1p-54) return x;
    if (x > 709.8) return HUGE_VAL;
    if (x < -40) return -1.0;
    double h, l;
    expm1_pair(x, &h, &l);
    return h;
}

/* ---- log family ----
 * x = 2^e m with m in [1, 2); the table is indexed by the top 7 fraction bits i, and
 * log(m) = log(1 + i/128) + log1p(r), r = (m - (1 + i/128)) / (1 + i/128) held as a pair.
 * Close to 1 the series in f = x - 1 is used directly. The result is a double+double pair
 * with a relative error below 2^-64, which pow, log2 and log10 need. */
static void log_dd(double x, double *hi, double *lo) { /* finite x > 0 */
    if (x > 0.9921875 && x < 1.0078125) {
        double f = x - 1.0, a, ae;
        two_product(f, f, &a, &ae);
        double r3 = f * a * (log_c[0] + f * (log_c[1] + f * (log_c[2] + f * (log_c[3] + f * (log_c[4] +
                    f * (log_c[5] + f * (log_c[6] + f * log_c[7])))))));
        double s, e1;
        fast_two_sum(f, -0.5 * a, &s, &e1);
        double t = e1 - 0.5 * ae + r3;
        *hi = s + t;
        *lo = t - (*hi - s);
        return;
    }
    uint64_t u = d2u(x);
    int e = 0;
    if (u < (1ULL << 52)) { /* subnormal */
        u = d2u(x * 0x1p54);
        e = -54;
    }
    e += (int)(u >> 52) - 1023;
    int i = (int)((u >> 45) & 127);
    double m = u2d((u & F64_FRAC) | 0x3ff0000000000000ULL);
    double d = m - (1.0 + i * 0x1p-7); /* exact */
    const double *T = log_tab[i];
    double rh, rl;
    two_product(d, T[0], &rh, &rl);
    rl += d * T[1];
    double a, ae;
    two_product(rh, rh, &a, &ae);
    double r3 = a * rh * (log_c[0] + rh * (log_c[1] + rh * (log_c[2] + rh * (log_c[3] + rh * (log_c[4] +
                rh * (log_c[5] + rh * (log_c[6] + rh * log_c[7])))))));
    double s1, e1;
    fast_two_sum(rh, -0.5 * a, &s1, &e1);
    double rest = e1 + rl - 0.5 * ae - rh * rl + r3;
    double fe = e;
    double s, es, S, E;
    two_sum(fe * LN2_HI, T[2], &s, &es); /* e * LN2_HI is exact */
    two_sum(s, s1, &S, &E);
    double tail = E + es + fe * LN2_LO + T[3] + rest;
    two_sum(S, tail, hi, lo);
}

/* NaN, zero, negative and infinite arguments of the logarithms; returns true when *r is the answer */
static bool log_special(double x, double *r) {
    if (x > 0 && x < HUGE_VAL) return false;
    *r = x != x ? x : x == 0 ? -HUGE_VAL : x < 0 ? NAN : x;
    return true;
}

double log(double x) {
    double r, lo;
    if (log_special(x, &r)) return r;
    log_dd(x, &r, &lo);
    return r;
}

/* exponent of a power of two, or 1 << 30 when x (finite, positive) is not one */
static int exact_log2(double x) {
    uint64_t u = d2u(x);
    if (u >= (1ULL << 52)) return (u & F64_FRAC) ? 1 << 30 : (int)(u >> 52) - 1023;
    if (u & (u - 1)) return 1 << 30;
    return 63 - __builtin_clzll(u) - 1074;
}

double log2(double x) {
    double r, h, l, p, pe;
    if (log_special(x, &r)) return r;
    int e = exact_log2(x);
    if (e != 1 << 30) return (double)e;
    log_dd(x, &h, &l);
    two_product(h, LOG2E_HI, &p, &pe);
    return p + (pe + (h * LOG2E_LO + l * LOG2E_HI));
}

double log10(double x) {
    double r, h, l, p, pe;
    if (log_special(x, &r)) return r;
    log_dd(x, &h, &l);
    two_product(h, LOG10E_HI, &p, &pe);
    return p + (pe + (h * LOG10E_LO + l * LOG10E_HI));
}

double log1p(double x) {
    if (x != x || x == 0) return x;
    if (x < -1) return NAN;
    if (x == -1) return -HUGE_VAL;
    if (x == HUGE_VAL) return x;
    double u, ue, h, l;
    two_sum(1.0, x, &u, &ue); /* log(1 + x) = log(u) + ue / u */
    log_dd(u, &h, &l);
    return h + (l + ue / u);
}

/* ---- pow ---- */
double pow(double x, double y) {
    if (y == 0 || x == 1) return 1.0;
    if (x != x || y != y) return x + y;
    double ax = fabs(x), ay = fabs(y);
    int yint = 0; /* 0 not an integer, 1 even, 2 odd */
    if (ay >= 0x1p53) yint = 1;
    else if (trunc(y) == y) yint = ((int64_t)y & 1) ? 2 : 1;
    if (ay == HUGE_VAL) {
        if (ax == 1.0) return 1.0;
        return (ax < 1.0) == (y > 0) ? 0.0 : HUGE_VAL;
    }
    if (ax == 0) {
        if (y < 0) return yint == 2 ? copysign(HUGE_VAL, x) : HUGE_VAL;
        return yint == 2 ? x : 0.0;
    }
    if (ax == HUGE_VAL) {
        if (x > 0) return y < 0 ? 0.0 : HUGE_VAL;
        if (y < 0) return yint == 2 ? -0.0 : 0.0;
        return yint == 2 ? -HUGE_VAL : HUGE_VAL;
    }
    bool neg = false;
    if (x < 0) {
        if (!yint) return NAN;
        neg = yint == 2;
    }
    if (ax == 1.0) return neg ? -1.0 : 1.0;
    if (y == 1.0) return x;
    if (y == 2.0) return x * x;
    if (y == -1.0) return 1.0 / x;
    if (y == 0.5 && x > 0) return sqrt(x);
    double h, l;
    log_dd(ax, &h, &l);
    double z = y * h, r;
    if (z > 710.0) r = HUGE_VAL;
    else if (z < -746.0) r = 0.0;
    else {
        double zh, zp;
        two_product(y, h, &zh, &zp);
        r = exp_dd(zh, zp + y * l);
    }
    return neg ? -r : r;
}

/* ---- inverse trigonometric functions ----
 * atan(u) for u = uh + ul in [0, 1]: atan(c) + atan((u - c) / (1 + c u)) with c the nearest eighth. */
static void atan_unit(double uh, double ul, double *hi, double *lo) {
    int i = (int)(uh * 8.0 + 0.5);
    double c = i * 0.125;
    double nh = uh - c; /* exact */
    double ph, pl, dh, dl;
    two_product(c, uh, &ph, &pl);
    fast_two_sum(1.0, ph, &dh, &dl);
    dl += pl + c * ul;
    double th = nh / dh, q, qe;
    two_product(th, dh, &q, &qe);
    double tl = (((nh - q) - qe) + (ul - th * dl)) / dh;
    double s = th * th;
    double r3 = th * s * (atan_c[0] + s * (atan_c[1] + s * (atan_c[2] + s * (atan_c[3] + s * (atan_c[4] +
                s * atan_c[5])))));
    double S, E;
    two_sum(atan_tab[i][0], th, &S, &E);
    double tail = E + atan_tab[i][1] + (tl - tl * s) + r3;
    two_sum(S, tail, hi, lo);
}

double atan(double x) {
    if (x != x) return x;
    double ax = fabs(x);
    if (ax < 0x1p-27) return x;
    if (ax > 0x1p66) return copysign(PIO2_HI, x);
    double h, l;
    if (ax <= 1.0) {
        atan_unit(ax, 0.0, &h, &l);
        return copysign(h, x);
    }
    double ih = 1.0 / ax, p, pe;
    two_product(ih, ax, &p, &pe);
    atan_unit(ih, ((1.0 - p) - pe) * ih, &h, &l);
    double s, e;
    two_sum(PIO2_HI, -h, &s, &e);
    return copysign(s + (e + PIO2_LO - l), x);
}

double atan2(double y, double x) {
    if (x != x || y != y) return x + y;
    bool sx = (d2u(x) & F64_SIGN) != 0;
    double ax = fabs(x), ay = fabs(y);
    if (ay == 0) return sx ? copysign(PI_HI, y) : y;
    if (ax == 0) return copysign(PIO2_HI, y);
    if (ax == HUGE_VAL) {
        if (ay == HUGE_VAL) return copysign(sx ? PI3_4_HI : 0.5 * PIO2_HI, y);
        return sx ? copysign(PI_HI, y) : copysign(0.0, y);
    }
    if (ay == HUGE_VAL) return copysign(PIO2_HI, y);
    int ea = (int)(d2u(ay) >> 52), ex = (int)(d2u(ax) >> 52);
    double res;
    if (ea - ex > 66) res = PIO2_HI; /* pi/2 +- a ratio below 2^-66 */
    else if (ex - ea > 66) res = sx ? PI_HI : ay / ax;
    else {
        bool swap = ay > ax;
        double num = swap ? ax : ay, den = swap ? ay : ax;
        int de = (int)(d2u(den) >> 52);
        if (de == 0) de = 1;
        int a = 1023 - de, a1 = a / 2, a2 = a - a1; /* scale by 2^a in two exact steps */
        num = num * pow2i(a1) * pow2i(a2);
        den = den * pow2i(a1) * pow2i(a2);
        double uh = num / den, p, pe, h, l, hi, lo, s, e;
        two_product(uh, den, &p, &pe);
        atan_unit(uh, ((num - p) - pe) / den, &h, &l);
        hi = h;
        lo = l;
        if (swap) { /* pi/2 - atan(u) */
            two_sum(PIO2_HI, -h, &s, &e);
            double tail = e + PIO2_LO - l;
            two_sum(s, tail, &hi, &lo);
        }
        if (sx) { /* pi - theta */
            two_sum(PI_HI, -hi, &s, &e);
            return copysign(s + (e + PI_LO - lo), y);
        }
        res = hi;
    }
    return copysign(res, y);
}

/* asin (want_acos == 0) or acos (want_acos != 0) of 0 < ax < 1 as a double+double pair, through
 * atan of the ratio ax / s or s / ax with s = sqrt(1 - ax^2) held to double+double. */
static void asin_acos_dd(double ax, int want_acos, double *hi, double *lo) {
    double a, ae, dh, dl, p, pe;
    two_product(ax, ax, &a, &ae);
    two_sum(1.0, -a, &dh, &dl);
    fast_two_sum(dh, dl - ae, &dh, &dl); /* 1 - ax^2 */
    double sh = sqrt(dh);
    two_product(sh, sh, &p, &pe);
    double sl = (((dh - p) - pe) + dl) / (2.0 * sh);
    bool direct = ax > 0.7071067811865476; /* the ratio is s / ax instead of ax / s */
    double nh = direct ? sh : ax, nl = direct ? sl : 0.0;
    double mh = direct ? ax : sh, ml = direct ? 0.0 : sl;
    double q = nh / mh;
    two_product(q, mh, &p, &pe);
    double ql = (((nh - p) - pe) + (nl - q * ml)) / mh;
    double h, l;
    atan_unit(q, ql, &h, &l);
    if (direct == (want_acos != 0)) { /* atan of the ratio is the result itself */
        *hi = h;
        *lo = l;
        return;
    }
    double s, e; /* pi/2 - atan(ratio) */
    two_sum(PIO2_HI, -h, &s, &e);
    fast_two_sum(s, e + PIO2_LO - l, hi, lo);
}

double asin(double x) {
    double ax = fabs(x);
    if (x != x || ax > 1.0) return NAN;
    if (ax < 0x1p-27) return x;
    if (ax == 1.0) return copysign(PIO2_HI, x);
    double h, l;
    asin_acos_dd(ax, 0, &h, &l);
    return copysign(h, x);
}

double acos(double x) {
    double ax = fabs(x);
    if (x != x || ax > 1.0) return NAN;
    if (ax == 1.0) return x < 0 ? PI_HI : 0.0;
    if (ax == 0) return PIO2_HI;
    double h, l;
    asin_acos_dd(ax, 1, &h, &l);
    if (x > 0) return h;
    double s, e; /* pi - acos(ax) */
    two_sum(PI_HI, -h, &s, &e);
    return s + (e + PI_LO - l);
}

/* ---- fmod: exact, by reducing the integer significand 11 bits at a time ---- */
double fmod(double x, double y) {
    uint64_t ux = d2u(x), uy = d2u(y), sx = ux & F64_SIGN;
    uint64_t ax = ux & ~F64_SIGN, ay = uy & ~F64_SIGN;
    if (ay == 0 || ax >= 0x7ff0000000000000ULL || ay > 0x7ff0000000000000ULL) return (x * y) / (x * y);
    if (ax < ay) return x;
    if (ax == ay) return u2d(sx);
    if (ay == 0x7ff0000000000000ULL) return x;
    int ex = (int)(ax >> 52), ey = (int)(ay >> 52);
    uint64_t mx = ax & F64_FRAC, my = ay & F64_FRAC;
    if (ex) mx |= 1ULL << 52; else ex = 1;
    if (ey) my |= 1ULL << 52; else ey = 1;
    int n = ex - ey;
    if (!n) mx %= my;
    while (n > 0) {
        int k = n > 11 ? 11 : n;
        mx = (mx << k) % my;
        n -= k;
        if (!mx) return u2d(sx);
    }
    if (!mx) return u2d(sx);
    int lz = __builtin_clzll(mx) - 11; /* value = mx * 2^(ey - 1075) */
    mx <<= lz;
    int E = ey - lz;
    if (E >= 1) return u2d(sx | ((uint64_t)E << 52) | (mx & F64_FRAC));
    return u2d(sx | (mx >> (1 - E)));
}

/* hypot without overflow or underflow of the squares */
double hypot(double x, double y) {
    double ax = fabs(x), ay = fabs(y);
    if (ax == HUGE_VAL || ay == HUGE_VAL) return HUGE_VAL;
    if (ax != ax || ay != ay) return ax + ay;
    if (ax < ay) { double t = ax; ax = ay; ay = t; }
    if (ay == 0) return ax;
    double scale = 1.0;
    if (ax > 0x1p500) {
        ax *= 0x1p-600;
        ay *= 0x1p-600;
        scale = 0x1p600;
    } else if (ax < 0x1p-500) {
        ax *= 0x1p600;
        ay *= 0x1p600;
        scale = 0x1p-600;
    }
    /* ax^2 + ay^2 as a double+double, then one Newton correction of the square root */
    double a, ae, b, be, s, se, hi, lo, p, pe;
    two_product(ax, ax, &a, &ae);
    two_product(ay, ay, &b, &be);
    two_sum(a, b, &s, &se);
    fast_two_sum(s, se + (ae + be), &hi, &lo);
    double r = sqrt(hi);
    two_product(r, r, &p, &pe);
    return scale * (r + (((hi - p) - pe) + lo) / (2.0 * r));
}

double sinh(double x) {
    double ax = fabs(x);
    if (!(ax < HUGE_VAL) || ax < 0x1p-28) return x;
    double r;
    if (ax < 22) {
        double th, tl, dh, dl, u, ue;
        expm1_pair(ax, &th, &tl);
        two_sum(th, 1.0, &dh, &dl);
        double q = dd_div(th, tl, dh, dl + tl); /* t / (t + 1) */
        two_sum(th, q, &u, &ue);
        r = 0.5 * (u + (ue + tl));
    } else if (ax < 709.78) {
        r = 0.5 * exp(ax);
    } else {
        double w = exp(0.5 * ax);
        r = (0.5 * w) * w;
    }
    return copysign(r, x);
}

double cosh(double x) {
    double ax = fabs(x);
    if (!(ax < HUGE_VAL)) return ax;
    if (ax < 0.3466) {
        double t = expm1(ax);
        return 1 + (t * t) / (2 * (1 + t));
    }
    if (ax < 22) {
        double t = exp(ax);
        return 0.5 * (t + 1 / t);
    }
    if (ax < 709.78) return 0.5 * exp(ax);
    double w = exp(0.5 * ax);
    return (0.5 * w) * w;
}

double tanh(double x) {
    double ax = fabs(x);
    if (ax != ax || ax < 0x1p-28) return x;
    if (ax > 22) return copysign(1.0, x);
    double th, tl, dh, dl;
    expm1_pair(2 * ax, &th, &tl); /* tanh = t / (t + 2) */
    two_sum(th, 2.0, &dh, &dl);
    return copysign(dd_div(th, tl, dh, dl + tl), x);
}

/* IEEE-754 binary64 rounding without floating-point arithmetic on the
 * discarded fraction: nearbyint must not raise the inexact exception.
 * Nocturne's scalar rounding mode is the x87 control-word RC field. */
double nearbyint(double x) {
    enum { FRACTION_BITS = 52, EXPONENT_BIAS = 1023, RC_SHIFT = 10 };
    union { double d; uint64_t u; } v = {x};
    const uint64_t sign = UINT64_C(1) << 63;
    unsigned exponent = (unsigned)((v.u >> FRACTION_BITS) & 0x7ff);
    int e = (int)exponent - EXPONENT_BIAS;
    if (exponent == 0x7ff) return x + x; /* Quiet signaling NaNs normally. */
    if (e >= FRACTION_BITS || !(v.u & ~sign)) return x;
    unsigned short control;
    __asm__ volatile("fnstcw %0" : "=m"(control));
    unsigned mode = (control >> RC_SHIFT) & 3;
    bool negative = (v.u & sign) != 0, increment;
    if (e < 0) {
        union { double d; uint64_t u; } half = {0.5}, one = {1.0};
        increment = mode == 0 ? (v.u & ~sign) > half.u :
                    mode == 1 ? negative : mode == 2 ? !negative : false;
        v.u = (v.u & sign) | (increment ? one.u : 0);
        return v.d;
    }
    unsigned shift = FRACTION_BITS - (unsigned)e;
    uint64_t unit = UINT64_C(1) << shift, mask = unit - 1, fraction = v.u & mask;
    if (!fraction) return x;
    increment = mode == 0 ? (fraction > (unit >> 1) ||
                (fraction == (unit >> 1) && (v.u & unit))) :
                mode == 1 ? negative : mode == 2 ? !negative : false;
    v.u &= ~mask;
    if (increment) v.u += unit;
    return v.d;
}
float nearbyintf(float x) { return (float)nearbyint((double)x); }

float sqrtf(float x) { return (float)sqrt(x); }
float sinf(float x) { return (float)sin(x); }
float cosf(float x) { return (float)cos(x); }
float fabsf(float x) { return (float)fabs(x); }
float floorf(float x) { return (float)floor(x); }
float hypotf(float x, float y) { return (float)hypot(x, y); }
float sinhf(float x) { return (float)sinh(x); }
float coshf(float x) { return (float)cosh(x); }
float tanhf(float x) { return (float)tanh(x); }
float asinhf(float x) { return (float)asinh(x); }
float acoshf(float x) { return (float)acosh(x); }
float atanhf(float x) { return (float)atanh(x); }
float log10f(float x) { return (float)log10(x); }
float cbrtf(float x) { return (float)cbrt(x); }
float expm1f(float x) { return (float)expm1(x); }
float log1pf(float x) { return (float)log1p(x); }

/* ---- exponent handling (needed by compilers and printf-style code) ---- */
long double ldexpl(long double x, int e) {
    while (e > 16000) {
        x *= 0x1p16000L;
        e -= 16000;
    }
    while (e < -16000) {
        x *= 0x1p-16000L;
        e += 16000;
    }
    if (e > 16383) { /* the factor itself must stay a normal number */
        x *= 0x1p16000L;
        e -= 16000;
    } else if (e < -16382) {
        x *= 0x1p-16000L;
        e += 16000;
    }
    union {
        long double ld;
        struct {
            uint64_t m;
            uint16_t se;
        } p;
    } f = {0};
    f.p.m = 1ULL << 63;
    f.p.se = (uint16_t)(16383 + e);
    return x * f.ld;
}

/* x * 2^n on the integer significand: one rounding (to nearest even) when the result is subnormal */
double scalbn(double x, int n) {
    uint64_t u = d2u(x), sign = u & F64_SIGN;
    int ex = (int)((u >> 52) & 0x7ff);
    if (ex == 0x7ff || x == 0) return x + x;
    uint64_t m = u & F64_FRAC;
    if (ex == 0) { /* subnormal: normalize */
        int lz = __builtin_clzll(m) - 11;
        m <<= lz;
        ex = 1 - lz;
    } else {
        m |= 1ULL << 52;
    }
    if (n > 5000) n = 5000;
    if (n < -5000) n = -5000;
    long E = (long)ex + n;
    if (E >= 0x7ff) return sign ? -HUGE_VAL : HUGE_VAL;
    if (E >= 1) return u2d(sign | ((uint64_t)E << 52) | (m & F64_FRAC));
    long s = 1 - E;
    if (s > 54) return u2d(sign);
    uint64_t q = m >> s, rem = m & ((1ULL << s) - 1), half = 1ULL << (s - 1);
    if (rem > half || (rem == half && (q & 1))) q++;
    return u2d(sign | q);
}
double ldexp(double x, int e) { return scalbn(x, e); }
float ldexpf(float x, int e) { return (float)scalbn((double)x, e); }

double frexp(double x, int *e) {
    union { double d; uint64_t u; } v = {x};
    int ex = (int)(v.u >> 52 & 0x7FF);
    if (ex == 0) { /* zero or subnormal */
        if (x == 0) {
            *e = 0;
            return x;
        }
        x = frexp(x * 0x1p64, e);
        *e -= 64;
        return x;
    }
    if (ex == 0x7FF) {
        *e = 0;
        return x;
    }
    *e = ex - 1022;
    v.u = (v.u & ~(0x7FFULL << 52)) | (1022ULL << 52);
    return v.d;
}

/* The unbiased exponent of a finite, nonzero x, as if it were normalized. A
   subnormal's significand m stands for m * 2^-1074. */
static int exponent_of(double x) {
    uint64_t u = d2u(x);
    int ex = (int)(u >> 52 & 0x7FF);
    return ex ? ex - 1023 : -1011 - __builtin_clzll(u & F64_FRAC);
}

/* The same for x87 extended precision, whose significand holds its integer
   bit: a subnormal's significand m stands for m * 2^-16445. */
static int exponent_of_ld(long double x) {
    union { long double d; struct { uint64_t m; uint16_t se; } p; } v = {x};
    int ex = v.p.se & 0x7FFF;
    return ex ? ex - 16383 : -16382 - __builtin_clzll(v.p.m);
}

static int ilogb_special(bool nan, bool zero) {
    feraiseexcept(FE_INVALID);
    return nan ? FP_ILOGBNAN : zero ? FP_ILOGB0 : INT_MAX;
}

int ilogb(double x) {
    if (x == 0 || !isfinite(x)) return ilogb_special(isnan(x), x == 0);
    return exponent_of(x);
}
int ilogbf(float x) { return ilogb(x); } /* float to double is exact */
int ilogbl(long double x) {
    if (x == 0 || !isfinite(x)) return ilogb_special(isnan(x), x == 0);
    return exponent_of_ld(x);
}

/* -1 / +0 is -inf with FE_DIVBYZERO; x * x is +inf for either infinity and
   NaN for NaN. */
double logb(double x) {
    if (x == 0) return -1 / fabs(x);
    if (!isfinite(x)) return x * x;
    return exponent_of(x);
}
float logbf(float x) { return (float)logb(x); }
long double logbl(long double x) {
    if (x == 0) return -1 / fabsl(x);
    if (!isfinite(x)) return x * x;
    return exponent_of_ld(x);
}

double modf(double x, double *ip) {
    double t = trunc(x);
    *ip = t;
    return isinf(x) ? 0.0 * x : x - t;
}

double fmin(double a, double b) { return isnan(a) ? b : isnan(b) ? a : a < b ? a : b; }
double fmax(double a, double b) { return isnan(a) ? b : isnan(b) ? a : a > b ? a : b; }
float fminf(float a, float b) { return (float)fmin(a, b); }
float fmaxf(float a, float b) { return (float)fmax(a, b); }
long lround(double x) { return (long)round(x); }

/* cbrt: a log/exp estimate (relative error ~1e-13), then one Newton step whose residual
 * r^3 - |x| is evaluated in double+double, which lands within a rounding of the true root. */
double cbrt(double x) {
    if (x != x || x == 0 || x == HUGE_VAL || x == -HUGE_VAL) return x;
    double a = fabs(x), scale = 1.0;
    if (a < 0x1p-900) {
        a *= 0x1p300;
        scale = 0x1p-100;
    } else if (a > 0x1p900) {
        a *= 0x1p-300;
        scale = 0x1p100;
    }
    double r = exp(log(a) * (1.0 / 3.0));
    double r2h, r2l, r3h, r3l;
    two_product(r, r, &r2h, &r2l);
    two_product(r2h, r, &r3h, &r3l);
    r3l += r2l * r;
    r -= ((r3h - a) + r3l) / (3.0 * r2h);
    return copysign(r * scale, x);
}
float ceilf(float x) { return (float)ceil(x); }
float roundf(float x) { return (float)round(x); }
float truncf(float x) { return (float)trunc(x); }
float powf(float x, float y) { return (float)pow(x, y); }
float expf(float x) { return (float)exp(x); }
float logf(float x) { return (float)log(x); }
float tanf(float x) { return (float)tan(x); }
float atan2f(float y, float x) { return (float)atan2(y, x); }
float acosf(float x) { return (float)acos(x); }
float asinf(float x) { return (float)asin(x); }
float atanf(float x) { return (float)atan(x); }
float fmodf(float x, float y) { return (float)fmod(x, y); }
long double fabsl(long double x) { return x < 0 ? -x : x; }

/* Cancellation-safe additions required by the ECMAScript Math intrinsics. */
long lrint(double x) {
    long r;
    __asm__("cvtsd2si %1, %0" : "=r"(r) : "x"(x));
    return r;
}
long lrintf(float x) {
    long r;
    __asm__("cvtss2si %1, %0" : "=r"(r) : "x"(x));
    return r;
}
long long llrint(double x) { return lrint(x); }
long long llrintf(float x) { return lrintf(x); }
/* rint rounds as nearbyint does; Nocturne does not track the inexact flag. */
double rint(double x) { return nearbyint(x); }
float rintf(float x) { return (float)nearbyint((double)x); }
long lroundf(float x) { return lround((double)x); }
long long llround(double x) { return lround(x); }
long long llroundf(float x) { return lround((double)x); }
/* Halfway cases go to the even neighbour, whatever the rounding mode. */
double roundeven(double x) {
    double t = trunc(x), d = fabs(x - t);
    if (d < 0.5) return t;
    if (d > 0.5) return t + copysign(1.0, x);
    return fmod(t, 2.0) == 0 ? t : t + copysign(1.0, x);
}
float roundevenf(float x) { return (float)roundeven((double)x); }
float copysignf(float x, float y) { return __builtin_copysignf(x, y); }
float frexpf(float x, int *e) { return (float)frexp((double)x, e); }
float log2f(float x) { return (float)log2((double)x); }
float exp2f(float x) { return (float)exp2((double)x); }
double nan(const char *tag) { (void)tag; return __builtin_nan(""); }
float nanf(const char *tag) { (void)tag; return __builtin_nanf(""); }

/* x*y+z with one rounding of the sum: the product is split exactly
 * (Veltkamp/Dekker) and the low parts are added after the high sum's error
 * is recovered (Knuth TwoSum). Overflow and non-finite inputs fall back to
 * the plain expression, which gives the same IEEE result there. */
double fma(double x, double y, double z) {
    if (!isfinite(x) || !isfinite(y) || !isfinite(z) || x == 0 || y == 0) return x * y + z;
    if (fabs(x) > 0x1p500 || fabs(y) > 0x1p500 || fabs(x * y) < 0x1p-900) return x * y + z;
    double p, e;
    two_product(x, y, &p, &e);
    double s = p + z;
    double bp = s - p, err = (p - (s - bp)) + (z - bp);
    return s + (err + e);
}
/* The float product is exact in double, so one double addition suffices
 * apart from rare double-rounding ties. */
float fmaf(float x, float y, float z) { return (float)((double)x * (double)y + (double)z); }

double asinh(double x) {
    if (x == 0 || !isfinite(x)) return x;
    double a = fabs(x), r;
    if (a > 0x1p28) r = log(a) + M_LN2;
    else if (a < 0x1p-28) r = a;
    else r = log1p(a + a * a / (1 + sqrt(1 + a * a)));
    return copysign(r, x);
}

double acosh(double x) {
    if (x < 1) return NAN;
    if (x == 1) return 0;
    if (!isfinite(x)) return x;
    if (x > 0x1p28) return log(x) + M_LN2;
    double t = x - 1;
    return log1p(t + sqrt(t * (t + 2)));
}

double atanh(double x) {
    if (x == 0 || isnan(x)) return x;
    double a = fabs(x);
    if (a > 1) return NAN;
    return copysign(0.5 * log1p(2 * a / (1 - a)), x);
}
