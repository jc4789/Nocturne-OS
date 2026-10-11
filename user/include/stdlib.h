#pragma once
#include <stddef.h>
#include <cdefs.h>
NOCTURNE_BEGIN_DECLS
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7FFFFFFF
void *malloc(size_t n);
void *calloc(size_t n, size_t m);
void *realloc(void *p, size_t n);
void free(void *p);
NOCTURNE_NORETURN void exit(int code);
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
char *realpath(const char *path, char *resolved);
int mkstemp(char *tmpl);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
NOCTURNE_END_DECLS
