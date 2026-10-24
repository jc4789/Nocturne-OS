#pragma once
/* ISO C <wchar.h>. A wide character is one UTF-32 code unit and the locale's
   multibyte characters are UTF-8 (<multibyte.h>). The "C" locale's collation
   is code point order, so wcscoll is wcscmp and wcsxfrm copies. */
#include <stddef.h>
#include <stdarg.h>
#include <multibyte.h>
#include <cdefs.h>
NOCTURNE_BEGIN_DECLS

#ifndef WCHAR_MAX
#define WCHAR_MIN (-0x7FFFFFFF - 1)
#define WCHAR_MAX 0x7FFFFFFF
#endif

typedef struct FILE FILE;
struct tm;

/* Formatted wide input and output: <stdio.h>'s conversions, where %c, %s and
   %[ take multibyte text and %lc, %ls and %l[ wide text, as in the byte
   functions; widths and precisions count wide characters. */
int fwprintf(FILE *f, const wchar_t *fmt, ...);
int fwscanf(FILE *f, const wchar_t *fmt, ...);
int swprintf(wchar_t *s, size_t n, const wchar_t *fmt, ...);
int swscanf(const wchar_t *s, const wchar_t *fmt, ...);
int vfwprintf(FILE *f, const wchar_t *fmt, va_list ap);
int vfwscanf(FILE *f, const wchar_t *fmt, va_list ap);
int vswprintf(wchar_t *s, size_t n, const wchar_t *fmt, va_list ap);
int vswscanf(const wchar_t *s, const wchar_t *fmt, va_list ap);
int vwprintf(const wchar_t *fmt, va_list ap);
int vwscanf(const wchar_t *fmt, va_list ap);
int wprintf(const wchar_t *fmt, ...);
int wscanf(const wchar_t *fmt, ...);

/* Wide character input and output. A stream's first character function
   fixes its orientation; fwide reads or sets it. */
wint_t fgetwc(FILE *f);
wchar_t *fgetws(wchar_t *s, int n, FILE *f);
wint_t fputwc(wchar_t c, FILE *f);
int fputws(const wchar_t *s, FILE *f);
int fwide(FILE *f, int mode);
wint_t getwc(FILE *f);
wint_t getwchar(void);
wint_t putwc(wchar_t c, FILE *f);
wint_t putwchar(wchar_t c);
wint_t ungetwc(wint_t c, FILE *f);

/* Numeric conversions, as <stdlib.h>'s; leading white space is Unicode's. */
double wcstod(const wchar_t *s, wchar_t **end);
float wcstof(const wchar_t *s, wchar_t **end);
long double wcstold(const wchar_t *s, wchar_t **end);
long wcstol(const wchar_t *s, wchar_t **end, int base);
long long wcstoll(const wchar_t *s, wchar_t **end, int base);
unsigned long wcstoul(const wchar_t *s, wchar_t **end, int base);
unsigned long long wcstoull(const wchar_t *s, wchar_t **end, int base);

/* Wide strings and arrays. */
wchar_t *wcscpy(wchar_t *d, const wchar_t *s);
wchar_t *wcsncpy(wchar_t *d, const wchar_t *s, size_t n);
wchar_t *wmemcpy(wchar_t *d, const wchar_t *s, size_t n);
wchar_t *wmemmove(wchar_t *d, const wchar_t *s, size_t n);
wchar_t *wcscat(wchar_t *d, const wchar_t *s);
wchar_t *wcsncat(wchar_t *d, const wchar_t *s, size_t n);
int wcscmp(const wchar_t *a, const wchar_t *b);
int wcscoll(const wchar_t *a, const wchar_t *b);
int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n);
size_t wcsxfrm(wchar_t *d, const wchar_t *s, size_t n);
int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n);
wchar_t *wcschr(const wchar_t *s, wchar_t c);
size_t wcscspn(const wchar_t *s, const wchar_t *reject);
wchar_t *wcspbrk(const wchar_t *s, const wchar_t *accept);
wchar_t *wcsrchr(const wchar_t *s, wchar_t c);
size_t wcsspn(const wchar_t *s, const wchar_t *accept);
wchar_t *wcsstr(const wchar_t *h, const wchar_t *n);
wchar_t *wcstok(wchar_t *s, const wchar_t *delim, wchar_t **save);
wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n);
size_t wcslen(const wchar_t *s);
wchar_t *wmemset(wchar_t *s, wchar_t c, size_t n);

/* strftime's conversions into a wide string. */
size_t wcsftime(wchar_t *s, size_t max, const wchar_t *fmt, const struct tm *tm);

/* Multibyte and wide characters; a null mbstate_t pointer selects the
   function's own state. */
wint_t btowc(int c);
int wctob(wint_t c);
int mbsinit(const mbstate_t *ps);
size_t mbrlen(const char *s, size_t n, mbstate_t *ps);
size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps);
size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps);
size_t mbsrtowcs(wchar_t *dst, const char **src, size_t len, mbstate_t *ps);
size_t wcsrtombs(char *dst, const wchar_t **src, size_t len, mbstate_t *ps);

NOCTURNE_END_DECLS
