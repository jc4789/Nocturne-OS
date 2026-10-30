/* The console, the system log and the null stream (sys/stream.h). */
#include "kernel.h"
#include "arch/cpu.h"
#include "dev/fbcon.h"
#include "dev/serial.h"
#include "fs/store.h"
#include "mm/heap.h"
#include "sys/sched.h"
#include "sys/syscall.h"
#include "sys/object.h"
#include "sys/stream.h"

#define COPY_PIECE 512 /* an app's bytes are copied into the kernel this much at a time */

/* Hands what was sent to out, piece by piece, each copied in from the app's memory first (a
   sibling thread may unmap it meanwhile: the copy then ends the send). The CPU is given up between
   pieces when another task waits for it: kernel code is not preempted. */
static int64_t stream_put(const void *buf, size_t n, bool user, void (*out)(const char *, size_t)) {
    if (!user) { out(buf, n); return (int64_t)n; }
    char piece[COPY_PIECE];
    for (size_t done = 0; done < n;) {
        size_t c = MIN(n - done, sizeof piece);
        if (copy_from_user(piece, (const char *)buf + done, c) < 0) return -N_ERR_BAD_ADDRESS;
        out(piece, c);
        done += c;
        if (need_resched) schedule();
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

/* ---- the system log on C: ----
   A machine with no serial port loses the log with its power, and what a start said before it
   stopped is what one wants to read. So the log is also kept in the user's AppData\Nocturne:
   System.log for this start, System-previous.log for the one before. It can be read from Files,
   or from another system (host/store). The writer commits it within seconds (fs/store.c). */

#define KEEP_MS 500             /* how often new text goes to C: */
#define KEEP_PIECE 65536        /* text taken from the log at a time */
#define KEEP_MOST (32ull << 20) /* a start keeps this much at most: C: has its other uses */

static void keep_note(const char *path, const char *fmt, unsigned long long n) {
    char note[96];
    int k = ksnprintf(note, sizeof note, fmt, n);
    store_append(path, note, (size_t)MIN(k, (int)sizeof note - 1), false);
}

static void log_keep(void *arg) {
    char *path = arg, *piece = kmalloc(KEEP_PIECE);
    if (!piece) return;
    uint64_t kept = 0;
    for (size_t at = 0, lost, n;;) {
        if (!(n = klog_since(&at, piece, KEEP_PIECE, &lost))) {
            sleep_ms(KEEP_MS);
            continue;
        }
        if (lost) keep_note(path, "\n[system log: %llu bytes were not kept here]\n", lost);
        if (kept + n > KEEP_MOST) {
            keep_note(path, "\n[system log: this start said more than %llu MiB; the rest is not kept]\n", KEEP_MOST >> 20);
            break;
        }
        store_append(path, piece, n, false);
        kept += n;
    }
    kfree(piece);
}

void stream_system_log_keep(void) {
    if (!store_raw_possible()) return; /* C: in memory ends with the power too */
    static char path[sizeof "C:\\Users\\" + 255 + sizeof "\\AppData\\Nocturne\\System-previous.log"];
    char previous[sizeof path];
    ksnprintf(path, sizeof path, "%s\\AppData\\Nocturne", store_user_folder());
    int r = store_folder_create(path, false);
    ksnprintf(previous, sizeof previous, "%s\\System-previous.log", path);
    strlcat(path, "\\System.log", sizeof path);
    if (r >= 0 || r == -N_ERR_EXISTS) r = store_entry_move(path, previous, true, false);
    if (r < 0 && r != -N_ERR_NOT_FOUND) kprintf("system log: it cannot be kept on C: (%d)\n", r);
    else if (!kthread_create("system log", log_keep, path)) kprintf("system log: it cannot be kept on C:\n");
}

/* ---- the console: text on the screen, keys from the keyboard ---- */

/* The keys typed and not read yet. console_lock guards the ring: the keyboard's interrupt handler
   (CPU 0) puts keys in and a reader on any CPU takes them, looking under it before it waits in
   console_wait. It comes before console_wait's lock. */
#define CONSOLE_INPUT 1024
#define CONSOLE_PIECE 256 /* bytes a read into an app's memory takes at a time */
static struct spinlock console_lock;
static char console_in[CONSOLE_INPUT];
static size_t console_head, console_tail, console_count;
static struct wait_queue console_wait;
static const struct object_ops console_ops;
static struct object console_object = {.ops = &console_ops};

void console_input_char(char c) {
    uint64_t flags = spin_lock(&console_lock);
    if (console_count < CONSOLE_INPUT) {
        console_in[console_head] = c;
        console_head = (console_head + 1) % CONSOLE_INPUT;
        console_count++;
    }
    spin_unlock(&console_lock, flags);
    wq_wake_all(&console_wait);
    object_notify(&console_object);
}

void console_input_str(const char *s) {
    while (*s) console_input_char(*s++);
}

/* An app's read takes at most CONSOLE_PIECE bytes, into the kernel under the lock and then out to
   its memory, which is looked at first: only a sibling unmapping the buffer meanwhile loses them. */
static int64_t console_read(struct handle *h, void *buf, size_t n, bool user, bool wait) {
    (void)h;
    char piece[CONSOLE_PIECE];
    if (!n) return 0;
    if (user && !user_ok_w(buf, MIN(n, sizeof piece))) return -N_ERR_BAD_ADDRESS;
    for (;;) {
        uint64_t flags = spin_lock(&console_lock);
        if (console_count) {
            size_t got = MIN(n, console_count);
            if (user) got = MIN(got, sizeof piece);
            char *out = user ? piece : buf;
            size_t first = MIN(got, CONSOLE_INPUT - console_tail);
            memcpy(out, console_in + console_tail, first);
            memcpy(out + first, console_in, got - first);
            console_tail = (console_tail + got) % CONSOLE_INPUT;
            console_count -= got;
            spin_unlock(&console_lock, flags);
            if (user && copy_to_user(buf, piece, got) < 0) return -N_ERR_BAD_ADDRESS;
            return (int64_t)got;
        }
        if (!wait) { spin_unlock(&console_lock, flags); return -N_ERR_NOT_READY; }
        if (current_task->killed) { spin_unlock(&console_lock, flags); return -N_ERR_STOPPED; }
        wq_wait_locked(&console_wait, &console_lock);
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
