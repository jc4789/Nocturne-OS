#ifndef WEB_SVG_GEOMETRY_H
#define WEB_SVG_GEOMETRY_H
#include <stdlib.h>
#include <string.h>
#include <math.h>

static inline int svg_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static inline int svg_digit(char c) { return c >= '0' && c <= '9'; }

/* SVG number, not libc's inf/nan/hex extensions. Bound the copied token so an
   attribute cannot make strtod scan arbitrary input. No scanset scanf needed. */
static inline int svg_number(const char **cursor, float *value) {
    const char *s = *cursor, *p = s;
    if (*p == '+' || *p == '-') p++;
    int digits = 0;
    while (svg_digit(*p)) { p++; digits++; }
    if (*p == '.') { p++; while (svg_digit(*p)) { p++; digits++; } }
    if (!digits) return 0;
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') p++;
        const char *start = p;
        while (svg_digit(*p)) p++;
        if (p == start) return 0;
    }
    if (p - s >= 64) return 0;
    char token[64];
    int n = (int)(p - s);
    for (int i = 0; i < n; i++) token[i] = s[i];
    token[n] = 0;
    char *end;
    double v = strtod(token, &end);
    if (*end || !isfinite(v) || v > 3.402823466e38 || v < -3.402823466e38) return 0;
    *value = (float)v;
    *cursor = p;
    return 1;
}

/* 0: missing/invalid (ignore it); 1: positive viewport; 2: valid zero viewport
   (suppress drawing). Negative width/height are invalid, not zero viewports. */
static inline int svg_viewbox(const char *s, float out[4]) {
    if (!s) return 0;
    while (svg_space(*s)) s++;
    for (int i = 0; i < 4; i++) {
        if (!svg_number(&s, &out[i])) return 0;
        const char *before = s;
        while (svg_space(*s)) s++;
        if (i < 3) {
            if (*s == ',') { s++; while (svg_space(*s)) s++; }
            else if (before == s) return 0;
        }
    }
    if (*s || out[2] < 0 || out[3] < 0) return 0;
    return out[2] == 0 || out[3] == 0 ? 2 : 1;
}

/* Whether d is whole, valid SVG path data (SVG 2 §9.3.9): a moveto first, each command's
   argument groups complete, an arc's flags single 0 or 1 digits, commas only between numbers. */
static inline int svg_path_data_valid(const char *d) {
    static const char commands[] = "MmZzLlHhVvCcSsQqTtAa";
    static const unsigned char arity[] = {2, 2, 0, 0, 2, 2, 1, 1, 1, 1, 6, 6, 4, 4, 4, 4, 2, 2, 7, 7};
    const char *p = d;
    int command = -1;
    for (;;) {
        while (svg_space(*p)) p++;
        if (!*p) return command >= 0;
        const char *found = strchr(commands, *p);
        if (found) {
            int index = (int)(found - commands);
            if (command < 0 && index > 1) return 0;
            command = index;
            p++;
            if (!arity[command]) continue;
        } else if (command < 0 || !arity[command]) return 0;
        for (int i = 0; i < arity[command]; i++) {
            while (svg_space(*p)) p++;
            if (i && *p == ',') { p++; while (svg_space(*p)) p++; }
            if (arity[command] == 7 && (i == 3 || i == 4)) {
                if (*p != '0' && *p != '1') return 0;
                p++;
            } else {
                float value;
                if (!svg_number(&p, &value)) return 0;
            }
        }
        while (svg_space(*p)) p++;
        if (*p == ',') {
            p++;
            while (svg_space(*p)) p++;
            if (!*p || strchr(commands, *p)) return 0; /* a comma joins argument groups only */
        }
    }
}
#endif
