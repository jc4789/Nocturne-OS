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

/* Native message channels (docs/nocturne-native-abi.md §4). A pair shares one
 * link; each end receives from its own queue and sends into the peer's. A
 * message keeps its boundary and may carry one document or shared-page handle.
 * Messages are kept in kernel memory as sent: there is no fixed capacity. */
struct message {
    struct message *next;
    struct file *attached;
    size_t size;
    uint8_t bytes[];
};
struct message_link {
    struct message *head[2], **tail[2];
    bool open[2];          /* end i still has a handle */
    struct vnode *end[2];  /* until that end's vnode is released */
    struct wait_queue wait[2];
};
static struct vnode_ops message_ops;

static int message_side(struct vnode *v) { return (int)v->mode; }

static void message_free(struct message *m) {
    while (m) {
        struct message *next = m->next;
        if (m->attached) vfs_close(m->attached);
        kfree(m);
        m = next;
    }
}

bool message_channel_is(struct file *f) { return f && f->vn->ops == &message_ops; }

int64_t message_channel_send(struct file *f, const void *udata, size_t n, struct file *attach) {
    struct vnode *v = f->vn;
    struct message_link *l = v->priv;
    int peer = !message_side(v);
    if (n > SIZE_MAX - sizeof(struct message)) return -N_ERR_TOO_LARGE;
    struct message *m = kmalloc(sizeof *m + n);
    if (!m) return -N_ERR_NO_MEMORY;
    m->next = NULL;
    m->size = n;
    m->attached = NULL;
    /* Copy in fixed pieces: a large message never makes one long
       non-preemptible copy, and a sibling unmapping the source is caught. */
    for (size_t done = 0; done < n;) {
        size_t piece = MIN(n - done, (size_t)PIPE_SEGMENT_SIZE);
        uint64_t fl = irq_save();
        bool ok = user_ok((const uint8_t *)udata + done, piece);
        if (ok) memcpy(m->bytes + done, (const uint8_t *)udata + done, piece);
        irq_restore(fl);
        if (!ok) { kfree(m); return -N_ERR_BAD_ADDRESS; }
        done += piece;
    }
    uint64_t fl = irq_save();
    if (!l->open[peer]) { irq_restore(fl); kfree(m); return -N_ERR_CLOSED; }
    if (attach) m->attached = vfs_dup(attach);
    *l->tail[peer] = m;
    l->tail[peer] = &m->next;
    wq_wake_all(&l->wait[peer]);
    vfs_poll_notify(l->end[peer]);
    irq_restore(fl);
    return (int64_t)n;
}

/* peek reports the next message's size without taking it. A message larger
 * than cap stays queued and reports N_ERR_TOO_LARGE. */
int64_t message_channel_receive(struct file *f, void *ubuf, size_t cap, bool wait, bool peek,
                                struct file **attached) {
    struct vnode *v = f->vn;
    struct message_link *l = v->priv;
    int side = message_side(v);
    *attached = NULL;
    for (;;) {
        uint64_t fl = irq_save();
        struct message *m = l->head[side];
        if (m) {
            if (peek) { irq_restore(fl); return (int64_t)m->size; }
            if (m->size > cap) { irq_restore(fl); return -N_ERR_TOO_LARGE; }
            if (!user_ok_w(ubuf, m->size)) { irq_restore(fl); return -N_ERR_BAD_ADDRESS; }
            l->head[side] = m->next;
            if (!l->head[side]) l->tail[side] = &l->head[side];
            m->next = NULL;
            irq_restore(fl);
            /* Taken off the queue: copy out in pieces without holding the
               guard. Only a sibling unmapping the buffer meanwhile loses it. */
            for (size_t done = 0; done < m->size;) {
                size_t piece = MIN(m->size - done, (size_t)PIPE_SEGMENT_SIZE);
                uint64_t cf = irq_save();
                bool ok = user_ok_w((uint8_t *)ubuf + done, piece);
                if (ok) memcpy((uint8_t *)ubuf + done, m->bytes + done, piece);
                irq_restore(cf);
                if (!ok) { message_free(m); return -N_ERR_BAD_ADDRESS; }
                done += piece;
            }
            int64_t size = (int64_t)m->size;
            *attached = m->attached;
            m->attached = NULL;
            kfree(m);
            return size;
        }
        if (!l->open[!side]) { irq_restore(fl); return -N_ERR_CLOSED; }
        if (!wait) { irq_restore(fl); return -N_ERR_NOT_READY; }
        if (current_task->killed) { irq_restore(fl); return -N_ERR_STOPPED; }
        wq_wait(&l->wait[side]);
        irq_restore(fl);
    }
}

static bool message_can_read(struct vnode *v, struct file *f) {
    (void)f;
    struct message_link *l = v->priv;
    int side = message_side(v);
    return l->head[side] || !l->open[!side];
}
static bool message_can_write(struct vnode *v, struct file *f) {
    (void)v; (void)f;
    return true; /* sending only allocates; a closed peer reports N_ERR_CLOSED */
}
static bool message_hangup(struct vnode *v, struct file *f) {
    (void)f;
    struct message_link *l = v->priv;
    return !l->open[!message_side(v)];
}
static void message_close(struct vnode *v, struct file *f) {
    (void)f;
    struct message_link *l = v->priv;
    int side = message_side(v);
    uint64_t fl = irq_save();
    l->open[side] = false;
    /* Nobody can receive this side's queue any more: drop it now, so an
     * attached handle is not kept alive by a dead end. */
    struct message *dropped = l->head[side];
    l->head[side] = NULL;
    l->tail[side] = &l->head[side];
    wq_wake_all(&l->wait[!side]);
    wq_wake_all(&l->wait[side]);
    vfs_poll_notify(l->end[!side]);
    irq_restore(fl);
    message_free(dropped);
}
static void message_release(struct vnode *v) {
    struct message_link *l = v->priv;
    int side = message_side(v);
    uint64_t fl = irq_save();
    l->end[side] = NULL;
    bool last = !l->end[!side];
    irq_restore(fl);
    if (last) {
        message_free(l->head[0]);
        message_free(l->head[1]);
        kfree(l);
    }
    kfree(v);
}
static struct vnode_ops message_ops = {
    .can_read = message_can_read, .can_write = message_can_write, .hangup = message_hangup,
    .close = message_close, .release = message_release,
};

int message_channel_pair(struct file **a, struct file **b) {
    struct message_link *l = kzalloc(sizeof *l);
    struct vnode *v[2] = {kzalloc(sizeof(struct vnode)), kzalloc(sizeof(struct vnode))};
    if (!l || !v[0] || !v[1]) { kfree(l); kfree(v[0]); kfree(v[1]); return -N_ERR_NO_MEMORY; }
    for (int i = 0; i < 2; i++) {
        l->tail[i] = &l->head[i];
        l->open[i] = true;
        l->end[i] = v[i];
        strlcpy(v[i]->name, "channel", sizeof v[i]->name);
        v[i]->type = VT_CHANNEL;
        v[i]->mode = (uint32_t)i; /* which end */
        v[i]->ops = &message_ops;
        v[i]->priv = l;
        v[i]->unlinked = true;
    }
    *a = vfs_open_vnode(v[0], O_RDWR);
    *b = *a ? vfs_open_vnode(v[1], O_RDWR) : NULL;
    if (!*a || !*b) {
        /* An opened end is released through its file; the other by hand. */
        if (*a) { vfs_close(*a); *a = NULL; message_release(v[1]); }
        else { kfree(v[0]); kfree(v[1]); kfree(l); }
        return -N_ERR_NO_MEMORY;
    }
    return 0;
}
