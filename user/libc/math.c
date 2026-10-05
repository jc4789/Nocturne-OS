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

double log2(double x) {
    double r;
    __asm__("fld1; fxch; fyl2x" : "=t"(r) : "0"(x));
    return r;
}

double log(double x) { return log2(x) * 0.69314718055994530942; }
double log10(double x) { return log2(x) * 0.30102999566398119521; }

static double exp2_(double x) {
    /* 2^x = 2^int * 2^frac */
    double r;
    __asm__("fld %%st(0)\n\t"
            "frndint\n\t"
            "fsub %%st(0), %%st(1)\n\t"
            "fxch\n\t"
            "f2xm1\n\t"
            "fld1\n\t"
            "faddp\n\t"
            "fscale\n\t"
            "fstp %%st(1)"
            : "=t"(r)
            : "0"(x));
    return r;
}

double exp(double x) {
    if (x > 709) return INFINITY;
    if (x < -745) return 0;
    return exp2_(x * 1.44269504088896340736);
}

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
    if (x < 0) {
        if (y != floor(y)) return NAN;
        double r = exp2_(y * log2(-x));
        return ((int64_t)y & 1) ? -r : r;
    }
    return exp2_(y * log2(x));
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
