/* Buffered stdio over native handles, and a printf/scanf implementation.
   A document has no shared cursor: its stream keeps its own position and reads
   and writes there. Channels and devices (an app's input, output and log)
   carry the bytes in order. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include "nocturne.h"

struct FILE {
    n_handle h;
    int kind;       /* N_KIND_*; 0 until a standard stream is first used, -1 if its handle is missing */
    bool append;    /* every write goes to the document's end */
    uint64_t pos;   /* documents: the offset after the read-ahead window, or of buf[0] while writing */
    int mode; /* _IONBF / _IOLBF / _IOFBF */
    char *buf;
    int bufsize;
    int wlen;       /* bytes pending write */
    int rpos, rlen; /* read buffer window */
    int ungot;
    bool eof, err, own_buf;
    char *temp_name; /* tmpfile: removed when closed */
    struct FILE *next;
};

static char stdin_buf[1024], stdout_buf[1024];
static FILE f_stdin = {.h = 0, .mode = _IOLBF, .buf = stdin_buf, .bufsize = sizeof stdin_buf, .ungot = -1};
static FILE f_stdout = {.h = 1, .mode = _IOLBF, .buf = stdout_buf, .bufsize = sizeof stdout_buf, .ungot = -1};
static FILE f_stderr = {.h = 2, .mode = _IONBF, .ungot = -1};
FILE *stdin = &f_stdin, *stdout = &f_stdout, *stderr = &f_stderr;
static FILE *open_files;

void __stdio_init(void) {}

static void stream_failed(FILE *f, n_result r) {
    f->err = true;
    errno = (int)(r < 0 ? -r : N_ERR_IO);
}

/* What a standard stream was given at launch. Output documents (a shell's
   "> name") are appended to, so apps sharing one write after each other. */
static int stream_kind(FILE *f) {
    if (!f->kind) {
        n_result k = n_handle_kind(f->h);
        f->kind = k < 0 ? -1 : (int)k;
        if (f->kind == N_KIND_DOCUMENT && f != stdin) {
            f->append = true;
            if (f->mode == _IOLBF) f->mode = _IOFBF; /* nobody reads it line by line */
        }
    }
    return f->kind;
}

/* All n bytes, or an error. */
static int raw_write(FILE *f, const char *s, size_t n) {
    int kind = stream_kind(f);
    n_result r = -N_ERR_NO_HANDLE;
    size_t off = 0;
    if (kind == N_KIND_DOCUMENT) {
        bool claimed = false;
        r = 0;
        if (f->append) {
            /* the claim keeps other appenders from writing at the same end */
            claimed = n_document_claim(f->h, true) == 0;
            r = n_document_size(f->h);
            if (r >= 0) f->pos = (uint64_t)r;
        }
        while (r >= 0 && off < n) {
            r = n_document_write_at(f->h, f->pos, s + off, n - off);
            if (r <= 0) break;
            off += (size_t)r;
            f->pos += (uint64_t)r;
        }
        if (claimed) n_document_release(f->h);
    } else if (kind > 0) {
        while (off < n) {
            r = n_channel_send(f->h, s + off, n - off, -1);
            if (r <= 0) break;
            off += (size_t)r;
        }
    }
    if (off < n) {
        stream_failed(f, r);
        return EOF;
    }
    return 0;
}

/* Bytes read, 0 at the end, EOF on error, or -2 when the next message of a
   channel is larger than cap. */
static int64_t raw_read(FILE *f, char *buf, size_t cap) {
    int kind = stream_kind(f);
    n_result r = -N_ERR_NO_HANDLE;
    if (kind == N_KIND_DOCUMENT) {
        r = n_document_read_at(f->h, f->pos, buf, cap);
        if (r > 0) f->pos += (uint64_t)r;
    } else if (kind > 0) {
        /* an empty message carries no bytes: wait for the next one */
        do r = n_channel_receive(f->h, buf, cap, N_CHANNEL_WAIT, NULL);
        while (r == 0);
        if (r == -N_ERR_CLOSED) r = 0;
        else if (r == -N_ERR_TOO_LARGE) return -2;
    }
    if (r < 0) {
        stream_failed(f, r);
        return EOF;
    }
    return r;
}

int fflush(FILE *f) {
    if (!f) {
        int r = fflush(stdout) | fflush(stderr);
        for (FILE *p = open_files; p; p = p->next) r |= fflush(p);
        return r ? EOF : 0;
    }
    if (!f->wlen) return 0;
    int r = raw_write(f, f->buf, (size_t)f->wlen);
    f->wlen = 0;
    return r;
}

/* At exit, as ISO C closes every stream: everything written goes out and is its document's saved
   content (an app opening the document next reads it; the medium has it moments later), and
   temporary documents go away. */
void __stdio_flush_all(void) {
    fflush(NULL);
    if (stdout->kind == N_KIND_DOCUMENT) n_document_commit(stdout->h, N_COMMIT_START);
    if (stderr->kind == N_KIND_DOCUMENT) n_document_commit(stderr->h, N_COMMIT_START);
    for (FILE *f = open_files; f; f = f->next)
        if (f->temp_name) {
            n_handle_close(f->h);
            f->kind = -1;
            n_entry_remove(f->temp_name);
        } else if (f->kind == N_KIND_DOCUMENT) {
            n_document_commit(f->h, N_COMMIT_START);
        }
}

/* A full path for a name: a name that is not complete is taken inside the launcher's folder. */
static const char *full_path(const char *name, char *out, size_t cap) {
    n_result r = n_path_complete(name, out, cap);
    if (r < 0) {
        errno = (int)-r;
        return NULL;
    }
    return out;
}

static n_handle open_or_create(const char *path) {
    n_handle h = n_document_open(path, N_DOCUMENT_EDIT);
    if (h == -N_ERR_NOT_FOUND) {
        h = n_document_open(path, N_DOCUMENT_CREATE);
        if (h == -N_ERR_EXISTS) h = n_document_open(path, N_DOCUMENT_EDIT); /* made meanwhile */
    }
    return h;
}

/* ISO C modes: r, w, a, each with an optional + and b; w may add x. */
static n_handle open_document(const char *name, const char *m, bool *append) {
    char buf[4096];
    const char *path = full_path(name, buf, sizeof buf);
    if (!path) return -(n_result)errno;
    bool plus = strchr(m, '+') != NULL;
    *append = false;
    switch (m[0]) {
    case 'r': return n_document_open(path, plus ? N_DOCUMENT_EDIT : N_DOCUMENT_READ);
    case 'w': {
        if (strchr(m, 'x')) return n_document_open(path, N_DOCUMENT_CREATE);
        n_handle h = open_or_create(path);
        n_result r = h < 0 ? h : n_document_set_size(h, 0);
        if (r < 0 && h >= 0) n_handle_close(h);
        return r < 0 ? (n_handle)r : h;
    }
    case 'a':
        *append = true;
        return open_or_create(path);
    }
    return -N_ERR_INVALID;
}

static FILE *stream_new(n_handle h, int kind, bool append) {
    FILE *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->h = h;
    f->kind = kind;
    f->append = append;
    f->mode = _IOFBF;
    f->bufsize = 4096;
    f->buf = malloc(f->bufsize);
    f->own_buf = true;
    f->ungot = -1;
    if (!f->buf) {
        free(f);
        return NULL;
    }
    f->next = open_files;
    open_files = f;
    return f;
}

FILE *fdopen(int fd, const char *mode) {
    return stream_new(fd, 0, mode && mode[0] == 'a');
}

FILE *fopen(const char *path, const char *mode) {
    bool append;
    n_handle h = open_document(path, mode, &append);
    if (h < 0) {
        errno = (int)-h;
        return NULL;
    }
    FILE *f = stream_new(h, N_KIND_DOCUMENT, append);
    if (!f) {
        n_handle_close(h);
        errno = N_ERR_NO_MEMORY;
    }
    return f;
}

/* The same FILE on a new document; the old handle is closed. */
FILE *freopen(const char *path, const char *mode, FILE *f) {
    if (!path) {
        errno = N_ERR_INVALID;
        return NULL;
    }
    fflush(f);
    bool append;
    n_handle h = open_document(path, mode, &append);
    if (h < 0) {
        errno = (int)-h;
        return NULL;
    }
    if (f->kind >= 0) n_handle_close(f->h);
    f->h = h;
    f->kind = N_KIND_DOCUMENT;
    f->append = append;
    f->pos = 0;
    f->wlen = f->rpos = f->rlen = 0;
    f->ungot = -1;
    f->eof = f->err = false;
    return f;
}

int fclose(FILE *f) {
    int r = fflush(f);
    n_result closed = n_handle_close(f->h);
    if (closed < 0) {
        errno = (int)-closed;
        r = EOF;
    }
    if (f == stdin || f == stdout || f == stderr) {
        f->kind = -1;
        return r;
    }
    FILE **pp = &open_files;
    while (*pp && *pp != f) pp = &(*pp)->next;
    if (*pp) *pp = f->next;
    if (f->temp_name) {
        n_entry_remove(f->temp_name);
        free(f->temp_name);
    }
    if (f->own_buf) free(f->buf);
    free(f);
    return r;
}

/* A new document in the temporary folder that goes away when it is closed or the app ends. */
FILE *tmpfile(void) {
    static unsigned serial;
    char leaf[40], name[4096];
    for (int tries = 0; tries < 100; tries++) {
        uint64_t mix = n_clock_uptime_ms() * 0x9e3779b97f4a7c15ull + (uint64_t)(uintptr_t)&mix + ++serial;
        snprintf(leaf, sizeof leaf, "temporary-%llx", (unsigned long long)(mix ^ (mix >> 29)));
        n_result joined = n_path_join(n_temp_folder(), leaf, name, sizeof name);
        if (joined < 0) {
            errno = (int)-joined;
            return NULL;
        }
        n_handle h = n_document_open(name, N_DOCUMENT_CREATE);
        if (h == -N_ERR_EXISTS) continue;
        if (h < 0) {
            errno = (int)-h;
            return NULL;
        }
        FILE *f = stream_new(h, N_KIND_DOCUMENT, false);
        char *kept = f ? strdup(name) : NULL;
        if (!kept) {
            if (f) fclose(f);
            else n_handle_close(h);
            n_entry_remove(name);
            errno = N_ERR_NO_MEMORY;
            return NULL;
        }
        f->temp_name = kept;
        return f;
    }
    errno = N_ERR_EXISTS;
    return NULL;
}

int remove(const char *name) {
    char buf[4096];
    const char *path = full_path(name, buf, sizeof buf);
    n_result r = path ? n_entry_remove(path) : -(n_result)errno;
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return 0;
}

/* An existing document under the new name takes the old one's content whole,
   swapped in atomically, and the old name goes. */
int rename(const char *from, const char *to) {
    char a[4096], b[4096];
    const char *src = full_path(from, a, sizeof a), *dst = src ? full_path(to, b, sizeof b) : NULL;
    n_result r = dst ? n_entry_move(src, dst) : -(n_result)errno;
    if (r == -N_ERR_EXISTS) {
        struct n_entry s, d;
        if (n_entry_info(src, &s) == 0 && n_entry_info(dst, &d) == 0 &&
            s.kind == N_KIND_DOCUMENT && d.kind == N_KIND_DOCUMENT) {
            void *data;
            size_t size;
            r = n_document_load(src, &data, &size);
            if (r >= 0) {
                r = n_document_save(dst, data, size);
                if (data) n_memory_release(data, size);
            }
            if (r >= 0) r = n_entry_remove(src);
        }
    }
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return 0;
}

int setvbuf(FILE *f, char *buf, int mode, size_t size) {
    fflush(f);
    f->mode = mode;
    if (buf) {
        if (f->own_buf) free(f->buf);
        f->own_buf = false; /* the caller's buffer is never freed here */
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
        /* switching from reading: give back the read-ahead */
        if (stream_kind(f) == N_KIND_DOCUMENT) f->pos -= (uint64_t)(f->rlen - f->rpos);
        f->rlen = f->rpos = 0;
    }
    if (f->mode == _IONBF || !f->buf) return raw_write(f, s, n);
    for (size_t i = 0; i < n; i++) {
        f->buf[f->wlen++] = s[i];
        if (f->wlen == f->bufsize || (f->mode == _IOLBF && s[i] == '\n')) {
            if (fflush(f)) return EOF;
        }
    }
    return 0;
}

/* A message larger than the buffer: the buffer grows to hold it whole. */
static int grow_for_message(FILE *f) {
    n_result size = n_channel_receive(f->h, NULL, 0, N_CHANNEL_WAIT | N_CHANNEL_PEEK, NULL);
    if (size < 0) {
        stream_failed(f, size);
        return EOF;
    }
    if (size > INT32_MAX) {
        stream_failed(f, -N_ERR_TOO_LARGE);
        return EOF;
    }
    char *bigger = malloc((size_t)size);
    if (!bigger) {
        stream_failed(f, -N_ERR_NO_MEMORY);
        return EOF;
    }
    if (f->own_buf) free(f->buf);
    f->buf = bigger;
    f->bufsize = (int)size;
    f->own_buf = true;
    return 0;
}

static int fill(FILE *f) {
    if (f == stdin) fflush(stdout);
    if (f->wlen && fflush(f)) return EOF;
    if (!f->buf) {
        f->buf = malloc(BUFSIZ);
        if (!f->buf) {
            stream_failed(f, -N_ERR_NO_MEMORY);
            return EOF;
        }
        f->bufsize = BUFSIZ;
        f->own_buf = true;
    }
    int64_t n;
    while ((n = raw_read(f, f->buf, (size_t)f->bufsize)) == -2)
        if (grow_for_message(f)) return EOF;
    if (n <= 0) {
        if (n == 0) f->eof = true;
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
        if (total - got >= 4096 && !f->wlen) {
            int64_t r = raw_read(f, b + got, total - got);
            if (r > 0) {
                got += (size_t)r;
                continue;
            }
            if (r == 0) f->eof = true;
            if (r != -2) break; /* a message larger than the rest goes through the buffer */
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
void clearerr(FILE *f) { f->eof = f->err = false; }
int fileno(FILE *f) { return f->h; }

/* The stream's position: documents only; channels have none. */
long ftell(FILE *f) {
    if (stream_kind(f) != N_KIND_DOCUMENT) {
        errno = N_ERR_WRONG_KIND;
        return -1;
    }
    return (long)f->pos - (f->rlen - f->rpos) + f->wlen - (f->ungot >= 0);
}

int fseek(FILE *f, long off, int whence) {
    if (fflush(f)) return -1;
    if (stream_kind(f) != N_KIND_DOCUMENT) {
        errno = N_ERR_WRONG_KIND;
        return -1;
    }
    long base = 0;
    if (whence == SEEK_CUR) base = ftell(f);
    else if (whence == SEEK_END) {
        n_result size = n_document_size(f->h);
        if (size < 0) {
            errno = (int)-size;
            return -1;
        }
        base = (long)size;
    } else if (whence != SEEK_SET) {
        errno = N_ERR_INVALID;
        return -1;
    }
    if (off < 0 ? base < -off : off > INT64_MAX - base) {
        errno = N_ERR_INVALID;
        return -1;
    }
    f->pos = (uint64_t)(base + off);
    f->rpos = f->rlen = 0;
    f->ungot = -1;
    f->eof = false;
    return 0;
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

/* ---- exact double -> decimal conversion ----
   A double is m * 2^e with integer m, so its decimal expansion is finite. We compute all of its
   digits with base-1e9 big integers (as musl does) and round them half-to-even, which gives the
   same output as glibc: printf("%.0f", 2.5) is "2", and "%.20f" prints the true digits. */
#define DEC_MAX 800 /* 2^-1074 has 751 significant digits; 2^1024 has 309 */

struct dec {
    char d[DEC_MAX]; /* digits, d[0] != '0' unless the value is zero */
    int n;           /* number of digits (0 = zero) */
    int pt;          /* value = 0.d[0]d[1]... * 10^pt */
};

static void dec_from_double(double v, struct dec *x) {
    union { double d; uint64_t u; } bits = {v};
    int be = (int)(bits.u >> 52 & 0x7FF);
    uint64_t m = bits.u & ((1ULL << 52) - 1);
    int e;
    if (be) m |= 1ULL << 52, e = be - 1075;
    else e = -1074;
    x->n = 0;
    x->pt = 0;
    if (!m) return;
    uint32_t big[96]; /* little-endian base 1e9 */
    int nl = 0;
    while (m) big[nl++] = (uint32_t)(m % 1000000000), m /= 1000000000;
    /* multiply by 2^e, or for e < 0 by 5^-e and move the decimal point -e places left */
    int k = e > 0 ? e : -e;
    while (k > 0) {
        int step = e > 0 ? (k < 29 ? k : 29) : (k < 13 ? k : 13);
        uint64_t f = 1;
        for (int i = 0; i < step; i++) f *= e > 0 ? 2 : 5;
        uint64_t carry = 0;
        for (int i = 0; i < nl; i++) {
            uint64_t t = big[i] * f + carry;
            big[i] = (uint32_t)(t % 1000000000);
            carry = t / 1000000000;
        }
        while (carry) big[nl++] = (uint32_t)(carry % 1000000000), carry /= 1000000000;
        k -= step;
    }
    int n = 0;
    char tmp[12];
    int tl = 0;
    for (uint32_t t = big[nl - 1]; t; t /= 10) tmp[tl++] = (char)('0' + t % 10);
    while (tl) x->d[n++] = tmp[--tl];
    for (int i = nl - 2; i >= 0; i--) {
        for (int j = 8; j >= 0; j--) {
            uint32_t t = big[i];
            for (int q = 0; q < j; q++) t /= 10;
            x->d[n++] = (char)('0' + t % 10);
        }
    }
    x->pt = e < 0 ? n + e : n;
    while (n > 0 && x->d[n - 1] == '0') n--;
    x->n = n;
}

/* keep the first `keep` digits (keep may be <= 0 or >= n), rounding half to even */
static void dec_round(struct dec *x, int keep) {
    if (keep >= x->n) return;
    if (keep < 0) {
        x->n = 0;
        return;
    }
    int first = x->d[keep] - '0';
    bool up;
    if (first != 5) up = first > 5;
    else if (keep + 1 < x->n) up = true; /* more nonzero digits follow (trailing zeros are stripped) */
    else up = keep > 0 && (x->d[keep - 1] - '0') % 2; /* exactly half: to even */
    x->n = keep;
    if (up) {
        int i = keep - 1;
        while (i >= 0 && x->d[i] == '9') i--;
        if (i < 0) {
            x->d[0] = '1';
            x->n = 1;
            x->pt++;
        } else {
            x->d[i]++;
            x->n = i + 1;
        }
    }
    while (x->n > 0 && x->d[x->n - 1] == '0') x->n--;
}

static char dec_digit(const struct dec *x, int i) { return i >= 0 && i < x->n ? x->d[i] : '0'; }

/* %f: `prec` decimals; returns length */
static int fmt_fixed(char *buf, struct dec *x, int prec) {
    dec_round(x, x->pt + prec);
    int n = 0;
    if (x->n == 0 || x->pt <= 0) buf[n++] = '0';
    else
        for (int i = 0; i < x->pt; i++) buf[n++] = dec_digit(x, i);
    if (prec) {
        buf[n++] = '.';
        for (int i = 0; i < prec; i++) buf[n++] = x->n ? dec_digit(x, x->pt + i) : '0';
    }
    return n;
}

/* %e: one digit, `prec` decimals, exponent */
static int fmt_exp(char *buf, struct dec *x, int prec, bool upper) {
    dec_round(x, prec + 1);
    int e = x->n ? x->pt - 1 : 0, n = 0;
    buf[n++] = dec_digit(x, 0);
    if (prec) {
        buf[n++] = '.';
        for (int i = 1; i <= prec; i++) buf[n++] = dec_digit(x, i);
    }
    buf[n++] = upper ? 'E' : 'e';
    buf[n++] = e < 0 ? '-' : '+';
    if (e < 0) e = -e;
    if (e < 10) buf[n++] = '0';
    n += fmt_uint(buf + n, (unsigned)e, 10, false);
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
        char buf[720]; /* %f of 1e308 with 340 decimals */
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
            if (prec > 340) prec = 340;
            struct dec x;
            dec_from_double(v, &x);
            if (c == 'g' || c == 'G') {
                if (prec == 0) prec = 1;
                dec_round(&x, prec); /* P significant digits decide the style */
                int e = x.n ? x.pt - 1 : 0;
                if (e < -4 || e >= prec) n = fmt_exp(buf, &x, prec - 1, c == 'G');
                else n = fmt_fixed(buf, &x, prec - 1 - e);
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
                n = fmt_exp(buf, &x, prec, c == 'E');
            } else {
                n = fmt_fixed(buf, &x, prec);
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
        /* format into a temporary buffer so the output is one send */
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
        /* a field width limits how much a number may consume: parse a copy of that many chars */
        char field[64];
        const char *src = p;
        if (width && strchr("diuxfge", *f)) {
            int k = 0;
            while (k < width && k < (int)sizeof field - 1 && p[k]) field[k] = p[k], k++;
            field[k] = 0;
            src = field;
        }
        switch (*f) {
        case 'd':
        case 'i':
        case 'u':
        case 'x': {
            int base = *f == 'x' ? 16 : *f == 'i' ? 0 : 10;
            long long v = (*f == 'u' || *f == 'x') ? (long long)strtoull(src, &end, base) : strtoll(src, &end, base);
            if (end == src) goto done;
            p += end - src;
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
            double v = strtod(src, &end);
            if (end == src) goto done;
            p += end - src;
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
