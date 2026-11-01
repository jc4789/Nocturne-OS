#pragma once
#include <stddef.h>
#include <stdint.h>
#include <cdefs.h>
NOCTURNE_BEGIN_DECLS
typedef int64_t time_t;
struct timespec { time_t tv_sec; long tv_nsec; };
/* ISO C timespec_get bases: the wall clock and the monotonic uptime clock. */
#define TIME_UTC 1
#define TIME_MONOTONIC 2
int timespec_get(struct timespec *ts, int base);
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
};
time_t time(time_t *t);
/* Processor time: the CPU time of the calling app's threads since it started, in CLOCKS_PER_SEC
   units. clock_t is a count of ns. */
typedef int64_t clock_t;
#define CLOCKS_PER_SEC ((clock_t)1000000000)
clock_t clock(void);
/* No time zone is known: the clock keeps local time, and localtime breaks a time down as gmtime
   does. The _r forms fill the caller's tm and return it, or NULL (errno EOVERFLOW) when the year
   does not fit tm_year; the others return one buffer that the next call of either reuses. */
struct tm *localtime(const time_t *t);
struct tm *gmtime(const time_t *t);
struct tm *localtime_r(const time_t *t, struct tm *result);
struct tm *gmtime_r(const time_t *t, struct tm *result);
size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm);
time_t mktime(struct tm *tm);
double difftime(time_t t1, time_t t0);
/* The text of tm in ISO C's fixed layout, "Sun Sep 16 01:03:52 1973\n", in
   one buffer that the next call reuses; ctime is asctime(localtime(t)). */
char *asctime(const struct tm *tm);
char *ctime(const time_t *t);
NOCTURNE_END_DECLS
