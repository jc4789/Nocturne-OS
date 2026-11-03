/* The code of the entry image (kernel/src/sys/entry.c): the clocks, read without entering the
   kernel. It is built on its own, without relocations (kernel/entry/entry.ld), and every process
   maps the same frames; the clocks' data lies right after it in every process's map. It is called
   from the program's stack, which may be a Go goroutine's: it stays a leaf, with few locals. */
#include <stdint.h>
#include "clock_data.h"

extern const struct n_clock __clock_data __attribute__((visibility("hidden")));

/* The data's fields in force at one reading of this CPU's TSC (h: the header, when given), and the
   monotonic clock at it. A reading never goes back on a thread, nor after one it learned of from
   another thread: the kernel adds to each CPU's TSC what it reads behind the others. */
static inline __attribute__((always_inline)) uint64_t clock_read(struct n_clock *h) {
    const struct n_clock *c = &__clock_data;
    for (;;) {
        uint32_t seq = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE), lo, hi, cpu;
        if (seq & 1) { /* the kernel is writing it */
            __asm__ volatile("pause");
            continue;
        }
        /* RDTSCP waits for the load of seq before it; the LFENCE holds the fields' loads after it */
        __asm__ volatile("rdtscp; lfence" : "=a"(lo), "=d"(hi), "=c"(cpu) : : "memory");
        uint64_t base_tsc = __atomic_load_n(&c->base_tsc, __ATOMIC_RELAXED);
        uint64_t base_ns = __atomic_load_n(&c->base_ns, __ATOMIC_RELAXED);
        uint64_t mult = __atomic_load_n(&c->mult, __ATOMIC_RELAXED);
        uint32_t shift = __atomic_load_n(&c->shift, __ATOMIC_RELAXED);
        int64_t offset = cpu < __atomic_load_n(&c->cpus, __ATOMIC_RELAXED) ? __atomic_load_n(&c->offset[cpu], __ATOMIC_RELAXED) : 0;
        if (h) {
            h->asleep_ns = __atomic_load_n(&c->asleep_ns, __ATOMIC_RELAXED);
            h->wall_ns = __atomic_load_n(&c->wall_ns, __ATOMIC_RELAXED);
            h->wall_from = __atomic_load_n(&c->wall_from, __ATOMIC_RELAXED);
            h->wall_until = __atomic_load_n(&c->wall_until, __ATOMIC_RELAXED);
            h->wall_slew = __atomic_load_n(&c->wall_slew, __ATOMIC_RELAXED);
            h->wall_slew_mult = __atomic_load_n(&c->wall_slew_mult, __ATOMIC_RELAXED);
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&c->seq, __ATOMIC_RELAXED) != seq) continue;
        uint64_t t = ((uint64_t)hi << 32 | lo) - (uint64_t)offset;
        return t <= base_tsc ? base_ns : base_ns + (uint64_t)(((unsigned __int128)(t - base_tsc) * mult) >> shift);
    }
}

uint64_t entry_clock_monotonic(void) { return clock_read(0); }

uint64_t entry_clock_boot(void) {
    struct n_clock h;
    uint64_t t = clock_read(&h);
    return t + h.asleep_ns;
}

int64_t entry_clock_wall(void) {
    struct n_clock h;
    uint64_t t = clock_read(&h);
    int64_t wall = h.wall_ns + (int64_t)(t > h.wall_from ? t - h.wall_from : 0);
    if (h.wall_slew) {
        uint64_t until = t < h.wall_until ? t : h.wall_until, ran = until > h.wall_from ? until - h.wall_from : 0;
        uint64_t whole = h.wall_slew < 0 ? -(uint64_t)h.wall_slew : (uint64_t)h.wall_slew;
        uint64_t done = (uint64_t)(((unsigned __int128)ran * h.wall_slew_mult) >> 32);
        if (done > whole) done = whole;
        wall += h.wall_slew < 0 ? -(int64_t)done : (int64_t)done;
    }
    return wall;
}

uint64_t entry_clock_tsc_hz(void) { return __atomic_load_n(&__clock_data.tsc_hz, __ATOMIC_RELAXED); }

/* Where the kernel finds these, at the image's start: the order of N_ENTRY_CLOCK_* (abi.h). */
__asm__(".section .entry_head,\"a\"\n"
        ".long entry_clock_monotonic, entry_clock_boot, entry_clock_wall, entry_clock_tsc_hz\n"
        ".text\n");
