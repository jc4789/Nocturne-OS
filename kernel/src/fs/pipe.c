/* Anonymous pipes. */
#include "fs/vfs.h"
#include "mm/heap.h"
#include "arch/cpu.h"
#include "sys/syscall.h"

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

static int64_t pipe_read_common(struct vnode *v, struct file *f, void *buf, uint64_t off, size_t n, bool user) {
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
            if (user && !user_ok_w(out+done, got)) {
                irq_restore(fl); return done ? (int64_t)done : -N_ERR_BAD_ADDRESS;
            }
            memcpy(out+done,s->bytes+s->read_at,got);
            s->read_at+=got;
            p->count -= got;
            done+=got;
            if(s->read_at==s->write_at){
                if(s!=p->tail){p->head=s->next;kfree(s);}
                else s->read_at=s->write_at=0; /* reuse the empty final owner */
            }
            wq_wake_all(&p->wq);
            vfs_poll_notify(v);
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
            return -N_ERR_NOT_READY;
        }
        if (current_task->killed) {
            irq_restore(fl);
            return -N_ERR_STOPPED;
        }
        wq_wait(&p->rq);
        irq_restore(fl);
    }
}

static int64_t pipe_read(struct vnode *v, struct file *f, void *buf, uint64_t off, size_t n) {
    return pipe_read_common(v, f, buf, off, n, false);
}
static int64_t pipe_read_user(struct vnode *v, struct file *f, void *buf, uint64_t off, size_t n) {
    return pipe_read_common(v, f, buf, off, n, true);
}

static int64_t pipe_write_common(struct vnode *v, struct file *f, const void *buf, uint64_t off, size_t n, bool user) {
    struct pipe *p = v->priv;
    const uint8_t *in = buf;
    size_t done = 0;
    while (done < n) {
        uint64_t fl = irq_save();
        if (p->readers == 0) {
            irq_restore(fl);
            return done ? (int64_t)done : -N_ERR_CLOSED;
        }
        if(current_task->killed){
            irq_restore(fl);
            return done?(int64_t)done:-N_ERR_STOPPED;
        }
        if(n-done>SIZE_MAX-p->count){irq_restore(fl);return done?(int64_t)done:-N_ERR_INVALID;}
        size_t committed=0;int error=0;
        /* Grow only by the segment receiving actual bytes. Existing owners
           never move. At most one segment allocation and the former 16 KiB
           copy quantum occur per IRQ boundary, even for a large VIDEO.
           Writes within that quantum retain their original atomic boundary. */
        while(done<n&&committed<PIPE_SEGMENT_SIZE){
            struct pipe_segment *s=p->tail;
            if(s->write_at==PIPE_SEGMENT_SIZE){
                struct pipe_segment *next=pipe_segment_new();
                if(!next){error=-N_ERR_NO_MEMORY;break;}
                s->next=next;p->tail=s=next;
            }
            size_t put=MIN(n-done,MIN(PIPE_SEGMENT_SIZE-s->write_at,PIPE_SEGMENT_SIZE-committed));
            if (user && !user_ok(in+done, put)) { error=-N_ERR_BAD_ADDRESS; break; }
            memcpy(s->bytes+s->write_at,in+done,put);
            s->write_at+=put;
            p->count += put;
            done += put;
            committed+=put;
        }
        if(committed){
            wq_wake_all(&p->rq);
            vfs_poll_notify(v);
        }
        irq_restore(fl);
        if(error)return done?(int64_t)done:error;
    }
    return done;
}

static int64_t pipe_write(struct vnode *v, struct file *f, const void *buf, uint64_t off, size_t n) {
    return pipe_write_common(v, f, buf, off, n, false);
}
static int64_t pipe_write_user(struct vnode *v, struct file *f, const void *buf, uint64_t off, size_t n) {
    return pipe_write_common(v, f, buf, off, n, true);
}

static bool pipe_can_read(struct vnode *v, struct file *f) {
    struct pipe *p = v->priv;
    return p->count > 0 || p->writers == 0;
}

static bool pipe_can_write(struct vnode *v, struct file *f) {
    struct pipe *p = v->priv;
    /* Full current storage is not backpressure: the next write can grow it.
       poll must allow that attempt, without allocating or guessing OOM here.
       A closed reader is writable readiness for the real N_ERR_CLOSED result. */
    return p->count < SIZE_MAX || p->readers == 0;
}

static bool pipe_hangup(struct vnode *v,struct file *f) {
    struct pipe *p=v->priv;
    return (f->flags&O_ACCMODE)==O_RDONLY?p->writers==0:p->readers==0;
}

static void pipe_close(struct vnode *v, struct file *f) {
    struct pipe *p = v->priv;
    uint64_t fl = irq_save();
    if ((f->flags & O_ACCMODE) == O_RDONLY) p->readers--;
    else p->writers--;
    wq_wake_all(&p->rq);
    wq_wake_all(&p->wq);
    vfs_poll_notify(v);
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
    .read_user = pipe_read_user,
    .write_user = pipe_write_user,
    .can_read = pipe_can_read,
    .can_write = pipe_can_write,
    .hangup = pipe_hangup,
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
        return -N_ERR_NO_MEMORY;
    }
    strlcpy(v->name, "pipe", sizeof v->name);
    v->type = VT_PIPE;
    v->ops = &pipe_ops;
    v->priv = p;
    v->unlinked = true; /* freed when the last end closes */
    p->readers = 1;
    p->writers = 1;
    *rd = vfs_open_vnode(v, O_RDONLY);
    if(!*rd){pipe_release(v);*wr=NULL;return -N_ERR_NO_MEMORY;}
    *wr = vfs_open_vnode(v, O_WRONLY);
    if(!*wr){vfs_close(*rd);*rd=NULL;return -N_ERR_NO_MEMORY;}
    return 0;
}

/* Named native rendezvous. Only the registry/index is new: each duplex end
 * owns the existing dynamically segmented pipe directions. No path lookup,
 * network socket family, fixed client capacity or parent-process dependency. */
struct channel_end {struct file *rx,*tx;};
struct channel_pending {struct channel_pending *next;struct file *server;};
struct channel_listener {
    struct channel_listener *next;
    struct channel_pending *head,*tail;
    uint64_t hash;
    size_t key_length;
    bool published;
    struct vnode *vn;
    char key[];
};
enum {CHANNEL_BUCKETS=64};
static struct channel_listener *channel_names[CHANNEL_BUCKETS];

static uint64_t channel_hash(const char *key,size_t length) {
    uint64_t h=14695981039346656037ull;
    for(size_t i=0;i<length;i++)h=(h^(unsigned char)key[i])*1099511628211ull;
    return h;
}
static struct channel_listener *channel_find(const char *key,size_t length,uint64_t hash) {
    for(struct channel_listener *l=channel_names[hash&(CHANNEL_BUCKETS-1)];l;l=l->next)
        if(l->hash==hash && l->key_length==length && !memcmp(l->key,key,length))return l;
    return NULL;
}
static int64_t channel_read(struct vnode *v,struct file *f,void *buffer,uint64_t offset,size_t n) {
    struct channel_end *end=v->priv;
    return pipe_read(end->rx->vn,f,buffer,offset,n);
}
static int64_t channel_write(struct vnode *v,struct file *f,const void *buffer,uint64_t offset,size_t n) {
    struct channel_end *end=v->priv;
    return pipe_write(end->tx->vn,f,buffer,offset,n);
}
static int64_t channel_read_user(struct vnode *v,struct file *f,void *buffer,uint64_t offset,size_t n) {
    struct channel_end *end=v->priv;
    return pipe_read_user(end->rx->vn,f,buffer,offset,n);
}
static int64_t channel_write_user(struct vnode *v,struct file *f,const void *buffer,uint64_t offset,size_t n) {
    struct channel_end *end=v->priv;
    return pipe_write_user(end->tx->vn,f,buffer,offset,n);
}
static void channel_poll_register(struct vnode *v,struct file *f,struct poll_wait *wait) {
    (void)f;struct channel_end *end=v->priv;
    /* Either directional peer can hang up, independently of requested IN/OUT. */
    vfs_poll_register(wait,end->rx->vn);
    vfs_poll_register(wait,end->tx->vn);
}
static bool channel_can_read(struct vnode *v,struct file *f) {
    struct channel_end *end=v->priv;return pipe_can_read(end->rx->vn,f);
}
static bool channel_can_write(struct vnode *v,struct file *f) {
    struct channel_end *end=v->priv;return pipe_can_write(end->tx->vn,f);
}
static bool channel_hangup(struct vnode *v,struct file *f) {
    (void)f;struct channel_end *end=v->priv;
    struct pipe *rx=end->rx->vn->priv,*tx=end->tx->vn->priv;
    return rx->writers==0 || tx->readers==0;
}
static void channel_close(struct vnode *v,struct file *f) {
    (void)f;struct channel_end *end=v->priv;
    vfs_close(end->rx);vfs_close(end->tx);
}
static void channel_release(struct vnode *v) {kfree(v->priv);kfree(v);}
static struct vnode_ops channel_ops={
    .read=channel_read,.write=channel_write,.can_read=channel_can_read,
    .read_user=channel_read_user,.write_user=channel_write_user,.poll_register=channel_poll_register,
    .can_write=channel_can_write,.hangup=channel_hangup,.close=channel_close,.release=channel_release,
};
/* Transfers the two pipe references only on success. */
static struct file *channel_end_new(struct file *rx,struct file *tx) {
    struct vnode *v=kzalloc(sizeof *v);struct channel_end *end=kmalloc(sizeof *end);
    if(!v || !end){kfree(v);kfree(end);return NULL;}
    *end=(struct channel_end){rx,tx};v->type=VT_CHANNEL;v->ops=&channel_ops;v->priv=end;v->unlinked=true;
    strlcpy(v->name,"channel",sizeof v->name);
    struct file *f=vfs_open_vnode(v,O_RDWR|O_NONBLOCK);
    if(!f){kfree(end);kfree(v);}return f;
}
static int channel_pair(struct file **client,struct file **server) {
    struct file *r0,*w0,*r1,*w1;int e=pipe_create(&r0,&w0);
    if(e)return e;
    e=pipe_create(&r1,&w1);
    if(e){vfs_close(r0);vfs_close(w0);return e;}
    *client=channel_end_new(r1,w0);
    if(!*client){vfs_close(r0);vfs_close(w0);vfs_close(r1);vfs_close(w1);return -N_ERR_NO_MEMORY;}
    *server=channel_end_new(r0,w1);
    if(!*server){vfs_close(*client);*client=NULL;vfs_close(r0);vfs_close(w1);return -N_ERR_NO_MEMORY;}
    return 0;
}
static bool listener_can_read(struct vnode *v,struct file *f) {
    (void)f;struct channel_listener *l=v->priv;return l->head!=NULL;
}
static bool listener_can_write(struct vnode *v,struct file *f) {(void)v;(void)f;return false;}
static void listener_close(struct vnode *v,struct file *f) {
    (void)f;struct channel_listener *l=v->priv;uint64_t flags=irq_save();
    if(l->published) {
        struct channel_listener **at=&channel_names[l->hash&(CHANNEL_BUCKETS-1)];
        while(*at && *at!=l)at=&(*at)->next;
        if(*at==l)*at=l->next;l->published=false;
    }
    struct channel_pending *pending=l->head;l->head=l->tail=NULL;
    vfs_poll_notify(v);irq_restore(flags);
    /* A pending server reference is an ordinary owner. Closing it propagates
     * both directional EOF/N_ERR_CLOSED and wakes clients, even after owner death. */
    while(pending){struct channel_pending *next=pending->next;vfs_close(pending->server);kfree(pending);pending=next;}
}
static struct vnode_ops listener_ops={
    .can_read=listener_can_read,.can_write=listener_can_write,.close=listener_close,.release=channel_release,
};
int channel_listen(const char *key,size_t length,struct file **listener) {
    if(!length || length>SIZE_MAX-sizeof(struct channel_listener))return -N_ERR_INVALID;
    uint64_t hash=channel_hash(key,length);
    struct vnode *v=kzalloc(sizeof *v);
    struct channel_listener *l=kzalloc(sizeof *l+length);
    if(!v || !l){kfree(v);kfree(l);return -N_ERR_NO_MEMORY;}
    l->hash=hash;l->key_length=length;l->vn=v;memcpy(l->key,key,length);
    v->type=VT_CHANNEL;v->ops=&listener_ops;v->priv=l;v->unlinked=true;
    strlcpy(v->name,"channel-listener",sizeof v->name);
    struct file *f=vfs_open_vnode(v,O_RDWR|O_NONBLOCK);
    if(!f){kfree(l);kfree(v);return -N_ERR_NO_MEMORY;}
    uint64_t flags=irq_save();
    if(channel_find(key,length,hash)){irq_restore(flags);vfs_close(f);return -N_ERR_EXISTS;}
    unsigned bucket=hash&(CHANNEL_BUCKETS-1);
    l->next=channel_names[bucket];channel_names[bucket]=l;l->published=true;
    irq_restore(flags);*listener=f;return 0;
}
int channel_connect(const char *key,size_t length,struct file **client) {
    if(!length)return -N_ERR_INVALID;
    uint64_t hash=channel_hash(key,length);uint64_t flags=irq_save();
    struct channel_listener *l=channel_find(key,length,hash);
    if(!l){irq_restore(flags);return -N_ERR_NOT_FOUND;}
    struct channel_pending *pending=kmalloc(sizeof *pending);
    if(!pending){irq_restore(flags);return -N_ERR_NO_MEMORY;}
    struct file *server;int e=channel_pair(client,&server);
    if(e){kfree(pending);irq_restore(flags);return e;}
    pending->next=NULL;pending->server=server;
    if(l->tail)l->tail->next=pending;else l->head=pending;l->tail=pending;
    vfs_poll_notify(l->vn);irq_restore(flags);return 0;
}
int channel_accept(struct file *listener,struct file **server) {
    if(!listener || listener->vn->ops!=&listener_ops)return -N_ERR_INVALID;
    struct channel_listener *l=listener->vn->priv;uint64_t flags=irq_save();
    struct channel_pending *pending=l->head;
    if(!pending){irq_restore(flags);return -N_ERR_NOT_READY;}
    l->head=pending->next;if(!l->head)l->tail=NULL;
    *server=pending->server;kfree(pending);irq_restore(flags);return 0;
}
