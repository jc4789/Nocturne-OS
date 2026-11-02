/* The platform's reference clocks (dev/refclock.h): the HPET, the ACPI PM timer and the CMOS RTC.

   The HPET (IA-PC HPET 1.0a): its block of registers at the BASE_ADDRESS of the ACPI table
   "HPET" (3.2.4), mapped uncached. The main counter (2.3.7, offset 0F0h) counts up at
   COUNTER_CLK_PERIOD femtoseconds a count (2.3.4: not 0, at most 05F5E100h), and only while
   ENABLE_CNF is set (2.3.5: "If this bit is 0, then the main counter will halt"), which this sets
   by read-modify-write. A 64-bit counter (COUNT_SIZE_CAP) is read as two 32-bit halves, the high
   one before and after the low one (2.4.7: a 64-bit read may be split by the platform); a 32-bit
   one wraps, and is read at least every quarter of its wrap, so its count runs on past the wraps
   without doubt. Its readings may be 2 counts early or late (2.4.1 rule 2). Its registers, the
   counter among them, are not kept through S3 (2.3.3 rule 1): ENABLE_CNF is set again after it,
   and a new count begins.

   The ACPI PM timer (ACPI 6.5 4.8.2.1, 4.8.3.3): a free-running count-up timer at 3.579545 MHz,
   read-only, read as 32 bits at the FADT's X_PM_TMR_BLK (I/O or memory space) or else PM_TMR_BLK
   (I/O space, PM_TMR_LEN 4), 32 bits wide where TMR_VAL_EXT says so and 24 otherwise. Both blocks are
   optional, and a hardware-reduced platform's are to be ignored (5.2.9). It counts in S0 and may
   begin from any value after another state. ACPI states no error for a reading.

   The clock's rate reference is the first of these two that is there and counts, the HPET before
   the PM timer: it is read in one memory access, and the timer's run off the same 14.31818 MHz.
   Each reading, between two TSC readings, is a sample given to clock_rate_sample when the clock
   wants one, from an alarm set for the moment it answers.

   The RTC: the MC146818-compatible CMOS clock at ports 70h (index) and 71h (data), registers 00h
   seconds, 02h minutes, 04h hours, 07h day, 08h month, 09h year, 0Ah status A (bit 7 UIP: an
   update in progress or about to start) and 0Bh status B (bit 2: binary rather than BCD, bit 1:
   24-hour rather than 12-hour); the century at the index the FADT's CENTURY names (ACPI 6.5
   4.8.2.4 table 4.10: 0 when there is none), in the RTC's own BCD or binary. */
#include "kernel.h"
#include "abi.h"
#include "arch/cpu.h"
#include "arch/spin.h"
#include "dev/acpi.h"
#include "dev/refclock.h"
#include "mm/vmm.h"
#include "sys/alarm.h"
#include "sys/clock.h"

/* ---- the HPET ---- */

struct acpi_hpet { /* IA-PC HPET 1.0a 3.2.4 */
    struct acpi_sdt h;
    uint32_t block_id;
    struct acpi_gas base;
    uint8_t number;
    uint16_t min_tick;
    uint8_t protection;
} __attribute__((packed));

#define HPET_BLOCK 1024        /* its registers' span (2.3.1: 000h-3FFh) */
#define HPET_CAPS_LO 0x000     /* REV_ID 7:0, COUNT_SIZE_CAP 13 */
#define HPET_CAPS_HI 0x004     /* COUNTER_CLK_PERIOD */
#define HPET_CONFIG 0x010
#define HPET_COUNTER_LO 0x0F0
#define HPET_COUNTER_HI 0x0F4
#define HPET_REV_ID 0xFFu
#define HPET_COUNT_SIZE_CAP (1u << 13)
#define HPET_ENABLE_CNF 1u
#define HPET_PERIOD_MAX 0x05F5E100u
#define HPET_JITTER 2          /* 2.4.1 rule 2 */
#define FS_PER_S ((uint64_t)1000000000000000)

static volatile uint32_t *hpet;

/* The rate reference: one CPU at a time touches this, the alarm's fire, or CPU 0 starting it at
   boot or after a sleep, before alarms come. */
static struct {
    struct clock_rate_reference ref;
    uint64_t (*read)(uint64_t *ta, uint64_t *tb, uint32_t *cpu);
    uint32_t jitter;     /* counts a reading may be early or late */
    uint64_t mask;       /* its readings: modulo mask + 1 */
    uint64_t span_ns;    /* the longest time between readings: a quarter of its wrap */
    uint64_t raw, count; /* the last reading, and the count run on past the wraps at it */
    bool fresh;          /* the next reading starts the count (at the start, after a sleep) */
    uint64_t wanted;     /* when the clock wants the next sample; CLOCK_NEVER: none */
    struct alarm alarm;
} rate;

static inline uint32_t hpet_reg(uint32_t offset) { return mmio_read32(&hpet[offset / 4]); }

static void hpet_enable(void) {
    uint32_t config = hpet_reg(HPET_CONFIG);
    if (!(config & HPET_ENABLE_CNF)) mmio_write32(&hpet[HPET_CONFIG / 4], config | HPET_ENABLE_CNF);
}

/* One reading between two of this CPU's TSC readings, interrupts off. */
static uint64_t hpet_read32(uint64_t *ta, uint64_t *tb, uint32_t *cpu) {
    *ta = clock_tsc_read(cpu);
    uint32_t count = hpet_reg(HPET_COUNTER_LO);
    *tb = clock_tsc_read(NULL);
    return count;
}

/* The high half the same before and after the low one: the low one did not wrap between, and
   belongs to it. It wraps once in 2^32 counts, so a second try finds it settled. */
static uint64_t hpet_read64(uint64_t *ta, uint64_t *tb, uint32_t *cpu) {
    for (;;) {
        uint32_t high = hpet_reg(HPET_COUNTER_HI);
        *ta = clock_tsc_read(cpu);
        uint32_t low = hpet_reg(HPET_COUNTER_LO);
        *tb = clock_tsc_read(NULL);
        if (hpet_reg(HPET_COUNTER_HI) == high) return (uint64_t)high << 32 | low;
    }
}

static uint16_t pm_port;           /* where the timer is, in I/O space */
static volatile uint32_t *pm_mmio; /* or in memory */

static uint64_t pm_read(uint64_t *ta, uint64_t *tb, uint32_t *cpu) {
    *ta = clock_tsc_read(cpu);
    uint32_t count = pm_mmio ? mmio_read32(pm_mmio) : inl(pm_port);
    *tb = clock_tsc_read(NULL);
    return count;
}

/* The counter read again while each try is narrower than the last (a strictly smaller width each
   time, so it ends), the count run on by the narrowest. Interrupts off. */
static void rate_read(struct clock_rate_sample *s) {
    uint64_t width = UINT64_MAX, raw = 0;
    s->jitter = rate.jitter;
    for (;;) {
        uint64_t ta, tb;
        uint32_t cpu;
        uint64_t r = rate.read(&ta, &tb, &cpu);
        if (tb - ta >= width) break;
        width = tb - ta;
        s->tsc_a = ta;
        s->tsc_b = tb;
        s->cpu = cpu;
        raw = r;
    }
    if (!rate.fresh) rate.count += (raw - rate.raw) & rate.mask;
    rate.fresh = false;
    rate.raw = raw;
    s->count = rate.count;
}

/* A reading, given to the clock when it wants it, and the alarm set for its next wish, or sooner
   where the counter would wrap past doubt first. A 64th of the wait late is as good (the alarm
   shares an interrupt then). Interrupts off. */
static void rate_sample(void) {
    struct clock_rate_sample s;
    rate_read(&s);
    uint64_t now = clock_now_ns();
    if (now >= rate.wanted) rate.wanted = clock_rate_sample(&rate.ref, &s);
    if (rate.wanted == CLOCK_NEVER) return;
    uint64_t wait = rate.wanted > now ? rate.wanted - now : 0;
    if (wait > rate.span_ns) wait = rate.span_ns;
    uint64_t at = now + wait, late = wait / 64;
    alarm_set_range(&rate.alarm, at, late > CLOCK_NEVER - at ? CLOCK_NEVER : at + late);
}

static void rate_fire(struct alarm *a, struct regs *r) {
    (void)a;
    (void)r;
    rate_sample();
}

/* At the start and after a sleep: the counter on, a new count, and its first sample now. */
static void rate_begin(void) {
    if (hpet) hpet_enable();
    rate.fresh = true;
    rate.wanted = 0;
    rate_sample();
}

/* The device just found counts: a counter that does not move past any count's period, whatever
   the TSC's rate (2^26 counts, 6 ms at 10 GHz; the slowest here counts at 3.58 MHz), is none. */
#define COUNTING_TSC (1ull << 26)
static bool rate_counting(void) {
    struct clock_rate_sample a, b;
    rate.fresh = true;
    rate_read(&a);
    do {
        rate_read(&b);
        if (b.count != a.count) return true;
    } while (b.tsc_b - a.tsc_a < COUNTING_TSC);
    return false;
}

static bool hpet_find(void) {
    const struct acpi_hpet *t = acpi_find("HPET", 0);
    if (!t) return false;
    if (t->h.length < sizeof *t || t->base.space != 0 || !t->base.address) {
        kprintf("refclock: the HPET table names no block in memory space\n");
        return false;
    }
    hpet = vmm_map_mmio(t->base.address, HPET_BLOCK);
    if (!hpet) return false;
    uint32_t caps = hpet_reg(HPET_CAPS_LO), period = hpet_reg(HPET_CAPS_HI);
    if (!(caps & HPET_REV_ID) || !period || period > HPET_PERIOD_MAX) {
        kprintf("refclock: the HPET at %lx is not valid (capabilities %x, period %u fs)\n",
                t->base.address, caps, period);
        hpet = NULL;
        return false;
    }
    hpet_enable();
    bool wide = caps & HPET_COUNT_SIZE_CAP;
    rate.ref = (struct clock_rate_reference){.name = "the HPET", .hz_num = FS_PER_S, .hz_den = period,
                                             .read = rate_read, .begin = rate_begin};
    rate.read = wide ? hpet_read64 : hpet_read32;
    rate.jitter = HPET_JITTER;
    rate.mask = wide ? UINT64_MAX : UINT32_MAX;
    /* a quarter of a 32-bit counter's wrap: 2^30 counts of at most 10^8 fs, under 2^57 fs */
    rate.span_ns = wide ? CLOCK_NEVER : ((uint64_t)period << 30) / (FS_PER_S / NS_PER_S);
    if (!rate_counting()) {
        kprintf("refclock: the HPET at %lx does not count\n", t->base.address);
        hpet = NULL;
        return false;
    }
    kprintf("refclock: HPET at %lx, %u bits, %lu.%06lu MHz\n", t->base.address, wide ? 64 : 32,
            FS_PER_S / period / 1000000, FS_PER_S / period % 1000000);
    return true;
}

#define PM_TIMER_HZ 3579545u
#define GAS_MEMORY 0
#define GAS_IO 1

static bool pm_find(void) {
    const struct acpi_fadt *f = acpi_find("FACP", 0);
    if (!f || f->h.length < offsetof(struct acpi_fadt, reset_reg) || (f->flags & FADT_HW_REDUCED)) return false;
    const struct acpi_gas *x = f->h.length >= offsetof(struct acpi_fadt, x_gpe0) ? &f->x_pm_tmr : NULL;
    uint64_t at = 0;
    bool memory = false;
    if (x && x->address && (x->space == GAS_MEMORY || (x->space == GAS_IO && x->address <= UINT16_MAX))) {
        at = x->address;
        memory = x->space == GAS_MEMORY;
    } else if (f->pm_tmr && f->pm_tmr_len == 4 && f->pm_tmr <= UINT16_MAX) {
        at = f->pm_tmr;
    }
    if (!at) return false;
    if (memory && !(pm_mmio = vmm_map_mmio(at, sizeof *pm_mmio))) return false;
    if (!memory) pm_port = (uint16_t)at;
    bool wide = f->flags & FADT_TMR_VAL_EXT;
    rate.ref = (struct clock_rate_reference){.name = "the ACPI PM timer", .hz_num = PM_TIMER_HZ, .hz_den = 1,
                                             .read = rate_read, .begin = rate_begin};
    rate.read = pm_read;
    rate.jitter = 0;
    rate.mask = wide ? UINT32_MAX : 0xFFFFFF;
    rate.span_ns = (rate.mask + 1) / 4 * NS_PER_S / PM_TIMER_HZ;
    if (!rate_counting()) {
        kprintf("refclock: the ACPI PM timer at %s %lx does not count\n", memory ? "memory" : "port", at);
        pm_mmio = NULL;
        return false;
    }
    kprintf("refclock: ACPI PM timer at %s %lx, %u bits, 3.579545 MHz\n", memory ? "memory" : "port", at, wide ? 32 : 24);
    return true;
}

const struct clock_rate_reference *refclock_rate_reference(void) {
    if (!hpet_find() && !pm_find()) return NULL;
    alarm_init(&rate.alarm, rate_fire);
    return &rate.ref;
}

/* ---- the RTC ---- */

#define RTC_SECONDS 0x00
#define RTC_MINUTES 0x02
#define RTC_HOURS 0x04
#define RTC_WEEKDAY 0x06
#define RTC_DAY 0x07
#define RTC_MONTH 0x08
#define RTC_YEAR 0x09
#define RTC_STATUS_A 0x0A
#define RTC_STATUS_B 0x0B
#define RTC_UIP 0x80
#define RTC_BINARY 0x04
#define RTC_24H 0x02
#define RTC_PM 0x80              /* in the hours register, 12-hour mode */
#define RTC_SET 0x80             /* status register B: the date is being written, no update runs */
#define FADT_BOOT_NO_CMOS_RTC (1u << 5) /* IAPC_BOOT_ARCH, ACPI 6.5 5.2.9.3 */

static struct spinlock rtc_lock; /* the index and data ports: a leaf */
static enum { RTC_UNKNOWN, RTC_HERE, RTC_NONE } rtc_state;
static uint8_t century_index;

static uint8_t cmos(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static void cmos_put(uint8_t reg, uint8_t value) {
    outb(0x70, reg);
    outb(0x71, value);
}

static void rtc_find(void) {
    const struct acpi_fadt *f = acpi_find("FACP", 0);
    rtc_state = RTC_HERE;
    if (!f) return;
    if (f->h.length >= 109) century_index = f->century;
    /* IAPC_BOOT_ARCH is there from ACPI 2.0 (FADT revision 3) on */
    if (f->h.revision >= 3 && f->h.length >= 111 && (f->boot_arch & FADT_BOOT_NO_CMOS_RTC)) {
        kprintf("refclock: the FADT says there is no CMOS RTC\n");
        rtc_state = RTC_NONE;
    }
}

static int from_bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

enum rtc_result rtc_read(int64_t *unix_seconds) {
    if (rtc_state == RTC_UNKNOWN) rtc_find();
    if (rtc_state == RTC_NONE) return RTC_ABSENT;
    uint8_t sec = 0, min = 0, hour = 0, day = 0, mon = 0, year = 0, century = 0, b = 0;
    uint64_t f = spin_lock(&rtc_lock);
    bool whole = false;
    if (!(cmos(RTC_STATUS_A) & RTC_UIP)) {
        sec = cmos(RTC_SECONDS);
        min = cmos(RTC_MINUTES);
        hour = cmos(RTC_HOURS);
        day = cmos(RTC_DAY);
        mon = cmos(RTC_MONTH);
        year = cmos(RTC_YEAR);
        if (century_index) century = cmos(century_index);
        b = cmos(RTC_STATUS_B);
        /* no update began during the reads, and none came and went between them */
        whole = !(cmos(RTC_STATUS_A) & RTC_UIP) && cmos(RTC_SECONDS) == sec;
    }
    spin_unlock(&rtc_lock, f);
    if (!whole) return RTC_UPDATING;
    bool pm = !(b & RTC_24H) && (hour & RTC_PM);
    hour &= (uint8_t)~RTC_PM;
    int s = sec, mi = min, h = hour, d = day, mo = mon, y = year, c = century;
    if (!(b & RTC_BINARY)) {
        s = from_bcd(sec);
        mi = from_bcd(min);
        h = from_bcd(hour);
        d = from_bcd(day);
        mo = from_bcd(mon);
        y = from_bcd(year);
        c = from_bcd(century);
    }
    /* 12-hour mode counts 12, 1 .. 11: 12 AM is hour 0, 12 PM hour 12 */
    if (!(b & RTC_24H)) h = h % 12 + (pm ? 12 : 0);
    if (s > 59 || mi > 59 || h > 23 || d < 1 || d > 31 || mo < 1 || mo > 12 || y > 99 || c > 99)
        return RTC_ABSENT;
    struct tm_parts date = {.year = century_index && c ? c * 100 + y : 2000 + y,
                            .mon = mo, .mday = d, .hour = h, .min = mi, .sec = s};
    *unix_seconds = time_from_parts(&date);
    return RTC_READ;
}

static uint8_t to_bcd(int v) { return (uint8_t)((v / 10) << 4 | (v % 10)); }

int rtc_write(int64_t unix_seconds) {
    if (rtc_state == RTC_UNKNOWN) rtc_find();
    if (rtc_state == RTC_NONE) return -N_ERR_UNSUPPORTED;
    struct tm_parts p;
    time_to_parts(unix_seconds, &p);
    if (p.year < 2000 || (!century_index && p.year > 2099) || p.year > 9999) return -N_ERR_INVALID;
    uint64_t f = spin_lock(&rtc_lock);
    uint8_t b = cmos(RTC_STATUS_B);
    int hour = p.hour;
    uint8_t pm = 0;
    if (!(b & RTC_24H)) { /* 12, 1 .. 11, as rtc_read counts them */
        pm = hour >= 12 ? RTC_PM : 0;
        hour = hour % 12 ? hour % 12 : 12;
    }
    #define RTC_FIELD(v) ((b & RTC_BINARY) ? (uint8_t)(v) : to_bcd(v))
    cmos_put(RTC_STATUS_B, b | RTC_SET);
    cmos_put(RTC_SECONDS, RTC_FIELD(p.sec));
    cmos_put(RTC_MINUTES, RTC_FIELD(p.min));
    cmos_put(RTC_HOURS, RTC_FIELD(hour) | pm);
    cmos_put(RTC_WEEKDAY, RTC_FIELD(p.wday + 1)); /* 1 is Sunday */
    cmos_put(RTC_DAY, RTC_FIELD(p.mday));
    cmos_put(RTC_MONTH, RTC_FIELD(p.mon));
    cmos_put(RTC_YEAR, RTC_FIELD(p.year % 100));
    if (century_index) cmos_put(century_index, RTC_FIELD(p.year / 100));
    #undef RTC_FIELD
    cmos_put(RTC_STATUS_B, b);
    spin_unlock(&rtc_lock, f);
    return 0;
}
