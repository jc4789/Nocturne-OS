/* Event queues (docs/nocturne-native-abi.md §6): one wait for many handles,
   timers and memory words. Reports are level-triggered: a handle that stays
   ready is reported by every wait. */
#include "kernel.h"
#include "arch/cpu.h"
#include "sys/sched.h"
#include "sys/syscall.h"
#include "sys/proc.h"
#include "sys/events.h"
#include "fs/vfs.h"
#include "mm/heap.h"
#include "dev/timer.h"

/* A watch names an open file without holding it: the file's last close ends
   the watch, so watching never keeps a channel end or an app record open. */
struct event_watch {
    struct event_watch *queue_next, **queue_prev;
    struct event_watch *file_next;
    struct file *file;
    uint32_t interest;
    uint64_t token;
};
struct event_timer {
    struct event_timer *next;
    uint64_t due, period, token;
};
struct event_queue {
    struct event_watch *watches;
    struct event_timer *timers;
    struct vnode *addresses; /* made at the first memory-word watch */
    struct vnode *vn;
};

/* BSP IRQ guard held for all list changes below. */
static void watch_unlink(struct event_watch *w) {
    *w->queue_prev = w->queue_next;
    if (w->queue_next) w->queue_next->queue_prev = w->queue_prev;
    struct event_watch **at = &w->file->watchers;
    while (*at && *at != w) at = &(*at)->file_next;
    if (*at) *at = w->file_next;
}

void events_file_closed(struct file *f) {
    while (f->watchers) {
        struct event_watch *w = f->watchers;
        watch_unlink(w);
        kfree(w);
    }
}

static uint32_t watch_ready(struct event_watch *w) {
    struct file *f = w->file;
    struct vnode *v = f->vn;
    uint32_t ready = 0;
    if (proc_app_is(v)) {
        if (vfs_can_read(f)) ready |= N_EVENT_APP_ENDED;
    } else {
        if (vfs_can_read(f)) ready |= N_EVENT_RECEIVABLE;
        if (vfs_can_write(f)) ready |= N_EVENT_SENDABLE;
        if (v->ops && v->ops->hangup && v->ops->hangup(v, f)) ready |= N_EVENT_CLOSED;
        if (v->type == VT_FILE && vfs_save_known(f)) ready |= N_EVENT_SAVED;
    }
    return ready & w->interest;
}

static bool queue_can_read(struct vnode *v, struct file *f) { (void)v; (void)f; return false; }

static void queue_release(struct vnode *v) {
    struct event_queue *q = v->priv;
    uint64_t flags = irq_save();
    while (q->watches) {
        struct event_watch *w = q->watches;
        watch_unlink(w);
        kfree(w);
    }
    struct event_timer *timers = q->timers;
    q->timers = NULL;
    irq_restore(flags);
    while (timers) {
        struct event_timer *next = timers->next;
        kfree(timers);
        timers = next;
    }
    vnode_unref(q->addresses);
    kfree(q);
    kfree(v);
}

static struct vnode_ops queue_ops = {.can_read = queue_can_read, .release = queue_release};

struct vnode *events_new(void) {
    struct vnode *v = kzalloc(sizeof *v);
    struct event_queue *q = kzalloc(sizeof *q);
    if (!v || !q) { kfree(v); kfree(q); return NULL; }
    q->vn = v;
    strlcpy(v->name, "events", sizeof v->name);
    v->type = VT_EVENTS;
    v->ops = &queue_ops;
    v->priv = q;
    v->unlinked = true;
    return v;
}

bool events_is(struct file *f) { return f && f->vn->ops == &queue_ops; }

size_t events_watch_size(void) { return sizeof(struct event_watch); }

/* Watch f (looked up under the caller's guard, which is still held). A second
   watch of the same file with the same token changes its interest. */
int events_watch_locked(struct file *queue, struct file *f, uint32_t interest, uint64_t token,
                        struct event_watch **spare) {
    struct event_queue *q = queue->vn->priv;
    if (events_is(f)) return -N_ERR_WRONG_KIND; /* queues do not nest */
    for (struct event_watch *w = q->watches; w; w = w->queue_next)
        if (w->file == f && w->token == token) {
            w->interest = interest;
            vfs_poll_notify(queue->vn);
            return 0;
        }
    struct event_watch *w = *spare;
    *spare = NULL;
    *w = (struct event_watch){.queue_next = q->watches, .queue_prev = &q->watches,
                              .file_next = f->watchers, .file = f, .interest = interest, .token = token};
    if (q->watches) q->watches->queue_prev = &w->queue_next;
    q->watches = w;
    f->watchers = w;
    vfs_poll_notify(queue->vn); /* a waiting sibling thread rescans */
    return 0;
}

int events_watch_address(struct file *queue, volatile uint32_t *address, uint32_t expected, uint64_t token) {
    struct event_queue *q = queue->vn->priv;
    if (!q->addresses) {
        struct vnode *a = address_watch_queue_new();
        if (!a) return -N_ERR_NO_MEMORY;
        vnode_ref(a);
        uint64_t flags = irq_save();
        if (!q->addresses) { q->addresses = a; a = NULL; }
        irq_restore(flags);
        vnode_unref(a);
    }
    int r = address_watch_add(q->addresses, address, expected, token);
    if (!r) vfs_poll_notify(queue->vn);
    return r;
}

int events_timer(struct file *queue, uint64_t ms, bool repeat, uint64_t token) {
    struct event_queue *q = queue->vn->priv;
    if (repeat && !ms) return -N_ERR_INVALID;
    struct event_timer *t = kmalloc(sizeof *t);
    if (!t) return -N_ERR_NO_MEMORY;
    t->due = uptime_ms() + ms;
    t->period = repeat ? ms : 0;
    t->token = token;
    uint64_t flags = irq_save();
    t->next = q->timers;
    q->timers = t;
    vfs_poll_notify(queue->vn);
    irq_restore(flags);
    return 0;
}

/* 0 nothing had the token; 1 removed before its event; 2 its memory-word
   event had already been queued (and is now withdrawn). */
int events_unwatch(struct file *queue, uint64_t token) {
    struct event_queue *q = queue->vn->priv;
    int result = 0;
    struct event_timer *dropped = NULL;
    uint64_t flags = irq_save();
    for (struct event_watch *w = q->watches, *next; w; w = next) {
        next = w->queue_next;
        if (w->token != token) continue;
        watch_unlink(w);
        kfree(w);
        result = 1;
    }
    for (struct event_timer **at = &q->timers; *at;) {
        if ((*at)->token != token) { at = &(*at)->next; continue; }
        struct event_timer *t = *at;
        *at = t->next;
        t->next = dropped;
        dropped = t;
        result = 1;
    }
    irq_restore(flags);
    while (dropped) { struct event_timer *next = dropped->next; kfree(dropped); dropped = next; }
    if (q->addresses) {
        int a = address_watch_remove(q->addresses, token);
        if (a > result) result = a;
    }
    return result;
}

#define EVENTS_BATCH 64

int64_t events_wait(struct file *queue, struct n_event *uout, size_t max, int64_t timeout_ms) {
    struct event_queue *q = queue->vn->priv;
    if (!max) return -N_ERR_INVALID;
    if (max > EVENTS_BATCH) max = EVENTS_BATCH; /* the rest stays for the next wait */
    struct n_event out[EVENTS_BATCH];
    uint64_t tokens[EVENTS_BATCH];
    uint64_t deadline = timeout_ms > 0 ? uptime_ms() + (uint64_t)timeout_ms : 0;
    struct poll_wait wait = {0};
    int64_t result;
    for (;;) {
        uint64_t fl = irq_save();
        uint64_t generation = wait.generation;
        uint64_t now = uptime_ms(), next_due = UINT64_MAX;
        size_t n = 0;
        bool block = timeout_ms != 0;
        for (struct event_timer **at = &q->timers; *at;) {
            struct event_timer *t = *at;
            if (t->due <= now && n < max) {
                out[n++] = (struct n_event){.token = t->token, .events = N_EVENT_TIMER};
                if (!t->period) { *at = t->next; kfree(t); continue; }
                /* keep the cadence; a late wait does not report the missed ticks */
                t->due += t->period;
                if (t->due <= now) t->due = now + t->period;
            }
            if (t->due < next_due) next_due = t->due;
            at = &t->next;
        }
        /* A memory-word event is reported once; handles stay ready and are
           reported again, so many ready handles never hold one back. */
        if (q->addresses && n < max) {
            size_t got = address_watch_take(q->addresses, tokens, max - n);
            for (size_t i = 0; i < got; i++) out[n++] = (struct n_event){.token = tokens[i], .events = N_EVENT_ADDRESS};
            if (!got && block) vfs_poll_register(&wait, q->addresses);
        }
        for (struct event_watch *w = q->watches; w && n < max; w = w->queue_next) {
            uint32_t ready = watch_ready(w);
            if (ready) out[n++] = (struct n_event){.token = w->token, .events = ready};
            else if (block) vfs_poll_file(&wait, w->file);
        }
        if (block) vfs_poll_register(&wait, queue->vn);
        bool expired = timeout_ms > 0 && now >= deadline;
        if (n || !block || expired || wait.error) {
            result = n ? (int64_t)n : wait.error ? wait.error : 0;
            if (n) {
                if (!user_ok_w(uout, n * sizeof *uout)) result = -N_ERR_BAD_ADDRESS;
                else memcpy(uout, out, n * sizeof *uout);
            }
            irq_restore(fl);
            break;
        }
        if (current_task->killed) { irq_restore(fl); result = -N_ERR_STOPPED; break; }
        if (generation != wait.generation) { irq_restore(fl); vfs_poll_cleanup(&wait); continue; }
        uint64_t until = timeout_ms > 0 ? deadline : UINT64_MAX;
        if (next_due < until) until = next_due;
        if (until == UINT64_MAX) wq_wait(&wait.wq);
        else if (until > now) wq_wait_timeout(&wait.wq, until - now);
        irq_restore(fl);
        vfs_poll_cleanup(&wait);
    }
    vfs_poll_cleanup(&wait);
    return result;
}
