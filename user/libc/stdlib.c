#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>

int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }

unsigned long long strtoull(const char *s, char **end, int base) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    bool neg = false;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {
        p += 2;
        base = 16;
    } else if (base == 0 && p[0] == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
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
        v = v * base + d;
    }
    if (end) *end = (char *)(p == start ? s : p);
    return neg ? -v : v;
}

long long strtoll(const char *s, char **end, int base) { return (long long)strtoull(s, end, base); }
long strtol(const char *s, char **end, int base) { return (long)strtoull(s, end, base); }
unsigned long strtoul(const char *s, char **end, int base) { return (unsigned long)strtoull(s, end, base); }
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

double strtod(const char *s, char **end) { return (double)strtold(s, end); }
float strtof(const char *s, char **end) { return (float)strtold(s, end); }

double atof(const char *s) { return strtod(s, NULL); }

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

static void qsort_rec(char *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    while (n > 1) {
        if (n < 12) {
            for (size_t i = 1; i < n; i++)
                for (size_t j = i; j > 0 && cmp(base + (j - 1) * size, base + j * size) > 0; j--)
                    swap_bytes(base + (j - 1) * size, base + j * size, size);
            return;
        }
        swap_bytes(base, base + (n / 2) * size, size);
        size_t last = 0;
        for (size_t i = 1; i < n; i++)
            if (cmp(base + i * size, base) < 0) swap_bytes(base + (++last) * size, base + i * size, size);
        swap_bytes(base, base + last * size, size);
        /* recurse on the smaller half */
        if (last < n - last - 1) {
            qsort_rec(base, last, size, cmp);
            base += (last + 1) * size;
            n -= last + 1;
        } else {
            qsort_rec(base + (last + 1) * size, n - last - 1, size, cmp);
            n = last;
        }
    }
}

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    qsort_rec(base, n, size, cmp);
}

long long atoll(const char *s) { return strtoll(s, NULL, 10); }
long long llabs(long long x) { return x < 0 ? -x : x; }
div_t div(int a, int b) { return (div_t){a / b, a % b}; }
ldiv_t ldiv(long a, long b) { return (ldiv_t){a / b, a % b}; }

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
