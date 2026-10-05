#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>

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

double strtod(const char *s, char **end) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    bool neg = false;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    const char *start = p;
    double v = 0;
    bool any = false;
    while (isdigit((unsigned char)*p)) {
        v = v * 10 + (*p++ - '0');
        any = true;
    }
    if (*p == '.') {
        p++;
        double scale = 0.1;
        while (isdigit((unsigned char)*p)) {
            v += (*p++ - '0') * scale;
            scale *= 0.1;
            any = true;
        }
    }
    if (any && (*p == 'e' || *p == 'E')) {
        const char *q = p + 1;
        bool eneg = false;
        if (*q == '+' || *q == '-') eneg = *q++ == '-';
        if (isdigit((unsigned char)*q)) {
            int e = 0;
            while (isdigit((unsigned char)*q)) e = e * 10 + (*q++ - '0');
            double m = 1;
            while (e--) m *= 10;
            v = eneg ? v / m : v * m;
            p = q;
        }
    }
    if (!any) p = start == s ? s : s;
    if (end) *end = (char *)(any ? p : s);
    return neg ? -v : v;
}

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
