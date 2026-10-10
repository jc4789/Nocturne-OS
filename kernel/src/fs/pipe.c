/* Anonymous pipes. */
#include "fs/vfs.h"
#include "mm/heap.h"
#include "arch/cpu.h"

#define PIPE_SEGMENT_SIZE 16384 /* allocation/copy granularity, not a capacity ceiling */

struct pipe_segment {
    struct pipe_segment *next;
    size_t read_at, write_at;
    uint8_t bytes[PIPE_SEGMENT_SIZE];
};

struct pipe {
    struct pipe_segment *head, *tail;
    size_t count;
    int readers, writers;
    struct wait_queue rq, wq;
};

static struct pipe_segment *pipe_segment_new(void){
    struct pipe_segment *s=kmalloc(sizeof *s);
    if(s){s->next=NULL;s->read_at=s->write_at=0;}
    return s;
}

static int64_t pipe_read(struct vnode *v, struct file *f, void *buf, uint64_t off, size_t n) {
    struct pipe *p = v->priv;
    uint8_t *out = buf;
    size_t done=0;
    if(!n)return 0;
    for (;;) {
        uint64_t fl = irq_save();
        if (p->count > 0) {
            struct pipe_segment *s=p->head;
            size_t available=s->write_at-s->read_at;
            size_t got = n-done < available ? n-done : available;
            memcpy(out+done,s->bytes+s->read_at,got);
            s->read_at+=got;
            p->count -= got;
            done+=got;
            if(s->read_at==s->write_at){
                if(s!=p->tail){p->head=s->next;kfree(s);}
                else s->read_at=s->write_at=0; /* reuse the empty final owner */
            }
            wq_wake_all(&p->wq);
            poll_notify();
            irq_restore(fl);
            if(done==n)return (int64_t)done;
            continue;
        }
        if(done){irq_restore(fl);return (int64_t)done;}
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
        if(current_task->killed){
            irq_restore(fl);
            return done?(int64_t)done:-EINTR;
        }
        if(n-done>SIZE_MAX-p->count){irq_restore(fl);return done?(int64_t)done:-EINVAL;}
        size_t committed=0;int error=0;
        /* Grow only by the segment receiving actual bytes. Existing owners
           never move. At most one segment allocation and the former 16 KiB
           copy quantum occur per IRQ boundary, even for a large VIDEO.
           Writes within that quantum retain their original atomic boundary. */
        while(done<n&&committed<PIPE_SEGMENT_SIZE){
            struct pipe_segment *s=p->tail;
            if(s->write_at==PIPE_SEGMENT_SIZE){
                struct pipe_segment *next=pipe_segment_new();
                if(!next){error=-ENOMEM;break;}
                s->next=next;p->tail=s=next;
            }
            size_t put=MIN(n-done,MIN(PIPE_SEGMENT_SIZE-s->write_at,PIPE_SEGMENT_SIZE-committed));
            memcpy(s->bytes+s->write_at,in+done,put);
            s->write_at+=put;
            p->count += put;
            done += put;
            committed+=put;
        }
        if(committed){
            wq_wake_all(&p->rq);
            poll_notify();
        }
        irq_restore(fl);
        if(error)return done?(int64_t)done:error;
    }
    return done;
}

static bool pipe_can_read(struct vnode *v, struct file *f) {
    struct pipe *p = v->priv;
    return p->count > 0 || p->writers == 0;
}

static bool pipe_can_write(struct vnode *v, struct file *f) {
    struct pipe *p = v->priv;
    /* Full current storage is not backpressure: the next write can grow it.
       poll must allow that attempt, without allocating or guessing OOM here.
       A closed reader is writable readiness for the real EPIPE result. */
    return p->count < SIZE_MAX || p->readers == 0;
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
    struct pipe *p=v->priv;
    for(struct pipe_segment *s=p->head;s;){struct pipe_segment *next=s->next;kfree(s);s=next;}
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
    if(p)p->head=p->tail=pipe_segment_new();
    if (!v || !p || !p->head) {
        kfree(v);
        if(p)kfree(p->head);
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
    if(!*rd){pipe_release(v);*wr=NULL;return -ENOMEM;}
    *wr = vfs_open_vnode(v, O_WRONLY);
    if(!*wr){vfs_close(*rd);*rd=NULL;return -ENOMEM;}
    return 0;
}
