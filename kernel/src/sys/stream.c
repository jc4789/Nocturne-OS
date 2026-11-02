/* The console, the system log and the null stream (sys/stream.h). */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/spin.h"
#include "dev/input.h"
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

/* ---- the console: text for the serial port and the boot screen, keys from the keyboard ---- */

/* The presses typed and not read yet, as they came: which character a key makes is the reader's libc's.
   console_lock guards the ring: a keyboard's driver puts them in, in any context and on any CPU, and
   a reader takes them, looking under it before it waits in console_wait. It comes before console_wait's
   lock. The ring is fixed because a driver may not allocate here; it holds far more than anyone types
   ahead, and a press with no room is dropped, as a keyboard's own buffer does. */
#define CONSOLE_KEYS 256
#define CONSOLE_PIECE 32 /* presses a read takes at a time */
static struct spinlock console_lock;
static struct n_console_key console_keys[CONSOLE_KEYS];
static size_t console_head, console_tail, console_count;
static struct wait_queue console_wait;

static const struct object_ops console_ops;
static struct object console_object = {.ops = &console_ops};

void console_key(const struct key_event *e) {
    if (!e->pressed) return;
    uint64_t flags = spin_lock(&console_lock);
    if (console_count < CONSOLE_KEYS) {
        console_keys[console_head] = (struct n_console_key){.usage = e->usage, .text = e->text, .layout = e->layout, .mods = e->mods};
        console_head = (console_head + 1) % CONSOLE_KEYS;
        console_count++;
    }
    spin_unlock(&console_lock, flags);
    wq_wake_all(&console_wait);
    object_notify(&console_object);
}

/* Whole presses only, into the kernel under the lock and then out to the app's memory, which is
   looked at first: only a sibling unmapping the buffer meanwhile loses them. */
static int64_t console_read(struct handle *h, void *buf, size_t n, bool user, bool wait) {
    (void)h;
    struct n_console_key piece[CONSOLE_PIECE];
    size_t room = MIN(n / sizeof *piece, CONSOLE_PIECE);
    if (!room) return -N_ERR_TOO_LARGE;
    if (user && !user_ok_w(buf, room * sizeof *piece)) return -N_ERR_BAD_ADDRESS;
    for (;;) {
        uint64_t flags = spin_lock(&console_lock);
        if (console_count) {
            size_t got = MIN(room, console_count);
            for (size_t i = 0; i < got; i++) piece[i] = console_keys[(console_tail + i) % CONSOLE_KEYS];
            console_tail = (console_tail + got) % CONSOLE_KEYS;
            console_count -= got;
            spin_unlock(&console_lock, flags);
            if (user) {
                if (copy_to_user(buf, piece, got * sizeof *piece) < 0) return -N_ERR_BAD_ADDRESS;
            } else {
                memcpy(buf, piece, got * sizeof *piece);
            }
            return (int64_t)(got * sizeof *piece);
        }
        if (!wait) { spin_unlock(&console_lock, flags); return -N_ERR_NOT_READY; }
        if (thread_current()->killed) { spin_unlock(&console_lock, flags); return -N_ERR_STOPPED; }
        wq_wait_locked(&console_wait, &console_lock);
        irq_restore(flags);
    }
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
    return N_EVENT_SENDABLE | (console_count ? N_EVENT_RECEIVABLE : 0);
}
static const struct object_ops console_ops = {
    .kind = N_KIND_CONSOLE, .read = console_read, .write = console_write, .ready = console_ready,
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
