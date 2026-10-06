#pragma once
#define M_PI 3.14159265358979323846
#define M_E 2.7182818284590452354
#define M_SQRT2 1.41421356237309504880
#ifdef __TINYC__ /* TinyCC (the in-OS compiler) lacks these builtins */
#define INFINITY 1e50f
#define NAN (0.0f / 0.0f)
#define HUGE_VAL 1e500
#define HUGE_VALF 1e50f
#define HUGE_VALL 1e5000L
#define isnan(x) ((x) != (x))
#define isinf(x) (!isnan(x) && isnan((x) - (x)))
#define isfinite(x) (!isnan((x) - (x)))
#define signbit(x) ((x) < 0 || (1 / (double)(x)) < 0)
#else
#define INFINITY __builtin_inf()
#define NAN __builtin_nan("")
#define HUGE_VAL __builtin_huge_val()
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)
#endif
#ifndef __TINYC__
#define HUGE_VALF __builtin_huge_valf()
#define HUGE_VALL __builtin_huge_vall()
#endif
#define M_PI_2 1.57079632679489661923
#define M_LN2 0.69314718055994530942
#define M_LN10 2.30258509299404568402
double sqrt(double x);
double fabs(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double atan(double x);
double atan2(double y, double x);
double asin(double x);
double acos(double x);
double exp(double x);
double log(double x);
double log10(double x);
double log2(double x);
double pow(double x, double y);
double floor(double x);
double ceil(double x);
double round(double x);
double trunc(double x);
double fmod(double x, double y);
double hypot(double x, double y);
double sinh(double x);
double cosh(double x);
double tanh(double x);
float sqrtf(float x);
float sinf(float x);
float cosf(float x);
float fabsf(float x);
float floorf(float x);
double ldexp(double x, int e);
long double ldexpl(long double x, int e);
float ldexpf(float x, int e);
double scalbn(double x, int e);
double frexp(double x, int *e);
double modf(double x, double *ip);
double copysign(double x, double y);
double fmin(double a, double b);
double fmax(double a, double b);
float fminf(float a, float b);
float fmaxf(float a, float b);
long lround(double x);
double exp2(double x);
double cbrt(double x);
float ceilf(float x);
float roundf(float x);
float truncf(float x);
float powf(float x, float y);
float expf(float x);
float logf(float x);
float tanf(float x);
float atan2f(float y, float x);
float acosf(float x);
float asinf(float x);
float atanf(float x);
float fmodf(float x, float y);
long double fabsl(long double x);
long lrint(double x);
double expm1(double x);
double log1p(double x);
double acosh(double x);
double asinh(double x);
double atanh(double x);
