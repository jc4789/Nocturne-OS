/* A latch: a reader-writer lock for the store's data in memory (common/store), held only while
   that data is looked at or changed, never across anything that may wait (a read from the medium,
   a sleep, a lock of the program the store runs in that sleeps). Many hold it shared, or one holds
   it alone; one that asks for it alone goes before readers who come after it, so writers are not
   starved. Waiting spins: the program calls the store with interrupts open (the kernel answers its
   CPU's calls meanwhile), never from an interrupt handler.

   One 32-bit word: the holder alone (HELD), how many wait to hold it alone, how many share it. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct latch {
    uint32_t word;
};

#define LATCH_HELD 0x80000000u
#define LATCH_WAITER 0x00010000u
#define LATCH_WAITERS 0x7FFF0000u
#define LATCH_READERS 0x0000FFFFu

static inline void latch_pause(void) { __builtin_ia32_pause(); }

static inline void latch_share(struct latch *l) {
    for (;;) {
        uint32_t w = __atomic_load_n(&l->word, __ATOMIC_RELAXED);
        if (!(w & (LATCH_HELD | LATCH_WAITERS)) &&
            __atomic_compare_exchange_n(&l->word, &w, w + 1, true, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return;
        latch_pause();
    }
}

static inline void latch_unshare(struct latch *l) { __atomic_fetch_sub(&l->word, 1, __ATOMIC_RELEASE); }

static inline void latch_take(struct latch *l) {
    __atomic_fetch_add(&l->word, LATCH_WAITER, __ATOMIC_RELAXED);
    for (;;) {
        uint32_t w = __atomic_load_n(&l->word, __ATOMIC_RELAXED);
        if (!(w & (LATCH_HELD | LATCH_READERS)) &&
            __atomic_compare_exchange_n(&l->word, &w, (w - LATCH_WAITER) | LATCH_HELD, true, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
            return;
        latch_pause();
    }
}

static inline void latch_give(struct latch *l) { __atomic_fetch_and(&l->word, ~LATCH_HELD, __ATOMIC_RELEASE); }

/* For checks of the store's own rules: whether anyone holds it alone. */
static inline bool latch_held(const struct latch *l) { return __atomic_load_n(&l->word, __ATOMIC_RELAXED) & LATCH_HELD; }
