/* The platform's reference clocks (dev/refclock.h): the HPET and the CMOS RTC.

   The HPET (IA-PC HPET 1.0a): its block of registers at the BASE_ADDRESS of the ACPI table
   "HPET" (3.2.4), mapped uncached. The main counter (2.3.7, offset 0F0h) counts up at
   COUNTER_CLK_PERIOD femtoseconds a count (2.3.4: not 0, at most 05F5E100h), and only while
   ENABLE_CNF is set (2.3.5: "If this bit is 0, then the main counter will halt"), which this sets
   by read-modify-write. A 64-bit counter (COUNT_SIZE_CAP) is read as two 32-bit halves, the high
   one before and after the low one (2.4.7: a 64-bit read may be split by the platform); a 32-bit
   one wraps, and is read at least every quarter of its wrap, so its count runs on past the wraps
   without doubt. Its readings may be 2 counts early or late (2.4.1 rule 2). Its registers, the
   counter among them, are not kept through S3 (2.3.3 rule 1): ENABLE_CNF is set again after it,
   and a new count begins. It is the clock's rate reference: each reading, between two TSC
   readings, is a sample given to clock_rate_sample when the clock wants one, from an alarm set
   for the moment it answers.

   The RTC: the MC146818-compatible CMOS clock at ports 70h (index) and 71h (data), registers 00h
   seconds, 02h minutes, 04h hours, 07h day, 08h month, 09h year, 0Ah status A (bit 7 UIP: an
   update in progress or about to start) and 0Bh status B (bit 2: binary rather than BCD, bit 1:
   24-hour rather than 12-hour); the century at the index the FADT's CENTURY names (ACPI 6.5
   4.8.2.4 table 4.10: 0 when there is none), in the RTC's own BCD or binary. */
#include "kernel.h"
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

/* The HPET as the clock's rate reference: one CPU at a time touches this, the alarm's fire, or
   CPU 0 starting it at boot or after a sleep, before alarms come. */
static struct {
    struct clock_rate_reference ref;
    uint64_t (*read)(uint64_t *ta, uint64_t *tb, uint32_t *cpu);
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

/* The counter read again while each try is narrower than the last (a strictly smaller width each
   time, so it ends), the count run on by the narrowest; then given to the clock when it wants it,
   and the alarm set for its next wish, or sooner where the counter would wrap past doubt first.
   A 64th of the wait late is as good (the alarm shares an interrupt then). Interrupts off. */
static void hpet_sample(void) {
    struct clock_rate_sample s = {.jitter = HPET_JITTER};
    uint64_t width = UINT64_MAX, raw = 0;
    for (;;) {
        uint64_t ta, tb;
        uint32_t cpu;
        uint64_t r = rate.read(&ta, &tb, &cpu);
        if (tb - ta >= width) break;
        width = tb - ta;
        s.tsc_a = ta;
        s.tsc_b = tb;
        s.cpu = cpu;
        raw = r;
    }
    if (!rate.fresh) rate.count += (raw - rate.raw) & rate.mask;
    rate.fresh = false;
    rate.raw = raw;
    s.count = rate.count;
    uint64_t now = clock_now_ns();
    if (now >= rate.wanted) rate.wanted = clock_rate_sample(&rate.ref, &s);
    if (rate.wanted == CLOCK_NEVER) return;
    uint64_t wait = rate.wanted > now ? rate.wanted - now : 0;
    if (wait > rate.span_ns) wait = rate.span_ns;
    uint64_t at = now + wait, late = wait / 64;
    alarm_set_range(&rate.alarm, at, late > CLOCK_NEVER - at ? CLOCK_NEVER : at + late);
}

static void hpet_fire(struct alarm *a, struct regs *r) {
    (void)a;
    (void)r;
    hpet_sample();
}

/* After a sleep (clock_resume): the counter on again, a new count, and its first sample now. */
static void hpet_resume(void) {
    hpet_enable();
    rate.fresh = true;
    rate.wanted = 0;
    hpet_sample();
}

bool refclock_hpet_start(void) {
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
                                             .resume = hpet_resume};
    rate.read = wide ? hpet_read64 : hpet_read32;
    rate.mask = wide ? UINT64_MAX : UINT32_MAX;
    /* a quarter of a 32-bit counter's wrap: 2^30 counts of at most 10^8 fs, under 2^57 fs */
    rate.span_ns = wide ? CLOCK_NEVER : ((uint64_t)period << 30) / (FS_PER_S / NS_PER_S);
    rate.fresh = true;
    rate.wanted = 0;
    alarm_init(&rate.alarm, hpet_fire);
    kprintf("refclock: HPET at %lx, %u bits, %lu.%06lu MHz\n", t->base.address, wide ? 64 : 32,
            FS_PER_S / period / 1000000, FS_PER_S / period % 1000000);
    hpet_sample();
    return true;
}

/* ---- the RTC ---- */

#define RTC_SECONDS 0x00
#define RTC_MINUTES 0x02
#define RTC_HOURS 0x04
#define RTC_DAY 0x07
#define RTC_MONTH 0x08
#define RTC_YEAR 0x09
#define RTC_STATUS_A 0x0A
#define RTC_STATUS_B 0x0B
#define RTC_UIP 0x80
#define RTC_BINARY 0x04
#define RTC_24H 0x02
#define RTC_PM 0x80              /* in the hours register, 12-hour mode */
#define FADT_BOOT_NO_CMOS_RTC (1u << 5) /* IAPC_BOOT_ARCH, ACPI 6.5 5.2.9.3 */

static struct spinlock rtc_lock; /* the index and data ports: a leaf */
static enum { RTC_UNKNOWN, RTC_HERE, RTC_NONE } rtc_state;
static uint8_t century_index;

static uint8_t cmos(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
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
