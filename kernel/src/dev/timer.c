/* PIT timer (1 kHz) and CMOS real-time clock. */
#include "kernel.h"
#include "arch/cpu.h"
#include "vm/vm.h"
#include "arch/smp.h"
#include "sys/sched.h"
#include "dev/timer.h"
#include "dev/audio.h"
#include "dev/serial.h"
#include "sys/prof.h"
#include "arch/pstate.h"

volatile uint64_t timer_ticks;
static int64_t boot_unix_time;
static uint64_t tsc_base, tsc_per_ms;
static uint64_t reference_base;
static bool reference_clock;

/* Tickless idle: with a clock to catch up from, an idle CPU stops its 1 kHz tick. CPU 0's tick
   keeps the clock's work (the count without a clock, the sound, the serial line, the deadlines),
   so it comes back by the earliest of them; another CPU's has only its queue's accounting. */
static bool tickless;
static volatile bool tick_stopped[CPU_MAX_COUNT];
/* While CPU 0's tick is stopped: the uptime_ms it comes back at by itself (0 while it ticks). A
   CPU that gives it work sooner reads it after publishing the work, and CPU 0 looks at the work
   again after setting it, so one of the two sees the other. */
static uint64_t tick_back;

/* Set the count from the clock, on any CPU: it only moves forward, whoever reads the clock last.
   False without one (then CPU 0's tick adds one each time). */
static bool catch_up(void) {
    uint64_t reference, ms;
    if (reference_clock && vm_clock_read(&reference)) ms = (reference - reference_base) / 10000;
    else if (tsc_per_ms) ms = (rdtsc() - tsc_base) / tsc_per_ms;
    else return false;
    uint64_t seen = __atomic_load_n(&timer_ticks, __ATOMIC_RELAXED);
    while (ms > seen && !__atomic_compare_exchange_n(&timer_ticks, &seen, ms, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    return true;
}

/* uptime_ms as the clock has it now rather than at the last tick, for a deadline that must not come
   early. */
uint64_t timer_now_ms(void) {
    catch_up();
    return timer_ticks;
}

/* What SYS_CLOCK_INFO gives apps that read the clock themselves: the count now, with the readings
   of the TSC and of the reference clock the count is set from at that count. An app's clock then
   reads what catch_up sets, never less than the count, so a wait the kernel ends by the count does
   not end before the app's clock says it should. */
void timer_clock_sample(uint64_t *ms, uint64_t *tsc, uint64_t *reference_100ns) {
    uint64_t now = timer_now_ms();
    *ms = now;
    *tsc = tsc_per_ms ? tsc_base + now * tsc_per_ms : rdtsc();
    if (reference_clock) *reference_100ns = reference_base + now * 10000;
    else if (!vm_clock_read(reference_100ns)) *reference_100ns = 0;
}

/* Every CPU's tick: its run queue (sched_tick) and samples. CPU 0's also counts milliseconds. An
   emulator may drop timer interrupts when it falls behind (QEMU without acceleration loses a quarter
   of them), so with a measured TSC the count is set from it instead: a late tick catches up the
   time it missed. */
static void timer_irq(struct regs *r) {
    prof_tick(r, current_task->is_idle);
    if (pstate_sampling()) pstate_tick();
    if (!cpu_current_index()) {
        if (!catch_up()) timer_ticks++;
        audio_tick();
        serial_tick();
        sched_timeouts();
    }
    sched_tick();
}

/* The frequency samples (arch/pstate.c) are ratios over TSC time: a stopped tick only makes a
   CPU's window longer while it idles, so they do not keep the tick running. */
bool timer_tickless(void) { return tickless && !prof_enabled(); }

static bool clock_work(uint64_t back) {
    return audio_needs_tick() || serial_needs_tick() || sched_next_wake() < back;
}

/* The idle task, interrupts off: with nothing due before the next tick, the tick waits instead
   (at most a second). Any interrupt starts it again (timer_wake). */
void timer_idle(void) {
    unsigned self = cpu_current_index();
    if (tick_stopped[self] || !timer_tickless()) return;
    if (self) {
        lapic_tick_once(1000);
        tick_stopped[self] = true;
        return;
    }
    catch_up();
    uint64_t now = timer_ticks, next = sched_next_wake();
    if (next <= now + 1) return; /* due by the next tick anyway */
    uint64_t back = next - now > 1000 ? now + 1000 : next;
    __atomic_store_n(&tick_back, back, __ATOMIC_SEQ_CST);
    if (clock_work(back)) {
        __atomic_store_n(&tick_back, 0, __ATOMIC_RELAXED);
        return;
    }
    lapic_tick_once((unsigned)(back - now));
    tick_stopped[0] = true;
}

void timer_wake(void) {
    unsigned self = cpu_current_index();
    if (!tick_stopped[self]) return;
    tick_stopped[self] = false;
    if (!self) __atomic_store_n(&tick_back, 0, __ATOMIC_RELAXED);
    lapic_tick_resume();
    catch_up();
}

/* After the work is published: CPU 0 ticks again if it would come back later. */
static void tick_by(uint64_t at) {
    /* the caller's work is stored before tick_back is read, as CPU 0 stores tick_back before it
       looks at the work (timer_idle) */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint64_t back = __atomic_load_n(&tick_back, __ATOMIC_SEQ_CST);
    if (back && at < back && cpu_current_index()) cpu_kick(0);
}
void timer_deadline(uint64_t at) { tick_by(at); }
void timer_tick_needed(void) { tick_by(0); }

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

void timer_init(void) {
    /* The LAPIC timer works everywhere, including Hyper-V Generation 2, which has no PIT. */
    vector_register(VEC_TIMER, timer_irq);
    vm_clock_page();
    uint64_t reference;
    reference_clock = vm_clock_read(&reference);
    if (reference_clock) reference_base = reference - timer_ticks * 10000;
    if (lapic_timer_start(1000, VEC_TIMER)) {
        if (tsc_hz >= 1000000) { /* a TSC that is known and plausible */
            tsc_per_ms = tsc_hz / 1000;
            tsc_base = rdtsc() - timer_ticks * tsc_per_ms;
        }
        tickless = (tsc_per_ms || reference_clock) && !cmdline_has("notickless");
        kprintf("timer: tickless idle %s\n", tickless ? "on (notickless turns it off)" : "off");
    } else {
        uint16_t div = 1193182 / 1000;
        outb(0x43, 0x36);
        outb(0x40, div & 0xFF);
        outb(0x40, div >> 8);
        irq_register(0, timer_irq);
        pic_unmask(0);
        kprintf("timer: PIT at 1000 Hz\n");
    }
    boot_unix_time = rtc_read_unix();
}

/* Any CPU: the wall clock is the boot's moment, which time_set moves whole, plus the uptime. */
int64_t time_now(void) {
    return __atomic_load_n(&boot_unix_time, __ATOMIC_RELAXED) + (int64_t)(uptime_ms() / 1000);
}
void time_set(int64_t t) { __atomic_store_n(&boot_unix_time, t - (int64_t)(uptime_ms() / 1000), __ATOMIC_RELAXED); }

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

static int64_t rtc_before;

void timer_suspend(void) {
    catch_up();
    rtc_before = rtc_read_unix();
}

int64_t timer_resume(void) {
    uint64_t reference;
    if (reference_clock && vm_clock_read(&reference)) reference_base = reference - timer_ticks * 10000;
    if (tsc_per_ms) tsc_base = rdtsc() - timer_ticks * tsc_per_ms;
    tick_stopped[0] = false;
    tick_back = 0;
    int64_t slept = rtc_read_unix() - rtc_before;
    if (slept < 0) slept = 0;
    boot_unix_time += slept;
    return slept;
}
