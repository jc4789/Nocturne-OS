#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdint.h>
#include <errno.h>

void *memcpy(void *d, const void *s, size_t n) {
    void *r = d;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return r;
}

void *memmove(void *d, const void *s, size_t n) {
    unsigned char *dp = d;
    const unsigned char *sp = s;
    if (dp == sp || n == 0) return d;
    if (dp < sp || dp >= sp + n) return memcpy(d, s, n);
    while (n--) dp[n] = sp[n];
    return d;
}

void *memset(void *d, int c, size_t n) {
    void *r = d;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    for (size_t i = 0; i < n; i++)
        if (p[i] == (unsigned char)c) return (void *)(p + i);
    return NULL;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
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
