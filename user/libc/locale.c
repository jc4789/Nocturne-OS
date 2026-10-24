#include <locale.h>
#include <uchar.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

/* Nocturne's one locale, "C" (<locale.h>): numbers, money and times as ISO C
   defines them for "C", and UTF-8 multibyte characters. */

char *setlocale(int category, const char *locale) {
    static char name[] = "C";
    if (category < LC_ALL || category > LC_TIME) return NULL;
    if (locale && *locale && strcmp(locale, "C") != 0 && strcmp(locale, "C.UTF-8") != 0) return NULL;
    return name;
}

struct lconv *localeconv(void) {
    static char point[] = ".", none[] = "";
    static struct lconv c = {
        point, none, none, none, none, none, none, none, none, none,
        CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX,
        CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX,
    };
    return &c;
}

/* UTF-8 as Unicode defines it: the shortest form only, no surrogates, nothing
   above U+10FFFF. A lead byte fixes how many bytes follow and the range the
   first of them must be in; the rest are 80..BF. */

#define BAD ((size_t)-1)
#define INCOMPLETE ((size_t)-2)
#define MORE_UNITS ((size_t)-3)

/* Feeds one byte to the decoder in st: 1 when it completes the character in
   *out, 0 when more bytes must come, -1 when it cannot be part of one. */
static int decode_byte(mbstate_t *st, unsigned char b, char32_t *out) {
    if (!st->__need) {
        if (b < 0x80) { *out = b; return 1; }
        st->__low = 0x80, st->__high = 0xBF;
        if (b >= 0xC2 && b <= 0xDF) st->__need = 1, st->__value = b & 0x1F;
        else if (b >= 0xE0 && b <= 0xEF) {
            st->__need = 2, st->__value = b & 0x0F;
            if (b == 0xE0) st->__low = 0xA0;
            else if (b == 0xED) st->__high = 0x9F;
        } else if (b >= 0xF0 && b <= 0xF4) {
            st->__need = 3, st->__value = b & 0x07;
            if (b == 0xF0) st->__low = 0x90;
            else if (b == 0xF4) st->__high = 0x8F;
        } else return -1;
        return 0;
    }
    if (b < st->__low || b > st->__high) { *st = (mbstate_t){0}; return -1; }
    st->__value = st->__value << 6 | (b & 0x3F);
    st->__low = 0x80, st->__high = 0xBF;
    if (--st->__need) return 0;
    *out = st->__value;
    st->__value = 0;
    return 1;
}

static size_t utf8_length(char32_t c) { return c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4; }

/* The code unit at index i of c's UTF-8 form. */
static unsigned char utf8_unit(char32_t c, size_t i) {
    static const unsigned char lead[] = { 0, 0, 0xC0, 0xE0, 0xF0 };
    size_t len = utf8_length(c);
    if (len == 1) return (unsigned char)c;
    unsigned char unit = (c >> 6 * (len - 1 - i)) & 0x3F;
    return i ? 0x80 | unit : lead[len] | (unit & (0x7F >> len));
}

static size_t utf8_put(char *s, char32_t c) {
    size_t len = utf8_length(c);
    for (size_t i = 0; i < len; i++) s[i] = (char)utf8_unit(c, i);
    return len;
}

static int is_character(char32_t c) { return c <= 0x10FFFF && (c < 0xD800 || c > 0xDFFF); }

size_t mbrtoc32(char32_t *pc32, const char *s, size_t n, mbstate_t *ps) {
    static mbstate_t internal;
    if (!ps) ps = &internal;
    if (!s) s = "", n = 1, pc32 = NULL;
    for (size_t i = 0; i < n; i++) {
        char32_t c;
        int r = decode_byte(ps, (unsigned char)s[i], &c);
        if (r < 0) { errno = EILSEQ; return BAD; }
        if (r) {
            if (pc32) *pc32 = c;
            return c ? i + 1 : 0;
        }
    }
    return INCOMPLETE;
}

/* A character above U+FFFF gives a surrogate pair: the high half is returned
   with the bytes, the low half is kept in __value for the next call. */
size_t mbrtoc16(char16_t *pc16, const char *s, size_t n, mbstate_t *ps) {
    static mbstate_t internal;
    if (!ps) ps = &internal;
    if (!s) s = "", n = 1, pc16 = NULL;
    if (ps->__give) {
        if (pc16) *pc16 = (char16_t)ps->__value;
        *ps = (mbstate_t){0};
        return MORE_UNITS;
    }
    char32_t c;
    size_t r = mbrtoc32(&c, s, n, ps);
    if (r == BAD || r == INCOMPLETE) return r;
    if (c >= 0x10000) {
        c -= 0x10000;
        ps->__value = 0xDC00 | (c & 0x3FF), ps->__give = 1;
        c = 0xD800 | c >> 10;
    }
    if (pc16) *pc16 = (char16_t)c;
    return r;
}

/* The first UTF-8 code unit is returned with the bytes; the character waits in
   __value while __give of its units are still to come. */
size_t mbrtoc8(char8_t *pc8, const char *s, size_t n, mbstate_t *ps) {
    static mbstate_t internal;
    if (!ps) ps = &internal;
    if (!s) s = "", n = 1, pc8 = NULL;
    if (ps->__give) {
        char32_t c = ps->__value;
        if (pc8) *pc8 = utf8_unit(c, utf8_length(c) - ps->__give);
        if (!--ps->__give) ps->__value = 0;
        return MORE_UNITS;
    }
    char32_t c;
    size_t r = mbrtoc32(&c, s, n, ps);
    if (r == BAD || r == INCOMPLETE) return r;
    if (c >= 0x80) ps->__value = c, ps->__give = (unsigned char)(utf8_length(c) - 1);
    if (pc8) *pc8 = utf8_unit(c, 0);
    return r;
}

size_t c32rtomb(char *s, char32_t c32, mbstate_t *ps) {
    static mbstate_t internal;
    char buf[4];
    if (!ps) ps = &internal;
    if (!s) s = buf, c32 = 0;
    if (!is_character(c32)) { errno = EILSEQ; return BAD; }
    *ps = (mbstate_t){0};
    return utf8_put(s, c32);
}

/* A high surrogate waits in __value (with __need set) for its low half. */
size_t c16rtomb(char *s, char16_t c16, mbstate_t *ps) {
    static mbstate_t internal;
    char buf[4];
    if (!ps) ps = &internal;
    if (!s) s = buf, c16 = 0;
    char32_t c = c16;
    if (ps->__need) {
        if (c < 0xDC00 || c > 0xDFFF) goto bad;
        c = 0x10000 + ((ps->__value - 0xD800) << 10 | (c - 0xDC00));
    } else if (c >= 0xD800 && c <= 0xDBFF) {
        ps->__value = c, ps->__need = 1;
        return 0;
    } else if (c >= 0xDC00 && c <= 0xDFFF) goto bad;
    *ps = (mbstate_t){0};
    return utf8_put(s, c);
bad:
    *ps = (mbstate_t){0};
    errno = EILSEQ;
    return BAD;
}

/* UTF-8 code units are collected until they complete a character. */
size_t c8rtomb(char *s, char8_t c8, mbstate_t *ps) {
    static mbstate_t internal;
    char buf[4];
    if (!ps) ps = &internal;
    if (!s) s = buf, c8 = 0;
    char32_t c;
    int r = decode_byte(ps, c8, &c);
    if (r < 0) { errno = EILSEQ; return BAD; }
    return r ? utf8_put(s, c) : 0;
}

/* <wchar.h>: a wide character is a UTF-32 code unit, so these are the
   char32_t conversions above. */

size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps) {
    static mbstate_t internal;
    char32_t c;
    size_t r = mbrtoc32(&c, s, n, ps ? ps : &internal);
    if (pwc && s && r != BAD && r != INCOMPLETE) *pwc = (wchar_t)c;
    return r;
}

size_t mbrlen(const char *s, size_t n, mbstate_t *ps) {
    static mbstate_t internal;
    return mbrtowc(NULL, s, n, ps ? ps : &internal);
}

size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps) {
    static mbstate_t internal;
    return c32rtomb(s, (char32_t)wc, ps ? ps : &internal);
}

int mbsinit(const mbstate_t *ps) { return !ps || (!ps->__need && !ps->__give); }

/* The one-byte UTF-8 characters are ASCII. */
wint_t btowc(int c) { return c >= 0 && c < 0x80 ? (wint_t)c : WEOF; }
int wctob(wint_t c) { return c >= 0 && c < 0x80 ? (int)c : EOF; }

/* Each character is decoded from at most MB_CUR_MAX bytes; the decoder stops
   at a null byte, so nothing past the string's end is read. */
size_t mbsrtowcs(wchar_t *dst, const char **src, size_t len, mbstate_t *ps) {
    static mbstate_t internal;
    if (!ps) ps = &internal;
    const char *s = *src;
    size_t count = 0;
    for (; !dst || count < len; count++) {
        char32_t c;
        size_t r = mbrtoc32(&c, s, MB_CUR_MAX, ps);
        if (r == BAD || r == INCOMPLETE) {
            if (dst) *src = s;
            errno = EILSEQ;
            return BAD;
        }
        if (dst) dst[count] = (wchar_t)c;
        if (!r) {
            if (dst) *src = NULL;
            return count;
        }
        s += r;
    }
    *src = s;
    return count;
}

size_t wcsrtombs(char *dst, const wchar_t **src, size_t len, mbstate_t *ps) {
    static mbstate_t internal;
    if (!ps) ps = &internal;
    const wchar_t *s = *src;
    size_t count = 0;
    for (;; s++) {
        char units[4];
        size_t r = c32rtomb(units, (char32_t)*s, ps);
        if (r == BAD) {
            if (dst) *src = s;
            return BAD;
        }
        if (!*s) r = 0;
        if (dst) {
            if (r > len - count || (!*s && count == len)) {
                *src = s;
                return count;
            }
            memcpy(dst + count, units, r ? r : 1);
            if (!*s) {
                *src = NULL;
                return count;
            }
        } else if (!*s) {
            return count;
        }
        count += r;
    }
}

/* <stdlib.h>: the locale's multibyte characters carry no shift state, so each
   call starts from the initial state. */

int mbtowc(wchar_t *pwc, const char *s, size_t n) {
    if (!s) return 0;
    mbstate_t st = {0};
    size_t r = mbrtowc(pwc, s, n, &st);
    if (r == BAD || r == INCOMPLETE) {
        errno = EILSEQ;
        return -1;
    }
    return (int)r;
}

int mblen(const char *s, size_t n) { return mbtowc(NULL, s, n); }

int wctomb(char *s, wchar_t wc) {
    if (!s) return 0;
    mbstate_t st = {0};
    size_t r = wcrtomb(s, wc, &st);
    return r == BAD ? -1 : (int)r;
}

size_t mbstowcs(wchar_t *dst, const char *src, size_t n) {
    mbstate_t st = {0};
    return mbsrtowcs(dst, &src, n, &st);
}

size_t wcstombs(char *dst, const wchar_t *src, size_t n) {
    mbstate_t st = {0};
    return wcsrtombs(dst, &src, n, &st);
}
