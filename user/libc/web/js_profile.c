/* Where a runtime's JavaScript time goes, for --debug-js (js_profile.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nocturne.h>
#include "js_profile.h"

#define REPORT_FUNCTIONS 15
#define REPORT_SCRIPTS 8

struct js_profile_function {
    const void *function; /* its bytecode; NULL for an empty slot */
    uint64_t us;
    char *name;           /* "name line" */
    char *script;
};

struct js_profile {
    struct js_profile_function *slots;
    size_t capacity, used;
    uint64_t last_us, total_us;
};

static size_t slot_of(const void *function, size_t capacity) {
    return (size_t)(((uintptr_t)function >> 4) * UINT64_C(0x9E3779B97F4A7C15) >> 32) & (capacity - 1);
}

static bool grow(struct js_profile *p) {
    size_t capacity = p->capacity ? p->capacity * 2 : 1024;
    struct js_profile_function *slots = calloc(capacity, sizeof *slots);
    if (!slots) return false;
    for (size_t i = 0; i < p->capacity; i++) {
        if (!p->slots[i].function) continue;
        size_t at = slot_of(p->slots[i].function, capacity);
        while (slots[at].function) at = (at + 1) & (capacity - 1);
        slots[at] = p->slots[i];
    }
    free(p->slots);
    p->slots = slots;
    p->capacity = capacity;
    return true;
}

static struct js_profile_function *find(struct js_profile *p, const void *function, JSContext *ctx) {
    if ((p->used + 1) * 2 > p->capacity && !grow(p)) return NULL;
    size_t at = slot_of(function, p->capacity);
    while (p->slots[at].function && p->slots[at].function != function) at = (at + 1) & (p->capacity - 1);
    struct js_profile_function *f = &p->slots[at];
    if (f->function) return f;
    char name[64], script[256], label[96];
    int line = JS_DescribeFunction(ctx, function, name, sizeof name, script, sizeof script);
    snprintf(label, sizeof label, "%s %d", *name ? name : "(anonymous)", line);
    f->name = strdup(label);
    f->script = strdup(script);
    if (!f->name || !f->script) { free(f->name); free(f->script); f->name = f->script = NULL; return NULL; }
    f->function = function;
    p->used++;
    return f;
}

void js_profile_start(struct js_profile *profile) {
    if (profile) profile->last_us = uptime_us();
}

void js_profile_sample(struct js_profile **profile, JSRuntime *rt, JSContext *ctx) {
    struct js_profile *p = *profile;
    if (!p && !(p = *profile = calloc(1, sizeof *p))) return;
    uint64_t now = uptime_us(), spent = p->last_us ? now - p->last_us : 0;
    p->last_us = now;
    const void *function = JS_GetRunningFunction(rt);
    struct js_profile_function *f = function && spent ? find(p, function, ctx) : NULL;
    if (!f) return;
    f->us += spent;
    p->total_us += spent;
}

uint64_t js_profile_ms(const struct js_profile *profile) { return profile ? profile->total_us / 1000 : 0; }

static int by_time(const void *a, const void *b) {
    uint64_t x = ((const struct js_profile_function *)a)->us, y = ((const struct js_profile_function *)b)->us;
    return x < y ? 1 : x > y ? -1 : 0;
}

void js_profile_report(struct js_profile *profile, void (*log)(void *opaque, const char *line), void *opaque) {
    struct js_profile *p = profile;
    if (!p || !p->total_us) return;
    char line[256];
    /* Sorting moves the entries out of their hash slots: the table is
       emptied below in any case. */
    size_t n = 0;
    for (size_t i = 0; i < p->capacity; i++) if (p->slots[i].function) p->slots[n++] = p->slots[i];
    qsort(p->slots, n, sizeof *p->slots, by_time);
    double total = (double)p->total_us;
    snprintf(line, sizeof line, "JavaScript profile: %lu ms in %lu functions; the functions with the most time:",
             (unsigned long)(p->total_us / 1000), (unsigned long)n);
    log(opaque, line);
    for (size_t i = 0; i < n && i < REPORT_FUNCTIONS; i++) {
        const struct js_profile_function *f = &p->slots[i];
        snprintf(line, sizeof line, "  %5.1f%% %6lu ms  %s  %.120s", 100.0 * f->us / total,
                 (unsigned long)(f->us / 1000), f->name, f->script);
        log(opaque, line);
    }
    struct { const char *script; uint64_t us; } scripts[64];
    size_t count = 0;
    uint64_t other = 0;
    for (size_t i = 0; i < n; i++) {
        size_t s = 0;
        while (s < count && strcmp(scripts[s].script, p->slots[i].script)) s++;
        if (s == count) {
            if (count == sizeof scripts / sizeof *scripts) { other += p->slots[i].us; continue; }
            scripts[count].script = p->slots[i].script;
            scripts[count++].us = 0;
        }
        scripts[s].us += p->slots[i].us;
    }
    for (size_t rank = 0; rank < REPORT_SCRIPTS && rank < count; rank++) {
        size_t best = rank;
        for (size_t s = rank + 1; s < count; s++) if (scripts[s].us > scripts[best].us) best = s;
        if (best != rank) { __typeof__(scripts[0]) t = scripts[rank]; scripts[rank] = scripts[best]; scripts[best] = t; }
        snprintf(line, sizeof line, "  script %5.1f%% %6lu ms  %.160s", 100.0 * scripts[rank].us / total,
                 (unsigned long)(scripts[rank].us / 1000), scripts[rank].script);
        log(opaque, line);
    }
    if (other) {
        snprintf(line, sizeof line, "  script %5.1f%% %6lu ms  (more scripts)", 100.0 * other / total, (unsigned long)(other / 1000));
        log(opaque, line);
    }
    for (size_t i = 0; i < n; i++) { free(p->slots[i].name); free(p->slots[i].script); }
    memset(p->slots, 0, p->capacity * sizeof *p->slots);
    p->used = 0;
    p->total_us = 0;
}

void js_profile_free(struct js_profile *profile) {
    if (!profile) return;
    for (size_t i = 0; i < profile->capacity; i++) {
        free(profile->slots[i].name);
        free(profile->slots[i].script);
    }
    free(profile->slots);
    free(profile);
}
