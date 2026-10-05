#include <time.h>
#include <stdio.h>
#include <string.h>

static int is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
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

time_t mktime(struct tm *tm) {
    int y = tm->tm_year + 1900;
    int64_t days = 0;
    for (int i = 1970; i < y; i++) days += is_leap(i) ? 366 : 365;
    for (int m = 0; m < tm->tm_mon; m++) days += mdays[m] + (m == 1 && is_leap(y));
    days += tm->tm_mday - 1;
    return days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
}

static const char *wdays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *months[] = {"January", "February", "March", "April", "May", "June", "July",
                               "August", "September", "October", "November", "December"};

size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm) {
    size_t n = 0;
    char tmp[64];
    for (const char *p = fmt; *p && n + 1 < max; p++) {
        if (*p != '%') {
            s[n++] = *p;
            continue;
        }
        p++;
        tmp[0] = 0;
        switch (*p) {
        case 'Y': snprintf(tmp, sizeof tmp, "%d", tm->tm_year + 1900); break;
        case 'y': snprintf(tmp, sizeof tmp, "%02d", tm->tm_year % 100); break;
        case 'm': snprintf(tmp, sizeof tmp, "%02d", tm->tm_mon + 1); break;
        case 'd': snprintf(tmp, sizeof tmp, "%02d", tm->tm_mday); break;
        case 'e': snprintf(tmp, sizeof tmp, "%2d", tm->tm_mday); break;
        case 'H': snprintf(tmp, sizeof tmp, "%02d", tm->tm_hour); break;
        case 'I': snprintf(tmp, sizeof tmp, "%02d", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12); break;
        case 'p': snprintf(tmp, sizeof tmp, "%s", tm->tm_hour < 12 ? "AM" : "PM"); break;
        case 'M': snprintf(tmp, sizeof tmp, "%02d", tm->tm_min); break;
        case 'S': snprintf(tmp, sizeof tmp, "%02d", tm->tm_sec); break;
        case 'A': snprintf(tmp, sizeof tmp, "%s", wdays[tm->tm_wday]); break;
        case 'a': snprintf(tmp, sizeof tmp, "%.3s", wdays[tm->tm_wday]); break;
        case 'B': snprintf(tmp, sizeof tmp, "%s", months[tm->tm_mon]); break;
        case 'b': snprintf(tmp, sizeof tmp, "%.3s", months[tm->tm_mon]); break;
        case 'j': snprintf(tmp, sizeof tmp, "%03d", tm->tm_yday + 1); break;
        case '%': strcpy(tmp, "%"); break;
        default: snprintf(tmp, sizeof tmp, "%%%c", *p); break;
        }
        for (char *q = tmp; *q && n + 1 < max; q++) s[n++] = *q;
    }
    s[n] = 0;
    return n;
}
