/* 言語と地域の値と書式（nocturne_language.h）。見張りで今の値に保つので、読むたびにサービスへ頼まない。 */
#include <nocturne_language.h>
#include <nocturne_settings.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include "locale_private.h"

static once_flag start_once = ONCE_FLAG_INIT;
static mtx_t lock;
static n_handle watch = -1;
static struct n_locale now;
static uint64_t generation;

static int32_t read_key(uint32_t key, int32_t fallback, int32_t high) {
    int32_t v;
    if (n_setting_get_int(n_setting_at(N_DOMAIN_LANGUAGE, key), &v) < 0 || v < 0 || v > high) return fallback;
    return v;
}

/* lock の下。 */
static void read_all(void) {
    uint32_t language = (uint32_t)read_key(N_LANGUAGE_UI, N_LANGUAGE_JAPANESE, N_LANGUAGE_COUNT - 1);
    uint32_t region = (uint32_t)read_key(N_LANGUAGE_REGION, N_REGION_JAPAN, N_REGION_COUNT - 1);
    int32_t order = read_key(N_LANGUAGE_DATE_ORDER, N_DATE_OF_REGION, N_DATE_DMY);
    int32_t clock = read_key(N_LANGUAGE_CLOCK, N_CLOCK_OF_REGION, N_CLOCK_12);
    int32_t number = read_key(N_LANGUAGE_NUMBER, N_NUMBER_OF_REGION, N_NUMBER_SPACE_COMMA);
    static const uint8_t region_order[N_REGION_COUNT] = {N_DATE_YMD, N_DATE_MDY, N_DATE_DMY};
    struct n_locale l = {.language = language, .region = region};
    l.date_order = order == N_DATE_OF_REGION ? region_order[region] : (uint32_t)order;
    l.clock_12 = clock == N_CLOCK_OF_REGION ? region == N_REGION_US : clock == N_CLOCK_12;
    switch (number) {
    case N_NUMBER_POINT_COMMA: l.point = ','; l.group = '.'; break;
    case N_NUMBER_SPACE_COMMA: l.point = ','; l.group = ' '; break;
    default: l.point = '.'; l.group = ','; break;
    }
    l.tag = language == N_LANGUAGE_JAPANESE ? "ja" : region == N_REGION_US ? "en-US" : region == N_REGION_UK ? "en-GB" : "en";
    now = l;
    __atomic_store_n(&generation, generation + 1, __ATOMIC_RELEASE);
}

static void start(void) {
    mtx_init(&lock, mtx_plain);
    watch = n_setting_watch(N_DOMAIN_LANGUAGE, N_SETTING_ANY);
    read_all();
}

void n_locale_now(struct n_locale *out) {
    call_once(&start_once, start);
    mtx_lock(&lock);
    if (watch >= 0) {
        struct n_settings_changed heard;
        bool changed = false;
        while (n_setting_watch_take(watch, &heard) >= 0) changed = true;
        if (changed) read_all();
    }
    *out = now;
    mtx_unlock(&lock);
}

uint32_t n_language_now(void) {
    struct n_locale l;
    n_locale_now(&l);
    return l.language;
}

bool n_language_changed(uint64_t *seen) {
    struct n_locale l;
    n_locale_now(&l);
    uint64_t g = __atomic_load_n(&generation, __ATOMIC_ACQUIRE);
    bool changed = *seen != g;
    *seen = g;
    return changed;
}

size_t n_locale_languages(char *out, size_t cap) {
    struct n_locale l;
    n_locale_now(&l);
    const char *dash = strchr(l.tag, '-');
    int n;
    if (dash) n = snprintf(out, cap, "%s,%.*s", l.tag, (int)(dash - l.tag), l.tag);
    else n = snprintf(out, cap, "%s", l.tag);
    return n < 0 ? 0 : (size_t)n;
}

const char *n_locale_intl(void) { return n_language_now() == N_LANGUAGE_ENGLISH ? "en" : "ja"; }

/* ---- 名 ---- */
static const char *const weekdays[N_LANGUAGE_COUNT][7] = {
    [N_LANGUAGE_JAPANESE] = {"日曜日", "月曜日", "火曜日", "水曜日", "木曜日", "金曜日", "土曜日"},
    [N_LANGUAGE_ENGLISH] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"},
};
static const char *const weekdays_short[N_LANGUAGE_COUNT][7] = {
    [N_LANGUAGE_JAPANESE] = {"日", "月", "火", "水", "木", "金", "土"},
    [N_LANGUAGE_ENGLISH] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"},
};
static const char *const months[N_LANGUAGE_COUNT][12] = {
    [N_LANGUAGE_JAPANESE] = {"1月", "2月", "3月", "4月", "5月", "6月", "7月", "8月", "9月", "10月", "11月", "12月"},
    [N_LANGUAGE_ENGLISH] = {"January", "February", "March", "April", "May", "June", "July", "August", "September",
                            "October", "November", "December"},
};
static const char *const months_short[N_LANGUAGE_COUNT][12] = {
    [N_LANGUAGE_JAPANESE] = {"1月", "2月", "3月", "4月", "5月", "6月", "7月", "8月", "9月", "10月", "11月", "12月"},
    [N_LANGUAGE_ENGLISH] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"},
};

const char *n_locale_weekday(int index, bool abbreviated) {
    if (index < 0 || index > 6) return "";
    return (abbreviated ? weekdays_short : weekdays)[n_language_now()][index];
}
const char *n_locale_month(int index, bool abbreviated) {
    if (index < 0 || index > 11) return "";
    return (abbreviated ? months_short : months)[n_language_now()][index];
}

/* ---- 書式 ---- */
static size_t wrote(int n, size_t cap) {
    if (n < 0 || !cap) return 0;
    return (size_t)n < cap ? (size_t)n : cap - 1;
}

size_t n_locale_date(char *out, size_t cap, const struct tm *tm) {
    struct n_locale l;
    n_locale_now(&l);
    int y = tm->tm_year + 1900, m = tm->tm_mon + 1, d = tm->tm_mday;
    switch (l.date_order) {
    case N_DATE_MDY: return wrote(snprintf(out, cap, "%02d/%02d/%04d", m, d, y), cap);
    case N_DATE_DMY: return wrote(snprintf(out, cap, "%02d/%02d/%04d", d, m, y), cap);
    default: return wrote(snprintf(out, cap, "%04d/%02d/%02d", y, m, d), cap);
    }
}

size_t n_locale_time(char *out, size_t cap, const struct tm *tm, bool seconds) {
    struct n_locale l;
    n_locale_now(&l);
    if (!l.clock_12) {
        if (seconds) return wrote(snprintf(out, cap, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec), cap);
        return wrote(snprintf(out, cap, "%02d:%02d", tm->tm_hour, tm->tm_min), cap);
    }
    int h = tm->tm_hour % 12 ? tm->tm_hour % 12 : 12;
    const char *mark = l.language == N_LANGUAGE_JAPANESE ? (tm->tm_hour < 12 ? "午前" : "午後") : (tm->tm_hour < 12 ? "AM" : "PM");
    if (l.language == N_LANGUAGE_JAPANESE) {
        if (seconds) return wrote(snprintf(out, cap, "%s %d:%02d:%02d", mark, h, tm->tm_min, tm->tm_sec), cap);
        return wrote(snprintf(out, cap, "%s %d:%02d", mark, h, tm->tm_min), cap);
    }
    if (seconds) return wrote(snprintf(out, cap, "%d:%02d:%02d %s", h, tm->tm_min, tm->tm_sec, mark), cap);
    return wrote(snprintf(out, cap, "%d:%02d %s", h, tm->tm_min, mark), cap);
}

size_t n_locale_day(char *out, size_t cap, const struct tm *tm) {
    struct n_locale l;
    n_locale_now(&l);
    const char *wd = weekdays_short[l.language][tm->tm_wday >= 0 && tm->tm_wday < 7 ? tm->tm_wday : 0];
    const char *mo = months_short[l.language][tm->tm_mon >= 0 && tm->tm_mon < 12 ? tm->tm_mon : 0];
    if (l.language == N_LANGUAGE_JAPANESE) return wrote(snprintf(out, cap, "%s%d日（%s）", mo, tm->tm_mday, wd), cap);
    if (l.date_order == N_DATE_DMY) return wrote(snprintf(out, cap, "%s, %d %s", wd, tm->tm_mday, mo), cap);
    return wrote(snprintf(out, cap, "%s, %s %d", wd, mo, tm->tm_mday), cap);
}

size_t n_locale_number(char *out, size_t cap, long long value) {
    struct n_locale l;
    n_locale_now(&l);
    char digits[32];
    int n = snprintf(digits, sizeof digits, "%lld", value);
    if (n <= 0 || !cap) return 0;
    size_t at = 0;
    int start = digits[0] == '-';
    if (start && at + 1 < cap) out[at++] = '-';
    for (int i = start; i < n && at + 1 < cap; i++) {
        out[at++] = digits[i];
        int left = n - i - 1;
        if (left && left % 3 == 0 && at + 1 < cap) out[at++] = l.group;
    }
    out[at] = 0;
    return at;
}

/* ---- ISO C の locale ---- */
#define CATEGORIES (LC_TIME + 1)
static const char *category_names[CATEGORIES] = {"C", "C", "C", "C", "C", "C"};

/* ISO C の "C" の lconv（7.11.2.1）。 */
static const struct lconv c_conv = {
    (char *)".", (char *)"", (char *)"", (char *)"", (char *)"", (char *)"", (char *)"", (char *)"", (char *)"", (char *)"",
    CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX,
    CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX,
};
static struct lconv conv = c_conv;
static char point_text[2], group_text[2];

/* 地域の通貨。記号は UTF-8。国際の記号は ISO 4217 の三文字と空白。 */
static const struct {
    const char *symbol, *international;
    char digits;
} currencies[N_REGION_COUNT] = {
    [N_REGION_JAPAN] = {"\xC2\xA5", "JPY ", 0},
    [N_REGION_US] = {"$", "USD ", 2},
    [N_REGION_UK] = {"\xC2\xA3", "GBP ", 2},
};

static void numeric_apply(const struct n_locale *l) {
    if (!l) {
        __decimal_point = '.';
        conv.decimal_point = c_conv.decimal_point;
        conv.thousands_sep = c_conv.thousands_sep;
        conv.grouping = c_conv.grouping;
        return;
    }
    __decimal_point = l->point;
    point_text[0] = l->point;
    group_text[0] = l->group;
    conv.decimal_point = point_text;
    conv.thousands_sep = group_text;
    conv.grouping = (char *)"\3";
}

static void monetary_apply(const struct n_locale *l) {
    if (!l) {
        struct lconv keep = conv;
        conv = c_conv;
        conv.decimal_point = keep.decimal_point;
        conv.thousands_sep = keep.thousands_sep;
        conv.grouping = keep.grouping;
        return;
    }
    conv.mon_decimal_point = point_text[0] ? point_text : (char *)".";
    conv.mon_thousands_sep = group_text[0] ? group_text : (char *)",";
    conv.mon_grouping = (char *)"\3";
    conv.positive_sign = (char *)"";
    conv.negative_sign = (char *)"-";
    conv.currency_symbol = (char *)currencies[l->region].symbol;
    conv.int_curr_symbol = (char *)currencies[l->region].international;
    conv.frac_digits = conv.int_frac_digits = currencies[l->region].digits;
    conv.p_cs_precedes = conv.n_cs_precedes = conv.int_p_cs_precedes = conv.int_n_cs_precedes = 1;
    conv.p_sep_by_space = conv.n_sep_by_space = conv.int_p_sep_by_space = conv.int_n_sep_by_space = 0;
    conv.p_sign_posn = conv.n_sign_posn = conv.int_p_sign_posn = conv.int_n_sign_posn = 1;
}

static void time_apply(const struct n_locale *l) {
    static struct __time_names names;
    if (!l) {
        __time_names = NULL;
        return;
    }
    static const char *const dates[] = {[N_DATE_YMD] = "%Y/%m/%d", [N_DATE_MDY] = "%m/%d/%Y", [N_DATE_DMY] = "%d/%m/%Y"};
    bool japanese = l->language == N_LANGUAGE_JAPANESE;
    names.weekday = weekdays[l->language];
    names.weekday_short = weekdays_short[l->language];
    names.month = months[l->language];
    names.month_short = months_short[l->language];
    names.am = japanese ? "午前" : "AM";
    names.pm = japanese ? "午後" : "PM";
    names.date = dates[l->date_order];
    names.time = !l->clock_12 ? "%H:%M:%S" : japanese ? "%p %I:%M:%S" : "%I:%M:%S %p";
    names.date_time = "%x %X";
    __time_names = &names;
}

/* 一つのカテゴリーの名を決める。この名を知らなければ NULL。 */
static const char *name_known(const char *name, size_t length, const char *user) {
    if ((length == 1 && name[0] == 'C') || (length == 7 && !memcmp(name, "C.UTF-8", 7))) return "C";
    if (!length || (strlen(user) == length && !memcmp(name, user, length))) return user;
    return NULL;
}

static void category_apply(int category, const char *name, const struct n_locale *user) {
    const struct n_locale *l = strcmp(name, "C") ? user : NULL;
    category_names[category] = name;
    if (category == LC_NUMERIC) numeric_apply(l);
    else if (category == LC_MONETARY) monetary_apply(l);
    else if (category == LC_TIME) time_apply(l);
}

/* 今の名。呼ぶ側は次の setlocale まで読める。 */
static char *category_name(int category) {
    static char *said;
    bool same = true;
    size_t need = 1;
    for (int i = 1; i < CATEGORIES; i++) {
        same = same && !strcmp(category_names[i], category_names[1]);
        need += strlen(category_names[i]) + 1;
    }
    char *grown = realloc(said, need);
    if (!grown) return NULL;
    said = grown;
    if (category != LC_ALL) return strcpy(said, category_names[category]);
    if (same) return strcpy(said, category_names[1]);
    said[0] = 0;
    for (int i = 1; i < CATEGORIES; i++) {
        if (i > 1) strcat(said, ";");
        strcat(said, category_names[i]);
    }
    return said;
}

char *setlocale(int category, const char *locale) {
    if (category < LC_ALL || category >= CATEGORIES) return NULL;
    if (!locale) return category_name(category);
    struct n_locale user;
    n_locale_now(&user);
    const char *names[CATEGORIES] = {0};
    if (category == LC_ALL && strchr(locale, ';')) { /* category_name の混ざりの形 */
        const char *at = locale;
        for (int i = 1; i < CATEGORIES; i++) {
            const char *end = strchr(at, ';');
            size_t length = end ? (size_t)(end - at) : strlen(at);
            if (!(names[i] = name_known(at, length, user.tag)) || !length) return NULL;
            if ((i == CATEGORIES - 1) != !end) return NULL;
            at = end ? end + 1 : at;
        }
    } else {
        const char *name = name_known(locale, strlen(locale), user.tag);
        if (!name) return NULL;
        for (int i = 1; i < CATEGORIES; i++)
            if (category == LC_ALL || category == i) names[i] = name;
    }
    /* LC_NUMERIC は LC_MONETARY の区切りの元なので先に当てる。 */
    if (names[LC_NUMERIC]) category_apply(LC_NUMERIC, names[LC_NUMERIC], &user);
    for (int i = 1; i < CATEGORIES; i++)
        if (names[i] && i != LC_NUMERIC) category_apply(i, names[i], &user);
    return category_name(category);
}

struct lconv *localeconv(void) { return &conv; }
