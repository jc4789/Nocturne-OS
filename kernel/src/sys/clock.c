/* The clock (sys/clock.h). Two counters can hold it:

   - the hypervisor's reference time (vm/vm.h), which it keeps right for the guest across its own
     scheduling and migrations: in a virtual machine that offers one, it is the clock;
   - the TSC, on a CPU whose TSC runs at one rate in every power state (CPUID 80000007h EDX[8],
     invariant TSC). Its rate is the hypervisor's word or CPUID 15h's where either gives it,
     else measured against the ACPI PM timer (3.579545 MHz, ACPI 6.5 4.8.3.3), else the PIT's
     channel 2 (1.193182 MHz): at boot over five short spans (their median), and with the PM
     timer once more over CLOCK_REMEASURE_S, after which the clock runs on at the better rate
     from the value it had. The CPUs' TSCs are compared with each other, all at once, when they
     start (clock_cpus_compare); what a CPU's TSC is seen behind by is added to its readings.

   A TSC that is not invariant is used too, with a warning: a general machine without either is
   one this clock does not serve well yet (the HPET would). */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/smp.h"
#include "arch/spin.h"
#include "dev/acpi.h"
#include "sys/alarm.h"
#include "sys/clock.h"
#include "vm/vm.h"

#define PM_TIMER_HZ 3579545u
#define PIT_HZ 1193182u
#define MEASURE_SPANS 5
#define CLOCK_REMEASURE_S 64
#define COMPARE_ROUNDS 256

static enum { SOURCE_NONE, SOURCE_HYPERVISOR, SOURCE_TSC } source;
static uint64_t tsc_hz;
static const char *rate_from;

/* ns = base_ns + (tsc - base_tsc) * mult / 2^32, and back: tsc delta = ns delta * inverse / 2^24
   (rounded up, so a TSC deadline never comes before its ns). seq is odd while the conversion
   changes (a new rate, S3); readers retry. */
static struct {
    uint64_t base_tsc, base_ns, mult, inverse;
} conv;
static uint32_t conv_seq;
static struct spinlock conv_lock;

/* The hypervisor's time (ns) at which the clock read 0, modulo 2^64: after a hibernation image is
   restored the hypervisor's time began again, below the clock's, and this is "negative". */
static uint64_t hv_base;

/* Per CPU: what its TSC reads more than the clock's TSC (clock_cpus_compare); taken off its
   readings. */
PERCPU static int64_t tsc_offset;
static bool tsc_offsets;

/* The ACPI PM timer: its port and the bits that count. */
static uint16_t pm_port;
static uint32_t pm_mask;

/* The wall clock: Unix time in ns minus the clock's ns. */
static int64_t wall_offset_ns;

static inline uint64_t tsc_read(void) {
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return (uint64_t)hi << 32 | lo;
}

static uint64_t ns_of(uint64_t tsc) {
    for (;;) {
        uint32_t seq = __atomic_load_n(&conv_seq, __ATOMIC_ACQUIRE);
        if (seq & 1) {
            pause();
            continue;
        }
        uint64_t base_tsc = __atomic_load_n(&conv.base_tsc, __ATOMIC_RELAXED);
        uint64_t base_ns = __atomic_load_n(&conv.base_ns, __ATOMIC_RELAXED);
        uint64_t mult = __atomic_load_n(&conv.mult, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&conv_seq, __ATOMIC_RELAXED) != seq) continue;
        if (tsc <= base_tsc) return base_ns; /* a reading from before a change */
        return base_ns + (uint64_t)(((unsigned __int128)(tsc - base_tsc) * mult) >> 32);
    }
}

static uint64_t tsc_now(void) {
    uint64_t tsc = tsc_read();
    if (__atomic_load_n(&tsc_offsets, __ATOMIC_ACQUIRE)) tsc -= (uint64_t)this_cpu(tsc_offset);
    return tsc;
}

uint64_t clock_now_ns(void) {
    uint64_t ns;
    switch (source) {
    case SOURCE_TSC: return ns_of(tsc_now());
    case SOURCE_HYPERVISOR:
        if (!vm_clock_ns(&ns)) return 0;
        return (int64_t)(ns - hv_base) > 0 ? ns - hv_base : 0;
    default: return 0;
    }
}

uint64_t clock_tsc_hz(void) { return tsc_hz; }

bool clock_tsc_at(uint64_t ns, uint64_t *tsc) {
    if (source != SOURCE_TSC) return false;
    for (;;) {
        uint32_t seq = __atomic_load_n(&conv_seq, __ATOMIC_ACQUIRE);
        if (seq & 1) {
            pause();
            continue;
        }
        uint64_t base_tsc = conv.base_tsc, base_ns = conv.base_ns, inverse = conv.inverse;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&conv_seq, __ATOMIC_RELAXED) != seq) continue;
        uint64_t delta = ns > base_ns ? ns - base_ns : 0;
        unsigned __int128 ticks = ((unsigned __int128)delta * inverse + ((1u << 24) - 1)) >> 24;
        if (ticks >> 62) return false; /* past any TSC this machine reaches */
        *tsc = base_tsc + (uint64_t)ticks;
        if (__atomic_load_n(&tsc_offsets, __ATOMIC_ACQUIRE)) *tsc += (uint64_t)this_cpu(tsc_offset);
        return true;
    }
}

bool clock_hypervisor_at(uint64_t ns, uint64_t *reference_ns) {
    if (source != SOURCE_HYPERVISOR) return false;
    *reference_ns = hv_base + ns;
    return true;
}

/* conv_lock held: from the TSC reading at, the clock reads ns and runs at hz. */
static void conv_set(uint64_t at, uint64_t ns, uint64_t hz) {
    __atomic_store_n(&conv_seq, conv_seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&conv.base_tsc, at, __ATOMIC_RELAXED);
    __atomic_store_n(&conv.base_ns, ns, __ATOMIC_RELAXED);
    __atomic_store_n(&conv.mult, (NS_PER_S << 32) / hz, __ATOMIC_RELAXED);
    __atomic_store_n(&conv.inverse, ((hz << 24) + NS_PER_S - 1) / NS_PER_S, __ATOMIC_RELAXED);
    __atomic_store_n(&conv_seq, conv_seq + 1, __ATOMIC_RELEASE);
    __atomic_store_n(&tsc_hz, hz, __ATOMIC_RELAXED);
}

/* ---- the TSC's rate ---- */

static uint32_t pm_read(void) { return inl(pm_port) & pm_mask; }

/* The FADT's PM timer, in system I/O space: X_PM_TMR_BLK where it is given, else PM_TMR_BLK
   (ACPI 6.5 table 5.9). A hardware-reduced platform has none. */
static void pm_timer_find(void) {
    const struct acpi_fadt *f = acpi_find("FACP", 0);
    if (!f || f->h.length < 116 || (f->flags & FADT_HW_REDUCED)) return;
    uint64_t port = f->pm_tmr;
    if (f->h.length >= 220 && f->x_pm_tmr.address) {
        if (f->x_pm_tmr.space != 1) return; /* in memory space: not read here */
        port = f->x_pm_tmr.address;
    }
    if (!port || port > 0xFFFC || f->pm_tmr_len < 4) return;
    pm_port = (uint16_t)port;
    pm_mask = (f->flags & FADT_TMR_VAL_EXT) ? 0xFFFFFFFFu : 0xFFFFFFu;
    /* a port that does not count is no timer */
    uint32_t first = pm_read();
    for (unsigned i = 0; i < 100000; i++)
        if (pm_read() != first) return;
    pm_port = 0;
}

/* The TSC over about 10 ms of the PM timer, from one of its edges to another. */
static uint64_t measure_pm(void) {
    uint32_t start = pm_read(), now;
    while ((now = pm_read()) == start) {}
    uint64_t tsc0 = tsc_read();
    start = now;
    uint32_t span = PM_TIMER_HZ / 100, counted;
    while ((counted = (pm_read() - start) & pm_mask) < span) {}
    uint64_t tsc1 = tsc_read();
    return (tsc1 - tsc0) * PM_TIMER_HZ / counted;
}

/* The TSC over 10 ms of the PIT's channel 2 counting down once (its gate through port 61h), or
   0 where nothing answers there. */
static uint64_t measure_pit(void) {
    uint8_t p61 = inb(0x61);
    if (p61 == 0xFF) return 0;
    outb(0x61, (p61 & ~0x02) | 0x01); /* speaker off, gate on */
    outb(0x43, 0xB0);                 /* channel 2, low then high byte, mode 0 */
    uint16_t count = PIT_HZ / 100;
    outb(0x42, count & 0xFF);
    outb(0x42, count >> 8);
    outb(0x61, inb(0x61) & ~0x01);    /* the count starts as the gate goes high again */
    uint64_t tsc0 = tsc_read();
    outb(0x61, inb(0x61) | 0x01);
    uint64_t spins = 0;
    while (!(inb(0x61) & 0x20) && ++spins <= 50000000) pause();
    uint64_t tsc1 = tsc_read();
    outb(0x61, p61);
    if (spins > 50000000) return 0;
    return (tsc1 - tsc0) * PIT_HZ / count;
}

static uint64_t median_of(uint64_t (*measure)(void)) {
    uint64_t v[MEASURE_SPANS];
    for (unsigned i = 0; i < MEASURE_SPANS; i++) {
        v[i] = measure();
        if (!v[i]) return 0;
        for (unsigned j = i; j > 0 && v[j - 1] > v[j]; j--) {
            uint64_t t = v[j];
            v[j] = v[j - 1];
            v[j - 1] = t;
        }
    }
    return v[MEASURE_SPANS / 2];
}

/* The long measurement: the PM timer, extended past its wrap by an alarm each second, against the
   TSC from the boot's measurement to CLOCK_REMEASURE_S later. */
static struct {
    struct alarm alarm;
    uint64_t start_tsc, pm_total;
    uint32_t pm_last;
    unsigned seconds;
} remeasure;

static void remeasure_fire(struct alarm *a, struct regs *r) {
    uint32_t pm = pm_read();
    uint64_t tsc = tsc_read();
    remeasure.pm_total += (pm - remeasure.pm_last) & pm_mask;
    remeasure.pm_last = pm;
    if (++remeasure.seconds < CLOCK_REMEASURE_S) {
        alarm_set(a, a->at + NS_PER_S);
        return;
    }
    /* below 2^64: 64 s of a TSC under 70 GHz, times the PM timer's rate */
    uint64_t hz = (tsc - remeasure.start_tsc) * PM_TIMER_HZ / remeasure.pm_total;
    uint64_t was = tsc_hz, at = tsc_now();
    uint64_t f = spin_lock(&conv_lock);
    conv_set(at, ns_of(at), hz);
    spin_unlock(&conv_lock, f);
    int64_t ppm = ((int64_t)hz - (int64_t)was) * 1000000 / (int64_t)hz;
    kprintf("clock: TSC %lu.%06lu MHz over %u s of the PM timer (%ld ppm from the boot's)\n",
            hz / 1000000, hz % 1000000, CLOCK_REMEASURE_S, (long)ppm);
}

/* ---- the CPUs' TSCs against each other ----
   Every CPU comparing reads its TSC, COMPARE_ROUNDS times, against the highest reading any of
   them has published, and publishes its own when it is higher. A reading below one that another
   CPU published before this one read its own is a TSC behind that one's, by at least the
   difference: the clock could go back between them. Each CPU takes the most it saw behind as
   what its readings are short by, and all compare once more with that added; what any still sees
   behind then is a disagreement the offsets did not take away. All compare at once: the line of
   the highest reading crosses once a reading, so the time grows with the CPUs, not their pairs,
   and every CPU is compared with every other one that published while it read. */
static uint64_t highest __attribute__((aligned(64))); /* the highest reading of this pass */
static uint32_t compare_pass __attribute__((aligned(64))); /* 1, 2: the pass on; 0 between */
static struct {
    uint32_t cpus, through;    /* comparing; through this pass */
    uint32_t short_cpus;       /* whose readings were short */
    uint64_t most_short, still; /* the most added to a CPU's readings; the most seen behind after */
} __attribute__((aligned(64))) compare;

static void raise_to(uint64_t *word, uint64_t value) {
    uint64_t was = __atomic_load_n(word, __ATOMIC_RELAXED);
    while (value > was && !__atomic_compare_exchange_n(word, &was, value, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

/* The TSC read once the loads before it are done: RDTSCP waits for every older instruction (SDM
   Vol. 2B RDTSCP, APM Vol. 3 RDTSCP), on either vendor, where LFENCE orders RDTSC on AMD only
   when made dispatch serializing. */
static inline uint64_t tsc_read_after(void) {
    uint32_t lo, hi, aux;
    __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux) : : "memory");
    return (uint64_t)hi << 32 | lo;
}

/* One pass on this CPU: the most its readings (its offset taken off) were behind one published
   before them. */
static uint64_t compare_readings(void) {
    uint64_t most = 0;
    int64_t offset = this_cpu(tsc_offset);
    for (unsigned i = 0; i < COMPARE_ROUNDS; i++) {
        uint64_t seen = __atomic_load_n(&highest, __ATOMIC_ACQUIRE);
        uint64_t own = tsc_read_after() - (uint64_t)offset;
        if (own < seen) {
            if (seen - own > most) most = seen - own;
        } else if (own > seen) {
            __atomic_compare_exchange_n(&highest, &seen, own, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
        }
    }
    return most;
}

static void compare_through(void) { __atomic_fetch_add(&compare.through, 1, __ATOMIC_ACQ_REL); }

/* This CPU's first pass: what its readings were short by is added to them. */
static void compare_first(void) {
    uint64_t short_by = compare_readings();
    if (short_by) {
        this_cpu(tsc_offset) = -(int64_t)short_by;
        __atomic_fetch_add(&compare.short_cpus, 1, __ATOMIC_RELAXED);
        raise_to(&compare.most_short, short_by);
        __atomic_store_n(&tsc_offsets, true, __ATOMIC_RELEASE);
    }
    compare_through();
}

/* Its second, with that added. */
static void compare_second(void) {
    raise_to(&compare.still, compare_readings());
    compare_through();
}

/* The lead's wait for every CPU comparing to be through the pass. */
static void compare_wait(void) {
    while (__atomic_load_n(&compare.through, __ATOMIC_ACQUIRE) != compare.cpus) pause();
}

void clock_cpu_compare(void) {
    if (source != SOURCE_TSC) return;
    while (__atomic_load_n(&compare_pass, __ATOMIC_ACQUIRE) != 1) pause();
    compare_first();
    while (__atomic_load_n(&compare_pass, __ATOMIC_ACQUIRE) != 2) pause();
    compare_second();
}

void clock_cpus_compare(unsigned cpus) {
    if (source != SOURCE_TSC || cpus < 2) return;
    compare.cpus = cpus;
    compare.through = compare.short_cpus = 0;
    compare.most_short = compare.still = 0;
    __atomic_store_n(&highest, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&compare_pass, 1, __ATOMIC_RELEASE);
    compare_first();
    compare_wait();
    compare.through = 0;
    __atomic_store_n(&highest, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&compare_pass, 2, __ATOMIC_RELEASE);
    compare_second();
    compare_wait();
    __atomic_store_n(&compare_pass, 0, __ATOMIC_RELEASE);
    if (compare.short_cpus)
        kprintf("clock: %u of %u CPUs read their TSCs behind, by up to %lu counts; added to their readings\n",
                compare.short_cpus, cpus, compare.most_short);
    if (compare.still)
        kprintf("clock: the TSCs still disagree by up to %lu counts: the clock can go back by that much between CPUs\n",
                compare.still);
    else if (!compare.short_cpus)
        kprintf("clock: the TSCs of %u CPUs agree\n", cpus);
}

/* ---- boot and sleep ---- */

static uint8_t cmos(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static int bcd(int v) { return (v & 0x0F) + (v >> 4) * 10; }

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

int64_t rtc_read_unix(void) {
    while (cmos(0x0A) & 0x80) pause();
    int sec = cmos(0x00), min = cmos(0x02), hour = cmos(0x04);
    int day = cmos(0x07), mon = cmos(0x08), year = cmos(0x09);
    int century = cmos(0x32);
    uint8_t b = cmos(0x0B);
    if (!(b & 0x04)) {
        sec = bcd(sec);
        min = bcd(min);
        bool pm = hour & 0x80;
        hour = bcd(hour & 0x7F) | (pm ? 0x80 : 0);
        day = bcd(day);
        mon = bcd(mon);
        year = bcd(year);
        century = bcd(century);
    }
    /* 12-hour mode counts 12, 1 .. 11 with bit 7 for PM: 12 AM is hour 0 and 12 PM is hour 12 */
    if (!(b & 0x02)) hour = (hour & 0x7F) % 12 + (hour & 0x80 ? 12 : 0);
    hour &= 0x7F;
    int full_year = (century >= 19 && century <= 30) ? century * 100 + year : 2000 + year;
    return days_from_civil(full_year, mon, day) * 86400 + hour * 3600 + min * 60 + sec;
}

void clock_init(void) {
    uint64_t ns;
    bool remeasuring = false;
    if (vm_clock_ns(&ns)) {
        source = SOURCE_HYPERVISOR;
        hv_base = ns;
        tsc_hz = vm_tsc_hz();
        if (tsc_hz) kprintf("clock: the hypervisor's reference time, TSC %lu kHz by its word\n", tsc_hz / 1000);
        else kprintf("clock: the hypervisor's reference time\n");
    } else {
        uint64_t hz = vm_tsc_hz();
        rate_from = "the hypervisor's word";
        if (!hz && (hz = cpu_features.tsc_crystal_hz)) rate_from = "CPUID 15h";
        if (!hz) {
            pm_timer_find();
            if (pm_port && (hz = median_of(measure_pm))) {
                rate_from = "the PM timer";
                remeasuring = true;
            }
        }
        if (!hz && (hz = median_of(measure_pit))) rate_from = "the PIT";
        if (!hz) panic("clock: no TSC rate: no hypervisor, CPUID, PM timer or PIT gives one");
        source = SOURCE_TSC;
        uint64_t f = spin_lock(&conv_lock);
        conv_set(tsc_read(), 0, hz);
        spin_unlock(&conv_lock, f);
        kprintf("clock: TSC %lu kHz by %s%s\n", hz / 1000, rate_from,
                cpu_features.invariant_tsc ? "" : "; not invariant, so the clock drifts as its rate changes");
    }
    wall_offset_ns = rtc_read_unix() * (int64_t)NS_PER_S - (int64_t)clock_now_ns();
    if (remeasuring) {
        remeasure.start_tsc = tsc_read();
        remeasure.pm_last = pm_read();
        alarm_init(&remeasure.alarm, remeasure_fire);
        alarm_set(&remeasure.alarm, clock_after(NS_PER_S));
    }
}

static uint64_t stopped_ns;
static int64_t rtc_before;

void clock_suspend(void) {
    stopped_ns = clock_now_ns();
    rtc_before = rtc_read_unix();
}

int64_t clock_resume(void) {
    vm_clock_resume();
    if (source == SOURCE_TSC) {
        uint64_t f = spin_lock(&conv_lock);
        conv_set(tsc_read(), stopped_ns, tsc_hz);
        spin_unlock(&conv_lock, f);
    } else if (source == SOURCE_HYPERVISOR) {
        uint64_t ns;
        if (vm_clock_ns(&ns)) hv_base = ns - stopped_ns;
    }
    /* the other CPUs start again (smp_resume), and all compare afresh */
    __atomic_store_n(&tsc_offsets, false, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < topology.cpus; i++) per_cpu(tsc_offset, i) = 0;
    int64_t slept = rtc_read_unix() - rtc_before;
    if (slept < 0) slept = 0;
    __atomic_fetch_add(&wall_offset_ns, slept * (int64_t)NS_PER_S, __ATOMIC_RELAXED);
    return slept;
}

/* ---- the wall clock ---- */

int64_t clock_wall_ns(void) { return (int64_t)clock_now_ns() + __atomic_load_n(&wall_offset_ns, __ATOMIC_RELAXED); }

int64_t clock_wall_seconds(void) {
    int64_t ns = clock_wall_ns();
    return ns >= 0 ? ns / (int64_t)NS_PER_S : -((-ns + (int64_t)NS_PER_S - 1) / (int64_t)NS_PER_S);
}

void clock_set_wall(int64_t unix_ns) {
    __atomic_store_n(&wall_offset_ns, unix_ns - (int64_t)clock_now_ns(), __ATOMIC_RELAXED);
}

void time_to_parts(int64_t t, struct tm_parts *p) {
    int64_t days = t / 86400;
    int64_t rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
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
