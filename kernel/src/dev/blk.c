#include "kernel.h"
#include "dev/blk.h"
#include "mm/heap.h"
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

void blk_register(struct blkdev *d) {
    if (!d->block) d->block = 512;
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
            d->persist ? "" : ", no write cache");
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

static void enter(void) {
    uint64_t f = spin_lock(&gate_lock);
    while (holder && holder != thread_current()) {
        wq_wait_locked(&gate_wq, &gate_lock);
        spin_take(&gate_lock);
    }
    inflight++;
    spin_unlock(&gate_lock, f);
}

static void leave(void) {
    uint64_t f = spin_lock(&gate_lock);
    bool wake = !--inflight && holder;
    spin_unlock(&gate_lock, f);
    if (wake) wq_wake_all(&gate_wq);
}

void blk_busy_begin(void) { enter(); }
void blk_busy_end(void) { leave(); }

/* A failed request is counted, and the first few are logged: a dying disk must not flood the log.
   Drivers fail with -1 or a code of their own; callers see N_ERR_IO, or N_ERR_TIMEOUT. */
static int failed(struct blkdev *d, const char *what, uint64_t lba, uint32_t n, int r) {
    if (__atomic_fetch_add(&d->errors, 1, __ATOMIC_RELAXED) < 8)
        kprintf("blk: %s: %s of %u sectors at %lu failed (%d)\n", d->name, what, n, lba, r);
    return r == -N_ERR_TIMEOUT ? r : -N_ERR_IO;
}

int blk_read(struct blkdev *d, uint64_t lba, uint32_t n, void *buf) {
    enter();
    int r = d->read(d, lba, n, buf);
    leave();
    return r ? failed(d, "read", lba, n, r) : 0;
}

/* A write is counted once the device has it (or gave up on it, which may have changed the medium
   too); a persist covers the writes counted before it began. */
int blk_write(struct blkdev *d, uint64_t lba, uint32_t n, const void *buf) {
    enter();
    int r = d->write(d, lba, n, buf);
    __atomic_add_fetch(&d->written, 1, __ATOMIC_SEQ_CST);
    leave();
    return r ? failed(d, "write", lba, n, r) : 0;
}

int blk_by_blocks(struct blkdev *d, bool write, uint64_t lba, uint32_t count, uint8_t *p, uint8_t *bounce,
                  unsigned most, bool direct, blk_move_fn move, void *ctx) {
    if (lba + count > d->sectors) return -N_ERR_IO;
    unsigned per = d->block / 512;
    while (count) {
        uint64_t first = lba / per;
        unsigned off = (unsigned)(lba % per) * 512, want = count * 512u;
        unsigned n = (off + want + d->block - 1) / d->block;
        if (n > most) n = most;
        unsigned take = n * d->block - off;
        if (take > want) take = want;
        bool whole = !off && take == n * d->block;
        if (whole && direct) {
            if (move(ctx, write, first, n, p) < 0) return -N_ERR_IO;
        } else if (write) {
            /* another part of the same block being written is not written back as it was; a write
               of a whole block beside this one is the caller's to keep apart, as any two that overlap */
            if (!whole) mutex_lock(&d->partial);
            int r = whole ? 0 : move(ctx, false, first, n, bounce);
            if (r >= 0) {
                memcpy(bounce + off, p, take);
                r = move(ctx, true, first, n, bounce);
            }
            if (!whole) mutex_unlock(&d->partial);
            if (r < 0) return -N_ERR_IO;
        } else {
            if (move(ctx, false, first, n, bounce) < 0) return -N_ERR_IO;
            memcpy(p, bounce + off, take);
        }
        lba += take / 512;
        count -= take / 512;
        p += take;
    }
    return 0;
}

static void count_up_to(uint64_t *count, uint64_t upto) {
    uint64_t was = __atomic_load_n(count, __ATOMIC_SEQ_CST);
    while (was < upto &&
           !__atomic_compare_exchange_n(count, &was, upto, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {}
}

/* Nothing written since a persist that ended began: the medium has it all already. Persists on
   other CPUs at once each flush what they found unpersisted; the device's driver puts them one
   after the other. A flush covers the commands over before it began (NVMe 2.4, 7.2), which `ended`
   is read for. A loss of its cache is looked at after the flush, which may have waited for the
   device to come back. */
int blk_persist(struct blkdev *d) {
    if (!d->persist) return 0;
    enter();
    uint64_t upto = __atomic_load_n(&d->written, __ATOMIC_SEQ_CST);
    uint64_t ended = __atomic_load_n(&d->ended, __ATOMIC_SEQ_CST);
    int r = 0;
    if (__atomic_load_n(&d->persisted, __ATOMIC_SEQ_CST) < upto && !(r = d->persist(d))) {
        count_up_to(&d->persisted, upto);
        count_up_to(&d->flushed, ended);
    }
    if (!r && __atomic_load_n(&d->cache_lost, __ATOMIC_SEQ_CST)) r = -N_ERR_IO;
    leave();
    return r ? failed(d, "cache flush", 0, 0, r) : 0;
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
