#ifndef WEB_SVG_GEOMETRY_H
#define WEB_SVG_GEOMETRY_H
#include <stdlib.h>
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
#endif
