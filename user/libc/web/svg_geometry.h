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

/* One command of SVG path data, as written: its letter (z is kept as Z, which means the
   same) and its numbers, an arc's flags as 0 or 1. An argument group repeated after its
   command is a command of its own, and one after a moveto is a lineto (SVG 2 §9.3.3). */
struct svg_path_seg { char cmd; float v[7]; };
static inline int svg_path_arity(char cmd) {
    switch (cmd | 32) {
    case 'm': case 'l': case 't': return 2;
    case 'h': case 'v': return 1;
    case 'c': return 6;
    case 's': case 'q': return 4;
    case 'a': return 7;
    }
    return 0;
}
/* Whole, valid SVG path data (SVG 2 §9.3.9): a moveto first, each command's argument groups
   complete, an arc's flags single 0 or 1 digits, commas only between numbers. The number of
   commands, each written to seg[] while it fits cap; -1 when d is not path data. */
static inline long svg_path_data_parse(const char *d, struct svg_path_seg *seg, size_t cap) {
    static const char commands[] = "MmZzLlHhVvCcSsQqTtAa";
    const char *p = d;
    char command = 0;
    long count = 0;
    for (;;) {
        while (svg_space(*p)) p++;
        if (!*p) return command ? count : -1;
        int arity;
        if (strchr(commands, *p)) {
            if (!command && (*p | 32) != 'm') return -1;
            command = *p++;
            arity = svg_path_arity(command);
            if (!arity) {
                if (seg && (size_t)count < cap) seg[count].cmd = 'Z';
                count++;
                continue;
            }
        } else {
            if (!command || !(arity = svg_path_arity(command))) return -1;
            if ((command | 32) == 'm') command = command == 'M' ? 'L' : 'l';
        }
        struct svg_path_seg one = {command, {0}};
        for (int i = 0; i < arity; i++) {
            while (svg_space(*p)) p++;
            if (i && *p == ',') { p++; while (svg_space(*p)) p++; }
            if (arity == 7 && (i == 3 || i == 4)) {
                if (*p != '0' && *p != '1') return -1;
                one.v[i] = (float)(*p++ - '0');
            } else if (!svg_number(&p, &one.v[i])) return -1;
        }
        if (seg && (size_t)count < cap) seg[count] = one;
        count++;
        while (svg_space(*p)) p++;
        if (*p == ',') {
            p++;
            while (svg_space(*p)) p++;
            if (!*p || strchr(commands, *p)) return -1; /* a comma joins argument groups only */
        }
    }
}
#endif
