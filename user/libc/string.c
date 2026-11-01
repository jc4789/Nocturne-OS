#define NOCTURNE_STRING_IMPL
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdint.h>
#include <errno.h>
#include <emmintrin.h>
#include <wchar.h>

/* Most calls copy, fill or scan a few dozen bytes. rep movsb/stosb pay a
   startup cost of tens of cycles that only pays off for long runs, so short
   sizes use overlapping unaligned loads and stores (all loads before the
   stores they could overlap) and SSE2 blocks, and only long runs use the
   string instructions. Scans read whole aligned 16-byte blocks, which never
   cross a page and so never touch memory the caller could not read. */

static inline uint64_t load64(const unsigned char *p) { uint64_t v; __builtin_memcpy(&v, p, 8); return v; }
static inline uint32_t load32(const unsigned char *p) { uint32_t v; __builtin_memcpy(&v, p, 4); return v; }
static inline void store64(unsigned char *p, uint64_t v) { __builtin_memcpy(p, &v, 8); }
static inline void store32(unsigned char *p, uint32_t v) { __builtin_memcpy(p, &v, 4); }

/* 0..16 bytes; correct for any overlap because every load precedes every store */
static inline void copy_small(unsigned char *dp, const unsigned char *sp, size_t n) {
    if (n >= 8) {
        uint64_t a = load64(sp), b = load64(sp + n - 8);
        store64(dp, a);
        store64(dp + n - 8, b);
    } else if (n >= 4) {
        uint32_t a = load32(sp), b = load32(sp + n - 4);
        store32(dp, a);
        store32(dp + n - 4, b);
    } else if (n) {
        unsigned char a = sp[0], b = sp[n / 2], c = sp[n - 1];
        dp[0] = a;
        dp[n / 2] = b;
        dp[n - 1] = c;
    }
}

void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *dp = d;
    const unsigned char *sp = s;
    if (n <= 16) {
        copy_small(dp, sp, n);
        return d;
    }
    if (n <= 256) {
        __m128i last = _mm_loadu_si128((const __m128i *)(sp + n - 16));
        for (size_t i = 0; i + 16 < n; i += 16)
            _mm_storeu_si128((__m128i *)(dp + i), _mm_loadu_si128((const __m128i *)(sp + i)));
        _mm_storeu_si128((__m128i *)(dp + n - 16), last);
        return d;
    }
    __asm__ volatile("rep movsb" : "+D"(dp), "+S"(sp), "+c"(n) : : "memory");
    return d;
}

void *memmove(void *d, const void *s, size_t n) {
    unsigned char *dp = d;
    const unsigned char *sp = s;
    if (dp == sp || n == 0) return d;
    if (n <= 16) {
        copy_small(dp, sp, n);
        return d;
    }
    if (dp + n <= sp || dp >= sp + n) return memcpy(d, s, n);
    if (dp < sp) {
        /* forward: each block is read before any later store reaches it */
        size_t i = 0;
        for (; i + 16 <= n; i += 16)
            _mm_storeu_si128((__m128i *)(dp + i), _mm_loadu_si128((const __m128i *)(sp + i)));
        for (; i < n; i++) dp[i] = sp[i];
    } else {
        size_t i = n;
        for (; i >= 16; i -= 16)
            _mm_storeu_si128((__m128i *)(dp + i - 16), _mm_loadu_si128((const __m128i *)(sp + i - 16)));
        while (i--) dp[i] = sp[i];
    }
    return d;
}

void *memset(void *d, int c, size_t n) {
    unsigned char *dp = d;
    uint64_t v = (unsigned char)c * 0x0101010101010101ull;
    if (n <= 16) {
        if (n >= 8) { store64(dp, v); store64(dp + n - 8, v); }
        else if (n >= 4) { store32(dp, (uint32_t)v); store32(dp + n - 4, (uint32_t)v); }
        else if (n) { dp[0] = (unsigned char)c; dp[n / 2] = (unsigned char)c; dp[n - 1] = (unsigned char)c; }
        return d;
    }
    if (n <= 256) {
        __m128i x = _mm_set1_epi8((char)c);
        for (size_t i = 0; i + 16 < n; i += 16) _mm_storeu_si128((__m128i *)(dp + i), x);
        _mm_storeu_si128((__m128i *)(dp + n - 16), x);
        return d;
    }
    __asm__ volatile("rep stosb" : "+D"(dp), "+c"(n) : "a"(c) : "memory");
    return d;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        __m128i p = _mm_loadu_si128((const __m128i *)(x + i)), q = _mm_loadu_si128((const __m128i *)(y + i));
        unsigned diff = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(p, q)) ^ 0xFFFFu;
        if (diff) {
            unsigned k = (unsigned)__builtin_ctz(diff);
            return x[i + k] - y[i + k];
        }
    }
    for (; i < n; i++)
        if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    if (!n) return NULL;
    const __m128i needle = _mm_set1_epi8((char)c);
    /* the aligned block holding p, with the bytes before p masked off */
    uintptr_t misalign = (uintptr_t)p & 15;
    const unsigned char *block = p - misalign;
    unsigned mask = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_load_si128((const __m128i *)block), needle));
    mask &= 0xFFFFu << misalign;
    size_t remaining = n + misalign;
    for (;;) {
        if (mask) {
            size_t k = (size_t)__builtin_ctz(mask);
            return k < remaining ? (void *)(block + k) : NULL;
        }
        if (remaining <= 16) return NULL;
        remaining -= 16;
        block += 16;
        mask = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_load_si128((const __m128i *)block), needle));
    }
}

size_t strlen(const char *s) {
    const __m128i zero = _mm_setzero_si128();
    uintptr_t misalign = (uintptr_t)s & 15;
    const char *block = s - misalign;
    unsigned mask = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_load_si128((const __m128i *)block), zero));
    mask &= 0xFFFFu << misalign;
    while (!mask) {
        block += 16;
        mask = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_load_si128((const __m128i *)block), zero));
    }
    return (size_t)(block + __builtin_ctz(mask) - s);
}

size_t strnlen(const char *s, size_t max) {
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

char *strcpy(char *d, const char *s) {
    char *r = d;
    while ((*d++ = *s++)) {}
    return r;
}

char *strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}

size_t strlcpy(char *d, const char *s, size_t n) {
    size_t len = strlen(s);
    if (n) {
        size_t c = len < n - 1 ? len : n - 1;
        memcpy(d, s, c);
        d[c] = 0;
    }
    return len;
}

size_t strlcat(char *d, const char *s, size_t n) {
    size_t dl = strnlen(d, n);
    if (dl == n) return n + strlen(s);
    return dl + strlcpy(d + dl, s, n - dl);
}

char *strcat(char *d, const char *s) {
    strcpy(d + strlen(d), s);
    return d;
}

char *strncat(char *d, const char *s, size_t n) {
    char *p = d + strlen(d);
    size_t i = 0;
    for (; i < n && s[i]; i++) p[i] = s[i];
    p[i] = 0;
    return d;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i]) return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}

int strcoll(const char *a, const char *b) { return strcmp(a, b); }

/* Byte order needs no transformation: the result is the string itself. */
size_t strxfrm(char *d, const char *s, size_t n) {
    size_t len = strlen(s);
    if (len < n) memcpy(d, s, len + 1);
    return len;
}

int strcasecmp(const char *a, const char *b) {
    while (*a && tolower(*a) == tolower(*b)) a++, b++;
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int x = tolower((unsigned char)a[i]), y = tolower((unsigned char)b[i]);
        if (x != y || !x) return x - y;
    }
    return 0;
}

char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return NULL;
    }
}

char *strrchr(const char *s, int c) {
    const char *r = NULL;
    for (;; s++) {
        if (*s == (char)c) r = s;
        if (!*s) return (char *)r;
    }
}

char *strstr(const char *h, const char *n) {
    size_t nl = strlen(n);
    if (!nl) return (char *)h;
    for (; *h; h++)
        if (*h == *n && !strncmp(h, n, nl)) return (char *)h;
    return NULL;
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

char *strndup(const char *s, size_t n) {
    size_t l = strnlen(s, n);
    char *d = malloc(l + 1);
    if (d) {
        memcpy(d, s, l);
        d[l] = 0;
    }
    return d;
}

size_t strspn(const char *s, const char *a) {
    size_t n = 0;
    while (s[n] && strchr(a, s[n])) n++;
    return n;
}

size_t strcspn(const char *s, const char *r) {
    size_t n = 0;
    while (s[n] && !strchr(r, s[n])) n++;
    return n;
}

char *strpbrk(const char *s, const char *a) {
    s += strcspn(s, a);
    return *s ? (char *)s : NULL;
}

char *strtok_r(char *s, const char *delim, char **save) {
    if (!s) s = *save;
    if (!s) return NULL;
    s += strspn(s, delim);
    if (!*s) {
        *save = NULL;
        return NULL;
    }
    char *e = s + strcspn(s, delim);
    if (*e) {
        *e = 0;
        *save = e + 1;
    } else {
        *save = NULL;
    }
    return s;
}

char *strtok(char *s, const char *delim) {
    static char *save;
    return strtok_r(s, delim, &save);
}

/* <wchar.h>'s strings of UTF-32 code units, compared by code point. */

size_t wcslen(const wchar_t *s) {
    const wchar_t *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

wchar_t *wmemcpy(wchar_t *d, const wchar_t *s, size_t n) { return memcpy(d, s, n * sizeof *s); }
wchar_t *wmemmove(wchar_t *d, const wchar_t *s, size_t n) { return memmove(d, s, n * sizeof *s); }
wchar_t *wmemset(wchar_t *s, wchar_t c, size_t n) {
    for (size_t i = 0; i < n; i++) s[i] = c;
    return s;
}
int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (s[i] == c) return (wchar_t *)s + i;
    return NULL;
}

wchar_t *wcscpy(wchar_t *d, const wchar_t *s) { return wmemcpy(d, s, wcslen(s) + 1); }
wchar_t *wcsncpy(wchar_t *d, const wchar_t *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    wmemset(d + i, 0, n - i);
    return d;
}
wchar_t *wcscat(wchar_t *d, const wchar_t *s) {
    wcscpy(d + wcslen(d), s);
    return d;
}
wchar_t *wcsncat(wchar_t *d, const wchar_t *s, size_t n) {
    wchar_t *e = d + wcslen(d);
    size_t i = 0;
    for (; i < n && s[i]; i++) e[i] = s[i];
    e[i] = 0;
    return d;
}

int wcscmp(const wchar_t *a, const wchar_t *b) {
    while (*a && *a == *b) a++, b++;
    return *a == *b ? 0 : *a < *b ? -1 : 1;
}
int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
        if (!a[i]) break;
    }
    return 0;
}
int wcscoll(const wchar_t *a, const wchar_t *b) { return wcscmp(a, b); }
/* Code point order needs no transformation: the result is the string itself. */
size_t wcsxfrm(wchar_t *d, const wchar_t *s, size_t n) {
    size_t len = wcslen(s);
    if (len < n) wmemcpy(d, s, len + 1);
    return len;
}

wchar_t *wcschr(const wchar_t *s, wchar_t c) {
    for (;; s++) {
        if (*s == c) return (wchar_t *)s;
        if (!*s) return NULL;
    }
}
wchar_t *wcsrchr(const wchar_t *s, wchar_t c) {
    const wchar_t *found = NULL;
    for (;; s++) {
        if (*s == c) found = s;
        if (!*s) return (wchar_t *)found;
    }
}
size_t wcsspn(const wchar_t *s, const wchar_t *accept) {
    size_t n = 0;
    while (s[n] && wcschr(accept, s[n])) n++;
    return n;
}
size_t wcscspn(const wchar_t *s, const wchar_t *reject) {
    size_t n = 0;
    while (s[n] && !wcschr(reject, s[n])) n++;
    return n;
}
wchar_t *wcspbrk(const wchar_t *s, const wchar_t *accept) {
    s += wcscspn(s, accept);
    return *s ? (wchar_t *)s : NULL;
}
wchar_t *wcsstr(const wchar_t *h, const wchar_t *n) {
    size_t len = wcslen(n);
    for (; *h; h++)
        if (!wmemcmp(h, n, len)) return (wchar_t *)h;
    return len ? NULL : (wchar_t *)h;
}
/* ISO C's wcstok keeps its place in *save, like strtok_r. */
wchar_t *wcstok(wchar_t *s, const wchar_t *delim, wchar_t **save) {
    if (!s) s = *save;
    if (!s) return NULL;
    s += wcsspn(s, delim);
    if (!*s) {
        *save = NULL;
        return NULL;
    }
    wchar_t *e = s + wcscspn(s, delim);
    if (*e) {
        *e = 0;
        *save = e + 1;
    } else {
        *save = NULL;
    }
    return s;
}

char *strerror(int e) {
    switch (e) {
    case N_OK: return "success";
    case N_ERR_NOT_FOUND: return "not found";
    case N_ERR_EXISTS: return "already exists";
    case N_ERR_DENIED: return "not permitted";
    case N_ERR_NO_MEMORY: return "out of memory";
    case N_ERR_INVALID: return "invalid argument";
    case N_ERR_WRONG_KIND: return "wrong kind of handle";
    case N_ERR_NO_HANDLE: return "no such handle";
    case N_ERR_BUSY: return "busy";
    case N_ERR_NOT_READY: return "not ready";
    case N_ERR_CLOSED: return "closed";
    case N_ERR_TIMEOUT: return "timed out";
    case N_ERR_CANCELLED: return "cancelled";
    case N_ERR_TOO_LARGE: return "too large";
    case N_ERR_NOT_EMPTY: return "folder not empty";
    case N_ERR_IO: return "I/O error";
    case N_ERR_UNSUPPORTED: return "not supported";
    case N_ERR_BAD_ADDRESS: return "bad address";
    case N_ERR_STOPPED: return "stopped";
    case N_ERR_NOT_FOLDER: return "not a folder";
    case N_ERR_IS_FOLDER: return "is a folder";
    case N_ERR_READ_ONLY: return "read-only";
    case N_ERR_NO_SPACE: return "no space left";
    case N_ERR_NOT_PROGRAM: return "not a program";
    case N_ERR_NAME_TOO_LONG: return "name too long";
    case N_ERR_NO_APP: return "no such app";
    case N_ERR_TOO_MANY_ARGUMENTS: return "too many arguments";
    case N_ERR_CONNECTION_RESET: return "connection reset";
    case N_ERR_CONNECTION_REFUSED: return "connection refused";
    case N_ERR_ADDRESS_IN_USE: return "address in use";
    case N_ERR_NETWORK_DOWN: return "network is down";
    case N_ERR_NOT_CONNECTED: return "not connected";
    case N_ERR_UNREACHABLE: return "host unreachable";
    case N_ERR_PROTOCOL: return "protocol error";
    case EDOM: return "argument out of domain";
    case ERANGE: return "result out of range";
    case EILSEQ: return "illegal byte sequence";
    default: return "unknown error";
    }
}
