/* 装置の木と、その読出し（io/io.h）。 */
#include "kernel.h"
#include "io/internal.h"
#include "mm/heap.h"
#include "sys/syscall.h"
#include "sys/watch.h"

struct spinlock io_lock;
struct io_device io_root;
static uint32_t io_ids;
static uint64_t io_ledger_bytes;
static struct wait_queue io_holds_wq;
static struct watched io_changed;

void io_ledger_changed(void) { watched_changed(&io_changed); }

void io_device_init(struct io_device *d, uint32_t bus, const struct io_bus_ops *ops) {
    memset(d, 0, sizeof *d);
    d->bus = bus;
    d->ops = ops;
    d->node = -1;
    d->dma_bits = 32;
    io_prop_number(d, N_HAL_PROP_BUS, bus);
}

static struct n_hal_prop *prop_grow(struct io_device *d, uint32_t key, uint32_t size, uint64_t value) {
    struct n_hal_prop *grown = krealloc(d->props, (d->prop_count + 1) * sizeof *grown);
    if (!grown) {
        d->incomplete = true;
        return NULL;
    }
    d->props = grown;
    grown[d->prop_count] = (struct n_hal_prop){key, size, value};
    return &grown[d->prop_count++];
}

void io_prop_number(struct io_device *d, uint32_t key, uint64_t value) { prop_grow(d, key, 0, value); }

void io_prop_bytes(struct io_device *d, uint32_t key, const void *bytes, uint32_t size) {
    uint8_t *grown = krealloc(d->prop_bytes, d->prop_bytes_used + size);
    if (!grown) {
        d->incomplete = true;
        return;
    }
    d->prop_bytes = grown;
    if (!prop_grow(d, key, size, d->prop_bytes_used)) return;
    memcpy(grown + d->prop_bytes_used, bytes, size);
    d->prop_bytes_used += size;
}

void io_prop_text(struct io_device *d, uint32_t key, const char *text) {
    io_prop_bytes(d, key, text, (uint32_t)strlen(text));
}

void io_location(struct io_device *d, const char *text) { strlcpy(d->location, text, sizeof d->location); }

void io_device_discard(struct io_device *d) {
    ASSERT(!d->listed);
    kfree(d->props);
    kfree(d->prop_bytes);
    kfree(d->resources);
    d->props = NULL;
    d->prop_bytes = NULL;
    d->resources = NULL;
    d->prop_count = d->prop_bytes_used = d->resource_count = 0;
}

/* 根の下の深さ優先。 */
struct io_device *io_walk_next(struct io_device *d) {
    if (d->first_child) return d->first_child;
    while (d && !d->next) d = d->parent;
    return d ? d->next : NULL;
}

static size_t record_bytes(const struct io_device *d) {
    return ALIGN_UP(sizeof(struct n_hal_device) + d->prop_count * sizeof(struct n_hal_prop) +
                        d->offer_count * sizeof(struct n_hal_offer) + d->depend_count * sizeof(uint32_t) + d->prop_bytes_used,
                    8);
}

void io_init(void) {
    io_device_init(&io_root, N_BUS_ROOT, NULL);
    io_root.id = ++io_ids;
    io_root.listed = true;
    io_ledger_bytes = record_bytes(&io_root);
    watched_init(&io_changed, NULL);
}

void io_publish(struct io_device *parent, struct io_device *d) {
    if (!parent) parent = &io_root;
    uint64_t f = spin_lock(&io_lock);
    d->id = ++io_ids;
    d->parent = parent;
    d->prev = parent->last_child;
    *(parent->last_child ? &parent->last_child->next : &parent->first_child) = d;
    parent->last_child = d;
    d->listed = true;
    io_ledger_bytes += record_bytes(d);
    if (d->incomplete) {
        d->state = N_HAL_FAILED;
        d->reason = -N_ERR_NO_MEMORY;
    }
    spin_unlock(&io_lock, f);
    io_ledger_changed();
    if (!d->incomplete) io_bind_request(d);
}

void io_remove(struct io_device *d) {
    io_busy_begin();
    uint64_t f = spin_lock(&io_lock);
    __atomic_store_n(&d->gone, true, __ATOMIC_RELEASE);
    pending_forget(d);
    spin_unlock(&io_lock, f);
    mutex_lock(&d->bind);
    io_claim_end_device(d);
    if (d->driver) {
        ASSERT(d->driver->detach);
        d->driver->detach(d);
        io_release(d);
    }
    mutex_unlock(&d->bind);
    /* 動き出した attach の thread と、呼出しの中の claim は、まだ記録を読む */
    f = spin_lock(&io_lock);
    while (d->holds) {
        wq_wait_locked(&io_holds_wq, &io_lock);
        spin_take(&io_lock);
    }
    ASSERT(!d->first_child); /* 出した者が先に消す */
    *(d->prev ? &d->prev->next : &d->parent->first_child) = d->next;
    *(d->next ? &d->next->prev : &d->parent->last_child) = d->prev;
    io_ledger_bytes -= record_bytes(d);
    d->listed = false;
    /* 依存する側の鎖の d を外す：列を作るときに辿らない */
    struct io_depend_entry *freed = NULL;
    for (struct io_device *o = &io_root; o; o = io_walk_next(o)) {
        for (struct io_depend_entry **p = &o->depends; *p;) {
            struct io_depend_entry *e = *p;
            if (e->on != d) {
                p = &e->next;
                continue;
            }
            *p = e->next;
            o->depend_count--;
            io_ledger_bytes -= sizeof(uint32_t);
            e->next = freed;
            freed = e;
        }
    }
    struct io_offer_entry *offers = d->offers;
    struct io_depend_entry *depends = d->depends;
    d->offers = NULL;
    d->depends = NULL;
    spin_unlock(&io_lock, f);
    io_busy_end();
    while (freed) {
        struct io_depend_entry *next = freed->next;
        kfree(freed);
        freed = next;
    }
    while (offers) {
        struct io_offer_entry *next = offers->next;
        kfree(offers);
        offers = next;
    }
    while (depends) {
        struct io_depend_entry *next = depends->next;
        kfree(depends);
        depends = next;
    }
    kfree(d->props);
    kfree(d->prop_bytes);
    kfree(d->resources);
    io_ledger_changed();
}

void io_depends(struct io_device *d, struct io_device *on) {
    struct io_depend_entry *e = kmalloc(sizeof *e);
    if (!e) {
        kprintf("hal: no memory for %s's dependency\n", d->location);
        return;
    }
    e->on = on;
    uint64_t f = spin_lock(&io_lock);
    e->next = d->depends;
    d->depends = e;
    d->depend_count++;
    if (d->listed) io_ledger_bytes += sizeof(uint32_t);
    spin_unlock(&io_lock, f);
    io_ledger_changed();
}

/* 入るだけ写す。UTF-8 の字の途中では切らない。 */
void io_name(struct io_device *d, const char *name) {
    uint64_t f = spin_lock(&io_lock);
    size_t cap = sizeof d->name;
    if (strlcpy(d->name, name, cap) >= cap) {
        size_t end = cap - 1;
        while (end && ((uint8_t)name[end] & 0xC0) == 0x80) end--;
        d->name[end] = 0;
    }
    spin_unlock(&io_lock, f);
    io_ledger_changed();
}

void io_failed(struct io_device *d, int reason) {
    uint64_t f = spin_lock(&io_lock);
    d->state = N_HAL_FAILED;
    d->reason = reason;
    spin_unlock(&io_lock, f);
    io_ledger_changed();
}

void io_recovered(struct io_device *d) {
    uint64_t f = spin_lock(&io_lock);
    bool was = d->state == N_HAL_FAILED;
    if (was) {
        d->state = N_HAL_RUNNING;
        d->reason = 0;
    }
    spin_unlock(&io_lock, f);
    if (was) io_ledger_changed();
}

void io_offer(struct io_device *d, uint32_t kind, uint32_t instance, const char *label) {
    struct io_offer_entry *e = kzalloc(sizeof *e);
    if (!e) {
        kprintf("hal: no memory for an offer of %s\n", d->location);
        return;
    }
    e->offer.kind = kind;
    e->offer.instance = instance;
    strlcpy(e->offer.label, label, sizeof e->offer.label);
    uint64_t f = spin_lock(&io_lock);
    e->next = d->offers;
    d->offers = e;
    d->offer_count++;
    if (d->listed) io_ledger_bytes += sizeof(struct n_hal_offer);
    spin_unlock(&io_lock, f);
    io_ledger_changed();
}

void io_offer_withdraw(struct io_device *d, uint32_t kind, uint32_t instance) {
    struct io_offer_entry *gone = NULL;
    uint64_t f = spin_lock(&io_lock);
    for (struct io_offer_entry **p = &d->offers; *p; p = &(*p)->next) {
        if ((*p)->offer.kind != kind || (*p)->offer.instance != instance) continue;
        gone = *p;
        *p = gone->next;
        d->offer_count--;
        if (d->listed) io_ledger_bytes -= sizeof(struct n_hal_offer);
        break;
    }
    spin_unlock(&io_lock, f);
    if (gone) {
        kfree(gone);
        io_ledger_changed();
    }
}

struct io_device *io_find(uint32_t id) {
    uint64_t f = spin_lock(&io_lock);
    struct io_device *d = &io_root;
    while (d && d->id != id) d = io_walk_next(d);
    if (d && d->gone) d = NULL;
    if (d) d->holds++;
    spin_unlock(&io_lock, f);
    return d;
}

struct io_device *io_find_firmware(const struct firmware_node *f) {
    if (!f) return NULL;
    uint64_t flags = spin_lock(&io_lock);
    struct io_device *d = io_root.first_child;
    while (d && d->firmware != f) d = io_walk_next(d);
    if (d && d->gone) d = NULL;
    if (d) d->holds++;
    spin_unlock(&io_lock, flags);
    return d;
}

struct firmware_node *io_parent_firmware(const struct io_device *d) {
    return d->parent && d->parent != &io_root ? d->parent->firmware : NULL;
}

void io_put(struct io_device *d) {
    uint64_t f = spin_lock(&io_lock);
    ASSERT(d->holds);
    /* 起こす列は記録の外にある。io_remove はこの錠を取るまで戻らない */
    if (!--d->holds) wq_wake_all(&io_holds_wq);
    spin_unlock(&io_lock, f);
}

/* 記録は頭の中の長さも bytes に入れるので、丸めた分は読み手に見えない。 */
static char *record_write(const struct io_device *d, char *at) {
    struct n_hal_device *r = (struct n_hal_device *)at;
    memset(r, 0, sizeof *r);
    r->bytes = (uint32_t)record_bytes(d);
    r->id = d->id;
    r->parent = d->parent ? d->parent->id : 0;
    r->bus = d->bus;
    r->state = d->state;
    r->reason = d->reason;
    r->prop_count = d->prop_count;
    r->offer_count = d->offer_count;
    r->depend_count = d->depend_count;
    r->byte_count = d->prop_bytes_used;
    memcpy(r->name, d->name, sizeof r->name);
    memcpy(r->location, d->location, sizeof r->location);
    memcpy(r->driver, d->driver_name, sizeof r->driver);
    char *p = (char *)(r + 1);
    memcpy(p, d->props, d->prop_count * sizeof(struct n_hal_prop));
    p += d->prop_count * sizeof(struct n_hal_prop);
    for (const struct io_offer_entry *e = d->offers; e; e = e->next) {
        memcpy(p, &e->offer, sizeof e->offer);
        p += sizeof e->offer;
    }
    for (const struct io_depend_entry *e = d->depends; e; e = e->next) {
        memcpy(p, &e->on->id, sizeof(uint32_t));
        p += sizeof(uint32_t);
    }
    if (d->prop_bytes_used) memcpy(p, d->prop_bytes, d->prop_bytes_used);
    return at + r->bytes;
}

/* プログラムへの写しは眠るので、錠の下で核の記憶へ全部を写す。一度の呼出しが見せるのはいつも一つの時点の全部で、
   入らないなら何も書かずに要る大きさを返す。 */
int64_t io_syscall_list(void *user_out, uint64_t bytes) {
    char *copy = NULL;
    size_t room = 0, total;
    for (;;) {
        uint64_t f = spin_lock(&io_lock);
        total = io_ledger_bytes;
        if (bytes < total) {
            spin_unlock(&io_lock, f);
            kfree(copy);
            return (int64_t)total;
        }
        if (total <= room) {
            char *at = copy;
            for (const struct io_device *d = &io_root; d; d = io_walk_next((struct io_device *)d)) at = record_write(d, at);
            ASSERT((size_t)(at - copy) == total);
            spin_unlock(&io_lock, f);
            break;
        }
        spin_unlock(&io_lock, f);
        kfree(copy);
        room = total;
        copy = kmalloc(room);
        if (!copy) return -N_ERR_NO_MEMORY;
    }
    int64_t r = copy_to_user(user_out, copy, total) ? -N_ERR_BAD_ADDRESS : (int64_t)total;
    kfree(copy);
    return r;
}

int64_t io_syscall_watch(void) { return watched_open(&io_changed); }
