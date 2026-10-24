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
#include <limits.h>
#include <wchar.h>
#include <wctype.h>
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
    int ungot;      /* a pushed-back byte, or wide character on a wide stream; -1 for none */
    signed char orientation; /* 0 until a character function first runs; -1 bytes, 1 wide (fwide) */
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
    f->orientation = 0;
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

/* A document under the new name is replaced in the commit of the move (n_entry_replace). */
int rename(const char *from, const char *to) {
    char a[4096], b[4096];
    const char *src = full_path(from, a, sizeof a), *dst = src ? full_path(to, b, sizeof b) : NULL;
    n_result r = dst ? n_entry_replace(src, dst) : -(n_result)errno;
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

void setbuf(FILE *f, char *buf) {
    setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
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

/* A stream's first character function fixes its orientation. The byte and
   wide functions both work on either: a wide stream's characters are UTF-8. */
static void orient(FILE *f, int mode) {
    if (!f->orientation) f->orientation = (signed char)mode;
}

int fgetc(FILE *f) {
    orient(f, -1);
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
    orient(f, -1);
    f->ungot = (unsigned char)c;
    f->eof = false;
    return f->ungot;
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
    orient(f, -1);
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
    orient(f, -1);
    if (write_bytes(f, buf, size * n)) return 0;
    return n;
}

int fputc(int c, FILE *f) {
    char ch = (char)c;
    orient(f, -1);
    return write_bytes(f, &ch, 1) ? EOF : (unsigned char)c;
}
int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }
int fputs(const char *s, FILE *f) {
    orient(f, -1);
    return write_bytes(f, s, strlen(s)) ? EOF : 0;
}
int puts(const char *s) {
    if (fputs(s, stdout)) return EOF;
    return fputc('\n', stdout) == EOF ? EOF : 0;
}
int feof(FILE *f) { return f->eof; }
int ferror(FILE *f) { return f->err; }
void clearerr(FILE *f) { f->eof = f->err = false; }
int fileno(FILE *f) { return f->h; }

/* ---- wide characters: UTF-8 in the stream, one UTF-32 code unit each ---- */

int fwide(FILE *f, int mode) {
    if (mode) orient(f, mode > 0 ? 1 : -1);
    return f->orientation;
}

/* The next byte, left in the buffer. */
static int peek_byte(FILE *f) {
    if (f->rpos >= f->rlen && fill(f) == EOF) return EOF;
    return (unsigned char)f->buf[f->rpos];
}

/* A byte that cannot continue the character is left for the next read; the
   bad character and an end inside one are encoding errors (EILSEQ), which set
   the stream's error indicator. */
wint_t fgetwc(FILE *f) {
    orient(f, 1);
    if (f->ungot >= 0) {
        wint_t c = f->ungot;
        f->ungot = -1;
        return c;
    }
    mbstate_t st = {0};
    for (bool first = true;; first = false) {
        int b = peek_byte(f);
        if (b == EOF) {
            if (first || f->err) return WEOF;
            errno = EILSEQ;
            f->err = true;
            return WEOF;
        }
        char unit = (char)b;
        wchar_t c;
        size_t r = mbrtowc(&c, &unit, 1, &st);
        if (r != (size_t)-1 || first) f->rpos++;
        if (r == (size_t)-1) {
            f->err = true;
            return WEOF;
        }
        if (r != (size_t)-2) return (wint_t)c;
    }
}
wint_t getwc(FILE *f) { return fgetwc(f); }
wint_t getwchar(void) { return fgetwc(stdin); }

wint_t ungetwc(wint_t c, FILE *f) {
    if (c < 0) return WEOF;
    orient(f, 1);
    f->ungot = (int)c;
    f->eof = false;
    return c;
}

wchar_t *fgetws(wchar_t *s, int n, FILE *f) {
    int i = 0;
    while (i < n - 1) {
        wint_t c = fgetwc(f);
        if (c == WEOF) {
            if (ferror(f)) return NULL;
            break;
        }
        s[i++] = (wchar_t)c;
        if (c == L'\n') break;
    }
    if (i == 0 || n <= 0) return NULL;
    s[i] = 0;
    return s;
}

wint_t fputwc(wchar_t c, FILE *f) {
    char units[4];
    mbstate_t st = {0};
    orient(f, 1);
    size_t n = wcrtomb(units, c, &st);
    if (n == (size_t)-1) {
        f->err = true;
        return WEOF;
    }
    return write_bytes(f, units, n) ? WEOF : (wint_t)c;
}
wint_t putwc(wchar_t c, FILE *f) { return fputwc(c, f); }
wint_t putwchar(wchar_t c) { return fputwc(c, stdout); }

int fputws(const wchar_t *s, FILE *f) {
    orient(f, 1);
    while (*s) {
        char units[256];
        mbstate_t st = {0};
        size_t n = wcsrtombs(units, &s, sizeof units, &st);
        if (n == (size_t)-1) {
            f->err = true;
            return EOF;
        }
        if (write_bytes(f, units, n)) return EOF;
        if (!s) break;
    }
    return 0;
}

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

/* ---- formatting core ----
   The output is UTF-8 for the byte and the wide functions alike; for the wide
   ones (wide) it is counted, padded and cut in characters. */
typedef struct {
    bool (*put)(void *ctx, const char *s, size_t n); /* false when it could not be written */
    void *ctx;
    size_t count;
    bool wide, failed;
} out_t;

static void emit(out_t *o, const char *s, size_t n) {
    if (!o->put(o->ctx, s, n)) o->failed = true;
    if (!o->wide) o->count += n;
    else
        for (size_t i = 0; i < n; i++) o->count += ((unsigned char)s[i] & 0xC0) != 0x80;
}

static void encoding_error(out_t *o) {
    o->failed = true;
    errno = EILSEQ;
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

/* ---- exact binary -> decimal conversion ----
   A double or long double is m * 2^e with integer m, so its decimal expansion
   is finite. All of its digits come from base-1e9 big integers and are rounded
   half to even: printf("%.0f", 2.5) is "2", and "%.20f" prints the true digits. */
#define DEC_MAX 800     /* double: 2^-1074 has 751 significant digits; 2^1024 has 309 */
#define BIG_MAX 96
#define LDEC_MAX 11520  /* long double: m * 2^-16445 has up to 11515; 2^16384 has 4933 */
#define LBIG_MAX 1290

struct dec {
    char *d; /* digits, d[0] != '0' unless the value is zero */
    int n;   /* number of digits (0 = zero) */
    int pt;  /* value = 0.d[0]d[1]... * 10^pt */
};

/* The digits of m * 2^e into x->d, with big as room for them in base 1e9. */
static void dec_from_binary(uint64_t m, int e, struct dec *x, uint32_t *big) {
    x->n = 0;
    x->pt = 0;
    if (!m) return;
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

/* %f: `prec` decimals (and the point without them for '#'); returns length */
static int fmt_fixed(char *buf, struct dec *x, int prec, bool alt) {
    dec_round(x, x->pt + prec);
    int n = 0;
    if (x->n == 0 || x->pt <= 0) buf[n++] = '0';
    else
        for (int i = 0; i < x->pt; i++) buf[n++] = dec_digit(x, i);
    if (prec || alt) buf[n++] = '.';
    for (int i = 0; i < prec; i++) buf[n++] = x->n ? dec_digit(x, x->pt + i) : '0';
    return n;
}

/* %e: one digit, `prec` decimals, exponent */
static int fmt_exp(char *buf, struct dec *x, int prec, bool upper, bool alt) {
    dec_round(x, prec + 1);
    int e = x->n ? x->pt - 1 : 0, n = 0;
    buf[n++] = dec_digit(x, 0);
    if (prec || alt) buf[n++] = '.';
    for (int i = 1; i <= prec; i++) buf[n++] = dec_digit(x, i);
    buf[n++] = upper ? 'E' : 'e';
    buf[n++] = e < 0 ? '-' : '+';
    if (e < 0) e = -e;
    if (e < 10) buf[n++] = '0';
    n += fmt_uint(buf + n, (unsigned)e, 10, false);
    return n;
}

/* %a after its 0x: m * 2^e as 1.hhh...p+d (0p+0 for zero), with `prec` hex
   digits rounded half to even, or as many as m needs when prec < 0. */
static int fmt_hex(char *buf, uint64_t m, int e, int prec, bool upper, bool alt) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    uint64_t frac = 0; /* the bits after the leading 1, from bit 63 down */
    int lead = 0, n = 0;
    if (m) {
        int top = 63 - __builtin_clzll(m);
        frac = top ? m << (64 - top) : 0;
        lead = 1, e += top;
        if (prec >= 0 && prec < 16) {
            uint64_t half = (uint64_t)1 << 63;
            uint64_t kept = prec ? frac >> (64 - 4 * prec) : 0, rest = prec ? frac << (4 * prec) : frac;
            /* a carry into the leading digit makes it 2: 1.0 times the next power */
            if (rest > half || (rest == half && ((prec ? kept : 1) & 1)))
                if (!prec || ++kept >> (4 * prec)) kept = 0, e++;
            frac = prec ? kept << (64 - 4 * prec) : 0;
        }
    } else e = 0;
    buf[n++] = digits[lead];
    int count = prec;
    if (count < 0)
        for (count = 0; count < 16 && frac << (4 * count); count++) {}
    if (count || alt) buf[n++] = '.';
    for (int i = 0; i < count; i++) buf[n++] = i < 16 ? digits[frac >> (60 - 4 * i) & 15] : '0';
    buf[n++] = upper ? 'P' : 'p';
    buf[n++] = e < 0 ? '-' : '+';
    n += fmt_uint(buf + n, (unsigned)(e < 0 ? -e : e), 10, false);
    return n;
}

/* The letter, flags, width and precision of a %f %e %g %a conversion. */
struct conv {
    char c;
    bool left, plus, space, zero, alt;
    int width, prec;
};

/* A double or long double: m * 2^e, or the special value "inf" or "nan".
   x->d, big and buf have room for the type's longest expansion. */
static void put_float(out_t *o, const struct conv *cv, bool neg, const char *special, uint64_t m, int e,
                      struct dec *x, uint32_t *big, char *buf) {
    char c = cv->c, prefix[4] = {0};
    bool upper = c == 'F' || c == 'E' || c == 'G' || c == 'A';
    if (neg) strcpy(prefix, "-");
    else if (cv->plus) strcpy(prefix, "+");
    else if (cv->space) strcpy(prefix, " ");
    if (special) {
        emit_field(o, prefix, upper ? (*special == 'i' ? "INF" : "NAN") : special, 3, cv->width, cv->left, false);
        return;
    }
    int prec = cv->prec > 340 ? 340 : cv->prec, n;
    if (c == 'a' || c == 'A') {
        strcat(prefix, upper ? "0X" : "0x");
        n = fmt_hex(buf, m, e, prec, upper, cv->alt);
    } else {
        if (prec < 0) prec = 6;
        dec_from_binary(m, e, x, big);
        if (c == 'g' || c == 'G') {
            if (prec == 0) prec = 1;
            dec_round(x, prec); /* P significant digits decide the style */
            int ex = x->n ? x->pt - 1 : 0;
            if (ex < -4 || ex >= prec) n = fmt_exp(buf, x, prec - 1, upper, cv->alt);
            else n = fmt_fixed(buf, x, prec - 1 - ex, cv->alt);
            if (!cv->alt) {
                /* strip trailing zeros in the fraction */
                char *dot = memchr(buf, '.', n);
                if (dot) {
                    char *epos = memchr(buf, upper ? 'E' : 'e', n);
                    int end = epos ? (int)(epos - buf) : n;
                    int k = end;
                    while (k > 0 && buf[k - 1] == '0') k--;
                    if (k > 0 && buf[k - 1] == '.') k--;
                    memmove(buf + k, buf + end, n - end);
                    n -= end - k;
                }
            }
        } else if (c == 'e' || c == 'E') {
            n = fmt_exp(buf, x, prec, upper, cv->alt);
        } else {
            n = fmt_fixed(buf, x, prec, cv->alt);
        }
    }
    emit_field(o, prefix, buf, n, cv->width, cv->left, cv->zero);
}

/* Each type keeps its large buffers out of core's frame. */
static __attribute__((noinline)) void put_double(out_t *o, const struct conv *cv, double v) {
    char digits[DEC_MAX], buf[720]; /* %f of 1e308 with 340 decimals */
    uint32_t big[BIG_MAX];
    struct dec x = {.d = digits};
    union { double d; uint64_t u; } bits = {v};
    int be = (int)(bits.u >> 52 & 0x7FF);
    uint64_t m = bits.u & ((1ULL << 52) - 1);
    const char *special = be == 0x7FF ? (m ? "nan" : "inf") : NULL;
    if (be) m |= 1ULL << 52;
    put_float(o, cv, bits.u >> 63, special, m, (be ? be : 1) - 1075, &x, big, buf);
}

/* long double is x87 extended precision: a 64-bit significand that holds its
   integer bit, then the sign and a 15-bit exponent. */
static __attribute__((noinline)) void put_long_double(out_t *o, const struct conv *cv, long double v) {
    char digits[LDEC_MAX], buf[5400]; /* %Lf of 1e4932 with 340 decimals */
    uint32_t big[LBIG_MAX];
    struct dec x = {.d = digits};
    union { long double d; struct { uint64_t m; uint16_t se; } p; } bits = {v};
    int be = bits.p.se & 0x7FFF;
    uint64_t m = bits.p.m;
    const char *special = be == 0x7FFF ? (m << 1 ? "nan" : "inf") : NULL;
    put_float(o, cv, bits.p.se >> 15, special, m, (be ? be : 1) - 16446, &x, big, buf);
}

/* The next character of a multibyte or a wide string, as UTF-8 in u: its
   length, 0 at the string's end, or -1 for an encoding error. */
static int next_char(const char **mb, const wchar_t **w, char *u) {
    mbstate_t st = {0};
    size_t r;
    if (*w) {
        if (!**w) return 0;
        r = wcrtomb(u, *(*w)++, &st);
        return r == (size_t)-1 ? -1 : (int)r;
    }
    wchar_t c;
    r = mbrtowc(&c, *mb, MB_CUR_MAX, &st);
    if (!r) return 0;
    if (r > MB_CUR_MAX) return -1;
    memcpy(u, *mb, r);
    *mb += r;
    return (int)r;
}

/* %s's text: a multibyte string, or with l a wide one. Its width and
   precision count the output's units, and the precision never splits a
   character; only as much of the string is read as is written. */
static void put_text(out_t *o, const char *mb, const wchar_t *w, int prec, int width, bool left) {
    if (!o->wide && mb) { /* bytes as they are */
        int len = prec >= 0 ? (int)strnlen(mb, prec) : (int)strlen(mb);
        emit_field(o, "", mb, len, width, left, false);
        return;
    }
    const char *m = mb;
    const wchar_t *x = w;
    char u[4];
    size_t units = 0, chars = 0;
    int k = 0;
    while ((prec < 0 || units < (size_t)prec) && (k = next_char(&m, &x, u)) > 0) {
        size_t step = o->wide ? 1 : (size_t)k;
        if (prec >= 0 && units + step > (size_t)prec) break;
        units += step;
        chars++;
    }
    if (k < 0 && (prec < 0 || units < (size_t)prec)) {
        encoding_error(o);
        return;
    }
    if (!left) pad(o, ' ', width - (int)units);
    char batch[64];
    size_t used = 0;
    m = mb, x = w;
    for (size_t i = 0; i < chars; i++) {
        if (used + 4 > sizeof batch) emit(o, batch, used), used = 0;
        used += (size_t)next_char(&m, &x, batch + used);
    }
    if (used) emit(o, batch, used);
    if (left) pad(o, ' ', width - (int)units);
}

/* %c, or %lc with wide: the wide functions take %c's byte as btowc does. */
static void put_char(out_t *o, int c, bool wide, int width, bool left) {
    char u[4];
    size_t n = 1;
    u[0] = (char)c;
    if (wide || o->wide) {
        mbstate_t st = {0};
        wint_t wc = wide ? (wint_t)c : btowc(c);
        n = wc == WEOF ? (size_t)-1 : wcrtomb(u, (wchar_t)wc, &st);
        if (n == (size_t)-1) {
            encoding_error(o);
            return;
        }
    }
    int units = o->wide ? 1 : (int)n;
    if (!left) pad(o, ' ', width - units);
    emit(o, u, n);
    if (left) pad(o, ' ', width - units);
}

/* The length modifiers: hh, h, none, l (also j, z and t, which are long), ll. */
enum { SIZE_HH = -2, SIZE_H = -1, SIZE_INT = 0, SIZE_LONG = 1, SIZE_LONG_LONG = 2 };

static void core(out_t *o, const char *fmt, va_list ap) {
    for (const char *p = fmt; *p && !o->failed; p++) {
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
        int size = SIZE_INT;
        bool ldbl = false;
        if (*p == 'h') size = *++p == 'h' ? (p++, SIZE_HH) : SIZE_H;
        else if (*p == 'l') size = *++p == 'l' ? (p++, SIZE_LONG_LONG) : SIZE_LONG;
        else if (*p == 'j' || *p == 'z' || *p == 't') size = SIZE_LONG, p++;
        else if (*p == 'L') ldbl = true, p++;
        char buf[720]; /* an integer zero-padded to its precision */
        char prefix[4] = {0};
        int n = 0;
        switch (*p) {
        case 'd':
        case 'i': {
            long long v = size == SIZE_LONG_LONG ? va_arg(ap, long long)
                          : size == SIZE_LONG    ? va_arg(ap, long)
                                                 : va_arg(ap, int);
            if (size == SIZE_HH) v = (signed char)v;
            else if (size == SIZE_H) v = (short)v;
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
            unsigned long long v = size == SIZE_LONG_LONG ? va_arg(ap, unsigned long long)
                                   : size == SIZE_LONG    ? va_arg(ap, unsigned long)
                                                          : va_arg(ap, unsigned int);
            if (size == SIZE_HH) v = (unsigned char)v;
            else if (size == SIZE_H) v = (unsigned short)v;
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
        case 'c': put_char(o, size == SIZE_LONG ? (int)va_arg(ap, wint_t) : va_arg(ap, int), size == SIZE_LONG, width, left); break;
        case 's':
            if (size == SIZE_LONG) {
                const wchar_t *w = va_arg(ap, const wchar_t *);
                put_text(o, NULL, w ? w : L"(null)", prec, width, left);
            } else {
                const char *s = va_arg(ap, const char *);
                put_text(o, s ? s : "(null)", NULL, prec, width, left);
            }
            break;
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A': {
            struct conv cv = {*p, left, plus, space, zero, alt, width, prec};
            if (ldbl) put_long_double(o, &cv, va_arg(ap, long double));
            else put_double(o, &cv, va_arg(ap, double));
            break;
        }
        case '%': emit(o, "%", 1); break;
        case 'n':
            switch (size) {
            case SIZE_HH: *va_arg(ap, signed char *) = (signed char)o->count; break;
            case SIZE_H: *va_arg(ap, short *) = (short)o->count; break;
            case SIZE_LONG: *va_arg(ap, long *) = (long)o->count; break;
            case SIZE_LONG_LONG: *va_arg(ap, long long *) = (long long)o->count; break;
            default: *va_arg(ap, int *) = (int)o->count; break;
            }
            break;
        default:
            emit(o, "%", 1);
            if (*p) emit(o, p, 1);
            else p--;
            break;
        }
    }
}

/* What printf returns: the count, or -1 after an encoding or output error. */
static int format(out_t *o, const char *fmt, va_list ap) {
    core(o, fmt, ap);
    if (!o->failed && o->count > INT_MAX) {
        o->failed = true;
        errno = ERANGE;
    }
    return o->failed ? -1 : (int)o->count;
}

struct sbuf {
    char *buf;
    size_t cap, len;
};

static bool put_sbuf(void *ctx, const char *s, size_t n) {
    struct sbuf *b = ctx;
    for (size_t i = 0; i < n; i++) {
        if (b->len + 1 < b->cap) b->buf[b->len] = s[i];
        b->len++;
    }
    return true;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    struct sbuf b = {buf, n, 0};
    out_t o = {.put = put_sbuf, .ctx = &b};
    int r = format(&o, fmt, ap);
    if (n) buf[b.len < n ? b.len : n - 1] = 0;
    return r;
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

static bool put_file(void *ctx, const char *s, size_t n) { return write_bytes(ctx, s, n) == 0; }

/* fprintf and fwprintf. An unbuffered stream gets a short text in one send. */
static int print_file(FILE *f, bool wide, const char *fmt, va_list ap) {
    if (f->mode == _IONBF) {
        char tmp[512];
        struct sbuf b = {tmp, sizeof tmp, 0};
        out_t o = {put_sbuf, &b, .wide = wide};
        va_list ap2;
        va_copy(ap2, ap);
        int n = format(&o, fmt, ap2);
        va_end(ap2);
        if (n < 0) return n;
        if (b.len < sizeof tmp) return write_bytes(f, tmp, b.len) ? -1 : n;
    }
    out_t o = {put_file, f, .wide = wide};
    return format(&o, fmt, ap);
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    orient(f, -1);
    return print_file(f, false, fmt, ap);
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

/* The wide functions give core their format as UTF-8: in small when it fits,
   else in a block the caller frees. */
static char *narrow_format(const wchar_t *fmt, char *small, size_t cap) {
    mbstate_t st = {0};
    const wchar_t *p = fmt;
    size_t n = wcsrtombs(NULL, &p, 0, &st);
    if (n == (size_t)-1) return NULL;
    char *s = n < cap ? small : malloc(n + 1);
    if (!s) {
        errno = ENOMEM;
        return NULL;
    }
    p = fmt;
    wcsrtombs(s, &p, n + 1, &st);
    return s;
}

/* swprintf's output, decoded from core's UTF-8. */
struct wbuf {
    wchar_t *buf;
    size_t cap, len;
    mbstate_t st;
};

static bool put_wbuf(void *ctx, const char *s, size_t n) {
    struct wbuf *b = ctx;
    for (size_t i = 0; i < n; i++) {
        wchar_t c;
        if (mbrtowc(&c, s + i, 1, &b->st) == (size_t)-2) continue;
        if (b->len < b->cap) b->buf[b->len] = c;
        b->len++;
    }
    return true;
}

/* ISO C: -1 when n wide characters or more were to be written, which leaves
   the first n - 1 and a null. */
int vswprintf(wchar_t *s, size_t n, const wchar_t *fmt, va_list ap) {
    char small[256], *narrow = narrow_format(fmt, small, sizeof small);
    if (!narrow) return -1;
    struct wbuf b = {.buf = s, .cap = n};
    out_t o = {put_wbuf, &b, .wide = true};
    int r = format(&o, narrow, ap);
    if (narrow != small) free(narrow);
    if (n) s[b.len < n ? b.len : n - 1] = 0;
    return b.len >= n ? -1 : r;
}

int swprintf(wchar_t *s, size_t n, const wchar_t *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vswprintf(s, n, fmt, ap);
    va_end(ap);
    return r;
}

int vfwprintf(FILE *f, const wchar_t *fmt, va_list ap) {
    orient(f, 1);
    char small[256], *narrow = narrow_format(fmt, small, sizeof small);
    if (!narrow) return -1;
    int r = print_file(f, true, narrow, ap);
    if (narrow != small) free(narrow);
    return r;
}

int vwprintf(const wchar_t *fmt, va_list ap) { return vfwprintf(stdout, fmt, ap); }

int fwprintf(FILE *f, const wchar_t *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfwprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int wprintf(const wchar_t *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfwprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

/* ---- scanf: one engine for the byte and the wide functions ----
   The input and the format are read in units: bytes for the byte functions,
   wide characters for the wide ones. */

struct scan_input {
    const char *s;     /* a string's bytes, */
    const wchar_t *ws; /* or its wide characters, */
    FILE *f;           /* or a stream's */
    bool wide;         /* the units are wide characters */
    size_t count;      /* units read, for %n */
};

#define NOTHING_READ (-2)

static int scan_get(struct scan_input *in) {
    int c;
    if (in->f) c = in->wide ? (int)fgetwc(in->f) : fgetc(in->f);
    else if (in->wide) c = *in->ws ? (int)*in->ws++ : EOF;
    else c = *in->s ? (unsigned char)*in->s++ : EOF;
    if (c != EOF) in->count++;
    return c;
}

/* Gives back the unit just read (one at most, as ungetc keeps). */
static void scan_unget(struct scan_input *in, int c) {
    if (c < 0) return;
    in->count--;
    if (!in->f) {
        if (in->wide) in->ws--;
        else in->s--;
    } else if (in->wide) {
        ungetwc((wint_t)c, in->f);
    } else {
        ungetc(c, in->f);
    }
}

static bool scan_space(const struct scan_input *in, int c) {
    return c >= 0 && (in->wide ? iswspace((wint_t)c) : isspace(c));
}

/* Skips white space; false at the input's end. */
static bool skip_space(struct scan_input *in) {
    int c;
    while (scan_space(in, c = scan_get(in))) {}
    scan_unget(in, c);
    return c != EOF;
}

struct scan_format {
    const char *s;
    const wchar_t *ws;
};
static int fmt_unit(const struct scan_format *f, size_t i) { return f->ws ? (int)f->ws[i] : (unsigned char)f->s[i]; }

/* %[: the units between start and end, where a-b is a range. */
static bool in_scanset(const struct scan_format *f, size_t start, size_t end, bool negate, int c) {
    bool found = false;
    for (size_t i = start; i < end && !found; i++) {
        int a = fmt_unit(f, i);
        if (i + 2 < end && fmt_unit(f, i + 1) == '-') {
            found = a <= c && c <= fmt_unit(f, i + 2);
            i += 2;
        } else {
            found = a == c;
        }
    }
    return found != negate;
}

/* A number's text, handed to strtol, strtod and the like. */
struct number_text {
    char small[64], *text;
    size_t len, cap;
    bool no_memory;
};
static void number_init(struct number_text *t) {
    t->text = t->small;
    t->small[0] = 0;
    t->len = 0;
    t->cap = sizeof t->small;
    t->no_memory = false;
}
static void number_free(struct number_text *t) {
    if (t->text != t->small) free(t->text);
}
static void number_push(struct number_text *t, int c) {
    if (t->len + 1 >= t->cap) {
        char *bigger = t->text == t->small ? malloc(t->cap * 2) : realloc(t->text, t->cap * 2);
        if (!bigger) {
            t->no_memory = true;
            return;
        }
        if (t->text == t->small) memcpy(bigger, t->small, t->len + 1);
        t->text = bigger;
        t->cap *= 2;
    }
    t->text[t->len++] = (char)c;
    t->text[t->len] = 0;
}
/* ISO C: the longest text that is, or begins, a number must be one whole. */
static bool number_matches(const struct number_text *t, const char *end) {
    return !t->no_memory && t->len && end == t->text + t->len;
}

/* Keeps c and reads the next unit while the field has room. */
static int take(struct scan_input *in, struct number_text *t, int c, size_t *n, size_t width) {
    number_push(t, c);
    return ++*n < width ? scan_get(in) : NOTHING_READ;
}
static int ascii_lower(int c) { return c >= 0 && c < 0x80 ? tolower(c) : -1; }
static int digit_value(int c) {
    int l = ascii_lower(c);
    return l >= '0' && l <= '9' ? l - '0' : l >= 'a' && l <= 'z' ? l - 'a' + 10 : 99;
}

/* An integer in base, or with base 0 in the base its prefix gives, as strtol. */
static void scan_integer(struct scan_input *in, size_t width, int base, struct number_text *t) {
    size_t n = 0;
    int c = scan_get(in);
    if (c == '+' || c == '-') c = take(in, t, c, &n, width);
    if ((base == 0 || base == 16) && c == '0') {
        c = take(in, t, c, &n, width);
        if (ascii_lower(c) == 'x') c = take(in, t, c, &n, width), base = 16;
        else if (!base) base = 8;
    } else if (!base) {
        base = 10;
    }
    while (digit_value(c) < base) c = take(in, t, c, &n, width);
    scan_unget(in, c);
}

/* The letters of word, in either case, as far as they match; true for all. */
static bool scan_word(struct scan_input *in, struct number_text *t, int *c, size_t *n, size_t width, const char *word) {
    for (; *word; word++) {
        if (ascii_lower(*c) != *word) return false;
        *c = take(in, t, *c, n, width);
    }
    return true;
}

/* A floating number as strtod reads it: decimal, hexadecimal, inf or nan. */
static void scan_float(struct scan_input *in, size_t width, struct number_text *t) {
    size_t n = 0;
    int c = scan_get(in);
    if (c == '+' || c == '-') c = take(in, t, c, &n, width);
    if (ascii_lower(c) == 'i') {
        if (scan_word(in, t, &c, &n, width, "inf")) scan_word(in, t, &c, &n, width, "inity");
    } else if (ascii_lower(c) == 'n') {
        if (scan_word(in, t, &c, &n, width, "nan") && c == '(') {
            c = take(in, t, c, &n, width);
            while (digit_value(c) < 36 || c == '_') c = take(in, t, c, &n, width);
            if (c == ')') c = take(in, t, c, &n, width);
        }
    } else {
        bool hex = false, digits = false, dot = false;
        if (c == '0') {
            c = take(in, t, c, &n, width);
            digits = true;
            if (ascii_lower(c) == 'x') c = take(in, t, c, &n, width), hex = true, digits = false;
        }
        for (;; c = take(in, t, c, &n, width)) {
            if (digit_value(c) < (hex ? 16 : 10)) digits = true;
            else if (c == '.' && !dot) dot = true;
            else break;
        }
        if (digits && ascii_lower(c) == (hex ? 'p' : 'e')) {
            c = take(in, t, c, &n, width);
            if (c == '+' || c == '-') c = take(in, t, c, &n, width);
            while (digit_value(c) < 10) c = take(in, t, c, &n, width);
        }
    }
    scan_unget(in, c);
}

static void store_integer(va_list *ap, int size, unsigned long long v) {
    switch (size) {
    case SIZE_HH: *va_arg(*ap, unsigned char *) = (unsigned char)v; break;
    case SIZE_H: *va_arg(*ap, unsigned short *) = (unsigned short)v; break;
    case SIZE_LONG: *va_arg(*ap, unsigned long *) = (unsigned long)v; break;
    case SIZE_LONG_LONG: *va_arg(*ap, unsigned long long *) = v; break;
    default: *va_arg(*ap, unsigned *) = (unsigned)v; break;
    }
}

/* Where %c, %s and %[ store: bytes, or with l wide characters. Units of the
   other kind are converted (mbrtowc, wcrtomb). */
struct text_sink {
    char *s;
    wchar_t *w;
    bool to_wide, bad;
    mbstate_t st;
};
static void sink_put(struct text_sink *k, bool wide_input, int c) {
    if (wide_input == k->to_wide) {
        if (k->w) *k->w++ = (wchar_t)c;
        else if (k->s) *k->s++ = (char)c;
        return;
    }
    if (k->to_wide) {
        char unit = (char)c;
        wchar_t wc;
        size_t r = mbrtowc(&wc, &unit, 1, &k->st);
        if (r == (size_t)-1) k->bad = true;
        else if (r != (size_t)-2 && k->w) *k->w++ = wc;
    } else {
        char units[4];
        size_t r = wcrtomb(units, (wchar_t)c, &k->st);
        if (r == (size_t)-1) k->bad = true;
        else if (k->s) memcpy(k->s, units, r), k->s += r;
    }
}

/* ISO C's scanf: the number of items assigned, or EOF when the input ended
   (or failed) before the first conversion. */
static int scan(struct scan_input *in, const struct scan_format *fmt, va_list args) {
    va_list ap;
    va_copy(ap, args);
    int assigned = 0;
    bool converted = false, input_failure = false;
    for (size_t i = 0; fmt_unit(fmt, i); i++) {
        int u = fmt_unit(fmt, i);
        if (scan_space(in, u)) {
            skip_space(in);
            continue;
        }
        if (u != '%' || fmt_unit(fmt, i + 1) == '%') {
            if (u == '%') {
                i++;
                skip_space(in);
            }
            int c = scan_get(in);
            if (c == EOF) {
                input_failure = true;
                break;
            }
            if (c != u) {
                scan_unget(in, c);
                break;
            }
            continue;
        }
        i++;
        bool skip = fmt_unit(fmt, i) == '*';
        if (skip) i++;
        size_t width = 0;
        while (fmt_unit(fmt, i) >= '0' && fmt_unit(fmt, i) <= '9') width = width * 10 + (size_t)(fmt_unit(fmt, i++) - '0');
        int size = SIZE_INT, m = fmt_unit(fmt, i);
        bool ldbl = false;
        if (m == 'h') size = fmt_unit(fmt, ++i) == 'h' ? (i++, SIZE_HH) : SIZE_H;
        else if (m == 'l') size = fmt_unit(fmt, ++i) == 'l' ? (i++, SIZE_LONG_LONG) : SIZE_LONG;
        else if (m == 'j' || m == 'z' || m == 't') size = SIZE_LONG, i++;
        else if (m == 'L') ldbl = true, i++;
        int spec = fmt_unit(fmt, i);
        if (!spec) break;
        if (spec != '[' && spec != 'c' && spec != 'n' && !skip_space(in)) {
            input_failure = true;
            break;
        }
        if (!width) width = spec == 'c' ? 1 : SIZE_MAX;
        bool ok = true;
        switch (spec) {
        case 'n':
            if (!skip) store_integer(&ap, size, in->count);
            break;
        case 'd':
        case 'i':
        case 'o':
        case 'u':
        case 'x':
        case 'X':
        case 'p': {
            int base = spec == 'd' || spec == 'u' ? 10 : spec == 'i' ? 0 : spec == 'o' ? 8 : 16;
            struct number_text t;
            number_init(&t);
            scan_integer(in, width, base, &t);
            char *end;
            unsigned long long v = spec == 'd' || spec == 'i' ? (unsigned long long)strtoll(t.text, &end, base)
                                                              : strtoull(t.text, &end, base);
            ok = number_matches(&t, end);
            number_free(&t);
            if (ok && !skip) {
                if (spec == 'p') *va_arg(ap, void **) = (void *)(uintptr_t)v;
                else store_integer(&ap, size, v);
                assigned++;
            }
            break;
        }
        case 'a':
        case 'A':
        case 'e':
        case 'E':
        case 'f':
        case 'F':
        case 'g':
        case 'G': {
            struct number_text t;
            number_init(&t);
            scan_float(in, width, &t);
            char *end;
            if (ldbl) {
                long double v = strtold(t.text, &end);
                if ((ok = number_matches(&t, end)) && !skip) *va_arg(ap, long double *) = v;
            } else if (size == SIZE_LONG) {
                double v = strtod(t.text, &end);
                if ((ok = number_matches(&t, end)) && !skip) *va_arg(ap, double *) = v;
            } else {
                float v = strtof(t.text, &end);
                if ((ok = number_matches(&t, end)) && !skip) *va_arg(ap, float *) = v;
            }
            number_free(&t);
            if (ok && !skip) assigned++;
            break;
        }
        case 'c':
        case 's':
        case '[': {
            size_t set = 0, set_end = 0;
            bool negate = false;
            if (spec == '[') {
                set = ++i;
                if (fmt_unit(fmt, set) == '^') negate = true, set = ++i;
                if (fmt_unit(fmt, i) == ']') i++;
                while (fmt_unit(fmt, i) && fmt_unit(fmt, i) != ']') i++;
                if (!fmt_unit(fmt, i)) {
                    ok = false;
                    break;
                }
                set_end = i;
            }
            struct text_sink k = {.to_wide = size == SIZE_LONG};
            if (!skip) {
                if (k.to_wide) k.w = va_arg(ap, wchar_t *);
                else k.s = va_arg(ap, char *);
            }
            size_t n = 0;
            int c = NOTHING_READ;
            while (n < width) {
                c = scan_get(in);
                if (c == EOF) break;
                if (spec == 's' ? scan_space(in, c) : spec == '[' && !in_scanset(fmt, set, set_end, negate, c)) break;
                sink_put(&k, in->wide, c);
                n++;
                c = NOTHING_READ;
            }
            scan_unget(in, c);
            if (spec == 'c' ? n < width : !n) {
                input_failure = c == EOF;
                ok = false;
            } else if (k.bad || !mbsinit(&k.st)) {
                errno = EILSEQ;
                ok = false;
            } else if (!skip) {
                if (spec != 'c') {
                    if (k.w) *k.w = 0;
                    else *k.s = 0;
                }
                assigned++;
            }
            break;
        }
        default: ok = false; break;
        }
        if (!ok) break;
        if (spec != 'n') converted = true;
    }
    va_end(ap);
    return input_failure && !converted ? EOF : assigned;
}

int vsscanf(const char *s, const char *fmt, va_list ap) {
    struct scan_input in = {.s = s};
    return scan(&in, &(struct scan_format){.s = fmt}, ap);
}

int sscanf(const char *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}

int vfscanf(FILE *f, const char *fmt, va_list ap) {
    orient(f, -1);
    struct scan_input in = {.f = f};
    return scan(&in, &(struct scan_format){.s = fmt}, ap);
}

int vscanf(const char *fmt, va_list ap) { return vfscanf(stdin, fmt, ap); }

int fscanf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfscanf(f, fmt, ap);
    va_end(ap);
    return r;
}

int scanf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfscanf(stdin, fmt, ap);
    va_end(ap);
    return r;
}

int vswscanf(const wchar_t *s, const wchar_t *fmt, va_list ap) {
    struct scan_input in = {.ws = s, .wide = true};
    return scan(&in, &(struct scan_format){.ws = fmt}, ap);
}

int swscanf(const wchar_t *s, const wchar_t *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vswscanf(s, fmt, ap);
    va_end(ap);
    return r;
}

int vfwscanf(FILE *f, const wchar_t *fmt, va_list ap) {
    orient(f, 1);
    struct scan_input in = {.f = f, .wide = true};
    return scan(&in, &(struct scan_format){.ws = fmt}, ap);
}

int vwscanf(const wchar_t *fmt, va_list ap) { return vfwscanf(stdin, fmt, ap); }

int fwscanf(FILE *f, const wchar_t *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfwscanf(f, fmt, ap);
    va_end(ap);
    return r;
}

int wscanf(const wchar_t *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfwscanf(stdin, fmt, ap);
    va_end(ap);
    return r;
}
