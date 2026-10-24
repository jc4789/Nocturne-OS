#pragma once
#include <stddef.h>
#include <cdefs.h>
NOCTURNE_BEGIN_DECLS
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7FFFFFFF
/* The longest multibyte character: the locale's characters are UTF-8 (<locale.h>). */
#define MB_CUR_MAX ((size_t)4)
void *malloc(size_t n);
void *calloc(size_t n, size_t m);
void *realloc(void *p, size_t n);
void free(void *p);
/* ISO C: alignment is a power of two; free and realloc take the block. */
void *aligned_alloc(size_t alignment, size_t n);
NOCTURNE_NORETURN void exit(int code);
/* Ends the app at once: no atexit or C++ destructors, no stdio flush. */
NOCTURNE_NORETURN void _Exit(int code);
NOCTURNE_NORETURN void abort(void);
int atexit(void (*fn)(void));
int atoi(const char *s);
long atol(const char *s);
double atof(const char *s);
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
double strtod(const char *s, char **end);
int abs(int x);
long labs(long x);
int rand(void);
void srand(unsigned seed);
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
/* ISO C Annex K; returns 0, or EINVAL for a missing array or comparison. */
int qsort_s(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *, void *), void *context);
char *getenv(const char *name);
int system(const char *cmd);
long double strtold(const char *s, char **end);
float strtof(const char *s, char **end);
long long atoll(const char *s);
long long llabs(long long x);
typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;
div_t div(int a, int b);
ldiv_t ldiv(long a, long b);
lldiv_t lldiv(long long a, long long b);
void *bsearch(const void *key, const void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int mblen(const char *s, size_t n);
int mbtowc(wchar_t *pwc, const char *s, size_t n);
int wctomb(char *s, wchar_t wc);
size_t mbstowcs(wchar_t *dst, const char *src, size_t n);
size_t wcstombs(char *dst, const wchar_t *src, size_t n);
NOCTURNE_END_DECLS
