#pragma once
/* A set of CPUs by topology's index: a bit each, in 64-bit words, as many words as the CPUs at boot
   need (cpuset_words). Each word changes atomically, so CPUs may add and remove themselves at
   once; a reader sees each word as it was at some moment. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline size_t cpuset_words(unsigned cpus) { return ((size_t)cpus + 63) / 64; }

static inline bool cpuset_has(const uint64_t *set, unsigned cpu) {
    return __atomic_load_n(&set[cpu / 64], __ATOMIC_ACQUIRE) >> (cpu % 64) & 1;
}
static inline void cpuset_add(uint64_t *set, unsigned cpu) {
    __atomic_fetch_or(&set[cpu / 64], 1ULL << (cpu % 64), __ATOMIC_SEQ_CST);
}
static inline void cpuset_remove(uint64_t *set, unsigned cpu) {
    __atomic_fetch_and(&set[cpu / 64], ~(1ULL << (cpu % 64)), __ATOMIC_SEQ_CST);
}

/* The first CPU of set at cpu or after it, below cpus; cpus when there is none. */
static inline unsigned cpuset_next(const uint64_t *set, unsigned cpu, unsigned cpus) {
    while (cpu < cpus) {
        uint64_t w = __atomic_load_n(&set[cpu / 64], __ATOMIC_ACQUIRE) >> (cpu % 64);
        if (w) return cpu + (unsigned)__builtin_ctzll(w) < cpus ? cpu + (unsigned)__builtin_ctzll(w) : cpus;
        cpu = (cpu / 64 + 1) * 64;
    }
    return cpus;
}
#define cpuset_each(cpu, set, cpus) \
    for (unsigned cpu = cpuset_next((set), 0, (cpus)); cpu < (cpus); cpu = cpuset_next((set), cpu + 1, (cpus)))
