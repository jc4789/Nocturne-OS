/* Spinlocks: the kernel's short exclusion between CPUs (and with interrupt handlers on the
   holder's own CPU, whose interrupts stay off while it holds the lock).

   A CPU waiting for a lock has its interrupts off, so it cannot take a TLB shootdown's IPI; it
   serves a shootdown waiting for it while it spins instead (vmm_tlb_poll), so that a holder
   that changes mappings never waits on a CPU that waits on it. Holders never sleep and never
   wait for another CPU in any other way.

   Taking one lock while holding another follows the order the locks' owners document. */
#pragma once
#include "arch/cpu.h"
#include "arch/smp.h"

void vmm_tlb_poll(void);

struct spinlock {
    uint32_t taken;
    uint32_t holder; /* CPU index + 1 while taken: a CPU taking a lock it holds stops at once */
};

static inline void spin_take(struct spinlock *l) {
    unsigned self = cpu_current_index() + 1;
    while (__atomic_exchange_n(&l->taken, 1, __ATOMIC_ACQUIRE)) {
        if (__atomic_load_n(&l->holder, __ATOMIC_RELAXED) == self) __builtin_trap();
        while (__atomic_load_n(&l->taken, __ATOMIC_RELAXED)) {
            vmm_tlb_poll();
            pause();
        }
    }
    __atomic_store_n(&l->holder, self, __ATOMIC_RELAXED);
}
static inline void spin_give(struct spinlock *l) {
    __atomic_store_n(&l->holder, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&l->taken, 0, __ATOMIC_RELEASE);
}
/* Interrupts off until spin_unlock, which gives back the state it found. */
static inline uint64_t spin_lock(struct spinlock *l) {
    uint64_t flags = irq_save();
    spin_take(l);
    return flags;
}
static inline void spin_unlock(struct spinlock *l, uint64_t flags) {
    spin_give(l);
    irq_restore(flags);
}
static inline bool spin_held(const struct spinlock *l) {
    return __atomic_load_n(&l->holder, __ATOMIC_RELAXED) == cpu_current_index() + 1;
}
