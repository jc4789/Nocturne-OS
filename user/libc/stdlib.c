#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <limits.h>
#include <inttypes.h>
#include <wchar.h>
#include <wctype.h>

int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }

/* The magnitude of the integer at s and its sign, saturating at ULLONG_MAX.
   Sets *end past the digits, or to s when there are none. */
static unsigned long long parse_integer(const char *s, char **end, int base, bool *neg, bool *overflow) {
    const char *p = s;
    *neg = *overflow = false;
    if (base < 0 || base == 1 || base > 36) {
        errno = EINVAL;
        if (end) *end = (char *)s;
        return 0;
    }
    while (isspace((unsigned char)*p)) p++;
    if (*p == '+' || *p == '-') *neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = p[0] == '0' ? 8 : 10;
    }
    unsigned long long v = 0;
    const char *start = p;
    for (;; p++) {
        int d;
        if (isdigit((unsigned char)*p)) d = *p - '0';
        else if (*p >= 'a' && *p <= 'z') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z') d = *p - 'A' + 10;
        else break;
        if (d >= base) break;
        if (__builtin_mul_overflow(v, (unsigned)base, &v) || __builtin_add_overflow(v, (unsigned)d, &v)) {
            *overflow = true;
            v = ULLONG_MAX;
        }
    }
    if (end) *end = (char *)(p == start ? s : p);
    return v;
}

/* strtoull and strtoll for any narrower type: ISO C clamps an out-of-range
   value to the type's limit and sets ERANGE; unsigned conversions negate in
   the unsigned type. */
static unsigned long long to_unsigned(const char *s, char **end, int base, unsigned long long max) {
    bool neg, overflow;
    unsigned long long v = parse_integer(s, end, base, &neg, &overflow);
    if (overflow || v > max) {
        errno = ERANGE;
        return max;
    }
    return neg ? (0 - v) & max : v;
}
static long long to_signed(const char *s, char **end, int base, long long min, long long max) {
    bool neg, overflow;
    unsigned long long v = parse_integer(s, end, base, &neg, &overflow);
    unsigned long long limit = neg ? 0 - (unsigned long long)min : (unsigned long long)max;
    if (overflow || v > limit) {
        errno = ERANGE;
        return neg ? min : max;
    }
    return neg ? (long long)(0 - v) : (long long)v;
}

unsigned long long strtoull(const char *s, char **end, int base) { return to_unsigned(s, end, base, ULLONG_MAX); }
unsigned long strtoul(const char *s, char **end, int base) { return to_unsigned(s, end, base, ULONG_MAX); }
uintmax_t strtoumax(const char *s, char **end, int base) { return to_unsigned(s, end, base, UINTMAX_MAX); }
long long strtoll(const char *s, char **end, int base) { return to_signed(s, end, base, LLONG_MIN, LLONG_MAX); }
long strtol(const char *s, char **end, int base) { return to_signed(s, end, base, LONG_MIN, LONG_MAX); }
intmax_t strtoimax(const char *s, char **end, int base) { return to_signed(s, end, base, INTMAX_MIN, INTMAX_MAX); }
int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }

/* Decimal and hex floating point, plus inf/nan. Up to 19 significant digits are kept in an
   integer and scaled once in 80-bit long double, so double results are (almost always) the
   correctly rounded ones. */
static long double pow10l_(int e) {
    long double r = 1, b = 10;
    bool neg = e < 0;
    unsigned u = neg ? -(unsigned)e : (unsigned)e;
    while (u) {
        if (u & 1) r *= b;
        b *= b;
        u >>= 1;
    }
    return neg ? 1 / r : r;
}

static int ci_prefix(const char *p, const char *word) {
    int n = 0;
    while (word[n] && tolower((unsigned char)p[n]) == word[n]) n++;
    return word[n] ? 0 : n;
}

long double strtold(const char *s, char **end) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    bool neg = false;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    int n;
    if ((n = ci_prefix(p, "inf"))) {
        p += n;
        if ((n = ci_prefix(p, "inity"))) p += n;
        if (end) *end = (char *)p;
        return neg ? -__builtin_infl() : __builtin_infl();
    }
    if ((n = ci_prefix(p, "nan"))) {
        p += n;
        if (*p == '(') {
            const char *q = p + 1;
            while (isalnum((unsigned char)*q) || *q == '_') q++;
            if (*q == ')') p = q + 1;
        }
        if (end) *end = (char *)p;
        return neg ? -__builtin_nanl("") : __builtin_nanl("");
    }
    uint64_t mant = 0;
    int digits = 0, exp = 0;
    bool any = false;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
        (isxdigit((unsigned char)p[2]) || (p[2] == '.' && isxdigit((unsigned char)p[3])))) {
        p += 2;
        bool dot = false;
        for (;; p++) {
            int d;
            if (*p == '.' && !dot) {
                dot = true;
                continue;
            }
            if (*p >= '0' && *p <= '9') d = *p - '0';
            else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
            else if (*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
            else break;
            any = true;
            if (mant >> 60) { /* no room: keep a sticky bit for rounding */
                mant |= d != 0;
                if (!dot) exp += 4;
            } else {
                mant = mant << 4 | (uint64_t)d;
                if (dot) exp -= 4;
            }
        }
        if (*p == 'p' || *p == 'P') {
            const char *q = p + 1;
            bool eneg = false;
            if (*q == '+' || *q == '-') eneg = *q++ == '-';
            if (isdigit((unsigned char)*q)) {
                int e = 0;
                while (isdigit((unsigned char)*q)) {
                    if (e < 100000) e = e * 10 + (*q - '0');
                    q++;
                }
                exp += eneg ? -e : e;
                p = q;
            }
        }
        if (end) *end = (char *)p;
        long double v = ldexpl((long double)mant, exp);
        return neg ? -v : v;
    }
    bool dot = false;
    for (;; p++) {
        if (*p == '.' && !dot) {
            dot = true;
            continue;
        }
        if (!isdigit((unsigned char)*p)) break;
        any = true;
        if (digits < 19) {
            mant = mant * 10 + (uint64_t)(*p - '0');
            if (mant) digits++;
            if (dot) exp--;
        } else if (!dot) {
            exp++;
        }
    }
    if (!any) {
        if (end) *end = (char *)s;
        return 0;
    }
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        bool eneg = false;
        if (*q == '+' || *q == '-') eneg = *q++ == '-';
        if (isdigit((unsigned char)*q)) {
            int e = 0;
            while (isdigit((unsigned char)*q)) {
                if (e < 100000) e = e * 10 + (*q - '0');
                q++;
            }
            exp += eneg ? -e : e;
            p = q;
        }
    }
    if (end) *end = (char *)p;
    long double v = (long double)mant;
    if (mant && exp) {
        if (exp < -4000) v = 0;
        else if (exp > 4000) v = __builtin_infl();
        else v *= pow10l_(exp);
    }
    if (v == __builtin_infl() || (mant && v == 0)) errno = ERANGE;
    return neg ? -v : v;
}

/* A value that is finite or nonzero in long double but not in the narrower
   type is a range error too. */
double strtod(const char *s, char **end) {
    long double v = strtold(s, end);
    double d = (double)v;
    if ((isinf(d) && !isinf(v)) || (d == 0 && v != 0)) errno = ERANGE;
    return d;
}
float strtof(const char *s, char **end) {
    long double v = strtold(s, end);
    float f = (float)v;
    if ((isinf(f) && !isinf(v)) || (f == 0 && v != 0)) errno = ERANGE;
    return f;
}

double atof(const char *s) { return strtod(s, NULL); }

/* <wchar.h>'s numeric conversions. A number's text is ASCII, so after the
   Unicode white space the run of characters that can belong to one is
   narrowed, given to the byte parser above, and its end mapped back. */
struct narrowed {
    char small[64], *text;
    const wchar_t *start;
};
static const char *narrow_number(struct narrowed *n, const wchar_t *s) {
    while (iswspace((wint_t)*s)) s++;
    n->start = s;
    size_t len = 0;
    while (s[len] > 0 && s[len] < 0x80 && (isalnum((int)s[len]) || strchr("+-.()_", (int)s[len]))) len++;
    n->text = len < sizeof n->small ? n->small : malloc(len + 1);
    if (!n->text) {
        errno = ENOMEM;
        return NULL;
    }
    for (size_t i = 0; i < len; i++) n->text[i] = (char)s[i];
    n->text[len] = 0;
    return n->text;
}
static wchar_t *widened_end(struct narrowed *n, const wchar_t *s, const char *end) {
    const wchar_t *w = end == n->text ? s : n->start + (end - n->text);
    if (n->text != n->small) free(n->text);
    return (wchar_t *)w;
}
/* params and args are the parser's arguments after the text and its end. */
#define WIDE_NUMBER(name, type, narrow, params, args)       \
    type name(const wchar_t *s, wchar_t **end params) {     \
        struct narrowed n;                                  \
        char *e;                                            \
        const char *text = narrow_number(&n, s);            \
        if (!text) {                                        \
            if (end) *end = (wchar_t *)s;                   \
            return 0;                                       \
        }                                                   \
        type v = narrow(text, &e args);                     \
        wchar_t *w = widened_end(&n, s, e);                 \
        if (end) *end = w;                                  \
        return v;                                           \
    }
#define BASE , int base
#define WITH_BASE , base
WIDE_NUMBER(wcstol, long, strtol, BASE, WITH_BASE)
WIDE_NUMBER(wcstoll, long long, strtoll, BASE, WITH_BASE)
WIDE_NUMBER(wcstoul, unsigned long, strtoul, BASE, WITH_BASE)
WIDE_NUMBER(wcstoull, unsigned long long, strtoull, BASE, WITH_BASE)
WIDE_NUMBER(wcstoimax, intmax_t, strtoimax, BASE, WITH_BASE)
WIDE_NUMBER(wcstoumax, uintmax_t, strtoumax, BASE, WITH_BASE)
WIDE_NUMBER(wcstod, double, strtod, , )
WIDE_NUMBER(wcstof, float, strtof, , )
WIDE_NUMBER(wcstold, long double, strtold, , )

static unsigned long rng = 1;
int rand(void) {
    rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (int)((rng >> 33) & RAND_MAX);
}
void srand(unsigned seed) { rng = seed; }

static void swap_bytes(char *a, char *b, size_t n) {
    while (n--) {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

/* qsort's comparison, or qsort_s's with its context */
struct sort_order {
    int (*plain)(const void *, const void *);
    int (*with)(const void *, const void *, void *);
    void *context;
};
static int order(const struct sort_order *o, const void *a, const void *b) {
    return o->plain ? o->plain(a, b) : o->with(a, b, o->context);
}

static void qsort_rec(char *base, size_t n, size_t size, const struct sort_order *o) {
    while (n > 1) {
        if (n < 12) {
            for (size_t i = 1; i < n; i++)
                for (size_t j = i; j > 0 && order(o, base + (j - 1) * size, base + j * size) > 0; j--)
                    swap_bytes(base + (j - 1) * size, base + j * size, size);
            return;
        }
        swap_bytes(base, base + (n / 2) * size, size);
        size_t last = 0;
        for (size_t i = 1; i < n; i++)
            if (order(o, base + i * size, base) < 0) swap_bytes(base + (++last) * size, base + i * size, size);
        swap_bytes(base, base + last * size, size);
        /* recurse on the smaller half */
        if (last < n - last - 1) {
            qsort_rec(base, last, size, o);
            base += (last + 1) * size;
            n -= last + 1;
        } else {
            qsort_rec(base + (last + 1) * size, n - last - 1, size, o);
            n = last;
        }
    }
}

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    qsort_rec(base, n, size, &(struct sort_order){.plain = cmp});
}
/* ISO C Annex K: the comparison also receives the caller's context. */
int qsort_s(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *, void *), void *context) {
    if (n && (!base || !cmp || !size)) return EINVAL;
    qsort_rec(base, n, size, &(struct sort_order){.with = cmp, .context = context});
    return 0;
}

long long atoll(const char *s) { return strtoll(s, NULL, 10); }
long long llabs(long long x) { return x < 0 ? -x : x; }
div_t div(int a, int b) { return (div_t){a / b, a % b}; }
ldiv_t ldiv(long a, long b) { return (ldiv_t){a / b, a % b}; }
lldiv_t lldiv(long long a, long long b) { return (lldiv_t){a / b, a % b}; }
intmax_t imaxabs(intmax_t x) { return x < 0 ? -x : x; }
imaxdiv_t imaxdiv(intmax_t a, intmax_t b) { return (imaxdiv_t){a / b, a % b}; }

void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *)) {
    const char *lo = base;
    while (n) {
        const char *mid = lo + (n / 2) * size;
        int c = cmp(key, mid);
        if (c == 0) return (void *)mid;
        if (c > 0) {
            lo = mid + size;
            n -= n / 2 + 1;
        } else {
            n /= 2;
        }
    }
    return NULL;
}
