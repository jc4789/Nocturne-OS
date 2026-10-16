#pragma once
#include <stddef.h>
#include <stdint.h>
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
struct tm *localtime(const time_t *t);
struct tm *gmtime(const time_t *t);
size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm);
time_t mktime(struct tm *tm);
