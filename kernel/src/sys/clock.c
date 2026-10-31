/* The clocks (sys/clock.h).

   The data. struct clock_data lives in pages of its own (clock_data_pages), written under
   clock_lock (a leaf) by a sequence count: odd while the fields change. A reader that finds it
   odd waits for the write to end, unless the write is its own CPU's (an NMI's code on top of it):
   then it reads `before` and `before_offset`, the fields and its offset as they were; so does
   every reader once the kernel has stopped (clock_stop), when a writer halted mid-write never
   ends it. Readers take no lock: any context may read, an NMI's too. The writer makes the odd
   count seen everywhere (MFENCE) before it reads the TSC its new fields start from (RDTSCP does
   not wait for earlier stores, SDM Vol. 2B), and a reader reads the TSC after its first load of
   the count and before its fields (RDTSCP waits for earlier loads; the LFENCE after it holds
   later ones back): so a reader that sees the count unchanged read its TSC while the fields it
   read were in force, and every new set of fields starts from the old ones' reading at its TSC.
   The clock never goes back on a CPU and never steps. (A reading from `before` is the old line's
   at a TSC the new one may already cover: it can lead the new line by the change of slope times
   the write's length, a fraction of a ns for the few stores of a write.)

   The rate. Stated, and in use from the first reading: on the hardware the P0 P-state's
   (arch/cpu.h: 200 MHz * FID / DID, exact), under a hypervisor the one it registers
   (clock_register_tsc_hz). A hypervisor's rate is used as it states it. The P0 rate is nominal:
   spread spectrum lowers it (PPR 56713, the glossary's CoreCOF) and the crystal is off by its
   tolerance, so on the hardware a rate reference measures what the TSC really counts. Its owner
   (the HPET's, dev/refclock.c) reads its count between two TSC readings (a sample: the count was
   taken in that span) and gives the sample to clock_rate_sample, which answers when it wants the
   next. A sample's TSC readings less their CPU's offset are on the clock's one count, whatever
   CPU took them. Two samples give the ratio of TSC counts to reference counts, and with it ns per
   TSC count. Its error is bounded by what the samples cannot tell: up to `half` of each sample's
   span on the TSC's side and the count's quantum (one count, and each reading's jitter) on the
   reference's, together within (h/T + q/R) * R/(R - q) of the true ratio (T TSC counts, R
   reference counts, h and q the two ends' sums). That bound is against the reference; the
   reference's own accuracy is its own (the HPET's: 500 ppm over any 1 ms, IA-PC HPET 1.0a 2.4.1).
   The samples come at windows from the first one (the anchor) that double. The first window is
   as long as makes each part of its bound under 2^-12, so a stated rate off by more than twice
   that (a spread spectrum's few thousand ppm) is caught in it, within a few ms where the
   reference takes a us to read. A window's rate replaces the one in use when it agrees with it
   within their two bounds and its own bound is smaller (a stated rate has none: the first
   window's replaces it); when it does not agree, the ratio itself moved (the crystals' drift):
   the rate over the last window alone is taken, and the windows grow again from there. They begin
   again on a new reference, after a sleep, and when a CPU's offset changes. Each new rate starts
   from the clock's present reading: the slope changes, the clock does not step; the alarms' wake
   devices are set again (alarm_clock_changed). The error in the clock's reading this leaves (each
   rate's bound times the time it was in use; the stated rate's, from the TSC's start, what the
   first window shows it off by, and that window's bound) is summed and logged with each new rate.
   Hardware without an HPET keeps the P0 rate as stated, and says so.

   The CPUs. Their TSCs are compared as they start (clock_cpus_compare), all at once: each reads
   its TSC against the highest reading any of them has published and publishes its own when
   higher. A reading below one published before it was taken is a TSC behind that one's by at
   least the difference (which the time a cache line takes to cross only shrinks); each CPU takes
   the most it saw behind, in blocks of readings that double until one shows no more, as what its
   readings are short by, and all compare again with it added, until a pass in which none is seen
   behind. The offsets are added in readings only (offset[]), and only ever raise them.

   Sleep. The monotonic clock stands still from clock_suspend to clock_resume: after it, it goes on
   from where it stopped (the TSC began again, or was not stopped and counted only what the RTC
   says ran). The RTC, read on both sides, proves a least time asleep, which the boot and wall
   clocks take; once the RTC's next second is found, what more it proves is added.

   The wall. From the wall-time references' words (clock_wall_proof): each proves the time at
   least so much by a moment, and the wall clock moves on by what that is beyond what they proved
   before. The RTC is one: read at boot (a reading proves the time at least that second), then its
   next second is found by probes at the middle of where it can be (an alarm each, shifted by
   whole seconds into the future). clock_set_wall and clock_adjust_wall take over from the
   references' word. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/smp.h"
#include "arch/spin.h"
#include "dev/refclock.h"
#include "mm/pmm.h"
#include "sys/alarm.h"
#include "sys/clock.h"
#include "sys/wait.h"

#define SHIFT 48             /* mult's fixed point: 2^-46 of a 4 GHz TSC's ns per count */
#define ONE_Q32 (1ull << 32) /* the bounds' unit: 2^-32 */
#define FIRST_SHIFT 12       /* the first window: each part of its bound under 2^-12 (244 ppm) */

static struct clock_data *data; /* NULL before clock_init */
static uint64_t data_first;
static size_t data_count;
static struct spinlock clock_lock;
static uint32_t writer;            /* the CPU index + 1 inside a write */
static struct clock_data before;   /* the header as it was before the write in progress */
static int64_t before_offset;      /* and the writer's own offset */
static bool stopped;               /* clock_stop: a write in progress may never end */

static enum { RATE_P0, RATE_REGISTERED } rate_kind; /* where the stated rate came from */
static const char *rate_source;
static struct {
    uint64_t hz;
    const char *source;
} registered; /* before clock_init */
static uint32_t wall_gen; /* each step or slew of the wall clock (under clock_lock) */

#define SET(field, v) __atomic_store_n(&data->field, (v), __ATOMIC_RELAXED)
#define GET(h, field) __atomic_load_n(&(h)->field, __ATOMIC_RELAXED)

static inline uint64_t sat_add(uint64_t a, uint64_t b) { return b >= CLOCK_NEVER - a ? CLOCK_NEVER : a + b; }

static inline uint64_t ns_at(uint64_t base_tsc, uint64_t base_ns, uint64_t mult, uint64_t t) {
    if (t <= base_tsc) return base_ns;
    return base_ns + (uint64_t)(((unsigned __int128)(t - base_tsc) * mult) >> SHIFT);
}

/* ns per count, times 2^SHIFT, of a count at num / den a second (1 MHz or more) */
static bool mult_of(uint64_t num, uint64_t den, uint64_t *mult) {
    return den && den <= UINT64_MAX / NS_PER_S && num / den >= 1000000 &&
           clock_muldiv(NS_PER_S * den, 1ull << SHIFT, num, mult);
}

static uint64_t hz_of(uint64_t mult) {
    uint64_t hz = 0;
    clock_muldiv(NS_PER_S, 1ull << SHIFT, mult, &hz);
    return hz;
}

static uint64_t ppb_of(uint64_t bound) {
    uint64_t ppb = 0;
    clock_muldiv(bound, NS_PER_S, ONE_Q32, &ppb);
    return ppb;
}

/* ---- reading ---- */

/* The header's fields in force at one reading of this CPU's TSC (all of them, or only the line's
   three), with this CPU's offset; the TSC less the offset is the clock's reading. A write in
   progress is waited out, unless it is this CPU's own (an NMI on top of it) or the kernel has
   stopped (clock_stop): then the fields as they were before it. */
static uint64_t consistent(const struct clock_data *d, struct clock_data *h, bool all, int64_t *offset) {
    for (;;) {
        uint32_t seq = __atomic_load_n(&d->seq, __ATOMIC_ACQUIRE), cpu;
        uint64_t raw = clock_tsc_read(&cpu);
        const struct clock_data *from = d;
        bool own = false;
        if (seq & 1) {
            own = __atomic_load_n(&writer, __ATOMIC_RELAXED) == cpu + 1;
            if (!own && !__atomic_load_n(&stopped, __ATOMIC_RELAXED)) {
                pause();
                continue;
            }
            from = &before;
        }
        h->base_tsc = GET(from, base_tsc);
        h->base_ns = GET(from, base_ns);
        h->mult = GET(from, mult);
        if (all) {
            h->rate_bound = GET(from, rate_bound);
            h->tsc_hz = GET(from, tsc_hz);
            h->asleep_ns = GET(from, asleep_ns);
            h->wall_ns = GET(from, wall_ns);
            h->wall_from = GET(from, wall_from);
            h->wall_until = GET(from, wall_until);
            h->wall_slew = GET(from, wall_slew);
            h->wall_slew_mult = GET(from, wall_slew_mult);
        }
        *offset = own ? before_offset : cpu < d->cpus ? __atomic_load_n(&d->offset[cpu], __ATOMIC_RELAXED) : 0;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (from == d && __atomic_load_n(&d->seq, __ATOMIC_RELAXED) != seq) continue;
        return raw;
    }
}

uint64_t clock_now_ns(void) {
    const struct clock_data *d = __atomic_load_n(&data, __ATOMIC_ACQUIRE);
    if (!d) return 0;
    struct clock_data h;
    int64_t offset;
    uint64_t raw = consistent(d, &h, false, &offset);
    return ns_at(h.base_tsc, h.base_ns, h.mult, raw - (uint64_t)offset);
}

/* A consistent copy of the header, and the monotonic clock at one TSC reading of this CPU. */
static uint64_t snapshot(struct clock_data *h) {
    int64_t offset;
    uint64_t raw = consistent(data, h, true, &offset);
    return ns_at(h->base_tsc, h->base_ns, h->mult, raw - (uint64_t)offset);
}

uint64_t clock_boot_ns(void) {
    if (!__atomic_load_n(&data, __ATOMIC_ACQUIRE)) return 0;
    struct clock_data h;
    uint64_t t = snapshot(&h);
    return t + h.asleep_ns;
}

uint64_t clock_tsc_hz(void) {
    if (!__atomic_load_n(&data, __ATOMIC_ACQUIRE)) return 0;
    struct clock_data h;
    snapshot(&h);
    return h.tsc_hz;
}

bool clock_tsc_at(uint64_t ns, uint64_t *tsc) {
    const struct clock_data *d = __atomic_load_n(&data, __ATOMIC_ACQUIRE);
    if (!d) return false;
    struct clock_data h;
    int64_t offset;
    consistent(d, &h, false, &offset);
    uint64_t ticks, t;
    /* the first TSC count at which the clock reads ns or more */
    if (!clock_muldiv_up(ns > h.base_ns ? ns - h.base_ns : 0, 1ull << SHIFT, h.mult, &ticks)) return false;
    if (__builtin_add_overflow(h.base_tsc, ticks, &t) || __builtin_add_overflow(t, (uint64_t)offset, &t))
        return false;
    *tsc = t;
    return true;
}

void clock_stop(void) { __atomic_store_n(&stopped, true, __ATOMIC_RELEASE); }

size_t clock_data_pages(uint64_t *first) {
    *first = data_first;
    return data_count;
}

/* ---- writing (clock_lock held, interrupts off) ---- */

/* An NMI may come between any two of these stores and read the clock on this CPU: it must find
   `before` and `writer` set whenever the count is odd, so each store is ordered after the one
   before it (release), the compiler's order being the CPU's for its own NMI. */
static void write_begin(void) {
    unsigned cpu = cpu_current_index();
    before = *data;
    before_offset = data->offset[cpu];
    __atomic_store_n(&writer, cpu + 1, __ATOMIC_RELEASE);
    __atomic_store_n(&data->seq, data->seq + 1, __ATOMIC_RELEASE);
    __asm__ volatile("mfence" ::: "memory");
}

static void write_end(void) {
    __atomic_store_n(&data->seq, data->seq + 1, __ATOMIC_RELEASE);
    __atomic_store_n(&writer, 0, __ATOMIC_RELEASE);
}

/* In a write: this CPU's TSC less its offset (*t) and the monotonic clock at it. */
static uint64_t write_now(uint64_t *t) {
    uint32_t cpu;
    uint64_t raw = clock_tsc_read(&cpu);
    *t = raw - (uint64_t)data->offset[cpu];
    return ns_at(data->base_tsc, data->base_ns, data->mult, *t);
}

/* From the clock's present reading on, it runs at mult, whose bound is bound. */
static void rate_set(uint64_t mult, uint32_t bound) {
    write_begin();
    uint64_t t, ns = write_now(&t);
    SET(base_tsc, t);
    SET(base_ns, ns);
    SET(mult, mult);
    SET(rate_bound, bound);
    SET(tsc_hz, hz_of(mult));
    write_end();
}

/* ---- the rate, against a rate reference ---- */

struct sample {
    uint64_t tsc, half; /* the clock's count (the TSC less its CPU's offset) in the middle of the
                           span the reference's count was taken in, and half the span */
    uint64_t count;
    uint32_t jitter;
};

/* The windows (clock_lock). */
static struct {
    const struct clock_rate_reference *ref; /* the one they are on; NULL: none yet */
    uint64_t ns_q32;      /* ns per count of it, times 2^32 */
    bool begin;           /* the next sample anchors them: after a sleep, or a CPU's offset changed */
    struct sample anchor, mark; /* the window's start; the last window's end */
    uint64_t window;      /* counts from the anchor to the next estimate */
    uint32_t bound;       /* the rate in use's (CLOCK_RATE_UNMEASURED: as stated) */
    uint64_t adopted_ns;  /* when it was taken (0: from the TSC's start) */
    uint64_t error_ns;    /* the clock's reading's error the rates before it left, at most */
} disc;

/* The rate (ns per TSC count, times 2^SHIFT) from a to b, and its bound (2^-32 parts), false
   when the two cannot give one (too short, past the arithmetic). The count's quantum is one count
   and each end's jitter; the bound adds 2 for the fixed points' rounding (ns_q32 and mult each
   under 2^-32 of their value). */
static bool estimate(const struct sample *a, const struct sample *b, uint64_t *mult, uint32_t *bound) {
    if (b->tsc <= a->tsc || b->count <= a->count) return false;
    uint64_t T = b->tsc - a->tsc, h = a->half + b->half, R = b->count - a->count;
    uint64_t q = 1 + (uint64_t)a->jitter + b->jitter;
    if (h >= T || q >= R) return false;
    if (!clock_muldiv(R, disc.ns_q32 << (SHIFT - 32), T, mult)) return false;
    uint64_t e1, e2;
    if (!clock_muldiv_up(h, ONE_Q32, T, &e1) || !clock_muldiv_up(e1, R, R - q, &e1) ||
        !clock_muldiv_up(q, ONE_Q32, R - q, &e2))
        return false;
    uint64_t e = e1 + e2 + 2;
    if (e >= CLOCK_RATE_UNMEASURED) return false;
    *bound = (uint32_t)e;
    return true;
}

/* Two rates within their two bounds of each other. */
static bool agrees(uint64_t mult, uint32_t e, uint64_t other, uint32_t other_e) {
    uint64_t diff = mult > other ? mult - other : other - mult, rel;
    return clock_muldiv(diff, ONE_Q32, other, &rel) && rel <= (uint64_t)e + other_e;
}

/* The first window, from its anchor: as long as makes each part of its bound under
   2^-FIRST_SHIFT, the count's quantum against the reference's count and the anchor's span (taken
   for both ends) against the TSC's, at the rate in use. */
static uint64_t first_window(const struct sample *s) {
    uint64_t counts = (1 + 2 * (uint64_t)s->jitter) << FIRST_SHIFT, ns, by_tsc;
    if (clock_muldiv(s->half << (FIRST_SHIFT + 1), data->mult, 1ull << SHIFT, &ns) &&
        clock_muldiv(ns, ONE_Q32, disc.ns_q32, &by_tsc) && by_tsc > counts)
        counts = by_tsc;
    return counts;
}

/* What a sample's log lines say (built under the lock, written after it). */
struct rate_report {
    bool begun, changed, moved;
    const char *ref;
    uint64_t first_us;                     /* begun: the first window */
    uint64_t was, now_mult, ms, error_ns;  /* changed */
    uint32_t bound;
};

static void adopt(uint64_t mult, uint32_t bound, uint64_t now, uint64_t window, bool moved, struct rate_report *rep) {
    uint64_t rel = disc.bound, error;
    if (rel == CLOCK_RATE_UNMEASURED) {
        /* the stated rate was off by what this window shows, within its bound */
        uint64_t diff = data->mult > mult ? data->mult - mult : mult - data->mult;
        if (!clock_muldiv_up(diff, ONE_Q32, mult, &rel) || rel > UINT64_MAX - bound) rel = UINT64_MAX;
        else rel += bound;
    }
    if (!clock_muldiv(now - disc.adopted_ns, rel, ONE_Q32, &error)) error = CLOCK_NEVER;
    disc.error_ns = sat_add(disc.error_ns, error);
    disc.adopted_ns = now;
    disc.bound = bound;
    rep->changed = true;
    rep->moved = moved;
    rep->was = data->mult;
    rep->now_mult = mult;
    rep->error_ns = disc.error_ns;
    rep->bound = bound;
    clock_muldiv(window, disc.ns_q32, ONE_Q32 * NS_PER_MS, &rep->ms);
    rate_set(mult, bound);
}

static void report_rate(const struct rate_report *rep) {
    if (rep->begun)
        kprintf("clock: the TSC measured against %s over windows that double from %lu us\n", rep->ref, rep->first_us);
    if (!rep->changed) return;
    uint64_t hz = hz_of(rep->now_mult), was = hz_of(rep->was), change = 0;
    clock_muldiv(hz > was ? hz - was : was - hz, NS_PER_S, was, &change);
    kprintf("clock: TSC %lu.%06lu MHz within %lu ppb of %s over %s%lu ms (%c%lu ppb); the clock within %lu ns of it\n",
            hz / 1000000, hz % 1000000, ppb_of(rep->bound), rep->ref, rep->moved ? "the last " : "", rep->ms,
            hz >= was ? '+' : '-', change, rep->error_ns);
}

/* Under clock_lock: a sample of ref into the windows; when the next one is wanted. */
static uint64_t discipline(const struct clock_rate_reference *ref, const struct clock_rate_sample *in,
                           struct rate_report *rep) {
    if (rate_kind != RATE_P0 || in->tsc_b < in->tsc_a || in->cpu >= data->cpus) return CLOCK_NEVER;
    uint64_t width = in->tsc_b - in->tsc_a;
    struct sample s = {.tsc = in->tsc_a - (uint64_t)data->offset[in->cpu] + width / 2,
                       .half = width - width / 2, .count = in->count, .jitter = in->jitter};
    uint64_t now = clock_now_ns();
    if (ref != disc.ref) {
        uint64_t ns_q32;
        if (ref->hz_den > UINT64_MAX / NS_PER_S || !clock_muldiv(ref->hz_den * NS_PER_S, ONE_Q32, ref->hz_num, &ns_q32) ||
            !ns_q32 || ns_q32 >> (64 - (SHIFT - 32)))
            return CLOCK_NEVER; /* no rate the windows' arithmetic holds */
        disc.ref = ref;
        disc.ns_q32 = ns_q32;
        disc.begin = rep->begun = true;
    }
    if (disc.begin || s.count < disc.anchor.count) {
        disc.begin = false;
        disc.anchor = disc.mark = s;
        disc.window = first_window(&s);
        clock_muldiv(disc.window, disc.ns_q32, ONE_Q32 * NS_PER_US, &rep->first_us);
    } else if (s.count - disc.anchor.count >= disc.window) {
        uint64_t mult, reached = s.count - disc.anchor.count;
        uint32_t bound;
        if (estimate(&disc.anchor, &s, &mult, &bound)) {
            if (agrees(mult, bound, data->mult, disc.bound)) {
                if (bound < disc.bound) adopt(mult, bound, now, reached, false, rep);
            } else {
                /* the ratio itself moved: the last window alone, and the windows from there */
                if (estimate(&disc.mark, &s, &mult, &bound))
                    adopt(mult, bound, now, s.count - disc.mark.count, true, rep);
                disc.anchor = disc.mark;
            }
        } else {
            disc.anchor = disc.mark; /* past what the arithmetic holds: grow again from the last */
        }
        reached = s.count - disc.anchor.count;
        disc.window = reached > UINT64_MAX / 2 ? reached : 2 * reached;
        disc.mark = s;
    }
    /* at the window's end */
    uint64_t done = s.count - disc.anchor.count, wait;
    if (!clock_muldiv(done < disc.window ? disc.window - done : 0, disc.ns_q32, ONE_Q32, &wait)) return CLOCK_NEVER;
    return sat_add(now, wait);
}

uint64_t clock_rate_sample(const struct clock_rate_reference *ref, const struct clock_rate_sample *s) {
    if (!__atomic_load_n(&data, __ATOMIC_ACQUIRE)) return CLOCK_NEVER;
    struct rate_report rep = {.ref = ref->name};
    uint64_t f = spin_lock(&clock_lock);
    uint64_t next = discipline(ref, s, &rep);
    spin_unlock(&clock_lock, f);
    if (rep.changed) alarm_clock_changed();
    report_rate(&rep);
    return next;
}

void clock_register_tsc_hz(uint64_t hz, const char *source) {
    if (__atomic_load_n(&data, __ATOMIC_ACQUIRE)) {
        kprintf("clock: %s states the TSC's rate after the clock began, which keeps %s's\n", source, rate_source);
        return;
    }
    if (registered.hz) {
        kprintf("clock: %s's TSC rate is not used: %s gave one first\n", source, registered.source);
        return;
    }
    registered.hz = hz;
    registered.source = source;
}

/* ---- the CPUs' TSCs against each other ---- */

#define COMPARE_DONE UINT32_MAX
static uint64_t highest __attribute__((aligned(64)));      /* the highest reading of the pass */
static uint32_t compare_state __attribute__((aligned(64))); /* 0 between, a pass's number, COMPARE_DONE */
static struct {
    uint32_t session;             /* each comparison's number */
    uint32_t cpus;                /* comparing */
    uint32_t started, settled, through, left; /* in this pass; returned after it all */
    uint32_t behind_cpus, short_cpus;
    uint64_t behind_most, short_most;
} __attribute__((aligned(64))) compare;
PERCPU static uint32_t compare_joined; /* the session this CPU's offset belongs to */
PERCPU static int64_t compare_offset;  /* its offset as the comparison has it */

static void raise_to(uint64_t *word, uint64_t value) {
    uint64_t was = __atomic_load_n(word, __ATOMIC_RELAXED);
    while (value > was && !__atomic_compare_exchange_n(word, &was, value, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

/* One pass on this CPU: blocks of readings against the highest, doubling (once every CPU reads)
   until one shows it no further behind; then on, still taking what it sees, until every CPU has
   got there, so each is read against all the others to the end. What it was most behind by is
   added to its offset. */
static void compare_pass(void) {
    unsigned self = cpu_current_index();
    if (this_cpu(compare_joined) != compare.session) {
        this_cpu(compare_joined) = compare.session;
        this_cpu(compare_offset) = __atomic_load_n(&data->offset[self], __ATOMIC_RELAXED);
    }
    __atomic_fetch_add(&compare.started, 1, __ATOMIC_ACQ_REL);
    int64_t offset = this_cpu(compare_offset);
    uint64_t block = 1, most = 0;
    bool settled = false;
    for (;;) {
        uint64_t found = 0;
        for (uint64_t i = 0; i < block; i++) {
            uint64_t seen = __atomic_load_n(&highest, __ATOMIC_ACQUIRE);
            uint64_t own = clock_tsc_read(NULL) - (uint64_t)offset;
            if (own < seen) {
                if (seen - own > found) found = seen - own;
            } else if (own > seen) {
                __atomic_compare_exchange_n(&highest, &seen, own, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
            }
        }
        bool more = found > most;
        if (more) most = found;
        if (!settled && __atomic_load_n(&compare.started, __ATOMIC_ACQUIRE) == compare.cpus) {
            if (more) block *= 2;
            else {
                settled = true;
                __atomic_fetch_add(&compare.settled, 1, __ATOMIC_ACQ_REL);
            }
        }
        if (settled && __atomic_load_n(&compare.settled, __ATOMIC_ACQUIRE) == compare.cpus) break;
    }
    if (most) {
        this_cpu(compare_offset) = offset - (int64_t)most;
        __atomic_fetch_add(&compare.behind_cpus, 1, __ATOMIC_RELAXED);
        raise_to(&compare.behind_most, most);
    }
    __atomic_fetch_add(&compare.through, 1, __ATOMIC_ACQ_REL);
}

void clock_cpu_compare(void) {
    if (!__atomic_load_n(&data, __ATOMIC_ACQUIRE)) return;
    uint32_t last = 0, state;
    for (;;) {
        while ((state = __atomic_load_n(&compare_state, __ATOMIC_ACQUIRE)) == 0 || state == last) pause();
        if (state == COMPARE_DONE) break;
        compare_pass();
        last = state;
    }
    __atomic_fetch_add(&compare.left, 1, __ATOMIC_ACQ_REL);
}

void clock_cpus_compare(unsigned cpus) {
    if (!__atomic_load_n(&data, __ATOMIC_ACQUIRE) || cpus < 2) return;
    preempt_disable(); /* its passes are this CPU's readings, against the others' at once */
    compare.session++;
    compare.cpus = cpus;
    compare.left = compare.short_cpus = 0;
    compare.short_most = 0;
    uint32_t pass;
    for (pass = 1;; pass++) {
        compare.started = compare.settled = compare.through = compare.behind_cpus = 0;
        compare.behind_most = 0;
        __atomic_store_n(&highest, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&compare_state, pass, __ATOMIC_RELEASE);
        compare_pass();
        while (__atomic_load_n(&compare.through, __ATOMIC_ACQUIRE) != cpus) pause();
        if (!compare.behind_cpus) break;
        if (compare.behind_cpus > compare.short_cpus) compare.short_cpus = compare.behind_cpus;
        compare.short_most += compare.behind_most;
    }
    /* the offsets into the data, for the CPUs that compared; a changed one moves the clock's count
       under the rate's windows, which begin again */
    uint64_t f = spin_lock(&clock_lock);
    write_begin();
    for (unsigned i = 0; i < data->cpus; i++)
        if (per_cpu(compare_joined, i) == compare.session) SET(offset[i], per_cpu(compare_offset, i));
    write_end();
    if (compare.short_cpus) disc.begin = true;
    spin_unlock(&clock_lock, f);
    __atomic_store_n(&compare_state, COMPARE_DONE, __ATOMIC_RELEASE);
    while (__atomic_load_n(&compare.left, __ATOMIC_ACQUIRE) != cpus - 1) pause();
    __atomic_store_n(&compare_state, 0, __ATOMIC_RELEASE);
    preempt_enable();
    if (compare.short_cpus)
        kprintf("clock: of %u CPUs' TSCs, up to %u read behind, by up to %lu counts in all; added to their readings (%u passes)\n",
                cpus, compare.short_cpus, compare.short_most, pass);
    else
        kprintf("clock: the TSCs of %u CPUs agree\n", cpus);
}

/* ---- the RTC and the wall clock ---- */

/* The RTC, read again while it updates (a moment once a second: one still updating a second on
   is no RTC). a, b: the monotonic clock just before and after the reading that answered. */
static enum rtc_result rtc_now(int64_t *seconds, uint64_t *a, uint64_t *b) {
    uint64_t until = clock_after(NS_PER_S);
    for (;;) {
        *a = clock_now_ns();
        enum rtc_result r = rtc_read(seconds);
        *b = clock_now_ns();
        if (r != RTC_UPDATING) return r;
        if (*b >= until) return RTC_ABSENT;
        pause();
    }
}

/* What the wall-time references proved (clock_lock). In "the time less the monotonic clock" a
   word that the time was at least u by the moment `by` proves at least u - by. */
static struct {
    int64_t floor;     /* what they proved, at least: what the clocks have from them. At boot 0
                          (the wall clock reads the monotonic clock from 1970), after a wake the
                          most it could have been before the sleep */
    bool waking;       /* from a wake until the RTC's next second is found: what more they prove is
                          time asleep */
    uint32_t wall_gen; /* the wall clock's generation they began in (boot, a wake): a step since stands */
} proofs;

int64_t clock_wall_proof(int64_t unix_ns, uint64_t by) {
    if (!__atomic_load_n(&data, __ATOMIC_ACQUIRE)) return 0;
    int64_t proved = unix_ns - (int64_t)by, gain = 0;
    bool slept = false;
    uint64_t f = spin_lock(&clock_lock);
    if (proved > proofs.floor) {
        gain = proved - proofs.floor;
        proofs.floor = proved;
        bool wall = proofs.wall_gen == wall_gen;
        slept = proofs.waking;
        if (wall || slept) {
            write_begin();
            if (slept) SET(asleep_ns, data->asleep_ns + (uint64_t)gain);
            if (wall) SET(wall_ns, data->wall_ns + gain);
            write_end();
        }
    }
    spin_unlock(&clock_lock, f);
    if (slept) wait_boot_clock_moved(); /* the deadlines on the boot clock came nearer */
    return gain;
}

/* The search for the RTC's next second (the alarm's, one CPU at a time; set up with the alarm not
   armed). A reading of second s at [a, b] proves the time at least s*1e9 by b; the edge where it
   becomes next*1e9, known to lie in (lo, hi], proves at least next*1e9 by hi. */
#define EDGE_DRIFT (NS_PER_S >> 10) /* first assumed: an RTC second within 977 ppm of the clock's */
static struct {
    struct alarm alarm;
    int64_t next;       /* the RTC's next second: readable from a moment in (lo, hi] */
    uint64_t lo, hi;
    uint64_t shift;     /* whole seconds the probe on its way looks on by */
    uint64_t drift;     /* how far an RTC second may be from the clock's */
    bool afresh;        /* the next probe reads anew (after a contradiction while it updated) */
} edge;

/* The search is over (found, or given up): a wake's time asleep is what it proved by then. */
static void edge_end(void) {
    uint64_t f = spin_lock(&clock_lock);
    proofs.waking = false;
    spin_unlock(&clock_lock, f);
}

static void edge_probe(void) {
    uint64_t now = clock_now_ns(), at;
    if (edge.afresh) {
        at = now + NS_PER_S / 2; /* half the RTC's period on: its update is long over */
    } else {
        uint64_t mid = edge.lo + (edge.hi - edge.lo) / 2;
        edge.shift = mid > now ? 0 : (now - mid) / NS_PER_S + 1;
        at = mid + edge.shift * NS_PER_S;
    }
    alarm_set(&edge.alarm, at);
}

static void edge_begin(int64_t seconds, uint64_t a, uint64_t b) {
    edge.next = seconds + 1;
    edge.lo = a;
    edge.hi = b + NS_PER_S + NS_PER_S / 2; /* a second, its update, and room */
    edge.afresh = false;
    edge_probe();
}

/* The edge is known well enough: the clocks take what it proves beyond what they have. */
static void edge_found(void) {
    bool waking = __atomic_load_n(&proofs.waking, __ATOMIC_RELAXED); /* changes only with the search */
    int64_t gain = clock_wall_proof(edge.next * (int64_t)NS_PER_S, edge.hi);
    edge_end();
    kprintf("clock: the RTC's second found within %lu us; %s %ld us\n", (edge.hi - edge.lo) / NS_PER_US,
            waking ? "the time asleep longer by" : "the wall clock on by", gain / (int64_t)NS_PER_US);
}

static void edge_fire(struct alarm *al, struct regs *r) {
    int64_t w = 0;
    uint64_t a = clock_now_ns();
    enum rtc_result read = rtc_read(&w);
    uint64_t b = clock_now_ns();
    if (read == RTC_ABSENT) {
        kprintf("clock: the RTC stopped answering\n");
        edge_end();
        return;
    }
    if (edge.afresh) {
        if (read == RTC_UPDATING) edge_probe();
        else edge_begin(w, a, b);
        return;
    }
    /* the probe looked `shift` seconds on: the edge then is that many RTC seconds after the one
       sought, each within drift of the clock's */
    uint64_t k = edge.shift, d = edge.drift;
    edge.next += (int64_t)k;
    edge.lo += k * (NS_PER_S - d);
    edge.hi += k * (NS_PER_S + d);
    if (read == RTC_UPDATING || w == edge.next - 1) edge.lo = MAX(edge.lo, a);
    else if (w == edge.next) edge.hi = MIN(edge.hi, b);
    else edge.lo = edge.hi;
    if (edge.lo >= edge.hi) {
        /* not where it can be: the RTC's seconds are further from the clock's than assumed */
        edge.drift *= 2;
        if (edge.drift >= NS_PER_S / 2) {
            kprintf("clock: the RTC's seconds do not keep time; the clocks keep what they have from it\n");
            edge_end();
            return;
        }
        if (read == RTC_READ) edge_begin(w, a, b);
        else {
            edge.afresh = true;
            edge_probe();
        }
        return;
    }
    /* a probe a second on widens the span by twice the drift and halves it: past 4 drifts it
       closes in no further than 2 */
    if (edge.hi - edge.lo > 4 * edge.drift + (b - a)) edge_probe();
    else edge_found();
}

/* After a reading of second s at [a, b], which the clocks took (clock_wall_proof): the search
   begins (the alarm not armed, or its CPU's interrupts off with nothing else touching it). */
static void edge_start(int64_t s, uint64_t a, uint64_t b) {
    edge.drift = EDGE_DRIFT;
    edge_begin(s, a, b);
}

static int64_t wall_at(const struct clock_data *h, uint64_t t) {
    int64_t wall = h->wall_ns + (int64_t)(t > h->wall_from ? t - h->wall_from : 0);
    if (h->wall_slew) {
        uint64_t until = t < h->wall_until ? t : h->wall_until, ran = until > h->wall_from ? until - h->wall_from : 0;
        uint64_t whole = h->wall_slew < 0 ? -(uint64_t)h->wall_slew : (uint64_t)h->wall_slew;
        uint64_t done = (uint64_t)(((unsigned __int128)ran * h->wall_slew_mult) >> 32);
        if (done > whole) done = whole;
        wall += h->wall_slew < 0 ? -(int64_t)done : (int64_t)done;
    }
    return wall;
}

int64_t clock_wall_ns(void) {
    if (!__atomic_load_n(&data, __ATOMIC_ACQUIRE)) return 0;
    struct clock_data h;
    uint64_t t = snapshot(&h);
    return wall_at(&h, t);
}

int64_t clock_wall_seconds(void) {
    int64_t ns = clock_wall_ns();
    return ns >= 0 ? ns / (int64_t)NS_PER_S : -((-ns + (int64_t)NS_PER_S - 1) / (int64_t)NS_PER_S);
}

/* In a write: the wall clock is wall at the monotonic t, with no slew. */
static void wall_put(int64_t wall, uint64_t t) {
    SET(wall_ns, wall);
    SET(wall_from, t);
    SET(wall_until, t);
    SET(wall_slew, 0);
    SET(wall_slew_mult, 0);
}

void clock_set_wall(int64_t unix_ns) {
    if (!__atomic_load_n(&data, __ATOMIC_ACQUIRE)) return;
    uint64_t f = spin_lock(&clock_lock), t;
    write_begin();
    wall_put(unix_ns, write_now(&t));
    write_end();
    wall_gen++;
    spin_unlock(&clock_lock, f);
}

void clock_adjust_wall(int64_t offset_ns, uint32_t slew_ppm) {
    if (!__atomic_load_n(&data, __ATOMIC_ACQUIRE)) return;
    uint64_t f = spin_lock(&clock_lock), t;
    write_begin();
    uint64_t now = write_now(&t);
    int64_t wall = wall_at(data, now);
    uint64_t whole = offset_ns < 0 ? -(uint64_t)offset_ns : (uint64_t)offset_ns, span, mult;
    /* over whole * 10^6 / ppm ns (rounded up, so the rate stays within ppm), the slew's part
       reaching whole there by its multiplier (rounded up, and clamped to whole) */
    if (offset_ns && slew_ppm && slew_ppm < 1000000 && clock_muldiv_up(whole, 1000000, slew_ppm, &span) &&
        clock_muldiv_up(whole, ONE_Q32, span, &mult)) {
        wall_put(wall, now);
        SET(wall_until, sat_add(now, span));
        SET(wall_slew, offset_ns);
        SET(wall_slew_mult, mult);
    } else {
        wall_put(wall + offset_ns, now);
    }
    write_end();
    wall_gen++;
    spin_unlock(&clock_lock, f);
}

static void wall_init(void) {
    int64_t s;
    uint64_t a, b;
    alarm_init(&edge.alarm, edge_fire);
    if (rtc_now(&s, &a, &b) != RTC_READ) {
        kprintf("clock: no RTC answers: the wall clock starts at 1970\n");
        return;
    }
    clock_wall_proof(s * (int64_t)NS_PER_S, b);
    edge_start(s, a, b);
}

/* ---- boot ---- */

void clock_init(void) {
    unsigned cpus = topology.cpus;
    size_t bytes = sizeof(struct clock_data) + (size_t)cpus * sizeof(int64_t);
    data_count = ALIGN_UP(bytes, PAGE_SIZE) / PAGE_SIZE;
    data_first = pmm_alloc_contig(data_count, 64);
    if (!data_first) panic("clock: no memory for its data");
    struct clock_data *d = phys_to_virt(data_first);
    memset(d, 0, data_count * PAGE_SIZE);
    d->cpus = cpus;
    d->shift = SHIFT;
    /* The rate as stated: a registered one (a hypervisor's guest's TSC is the hypervisor's to set,
       and a TSC ratio may scale it), else the P0 P-state's (arch/cpu.c reads it where no hypervisor
       runs the CPU, and stops the boot where it cannot) */
    uint64_t num, den, mult;
    if (registered.hz) {
        num = registered.hz;
        den = 1;
        rate_kind = RATE_REGISTERED;
        rate_source = registered.source;
    } else if (cpu_features.p0_did) {
        num = CPU_COF_UNIT_HZ * cpu_features.p0_fid;
        den = cpu_features.p0_did;
        rate_kind = RATE_P0;
        rate_source = "the P0 P-state";
    } else {
        panic("clock: no TSC rate: the hypervisor registers none");
    }
    if (!mult_of(num, den, &mult)) panic("clock: %s gives the TSC no usable rate (%lu / %lu Hz)", rate_source, num, den);
    /* the monotonic clock from the TSC's 0 (the CPU's reset) */
    d->mult = mult;
    d->rate_bound = CLOCK_RATE_UNMEASURED;
    d->tsc_hz = num / den;
    disc.bound = CLOCK_RATE_UNMEASURED;
    __atomic_store_n(&data, d, __ATOMIC_RELEASE);
    uint64_t hz = d->tsc_hz;
    if (rate_kind == RATE_P0) {
        kprintf("clock: TSC %lu.%06lu MHz by the P0 P-state (FID %xh, DID %xh)\n", hz / 1000000, hz % 1000000,
                cpu_features.p0_fid, cpu_features.p0_did);
        if (!refclock_hpet_start())
            kprintf("clock: no HPET to measure the TSC against; it keeps the P0 P-state's nominal rate\n");
    } else {
        kprintf("clock: TSC %lu.%06lu MHz by %s\n", hz / 1000000, hz % 1000000, rate_source);
    }
    wall_init();
}

/* ---- sleep ---- */

static struct {
    uint64_t ns, tsc;       /* the monotonic clock and CPU 0's TSC as it stopped */
    bool rtc;               /* the RTC answered */
    int64_t rtc_low, rtc_up; /* the RTC's ns less the clock's then, at least and at most */
} stop;

void clock_suspend(void) {
    if (!data) return;
    alarm_cancel(&edge.alarm); /* a search not done: it begins again after the sleep */
    edge_end();
    int64_t s;
    uint64_t a, b;
    stop.rtc = rtc_now(&s, &a, &b) == RTC_READ;
    if (stop.rtc) {
        stop.rtc_low = s * (int64_t)NS_PER_S - (int64_t)b;
        stop.rtc_up = (s + 1) * (int64_t)NS_PER_S - (int64_t)a;
    }
    stop.ns = clock_now_ns();
    stop.tsc = clock_tsc_read(NULL);
}

int64_t clock_resume(void) {
    if (!data) return 0;
    /* the clock on from where it stopped, every CPU's offset gone (they compare again as they start) */
    uint64_t f = spin_lock(&clock_lock);
    uint64_t raw = clock_tsc_read(NULL);
    write_begin();
    for (unsigned i = 0; i < data->cpus; i++) SET(offset[i], 0);
    SET(base_tsc, raw);
    SET(base_ns, stop.ns);
    write_end();
    disc.begin = true; /* the windows begin again on the reference's new count (below) */
    const struct clock_rate_reference *ref = disc.ref;
    spin_unlock(&clock_lock, f);
    int64_t slept = 0, sa;
    uint64_t a, b;
    if (stop.rtc && rtc_now(&sa, &a, &b) == RTC_READ) {
        int64_t low = sa * (int64_t)NS_PER_S - (int64_t)b, up = (sa + 1) * (int64_t)NS_PER_S - (int64_t)a;
        /* The RTC's lead on the clock grew by the time the clock did not count, between low -
           rtc_up and up - rtc_low. A TSC that went on counting through it (a sleep that did not
           happen) counted time that ran, where its count fits there: the clock takes it. */
        uint64_t ran;
        if (raw >= stop.tsc && clock_muldiv(raw - stop.tsc, data->mult, 1ull << SHIFT, &ran) && ran < (uint64_t)INT64_MAX &&
            (int64_t)ran >= low - stop.rtc_up && (int64_t)ran <= up - stop.rtc_low) {
            f = spin_lock(&clock_lock);
            write_begin();
            SET(base_ns, data->base_ns + ran);
            write_end();
            spin_unlock(&clock_lock, f);
            a += ran;
            b += ran;
        }
        /* The time asleep is what the RTC proves beyond the most it could have been before the
           sleep: this reading's, at once, then its next second's */
        f = spin_lock(&clock_lock);
        proofs.floor = stop.rtc_up;
        proofs.waking = true;
        proofs.wall_gen = wall_gen;
        spin_unlock(&clock_lock, f);
        slept = clock_wall_proof(sa * (int64_t)NS_PER_S, b);
        edge_start(sa, a, b);
    }
    if (ref) ref->resume();
    return slept / (int64_t)NS_PER_S;
}

/* ---- dates ---- */

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

void time_to_parts(int64_t t, struct tm_parts *p) {
    int64_t days = t / 86400;
    int64_t rem = t % 86400;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    p->hour = rem / 3600;
    p->min = (rem % 3600) / 60;
    p->sec = rem % 60;
    p->wday = (int)((days + 4) % 7);
    if (p->wday < 0) p->wday += 7;
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned doe = (unsigned)(days - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    p->mday = doy - (153 * mp + 2) / 5 + 1;
    p->mon = mp < 10 ? mp + 3 : mp - 9;
    p->year = (int)(y + (p->mon <= 2));
}

int64_t time_from_parts(const struct tm_parts *p) {
    return days_from_civil(p->year, p->mon, p->mday) * 86400 + p->hour * 3600 + p->min * 60 + p->sec;
}
