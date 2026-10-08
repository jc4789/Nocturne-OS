#include "form_face.h"
#include <math.h>
#include <stdio.h>
#include "nocturne.h"
#include "form_value.h"
#include "form_validation.h"
#include "form_direction.h"
#include "form_file.h"

#define DAY_MS 86400000.0
#define WEEK_MS 604800000.0
static const char *const type_names[] = {
    "text", "search", "tel", "url", "email", "password", "hidden", "date", "month", "week", "time",
    "datetime-local", "number", "range", "color", "checkbox", "radio", "file", "submit", "image", "reset", "button"
};
static bool digit(int c) { return c >= '0' && c <= '9'; }
static bool space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
enum web_input_kind web_input_type(const node_t *n) {
    const char *type = n ? node_attr(n, "type") : NULL;
    if (type) for (unsigned i = 0; i < sizeof type_names / sizeof *type_names; i++)
        if (str_ieq(type, type_names[i])) return (enum web_input_kind)i;
    return WEB_INPUT_TEXT;
}
const char *web_input_type_name(enum web_input_kind type) {
    return (unsigned)type < sizeof type_names / sizeof *type_names ? type_names[type] : "text";
}
enum web_input_mode web_input_value_mode(enum web_input_kind type) {
    if (type == WEB_INPUT_CHECKBOX || type == WEB_INPUT_RADIO) return WEB_INPUT_DEFAULT_ON;
    if (type == WEB_INPUT_FILE) return WEB_INPUT_FILENAME;
    if (type == WEB_INPUT_HIDDEN || type >= WEB_INPUT_SUBMIT) return WEB_INPUT_DEFAULT;
    return WEB_INPUT_VALUE;
}
bool web_input_numeric(enum web_input_kind type) { return type >= WEB_INPUT_DATE && type <= WEB_INPUT_RANGE; }
bool web_input_date_supported(enum web_input_kind type) { return type >= WEB_INPUT_DATE && type <= WEB_INPUT_TIME; }

/* No strtod extension (hex, Infinity, NaN, leading '+', trailing garbage)
   is admitted as an input's valid floating-point number. */
static bool floating_decimal(const char *text, bool strict, double *out, long double *decimal) {
    if (!text) return false;
    const char *p = text;
    if (!strict) while (space((unsigned char)*p)) p++;
    const char *begin = p;
    if (*p == '-' || (!strict && *p == '+')) p++;
    bool integer = false, fraction = false;
    while (digit(*p)) { integer = true; p++; }
    if (*p == '.') {
        p++;
        while (digit(*p)) { fraction = true; p++; }
        if (strict && !fraction) return false;
    }
    if (!integer && !fraction) return false;
    if (*p == 'e' || *p == 'E') {
        const char *exponent = p++;
        if (*p == '+' || *p == '-') p++;
        const char *digits = p;
        while (digit(*p)) p++;
        if (p == digits) { if (strict) return false; p = exponent; }
    }
    if (strict && *p) return false;
    /* Bound the temporary conversion without dropping significant input. */
    size_t len = (size_t)(p - begin);
    char local[128], *copy = len < sizeof local ? local : malloc(len + 1);
    if (!copy) return false;
    memcpy(copy, begin, len); copy[len] = 0;
    char *end; long double wide = strtold(copy, &end); double value = (double)wide;
    bool valid = end == copy + len && isfinite(value);
    if (copy != local) free(copy);
    if (valid && out) *out = value == 0 ? 0 : value;
    if (valid && decimal) *decimal = wide == 0 ? 0 : wide;
    return valid;
}
static bool floating(const char *text, bool strict, double *out) { return floating_decimal(text, strict, out, NULL); }

/* Proleptic Gregorian arithmetic; UTC epochs are independent of Nocturne's
   native clock and avoid POSIX time/localtime dependencies. */
static bool leap(int64_t y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }
static unsigned month_days(int64_t y, unsigned m) {
    static const unsigned days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return days[m - 1] + (m == 2 && leap(y));
}
static int64_t civil_days(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
static void days_civil(int64_t days, int64_t *y, unsigned *m, unsigned *d) {
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned doe = (unsigned)(days - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    *y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y += *m <= 2;
}
static unsigned monday_index(int64_t day) {
    int64_t i = (day + 3) % 7;
    return (unsigned)(i < 0 ? i + 7 : i);
}
static int64_t week_one(int64_t y) {
    int64_t jan4 = civil_days(y, 1, 4);
    return jan4 - monday_index(jan4);
}
static bool two_digits(const char **text, unsigned *out) {
    const char *p = *text;
    if (!digit(p[0]) || !digit(p[1])) return false;
    *out = (unsigned)((p[0] - '0') * 10 + p[1] - '0'); *text += 2; return true;
}
static bool year_month(const char **text, int64_t *year, unsigned *month) {
    const char *p = *text, *begin = p; int64_t y = 0;
    while (digit(*p)) {
        unsigned v = (unsigned)(*p++ - '0');
        /* Gregorian day counts must stay within int64_t, even for huge years. */
        if (y > (INT64_MAX / 366 - 1 - v) / 10) return false;
        y = y * 10 + v;
    }
    if (p - begin < 4 || !y || *p++ != '-') return false;
    unsigned m;
    if (!two_digits(&p, &m) || m < 1 || m > 12) return false;
    *year = y; *month = m; *text = p; return true;
}
static bool parse_date(const char **text, int64_t *day) {
    const char *p = *text; int64_t y; unsigned m, d;
    if (!year_month(&p, &y, &m) || *p++ != '-' || !two_digits(&p, &d) || !d || d > month_days(y, m)) return false;
    *day = civil_days(y, m, d); *text = p; return true;
}
static bool parse_time(const char **text, double *value) {
    const char *p = *text; unsigned hour, minute, second = 0, ms = 0;
    if (!two_digits(&p, &hour) || hour > 23 || *p++ != ':' || !two_digits(&p, &minute) || minute > 59) return false;
    if (*p == ':') {
        p++;
        if (!two_digits(&p, &second) || second > 59) return false;
        if (*p == '.') {
            p++; unsigned digits = 0;
            while (digit(*p) && digits < 3) { ms = ms * 10 + (unsigned)(*p++ - '0'); digits++; }
            if (!digits || digit(*p)) return false;
            while (digits++ < 3) ms *= 10;
        }
    }
    *value = ((hour * 60 + minute) * 60 + second) * 1000.0 + ms;
    *text = p; return true;
}
bool web_input_parse_number(enum web_input_kind type, const char *text, double *out) {
    if (!text || !*text || !web_input_numeric(type)) return false;
    if (type == WEB_INPUT_NUMBER || type == WEB_INPUT_RANGE) return floating(text, true, out);
    const char *p = text; double value = 0; int64_t day = 0, y; unsigned m;
    switch (type) {
    case WEB_INPUT_DATE:
        if (!parse_date(&p, &day)) return false;
        value = (double)day * DAY_MS; break;
    case WEB_INPUT_MONTH:
        if (!year_month(&p, &y, &m)) return false;
        value = (double)(y - 1970) * 12 + m - 1; break;
    case WEB_INPUT_WEEK: {
        const char *begin = p; y = 0;
        while (digit(*p)) {
            unsigned v = (unsigned)(*p++ - '0');
            if (y > (INT64_MAX / 366 - 1 - v) / 10) return false;
            y = y * 10 + v;
        }
        if (p - begin < 4 || !y || *p++ != '-' || *p++ != 'W' || !two_digits(&p, &m) || !m) return false;
        unsigned weeks = (unsigned)((week_one(y + 1) - week_one(y)) / 7);
        if (m > weeks) return false;
        value = (double)(week_one(y) + (m - 1) * 7) * DAY_MS; break;
    }
    case WEB_INPUT_TIME: if (!parse_time(&p, &value)) return false; break;
    case WEB_INPUT_DATETIME_LOCAL:
        if (!parse_date(&p, &day) || (*p != 'T' && *p != ' ')) return false;
        p++; if (!parse_time(&p, &value)) return false;
        value += (double)day * DAY_MS; break;
    default: return false;
    }
    if (*p || !isfinite(value)) return false;
    if (out) *out = value;
    return true;
}
bool web_input_parse_attribute(enum web_input_kind type, const char *text, double *out) {
    return type == WEB_INPUT_NUMBER || type == WEB_INPUT_RANGE ? floating(text, false, out) : web_input_parse_number(type, text, out);
}

/* Find the shortest significant decimal that round-trips, then use the
   decimal/exponent presentation of ECMAScript's finite Number strings. */
static bool number_string(double value, char *out, size_t capacity) {
    if (!isfinite(value) || capacity < 2) return false;
    if (value == 0) { strcpy(out, "0"); return true; }
    char candidate[128];
    for (int precision = 1; precision <= 17; precision++) {
        snprintf(candidate, sizeof candidate, "%.*g", precision, value);
        if (strtod(candidate, NULL) == value) break;
    }
    const char *p = candidate; bool negative = *p == '-'; if (negative) p++;
    char digits[32]; unsigned n = 0; int before = 0; bool dot = false;
    while (*p && *p != 'e' && *p != 'E') {
        if (*p == '.') dot = true;
        else { digits[n++] = *p; if (!dot) before++; }
        p++;
    }
    int exponent = *p ? atoi(p + 1) : 0;
    int point = before + exponent;
    while (n > 1 && digits[0] == '0') { memmove(digits, digits + 1, --n); point--; }
    while (n > 1 && digits[n - 1] == '0') n--;
    char result[128]; size_t at = 0;
    if (negative) result[at++] = '-';
    if (point > 0 && point <= 21) {
        for (int i = 0; i < point; i++) result[at++] = (unsigned)i < n ? digits[i] : '0';
        if ((unsigned)point < n) { result[at++] = '.'; for (unsigned i = (unsigned)point; i < n; i++) result[at++] = digits[i]; }
    } else if (point <= 0 && point > -6) {
        result[at++] = '0'; result[at++] = '.';
        for (int i = 0; i < -point; i++) result[at++] = '0';
        for (unsigned i = 0; i < n; i++) result[at++] = digits[i];
    } else {
        result[at++] = digits[0];
        if (n > 1) { result[at++] = '.'; for (unsigned i = 1; i < n; i++) result[at++] = digits[i]; }
        at += (size_t)snprintf(result + at, sizeof result - at, "e%s%d", point - 1 >= 0 ? "+" : "", point - 1);
    }
    if (at >= capacity) return false;
    memcpy(out, result, at); out[at] = 0; return true;
}
static bool format_time(double value, char *out, size_t capacity) {
    value = fmod(trunc(value), DAY_MS); if (value < 0) value += DAY_MS;
    unsigned ms = (unsigned)value, h = ms / 3600000, m = ms / 60000 % 60, s = ms / 1000 % 60, f = ms % 1000;
    int len;
    if (f) {
        len = snprintf(out, capacity, "%02u:%02u:%02u.%03u", h, m, s, f);
        if (len > 0 && (size_t)len < capacity) while (out[len - 1] == '0') out[--len] = 0;
    } else if (s) len = snprintf(out, capacity, "%02u:%02u:%02u", h, m, s);
    else len = snprintf(out, capacity, "%02u:%02u", h, m);
    return len > 0 && (size_t)len < capacity;
}
bool web_input_format_number(enum web_input_kind type, double value, char *out, size_t capacity) {
    if (!out || !capacity || !isfinite(value) || !web_input_numeric(type)) return false;
    if (type == WEB_INPUT_NUMBER || type == WEB_INPUT_RANGE) return number_string(value, out, capacity);
    if (type == WEB_INPUT_TIME) return format_time(value, out, capacity);
    int64_t y; unsigned m, day;
    if (type == WEB_INPUT_MONTH) {
        double months = floor(value);
        if (months < -23628 || months > (double)(INT64_MAX / 366 - 1971) * 12) return false;
        y = (int64_t)floor(months / 12) + 1970;
        m = (unsigned)(months - floor(months / 12) * 12) + 1;
        int len = snprintf(out, capacity, "%04lld-%02u", (long long)y, m);
        return len > 0 && (size_t)len < capacity;
    }
    double days = floor(value / DAY_MS);
    if (days < (double)civil_days(1, 1, 1) || days > (double)(INT64_MAX / 2)) return false;
    int64_t count = (int64_t)days;
    days_civil(count, &y, &m, &day);
    if (y < 1) return false;
    int len;
    if (type == WEB_INPUT_WEEK) {
        int64_t thursday = count + 3 - monday_index(count); unsigned ignored;
        days_civil(thursday, &y, &ignored, &day);
        if (y < 1) return false;
        unsigned week = (unsigned)((count - week_one(y)) / 7 + 1);
        len = snprintf(out, capacity, "%04lld-W%02u", (long long)y, week);
    } else {
        len = snprintf(out, capacity, "%04lld-%02u-%02u", (long long)y, m, day);
        if (type == WEB_INPUT_DATETIME_LOCAL && len > 0 && (size_t)len + 1 < capacity) {
            out[len++] = 'T';
            if (!format_time(value - days * DAY_MS, out + len, capacity - (size_t)len)) return false;
            return true;
        }
    }
    return len > 0 && (size_t)len < capacity;
}
void web_input_constraints(const node_t *n, struct web_input_limits *out) {
    memset(out, 0, sizeof *out); enum web_input_kind type = web_input_type(n);
    out->numeric = web_input_numeric(type); if (!out->numeric) return;
    out->has_min = web_input_parse_attribute(type, node_attr(n, "min"), &out->min);
    out->has_max = web_input_parse_attribute(type, node_attr(n, "max"), &out->max);
    if (type == WEB_INPUT_RANGE) {
        if (!out->has_min) out->min = 0;
        if (!out->has_max) out->max = 100;
        out->has_min = out->has_max = true;
    }
    double scale = type == WEB_INPUT_DATE ? DAY_MS : type == WEB_INPUT_WEEK ? WEEK_MS :
                   type == WEB_INPUT_TIME || type == WEB_INPUT_DATETIME_LOCAL ? 1000 : 1;
    double step = type == WEB_INPUT_TIME || type == WEB_INPUT_DATETIME_LOCAL ? 60 : 1;
    const char *attr = node_attr(n, "step");
    out->any = attr && str_ieq(attr, "any"); double explicit_step;
    if (!out->any && floating(attr, false, &explicit_step) && explicit_step > 0) step = explicit_step;
    out->step = step * scale;
    /* Extremely large positive steps still have an allowed step; arithmetic
       that cannot produce a finite candidate safely leaves the value alone. */
    double base;
    if (web_input_parse_attribute(type, node_attr(n, "min"), &base) ||
        web_input_parse_attribute(type, node_attr(n, "value"), &base)) out->base = base;
    else out->base = type == WEB_INPUT_WEEK ? -259200000.0 : 0;
}
static bool mismatch(const struct web_input_limits *limits, double value) {
    if (!limits->numeric || limits->any || !isfinite(limits->step)) return false;
    double quotient = (value - limits->base) / limits->step;
    if (!isfinite(quotient)) return true;
    double nearest = round(quotient);
    /* Decimal HTML steps (0.1, 0.01) must not inherit binary-double noise.
       Keep the tolerance relative to quotient, not to absolute epoch time. */
    return fabs(quotient - nearest) > fmin(1e-7, 8 * 2.2204460492503131e-16 * fmax(1, fabs(quotient)));
}
bool web_input_step_mismatch(const node_t *n, double value) {
    struct web_input_limits limits; web_input_constraints(n, &limits); return mismatch(&limits, value);
}
static void decimal_lattice(const node_t *n, const struct web_input_limits *limits, long double *step, long double *base) {
    *step = limits->step; *base = limits->base;
    enum web_input_kind type = web_input_type(n);
    if (type != WEB_INPUT_NUMBER && type != WEB_INPUT_RANGE) return;
    long double exact; double parsed;
    if (floating_decimal(node_attr(n, "step"), false, &parsed, &exact) && parsed > 0) *step = exact;
    if (floating_decimal(node_attr(n, "min"), false, NULL, &exact) ||
        floating_decimal(node_attr(n, "value"), false, NULL, &exact)) *base = exact;
}
static double aligned(const node_t *n, double value, const struct web_input_limits *limits, bool up) {
    long double step, base; decimal_lattice(n, limits, &step, &base);
    double q = (double)(((long double)value - base) / step);
    if (!mismatch(limits, value)) q = round(q);
    else q = up ? ceil(q) : floor(q);
    return (double)(base + (long double)q * step);
}
static void trim(char *value, size_t *len) {
    size_t first = 0, last = *len;
    while (first < last && space((unsigned char)value[first])) first++;
    while (last > first && space((unsigned char)value[last - 1])) last--;
    *len = last - first; memmove(value, value + first, *len); value[*len] = 0;
}
size_t web_input_sanitize(const node_t *n, const char *text, size_t len, char *out) {
    enum web_input_kind type = web_input_type(n); size_t used = 0;
    bool single_line = type <= WEB_INPUT_PASSWORD;
    for (size_t i = 0; i < len; i++) if (!single_line || (text[i] != '\r' && text[i] != '\n')) out[used++] = text[i];
    out[used] = 0;
    if (type == WEB_INPUT_URL || type == WEB_INPUT_EMAIL) {
        trim(out, &used);
        if (type == WEB_INPUT_EMAIL && node_attr(n, "multiple")) {
            size_t read = 0, write = 0;
            while (read <= used) {
                size_t end = read; while (end < used && out[end] != ',') end++;
                size_t first = read, last = end;
                while (first < last && space((unsigned char)out[first])) first++;
                while (last > first && space((unsigned char)out[last - 1])) last--;
                memmove(out + write, out + first, last - first); write += last - first;
                if (end == used) break;
                out[write++] = ','; read = end + 1;
            }
            used = write; out[used] = 0;
        }
    } else if (web_input_numeric(type)) {
        double value; bool valid = web_input_parse_number(type, out, &value);
        if (type == WEB_INPUT_RANGE) {
            struct web_input_limits limits; web_input_constraints(n, &limits);
            double initial = valid ? value : NAN;
            if (!valid) value = limits.max < limits.min ? limits.min : limits.min / 2 + limits.max / 2;
            if (value < limits.min) value = limits.min;
            if (limits.max >= limits.min && value > limits.max) value = limits.max;
            if (mismatch(&limits, value) && isfinite(limits.step)) {
                double low = aligned(n, value, &limits, false), high = aligned(n, value, &limits, true);
                bool low_ok = low >= limits.min && (limits.max < limits.min || low <= limits.max);
                bool high_ok = high >= limits.min && (limits.max < limits.min || high <= limits.max);
                if (high_ok && (!low_ok || high - value <= value - low)) value = high;
                else if (low_ok) value = low;
            }
            if ((!valid || value != initial) && !number_string(value, out, 128)) out[0] = 0;
            used = strlen(out);
        } else if (!valid) { used = 0; out[0] = 0; }
        else if (type == WEB_INPUT_DATETIME_LOCAL) {
            /* Preserve the valid date's spelling; normalize only separator and
               time, so large years do not lose low bits through a double. */
            char *separator = strchr(out, 'T'); if (!separator) separator = strchr(out, ' ');
            double time; const char *p = separator + 1; parse_time(&p, &time);
            *separator = 'T'; format_time(time, separator + 1, 128); used = strlen(out);
        }
    } else if (type == WEB_INPUT_COLOR) {
        /* Legacy opaque limited-sRGB subset. alpha/display-p3 and general CSS
           color parsing need a real color-well implementation, not a claim. */
        bool valid = used == 7 && out[0] == '#';
        for (size_t i = 1; valid && i < 7; i++) {
            int c = lower((unsigned char)out[i]);
            if (!digit(c) && (c < 'a' || c > 'f')) valid = false;
            out[i] = (char)c;
        }
        if (!valid) { strcpy(out, "#000000"); used = 7; }
    } else if (type == WEB_INPUT_FILE) { out[0] = 0; used = 0; }
    return used;
}
double web_input_value_number(web_doc *d, node_t *n) {
    doc_control_init(d, n); double value;
    return web_input_parse_number(web_input_type(n), n->value, &value) ? value : NAN;
}
double web_input_value_date(web_doc *d, node_t *n) {
    enum web_input_kind type = web_input_type(n);
    if (!web_input_date_supported(type)) return NAN;
    double value = web_input_value_number(d, n);
    if (type == WEB_INPUT_MONTH && isfinite(value)) {
        int64_t year = (int64_t)floor(value / 12) + 1970;
        unsigned month = (unsigned)(value - floor(value / 12) * 12) + 1;
        value = (double)civil_days(year, month, 1) * DAY_MS;
    }
    return value;
}
enum web_input_result web_input_set_number(web_doc *d, node_t *n, double value, bool as_date) {
    enum web_input_kind type = web_input_type(n);
    if (!as_date && isinf(value)) return WEB_INPUT_INVALID_NUMBER;
    if (as_date ? !web_input_date_supported(type) : !web_input_numeric(type)) return WEB_INPUT_INVALID_STATE;
    char text[128]; text[0] = 0;
    if (isfinite(value)) {
        if (as_date && type == WEB_INPUT_MONTH) {
            double day = floor(value / DAY_MS);
            if (day >= (double)civil_days(1, 1, 1) && day <= (double)(INT64_MAX / 2)) {
                int64_t year; unsigned month, date;
                days_civil((int64_t)day, &year, &month, &date);
                value = (double)(year - 1970) * 12 + month - 1;
            } else value = NAN;
        }
        if (isfinite(value) && !web_input_format_number(type, value, text, sizeof text)) text[0] = 0;
    }
    return doc_node_value(d, n, text, strlen(text)) ? WEB_INPUT_OK : WEB_INPUT_OOM;
}
enum web_input_result web_input_step(web_doc *d, node_t *n, int32_t count, bool down) {
    struct web_input_limits limits; web_input_constraints(n, &limits);
    if (!limits.numeric || limits.any) return WEB_INPUT_INVALID_STATE;
    if ((limits.has_min && limits.has_max && limits.min > limits.max) || !isfinite(limits.step)) return WEB_INPUT_OK;
    if (limits.has_min && limits.has_max && aligned(n, limits.min, &limits, true) > limits.max) return WEB_INPUT_OK;
    double value = web_input_value_number(d, n); if (!isfinite(value)) value = 0;
    double before = value;
    if (mismatch(&limits, value)) value = aligned(n, value, &limits, !down);
    else {
        long double step, base, exact = value; decimal_lattice(n, &limits, &step, &base);
        enum web_input_kind type = web_input_type(n);
        if (type == WEB_INPUT_NUMBER || type == WEB_INPUT_RANGE) floating_decimal(n->value, true, NULL, &exact);
        value = (double)(exact + step * (down ? -(long double)count : (long double)count));
    }
    if (limits.has_min && value < limits.min) value = aligned(n, limits.min, &limits, true);
    if (limits.has_max && value > limits.max) value = aligned(n, limits.max, &limits, false);
    if (!isfinite(value) || (down ? value > before : value < before)) return WEB_INPUT_OK;
    return web_input_set_number(d, n, value, false);
}

const char *web_input_edit_text(const node_t *n) {
    if (n->tag == T_input && !n->foreign && web_input_type(n) == WEB_INPUT_FILE) return n->files && n->files->count ? n->files->files[0].name : "Choose file...";
    return n->input_edit ? n->input_edit : n->value ? n->value : "";
}
void web_input_clear_edit(web_doc *d, node_t *n) {
    if (n->input_edit) {
        web_doc *allocation = n->allocation_doc ? n->allocation_doc : d;
        allocation->control_bytes -= n->input_edit_capacity;
        free(n->input_edit); n->input_edit = NULL; n->input_edit_capacity = 0;
    }
    n->input_bad_input = false; n->control_user_edited = false;
}
bool web_input_user_value(web_doc *d, node_t *n, const char *text, size_t len) {
    enum web_input_kind type = web_input_type(n);
    bool editing = web_input_numeric(type) && type != WEB_INPUT_RANGE;
    if (!editing) {
        bool ok = doc_node_value(d, n, text, len);
        if (ok) n->control_user_edited = true;
        return ok;
    }
    if (len > (16u << 20) || (len && !text)) return false;
    size_t raw_capacity = len + 1, canonical_capacity = len < 127 ? 128 : len + 1;
    size_t old_capacity = n->value_capacity + n->input_edit_capacity;
    web_doc *allocation = n->allocation_doc ? n->allocation_doc : d;
    if (d->dom_family) {
        if (raw_capacity + canonical_capacity > doc_dom_remaining(d) + old_capacity) return false;
    } else if (d->live) {
        size_t other = allocation->control_bytes - old_capacity;
        if (other > (32u << 20) || raw_capacity + canonical_capacity > (32u << 20) - other ||
            d->mem.allocated > (32u << 20) - other - raw_capacity - canonical_capacity) return false;
    }
    char *raw = malloc(raw_capacity); if (!raw) return false;
    if (len) memcpy(raw, text, len);
    raw[len] = 0;
    if (!doc_node_value(d, n, text, len)) { free(raw); return false; }
    n->input_edit = raw; n->input_edit_capacity = raw_capacity;
    double number;
    n->input_bad_input = len != 0 && !web_input_parse_number(type, raw, &number);
    n->control_user_edited = true;
    allocation->control_bytes += raw_capacity;
    uint32_t units = doc_utf16_length(raw);
    if (n->selection_start > units) n->selection_start = units;
    if (n->selection_end > units) n->selection_end = units;
    if (d->dom_family) doc_dom_budget(d);
    else if (d->live) d->mem.limit = (32u << 20) - d->control_bytes;
    return true;
}

static bool html_tag(const node_t *n, int tag) { return n && n->type == N_ELEM && !n->foreign && n->tag == tag; }
static node_t *option_select_before(node_t *option, node_t *subtree, node_t *old_parent) {
    bool group = html_tag(option, T_optgroup);
    /* Walking through the moved root substitutes its former parent without
       caching a second owner or changing the native tree during the walk. */
    for (node_t *p = option == subtree ? old_parent : option ? option->parent : NULL;
         p; p = p == subtree ? old_parent : p->parent) {
        if (p->foreign) continue;
        if (html_tag(p, T_select)) return p;
        if (html_tag(p, T_option) || html_tag(p, T_datalist) || html_tag(p, T_hr)) return NULL;
        if (html_tag(p, T_optgroup)) { if (group) return NULL; group = true; }
    }
    return NULL;
}
node_t *web_option_select(node_t *option) { return option_select_before(option, NULL, NULL); }
bool web_option_disabled(const node_t *option) {
    if (node_attr(option, "disabled")) return true;
    for (const node_t *p = option->parent; p; p = p->parent) {
        if (html_tag(p, T_select) || html_tag(p, T_option) || html_tag(p, T_datalist) || html_tag(p, T_hr)) return false;
        if (html_tag(p, T_optgroup)) return node_attr(p, "disabled") != NULL;
    }
    return false;
}
node_t *web_select_next_option(node_t *select, node_t *after) {
    if (!html_tag(select, T_select)) return NULL;
    node_t *n = after ? after : select;
    for (;;) {
        bool skip = n != select && (html_tag(n, T_select) || html_tag(n, T_hr) || html_tag(n, T_option) || html_tag(n, T_datalist) ||
                    html_tag(n, T_template) || (html_tag(n, T_optgroup) && !web_option_select(n)));
        if (!skip && n->first) n = n->first;
        else {
            while (n != select && !n->next) n = n->parent;
            if (n == select) return NULL;
            n = n->next;
        }
        if (html_tag(n, T_option) && web_option_select(n) == select) return n;
    }
}
static unsigned select_display_size(const node_t *select) {
    const char *p = node_attr(select, "size");
    if (p) {
        while (space((unsigned char)*p)) p++;
        if (*p == '+') p++;
        if (digit(*p)) {
            uint64_t size = 0;
            while (digit(*p)) { size = size * 10 + (unsigned)(*p++ - '0'); if (size > UINT32_MAX) return node_attr(select, "multiple") ? 4 : 1; }
            return (unsigned)size;
        }
    }
    return node_attr(select, "multiple") ? 4 : 1;
}
static void option_init(node_t *option) {
    if (option->checked_dirty) return;
    option->checked_dirty = true; /* option-only initialization bit */
    if (!option->selected_set) option->checked = node_attr(option, "selected") != NULL;
}
void web_select_sync(web_doc *d, node_t *select, bool reset) {
    if (!html_tag(select, T_select)) return;
    bool multiple = node_attr(select, "multiple") != NULL;
    node_t *last = NULL, *enabled = NULL;
    int first = -1, index = 0;
    for (node_t *o = web_select_next_option(select, NULL); o; o = web_select_next_option(select, o), index++) {
        option_init(o);
        if (!enabled && !web_option_disabled(o)) enabled = o;
        if (o->checked) {
            if (!multiple && last) last->checked = false;
            last = o;
            if (first < 0 || !multiple) first = index;
        }
    }
    if (reset && !multiple && !last && enabled && select_display_size(select) == 1) {
        enabled->checked = true;
        index = 0;
        for (node_t *o = web_select_next_option(select, NULL); o && o != enabled; o = web_select_next_option(select, o)) index++;
        first = index;
    }
    select->selected = first; select->control_ready = true;
    if (reset) select->selected_set = false;
    (void)d;
}
void web_option_set_selected(web_doc *d, node_t *option, bool selected, bool dirty) {
    if (!html_tag(option, T_option)) return;
    node_t *select = web_option_select(option);
    if (select) web_select_sync(d, select, !select->control_ready);
    option_init(option); option->checked = selected;
    if (dirty) option->selected_set = true;
    if (select && selected && !node_attr(select, "multiple"))
        for (node_t *o = web_select_next_option(select, NULL); o; o = web_select_next_option(select, o)) if (o != option) o->checked = false;
    if (select) web_select_sync(d, select, true);
    d->dirty = d->need_style = true;
}
void web_select_set_index(web_doc *d, node_t *select, int index) {
    if (!html_tag(select, T_select)) return;
    web_select_sync(d, select, !select->control_ready);
    int i = 0;
    for (node_t *o = web_select_next_option(select, NULL); o; o = web_select_next_option(select, o), i++) {
        o->checked = i == index;
        if (o->checked) o->selected_set = true;
    }
    select->selected = index >= 0 && index < i ? index : -1;
    select->selected_set = true;
    d->dirty = d->need_style = true;
}
void web_select_attribute_changed(web_doc *d, node_t *node, const char *name, bool present) {
    if (html_tag(node, T_select) && (!strcmp(name, "multiple") || !strcmp(name, "size"))) {
        if (!strcmp(name, "multiple") && !present) {
            bool found = false;
            for (node_t *o = web_select_next_option(node, NULL); o; o = web_select_next_option(node, o)) {
                option_init(o);
                if (o->checked) { if (found) o->checked = false; else found = true; }
            }
        }
        web_select_sync(d, node, true);
    } else if (html_tag(node, T_option)) {
        if (!strcmp(name, "selected") && !node->selected_set) web_option_set_selected(d, node, present, false);
        else if (!strcmp(name, "disabled")) {
            node_t *select = web_option_select(node); if (select) web_select_sync(d, select, true);
        }
    }
}
struct select_move { node_t *old_select, *new_select, *selected; };
static void select_subtree_moved(web_doc *d, node_t *node, node_t *subtree, node_t *old_parent, struct select_move *move) {
    if (html_tag(node, T_select)) {
        /* Its own options retain the same nearest select when the whole select
           or an enclosing container moves. Preserve an explicit empty value. */
        web_select_sync(d, node, !node->control_ready);
        return;
    }
    if (html_tag(node, T_option)) {
        option_init(node);
        node_t *old_select = option_select_before(node, subtree, old_parent);
        node_t *select = web_option_select(node);
        if (old_select != select) {
            if (old_select) move->old_select = old_select;
            if (select) {
                move->new_select = select;
                if (node->checked) move->selected = node;
            }
        }
        return;
    }
    if (html_tag(node, T_template)) return;
    for (node_t *n = node->first; n; n = n->next) select_subtree_moved(d, n, subtree, old_parent, move);
}
void web_select_inserted(web_doc *d, node_t *subtree, node_t *old_parent) {
    struct select_move move = {0};
    if (subtree) select_subtree_moved(d, subtree, subtree, old_parent, &move);
    /* Snapshot the last selected incoming option before any single-select
       normalization can clear its siblings. No internal select has changed
       owner; the changed options can only belong to the enclosing selects. */
    if (move.old_select) web_select_sync(move.old_select->owner, move.old_select, true);
    if (move.new_select) {
        if (move.selected) web_option_set_selected(d, move.selected, true, false);
        else web_select_sync(d, move.new_select, true);
    }
}
static void option_content(node_t *n, sbuf *out, bool *pending) {
    if (html_tag(n, T_script)) return;
    if (n->type == N_TEXT) {
        for (size_t i = 0; i < n->textlen; i++) {
            char c = n->text[i];
            if (space((unsigned char)c)) { if (out->n) *pending = true; }
            else { if (*pending) sb_putc(out, ' '); sb_putc(out, c); *pending = false; }
        }
    } else for (node_t *c = n->first; c; c = c->next) option_content(c, out, pending);
}
void web_option_value(node_t *option, sbuf *out) {
    const char *value = node_attr(option, "value");
    if (value) sb_puts(out, value);
    else web_option_text(option, out);
}
void web_option_text(node_t *option, sbuf *out) { bool pending = false; option_content(option, out, &pending); }

/* Form association, native suggestions and gauges share the real DOM. */
bool web_control_labelable(const node_t *n) {
    if (!n || n->type != N_ELEM || n->foreign) return false;
    return n->face_associated || n->tag == T_button || n->tag == T_select || n->tag == T_textarea || n->tag == T_output ||
        n->tag == T_progress || n->tag == T_meter || (n->tag == T_input && web_input_type(n) != WEB_INPUT_HIDDEN);
}
static node_t *control_first_id(node_t *root, const char *id) {
    if (root->type == N_ELEM && root->id && !strcmp(root->id, id)) return root;
    for (node_t *child = root->first; child; child = child->next) {
        node_t *found = control_first_id(child, id); if (found) return found;
    }
    return NULL;
}
static node_t *label_descendant(node_t *label) {
    for (node_t *child = label->first; child; child = child->next) {
        if (web_control_labelable(child)) return child;
        node_t *found = label_descendant(child); if (found) return found;
    }
    return NULL;
}
node_t *web_label_control(node_t *label) {
    if (!html_tag(label, T_label)) return NULL;
    const char *id = node_attr(label, "for");
    if (!id) return label_descendant(label);
    node_t *control = *id ? control_first_id(doc_node_root(label, false), id) : NULL;
    return web_control_labelable(control) ? control : NULL;
}
node_t *web_label_activation(node_t *target) {
    if (!target) return NULL;
    for (node_t *n = target; n; n = n->parent) {
        if (html_tag(n, T_label)) {
            node_t *control = web_label_control(n);
            return control == target ? NULL : control;
        }
        if (n->type != N_ELEM || n->foreign) continue;
        if (web_control_labelable(n) || n->tag == T_summary || n->tag == T_iframe || n->tag == T_embed ||
            (n->tag == T_a && node_attr(n, "href")) ||
            ((n->tag == T_audio || n->tag == T_video) && node_attr(n, "controls"))) return NULL;
    }
    return NULL;
}
node_t *web_input_datalist(node_t *input) {
    if (!html_tag(input, T_input)) return NULL;
    enum web_input_kind kind = web_input_type(input);
    if (!(kind <= WEB_INPUT_EMAIL || (kind >= WEB_INPUT_DATE && kind <= WEB_INPUT_COLOR))) return NULL;
    const char *id = node_attr(input, "list");
    node_t *list = id && *id ? control_first_id(doc_node_root(input, false), id) : NULL;
    return html_tag(list, T_datalist) ? list : NULL;
}
/* The caller does not retain author-controlled string pointers across mutation:
   an option number is resolved again at selection, never a cached filename. */
node_t *web_datalist_option(node_t *input, int index) {
    node_t *list = web_input_datalist(input);
    if (!list || index < 0) return NULL;
    node_t *n = list;
    for (;;) {
        if (n->first && !html_tag(n, T_template)) n = n->first;
        else {
            while (n != list && !n->next) n = n->parent;
            if (n == list) return NULL;
            n = n->next;
        }
        if (html_tag(n, T_option) && !web_option_disabled(n)) {
            sbuf value = {0}; web_option_value(n, &value);
            bool available = value.n != 0; sb_free(&value);
            if (available && !index--) return n;
        }
    }
}
int web_datalist_options(web_doc *d, web_node *input, const char **labels, int capacity) {
    if (!d || !input || input->owner != d || !labels || capacity <= 0) return 0;
    int count = 0;
    for (node_t *option; count < capacity && (option = web_datalist_option(input, count)); count++) {
        if (!option->option_label || option->option_label_revision != d->dom_revision) {
            sbuf *label = calloc(1, sizeof *label); if (!label) return count;
            const char *attribute = node_attr(option, "label");
            if (attribute && *attribute) sb_puts(label, attribute); else web_option_text(option, label);
            if (!label->n) web_option_value(option, label);
            jmp_buf trap; jmp_buf *old = d->mem.trap; d->mem.trap = &trap;
            if (setjmp(trap)) { d->mem.trap = old; sb_free(label); free(label); return count; }
            option->option_label = ar_strdup(&d->mem, label->p ? sb_cstr(label) : "");
            d->mem.trap = old; sb_free(label); free(label); option->option_label_revision = d->dom_revision;
        }
        labels[count] = option->option_label;
    }
    return count;
}
bool web_datalist_choose(web_doc *d, web_node *input, int index) {
    if (!d || !input || input->owner != d || web_control_disabled(input) || node_attr(input, "readonly")) return false;
    node_t *option = web_datalist_option(input, index); if (!option) return false;
    sbuf value = {0}; web_option_value(option, &value);
    bool result = web_input_user_value(d, input, value.p ? sb_cstr(&value) : "", value.n);
    sb_free(&value);
    if (result) doc_control_selection(d, input, doc_utf16_length(input->value), doc_utf16_length(input->value), 0);
    return result;
}
static double gauge_attribute(const node_t *n, const char *name, double fallback) {
    double value;
    return floating_decimal(node_attr(n, name), false, &value, NULL) ? value : fallback;
}
static double gauge_clamp(double value, double min, double max) { return value < min ? min : value > max ? max : value; }
double web_gauge_value(const node_t *n, const char *property) {
    if (!html_tag(n, T_progress) && !html_tag(n, T_meter)) return 0;
    bool meter = n->tag == T_meter;
    double min = meter ? gauge_attribute(n, "min", 0) : 0;
    double max = gauge_attribute(n, "max", 1);
    if (meter) { if (max < min) max = min; } else if (max <= 0) max = 1;
    double value = gauge_clamp(gauge_attribute(n, "value", 0), min, max);
    if (!strcmp(property, "min")) return min;
    if (!strcmp(property, "max")) return max;
    if (!strcmp(property, "value")) return value;
    if (!strcmp(property, "position")) return !node_attr(n, "value") ? -1 : value / max;
    double low = gauge_clamp(gauge_attribute(n, "low", min), min, max);
    double high = gauge_clamp(gauge_attribute(n, "high", max), low, max);
    if (!strcmp(property, "low")) return low;
    if (!strcmp(property, "high")) return high;
    return gauge_clamp(gauge_attribute(n, "optimum", min / 2 + max / 2), min, max);
}
int web_meter_quality(const node_t *n) {
    double value = web_gauge_value(n, "value"), low = web_gauge_value(n, "low"),
        high = web_gauge_value(n, "high"), optimum = web_gauge_value(n, "optimum");
    if (optimum < low) return value <= low ? 0 : value <= high ? 1 : 2;
    if (optimum > high) return value >= high ? 0 : value >= low ? 1 : 2;
    return value >= low && value <= high ? 0 : 1;
}
/* API values remain LF-only. Submission normalization and hard wrapping do
   not make the control dirty or alter selection/validation lengths. */
static bool submission_capacity(sbuf *out, size_t length) {
    if (length > WEB_FILE_BYTES) return false;
    if (length < out->cap) return true;
    size_t capacity = out->cap ? out->cap : 64;
    while (capacity <= length && capacity < WEB_FILE_BYTES) capacity *= 2;
    if (capacity <= length) capacity = WEB_FILE_BYTES + 1;
    char *bytes = realloc(out->p, capacity);
    if (!bytes) return false;
    out->p = bytes; out->cap = capacity; return true;
}
bool web_textarea_submission(node_t *n, sbuf *out) {
    const char *value = n->value ? n->value : "", *wrap = node_attr(n, "wrap");
    unsigned columns = 20, column = 0;
    const char *cols = node_attr(n, "cols");
    if (cols) {
        while (space((unsigned char)*cols)) cols++;
        if (*cols == '+') cols++;
        if (digit(*cols)) { unsigned count = 0; while (digit(*cols) && count < 100000) count = count * 10 + (unsigned)(*cols++ - '0'); if (count && count < 100000) columns = count; }
    }
    bool hard = wrap && str_ieq(wrap, "hard");
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        size_t extra = *p == '\r' || *p == '\n' ? 2 :
            1 + (hard && (*p & 0xc0) != 0x80 && column == columns ? 2 : 0);
        if (!submission_capacity(out, out->n + extra)) return false;
        if (*p == '\r' || *p == '\n') { if (*p == '\r' && p[1] == '\n') p++; sb_puts(out, "\r\n"); column = 0; }
        else {
            if ((*p & 0xc0) != 0x80) { if (hard && column == columns) { sb_puts(out, "\r\n"); column = 0; } column++; }
            sb_putc(out, (char)*p);
        }
    }
    return true;
}
static int form_first_strong(const char *text) {
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    while (*p) {
        uint32_t c = *p++, minimum = 0; unsigned remaining = 0;
        if (c >= 0xc2 && c < 0xe0) { c &= 0x1f; remaining = 1; minimum = 0x80; }
        else if (c >= 0xe0 && c < 0xf0) { c &= 0x0f; remaining = 2; minimum = 0x800; }
        else if (c >= 0xf0 && c < 0xf5) { c &= 7; remaining = 3; minimum = 0x10000; }
        else if (c >= 0x80) continue;
        bool valid = true;
        while (remaining--) { if ((*p & 0xc0) != 0x80) { valid = false; break; } c = (c << 6) | (*p++ & 0x3f); }
        if (!valid || c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) continue;
        size_t low = 0, high = sizeof form_bidi_ranges / sizeof *form_bidi_ranges;
        while (low < high) {
            size_t mid = low + (high - low) / 2;
            if (c < form_bidi_ranges[mid].first) high = mid;
            else if (c > form_bidi_ranges[mid].last) low = mid + 1;
            else return form_bidi_ranges[mid].direction;
        }
    }
    return 0;
}
static int form_first_strong_tree(node_t *root) {
    for (node_t *n = root->first; n; n = n->next) {
        if (n->type == N_TEXT) { int strong = form_first_strong(n->text); if (strong) return strong; }
        else if (n->type == N_ELEM) {
            const char *dir = node_attr(n, "dir");
            if (n->tag == T_bdi || n->tag == T_script || n->tag == T_style || n->tag == T_textarea ||
                (dir && (str_ieq(dir, "ltr") || str_ieq(dir, "rtl") || str_ieq(dir, "auto")))) continue;
            int strong = form_first_strong_tree(n); if (strong) return strong;
        }
    }
    return 0;
}
const char *web_control_direction(node_t *control) {
    for (node_t *n = control; n; n = n->parent ? n->parent : n->shadow_host) {
        if (n->type != N_ELEM || n->foreign) continue;
        const char *direction = node_attr(n, "dir");
        if (direction && str_ieq(direction, "rtl")) return "rtl";
        if (direction && str_ieq(direction, "ltr")) return "ltr";
        if ((direction && str_ieq(direction, "auto")) || n->tag == T_bdi) {
            int strong;
            if (n == control && (html_tag(n, T_input) || html_tag(n, T_textarea))) strong = form_first_strong(n->value);
            else strong = form_first_strong_tree(n);
            return strong == 2 ? "rtl" : "ltr";
        }
        if (n == control && html_tag(n, T_input) && web_input_type(n) == WEB_INPUT_TEL) return "ltr";
    }
    return "ltr";
}

/* This request owns every exported byte until free. Native transport receives
   body_len and content_type, not a guessed strlen or a form-data placeholder. */
struct form_encoding { sbuf body; const char *kind; char boundary[96]; bool collision, failed; };
static bool submission_contains(const void *bytes, size_t length, const char *needle) {
    size_t count = strlen(needle); const unsigned char *data = bytes;
    if (count > length) return false;
    for (size_t i = 0; i <= length - count; i++) if (!memcmp(data + i, needle, count)) return true;
    return false;
}
static bool submission_newlines(sbuf *out, const char *text) {
    size_t length = 0;
    for (const char *p = text ? text : ""; *p; p++) {
        length += *p == '\r' || *p == '\n' ? 2 : 1;
        if (*p == '\r' && p[1] == '\n') p++;
        if (length > WEB_FILE_BYTES) return false;
    }
    if (!submission_capacity(out, out->n + length)) return false;
    for (const char *p = text ? text : ""; *p; p++) {
        if (*p == '\r' || *p == '\n') { if (*p == '\r' && p[1] == '\n') p++; sb_puts(out, "\r\n"); }
        else sb_putc(out, *p);
    }
    return true;
}
static size_t multipart_name_length(const char *name) {
    size_t length = 0;
    for (const char *p = name; *p; p++) length += *p == '"' || *p == '\r' || *p == '\n' ? 3 : 1;
    return length;
}
static size_t submission_url_length(const char *text) {
    size_t length = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        length += ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '*' || *p == '-' || *p == '.' || *p == '_' || *p == ' ') ? 1 : 3;
    return length;
}
static void multipart_name(sbuf *out, const char *name) {
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p == '"') sb_puts(out, "%22"); else if (*p == '\r') sb_puts(out, "%0D"); else if (*p == '\n') sb_puts(out, "%0A"); else sb_putc(out, (char)*p);
    }
}
static void submission_pair(struct form_encoding *encoding, const char *name, const char *text) {
    if (encoding->failed) return;
    size_t name_len = strlen(name), text_len = strlen(text ? text : "");
    if (name_len > (16u << 20) || text_len > (16u << 20) || encoding->body.n > (16u << 20) - name_len || text_len > (16u << 20) - name_len - encoding->body.n) { encoding->failed = true; return; }
    sbuf normalized_name = {0}, normalized_value = {0};
    if (!submission_newlines(&normalized_name, name) || !submission_newlines(&normalized_value, text)) {
        encoding->failed = true; sb_free(&normalized_name); sb_free(&normalized_value); return;
    }
    name = normalized_name.p ? sb_cstr(&normalized_name) : "";
    text = normalized_value.p ? sb_cstr(&normalized_value) : "";
    sbuf *body = &encoding->body;
    size_t extra = !strcmp(encoding->kind, "text/plain") ? normalized_name.n + normalized_value.n + 3 :
        !strcmp(encoding->kind, "multipart/form-data") ? 2 + strlen(encoding->boundary) + strlen("\r\nContent-Disposition: form-data; name=\"") + multipart_name_length(name) + 5 + normalized_value.n + 2 :
        (body->n ? 1 : 0) + submission_url_length(name) + 1 + submission_url_length(text);
    if (!submission_capacity(body, body->n + extra)) { encoding->failed = true; sb_free(&normalized_name); sb_free(&normalized_value); return; }
    if (!strcmp(encoding->kind, "text/plain")) {
        sb_puts(body, name); sb_putc(body, '='); sb_puts(body, text); sb_puts(body, "\r\n");
    } else if (!strcmp(encoding->kind, "multipart/form-data")) {
        if (strstr(name, encoding->boundary) || strstr(text, encoding->boundary)) encoding->collision = true;
        sb_puts(body, "--"); sb_puts(body, encoding->boundary);
        sb_puts(body, "\r\nContent-Disposition: form-data; name=\""); multipart_name(body, name);
        sb_puts(body, "\"\r\n\r\n"); sb_puts(body, text); sb_puts(body, "\r\n");
    } else {
        if (body->n) sb_putc(body, '&');
        url_encode_form(body, name); sb_putc(body, '='); url_encode_form(body, text);
    }
    if (body->n > (16u << 20)) encoding->failed = true;
    sb_free(&normalized_name); sb_free(&normalized_value);
}
/* FACE FormData entries retain lengths, including embedded NUL and file
   bytes. No strlen is used to truncate author form values or file names. */
static bool face_newlines(sbuf *out,const void *data,size_t length){
    const unsigned char *bytes=data;size_t size=0;
    for(size_t i=0;i<length;i++){
        size+=bytes[i]=='\r'||bytes[i]=='\n'?2:1;
        if(bytes[i]=='\r'&&i+1<length&&bytes[i+1]=='\n')i++;
        if(size>WEB_FILE_BYTES)return false;
    }
    if(!submission_capacity(out,size))return false;
    for(size_t i=0;i<length;i++){
        if(bytes[i]=='\r'||bytes[i]=='\n'){if(bytes[i]=='\r'&&i+1<length&&bytes[i+1]=='\n')i++;sb_puts(out,"\r\n");}
        else sb_putc(out,(char)bytes[i]);
    }return true;
}
static bool face_url_safe(unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='*'||c=='-'||c=='.'||c=='_'||c==' ';}
static size_t face_encoded_size(const sbuf *text,bool name){
    size_t size=0;for(size_t i=0;i<text->n;i++){unsigned char c=(unsigned char)text->p[i];
        size+=name?(c=='"'||c=='\r'||c=='\n'?3:1):(face_url_safe(c)?1:3);
    }return size;
}
static void face_encoded(sbuf *out,const sbuf *text,bool name){
    static const char hex[]="0123456789ABCDEF";
    for(size_t i=0;i<text->n;i++){unsigned char c=(unsigned char)text->p[i];
        if(name?(c=='"'||c=='\r'||c=='\n'):!face_url_safe(c)){sb_putc(out,'%');sb_putc(out,hex[c>>4]);sb_putc(out,hex[c&15]);}
        else sb_putc(out,!name&&c==' '?'+':(char)c);
    }
}
static void submission_face(struct form_encoding *encoding,const struct web_face_entry *entry,const char *single_name){
    if(encoding->failed)return;
    const char *name=single_name?single_name:entry->name;size_t name_length=single_name?strlen(single_name):entry->name_length;
    bool multipart=!strcmp(encoding->kind,"multipart/form-data"),plain=!strcmp(encoding->kind,"text/plain");
    sbuf normal_name={0},normal_value={0};
    const void *value=entry->file?(const void*)entry->filename:(const void*)entry->bytes;
    size_t value_length=entry->file?entry->filename_length:entry->size;
    if(!face_newlines(&normal_name,name,name_length)||!face_newlines(&normal_value,value,value_length))encoding->failed=true;
    sbuf *body=&encoding->body;
    const char *mime=entry->mime&&*entry->mime?entry->mime:"application/octet-stream";
    size_t extra=0;
    if(!encoding->failed){
        if(plain)extra=normal_name.n+normal_value.n+3;
        else if(multipart){
            extra=2+strlen(encoding->boundary)+strlen("\r\nContent-Disposition: form-data; name=\"")+face_encoded_size(&normal_name,true)+5+2;
            if(entry->file)extra+=strlen("; filename=\"")+face_encoded_size(&normal_value,true)+1+strlen("\r\nContent-Type: ")+strlen(mime)+entry->size;
            else extra+=normal_value.n;
        }else extra=(body->n?1:0)+face_encoded_size(&normal_name,false)+1+face_encoded_size(&normal_value,false);
        if(body->n>WEB_FILE_BYTES||extra>WEB_FILE_BYTES-body->n||!submission_capacity(body,body->n+extra))encoding->failed=true;
    }
    if(!encoding->failed){
        if(plain){sb_put(body,normal_name.p,normal_name.n);sb_putc(body,'=');sb_put(body,normal_value.p,normal_value.n);sb_puts(body,"\r\n");}
        else if(multipart){
            if(submission_contains(normal_name.p,normal_name.n,encoding->boundary)||
                submission_contains(normal_value.p,normal_value.n,encoding->boundary)||
                (entry->file&&submission_contains(entry->bytes,entry->size,encoding->boundary)))encoding->collision=true;
            sb_puts(body,"--");sb_puts(body,encoding->boundary);sb_puts(body,"\r\nContent-Disposition: form-data; name=\"");face_encoded(body,&normal_name,true);
            if(entry->file){sb_puts(body,"\"; filename=\"");face_encoded(body,&normal_value,true);sb_puts(body,"\"\r\nContent-Type: ");sb_puts(body,mime);}
            else sb_putc(body,'"');
            sb_puts(body,"\r\n\r\n");
            if(entry->file)sb_put(body,(const char*)entry->bytes,entry->size);else sb_put(body,normal_value.p,normal_value.n);
            sb_puts(body,"\r\n");
        }else{
            if(body->n)sb_putc(body,'&');face_encoded(body,&normal_name,false);sb_putc(body,'=');face_encoded(body,&normal_value,false);
        }
    }
    sb_free(&normal_name);sb_free(&normal_value);
}

static bool dirname_applicable(node_t *n) {
    return html_tag(n, T_textarea) || (html_tag(n, T_input) && (web_input_type(n) <= WEB_INPUT_URL || web_input_type(n) == WEB_INPUT_EMAIL));
}
static void submission_collect(web_doc *d, node_t *form, node_t *scope, node_t *submitter, struct form_encoding *encoding) {
    if (encoding->failed) return;
    for (node_t *n = scope->first; n; n = n->next) {
        if (n->type != N_ELEM || n->foreign || n->tag == T_template || n->tag == T_datalist) continue;
        const char *name = node_attr(n, "name");
        if (web_form_owner(d, n) == form && !web_control_disabled(n)) {
            doc_control_init(d, n);
            if(n->face_associated){
                struct web_face_value *v=n->internals?n->internals->value:NULL;
                if(v && (!v->single || (name && *name)))for(unsigned i=0;i<v->count;i++)
                    submission_face(encoding,&v->entries[i],v->single?name:NULL);
            } else if (n->tag == T_input && web_input_type(n) == WEB_INPUT_IMAGE && n == submitter) {
                sbuf coord = {0};
                if (!submission_capacity(&coord, (name ? strlen(name) : 0) + 2)) { encoding->failed = true; return; }
                if (name && *name) { sb_puts(&coord, name); sb_putc(&coord, '.'); } sb_putc(&coord, 'x');
                submission_pair(encoding, sb_cstr(&coord), "0"); coord.n--; sb_putc(&coord, 'y'); submission_pair(encoding, sb_cstr(&coord), "0"); sb_free(&coord);
            } else if (name && *name) {
                if (n->tag == T_input) {
                    enum web_input_kind kind = web_input_type(n);
                    if (kind == WEB_INPUT_CHECKBOX || kind == WEB_INPUT_RADIO) {
                        if (n->checked) { const char *value = node_attr(n, "value"); submission_pair(encoding, name, value ? value : "on"); }
                    } else if (kind == WEB_INPUT_SUBMIT) { if (n == submitter) { const char *value = node_attr(n, "value"); submission_pair(encoding, name, value ? value : "Submit"); } }
                    else if (kind == WEB_INPUT_FILE) {
                        unsigned count = n->files ? n->files->count : 0;
                        for (unsigned index = 0; index < (count ? count : 1); index++) {
                            const struct web_form_file *file = count ? &n->files->files[index] : NULL;
                            const char *filename = file ? file->name : "";
                            if (strcmp(encoding->kind, "multipart/form-data")) submission_pair(encoding, name, filename);
                            else {
                                sbuf *body = &encoding->body;
                                if (encoding->failed || body->n > WEB_FILE_BYTES || (file && file->size > WEB_FILE_BYTES - body->n)) { encoding->failed = true; break; }
                                sbuf part_name = {0}, part_filename = {0};
                                if (!submission_newlines(&part_name, name) || !submission_newlines(&part_filename, filename)) {
                                    encoding->failed = true; sb_free(&part_name); sb_free(&part_filename); break;
                                }
                                const char *field_name = sb_cstr(&part_name), *field_filename = sb_cstr(&part_filename), *mime = file && *file->type ? file->type : "application/octet-stream";
                                size_t extra = 2 + strlen(encoding->boundary) + strlen("\r\nContent-Disposition: form-data; name=\"") + multipart_name_length(field_name) +
                                    strlen("\"; filename=\"") + multipart_name_length(field_filename) + strlen("\"\r\nContent-Type: ") + strlen(mime) + 4 + (file ? file->size : 0) + 2;
                                if (!submission_capacity(body, body->n + extra)) { encoding->failed = true; sb_free(&part_name); sb_free(&part_filename); break; }
                                if ((file && file->size && submission_contains(file->bytes, file->size, encoding->boundary)) || strstr(filename, encoding->boundary) || strstr(name, encoding->boundary)) encoding->collision = true;
                                sb_puts(body, "--"); sb_puts(body, encoding->boundary);
                                sb_puts(body, "\r\nContent-Disposition: form-data; name=\""); multipart_name(body, field_name);
                                sb_puts(body, "\"; filename=\""); multipart_name(body, field_filename);
                                sb_puts(body, "\"\r\nContent-Type: "); sb_puts(body, mime);
                                sb_puts(body, "\r\n\r\n"); if (file && file->size) sb_put(body, (const char *)file->bytes, file->size); sb_puts(body, "\r\n");
                                sb_free(&part_name); sb_free(&part_filename);
                                if (body->n > WEB_FILE_BYTES) encoding->failed = true;
                            }
                        }
                    } else if (kind < WEB_INPUT_SUBMIT) submission_pair(encoding, name, n->value ? n->value : "");
                } else if (n->tag == T_textarea) {
                    sbuf wrapped = {0};
                    if (web_textarea_submission(n, &wrapped)) submission_pair(encoding, name, wrapped.p ? sb_cstr(&wrapped) : "");
                    else encoding->failed = true;
                    sb_free(&wrapped);
                } else if (n->tag == T_select) {
                    for (node_t *option = web_select_next_option(n, NULL); option; option = web_select_next_option(n, option)) {
                        if (!option->checked || web_option_disabled(option)) continue;
                        sbuf value = {0};
                        const char *attribute = node_attr(option, "value"); size_t capacity = attribute ? strlen(attribute) : 0;
                        if (!attribute) for (node_t *p = option; p; ) {
                            if (p->type == N_TEXT) capacity += p->textlen;
                            if (capacity > WEB_FILE_BYTES) break;
                            if (p->first && !html_tag(p, T_script)) p = p->first;
                            else { while (p != option && !p->next) p = p->parent; if (p == option) break; p = p->next; }
                        }
                        if (!submission_capacity(&value, capacity)) { encoding->failed = true; break; }
                        web_option_value(option, &value);
                        submission_pair(encoding, name, value.p ? sb_cstr(&value) : ""); sb_free(&value);
                    }
                } else if (n->tag == T_button && n == submitter && web_control_submit_button(n)) {
                    const char *value = node_attr(n, "value"); submission_pair(encoding, name, value ? value : "");
                }
                const char *dirname = node_attr(n, "dirname");
                if (dirname && *dirname && dirname_applicable(n)) submission_pair(encoding, dirname, web_control_direction(n));
            }
        }
        if (encoding->failed) return;
        submission_collect(d, form, n, submitter, encoding);
    }
}
void web_submit_request_free(struct web_form_request *request) {
    if (!request) return;
    free(request->url); free(request->body); free(request->content_type); free(request->target);
    memset(request, 0, sizeof *request);
}
static const char *submit_attribute(node_t *form, node_t *submitter, const char *override, const char *attribute) {
    const char *value = web_control_submit_button(submitter) ? node_attr(submitter, override) : NULL;
    return value ? value : node_attr(form, attribute);
}
bool web_submit_request(web_doc *d, web_node *submitter, struct web_form_request *request) {
    if (!request) return false;
    memset(request, 0, sizeof *request);
    if (!d || !submitter || submitter->owner != d || submitter->type != N_ELEM || submitter->foreign) return false;
    node_t *form = html_tag(submitter, T_form) ? submitter : web_form_owner(d, submitter);
    if (!html_tag(form, T_form)) return false;
    const char *action = submit_attribute(form, submitter, "formaction", "action");
    const char *method = submit_attribute(form, submitter, "formmethod", "method");
    if (method && str_ieq(method, "dialog")) return false; // handled by the native dialog owner, never GET
    bool post = method && str_ieq(method, "post");
    const char *kind = submit_attribute(form, submitter, "formenctype", "enctype");
    kind = post && kind && str_ieq(kind, "text/plain") ? "text/plain" : post && kind && str_ieq(kind, "multipart/form-data") ? "multipart/form-data" : "application/x-www-form-urlencoded";
    const char *target = submit_attribute(form, submitter, "formtarget", "target");
    if (!target) {
        for (node_t *n = d->root; n; ) {
            if (html_tag(n, T_base) && node_attr(n, "target")) { target = node_attr(n, "target"); break; }
            if (n->first && !html_tag(n, T_template)) n = n->first;
            else { while (n != d->root && !n->next) n = n->parent; if (n == d->root) break; n = n->next; }
        }
    }
    if (!target || !*target) target = "_self";
    if (strchr(target, '\t') || strchr(target, '\n') || strchr(target, '<')) target = "_blank";
    char absolute[2048];
    if (!url_resolve(d->base, action && *action ? action : d->url, absolute, sizeof absolute) || !strncasecmp(absolute, "javascript:", 11)) return false;
    struct form_encoding encoding = {.kind = kind};
    uint64_t stamp = uptime_ms();
    for (unsigned retry = 0; retry < 64; retry++) {
        encoding.body.n = 0; encoding.collision = false;
        snprintf(encoding.boundary, sizeof encoding.boundary, "----NocturneForm%llx%llx%u", (unsigned long long)stamp, (unsigned long long)d->dom_revision, retry);
        submission_collect(d, form, doc_node_root(form, false), submitter, &encoding);
        if (encoding.failed || !encoding.collision) break;
    }
    if (encoding.failed || encoding.collision) { sb_free(&encoding.body); return false; }
    if (!strcmp(kind, "multipart/form-data")) {
        if (!submission_capacity(&encoding.body, encoding.body.n + strlen(encoding.boundary) + 6)) { sb_free(&encoding.body); return false; }
        sb_puts(&encoding.body, "--"); sb_puts(&encoding.body, encoding.boundary); sb_puts(&encoding.body, "--\r\n");
    }
    request->target = strdup(target);
    if (post) {
        request->url = strdup(absolute); request->body_len = encoding.body.n;
        request->body = malloc(request->body_len + 1);
        if (request->body) { if (request->body_len) memcpy(request->body, encoding.body.p, request->body_len); request->body[request->body_len] = 0; }
        sbuf content = {0};
        if (!submission_capacity(&content, strlen(kind) + strlen(encoding.boundary) + 11)) { sb_free(&encoding.body); web_submit_request_free(request); return false; }
        sb_puts(&content, kind);
        if (!strcmp(kind, "multipart/form-data")) { sb_puts(&content, "; boundary="); sb_puts(&content, encoding.boundary); }
        request->content_type = strdup(sb_cstr(&content)); sb_free(&content);
    } else {
        char fragment_text[2048] = {0};
        char *fragment = strchr(absolute, '#'); if (fragment) { strcpy(fragment_text, fragment); *fragment = 0; }
        char *query = strchr(absolute, '?'); if (query) *query = 0;
        size_t length = strlen(absolute) + encoding.body.n + strlen(fragment_text) + 2;
        request->url = malloc(length);
        if (request->url) snprintf(request->url, length, "%s?%s%s", absolute, encoding.body.p ? sb_cstr(&encoding.body) : "", fragment_text);
    }
    sb_free(&encoding.body);
    if (!request->url || !request->target || (post && (!request->body || !request->content_type))) { web_submit_request_free(request); return false; }
    return true;
}

bool web_radio_same_group(web_doc *d, node_t *a, node_t *b) {
    if (!html_tag(a, T_input) || !html_tag(b, T_input) ||
        web_input_type(a) != WEB_INPUT_RADIO || web_input_type(b) != WEB_INPUT_RADIO) return false;
    const char *name = node_attr(a, "name"), *other = node_attr(b, "name");
    return name && *name && other && !strcmp(name, other) &&
        doc_node_root(a, false) == doc_node_root(b, false) && web_form_owner(d, a) == web_form_owner(d, b);
}
/* Checkedness is visible during click dispatch; canceled activation restores
   the captured native state rather than trying to undo a second toggle. */
void web_control_activation_begin(web_doc *d, web_node *n, struct web_control_activation *activation) {
    memset(activation, 0, sizeof *activation);
    if (!d || !html_tag(n, T_input) || web_control_disabled(n)) return;
    enum web_input_kind kind = web_input_type(n);
    if (kind != WEB_INPUT_CHECKBOX && kind != WEB_INPUT_RADIO) return;
    doc_control_init(d, n);
    activation->control = n; activation->checked = n->checked; activation->indeterminate = n->indeterminate;
    activation->radio = kind == WEB_INPUT_RADIO;
    if (activation->radio) {
        node_t *root = doc_node_root(n, false);
        for (node_t *p = root; p; ) {
            if (p == n || web_radio_same_group(d, n, p)) {
                doc_control_init(d, p); if (p->checked) activation->previous = p;
            }
            if (p->first) p = p->first;
            else { while (p != root && !p->next) p = p->parent; if (p == root) break; p = p->next; }
        }
    } else n->indeterminate = false;
    doc_control_checked(d, n, activation->radio ? true : !activation->checked);
}
bool web_control_activation_end(web_doc *d, struct web_control_activation *activation, bool allowed) {
    node_t *n = activation->control;
    if (!n || !d || n->owner != d) return false;
    enum web_input_kind kind = web_input_type(n);
    if (!allowed) {
        if (kind == WEB_INPUT_CHECKBOX) {
            n->indeterminate = activation->indeterminate; doc_control_checked(d, n, activation->checked);
        } else if (kind == WEB_INPUT_RADIO) {
            node_t *previous = activation->previous;
            if (previous && (previous == n || web_radio_same_group(d, n, previous))) doc_control_checked(d, previous, true);
            else doc_control_checked(d, n, false);
        }
        return false;
    }
    return doc_node_connected(n) && !web_control_disabled(n) && (kind == WEB_INPUT_CHECKBOX || kind == WEB_INPUT_RADIO);
}
