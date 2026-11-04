#include "kernel.h"
#include "dev/blk.h"
#include "hal/hal.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sys/clock.h"
#include "sys/thread.h"
#include "sys/wait.h"

/* The devices, in the order their drivers found them (a USB disk at any time), never removed, any
   number of them. devs_lock (a mutex: an append may allocate) serializes the appends. The table
   grows by doubling into a new one, published before ndevs counts its new entry; a reader reads
   ndevs with acquire, then the table, whose every entry below ndevs is there. An old table stays,
   since a reader may still be in it: all of them together are at most the size of the last. */
static struct mutex devs_lock;
static struct blkdev **devs;
static int ndevs, room;

void blk_register(struct hal_device *hal, struct blkdev *d) {
    ASSERT(d->ops && d->sectors);
    if (!d->block) d->block = 512;
    if (!d->segment_align) d->segment_align = 512;
    d->node = hal->node;
    mutex_lock(&devs_lock);
    int n = ndevs;
    if (n == room) {
        int grown = room ? 2 * room : 8;
        struct blkdev **table = kmalloc((size_t)grown * sizeof *table);
        if (!table) {
            mutex_unlock(&devs_lock);
            kprintf("blk: %s: no memory to list it\n", d->name);
            return;
        }
        if (n) memcpy(table, devs, (size_t)n * sizeof *table);
        __atomic_store_n(&devs, table, __ATOMIC_RELEASE);
        room = grown;
    }
    d->index = n;
    devs[n] = d;
    __atomic_store_n(&ndevs, n + 1, __ATOMIC_RELEASE);
    mutex_unlock(&devs_lock);
    kprintf("blk: %s: %s, %lu MiB, %u-byte blocks%s\n", d->name, d->model, d->sectors / 2048, d->block,
            d->has_cache ? "" : ", no write cache");
    if (hal) hal_offer(hal, N_OFFER_DISK, (uint32_t)d->index, d->name);
}

int blk_count(void) { return __atomic_load_n(&ndevs, __ATOMIC_ACQUIRE); }

struct blkdev *blk_get(int i) {
    return i >= 0 && i < blk_count() ? __atomic_load_n(&devs, __ATOMIC_ACQUIRE)[i] : NULL;
}

/* The power gate: holder (the one thread that reaches the disks while it is set) and inflight
   (the requests under way), under gate_lock. A request waits for holder, and blk_hold for
   inflight, in gate_wq, looking under the lock and enrolled before it is given back. The lock
   comes before gate_wq's. */
static struct spinlock gate_lock;
static struct wait_queue gate_wq;
static struct thread *holder;
static unsigned inflight;

static void enter(unsigned n) {
    uint64_t f = spin_lock(&gate_lock);
    while (holder && holder != thread_current()) {
        wq_wait_locked(&gate_wq, &gate_lock);
        spin_take(&gate_lock);
    }
    inflight += n;
    spin_unlock(&gate_lock, f);
}

static void leave(void) {
    uint64_t f = spin_lock(&gate_lock);
    bool wake = !--inflight && holder;
    spin_unlock(&gate_lock, f);
    if (wake) wq_wake_all(&gate_wq);
}

void blk_hold(void) {
    uint64_t f = spin_lock(&gate_lock);
    holder = thread_current();
    while (inflight) {
        wq_wait_locked(&gate_wq, &gate_lock);
        spin_take(&gate_lock);
    }
    spin_unlock(&gate_lock, f);
}

void blk_release(void) {
    uint64_t f = spin_lock(&gate_lock);
    holder = NULL;
    spin_unlock(&gate_lock, f);
    wq_wake_all(&gate_wq);
}

struct thread *blk_holder(void) {
    uint64_t f = spin_lock(&gate_lock);
    struct thread *t = holder;
    spin_unlock(&gate_lock, f);
    return t;
}

void blk_write_given(struct blkdev *d) { __atomic_add_fetch(&d->given, 1, __ATOMIC_SEQ_CST); }

void blk_write_ended(struct blkdev *d) { __atomic_add_fetch(&d->ended, 1, __ATOMIC_SEQ_CST); }

void blk_cache_lost(struct blkdev *d) {
    if (__atomic_load_n(&d->flushed, __ATOMIC_SEQ_CST) < __atomic_load_n(&d->given, __ATOMIC_SEQ_CST))
        __atomic_store_n(&d->cache_lost, true, __ATOMIC_SEQ_CST);
}

static void count_up_to(uint64_t *count, uint64_t upto) {
    uint64_t was = __atomic_load_n(count, __ATOMIC_SEQ_CST);
    while (was < upto &&
           !__atomic_compare_exchange_n(count, &was, upto, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {}
}

/* ---- the end of a request ---- */
void blk_complete(struct blk_request *r, int status) { r->done(r, status); }

/* A failed request is counted, and the first few are logged: a dying disk must not flood the log.
   A driver fails with a code of its own; the caller sees N_ERR_IO, N_ERR_TIMEOUT or N_ERR_NO_MEMORY. */
static int failed(struct blk_request *r, int status) {
    struct blkdev *d = r->device;
    if (__atomic_fetch_add(&d->errors, 1, __ATOMIC_RELAXED) < 8)
        kprintf("blk: %s: %s of %u sectors at %lu failed (%d)\n", d->name,
                r->op == BLK_READ ? "read" : r->op == BLK_WRITE ? "write" : "cache flush", r->sectors, r->lba, status);
    return status == -N_ERR_TIMEOUT || status == -N_ERR_NO_MEMORY ? status : -N_ERR_IO;
}

/* A request as the caller gave it is over, however many parts it had: a write is counted once the
   device has it (or gave up on it, which may have changed the medium too); a persist that ended
   covers the writes counted before it began, and fails if the device lost its cache after. */
static void request_over(struct blk_request *r, int status) {
    struct blkdev *d = r->device;
    if (r->op == BLK_WRITE) {
        __atomic_add_fetch(&d->written, 1, __ATOMIC_SEQ_CST);
    } else if (r->op == BLK_PERSIST && !status) {
        count_up_to(&d->persisted, r->written_at);
        count_up_to(&d->flushed, r->ended_at);
        if (__atomic_load_n(&d->cache_lost, __ATOMIC_SEQ_CST)) status = -N_ERR_IO;
    }
    if (status) status = failed(r, status);
    void (*done)(struct blk_request *, int) = r->done_to;
    leave();
    done(r, status);
}

/* ---- waiting for a request in a thread ---- */
/* The word lives on the waiter's stack, which it may leave as soon as the word moves: the wake is
   on a queue that is always there, picked by the word's address. */
#define SYNC_QUEUES 64
static struct wait_queue sync_wq[SYNC_QUEUES];

struct sync_wait {
    uint64_t seq;
    int status;
};

static struct wait_queue *sync_queue(const struct sync_wait *w) { return &sync_wq[((uintptr_t)w >> 6) % SYNC_QUEUES]; }

static void sync_done(struct blk_request *r, int status) {
    struct sync_wait *w = r->context;
    struct wait_queue *q = sync_queue(w);
    w->status = status;
    __atomic_add_fetch(&w->seq, 1, __ATOMIC_RELEASE);
    wq_wake_all(q);
}

static int sync_wait(struct sync_wait *w) {
    while (!__atomic_load_n(&w->seq, __ATOMIC_ACQUIRE)) wq_wait_unless(sync_queue(w), &w->seq, 0, CLOCK_NEVER);
    return w->status;
}

/* ---- regular requests ---- */
static uint32_t request_bytes(const struct blk_request *r) {
    uint64_t sum = 0;
    for (uint32_t i = 0; i < r->segment_count; i++) sum += r->segments[i].bytes;
    return sum > UINT32_MAX ? UINT32_MAX : (uint32_t)sum;
}

/* Whole blocks, every segment aligned as the device wants: what a driver can move as it is. */
static bool shaped(const struct blkdev *d, const struct blk_request *r) {
    unsigned per = d->block / 512;
    if (r->lba % per || r->sectors % per) return false;
    for (uint32_t i = 0; i < r->segment_count; i++)
        if (r->segments[i].phys % d->segment_align || r->segments[i].bytes % d->segment_align) return false;
    return true;
}

static bool joined(const struct blkdev *d, uint64_t end, uint64_t next) {
    return !d->segment_boundary || (!(end % d->segment_boundary) && !(next % d->segment_boundary));
}

static bool fits(const struct blkdev *d, const struct blk_request *r) {
    if (d->max_sectors && r->sectors > d->max_sectors) return false;
    if (d->max_segments && r->segment_count > d->max_segments) return false;
    for (uint32_t i = 1; i < r->segment_count; i++)
        if (!joined(d, r->segments[i - 1].phys + r->segments[i - 1].bytes, r->segments[i].phys)) return false;
    return true;
}

/* A place in a request's segments. */
struct cursor {
    uint32_t index;
    uint32_t used; /* bytes of segments[index] taken */
};

/* The next part from c: its bytes, within the device's limits, ending on a block; its segments
   counted in *count. 0 when no part is possible (a limit is reached inside a block). With out, the
   segments written there and c moved on. */
static uint32_t part_cut(const struct blkdev *d, const struct blk_request *r, struct cursor *c, uint32_t *count,
                         struct blk_segment *out) {
    uint64_t limit = d->max_sectors ? (uint64_t)d->max_sectors * 512 / d->block * d->block : UINT32_MAX;
    uint32_t got = 0, n = 0;
    struct cursor at = *c;
    uint64_t end = 0;
    while (at.index < r->segment_count && got < limit) {
        const struct blk_segment *s = &r->segments[at.index];
        uint64_t start = s->phys + at.used;
        if (n && (n == d->max_segments || !joined(d, end, start))) break;
        uint32_t take = (uint32_t)MIN((uint64_t)s->bytes - at.used, limit - got);
        if (out) out[n] = (struct blk_segment){start, take};
        n++;
        got += take;
        end = start + take;
        at.used += take;
        if (at.used == s->bytes) {
            at.index++;
            at.used = 0;
        }
    }
    if (got % d->block) {
        /* ended inside a block: only the request's own end may */
        if (at.index < r->segment_count) return 0;
    }
    *count = n;
    if (out) *c = at;
    return got;
}

struct part {
    struct blk_request request;
    struct blk_segment segments[];
};

static void part_over(struct blk_request *p, int status) {
    struct blk_request *whole = p->parent;
    int none = 0;
    if (status) __atomic_compare_exchange_n(&whole->status, &none, status, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    kfree(p);
    if (__atomic_sub_fetch(&whole->parts, 1, __ATOMIC_ACQ_REL)) return;
    whole->done(whole, __atomic_load_n(&whole->status, __ATOMIC_ACQUIRE));
}

/* r in parts the device takes, put at the end of the chain *tail points to: false, nothing made,
   when it cannot be cut on blocks, or without memory (*memory). */
static bool split(const struct blkdev *d, struct blk_request *r, struct blk_request ***tail, bool *memory) {
    struct cursor c = {0, 0};
    uint32_t parts = 0, count, bytes;
    while (c.index < r->segment_count) {
        struct cursor next = c;
        if (!(bytes = part_cut(d, r, &next, &count, NULL))) return false;
        c = next;
        parts++;
    }
    struct part **made = kzalloc(parts * sizeof *made);
    if (!made) {
        *memory = true;
        return false;
    }
    c = (struct cursor){0, 0};
    uint64_t lba = r->lba;
    for (uint32_t i = 0; i < parts; i++) {
        struct cursor next = c;
        bytes = part_cut(d, r, &next, &count, NULL);
        struct part *p = kmalloc(sizeof *p + count * sizeof(struct blk_segment));
        if (!p) {
            for (uint32_t k = 0; k < i; k++) kfree(made[k]);
            kfree(made);
            *memory = true;
            return false;
        }
        part_cut(d, r, &c, &count, p->segments);
        p->request = (struct blk_request){.op = r->op, .lba = lba, .sectors = bytes / 512, .segments = p->segments,
                                          .segment_count = count, .done = part_over, .device = r->device,
                                          .parent = r};
        lba += bytes / 512;
        made[i] = p;
    }
    r->status = 0;
    __atomic_store_n(&r->parts, parts, __ATOMIC_RELEASE);
    for (uint32_t i = 0; i < parts; i++) {
        **tail = &made[i]->request;
        *tail = &made[i]->request.next;
    }
    kfree(made);
    return true;
}

/* ---- requests that are not whole blocks, or not aligned ---- */
/* Bytes between the request's segments, from `skip` bytes in, and buf. */
static void copy_segments(const struct blk_request *r, uint64_t skip, uint8_t *buf, size_t n, bool into_buf) {
    for (uint32_t i = 0; i < r->segment_count && n; i++) {
        const struct blk_segment *s = &r->segments[i];
        if (skip >= s->bytes) {
            skip -= s->bytes;
            continue;
        }
        size_t take = MIN((size_t)(s->bytes - skip), n);
        uint8_t *at = (uint8_t *)phys_to_virt(s->phys + skip);
        if (into_buf) memcpy(buf, at, take);
        else memcpy(at, buf, take);
        buf += take;
        n -= take;
        skip = 0;
    }
}

/* One regular request through the driver and waited for, which has no part in the gate or the
   counters: its caller's request has those. */
static int run_inside(struct blkdev *d, enum blk_op op, uint64_t lba, uint32_t sectors, uint64_t phys) {
    struct blk_segment seg = {phys, sectors * 512};
    struct sync_wait w = {0};
    struct blk_request r = {.op = op, .lba = lba, .sectors = sectors, .segments = &seg, .segment_count = 1,
                            .done = sync_done, .context = &w, .device = d};
    struct blk_request *chain = NULL, **tail = &chain;
    bool memory = false;
    if (fits(d, &r)) chain = &r;
    else if (!split(d, &r, &tail, &memory)) return memory ? -N_ERR_NO_MEMORY : -N_ERR_IO;
    d->ops->submit(d, chain);
    return sync_wait(&w);
}

/* A write of part of a block, or of memory the device cannot reach as it is, or a read of the same:
   each stretch of blocks goes through a buffer of our own. Another part of the same block being
   written is not written back as it was (partial); a write of a whole block beside this one is the
   caller's to keep apart, as any two that overlap. */
static int bounce_io(struct blkdev *d, struct blk_request *r) {
    unsigned per = d->block / 512;
    uint64_t pages = MAX((uint64_t)32, (uint64_t)d->block / PAGE_SIZE ? (uint64_t)d->block / PAGE_SIZE : 1);
    uint64_t bytes = pages * PAGE_SIZE, phys = pmm_alloc_run(pages, (unsigned)MAX(d->node, 0));
    if (!phys) {
        pages = MAX((uint64_t)1, (uint64_t)d->block / PAGE_SIZE);
        bytes = pages * PAGE_SIZE;
        if (!(phys = pmm_alloc_run(pages, 0))) return -N_ERR_NO_MEMORY;
    }
    uint8_t *buf = phys_to_virt(phys);
    uint64_t first = r->lba / per, end = (r->lba + r->sectors + per - 1) / per;
    uint64_t want_start = r->lba * 512, want_end = (r->lba + r->sectors) * 512;
    int status = 0;
    for (uint64_t at = first; at < end && !status;) {
        uint64_t n = MIN(end - at, bytes / d->block);
        uint64_t from = MAX(at * d->block, want_start), to = MIN((at + n) * d->block, want_end);
        bool whole = from == at * d->block && to == (at + n) * d->block;
        if (r->op == BLK_READ) {
            status = run_inside(d, BLK_READ, at * per, (uint32_t)(n * per), phys);
            if (!status) copy_segments(r, from - want_start, buf + (from - at * d->block), (size_t)(to - from), false);
        } else {
            if (!whole) mutex_lock(&d->partial);
            if (!whole) status = run_inside(d, BLK_READ, at * per, (uint32_t)(n * per), phys);
            if (!status) {
                copy_segments(r, from - want_start, buf + (from - at * d->block), (size_t)(to - from), true);
                status = run_inside(d, BLK_WRITE, at * per, (uint32_t)(n * per), phys);
            }
            if (!whole) mutex_unlock(&d->partial);
        }
        at += n;
    }
    pmm_free_contig(phys, pages);
    return status;
}

/* ---- submit ---- */
void blk_submit(struct blkdev *d, struct blk_request *chain) {
    unsigned n = 0;
    for (struct blk_request *r = chain; r; r = r->next) n++;
    if (!n) return;
    enter(n);
    struct blk_request *batch = NULL, **tail = &batch, *slow = NULL, **slow_tail = &slow;
    for (struct blk_request *r = chain, *next; r; r = next) {
        next = r->next;
        r->next = NULL;
        r->device = d;
        r->done_to = r->done;
        r->done = request_over;
        if (__atomic_load_n(&d->gone, __ATOMIC_ACQUIRE)) {
            request_over(r, -N_ERR_IO);
        } else if (r->op == BLK_PERSIST) {
            r->written_at = __atomic_load_n(&d->written, __ATOMIC_SEQ_CST);
            r->ended_at = __atomic_load_n(&d->ended, __ATOMIC_SEQ_CST);
            if (!d->has_cache || __atomic_load_n(&d->persisted, __ATOMIC_SEQ_CST) >= r->written_at)
                request_over(r, d->has_cache || !__atomic_load_n(&d->cache_lost, __ATOMIC_SEQ_CST) ? 0 : -N_ERR_IO);
            else
                *tail = r, tail = &r->next;
        } else if (r->lba + r->sectors > d->sectors || request_bytes(r) != r->sectors * 512 || !r->sectors) {
            request_over(r, -N_ERR_IO);
        } else if (!shaped(d, r)) {
            *slow_tail = r;
            slow_tail = &r->next;
        } else if (fits(d, r)) {
            *tail = r;
            tail = &r->next;
        } else {
            bool memory = false;
            if (!split(d, r, &tail, &memory)) request_over(r, memory ? -N_ERR_NO_MEMORY : -N_ERR_IO);
        }
    }
    if (batch) d->ops->submit(d, batch);
    for (struct blk_request *r = slow, *next; r; r = next) {
        next = r->next;
        r->next = NULL;
        request_over(r, __atomic_load_n(&d->gone, __ATOMIC_ACQUIRE) ? -N_ERR_IO : bounce_io(d, r));
    }
}

/* ---- a request from one thread ---- */
/* The physical segments of kernel memory at buf: the pages one after the other, joined where they
   are one run. segments is stack, or the heap if it holds too few (*heap). */
static struct blk_segment *segments_of(const void *buf, size_t bytes, struct blk_segment *stack, unsigned room,
                                       uint32_t *count, struct blk_segment **heap) {
    uint64_t va = (uint64_t)buf;
    unsigned pages = (unsigned)(((va & (PAGE_SIZE - 1)) + bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    struct blk_segment *out = stack;
    *heap = NULL;
    if (pages > room) {
        if (!(out = *heap = kmalloc(pages * sizeof *out))) return NULL;
    }
    uint32_t n = 0;
    while (bytes) {
        uint64_t phys = vmm_kernel_translate(va);
        uint32_t take = (uint32_t)MIN((uint64_t)bytes, PAGE_SIZE - (va & (PAGE_SIZE - 1)));
        if (!phys) {
            kfree(*heap);
            *heap = NULL;
            return NULL;
        }
        if (n && out[n - 1].phys + out[n - 1].bytes == phys) out[n - 1].bytes += take;
        else out[n++] = (struct blk_segment){phys, take};
        va += take;
        bytes -= take;
    }
    *count = n;
    return out;
}

static int run(struct blkdev *d, enum blk_op op, uint64_t lba, uint32_t n, const void *buf) {
    struct blk_segment stack[8], *heap;
    uint32_t count = 0;
    struct blk_segment *segs = NULL;
    if (op != BLK_PERSIST && !(segs = segments_of(buf, (size_t)n * 512, stack, ARRAY_SIZE(stack), &count, &heap)))
        return -N_ERR_NO_MEMORY;
    struct sync_wait w = {0};
    struct blk_request r = {.op = op, .lba = lba, .sectors = n, .segments = segs, .segment_count = count,
                            .done = sync_done, .context = &w};
    blk_submit(d, &r);
    int status = sync_wait(&w);
    if (op != BLK_PERSIST) kfree(heap);
    return status;
}

int blk_read(struct blkdev *d, uint64_t lba, uint32_t n, void *buf) { return run(d, BLK_READ, lba, n, buf); }

int blk_write(struct blkdev *d, uint64_t lba, uint32_t n, const void *buf) { return run(d, BLK_WRITE, lba, n, buf); }

int blk_persist(struct blkdev *d) { return run(d, BLK_PERSIST, 0, 0, NULL); }

/* ---- a device that does one thing at a time ---- */
struct blk_serial {
    struct blkdev *d;
    blk_transfer_fn transfer;
    struct spinlock lock;
    struct blk_request *head, **tail;
    struct wait_queue wq;
    bool stop;
};

static void serial_submit(struct blkdev *d, struct blk_request *chain) {
    struct blk_serial *s = d->serial;
    uint64_t f = spin_lock(&s->lock);
    *s->tail = chain;
    while (*s->tail) s->tail = &(*s->tail)->next;
    spin_unlock(&s->lock, f);
    wq_wake_all(&s->wq);
}

const struct blk_ops blk_serial_ops = {.submit = serial_submit};

static void serial_thread(void *arg) {
    struct blk_serial *s = arg;
    for (;;) {
        uint64_t f = spin_lock(&s->lock);
        while (!s->head && !s->stop) {
            wq_wait_locked(&s->wq, &s->lock);
            spin_take(&s->lock);
        }
        struct blk_request *r = s->head;
        if (r && !(s->head = r->next)) s->tail = &s->head;
        bool stop = s->stop;
        spin_unlock(&s->lock, f);
        if (!r) break;
        r->next = NULL;
        blk_complete(r, stop || __atomic_load_n(&s->d->gone, __ATOMIC_ACQUIRE) ? -N_ERR_IO : s->transfer(s->d, r));
    }
    kfree(s);
}

void blk_serial_start(struct blkdev *d, blk_transfer_fn transfer) {
    struct blk_serial *s = kzalloc(sizeof *s);
    ASSERT(s);
    s->d = d;
    s->transfer = transfer;
    s->tail = &s->head;
    d->serial = s;
    d->ops = &blk_serial_ops;
    ASSERT(kthread_create(d->name[0] ? d->name : "blk", serial_thread, s));
}

void blk_serial_stop(struct blkdev *d) {
    struct blk_serial *s = d->serial;
    d->serial = NULL;
    uint64_t f = spin_lock(&s->lock);
    s->stop = true;
    spin_unlock(&s->lock, f);
    wq_wake_all(&s->wq);
}
