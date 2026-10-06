#pragma once
#include <stdint.h>
struct tm_parts {
    int year, mon, mday, hour, min, sec, wday;
};
void timer_init(void);
int64_t rtc_read_unix(void);
int64_t time_now(void);
void time_set(int64_t unix_time); /* set the wall clock */
void time_to_parts(int64_t t, struct tm_parts *p);
int64_t time_from_parts(const struct tm_parts *p);
