/* The console, the system log and the null stream (sys/stream.h). */
#include "kernel.h"
#include "arch/cpu.h"
#include "dev/fbcon.h"
#include "dev/serial.h"
#include "sys/sched.h"
#include "sys/syscall.h"
#include "sys/object.h"
#include "sys/stream.h"

#define COPY_PIECE 512 /* an app's bytes are copied in under the guard this much at a time */

/* Hands what was sent to out, piece by piece: a sibling thread may unmap the app's memory
   meanwhile, so each piece is checked and copied with the guard held. */
static int64_t stream_put(const void *buf, size_t n, bool user, void (*out)(const char *, size_t)) {
    if (!user) { out(buf, n); return (int64_t)n; }
    char piece[COPY_PIECE];
    for (size_t done = 0; done < n;) {
        size_t c = MIN(n - done, sizeof piece);
        uint64_t flags = irq_save();
        bool ok = user_ok((const char *)buf + done, c);
        if (ok) memcpy(piece, (const char *)buf + done, c);
        irq_restore(flags);
        if (!ok) return -N_ERR_BAD_ADDRESS;
        out(piece, c);
        done += c;
    }
    return (int64_t)n;
}

static struct handle *stream_open(struct object *o) { return handle_open(o, true); }

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
   An app's system log lines go where the kernel's own go, the text console included: a driver
   app (nvrm) has no other place to say how far it got before the screen stops changing. */

static void log_out(const char *s, size_t n) {
    klog_write(s, n);
    serial_write(s, n);
    fbcon_write_kernel(s, n);
}
static int64_t log_write(struct handle *h, const void *buf, size_t n, bool user) {
    (void)h;
    return stream_put(buf, n, user, log_out);
}
static uint32_t log_ready(struct handle *h) { (void)h; return N_EVENT_SENDABLE; }
static const struct object_ops log_ops = {.kind = N_KIND_DEVICE, .write = log_write, .ready = log_ready};
static struct object log_object = {.ops = &log_ops};

struct handle *stream_system_log_open(void) { return stream_open(&log_object); }

/* ---- the console: text on the screen, keys from the keyboard ---- */

#define CONSOLE_INPUT 1024
static char console_in[CONSOLE_INPUT];
static size_t console_head, console_tail, console_count;
static struct wait_queue console_wait;
static const struct object_ops console_ops;
static struct object console_object = {.ops = &console_ops};

void console_input_char(char c) {
    uint64_t flags = irq_save();
    if (console_count < CONSOLE_INPUT) {
        console_in[console_head] = c;
        console_head = (console_head + 1) % CONSOLE_INPUT;
        console_count++;
    }
    wq_wake_all(&console_wait);
    object_notify(&console_object);
    irq_restore(flags);
}

void console_input_str(const char *s) {
    while (*s) console_input_char(*s++);
}

static int64_t console_read(struct handle *h, void *buf, size_t n, bool user, bool wait) {
    (void)h;
    char *out = buf;
    if (!n) return 0;
    for (;;) {
        uint64_t flags = irq_save();
        if (console_count) {
            size_t got = MIN(n, console_count);
            if (user && !user_ok_w(buf, got)) { irq_restore(flags); return -N_ERR_BAD_ADDRESS; }
            size_t first = MIN(got, CONSOLE_INPUT - console_tail);
            memcpy(out, console_in + console_tail, first);
            memcpy(out + first, console_in, got - first);
            console_tail = (console_tail + got) % CONSOLE_INPUT;
            console_count -= got;
            irq_restore(flags);
            return (int64_t)got;
        }
        if (!wait) { irq_restore(flags); return -N_ERR_NOT_READY; }
        if (current_task->killed) { irq_restore(flags); return -N_ERR_STOPPED; }
        wq_wait(&console_wait);
        irq_restore(flags);
    }
}

static void console_out(const char *s, size_t n) {
    fbcon_write(s, n);
    serial_write(s, n);
}
static int64_t console_write(struct handle *h, const void *buf, size_t n, bool user) {
    (void)h;
    return stream_put(buf, n, user, console_out);
}
static uint32_t console_ready(struct handle *h) {
    (void)h;
    return N_EVENT_SENDABLE | (console_count ? N_EVENT_RECEIVABLE : 0);
}
static const struct object_ops console_ops = {
    .kind = N_KIND_DEVICE, .read = console_read, .write = console_write, .ready = console_ready,
};

struct handle *stream_console_open(void) { return stream_open(&console_object); }
