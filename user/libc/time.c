#include <time.h>
#include "nocturne.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <wchar.h>

static bool is_leap(long long y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

struct tm *gmtime(const time_t *tp) {
    static struct tm tm;
    time_t t = *tp;
    int64_t days = t / 86400, rem = t % 86400;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    tm.tm_hour = (int)(rem / 3600);
    tm.tm_min = (int)(rem % 3600 / 60);
    tm.tm_sec = (int)(rem % 60);
    tm.tm_wday = (int)((days + 4) % 7);
    if (tm.tm_wday < 0) tm.tm_wday += 7;
    int y = 1970;
    for (;;) {
        int yd = is_leap(y) ? 366 : 365;
        if (days >= yd) {
            days -= yd;
            y++;
        } else if (days < 0) {
            y--;
            days += is_leap(y) ? 366 : 365;
        } else {
            break;
        }
    }
    tm.tm_year = y - 1900;
    tm.tm_yday = (int)days;
    int m = 0;
    for (; m < 12; m++) {
        int md = mdays[m] + (m == 1 && is_leap(y));
        if (days < md) break;
        days -= md;
    }
    tm.tm_mon = m;
    tm.tm_mday = (int)days + 1;
    tm.tm_isdst = 0;
    return &tm;
}

/* the RTC holds local time, so localtime == gmtime */
struct tm *localtime(const time_t *t) { return gmtime(t); }

/* Days from 1970-01-01 to the first of month m (1-12) of year y in the
   proleptic Gregorian calendar, counting in 400-year eras of 146097 days with
   each year starting in March, so the leap day comes last. */
static long long days_from_civil(long long y, int m) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long year_of_era = y - era * 400;
    long long day_of_year = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5;
    return era * 146097 + year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year - 719468;
}

/* ISO C: the fields may lie outside their ranges; the result is normalized
   into tm, with tm_wday and tm_yday set. */
time_t mktime(struct tm *tm) {
    long long mon = tm->tm_mon, year = tm->tm_year + 1900LL + mon / 12;
    mon %= 12;
    if (mon < 0) {
        mon += 12;
        year--;
    }
    time_t t = (days_from_civil(year, (int)mon + 1) + tm->tm_mday - 1) * 86400 + tm->tm_hour * 3600LL +
               tm->tm_min * 60LL + tm->tm_sec;
    *tm = *gmtime(&t);
    return t;
}

static const char *const wdays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *const months[] = {"January", "February", "March", "April", "May", "June", "July",
                                     "August", "September", "October", "November", "December"};
static const char *name_of(const char *const *names, int i, int n) { return i >= 0 && i < n ? names[i] : "?"; }

double difftime(time_t t1, time_t t0) {
    time_t d;
    return __builtin_sub_overflow(t1, t0, &d) ? (double)t1 - (double)t0 : (double)d;
}

/* The fixed English layout ISO C gives, "Sun Sep 16 01:03:52 1973\n". */
char *asctime(const struct tm *tm) {
    static char text[64];
    snprintf(text, sizeof text, "%.3s %.3s%3d %.2d:%.2d:%.2d %d\n", name_of(wdays, tm->tm_wday, 7),
             name_of(months, tm->tm_mon, 12), tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900);
    return text;
}
char *ctime(const time_t *t) { return asctime(localtime(t)); }

/* strftime's output: everything is counted, only what fits is stored. */
struct time_text {
    char *s;
    size_t max, n;
};
static void put_text(struct time_text *o, const char *t, size_t limit) {
    for (; *t && limit; t++, limit--) {
        if (o->n < o->max) o->s[o->n] = *t;
        o->n++;
    }
}
static void put_number(struct time_text *o, long long v, int width, bool spaces) {
    char digits[24];
    snprintf(digits, sizeof digits, spaces ? "%*lld" : "%0*lld", width, v);
    put_text(o, digits, sizeof digits);
}

/* ISO 8601: weeks start on Monday and week 1 holds the year's first Thursday,
   so a year has 53 weeks when it starts on a Thursday, or on a Wednesday in a
   leap year. */
static int iso_weeks(int jan1_wday, bool leap) { return jan1_wday == 4 || (leap && jan1_wday == 3) ? 53 : 52; }
static int iso_week(const struct tm *tm, long long *year) {
    long long y = tm->tm_year + 1900LL;
    int jan1 = ((tm->tm_wday - tm->tm_yday) % 7 + 7) % 7;
    int week = (tm->tm_yday - (tm->tm_wday + 6) % 7 + 10) / 7;
    *year = y;
    if (week < 1) {
        *year = y - 1;
        return iso_weeks(((jan1 - (is_leap(y - 1) ? 366 : 365)) % 7 + 7) % 7, is_leap(y - 1));
    }
    if (week > iso_weeks(jan1, is_leap(y))) {
        *year = y + 1;
        return 1;
    }
    return week;
}

/* Every ISO C conversion in the "C" locale. The E and O modifiers select
   alternative forms that the "C" locale does not have. No time zone is known
   (the clock keeps local time), so %z and %Z give no characters. */
static void format_time(struct time_text *o, const char *fmt, const struct tm *tm) {
    for (const char *p = fmt; *p; p++) {
        if (*p != '%' || !p[1]) {
            put_text(o, p, 1);
            continue;
        }
        p++;
        if ((*p == 'E' || *p == 'O') && p[1]) p++;
        long long year = tm->tm_year + 1900LL, iso_year;
        switch (*p) {
        case 'a': put_text(o, name_of(wdays, tm->tm_wday, 7), 3); break;
        case 'A': put_text(o, name_of(wdays, tm->tm_wday, 7), SIZE_MAX); break;
        case 'b':
        case 'h': put_text(o, name_of(months, tm->tm_mon, 12), 3); break;
        case 'B': put_text(o, name_of(months, tm->tm_mon, 12), SIZE_MAX); break;
        case 'c': format_time(o, "%a %b %e %H:%M:%S %Y", tm); break;
        case 'C': put_number(o, year / 100, 2, false); break;
        case 'd': put_number(o, tm->tm_mday, 2, false); break;
        case 'D':
        case 'x': format_time(o, "%m/%d/%y", tm); break;
        case 'e': put_number(o, tm->tm_mday, 2, true); break;
        case 'F': format_time(o, "%Y-%m-%d", tm); break;
        case 'g': iso_week(tm, &iso_year); put_number(o, iso_year % 100 < 0 ? -(iso_year % 100) : iso_year % 100, 2, false); break;
        case 'G': iso_week(tm, &iso_year); put_number(o, iso_year, 1, false); break;
        case 'H': put_number(o, tm->tm_hour, 2, false); break;
        case 'I': put_number(o, tm->tm_hour % 12 ? tm->tm_hour % 12 : 12, 2, false); break;
        case 'j': put_number(o, tm->tm_yday + 1, 3, false); break;
        case 'm': put_number(o, tm->tm_mon + 1, 2, false); break;
        case 'M': put_number(o, tm->tm_min, 2, false); break;
        case 'n': put_text(o, "\n", 1); break;
        case 'p': put_text(o, tm->tm_hour < 12 ? "AM" : "PM", 2); break;
        case 'r': format_time(o, "%I:%M:%S %p", tm); break;
        case 'R': format_time(o, "%H:%M", tm); break;
        case 'S': put_number(o, tm->tm_sec, 2, false); break;
        case 't': put_text(o, "\t", 1); break;
        case 'T':
        case 'X': format_time(o, "%H:%M:%S", tm); break;
        case 'u': put_number(o, tm->tm_wday ? tm->tm_wday : 7, 1, false); break;
        case 'U': put_number(o, (tm->tm_yday + 7 - tm->tm_wday) / 7, 2, false); break;
        case 'V': put_number(o, iso_week(tm, &iso_year), 2, false); break;
        case 'w': put_number(o, tm->tm_wday, 1, false); break;
        case 'W': put_number(o, (tm->tm_yday + 7 - (tm->tm_wday + 6) % 7) / 7, 2, false); break;
        case 'y': put_number(o, year % 100 < 0 ? -(year % 100) : year % 100, 2, false); break;
        case 'Y': put_number(o, year, 1, false); break;
        case 'z':
        case 'Z': break;
        case '%': put_text(o, "%", 1); break;
        default:
            put_text(o, "%", 1);
            put_text(o, p, 1);
            break;
        }
    }
}

size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm) {
    struct time_text o = {s, max, 0};
    format_time(&o, fmt, tm);
    if (o.n >= max) return 0;
    s[o.n] = 0;
    return o.n;
}

/* Each conversion's "C" locale text is ASCII, so it widens one byte to one
   wide character; the rest of the format is copied as it is. */
static void put_wide(wchar_t *s, size_t max, size_t *n, wchar_t c) {
    if (*n < max) s[*n] = c;
    ++*n;
}
size_t wcsftime(wchar_t *s, size_t max, const wchar_t *fmt, const struct tm *tm) {
    size_t n = 0;
    for (const wchar_t *p = fmt; *p; p++) {
        wchar_t c = p[1];
        if (*p != L'%' || !c) {
            put_wide(s, max, &n, *p);
            continue;
        }
        char conversion[4] = {'%'}, text[128];
        int i = 1;
        if ((c == L'E' || c == L'O') && p[2]) {
            conversion[i++] = (char)c;
            c = (++p)[1];
        }
        p++;
        if (c >= 0x80) {
            put_wide(s, max, &n, L'%');
            put_wide(s, max, &n, c);
            continue;
        }
        conversion[i] = (char)c;
        size_t len = strftime(text, sizeof text, conversion, tm);
        for (size_t k = 0; k < len; k++) put_wide(s, max, &n, (unsigned char)text[k]);
    }
    if (n >= max) return 0;
    s[n] = 0;
    return n;
}

/* ISO C timespec_get. The system's wall clock counts whole seconds; the
   milliseconds come from the uptime counter through an offset that only ever
   grows toward the true one (each whole-second reading is at most a second
   early), so the result never goes backwards and its seconds and fraction
   always agree. */
static int64_t wall_offset_ms = INT64_MIN;
static int64_t wall_ms(void) {
    int64_t up = (int64_t)n_clock_uptime_ms();
    int64_t seen = n_clock_wall() * 1000 - up;
    int64_t offset = __atomic_load_n(&wall_offset_ms, __ATOMIC_RELAXED);
    while (seen > offset &&
           !__atomic_compare_exchange_n(&wall_offset_ms, &offset, seen, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
    return up + (seen > offset ? seen : offset);
}
int timespec_get(struct timespec *ts, int base) {
    if (!ts) return 0;
    int64_t ms;
    switch (base) {
    case TIME_UTC: ms = wall_ms(); break;
    case TIME_MONOTONIC: ms = (int64_t)n_clock_uptime_ms(); break;
    default: return 0;
    }
    ts->tv_sec = (time_t)(ms / 1000);
    ts->tv_nsec = (long)(ms % 1000) * 1000000L;
    return base;
}
