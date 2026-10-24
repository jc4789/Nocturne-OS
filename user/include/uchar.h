#pragma once
#include <stddef.h>
#include <stdint.h>
#include <multibyte.h>
#include <cdefs.h>
NOCTURNE_BEGIN_DECLS
/* ISO C <uchar.h>: the locale's multibyte characters (UTF-8, <locale.h>) to
   and from UTF-8, UTF-16 and UTF-32 code units. Each conversion keeps its own
   state in an mbstate_t (<multibyte.h>). */
#ifndef __cplusplus
typedef unsigned char char8_t;
typedef uint_least16_t char16_t;
typedef uint_least32_t char32_t;
#endif
#if !defined(__cplusplus) || defined(__cpp_char8_t)
size_t mbrtoc8(char8_t *pc8, const char *s, size_t n, mbstate_t *ps);
size_t c8rtomb(char *s, char8_t c8, mbstate_t *ps);
#endif
size_t mbrtoc16(char16_t *pc16, const char *s, size_t n, mbstate_t *ps);
size_t c16rtomb(char *s, char16_t c16, mbstate_t *ps);
size_t mbrtoc32(char32_t *pc32, const char *s, size_t n, mbstate_t *ps);
size_t c32rtomb(char *s, char32_t c32, mbstate_t *ps);
NOCTURNE_END_DECLS
