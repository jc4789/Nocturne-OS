/* libm on top of SSE2 and the x87 transcendental instructions. */
#include <math.h>
#include <fenv.h>
#include <stdint.h>
#include <stdbool.h>

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

double fabs(double x) {
    union { double d; uint64_t u; } v = {x};
    v.u &= ~(1ULL << 63);
    return v.d;
}

double sqrt(double x) {
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}

double sin(double x) {
    double r;
    __asm__("fsin" : "=t"(r) : "0"(x));
    return r;
}

double cos(double x) {
    double r;
    __asm__("fcos" : "=t"(r) : "0"(x));
    return r;
}

double tan(double x) {
    double r, one;
    __asm__("fptan" : "=t"(one), "=u"(r) : "0"(x));
    return r;
}

double atan2(double y, double x) {
    double r;
    __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    return r;
}

double atan(double x) { return atan2(x, 1.0); }
double asin(double x) { return atan2(x, sqrt((1 - x) * (1 + x))); }
double acos(double x) { return atan2(sqrt((1 - x) * (1 + x)), x); }

/* log, exp and pow use the x87 unit in 80-bit precision, one instruction per helper (multi-
   instruction x87 asm is easy to get wrong: AT&T syntax swaps the operands of fsub/fdiv). */
static long double x87_log2(long double x) {
    long double r;
    __asm__("fld1; fxch; fyl2x" : "=t"(r) : "0"(x)); /* 1 * log2(x) */
    return r;
}
static long double x87_rint(long double x) {
    long double r;
    __asm__("frndint" : "=t"(r) : "0"(x));
    return r;
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
static long double x87_f2xm1(long double x) { /* 2^x - 1 for |x| <= 1 */
    long double r;
    __asm__("f2xm1" : "=t"(r) : "0"(x));
    return r;
}
static long double x87_scale(long double x, long double n) { /* x * 2^n */
    long double r;
    __asm__("fscale" : "=t"(r) : "0"(x), "u"(n));
    return r;
}

static long double exp2l_(long double t) {
    if (t != t) return t;
    if (t > 20000) return INFINITY;
    if (t < -20000) return 0;
    long double n = x87_rint(t);
    return x87_scale(x87_f2xm1(t - n) + 1, n);
}

#define LN2L  0.693147180559945309417232121458176568L
#define LOG2EL 1.442695040888963407359924681001892137L

double log2(double x) { return (double)x87_log2(x); }
double log(double x) { return (double)(x87_log2(x) * LN2L); }
double log10(double x) { return (double)(x87_log2(x) * 0.301029995663981195213738894724493027L); }
double exp(double x) { return (double)exp2l_((long double)x * LOG2EL); }

double floor(double x) {
    if (!(fabs(x) < 4503599627370496.0)) return x;
    double i = (double)(int64_t)x;
    return i > x ? i - 1 : i;
}

double ceil(double x) {
    if (!(fabs(x) < 4503599627370496.0)) return x;
    double i = (double)(int64_t)x;
    return i < x ? i + 1 : i;
}

double trunc(double x) {
    if (!(fabs(x) < 4503599627370496.0)) return x;
    return (double)(int64_t)x;
}

double round(double x) { return x < 0 ? -floor(-x + 0.5) : floor(x + 0.5); }

double pow(double x, double y) {
    if (y == 0) return 1;
    if (x == 0) return y > 0 ? 0 : INFINITY;
    if (y == (double)(int64_t)y && fabs(y) < 64) {
        int64_t n = (int64_t)y;
        bool neg = n < 0;
        if (neg) n = -n;
        double r = 1, b = x;
        while (n) {
            if (n & 1) r *= b;
            b *= b;
            n >>= 1;
        }
        return neg ? 1 / r : r;
    }
    if (x == 1 || y != y || x != x) return x == 1 ? 1 : NAN;
    if (x < 0) {
        if (y != floor(y)) return NAN;
        double r = (double)exp2l_((long double)y * x87_log2(-x));
        return fmod(y, 2) != 0 ? -r : r; /* odd integer power of a negative number */
    }
    return (double)exp2l_((long double)y * x87_log2(x));
}

double fmod(double x, double y) {
    double r;
    __asm__("1: fprem\n\t"
            "fnstsw %%ax\n\t"
            "testw $0x400, %%ax\n\t"
            "jnz 1b"
            : "=t"(r)
            : "0"(x), "u"(y)
            : "ax", "cc");
    return r;
}

double hypot(double x, double y) { return sqrt(x * x + y * y); }
double sinh(double x) { return (exp(x) - exp(-x)) / 2; }
double cosh(double x) { return (exp(x) + exp(-x)) / 2; }
double tanh(double x) {
    if (x > 20) return 1;
    if (x < -20) return -1;
    double e = exp(2 * x);
    return (e - 1) / (e + 1);
}

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

double ldexp(double x, int e) { return (double)ldexpl(x, e); }
double scalbn(double x, int e) { return ldexp(x, e); }
float ldexpf(float x, int e) { return (float)ldexpl(x, e); }

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

double modf(double x, double *ip) {
    double t = trunc(x);
    *ip = t;
    return isinf(x) ? 0.0 * x : x - t;
}

double copysign(double x, double y) {
    union { double d; uint64_t u; } a = {x}, b = {y};
    a.u = (a.u & ~(1ULL << 63)) | (b.u & (1ULL << 63));
    return a.d;
}

double fmin(double a, double b) { return isnan(a) ? b : isnan(b) ? a : a < b ? a : b; }
double fmax(double a, double b) { return isnan(a) ? b : isnan(b) ? a : a > b ? a : b; }
float fminf(float a, float b) { return (float)fmin(a, b); }
float fmaxf(float a, float b) { return (float)fmax(a, b); }
long lround(double x) { return (long)round(x); }
double exp2(double x) { return (double)exp2l_(x); }
double cbrt(double x) { return x < 0 ? -pow(-x, 1.0 / 3) : pow(x, 1.0 / 3); }
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
static void two_product(double a, double b, double *p, double *e) {
    const double split = 134217729.0; /* 2^27 + 1 */
    double ca = split * a, cb = split * b;
    double ah = ca - (ca - a), al = a - ah;
    double bh = cb - (cb - b), bl = b - bh;
    *p = a * b;
    *e = ((ah * bh - *p) + ah * bl + al * bh) + al * bl;
}
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

double expm1(double x) {
    if (x == 0 || isnan(x)) return x;
    if (x == -INFINITY) return -1;
    if (fabs(x) <= 0.5) return (double)x87_f2xm1((long double)x * LOG2EL);
    return exp(x) - 1;
}

double log1p(double x) {
    if (x == 0 || isnan(x)) return x;
    if (x == INFINITY) return x;
    if (x == -1) return -INFINITY;
    if (x < -1) return NAN;
    if (fabs(x) < 0.25) {
        long double r;
        __asm__("fyl2xp1" : "=t"(r) : "0"((long double)x), "u"(LN2L) : "st(1)");
        return (double)r;
    }
    double u = 1 + x;
    return log(u) - ((u - 1) - x) / u;
}

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
