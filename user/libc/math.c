/* libm on top of SSE2 and the x87 transcendental instructions. */
#include <math.h>
#include <stdint.h>
#include <stdbool.h>

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
float fmodf(float x, float y) { return (float)fmod(x, y); }
long double fabsl(long double x) { return x < 0 ? -x : x; }
