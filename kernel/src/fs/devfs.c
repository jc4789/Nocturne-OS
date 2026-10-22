/* Device nodes under /dev. */
#include "fs/ramfs.h"
#include "mm/heap.h"
#include "arch/cpu.h"
#include "dev/fbcon.h"
#include "dev/serial.h"
#include "dev/entropy.h"
#include "sys/syscall.h"

/* ---- null / zero / random ---- */
static int64_t null_read(struct vnode *v, struct file *f, void *buf, uint64_t off, size_t n) { return 0; }
static int64_t null_write(struct vnode *v, struct file *f, const void *buf, uint64_t off, size_t n) { return n; }
static int64_t zero_read(struct vnode *v, struct file *f, void *buf, uint64_t off, size_t n) {
    memset(buf, 0, n);
    return n;
}

static int64_t random_read(struct vnode *v, struct file *f, void *buf, uint64_t off, size_t n) {
    entropy_get(buf, n);
    return n;
}

static struct vnode_ops null_ops = {.read = null_read, .read_user = null_read, .write = null_write};
static struct vnode_ops zero_ops = {.read = zero_read, .write = null_write};
static struct vnode_ops random_ops = {.read = random_read, .write = null_write};

/* ---- kernel log ---- */
static int64_t kmsg_read_common(struct vnode *v, struct file *f, void *buf, uint64_t off, size_t n, bool user) {
    static char tmp[65536];
    size_t total = klog_read(tmp, sizeof tmp);
    if (off >= total) return 0;
    size_t c = MIN(n, total - off);
    if (user && !user_ok_w(buf, c)) return -N_ERR_BAD_ADDRESS;
    memcpy(buf, tmp + off, c);
    f->off += c;
    return c;
}
static int64_t kmsg_read(struct vnode *v, struct file *f, void *buf, uint64_t off, size_t n) {
    return kmsg_read_common(v, f, buf, off, n, false);
}
static int64_t kmsg_read_user(struct vnode *v, struct file *f, void *buf, uint64_t off, size_t n) {
    return kmsg_read_common(v, f, buf, off, n, true);
}
/* An app's system log lines go where the kernel's own go, the text console included: a driver
   app (nvrm) has no other place to say how far it got before the screen stops changing. */
static int64_t kmsg_write(struct vnode *v, struct file *f, const void *buf, uint64_t off, size_t n) {
    klog_write(buf, n);
    serial_write(buf, n);
    fbcon_write_kernel(buf, n);
    return n;
}
static struct vnode_ops kmsg_ops = {.read = kmsg_read, .read_user = kmsg_read_user, .write = kmsg_write};

/* ---- console: framebuffer text + keyboard ---- */
#define CONS_BUF 1024
static char cons_buf[CONS_BUF];
static int cons_head, cons_tail, cons_count;
static struct wait_queue cons_wq;
static struct vnode *cons_vnode;

void console_input_char(char c) {
    uint64_t f = irq_save();
    if (cons_count < CONS_BUF) {
        cons_buf[cons_head] = c;
        cons_head = (cons_head + 1) % CONS_BUF;
        cons_count++;
    }
    wq_wake_all(&cons_wq);
    vfs_poll_notify(cons_vnode);
    irq_restore(f);
}

void console_input_str(const char *s) {
    while (*s) console_input_char(*s++);
}

static int64_t cons_read_common(struct vnode *v, struct file *f, void *buf, uint64_t off, size_t n, bool user) {
    char *out = buf;
    if (!n) return 0;
    for (;;) {
        uint64_t fl = irq_save();
        if (cons_count) {
            size_t got = MIN(n, (size_t)cons_count);
            if (user && !user_ok_w(buf, got)) { irq_restore(fl); return -N_ERR_BAD_ADDRESS; }
            size_t first = MIN(got, (size_t)CONS_BUF - cons_tail);
            memcpy(out, cons_buf + cons_tail, first);
            memcpy(out + first, cons_buf, got - first);
            cons_tail = (cons_tail + got) % CONS_BUF;
            cons_count -= got;
            irq_restore(fl);
            return got;
        }
        if (f->flags & O_NONBLOCK) {
            irq_restore(fl);
            return -N_ERR_NOT_READY;
        }
        if (current_task->killed) {
            irq_restore(fl);
            return -N_ERR_STOPPED;
        }
        wq_wait(&cons_wq);
        irq_restore(fl);
    }
}
static int64_t cons_read(struct vnode *v, struct file *f, void *buf, uint64_t off, size_t n) {
    return cons_read_common(v, f, buf, off, n, false);
}
static int64_t cons_read_user(struct vnode *v, struct file *f, void *buf, uint64_t off, size_t n) {
    return cons_read_common(v, f, buf, off, n, true);
}

static int64_t cons_write(struct vnode *v, struct file *f, const void *buf, uint64_t off, size_t n) {
    fbcon_write(buf, n);
    serial_write(buf, n);
    return n;
}

static bool cons_can_read(struct vnode *v, struct file *f) { return cons_count > 0; }

static struct vnode_ops cons_ops = {.read = cons_read, .read_user = cons_read_user, .write = cons_write, .can_read = cons_can_read};

static void add_dev(struct vnode *dir, const char *name, struct vnode_ops *ops) {
    struct vnode *v = ramfs_new_node(name, VT_CHAR);
    v->ops = ops;
    if (ops == &cons_ops) cons_vnode = v;
    v->mode = 0666;
    ramfs_add_child(dir, v);
}

void devfs_init(void) {
    struct vnode *dev = ramfs_new_root(); /* a tree of its own: never recorded */
    if (!dev || vfs_mount("/dev", dev) < 0) panic("devfs: cannot mount /dev");
    add_dev(dev, "null", &null_ops);
    add_dev(dev, "zero", &zero_ops);
    add_dev(dev, "random", &random_ops);
    add_dev(dev, "urandom", &random_ops);
    add_dev(dev, "console", &cons_ops);
    add_dev(dev, "kmsg", &kmsg_ops);
}
