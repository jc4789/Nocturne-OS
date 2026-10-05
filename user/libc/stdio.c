/* Buffered stdio and a printf/scanf implementation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include "nocturne.h"

struct FILE {
    int fd;
    int mode; /* _IONBF / _IOLBF / _IOFBF */
    char *buf;
    int bufsize;
    int wlen;       /* bytes pending write */
    int rpos, rlen; /* read buffer window */
    int ungot;
    bool eof, err, own_buf;
    struct FILE *next;
};

static char stdin_buf[1024], stdout_buf[1024];
static FILE f_stdin = {0, _IOLBF, stdin_buf, sizeof stdin_buf, 0, 0, 0, -1, false, false, false, NULL};
static FILE f_stdout = {1, _IOLBF, stdout_buf, sizeof stdout_buf, 0, 0, 0, -1, false, false, false, NULL};
static FILE f_stderr = {2, _IONBF, NULL, 0, 0, 0, 0, -1, false, false, false, NULL};
FILE *stdin = &f_stdin, *stdout = &f_stdout, *stderr = &f_stderr;
static FILE *open_files;

void __stdio_init(void) {}

int fflush(FILE *f) {
    if (!f) {
        fflush(stdout);
        fflush(stderr);
        for (FILE *p = open_files; p; p = p->next) fflush(p);
        return 0;
    }
    int off = 0;
    while (off < f->wlen) {
        ssize_t n = write(f->fd, f->buf + off, f->wlen - off);
        if (n <= 0) {
            f->err = true;
            f->wlen = 0;
            return EOF;
        }
        off += n;
    }
    f->wlen = 0;
    return 0;
}

void __stdio_flush_all(void) { fflush(NULL); }

static int parse_mode(const char *m) {
    int fl = 0;
    bool plus = strchr(m, '+') != NULL;
    switch (m[0]) {
    case 'r': fl = plus ? O_RDWR : O_RDONLY; break;
    case 'w': fl = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; break;
    case 'a': fl = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND; break;
    default: return -1;
    }
    return fl;
}

FILE *fdopen(int fd, const char *mode) {
    FILE *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->fd = fd;
    f->mode = _IOFBF;
    f->bufsize = 4096;
    f->buf = malloc(f->bufsize);
    f->own_buf = true;
    f->ungot = -1;
    f->next = open_files;
    open_files = f;
    return f;
}

FILE *fopen(const char *path, const char *mode) {
    int fl = parse_mode(mode);
    if (fl < 0) {
        errno = EINVAL;
        return NULL;
    }
    int fd = open(path, fl);
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, mode);
    if (!f) close(fd);
    return f;
}

int fclose(FILE *f) {
    fflush(f);
    int r = close(f->fd);
    if (f == stdin || f == stdout || f == stderr) return r;
    FILE **pp = &open_files;
    while (*pp && *pp != f) pp = &(*pp)->next;
    if (*pp) *pp = f->next;
    if (f->own_buf) free(f->buf);
    free(f);
    return r;
}

int setvbuf(FILE *f, char *buf, int mode, size_t size) {
    fflush(f);
    f->mode = mode;
    if (buf) {
        f->buf = buf;
        f->bufsize = (int)size;
    }
    if (mode != _IONBF && !f->buf) {
        f->buf = malloc(BUFSIZ);
        f->bufsize = BUFSIZ;
        f->own_buf = true;
    }
    return 0;
}

static int write_bytes(FILE *f, const char *s, size_t n) {
    if (f->rlen) {
        /* switching from reading: discard read-ahead */
        lseek(f->fd, -(long)(f->rlen - f->rpos), SEEK_CUR);
        f->rlen = f->rpos = 0;
    }
    if (f->mode == _IONBF || !f->buf) {
        size_t off = 0;
        while (off < n) {
            ssize_t w = write(f->fd, s + off, n - off);
            if (w <= 0) {
                f->err = true;
                return EOF;
            }
            off += w;
        }
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        f->buf[f->wlen++] = s[i];
        if (f->wlen == f->bufsize || (f->mode == _IOLBF && s[i] == '\n')) {
            if (fflush(f)) return EOF;
        }
    }
    return 0;
}

static int fill(FILE *f) {
    if (f == stdin) fflush(stdout);
    if (f->wlen) fflush(f);
    if (!f->buf) {
        f->buf = malloc(BUFSIZ);
        f->bufsize = BUFSIZ;
        f->own_buf = true;
    }
    ssize_t n = read(f->fd, f->buf, f->bufsize);
    if (n <= 0) {
        if (n < 0) f->err = true;
        else f->eof = true;
        return EOF;
    }
    f->rpos = 0;
    f->rlen = (int)n;
    return 0;
}

int fgetc(FILE *f) {
    if (f->ungot >= 0) {
        int c = f->ungot;
        f->ungot = -1;
        return c;
    }
    if (f->rpos >= f->rlen && fill(f) == EOF) return EOF;
    return (unsigned char)f->buf[f->rpos++];
}

int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }

int ungetc(int c, FILE *f) {
    if (c == EOF) return EOF;
    f->ungot = c;
    f->eof = false;
    return c;
}

char *fgets(char *s, int n, FILE *f) {
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;
    s[i] = 0;
    return s;
}

size_t fread(void *buf, size_t size, size_t n, FILE *f) {
    size_t total = size * n, got = 0;
    char *b = buf;
    if (total && f->ungot >= 0) {
        b[got++] = (char)f->ungot;
        f->ungot = -1;
    }
    while (got < total) {
        if (f->rpos < f->rlen) {
            size_t c = (size_t)(f->rlen - f->rpos);
            if (c > total - got) c = total - got;
            memcpy(b + got, f->buf + f->rpos, c);
            f->rpos += (int)c;
            got += c;
            continue;
        }
        if (total - got >= 4096) {
            ssize_t r = read(f->fd, b + got, total - got);
            if (r <= 0) {
                if (r < 0) f->err = true;
                else f->eof = true;
                break;
            }
            got += r;
            continue;
        }
        if (fill(f) == EOF) break;
    }
    return size ? got / size : 0;
}

size_t fwrite(const void *buf, size_t size, size_t n, FILE *f) {
    if (write_bytes(f, buf, size * n)) return 0;
    return n;
}

int fputc(int c, FILE *f) {
    char ch = (char)c;
    return write_bytes(f, &ch, 1) ? EOF : (unsigned char)c;
}
int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }
int fputs(const char *s, FILE *f) { return write_bytes(f, s, strlen(s)) ? EOF : 0; }
int puts(const char *s) {
    if (fputs(s, stdout)) return EOF;
    return fputc('\n', stdout) == EOF ? EOF : 0;
}
int feof(FILE *f) { return f->eof; }
int ferror(FILE *f) { return f->err; }
int fileno(FILE *f) { return f->fd; }

int fseek(FILE *f, long off, int whence) {
    fflush(f);
    if (whence == SEEK_CUR) off -= (f->rlen - f->rpos);
    f->rpos = f->rlen = 0;
    f->ungot = -1;
    f->eof = false;
    return lseek(f->fd, off, whence) < 0 ? -1 : 0;
}

long ftell(FILE *f) {
    long p = lseek(f->fd, 0, SEEK_CUR);
    if (p < 0) return -1;
    return p - (f->rlen - f->rpos) + f->wlen;
}

void rewind(FILE *f) { fseek(f, 0, SEEK_SET); }

void perror(const char *msg) {
    if (msg && *msg) fprintf(stderr, "%s: %s\n", msg, strerror(errno));
    else fprintf(stderr, "%s\n", strerror(errno));
}

/* ---- formatting core ---- */
typedef struct {
    void (*put)(void *ctx, const char *s, size_t n);
    void *ctx;
    size_t count;
} out_t;

static void emit(out_t *o, const char *s, size_t n) {
    o->put(o->ctx, s, n);
    o->count += n;
}

static void pad(out_t *o, char c, int n) {
    char buf[32];
    memset(buf, c, sizeof buf);
    while (n > 0) {
        int k = n > 32 ? 32 : n;
        emit(o, buf, k);
        n -= k;
    }
}

static void emit_field(out_t *o, const char *prefix, const char *body, int blen, int width, bool left, bool zero) {
    int plen = (int)strlen(prefix);
    int total = plen + blen;
    if (!left && !zero) pad(o, ' ', width - total);
    emit(o, prefix, plen);
    if (!left && zero) pad(o, '0', width - total);
    emit(o, body, blen);
    if (left) pad(o, ' ', width - total);
}

static int fmt_uint(char *buf, unsigned long long v, int base, bool upper) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[66];
    int n = 0;
    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v);
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    return n;
}

/* fixed-point formatting of a double with `prec` decimals; returns length */
static int fmt_fixed(char *buf, int cap, double v, int prec) {
    int n = 0;
    if (prec > 17) prec = 17;
    double scale = 1;
    for (int i = 0; i < prec; i++) scale *= 10;
    if (v >= 1e18) {
        /* huge: print digits with exponent-free approximation */
        int e = 0;
        while (v >= 1e18) {
            v /= 10;
            e++;
        }
        unsigned long long ip = (unsigned long long)v;
        n = fmt_uint(buf, ip, 10, false);
        while (e-- && n < cap - 1) buf[n++] = '0';
        if (prec) {
            buf[n++] = '.';
            for (int i = 0; i < prec && n < cap - 1; i++) buf[n++] = '0';
        }
        return n;
    }
    unsigned long long ip = (unsigned long long)v;
    double frac = v - (double)ip;
    unsigned long long fp = (unsigned long long)(frac * scale + 0.5);
    if (prec == 0 && frac >= 0.5) {
        ip++;
        fp = 0;
    }
    if ((double)fp >= scale && prec > 0) {
        ip++;
        fp -= (unsigned long long)scale;
    }
    n = fmt_uint(buf, ip, 10, false);
    if (prec) {
        buf[n++] = '.';
        char tmp[24];
        int fl = fmt_uint(tmp, fp, 10, false);
        for (int i = fl; i < prec; i++) buf[n++] = '0';
        memcpy(buf + n, tmp, fl);
        n += fl;
    }
    return n;
}

static int fmt_exp(char *buf, int cap, double v, int prec, bool upper) {
    int e = 0;
    if (v != 0) {
        while (v >= 10) {
            v /= 10;
            e++;
        }
        while (v < 1) {
            v *= 10;
            e--;
        }
    }
    /* rounding may push us to 10.0 */
    double r = 0.5;
    for (int i = 0; i < prec; i++) r /= 10;
    if (v + r >= 10) {
        v /= 10;
        e++;
    }
    int n = fmt_fixed(buf, cap, v, prec);
    buf[n++] = upper ? 'E' : 'e';
    buf[n++] = e < 0 ? '-' : '+';
    if (e < 0) e = -e;
    if (e < 10) buf[n++] = '0';
    n += fmt_uint(buf + n, e, 10, false);
    return n;
}

static int core(out_t *o, const char *fmt, va_list ap) {
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            const char *s = p;
            while (p[1] && p[1] != '%') p++;
            emit(o, s, p - s + 1);
            continue;
        }
        p++;
        bool left = false, plus = false, space = false, zero = false, alt = false;
        for (;; p++) {
            if (*p == '-') left = true;
            else if (*p == '+') plus = true;
            else if (*p == ' ') space = true;
            else if (*p == '0') zero = true;
            else if (*p == '#') alt = true;
            else break;
        }
        int width = 0, prec = -1;
        if (*p == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                left = true;
                width = -width;
            }
            p++;
        } else {
            while (isdigit((unsigned char)*p)) width = width * 10 + (*p++ - '0');
        }
        if (*p == '.') {
            p++;
            prec = 0;
            if (*p == '*') {
                prec = va_arg(ap, int);
                p++;
            } else {
                while (isdigit((unsigned char)*p)) prec = prec * 10 + (*p++ - '0');
            }
        }
        int lng = 0;
        for (;; p++) {
            if (*p == 'l') lng++;
            else if (*p == 'z' || *p == 'j' || *p == 't') lng = 2;
            else if (*p == 'h') {}
            else break;
        }
        char buf[352];
        char prefix[4] = {0};
        int n = 0;
        switch (*p) {
        case 'd':
        case 'i': {
            long long v = lng ? va_arg(ap, long long) : va_arg(ap, int);
            unsigned long long u = v < 0 ? -(unsigned long long)v : (unsigned long long)v;
            if (v < 0) strcpy(prefix, "-");
            else if (plus) strcpy(prefix, "+");
            else if (space) strcpy(prefix, " ");
            n = fmt_uint(buf, u, 10, false);
            if (prec >= 0) {
                zero = false;
                if (n < prec) {
                    memmove(buf + (prec - n), buf, n);
                    memset(buf, '0', prec - n);
                    n = prec;
                }
                if (prec == 0 && u == 0) n = 0;
            }
            emit_field(o, prefix, buf, n, width, left, zero);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            unsigned long long v = lng ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned int);
            int base = *p == 'u' ? 10 : *p == 'o' ? 8 : 16;
            n = fmt_uint(buf, v, base, *p == 'X');
            if (alt && v) {
                if (base == 16) strcpy(prefix, *p == 'X' ? "0X" : "0x");
                else if (base == 8) strcpy(prefix, "0");
            }
            if (prec >= 0) {
                zero = false;
                if (n < prec) {
                    memmove(buf + (prec - n), buf, n);
                    memset(buf, '0', prec - n);
                    n = prec;
                }
            }
            emit_field(o, prefix, buf, n, width, left, zero);
            break;
        }
        case 'p': {
            uintptr_t v = (uintptr_t)va_arg(ap, void *);
            n = fmt_uint(buf, v, 16, false);
            emit_field(o, "0x", buf, n, width, left, false);
            break;
        }
        case 'c': {
            buf[0] = (char)va_arg(ap, int);
            emit_field(o, "", buf, 1, width, left, false);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int l = prec >= 0 ? (int)strnlen(s, prec) : (int)strlen(s);
            emit_field(o, "", s, l, width, left, false);
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G': {
            double v = va_arg(ap, double);
            if (prec < 0) prec = 6;
            if (v < 0 || (v == 0 && 1 / v < 0)) {
                strcpy(prefix, "-");
                v = -v;
            } else if (plus) strcpy(prefix, "+");
            else if (space) strcpy(prefix, " ");
            if (__builtin_isnan(v)) {
                emit_field(o, prefix, "nan", 3, width, left, false);
                break;
            }
            if (__builtin_isinf(v)) {
                emit_field(o, prefix, "inf", 3, width, left, false);
                break;
            }
            char c = *p;
            if (c == 'g' || c == 'G') {
                if (prec == 0) prec = 1;
                int e = 0;
                double t = v;
                if (t != 0) {
                    while (t >= 10) {
                        t /= 10;
                        e++;
                    }
                    while (t < 1) {
                        t *= 10;
                        e--;
                    }
                }
                if (e < -4 || e >= prec) {
                    n = fmt_exp(buf, sizeof buf, v, prec - 1, c == 'G');
                } else {
                    n = fmt_fixed(buf, sizeof buf, v, prec - 1 - e);
                }
                if (!alt) {
                    /* strip trailing zeros in the fraction */
                    char *dot = memchr(buf, '.', n);
                    if (dot) {
                        char *epos = memchr(buf, c == 'G' ? 'E' : 'e', n);
                        int end = epos ? (int)(epos - buf) : n;
                        int k = end;
                        while (k > 0 && buf[k - 1] == '0') k--;
                        if (k > 0 && buf[k - 1] == '.') k--;
                        memmove(buf + k, buf + end, n - end);
                        n -= end - k;
                    }
                }
            } else if (c == 'e' || c == 'E') {
                n = fmt_exp(buf, sizeof buf, v, prec, c == 'E');
            } else {
                n = fmt_fixed(buf, sizeof buf, v, prec);
            }
            emit_field(o, prefix, buf, n, width, left, zero);
            break;
        }
        case '%': emit(o, "%", 1); break;
        case 'n': *va_arg(ap, int *) = (int)o->count; break;
        default:
            emit(o, "%", 1);
            if (*p) emit(o, p, 1);
            else p--;
            break;
        }
    }
    return (int)o->count;
}

struct sbuf {
    char *buf;
    size_t cap, len;
};

static void put_sbuf(void *ctx, const char *s, size_t n) {
    struct sbuf *b = ctx;
    for (size_t i = 0; i < n; i++) {
        if (b->len + 1 < b->cap) b->buf[b->len] = s[i];
        b->len++;
    }
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    struct sbuf b = {buf, n, 0};
    out_t o = {put_sbuf, &b, 0};
    core(&o, fmt, ap);
    if (n) buf[b.len < n ? b.len : n - 1] = 0;
    return (int)b.len;
}

int vsprintf(char *buf, const char *fmt, va_list ap) { return vsnprintf(buf, (size_t)-1 >> 1, fmt, ap); }

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    return r;
}

static void put_file(void *ctx, const char *s, size_t n) { write_bytes(ctx, s, n); }

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    out_t o = {put_file, f, 0};
    if (f->mode == _IONBF) {
        /* format into a temporary buffer so the output is one write() */
        char tmp[512];
        va_list ap2;
        va_copy(ap2, ap);
        int n = vsnprintf(tmp, sizeof tmp, fmt, ap2);
        va_end(ap2);
        if (n < (int)sizeof tmp) {
            write_bytes(f, tmp, n);
            return n;
        }
    }
    return core(&o, fmt, ap);
}

int vprintf(const char *fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

/* ---- sscanf (subset: %d %i %u %x %ld %lu %f %lf %s %c %[...] not supported) ---- */
int sscanf(const char *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int assigned = 0;
    const char *p = s;
    for (const char *f = fmt; *f; f++) {
        if (isspace((unsigned char)*f)) {
            while (isspace((unsigned char)*p)) p++;
            continue;
        }
        if (*f != '%') {
            if (*p != *f) break;
            p++;
            continue;
        }
        f++;
        bool skip = false;
        if (*f == '*') {
            skip = true;
            f++;
        }
        int width = 0;
        while (isdigit((unsigned char)*f)) width = width * 10 + (*f++ - '0');
        int lng = 0;
        while (*f == 'l' || *f == 'h' || *f == 'z') {
            if (*f == 'l' || *f == 'z') lng++;
            f++;
        }
        if (*f != 'c') while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        char *end;
        switch (*f) {
        case 'd':
        case 'i':
        case 'u':
        case 'x': {
            int base = *f == 'x' ? 16 : *f == 'i' ? 0 : 10;
            long long v = (*f == 'u' || *f == 'x') ? (long long)strtoull(p, &end, base) : strtoll(p, &end, base);
            if (end == p) goto done;
            p = end;
            if (!skip) {
                if (lng) *va_arg(ap, long *) = (long)v;
                else *va_arg(ap, int *) = (int)v;
                assigned++;
            }
            break;
        }
        case 'f':
        case 'g':
        case 'e': {
            double v = strtod(p, &end);
            if (end == p) goto done;
            p = end;
            if (!skip) {
                if (lng) *va_arg(ap, double *) = v;
                else *va_arg(ap, float *) = (float)v;
                assigned++;
            }
            break;
        }
        case 's': {
            char *out = skip ? NULL : va_arg(ap, char *);
            int n = 0;
            while (*p && !isspace((unsigned char)*p) && (!width || n < width)) {
                if (out) out[n] = *p;
                n++;
                p++;
            }
            if (out) {
                out[n] = 0;
                assigned++;
            }
            break;
        }
        case 'c': {
            char *out = skip ? NULL : va_arg(ap, char *);
            if (out) {
                *out = *p;
                assigned++;
            }
            p++;
            break;
        }
        default: goto done;
        }
    }
done:
    va_end(ap);
    return assigned;
}
