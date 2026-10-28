/* Watches (sys/watch.h). */
#include "kernel.h"
#include "arch/cpu.h"
#include "mm/heap.h"
#include "sys/watch.h"

/* A handle's own state: the owner's count when it last took. */
struct opening {
    uint64_t seen;
};

static struct watched *of(struct object *o) { return (struct watched *)((char *)o - offsetof(struct watched, object)); }

/* Guard held or an AP read section: it only reads. */
static uint32_t watch_ready(struct handle *h) {
    struct watched *w = of(h->object);
    const struct opening *s = h->priv;
    uint32_t r = __atomic_load_n(&w->changes, __ATOMIC_RELAXED) != __atomic_load_n(&s->seen, __ATOMIC_RELAXED)
                     ? N_EVENT_RECEIVABLE : 0;
    return r | (__atomic_load_n(&w->gone, __ATOMIC_RELAXED) ? N_EVENT_CLOSED : 0);
}
static void watch_close(struct handle *h) { kfree(h->priv); }
static void watch_release(struct object *o) {
    struct watched *w = of(o);
    if (w->release) w->release(w);
}
static const struct object_ops watch_ops = {
    .kind = N_KIND_WATCH, .ready = watch_ready, .close = watch_close, .release = watch_release,
};

void watched_init(struct watched *w, void (*release)(struct watched *w)) {
    object_init(&w->object, &watch_ops, NULL);
    w->changes = 0;
    w->gone = false;
    w->release = release;
}

int64_t watched_open(struct watched *w) {
    struct opening *s = kzalloc(sizeof *s);
    if (!s) return -N_ERR_NO_MEMORY;
    uint64_t f = irq_save();
    s->seen = w->changes;
    irq_restore(f);
    struct handle *h = handle_open(&w->object, false);
    if (!h) {
        kfree(s);
        return -N_ERR_NO_MEMORY;
    }
    h->priv = s;
    return handle_install(h); /* a failure closes h, and watch_close frees s */
}

void watched_changed(struct watched *w) {
    uint64_t f = irq_save();
    __atomic_store_n(&w->changes, w->changes + 1, __ATOMIC_RELAXED);
    irq_restore(f);
    object_notify(&w->object);
}

void watched_gone(struct watched *w) {
    uint64_t f = irq_save();
    __atomic_store_n(&w->gone, true, __ATOMIC_RELAXED);
    irq_restore(f);
    object_notify(&w->object);
}

int64_t watch_take(int index) {
    struct handle *h = handle_get(index);
    if (!h) return -N_ERR_NO_HANDLE;
    int64_t r = -N_ERR_WRONG_KIND;
    if (handle_is(h, &watch_ops)) {
        struct watched *w = of(h->object);
        struct opening *s = h->priv;
        uint64_t f = irq_save();
        uint64_t n = w->changes - s->seen;
        __atomic_store_n(&s->seen, w->changes, __ATOMIC_RELAXED);
        r = !n && w->gone ? -N_ERR_CLOSED : (int64_t)MIN(n, (uint64_t)N_WATCH_CHANGES_MAX);
        irq_restore(f);
    }
    handle_close(h);
    return r;
}
