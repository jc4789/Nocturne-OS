/* Anonymous pipes. */
#include "fs/vfs.h"
#include "mm/heap.h"
#include "arch/cpu.h"

#define PIPE_SIZE 16384

struct pipe {
    uint8_t buf[PIPE_SIZE];
    uint32_t head, tail, count;
    int readers, writers;
    struct wait_queue rq, wq;
};

static int64_t pipe_read(struct vnode *v, struct file *f, void *buf, uint64_t off, size_t n) {
    struct pipe *p = v->priv;
    uint8_t *out = buf;
    for (;;) {
        uint64_t fl = irq_save();
        if (p->count > 0) {
            size_t got = 0;
            while (got < n && p->count > 0) {
                out[got++] = p->buf[p->tail];
                p->tail = (p->tail + 1) % PIPE_SIZE;
                p->count--;
            }
            wq_wake_all(&p->wq);
            poll_notify();
            irq_restore(fl);
            return got;
        }
        if (p->writers == 0) {
            irq_restore(fl);
            return 0; /* EOF */
        }
        if (f->flags & O_NONBLOCK) {
            irq_restore(fl);
            return -EAGAIN;
        }
        if (current_task->killed) {
            irq_restore(fl);
            return -EINTR;
        }
        wq_wait(&p->rq);
        irq_restore(fl);
    }
}

static int64_t pipe_write(struct vnode *v, struct file *f, const void *buf, uint64_t off, size_t n) {
    struct pipe *p = v->priv;
    const uint8_t *in = buf;
    size_t done = 0;
    while (done < n) {
        uint64_t fl = irq_save();
        if (p->readers == 0) {
            irq_restore(fl);
            return done ? (int64_t)done : -EPIPE;
        }
        if (p->count < PIPE_SIZE) {
            while (done < n && p->count < PIPE_SIZE) {
                p->buf[p->head] = in[done++];
                p->head = (p->head + 1) % PIPE_SIZE;
                p->count++;
            }
            wq_wake_all(&p->rq);
            poll_notify();
            irq_restore(fl);
            continue;
        }
        if (f->flags & O_NONBLOCK) {
            irq_restore(fl);
            return done ? (int64_t)done : -EAGAIN;
        }
        if (current_task->killed) {
            irq_restore(fl);
            return done ? (int64_t)done : -EINTR;
        }
        wq_wait(&p->wq);
        irq_restore(fl);
    }
    return done;
}

static bool pipe_can_read(struct vnode *v, struct file *f) {
    struct pipe *p = v->priv;
    return p->count > 0 || p->writers == 0;
}

static bool pipe_can_write(struct vnode *v, struct file *f) {
    struct pipe *p = v->priv;
    return p->count < PIPE_SIZE || p->readers == 0;
}

static void pipe_close(struct vnode *v, struct file *f) {
    struct pipe *p = v->priv;
    uint64_t fl = irq_save();
    if ((f->flags & O_ACCMODE) == O_RDONLY) p->readers--;
    else p->writers--;
    wq_wake_all(&p->rq);
    wq_wake_all(&p->wq);
    poll_notify();
    irq_restore(fl);
}

static void pipe_release(struct vnode *v) {
    kfree(v->priv);
    kfree(v);
}

static struct vnode_ops pipe_ops = {
    .read = pipe_read,
    .write = pipe_write,
    .can_read = pipe_can_read,
    .can_write = pipe_can_write,
    .close = pipe_close,
    .release = pipe_release,
};

int pipe_create(struct file **rd, struct file **wr) {
    struct vnode *v = kzalloc(sizeof *v);
    struct pipe *p = kzalloc(sizeof *p);
    if (!v || !p) {
        kfree(v);
        kfree(p);
        return -ENOMEM;
    }
    strlcpy(v->name, "pipe", sizeof v->name);
    v->type = VT_PIPE;
    v->ops = &pipe_ops;
    v->priv = p;
    v->unlinked = true; /* freed when the last end closes */
    p->readers = 1;
    p->writers = 1;
    *rd = vfs_open_vnode(v, O_RDONLY);
    *wr = vfs_open_vnode(v, O_WRONLY);
    return 0;
}
