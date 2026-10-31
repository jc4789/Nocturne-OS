/* Address waits (sys/address_wait.h). A wait is keyed by the word's physical address, so the
   threads of one app and the apps that map the same shared page meet on it. Each key hashes to a
   bucket, a wait queue (sys/wait.h) whose lock guards the waiters of its words in the order they
   came: threads blocked on one (thread items) and event queues' watches of one (callback items,
   sys/events.c), each item's key its word. A wake walks its word's bucket alone, and takes the
   oldest of the word's waiters, whichever kind: a thread is made ready, a watch's callback is
   called there, under the bucket's lock. The buckets are counted from the CPUs. The kernel reads
   the word through the direct map at its physical address, never at the app's: a sibling thread
   may take the page away meanwhile. */
#include "kernel.h"
#include "arch/topology.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "sys/address_wait.h"
#include "sys/clock.h"
#include "sys/process.h"
#include "sys/thread.h"
#include "sys/wait.h"

#define BUCKETS_PER_CPU 64

struct address_bucket {
    struct wait_queue queue;
} __attribute__((aligned(64)));

static struct address_bucket *buckets;
static unsigned bucket_bits;

static struct address_bucket *bucket_of(uint64_t phys) {
    return &buckets[(phys * 0x9E3779B97F4A7C15ull) >> (64 - bucket_bits)];
}

void address_wait_init(void) {
    uint64_t want = (uint64_t)BUCKETS_PER_CPU * topology.cpus;
    for (bucket_bits = 6; ((uint64_t)1 << bucket_bits) < want; bucket_bits++) {}
    buckets = kzalloc(sizeof *buckets << bucket_bits);
    if (!buckets) panic("address waits: no memory for %lu buckets", (uint64_t)1 << bucket_bits);
}

uint64_t address_word(volatile void *address, bool wide) {
    uint64_t va = (uint64_t)address;
    if ((va & (wide ? 7 : 3)) || va >= USER_TOP) return 0;
    struct vmm_space *space = process_current()->vmm_space;
    uint64_t phys = vmm_translate(space, va);
    if (!phys && vmm_demand_fault(space, va, false) > 0) phys = vmm_translate(space, va);
    return phys;
}
static uint64_t address_read(uint64_t phys, bool wide) {
    void *word = phys_to_virt(phys);
    return wide ? __atomic_load_n((volatile uint64_t *)word, __ATOMIC_SEQ_CST)
                : __atomic_load_n((volatile uint32_t *)word, __ATOMIC_SEQ_CST);
}

int address_wait(volatile void *address, uint64_t expected, bool wide, uint64_t deadline) {
    struct thread *t = thread_current();
    if (t->killed) return -N_ERR_STOPPED;
    uint64_t phys = address_word(address, wide);
    if (!phys) return -N_ERR_BAD_ADDRESS;
    /* Read and enrolled under the bucket's lock, which a waker holds while it looks: either it
       finds this thread or this thread reads its new value. */
    struct address_bucket *b = bucket_of(phys);
    uint64_t flags = spin_lock(&b->queue.lock);
    uint64_t current = address_read(phys, wide);
    if (current != expected || clock_now_ns() >= deadline) {
        spin_unlock(&b->queue.lock, flags);
        return current != expected ? -N_ERR_NOT_READY : -N_ERR_TIMEOUT;
    }
    t->wait.item.key = phys;
    bool awake = wq_wait_locked_until(&b->queue, &b->queue.lock, deadline);
    t->wait.item.key = 0;
    irq_restore(flags);
    if (t->killed) return -N_ERR_STOPPED;
    return awake ? 0 : -N_ERR_TIMEOUT;
}

/* The oldest waiters of the physical word first, whether blocked or watching. */
static unsigned phys_wake(uint64_t phys, unsigned count) {
    struct address_bucket *b = bucket_of(phys);
    unsigned done = 0;
    uint64_t flags = spin_lock(&b->queue.lock);
    for (struct wait_item *i = b->queue.head, *next; i && (!count || done < count); i = next) {
        next = i->next;
        if (i->key != phys) continue;
        wq_wake_item(&b->queue, i);
        done++;
    }
    spin_unlock(&b->queue.lock, flags);
    return done;
}

int address_wake(volatile uint32_t *address, unsigned count) {
    uint64_t va = (uint64_t)address;
    uint64_t phys = va < USER_TOP && !(va & 3) ? vmm_translate(process_current()->vmm_space, va) : 0;
    return phys ? (int)phys_wake(phys, count) : 0;
}

/* Read and enrolled under the bucket's lock, as address_wait. */
bool address_watch_arm(struct wait_item *item, uint64_t phys, uint32_t expected) {
    struct address_bucket *b = bucket_of(phys);
    item->key = phys;
    uint64_t flags = spin_lock(&b->queue.lock);
    bool waits = address_read(phys, false) == expected;
    if (waits) wq_add(&b->queue, item);
    spin_unlock(&b->queue.lock, flags);
    return waits;
}

bool address_watch_disarm(struct wait_item *item) {
    struct address_bucket *b = bucket_of(item->key);
    uint64_t flags = spin_lock(&b->queue.lock);
    bool waiting = wq_remove(&b->queue, item);
    spin_unlock(&b->queue.lock, flags);
    return waiting;
}
