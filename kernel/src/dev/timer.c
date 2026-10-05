/* PIT timer (1 kHz) and CMOS real-time clock. */
#include "kernel.h"
#include "arch/cpu.h"
#include "sys/sched.h"
#include "dev/timer.h"

volatile uint64_t timer_ticks;
static int64_t boot_unix_time;

static void timer_irq(struct regs *r) {
    (void)r;
    timer_ticks++;
    sched_tick();
}

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
    if (!(b & 0x02) && (hour & 0x80)) hour = ((hour & 0x7F) + 12) % 24;
    hour &= 0x7F;
    int full_year = (century >= 19 && century <= 30) ? century * 100 + year : 2000 + year;
    return days_from_civil(full_year, mon, day) * 86400 + hour * 3600 + min * 60 + sec;
}

void timer_init(void) {
    uint16_t div = 1193182 / 1000;
    outb(0x43, 0x36);
    outb(0x40, div & 0xFF);
    outb(0x40, div >> 8);
    irq_register(0, timer_irq);
    pic_unmask(0);
    boot_unix_time = rtc_read_unix();
}

int64_t time_now(void) { return boot_unix_time + (int64_t)(timer_ticks / 1000); }

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
