/* Watches (sys/watch.h). */
#include "kernel.h"
#include "mm/heap.h"
#include "sys/watch.h"

/* A handle's own state: the owner's count when it last took (atomic: the app's threads may take
   on one handle at once). */
struct opening {
    uint64_t seen;
};

static struct watched *of(struct object *o) { return (struct watched *)((char *)o - offsetof(struct watched, object)); }

/* It only reads. */
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
    s->seen = __atomic_load_n(&w->changes, __ATOMIC_SEQ_CST);
    struct handle *h = handle_open(&w->object, 0);
    if (!h) {
        kfree(s);
        return -N_ERR_NO_MEMORY;
    }
    h->priv = s;
    return handle_install(h); /* a failure closes h, and watch_close frees s */
}

void watched_changed(struct watched *w) {
    __atomic_add_fetch(&w->changes, 1, __ATOMIC_SEQ_CST);
    object_notify(&w->object);
}

void watched_gone(struct watched *w) {
    __atomic_store_n(&w->gone, true, __ATOMIC_SEQ_CST);
    object_notify(&w->object);
}

int64_t watch_take(int index) {
    struct handle *h = handle_get(index);
    if (!h) return -N_ERR_NO_HANDLE;
    int64_t r = -N_ERR_WRONG_KIND;
    if (handle_is(h, &watch_ops)) {
        struct watched *w = of(h->object);
        struct opening *s = h->priv;
        /* gone before the count: every change said before it went is in the count read after */
        bool gone = __atomic_load_n(&w->gone, __ATOMIC_SEQ_CST);
        uint64_t seen = __atomic_load_n(&s->seen, __ATOMIC_SEQ_CST), now;
        /* a take of another thread on this handle meanwhile has its changes, this one the rest */
        do now = __atomic_load_n(&w->changes, __ATOMIC_SEQ_CST);
        while (!__atomic_compare_exchange_n(&s->seen, &seen, now, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
        uint64_t n = now - seen;
        r = !n && gone ? -N_ERR_CLOSED : (int64_t)MIN(n, (uint64_t)N_WATCH_CHANGES_MAX);
    }
    handle_close(h);
    return r;
}
