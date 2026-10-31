/* The kernel's formatter (kernel.h): printf's conversions the kernel uses, into a buffer of a
   given size, returning the length the whole text has. The log (sys/log.c) formats with it in
   two passes, the first measuring (buf NULL, size 0), the second straight into its ring. */
#include "kernel.h"

struct outbuf {
    char *buf;
    size_t size;
    size_t len;
};

static void out_c(struct outbuf *o, char c) {
    if (o->len + 1 < o->size) o->buf[o->len] = c;
    o->len++;
}

static void out_num(struct outbuf *o, uint64_t v, bool neg, int base, bool upper,
                    int width, int prec, bool left, bool zero, char sign, bool alt) {
    char tmp[32];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    if (v == 0 && prec != 0) tmp[n++] = '0';
    while (v) {
        tmp[n++] = digits[v % base];
        v /= base;
    }
    int zeros = prec > n ? prec - n : 0;
    char sch = neg ? '-' : sign;
    const char *pfx = (alt && base == 16) ? (upper ? "0X" : "0x") : "";
    int plen = strlen(pfx);
    int total = n + zeros + (sch ? 1 : 0) + plen;
    if (zero && !left && prec < 0) {
        zeros += width > total ? width - total : 0;
        total = width > total ? width : total;
    }
    if (!left)
        for (int i = total; i < width; i++) out_c(o, ' ');
    if (sch) out_c(o, sch);
    for (int i = 0; i < plen; i++) out_c(o, pfx[i]);
    for (int i = 0; i < zeros; i++) out_c(o, '0');
    while (n) out_c(o, tmp[--n]);
    if (left)
        for (int i = total; i < width; i++) out_c(o, ' ');
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    struct outbuf o = {buf, size, 0};
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            out_c(&o, *fmt);
            continue;
        }
        fmt++;
        bool left = false, zero = false, alt = false;
        char sign = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') zero = true;
            else if (*fmt == '+') sign = '+';
            else if (*fmt == ' ') { if (!sign) sign = ' '; }
            else if (*fmt == '#') alt = true;
            else break;
        }
        int width = 0, prec = -1;
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; if (width < 0) { left = true; width = -width; } }
        else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }
        int lng = 0;
        while (*fmt == 'l' || *fmt == 'z' || *fmt == 'h') {
            if (*fmt != 'h') lng++;
            fmt++;
        }
        switch (*fmt) {
        case 'd': case 'i': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            out_num(&o, v < 0 ? -(uint64_t)v : (uint64_t)v, v < 0, 10, false, width, prec, left, zero, sign, false);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            uint64_t v = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned int);
            int base = *fmt == 'u' ? 10 : (*fmt == 'o' ? 8 : 16);
            out_num(&o, v, false, base, *fmt == 'X', width, prec, left, zero, 0, alt);
            break;
        }
        case 'p': {
            uint64_t v = (uint64_t)va_arg(ap, void *);
            out_num(&o, v, false, 16, false, 16, -1, false, true, 0, false);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            if (!left) for (int i = 1; i < width; i++) out_c(&o, ' ');
            out_c(&o, c);
            if (left) for (int i = 1; i < width; i++) out_c(&o, ' ');
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = prec >= 0 ? (int)strnlen(s, prec) : (int)strlen(s);
            if (!left) for (int i = len; i < width; i++) out_c(&o, ' ');
            for (int i = 0; i < len; i++) out_c(&o, s[i]);
            if (left) for (int i = len; i < width; i++) out_c(&o, ' ');
            break;
        }
        case '%': out_c(&o, '%'); break;
        case 0: fmt--; break;
        default: out_c(&o, '%'); out_c(&o, *fmt); break;
        }
    }
    if (size) buf[o.len < size ? o.len : size - 1] = 0;
    return (int)o.len;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}
