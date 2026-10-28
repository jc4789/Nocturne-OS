/* host.animation(op, ...): the script facade (js_animations.js) over animation.c's records.
   Animations and effects are named by their ids; a facade object holds one reference
   (WAO_WRAP, WAO_RELEASE from its FinalizationRegistry), so a record outlives neither its
   object nor, while it can still run or fire events, its own state. Included by js.c for
   wrap, unwrap and flush_layout. The op numbers are the facade's `op` table. */
#pragma once
#include "animation.h"

enum { WAO_TIMELINE, WAO_EFFECT_NEW, WAO_EFFECT_TIMING, WAO_EFFECT_TIMING_GET, WAO_EFFECT_COMPUTED,
       WAO_KEYFRAMES_SET, WAO_KEYFRAMES_GET, WAO_TARGET_SET, WAO_TARGET_GET, WAO_COMPOSITE_SET, WAO_COMPOSITE_GET,
       WAO_EFFECT_WRAP, WAO_EFFECT_RELEASE, WAO_NEW, WAO_STATE, WAO_PLAY, WAO_PAUSE, WAO_FINISH, WAO_CANCEL, WAO_REVERSE, WAO_PERSIST,
       WAO_UPDATE_RATE, WAO_SET_RATE, WAO_SET_CURRENT, WAO_SET_START, WAO_SET_EFFECT, WAO_SET_TIMELINE, WAO_NOTIFY,
       WAO_RELEASE, WAO_WRAP, WAO_LIST, WAO_EVENTS, WAO_CSS, WAO_COMMIT, WAO_EASING, WAO_FLUSH, WAO_SYNC, WAO_PROPERTY };

static double wao_number(JSContext *ctx, JSValueConst v) {
    double d;
    if (JS_IsNull(v) || JS_IsUndefined(v) || JS_ToFloat64(ctx, &d, v)) return NAN;
    return d;
}
static JSValue wao_time(JSContext *ctx, double t) { return isnan(t) ? JS_NULL : JS_NewFloat64(ctx, t); }
/* 0 the element, 1 ::before, 2 ::after, 3 another pseudo-element (no site of ours samples it) */
static uint8_t wao_pseudo(int v) { return v == 1 ? PE_BEFORE : v == 2 ? PE_AFTER : v == 3 ? PE_OTHER : PE_NONE; }
static int wao_pseudo_code(uint8_t pseudo) { return pseudo == PE_BEFORE ? 1 : pseudo == PE_AFTER ? 2 : pseudo == PE_NONE ? 0 : 3; }
static JSValue wao_list(JSContext *ctx, JSValue *items, uint32_t n) {
    JSValue a = JS_NewArray(ctx);
    for (uint32_t i = 0; i < n; i++) JS_SetPropertyUint32(ctx, a, i, items[i]);
    return a;
}
static JSValue wao_easing_text(JSContext *ctx, const struct css_easing *e) {
    sbuf b = {0};
    if (e->kind == CM_LINEAR) sb_puts(&b, "linear"); else css_easing_serialize(e, &b);
    JSValue v = JS_NewString(ctx, b.n ? sb_cstr(&b) : "linear");
    sb_free(&b);
    return v;
}
/* a CSSAnimation's API overrides (CSS Animations 2 §4.1) */
static void wao_override(struct web_effect *e, uint32_t bits) {
    if (e && e->animation && e->animation->kind == WA_CSS_ANIMATION) e->animation->overrides |= bits;
}
static bool wao_easing(JSContext *ctx, JSValueConst v, struct css_easing *out) {
    const char *text = JS_ToCString(ctx, v);
    if (!text) return false;
    size_t n = strlen(text);
    bool ok = css_easing_parse(text, n, out);
    JS_FreeCString(ctx, text);
    return ok;
}
static JSValue wao_keyframes_set(JSContext *ctx, struct web_effect *e, JSValueConst frames) {
    uint32_t count = 0;
    JSValue length = JS_GetPropertyStr(ctx, frames, "length");
    if (JS_ToUint32(ctx, &count, length)) { JS_FreeValue(ctx, length); return JS_EXCEPTION; }
    JS_FreeValue(ctx, length);
    struct web_keyframe *list = count ? calloc(count, sizeof *list) : NULL;
    if (count && !list) return JS_ThrowOutOfMemory(ctx);
    JSValue result = JS_NewInt32(ctx, 0);
    uint32_t built = 0;
    for (; built < count; built++) {
        JSValue f = JS_GetPropertyUint32(ctx, frames, built);
        uint32_t items = 0;
        JSValue fl = JS_GetPropertyStr(ctx, f, "length");
        JS_ToUint32(ctx, &items, fl); JS_FreeValue(ctx, fl);
        struct web_keyframe *k = &list[built];
        JSValue offset = JS_GetPropertyUint32(ctx, f, 0), easing = JS_GetPropertyUint32(ctx, f, 1),
                composite = JS_GetPropertyUint32(ctx, f, 2);
        k->offset = wao_number(ctx, offset);
        int32_t c = WA_COMPOSITE_AUTO; JS_ToInt32(ctx, &c, composite);
        k->composite = (uint8_t)(c >= 0 && c <= WA_COMPOSITE_AUTO ? c : WA_COMPOSITE_AUTO);
        bool easing_ok = wao_easing(ctx, easing, &k->easing);
        JS_FreeValue(ctx, offset); JS_FreeValue(ctx, easing); JS_FreeValue(ctx, composite);
        if (!easing_ok) { JS_FreeValue(ctx, f); result = JS_NewInt32(ctx, -1); built++; break; }
        uint32_t pairs = items > 3 ? (items - 3) / 2 : 0;
        k->values = pairs ? calloc(pairs, sizeof *k->values) : NULL;
        for (uint32_t j = 0; j < pairs; j++) {
            JSValue name = JS_GetPropertyUint32(ctx, f, 3 + 2 * j), value = JS_GetPropertyUint32(ctx, f, 4 + 2 * j);
            const char *n = JS_ToCString(ctx, name), *v = JS_ToCString(ctx, value);
            const struct propdef *p = n ? css_prop_lookup(n, strlen(n)) : NULL;
            /* Web Animations 1 "processing a keyframes argument": a value that does not parse is dropped */
            if (p && v && k->values && css_value_valid(p, v, strlen(v)))
                k->values[k->count++] = (struct web_keyframe_value){.p = p, .text = strdup(v)};
            if (n) JS_FreeCString(ctx, n);
            if (v) JS_FreeCString(ctx, v);
            JS_FreeValue(ctx, name); JS_FreeValue(ctx, value);
        }
        JS_FreeValue(ctx, f);
    }
    int32_t status = 0; JS_ToInt32(ctx, &status, result);
    if (!status) {
        struct web_keyframes *k = web_keyframes_build(list, count);
        if (!k) result = JS_ThrowOutOfMemory(ctx);
        else { web_effect_set_keyframes(e, k); wao_override(e, WA_OVERRIDE_KEYFRAMES); }
    }
    for (uint32_t i = 0; i < built && i < count; i++) {
        for (uint32_t j = 0; j < list[i].count; j++) free(list[i].values[j].text);
        free(list[i].values);
    }
    free(list);
    return result;
}
static JSValue wao_keyframes_get(JSContext *ctx, const struct web_effect *e) {
    JSValue out = JS_NewArray(ctx);
    const struct web_keyframes *k = e->frames;
    for (uint32_t i = 0; k && i < k->count; i++) {
        const struct web_keyframe *f = &k->v[i];
        JSValue frame = JS_NewArray(ctx);
        uint32_t at = 0;
        JS_SetPropertyUint32(ctx, frame, at++, wao_time(ctx, f->offset));
        JS_SetPropertyUint32(ctx, frame, at++, JS_NewFloat64(ctx, f->computed));
        JS_SetPropertyUint32(ctx, frame, at++, wao_easing_text(ctx, &f->easing));
        JS_SetPropertyUint32(ctx, frame, at++, JS_NewInt32(ctx, f->composite));
        for (uint32_t j = 0; j < f->count; j++) {
            JS_SetPropertyUint32(ctx, frame, at++, JS_NewString(ctx, css_prop_name(css_prop_index(f->values[j].p), NULL)));
            JS_SetPropertyUint32(ctx, frame, at++, JS_NewString(ctx, f->values[j].text));
        }
        JS_SetPropertyUint32(ctx, out, i, frame);
    }
    return out;
}
static bool wao_alive(const struct web_animation *a) {
    int state = web_animation_play_state(a);
    return a->pending || state == WA_RUNNING || state == WA_PAUSED || a->finish_task || a->outbox || web_animation_relevant(a);
}
static JSValue native_animation(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    struct web_js_state *s = state(ctx);
    web_doc *d = s->doc;
    int32_t op = -1;
    if (argc < 1 || JS_ToInt32(ctx, &op, argv[0])) return JS_EXCEPTION;
    if (!web_animations_of(d)) return JS_ThrowOutOfMemory(ctx);
    uint32_t id = 0;
    if (argc > 1 && JS_IsNumber(argv[1])) JS_ToUint32(ctx, &id, argv[1]);
#define ARG(i) (argc > (i) ? argv[i] : JS_UNDEFINED)
    switch (op) {
    case WAO_TIMELINE: return wao_time(ctx, d->animations->now);
    case WAO_FLUSH: flush_layout(s); return JS_UNDEFINED;
    case WAO_PROPERTY: {
        /* the CSS name of an animatable property (a shorthand with an animatable longhand), else null */
        const char *name = JS_ToCString(ctx, ARG(1));
        if (!name) return JS_EXCEPTION;
        const struct propdef *p = css_prop_lookup(name, strlen(name));
        JS_FreeCString(ctx, name);
        if (!p || css_prop_is_motion(p)) return JS_NULL;
        int n = css_prop_count();
        uint8_t set[n]; css_prop_longhands(p, set);
        for (int i = 0; i < n; i++)
            if (set[i] && css_prop_animation(css_prop_at(i)) != CSS_ANIM_NONE) return JS_NewString(ctx, css_prop_name(css_prop_index(p), NULL));
        return JS_NULL;
    }
    case WAO_SYNC: {
        /* the promise state of every animation a facade object holds; nothing else can observe a
           finish notification's timing, so the others' run here */
        struct web_animations *x = d->animations;
        web_animations_join(d);
        JSValue out = JS_NewArray(ctx);
        uint32_t at = 0;
        for (struct web_animation *a = x->first; a; a = a->next) {
            if (!a->wrapped) { if (a->finish_task) web_animation_finish_notify(a); continue; }
            JSValue items[9] = {JS_NewUint32(ctx, a->id), JS_NewUint32(ctx, a->ready_gen), JS_NewInt32(ctx, a->ready_state),
                JS_NewInt32(ctx, a->ready_prev), JS_NewUint32(ctx, a->finished_gen), JS_NewInt32(ctx, a->finished_state),
                JS_NewInt32(ctx, a->finished_prev), JS_NewBool(ctx, a->finish_task), JS_NewBool(ctx, wao_alive(a))};
            JS_SetPropertyUint32(ctx, out, at++, wao_list(ctx, items, 9));
        }
        web_animations_collect(d);
        /* what is left is in these rows or the event queue */
        __atomic_store_n(&x->script_dirty, false, __ATOMIC_RELAXED);
        return out;
    }
    case WAO_EASING: {
        struct css_easing e;
        if (!wao_easing(ctx, ARG(1), &e)) return JS_NULL;
        return wao_easing_text(ctx, &e);
    }
    case WAO_EFFECT_NEW: {
        struct web_effect *e = web_effect_new(d);
        if (!e) return JS_ThrowOutOfMemory(ctx);
        e->wrapped = 1;
        web_animations_join(d);
        int32_t pseudo = 0; JS_ToInt32(ctx, &pseudo, ARG(2));
        web_effect_set_target(e, unwrap(ctx, ARG(1)), wao_pseudo(pseudo));
        return JS_NewUint32(ctx, e->id);
    }
    case WAO_LIST: {
        flush_layout(s);
        struct web_animation **v = NULL;
        int32_t pseudo = -1; JS_ToInt32(ctx, &pseudo, ARG(3));
        size_t n = web_animations_list(d, unwrap(ctx, ARG(1)), JS_ToBool(ctx, ARG(2)), pseudo < 0 ? -1 : wao_pseudo(pseudo), &v);
        JSValue out = JS_NewArray(ctx);
        for (size_t i = 0; i < n; i++) JS_SetPropertyUint32(ctx, out, (uint32_t)i, JS_NewUint32(ctx, v[i]->id));
        free(v);
        return out;
    }
    case WAO_EVENTS: {
        struct web_animation_event *v = NULL;
        size_t n = web_animations_take_events(d, &v);
        JSValue out = JS_NewArray(ctx);
        for (size_t i = 0; i < n; i++) {
            JSValue items[8] = {JS_NewUint32(ctx, v[i].animation), v[i].target ? wrap(s, v[i].target) : JS_NULL,
                JS_NewInt32(ctx, wao_pseudo_code(v[i].pseudo)), JS_NewString(ctx, v[i].type), wao_time(ctx, v[i].current),
                wao_time(ctx, v[i].timeline), JS_NewFloat64(ctx, v[i].elapsed), v[i].name ? JS_NewString(ctx, v[i].name) : JS_NULL};
            JS_SetPropertyUint32(ctx, out, (uint32_t)i, wao_list(ctx, items, 8));
            free(v[i].name);
        }
        free(v);
        return out;
    }
    case WAO_NEW: {
        struct web_effect *e = web_effect_by_id(d, id);
        struct web_animation *a = web_animation_new(d, WA_SCRIPT);
        if (!a) return JS_ThrowOutOfMemory(ctx);
        a->wrapped = 1;
        web_animations_join(d);
        double origin = wao_number(ctx, ARG(3));
        if (!JS_ToBool(ctx, ARG(2))) a->timeline = false;
        else a->timeline_origin = isnan(origin) ? 0 : origin;
        if (e) web_animation_set_effect(a, e);
        return JS_NewUint32(ctx, a->id);
    }
    }
    if (op <= WAO_EFFECT_RELEASE) {
        struct web_effect *e = web_effect_by_id(d, id);
        if (!e) return JS_ThrowTypeError(ctx, "Illegal AnimationEffect receiver");
        switch (op) {
        case WAO_EFFECT_TIMING: {
            int32_t mask = 0; JS_ToInt32(ctx, &mask, ARG(2));
            struct web_timing t = e->timing;
            if ((mask & 128) && !wao_easing(ctx, ARG(10), &t.easing)) return JS_NewInt32(ctx, -1);
            int32_t k = 0;
            if (mask & 1) t.delay = wao_number(ctx, ARG(3));
            if (mask & 2) t.end_delay = wao_number(ctx, ARG(4));
            if (mask & 4) { JS_ToInt32(ctx, &k, ARG(5)); t.fill = (uint8_t)k; }
            if (mask & 8) t.iteration_start = wao_number(ctx, ARG(6));
            if (mask & 16) t.iterations = wao_number(ctx, ARG(7));
            if (mask & 32) t.duration = wao_number(ctx, ARG(8));
            if (mask & 64) { JS_ToInt32(ctx, &k, ARG(9)); t.direction = (uint8_t)k; }
            e->timing = t;
            wao_override(e, (mask & 1 ? WA_OVERRIDE_DELAY : 0) | (mask & 2 ? WA_OVERRIDE_END_DELAY : 0) |
                (mask & 4 ? WA_OVERRIDE_FILL : 0) | (mask & 8 ? WA_OVERRIDE_ITERATION_START : 0) |
                (mask & 16 ? WA_OVERRIDE_ITERATIONS : 0) | (mask & 32 ? WA_OVERRIDE_DURATION : 0) |
                (mask & 64 ? WA_OVERRIDE_DIRECTION : 0) | (mask & 128 ? WA_OVERRIDE_EASING : 0));
            web_effect_timing_changed(e);
            web_animations_collect(d);
            return JS_NewInt32(ctx, 0);
        }
        case WAO_EFFECT_TIMING_GET: {
            if (e->animation && e->animation->kind != WA_SCRIPT) flush_layout(s);
            const struct web_timing *t = &e->timing;
            JSValue items[8] = {JS_NewFloat64(ctx, t->delay), JS_NewFloat64(ctx, t->end_delay), JS_NewInt32(ctx, t->fill),
                JS_NewFloat64(ctx, t->iteration_start), JS_NewFloat64(ctx, t->iterations),
                isnan(t->duration) ? JS_NewString(ctx, "auto") : JS_NewFloat64(ctx, t->duration),
                JS_NewInt32(ctx, t->direction), wao_easing_text(ctx, &t->easing)};
            return wao_list(ctx, items, 8);
        }
        case WAO_EFFECT_COMPUTED: {
            if (e->animation && e->animation->kind != WA_SCRIPT) flush_layout(s);
            struct web_effect_progress p; web_effect_compute(e, &p);
            JSValue items[5] = {JS_NewFloat64(ctx, web_effect_end(&e->timing)),
                JS_NewFloat64(ctx, web_effect_active_duration(&e->timing)), wao_time(ctx, p.local),
                wao_time(ctx, p.progress), wao_time(ctx, p.iteration)};
            return wao_list(ctx, items, 5);
        }
        case WAO_KEYFRAMES_SET: return wao_keyframes_set(ctx, e, ARG(2));
        case WAO_KEYFRAMES_GET:
            if (e->animation && e->animation->kind != WA_SCRIPT) flush_layout(s);
            return wao_keyframes_get(ctx, e);
        case WAO_TARGET_SET: {
            int32_t pseudo = 0; JS_ToInt32(ctx, &pseudo, ARG(3));
            web_effect_set_target(e, unwrap(ctx, ARG(2)), wao_pseudo(pseudo));
            if (e->animation) web_effect_timing_changed(e);
            return JS_UNDEFINED;
        }
        case WAO_TARGET_GET: {
            JSValue items[2] = {e->target ? wrap(s, e->target) : JS_NULL, JS_NewInt32(ctx, wao_pseudo_code(e->pseudo))};
            return wao_list(ctx, items, 2);
        }
        case WAO_COMPOSITE_SET: {
            int32_t c = -1, ic = -1; JS_ToInt32(ctx, &c, ARG(2)); JS_ToInt32(ctx, &ic, ARG(3));
            if (c >= 0) { e->composite = (uint8_t)c; wao_override(e, WA_OVERRIDE_COMPOSITE); }
            if (ic >= 0) e->iteration_composite = (uint8_t)ic;
            web_effect_timing_changed(e);
            return JS_UNDEFINED;
        }
        case WAO_COMPOSITE_GET: {
            if (e->animation && e->animation->kind != WA_SCRIPT) flush_layout(s);
            JSValue items[2] = {JS_NewInt32(ctx, e->composite), JS_NewInt32(ctx, e->iteration_composite)};
            return wao_list(ctx, items, 2);
        }
        case WAO_EFFECT_WRAP: e->wrapped++; return JS_UNDEFINED;
        case WAO_EFFECT_RELEASE: web_effect_release(e); return JS_UNDEFINED;
        }
        return JS_UNDEFINED;
    }
    struct web_animation *a = web_animation_by_id(d, id);
    if (!a) return JS_ThrowTypeError(ctx, "Illegal Animation receiver");
    int status = 0;
    switch (op) {
    case WAO_STATE: {
        if (a->kind != WA_SCRIPT && JS_ToBool(ctx, ARG(2))) flush_layout(s);
        JSValue items[17] = {JS_NewInt32(ctx, web_animation_play_state(a)), JS_NewBool(ctx, a->pending != 0),
            wao_time(ctx, web_animation_current(a)), wao_time(ctx, a->start), JS_NewFloat64(ctx, a->rate),
            JS_NewInt32(ctx, a->replace), JS_NewUint32(ctx, a->ready_gen), JS_NewInt32(ctx, a->ready_state),
            JS_NewInt32(ctx, a->ready_prev), JS_NewUint32(ctx, a->finished_gen), JS_NewInt32(ctx, a->finished_state),
            JS_NewInt32(ctx, a->finished_prev), JS_NewBool(ctx, a->finish_task), JS_NewUint32(ctx, a->effect ? a->effect->id : 0),
            JS_NewInt32(ctx, a->kind), JS_NewBool(ctx, a->timeline), JS_NewBool(ctx, wao_alive(a))};
        return wao_list(ctx, items, 17);
    }
    case WAO_CSS: {
        JSValue items[2] = {JS_NewInt32(ctx, a->kind), a->name ? JS_NewString(ctx, a->name) : JS_NULL};
        return wao_list(ctx, items, 2);
    }
    case WAO_COMMIT: {
        flush_layout(s);
        /* HTML "being rendered": an element with a box */
        if (!a->effect || !a->effect->target || !a->effect->target->box) return JS_NULL;
        sbuf b = {0}; int count = 0;
        if (!web_animation_commit(a, &b, &count)) { sb_free(&b); return JS_NULL; }
        JSValue out = JS_NewArray(ctx);
        const char *p = b.p;
        for (int i = 0; i < 2 * count && p; i++) { JS_SetPropertyUint32(ctx, out, (uint32_t)i, JS_NewString(ctx, p)); p += strlen(p) + 1; }
        sb_free(&b);
        return out;
    }
    case WAO_PLAY:
        if (a->kind == WA_CSS_ANIMATION) { flush_layout(s); a->overrides |= WA_OVERRIDE_PLAY; }
        status = web_animation_play(a, true); break;
    case WAO_PAUSE:
        if (a->kind == WA_CSS_ANIMATION) { flush_layout(s); a->overrides |= WA_OVERRIDE_PLAY; }
        status = web_animation_pause(a); break;
    case WAO_FINISH: status = web_animation_finish(a); break;
    case WAO_CANCEL: web_animation_cancel(a); break;
    case WAO_REVERSE: status = web_animation_reverse(a); break;
    case WAO_PERSIST: web_animation_persist(a); break;
    case WAO_UPDATE_RATE: web_animation_update_rate(a, wao_number(ctx, ARG(2))); break;
    case WAO_SET_RATE: web_animation_set_rate(a, wao_number(ctx, ARG(2))); break;
    case WAO_SET_CURRENT: status = web_animation_set_current(a, wao_number(ctx, ARG(2))); break;
    case WAO_SET_START: web_animation_set_start(a, wao_number(ctx, ARG(2))); break;
    case WAO_SET_EFFECT: {
        uint32_t effect = 0; JS_ToUint32(ctx, &effect, ARG(2));
        web_animation_set_effect(a, effect ? web_effect_by_id(d, effect) : NULL);
    } break;
    case WAO_SET_TIMELINE: {
        double origin = wao_number(ctx, ARG(3));
        web_animation_set_timeline(a, JS_ToBool(ctx, ARG(2)), isnan(origin) ? 0 : origin);
    } break;
    case WAO_NOTIFY: web_animation_finish_notify(a); break;
    case WAO_RELEASE: web_animation_release(a); return JS_UNDEFINED;
    case WAO_WRAP: a->wrapped++; return JS_UNDEFINED;
    default: return JS_ThrowRangeError(ctx, "Unknown animation operation");
    }
#undef ARG
    web_animations_collect(d);
    return JS_NewInt32(ctx, status);
}
