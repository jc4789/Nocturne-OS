#pragma once
#define M_PI 3.14159265358979323846
#define M_E 2.7182818284590452354
#define M_SQRT2 1.41421356237309504880
#define INFINITY __builtin_inf()
#define NAN __builtin_nan("")
#define HUGE_VAL __builtin_huge_val()
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
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
