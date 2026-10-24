#pragma once
#include <stdbool.h>
#include <stdint.h>
struct tm_parts {
    int year, mon, mday, hour, min, sec, wday;
};
void timer_init(void);
uint64_t timer_now_ms(void); /* uptime_ms caught up with the clock (BSP), for deadlines */
/* SYS_CLOCK_INFO's sample: uptime_ms now and the TSC and reference time (100 ns) at it */
void timer_clock_sample(uint64_t *ms, uint64_t *tsc, uint64_t *reference_100ns);
int64_t rtc_read_unix(void);
int64_t time_now(void);
void time_set(int64_t unix_time); /* set the wall clock */
void time_to_parts(int64_t t, struct tm_parts *p);
int64_t time_from_parts(const struct tm_parts *p);
/* Tickless idle (dev/timer.c). */
extern volatile bool tick_stopped;
bool timer_tickless(void); /* the BSP may stop its tick when idle (APs keep theirs) */
void timer_idle(void);     /* BSP idle task, interrupts off: maybe stop the tick */
void timer_wake(void);     /* BSP, any interrupt while tick_stopped: tick again */
/* Sleep: uptime_ms leaves the time asleep out, the wall clock takes it from the RTC. suspend before
   S3, resume after it with interrupts off (the TSC may have started again from 0). resume returns
   the seconds slept. */
void timer_suspend(void);
int64_t timer_resume(void);
