#include "kernel.h"
#include "dev/blk.h"
#include "sys/thread.h"
#include "sys/wait.h"

/* The devices, in the order their drivers found them (a USB disk at any time), never removed.
   devs_lock serializes the appends; a reader reads ndevs with acquire, then the entries below it,
   each whole before ndevs counts it. */
#define MAX_BLK 16
static struct spinlock devs_lock;
static struct blkdev *devs[MAX_BLK];
static int ndevs;

void blk_register(struct blkdev *d) {
    if (!d->block) d->block = 512;
    uint64_t f = spin_lock(&devs_lock);
    int n = ndevs;
    if (n < MAX_BLK) {
        d->index = n;
        devs[n] = d;
        __atomic_store_n(&ndevs, n + 1, __ATOMIC_RELEASE);
    }
    spin_unlock(&devs_lock, f);
    if (n == MAX_BLK) return;
    kprintf("blk: %s: %s, %lu MiB, %u-byte blocks%s\n", d->name, d->model, d->sectors / 2048, d->block,
            d->persist ? "" : ", no write cache");
}

int blk_count(void) { return __atomic_load_n(&ndevs, __ATOMIC_ACQUIRE); }

struct blkdev *blk_get(int i) { return i >= 0 && i < blk_count() ? devs[i] : NULL; }

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
            if (!whole && move(ctx, false, first, n, bounce) < 0) return -N_ERR_IO;
            memcpy(bounce + off, p, take);
            if (move(ctx, true, first, n, bounce) < 0) return -N_ERR_IO;
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

/* Nothing written since a persist that ended began: the medium has it all already. Persists on
   other CPUs at once each flush what they found unpersisted; the device's driver puts them one
   after the other. */
int blk_persist(struct blkdev *d) {
    if (!d->persist) return 0;
    enter();
    uint64_t upto = __atomic_load_n(&d->written, __ATOMIC_SEQ_CST);
    int r = 0;
    if (__atomic_load_n(&d->persisted, __ATOMIC_SEQ_CST) < upto && !(r = d->persist(d))) {
        uint64_t was = __atomic_load_n(&d->persisted, __ATOMIC_SEQ_CST);
        while (was < upto && !__atomic_compare_exchange_n(&d->persisted, &was, upto, false, __ATOMIC_SEQ_CST,
                                                          __ATOMIC_SEQ_CST)) {}
    }
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
