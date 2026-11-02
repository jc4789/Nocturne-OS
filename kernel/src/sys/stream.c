/* The console, the system log and the null stream (sys/stream.h). */
#include "kernel.h"
#include "arch/cpu.h"
#include "mm/heap.h"
#include "sys/channel.h"
#include "sys/log.h"
#include "sys/process.h"
#include "sys/syscall.h"
#include "sys/object.h"
#include "sys/stream.h"
#include "sys/thread.h"
#include "sys/wait.h"

#define COPY_PIECE 512 /* an app's bytes are copied into the kernel this much at a time */

/* Hands what was sent to out, piece by piece, each copied in from the app's memory first (a
   sibling thread may unmap it meanwhile: the copy then ends the send). */
static int64_t stream_put(const void *buf, size_t n, bool user, void (*out)(const char *, size_t)) {
    if (!user) { out(buf, n); return (int64_t)n; }
    char piece[COPY_PIECE];
    for (size_t done = 0; done < n;) {
        size_t c = MIN(n - done, sizeof piece);
        if (copy_from_user(piece, (const char *)buf + done, c) < 0) return -N_ERR_BAD_ADDRESS;
        out(piece, c);
        done += c;
    }
    return (int64_t)n;
}

static struct handle *stream_open(struct object *o) { return handle_open(o, HANDLE_WRITE); }

/* ---- nothing: what is sent is dropped, a receive finds the end ---- */

static int64_t null_read(struct handle *h, void *buf, size_t n, bool user, bool wait) {
    (void)h; (void)buf; (void)n; (void)user; (void)wait;
    return 0;
}
static int64_t null_write(struct handle *h, const void *buf, size_t n, bool user) {
    (void)h; (void)buf; (void)user;
    return (int64_t)n;
}
static uint32_t null_ready(struct handle *h) { (void)h; return N_EVENT_RECEIVABLE | N_EVENT_SENDABLE; }
static const struct object_ops null_ops = {
    .kind = N_KIND_DEVICE, .read = null_read, .write = null_write, .ready = null_ready,
};
static struct object null_object = {.ops = &null_ops};

struct handle *stream_null_open(void) { return stream_open(&null_object); }

/* ---- the system log ----
   What an app sends becomes records of the kernel's log (sys/log.h), which the serial port and
   the boot screen show with the kernel's own: a driver app (nvrm) has no other place to say how
   far it got before the screen stops changing. What an app receives is the log's text: each
   opening reads with a cursor of its own, made at its first receive (an app that only sends
   never has one), from the oldest record the rings hold then. Keeping the log on C: is an app's
   work (init). */

struct log_reader {
    struct mutex lock; /* one receive at a time on the opening: the cursor */
    struct log_cursor *cursor;
};

static void log_out(const char *s, size_t n) { log_text(LOG_INFO, s, n); }
static int64_t log_write(struct handle *h, const void *buf, size_t n, bool user) {
    (void)h;
    return stream_put(buf, n, user, log_out);
}

static struct log_reader *log_reader_of(struct handle *h) {
    struct log_reader *r = __atomic_load_n((struct log_reader **)&h->priv, __ATOMIC_ACQUIRE);
    if (r) return r;
    struct log_reader *made = kzalloc(sizeof *made);
    if (!made || !(made->cursor = log_cursor_new(0))) {
        kfree(made);
        return NULL;
    }
    if (__atomic_compare_exchange_n((struct log_reader **)&h->priv, &r, made, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return made;
    log_cursor_free(made->cursor); /* another receive made one first */
    kfree(made);
    return r;
}

/* The text after the opening's cursor, as much as fits in n (at least something with wait). */
static int64_t log_receive(struct handle *h, void *buf, size_t n, bool user, bool wait) {
    if (!handle_allows(h, HANDLE_READ)) return -N_ERR_DENIED;
    struct log_reader *r = log_reader_of(h);
    if (!r) return -N_ERR_NO_MEMORY;
    char piece[COPY_PIECE];
    for (;;) {
        uint64_t seen = log_told();
        size_t done = 0;
        bool fault = false;
        mutex_lock(&r->lock);
        while (done < n) {
            char *to = user ? piece : (char *)buf + done;
            size_t got = log_read(r->cursor, to, user ? MIN(n - done, sizeof piece) : n - done);
            if (!got) break;
            if (user && copy_to_user((char *)buf + done, piece, got) < 0) {
                fault = true;
                break;
            }
            done += got;
        }
        mutex_unlock(&r->lock);
        if (done) return (int64_t)done;
        if (fault) return -N_ERR_BAD_ADDRESS;
        if (!wait) return -N_ERR_NOT_READY;
        if (thread_current()->killed) return -N_ERR_STOPPED;
        log_wait(seen, CLOCK_NEVER);
    }
}

/* A hint (an event queue's lock held): an opening that never received has the whole log ahead. */
static uint32_t log_ready(struct handle *h) {
    struct log_reader *r = __atomic_load_n((struct log_reader **)&h->priv, __ATOMIC_ACQUIRE);
    if (!handle_allows(h, HANDLE_READ)) return N_EVENT_SENDABLE;
    return N_EVENT_SENDABLE | (!r || log_pending(r->cursor) ? N_EVENT_RECEIVABLE : 0);
}

static void log_close(struct handle *h) {
    struct log_reader *r = h->priv;
    if (!r) return;
    log_cursor_free(r->cursor);
    kfree(r);
}

static const struct object_ops log_ops = {
    .kind = N_KIND_DEVICE, .read = log_receive, .write = log_write, .ready = log_ready, .close = log_close,
};
static struct object log_object = {.ops = &log_ops};

struct handle *stream_system_log_open(bool read) {
    return handle_open(&log_object, HANDLE_WRITE | (read ? HANDLE_READ : 0));
}

void stream_system_log_told(void) { object_notify(&log_object); }

/* ---- the console: text for the serial port and the boot screen; nothing comes from it ---- */

static const struct object_ops console_ops;
static struct object console_object = {.ops = &console_ops};

static int64_t console_read(struct handle *h, void *buf, size_t n, bool user, bool wait) {
    (void)h; (void)buf; (void)n; (void)user; (void)wait;
    return 0;
}

/* The console's text goes into the log as text of its own, which the serial port and the boot
   screen show and the system log's receivers do not. */
static void console_out(const char *s, size_t n) { log_console(s, n); }
static int64_t console_write(struct handle *h, const void *buf, size_t n, bool user) {
    (void)h;
    return stream_put(buf, n, user, console_out);
}
static uint32_t console_ready(struct handle *h) {
    (void)h;
    return N_EVENT_SENDABLE | N_EVENT_RECEIVABLE;
}
static const struct object_ops console_ops = {
    .kind = N_KIND_DEVICE, .read = console_read, .write = console_write, .ready = console_ready,
};

struct handle *stream_console_open(void) { return stream_open(&console_object); }

/* ---- an app's error output ---- */

void stream_error_printf(struct process *p, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    struct handle *h = handle_get_of(&p->handles, 2);
    if (!h) return;
    /* A message channel has no byte write: the text goes as one message. */
    if (message_channel_is(h)) message_channel_post(h, buf, (size_t)n, (uint64_t)p->pid);
    else if (h->object->ops->write) h->object->ops->write(h, buf, (size_t)n, false);
    handle_close(h);
}
