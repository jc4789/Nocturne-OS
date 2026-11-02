/* setlocale (language.c) が libc の書式の部品へ渡す値。printf・strtod・strftime は language.c を引き込まない。 */
#pragma once

/* 小数点。"C" は '.'（stdlib.c が持つ）。 */
extern char __decimal_point;

/* strftime の LC_TIME。NULL は "C"（time.c が持つ）。 */
struct __time_names {
    const char *const *weekday;
    const char *const *weekday_short;
    const char *const *month;
    const char *const *month_short;
    const char *am, *pm;
    const char *date, *time, *date_time; /* %x、%X と %r、%c の書式 */
};
extern const struct __time_names *__time_names;
