/* The one time model (animation.h): Web Animations 1 §4 (timing model) and §5 (animation model),
   with CSS Animations 2 and CSS Transitions 2 as the procedures that create and update the
   declarative animations from computed styles. The cascade's workers create and update the
   animations an element owns (each worker only those of its own element) and push new records
   onto lock-free stacks; the main thread joins them, runs the pending tasks, updates the
   finished states, queues the events and frees what nothing holds. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <nocturne.h>
#include "animation.h"

static bool resolved(double t) { return !isnan(t); }
enum { OUT_FINISH = 1, OUT_CANCEL = 2, OUT_REMOVE = 4 };

/* ---------------------------------------------------------------- records */
struct web_animations *web_animations_of(web_doc *d) {
    if (!d) return NULL;
    if (!d->animations) {
        struct web_animations *x = calloc(1, sizeof *x);
        if (!x) return NULL;
        x->doc = d; x->now = NAN;
        d->animations = x;
    }
    return d->animations;
}
static void script_dirty(const struct web_animation *a) {
    if (a->doc) __atomic_store_n(&a->doc->script_dirty, true, __ATOMIC_RELAXED);
}
static void keyframes_free(struct web_keyframes *k) {
    if (!k) return;
    for (uint32_t i = 0; i < k->count; i++) {
        for (uint32_t j = 0; j < k->v[i].count; j++) { free(k->v[i].values[j].text); free(k->v[i].values[j].longhands); }
        free(k->v[i].values);
    }
    free(k->v); free(k->properties); free(k);
}
/* Keyframes another worker may be sampling retire to the join, never freed in place. */
static void keyframes_retire(struct web_animations *x, struct web_keyframes *k) {
    if (!k) return;
    if (!x) { keyframes_free(k); return; }
    struct web_keyframes *head;
    do { head = __atomic_load_n(&x->garbage, __ATOMIC_RELAXED); k->garbage_next = head; }
    while (!__atomic_compare_exchange_n(&x->garbage, &head, k, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
}
/* Web Animations 1 §6.6.1 "compute missing keyframe offsets", with each value's longhands */
struct web_keyframes *web_keyframes_build(const struct web_keyframe *frames, uint32_t count) {
    struct web_keyframes *k = calloc(1, sizeof *k);
    int nprops = css_prop_count();
    if (!k) return NULL;
    k->properties = calloc((size_t)nprops, 1);
    k->v = count ? calloc(count, sizeof *k->v) : NULL;
    if (!k->properties || (count && !k->v)) { keyframes_free(k); return NULL; }
    k->count = count;
    uint8_t set[nprops];
    for (uint32_t i = 0; i < count; i++) {
        struct web_keyframe *f = &k->v[i];
        *f = frames[i]; f->values = NULL; f->count = 0;
        if (frames[i].count) {
            f->values = calloc(frames[i].count, sizeof *f->values);
            if (!f->values) { keyframes_free(k); return NULL; }
        }
        for (uint32_t j = 0; j < frames[i].count; j++) {
            const struct web_keyframe_value *v = &frames[i].values[j];
            if (!v->p || !v->text) continue;
            css_prop_longhands(v->p, set);
            uint16_t n = 0;
            for (int p = 0; p < nprops; p++) if (set[p] && css_prop_animation(css_prop_at(p)) != CSS_ANIM_NONE) n++;
            if (!n) continue;
            struct web_keyframe_value *out = &f->values[f->count];
            out->p = v->p; out->text = strdup(v->text); out->longhands = malloc(n * sizeof *out->longhands);
            if (!out->text || !out->longhands) { free(out->text); free(out->longhands); keyframes_free(k); return NULL; }
            out->nlonghands = 0;
            for (int p = 0; p < nprops; p++)
                if (set[p] && css_prop_animation(css_prop_at(p)) != CSS_ANIM_NONE) { out->longhands[out->nlonghands++] = (uint16_t)p; k->properties[p] = 1; }
            f->count++;
        }
    }
    /* computed offsets: a lone null keyframe is at 1, the ends at 0 and 1, the rest spaced evenly */
    for (uint32_t i = 0; i < count; i++) k->v[i].computed = k->v[i].offset;
    if (count == 1 && !resolved(k->v[0].computed)) k->v[0].computed = 1;
    else if (count > 1) {
        if (!resolved(k->v[0].computed)) k->v[0].computed = 0;
        if (!resolved(k->v[count - 1].computed)) k->v[count - 1].computed = 1;
    }
    for (uint32_t left = 0; left + 1 < count;) {
        uint32_t right = left + 1;
        while (right < count && !resolved(k->v[right].computed)) right++;
        if (right >= count) break;
        for (uint32_t i = left + 1; i < right; i++)
            k->v[i].computed = k->v[left].computed + (k->v[right].computed - k->v[left].computed) * (double)(i - left) / (double)(right - left);
        left = right;
    }
    return k;
}
static bool keyframes_same(const struct web_keyframes *k, const struct web_keyframe *frames, uint32_t count) {
    if (!k || k->count != count) return false;
    for (uint32_t i = 0; i < count; i++) {
        const struct web_keyframe *a = &k->v[i], *b = &frames[i];
        if (!(a->offset == b->offset || (!resolved(a->offset) && !resolved(b->offset))) ||
            a->composite != b->composite || !css_easing_same(&a->easing, &b->easing)) return false;
        uint32_t j = 0;
        for (uint32_t n = 0; n < b->count; n++) {
            if (!b->values[n].p || !b->values[n].text) continue;
            if (j >= a->count) return false;
            if (a->values[j].p != b->values[n].p || strcmp(a->values[j].text, b->values[n].text)) {
                /* a value that animates nothing was dropped when it was built */
                uint8_t set[css_prop_count()]; css_prop_longhands(b->values[n].p, set); bool any = false;
                for (int p = 0; p < css_prop_count(); p++) if (set[p] && css_prop_animation(css_prop_at(p)) != CSS_ANIM_NONE) any = true;
                if (any) return false;
                continue;
            }
            j++;
        }
        if (j != a->count) return false;
    }
    return true;
}

static uint32_t next_id(struct web_animations *x) { return __atomic_add_fetch(&x->next_id, 1, __ATOMIC_RELAXED); }
struct web_effect *web_effect_new(web_doc *d) {
    struct web_animations *x = d ? d->animations : NULL;
    if (!x) return NULL;
    struct web_effect *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->id = next_id(x); e->doc = x;
    e->timing = (struct web_timing){.iterations = 1, .duration = NAN, .fill = WA_FILL_AUTO, .direction = CM_NORMAL,
                                    .easing = {.kind = CM_LINEAR}};
    e->composite = CM_COMPOSITE_REPLACE; e->iteration_composite = CM_COMPOSITE_REPLACE;
    struct web_effect *head;
    do { head = __atomic_load_n(&x->added_effects, __ATOMIC_RELAXED); e->next = head; }
    while (!__atomic_compare_exchange_n(&x->added_effects, &head, e, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    return e;
}
struct web_animation *web_animation_new(web_doc *d, uint8_t kind) {
    struct web_animations *x = d ? d->animations : NULL;
    if (!x) return NULL;
    struct web_animation *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->id = next_id(x); a->doc = x; a->kind = kind; a->timeline = true;
    a->start = a->hold = a->pending_rate = a->previous_current = a->event_iteration = NAN;
    a->cancel_elapsed = NAN;
    a->rate = 1; a->shortening = 1;
    a->ready_state = WA_PROMISE_RESOLVED; a->finished_state = WA_PROMISE_PENDING;
    a->event_phase = WA_PHASE_IDLE;
    a->sequence = __atomic_add_fetch(&x->sequence, 1, __ATOMIC_RELAXED);
    struct web_animation *head;
    do { head = __atomic_load_n(&x->added, __ATOMIC_RELAXED); a->next = head; }
    while (!__atomic_compare_exchange_n(&x->added, &head, a, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    return a;
}
static void unlink_target(struct web_effect *e) {
    if (!e->target) return;
    for (struct web_effect **p = &e->target->effects; *p; p = &(*p)->target_next)
        if (*p == e) { *p = e->target_next; break; }
    e->target = NULL; e->target_next = NULL;
}
static void mark_target(struct web_effect *e) {
    /* inside the cascade the style being computed already reads the new state */
    if (e && e->target && e->doc && e->doc->doc && !e->doc->cascading) css_mark_style_own(e->doc->doc, e->target);
}
void web_effect_set_target(struct web_effect *e, node_t *target, uint8_t pseudo) {
    if (!e || (e->target == target && e->pseudo == pseudo)) return;
    mark_target(e);
    unlink_target(e);
    if (target && target->type == N_ELEM) {
        e->target = target; e->pseudo = pseudo;
        e->target_next = target->effects; target->effects = e;
        mark_target(e);
    }
}
void web_effect_set_keyframes(struct web_effect *e, struct web_keyframes *k) {
    struct web_keyframes *old = e->frames;
    e->frames = k;
    keyframes_retire(e->doc, old);
    mark_target(e);
}
static void unlink_owner(struct web_animation *a) {
    if (!a->owner) return;
    for (struct web_animation **p = &a->owner->owned; *p; p = &(*p)->owner_next)
        if (*p == a) { *p = a->owner_next; break; }
    a->owner = NULL; a->owner_next = NULL;
}
static void effect_free(struct web_effect *e) {
    unlink_target(e);
    keyframes_retire(e->doc, e->frames);
    free(e);
}
static void animation_free(struct web_animation *a) {
    unlink_owner(a);
    if (a->effect) {
        struct web_effect *e = a->effect; e->animation = NULL; a->effect = NULL;
        if (!e->wrapped) {
            /* unlist it from the document's effects, then free it */
            struct web_animations *x = e->doc;
            for (struct web_effect **p = &x->effects; *p; p = &(*p)->next) if (*p == e) { *p = e->next; break; }
            effect_free(e);
        }
    }
    free(a->name); free(a->to); free(a->reversing_from); free(a);
}

void web_animations_join(web_doc *d) {
    struct web_animations *x = d ? d->animations : NULL;
    if (!x) return;
    struct web_animation *a = __atomic_exchange_n(&x->added, NULL, __ATOMIC_ACQUIRE), *rev = NULL;
    while (a) { struct web_animation *n = a->next; a->next = rev; rev = a; a = n; }
    while (rev) {
        struct web_animation *n = rev->next; rev->next = NULL;
        if (x->last) x->last->next = rev; else x->first = rev;
        x->last = rev; rev = n;
    }
    struct web_effect *e = __atomic_exchange_n(&x->added_effects, NULL, __ATOMIC_ACQUIRE);
    while (e) { struct web_effect *n = e->next; e->next = x->effects; x->effects = e; e = n; }
    struct web_keyframes *k = __atomic_exchange_n(&x->garbage, NULL, __ATOMIC_ACQUIRE);
    while (k) { struct web_keyframes *n = k->garbage_next; keyframes_free(k); k = n; }
}
struct web_animation *web_animation_by_id(web_doc *d, uint32_t id) {
    struct web_animations *x = d ? d->animations : NULL;
    if (!x || !id) return NULL;
    if (__atomic_load_n(&x->added, __ATOMIC_ACQUIRE)) web_animations_join(d);
    for (struct web_animation *a = x->first; a; a = a->next) if (a->id == id) return a;
    return NULL;
}
struct web_effect *web_effect_by_id(web_doc *d, uint32_t id) {
    struct web_animations *x = d ? d->animations : NULL;
    if (!x || !id) return NULL;
    if (__atomic_load_n(&x->added_effects, __ATOMIC_ACQUIRE)) web_animations_join(d);
    for (struct web_effect *e = x->effects; e; e = e->next) if (e->id == id) return e;
    return NULL;
}

/* ---------------------------------------------------------------- timing model (§4.6–4.9) */
static double iteration_duration(const struct web_timing *t) { return isnan(t->duration) ? 0 : t->duration; }
double web_effect_active_duration(const struct web_timing *t) {
    double d = iteration_duration(t);
    return d == 0 || t->iterations == 0 ? 0 : d * t->iterations;
}
double web_effect_end(const struct web_timing *t) { return fmax(t->delay + web_effect_active_duration(t) + t->end_delay, 0); }
static double timeline_time(const struct web_animation *a) { return a->timeline && a->doc ? a->doc->now - a->timeline_origin : NAN; }
static double effect_end(const struct web_animation *a) { return a->effect ? web_effect_end(&a->effect->timing) : 0; }
static double effective_rate(const struct web_animation *a) { return resolved(a->pending_rate) ? a->pending_rate : a->rate; }
static double current_without_hold(const struct web_animation *a) {
    double t = timeline_time(a);
    return resolved(t) && resolved(a->start) ? (t - a->start) * a->rate : NAN;
}
double web_animation_current(const struct web_animation *a) {
    return resolved(a->hold) ? a->hold : current_without_hold(a);
}
int web_animation_play_state(const struct web_animation *a) {
    double current = web_animation_current(a);
    if (!resolved(current) && !resolved(a->start) && !a->pending) return WA_IDLE;
    if (a->pending == WA_PENDING_PAUSE || (!resolved(a->start) && a->pending != WA_PENDING_PLAY)) return WA_PAUSED;
    double rate = effective_rate(a);
    if (resolved(current) && ((rate > 0 && current >= effect_end(a)) || (rate < 0 && current <= 0))) return WA_FINISHED;
    return WA_RUNNING;
}
/* §4.8: phase, active time, overall and simple iteration progress, current iteration,
   directed and transformed progress. fill: the fill mode to use (auto is none). */
static void effect_compute_fill(const struct web_effect *e, int fill, struct web_effect_progress *o) {
    const struct web_timing *t = &e->timing;
    const struct web_animation *a = e->animation;
    o->local = a ? web_animation_current(a) : NAN;
    o->active = o->progress = o->iteration = NAN; o->phase = WA_PHASE_IDLE;
    if (!resolved(o->local)) return;
    double ad = web_effect_active_duration(t), end = web_effect_end(t), id = iteration_duration(t);
    double before_active = fmax(fmin(t->delay, end), 0), active_after = fmax(fmin(t->delay + ad, end), 0);
    bool backwards = a && a->rate < 0;
    if (o->local < before_active || (backwards && o->local == before_active)) o->phase = WA_PHASE_BEFORE;
    else if (o->local > active_after || (!backwards && o->local == active_after)) o->phase = WA_PHASE_AFTER;
    else o->phase = WA_PHASE_ACTIVE;
    if (fill == WA_FILL_AUTO) fill = CM_FILL_NONE;
    if (o->phase == WA_PHASE_BEFORE) {
        if (fill == CM_FILL_BACKWARDS || fill == CM_FILL_BOTH) o->active = fmax(o->local - t->delay, 0);
    } else if (o->phase == WA_PHASE_ACTIVE) o->active = o->local - t->delay;
    else if (fill == CM_FILL_FORWARDS || fill == CM_FILL_BOTH) o->active = fmax(fmin(o->local - t->delay, ad), 0);
    if (!resolved(o->active)) return;
    double overall = id == 0 ? (o->phase == WA_PHASE_BEFORE ? 0 : t->iterations) : o->active / id;
    overall += t->iteration_start;
    double simple = isinf(overall) ? fmod(t->iteration_start, 1.0) : fmod(overall, 1.0);
    if (simple == 0 && (o->phase == WA_PHASE_ACTIVE || o->phase == WA_PHASE_AFTER) && o->active == ad && t->iterations != 0)
        simple = 1;
    if (o->phase == WA_PHASE_AFTER && isinf(t->iterations)) o->iteration = INFINITY;
    else o->iteration = simple == 1 ? floor(overall) - 1 : floor(overall);
    bool forwards = true;
    if (t->direction == CM_REVERSE) forwards = false;
    else if (t->direction == CM_ALTERNATE || t->direction == CM_ALTERNATE_REVERSE) {
        double d = o->iteration + (t->direction == CM_ALTERNATE_REVERSE ? 1 : 0);
        forwards = isinf(d) || fmod(d, 2) == 0;
    }
    double directed = forwards ? simple : 1 - simple;
    bool before = (o->phase == WA_PHASE_BEFORE && forwards) || (o->phase == WA_PHASE_AFTER && !forwards);
    o->progress = css_easing_apply(&t->easing, directed, before);
}
void web_effect_compute(const struct web_effect *e, struct web_effect_progress *o) { effect_compute_fill(e, e->timing.fill, o); }
bool web_animation_relevant(const struct web_animation *a) {
    if (!a->effect || a->replace == WA_REMOVED) return false;
    struct web_effect_progress p;
    web_effect_compute(a->effect, &p);
    if (p.phase == WA_PHASE_IDLE) return false;
    int state = web_animation_play_state(a);
    if (state == WA_IDLE) return false;
    return resolved(p.active) || state != WA_FINISHED || (p.phase == WA_PHASE_BEFORE && a->rate > 0) ||
        (p.phase == WA_PHASE_AFTER && a->rate < 0);
}

/* ---------------------------------------------------------------- promises and events */
static void ready_new(struct web_animation *a, uint8_t state) {
    a->ready_prev = a->ready_state; a->ready_gen++; a->ready_state = state; script_dirty(a);
}
static void ready_resolve(struct web_animation *a) {
    if (a->ready_state == WA_PROMISE_PENDING) { a->ready_state = WA_PROMISE_RESOLVED; script_dirty(a); }
}
static void finished_new(struct web_animation *a) {
    a->finished_prev = a->finished_state; a->finished_gen++; a->finished_state = WA_PROMISE_PENDING; script_dirty(a);
}
static void outbox(struct web_animation *a, int kind, double current, double timeline) {
    int i = kind == OUT_FINISH ? 0 : kind == OUT_CANCEL ? 1 : 2;
    a->outbox |= (uint8_t)kind; a->out_time[i][0] = current; a->out_time[i][1] = timeline; script_dirty(a);
}
/* §4.4.17 the finish notification steps */
void web_animation_finish_notify(struct web_animation *a) {
    a->finish_task = false;
    if (web_animation_play_state(a) != WA_FINISHED) return;
    if (a->finished_state == WA_PROMISE_PENDING) { a->finished_state = WA_PROMISE_RESOLVED; script_dirty(a); }
    outbox(a, OUT_FINISH, web_animation_current(a), timeline_time(a));
}
/* §4.4.16 updating the finished state */
static void update_finished(struct web_animation *a, bool did_seek, bool sync) {
    double unconstrained = did_seek ? web_animation_current(a) : current_without_hold(a), t = timeline_time(a);
    if (resolved(unconstrained) && resolved(a->start) && !a->pending) {
        double end = effect_end(a);
        if (a->rate > 0 && unconstrained >= end)
            a->hold = did_seek ? unconstrained : resolved(a->previous_current) ? fmax(a->previous_current, end) : end;
        else if (a->rate < 0 && unconstrained <= 0)
            a->hold = did_seek ? unconstrained : resolved(a->previous_current) ? fmin(a->previous_current, 0) : 0;
        else if (a->rate != 0 && resolved(t)) {
            if (did_seek && resolved(a->hold)) a->start = t - a->hold / a->rate;
            a->hold = NAN;
        }
    }
    a->previous_current = web_animation_current(a);
    bool finished = web_animation_play_state(a) == WA_FINISHED;
    if (finished && a->finished_state == WA_PROMISE_PENDING) {
        if (sync) web_animation_finish_notify(a);
        else if (!a->finish_task) { a->finish_task = true; script_dirty(a); }
    }
    if (!finished && a->finished_state == WA_PROMISE_RESOLVED) finished_new(a);
}
static void apply_pending_rate(struct web_animation *a) {
    if (resolved(a->pending_rate)) { a->rate = a->pending_rate; a->pending_rate = NAN; }
}
/* §4.4.4 reset an animation's pending tasks */
static void reset_pending(struct web_animation *a) {
    if (!a->pending) return;
    a->pending = WA_PENDING_NONE;
    apply_pending_rate(a);
    if (a->ready_state == WA_PROMISE_PENDING) a->ready_state = WA_PROMISE_REJECTED;
    ready_new(a, WA_PROMISE_RESOLVED);
}
static void run_pending_play(struct web_animation *a) {
    double ready = timeline_time(a);
    a->pending = WA_PENDING_NONE;
    if (resolved(a->hold)) {
        apply_pending_rate(a);
        a->start = a->rate == 0 ? ready : ready - a->hold / a->rate;
        if (a->rate != 0) a->hold = NAN;
    } else if (resolved(a->start) && resolved(a->pending_rate)) {
        double match = (ready - a->start) * a->rate;
        apply_pending_rate(a);
        if (a->rate == 0) { a->hold = match; a->start = ready; }
        else a->start = ready - match / a->rate;
    }
    ready_resolve(a);
    update_finished(a, false, false);
}
static void run_pending_pause(struct web_animation *a) {
    double ready = timeline_time(a);
    a->pending = WA_PENDING_NONE;
    if (resolved(a->start) && !resolved(a->hold)) a->hold = (ready - a->start) * a->rate;
    apply_pending_rate(a);
    a->start = NAN;
    ready_resolve(a);
    update_finished(a, false, false);
}
/* §4.4.5 silently set the current time; -1: TypeError */
static int silently_set_current(struct web_animation *a, double seek) {
    if (!resolved(seek)) return resolved(web_animation_current(a)) ? -1 : 1;
    double t = timeline_time(a);
    if (resolved(a->hold) || !resolved(a->start) || !resolved(t) || a->rate == 0) a->hold = seek;
    else a->start = t - seek / a->rate;
    if (!resolved(t)) a->start = NAN;
    a->previous_current = NAN;
    return 0;
}
int web_animation_set_current(struct web_animation *a, double seek) {
    int r = silently_set_current(a, seek);
    if (r) return r < 0 ? -1 : 0;
    if (a->pending == WA_PENDING_PAUSE) {
        a->hold = seek; apply_pending_rate(a); a->start = NAN;
        a->pending = WA_PENDING_NONE; ready_resolve(a);
    }
    update_finished(a, true, false);
    mark_target(a->effect);
    return 0;
}
/* §4.4.6 set the start time */
void web_animation_set_start(struct web_animation *a, double start) {
    double t = timeline_time(a);
    if (!resolved(t) && resolved(start)) a->hold = NAN;
    double previous = web_animation_current(a);
    apply_pending_rate(a);
    a->start = start;
    if (resolved(start)) { if (a->rate != 0) a->hold = NAN; }
    else a->hold = previous;
    if (a->pending) { a->pending = WA_PENDING_NONE; ready_resolve(a); }
    update_finished(a, true, false);
    mark_target(a->effect);
}
/* §4.4.8 play an animation; -1: InvalidStateError */
int web_animation_play(struct web_animation *a, bool auto_rewind) {
    bool aborted_pause = a->pending == WA_PENDING_PAUSE, has_pending_ready = false;
    double seek = NAN, rate = effective_rate(a), current = web_animation_current(a), end = effect_end(a);
    if (auto_rewind) {
        if (rate >= 0 && (!resolved(current) || current < 0 || current >= end)) seek = 0;
        else if (rate < 0 && (!resolved(current) || current <= 0 || current > end)) {
            if (isinf(end)) return -1;
            seek = end;
        }
    }
    if (!resolved(seek) && !resolved(a->start) && !resolved(current)) seek = 0;
    if (resolved(seek)) a->hold = seek;
    if (resolved(a->hold)) a->start = NAN;
    if (a->pending) { a->pending = WA_PENDING_NONE; has_pending_ready = true; }
    if (!resolved(a->hold) && !resolved(seek) && !aborted_pause && !resolved(a->pending_rate)) return 0;
    if (!has_pending_ready) ready_new(a, WA_PROMISE_PENDING);
    a->pending = WA_PENDING_PLAY;
    update_finished(a, false, false);
    mark_target(a->effect);
    return 0;
}
/* §4.4.10 pause an animation */
int web_animation_pause(struct web_animation *a) {
    if (a->pending == WA_PENDING_PAUSE || web_animation_play_state(a) == WA_PAUSED) return 0;
    double seek = NAN;
    if (!resolved(web_animation_current(a))) {
        if (a->rate >= 0) seek = 0;
        else { double end = effect_end(a); if (isinf(end)) return -1; seek = end; }
    }
    if (resolved(seek)) a->hold = seek;
    bool has_pending_ready = false;
    if (a->pending == WA_PENDING_PLAY) { a->pending = WA_PENDING_NONE; has_pending_ready = true; }
    if (!has_pending_ready) ready_new(a, WA_PROMISE_PENDING);
    a->pending = WA_PENDING_PAUSE;
    update_finished(a, false, false);
    mark_target(a->effect);
    return 0;
}
/* §4.4.15 finish an animation */
int web_animation_finish(struct web_animation *a) {
    double rate = effective_rate(a), end = effect_end(a);
    if (rate == 0 || (rate > 0 && isinf(end))) return -1;
    apply_pending_rate(a);
    double limit = a->rate > 0 ? end : 0, t = timeline_time(a);
    silently_set_current(a, limit);
    if (!resolved(a->start) && resolved(t)) a->start = t - limit / a->rate;
    if (a->pending == WA_PENDING_PAUSE && resolved(a->start)) { a->hold = NAN; a->pending = WA_PENDING_NONE; ready_resolve(a); }
    if (a->pending == WA_PENDING_PLAY && resolved(a->start)) { a->pending = WA_PENDING_NONE; ready_resolve(a); }
    update_finished(a, true, true);
    mark_target(a->effect);
    return 0;
}
/* §4.4.18 cancel an animation */
void web_animation_cancel(struct web_animation *a) {
    if (web_animation_play_state(a) != WA_IDLE) {
        if (a->kind != WA_SCRIPT && a->effect) {
            /* CSS events: the active time at the cancel, with a fill of both */
            struct web_effect_progress p; effect_compute_fill(a->effect, CM_FILL_BOTH, &p);
            a->cancel_elapsed = p.active;
        }
        reset_pending(a);
        if (a->finished_state == WA_PROMISE_PENDING) a->finished_state = WA_PROMISE_REJECTED;
        finished_new(a);
        outbox(a, OUT_CANCEL, NAN, timeline_time(a));
    }
    mark_target(a->effect);
    a->hold = a->start = NAN;
}
/* §4.4.19 reverse an animation */
int web_animation_reverse(struct web_animation *a) {
    if (!a->timeline || !resolved(timeline_time(a))) return -1;
    double original = a->pending_rate;
    a->pending_rate = -effective_rate(a);
    if (web_animation_play(a, true) < 0) { a->pending_rate = original; return -1; }
    return 0;
}
/* §4.5.1 seamlessly update the playback rate */
void web_animation_update_rate(struct web_animation *a, double rate) {
    int previous = web_animation_play_state(a);
    a->pending_rate = rate;
    if (a->pending) return;
    if (previous == WA_IDLE || previous == WA_PAUSED || !resolved(web_animation_current(a))) apply_pending_rate(a);
    else if (previous == WA_FINISHED) {
        double unconstrained = current_without_hold(a), t = timeline_time(a);
        a->start = rate == 0 ? t : t - unconstrained / rate;
        apply_pending_rate(a);
        update_finished(a, false, false);
    } else web_animation_play(a, false);
    mark_target(a->effect);
}
/* §4.5 setting the playback rate */
void web_animation_set_rate(struct web_animation *a, double rate) {
    a->pending_rate = NAN;
    double previous = web_animation_current(a);
    a->rate = rate;
    if (resolved(previous)) web_animation_set_current(a, previous);
    mark_target(a->effect);
}
/* §4.4.3 setting the associated effect */
bool web_animation_set_effect(struct web_animation *a, struct web_effect *e) {
    if (a->effect == e) return true;
    if (e && e->animation) web_animation_set_effect(e->animation, NULL);
    mark_target(a->effect);
    if (a->effect) a->effect->animation = NULL;
    a->effect = e;
    if (e) e->animation = a;
    update_finished(a, false, false);
    mark_target(e);
    return true;
}
/* §4.4.1 setting the timeline (the document timeline or none) */
void web_animation_set_timeline(struct web_animation *a, bool document, double origin) {
    if (a->timeline == document && (!document || a->timeline_origin == origin)) return;
    a->timeline = document; a->timeline_origin = document ? origin : 0;
    if (resolved(a->start)) a->hold = NAN;
    update_finished(a, false, false);
    mark_target(a->effect);
}
void web_animation_persist(struct web_animation *a) { a->replace = WA_PERSISTED; }
void web_effect_timing_changed(struct web_effect *e) {
    if (e->animation) update_finished(e->animation, false, false);
    mark_target(e);
}

/* ---------------------------------------------------------------- composite order (§5.4.2) */
static node_t *composed_parent(node_t *n) { return n->parent ? n->parent : n->shadow_host; }
static int tree_order(node_t *a, node_t *b) {
    if (a == b) return 0;
    if (!a || !b) return a ? -1 : 1;
    int da = 0, db = 0;
    for (node_t *n = a; n; n = composed_parent(n)) da++;
    for (node_t *n = b; n; n = composed_parent(n)) db++;
    node_t *x = a, *y = b;
    while (da > db) { x = composed_parent(x); da--; }
    while (db > da) { y = composed_parent(y); db--; }
    if (x == y) return a == x ? -1 : 1;
    while (composed_parent(x) != composed_parent(y)) { x = composed_parent(x); y = composed_parent(y); }
    if (!x->parent && x->shadow_host) return -1; /* a shadow root precedes its host's children */
    if (!y->parent && y->shadow_host) return 1;
    for (node_t *s = x->next; s; s = s->next) if (s == y) return -1;
    return 1;
}
/* the element, then ::before, then ::after */
static int pseudo_rank(uint8_t pseudo) { return pseudo == PE_NONE ? 0 : pseudo == PE_BEFORE ? 1 : pseudo == PE_AFTER ? 3 : 2; }
static int animation_class(const struct web_animation *a) {
    return !a->owner || a->disowned ? 2 : a->kind == WA_CSS_TRANSITION ? 0 : a->kind == WA_CSS_ANIMATION ? 1 : 2;
}
static int composite_order(const struct web_animation *a, const struct web_animation *b) {
    int ca = animation_class(a), cb = animation_class(b);
    if (ca != cb) return ca - cb;
    if (ca < 2) {
        int order = tree_order(a->owner, b->owner);
        if (order) return order;
        if (a->owner_pseudo != b->owner_pseudo) return pseudo_rank(a->owner_pseudo) - pseudo_rank(b->owner_pseudo);
        if (ca == 0) {
            if (a->generation != b->generation) return a->generation < b->generation ? -1 : 1;
            int name = strcmp(a->name ? a->name : "", b->name ? b->name : "");
            if (name) return name;
        } else if (a->name_index != b->name_index) return a->name_index < b->name_index ? -1 : 1;
    }
    return a->sequence < b->sequence ? -1 : a->sequence > b->sequence;
}
static int composite_qsort(const void *x, const void *y) {
    return composite_order(*(struct web_animation *const *)x, *(struct web_animation *const *)y);
}
size_t web_animations_list(web_doc *d, node_t *root, bool subtree, int pseudo, struct web_animation ***out) {
    *out = NULL;
    struct web_animations *x = d ? d->animations : NULL;
    if (!x) return 0;
    web_animations_join(d);
    size_t n = 0, cap = 0; struct web_animation **v = NULL;
    for (struct web_animation *a = x->first; a; a = a->next) {
        if (!a->effect || !a->effect->target || !web_animation_relevant(a)) continue;
        node_t *t = a->effect->target;
        if (!root) { if (doc_node_root(t, true) != d->root) continue; }
        else if (pseudo >= 0) { if (t != root || a->effect->pseudo != pseudo) continue; }
        else {
            bool inside = t == root;
            if (subtree) for (node_t *p = t->parent; p && !inside; p = p->parent) inside = p == root;
            if (!inside) continue;
        }
        if (n == cap) {
            size_t c = cap ? cap * 2 : 8;
            struct web_animation **m = realloc(v, c * sizeof *m);
            if (!m) break;
            v = m; cap = c;
        }
        v[n++] = a;
    }
    if (n > 1) qsort(v, n, sizeof *v, composite_qsort);
    *out = v;
    return n;
}

/* ---------------------------------------------------------------- the effect value (§5.4.4) */
struct sampler {
    const struct css_anim_ctx *ctx;
    sbuf text, subst;
};
/* a keyframe value's style, var() substituted against the base style */
static style_t *value_style(struct sampler *s, const struct web_keyframe_value *v) {
    s->subst.n = 0;
    if (!css_substitute(s->ctx->base, v->text, &s->subst)) return NULL;
    style_t *st = ar_alloc(s->ctx->a, sizeof *st);
    return css_anim_parse(s->ctx, v->p, sb_cstr(&s->subst), s->subst.n, false, st, NULL) ? st : NULL;
}
/* a value this sampler combined: not yet final, so nothing in it is clamped (CSS Values 4 §3.1) */
static style_t *text_style(struct sampler *s, const struct propdef *p, const char *text) {
    style_t *st = ar_alloc(s->ctx->a, sizeof *st);
    return css_anim_parse(s->ctx, p, text, strlen(text), true, st, NULL) ? st : NULL;
}
/* the last value in keyframe f that sets longhand p */
static const struct web_keyframe_value *frame_value(const struct web_keyframe *f, int p) {
    for (uint32_t j = f->count; j-- > 0;)
        for (uint16_t k = 0; k < f->values[j].nlonghands; k++) if (f->values[j].longhands[k] == p) return &f->values[j];
    return NULL;
}
struct endpoint { double offset; const struct web_keyframe *frame; const style_t *value; };
static const style_t *composited(struct sampler *s, const struct propdef *p, const style_t *under, const style_t *value, int op) {
    if (op == CM_COMPOSITE_REPLACE || !under || !value) return value;
    s->text.n = 0;
    if (!css_anim_compose(s->ctx, p, under, value, op == CM_COMPOSITE_ACCUMULATE, &s->text)) return value;
    style_t *st = text_style(s, p, sb_cstr(&s->text));
    return st ? st : value;
}
/* The effect's value of longhand index li over the underlying value under, into out as computed
   text. false: the effect does not set it now. */
static bool effect_value(struct sampler *s, const struct web_effect *e, const struct web_effect_progress *pr, int li,
                         const style_t *under, sbuf *out) {
    const struct web_keyframes *k = e->frames;
    const struct propdef *p = css_prop_at(li);
    if (!k || !resolved(pr->progress)) return false;
    /* property-specific keyframes, with neutral keyframes (the underlying value) at 0 and 1 */
    struct endpoint list[k->count + 2];
    uint32_t n = 0, zero = 0, one = 0;
    for (uint32_t i = 0; i < k->count; i++) {
        const struct web_keyframe_value *v = frame_value(&k->v[i], li);
        if (!v) continue;
        const style_t *st = value_style(s, v);
        if (!st) continue; /* an invalid value is no keyframe for the property */
        list[n++] = (struct endpoint){k->v[i].computed, &k->v[i], st};
    }
    if (!n) return false;
    for (uint32_t i = 0; i < n; i++) { if (list[i].offset == 0) zero++; if (list[i].offset == 1) one++; }
    if (!zero) { memmove(list + 1, list, n * sizeof *list); list[0] = (struct endpoint){0, NULL, under}; n++; zero = 1; }
    if (!one) { list[n++] = (struct endpoint){1, NULL, under}; one = 1; }
    double progress = pr->progress;
    struct endpoint *a = NULL, *b = NULL;
    if (progress < 0 && zero > 1) a = &list[0];
    else if (progress >= 1 && one > 1) a = &list[n - 1];
    else {
        for (uint32_t i = 0; i < n; i++) if (list[i].offset <= progress && list[i].offset < 1) a = &list[i];
        if (!a) for (uint32_t i = 0; i < n; i++) if (list[i].offset == 0) a = &list[i];
        if (!a) a = &list[0];
        b = a + 1 < list + n ? a + 1 : NULL;
    }
    struct endpoint *ends[2] = {a, b};
    const style_t *values[2];
    for (int i = 0; i < 2; i++) {
        if (!ends[i]) { values[i] = NULL; continue; }
        const style_t *v = ends[i]->value;
        /* iteration composite accumulate: the final keyframe's value, current iteration times */
        if (ends[i]->frame && e->iteration_composite == CM_COMPOSITE_ACCUMULATE && resolved(pr->iteration) &&
            pr->iteration > 0 && isfinite(pr->iteration)) {
            const style_t *final = list[n - 1].value;
            s->text.n = 0;
            if (css_anim_compose(s->ctx, p, final, v, true, &s->text)) {
                style_t *once = text_style(s, p, sb_cstr(&s->text));
                s->text.n = 0;
                if (once && css_anim_interpolate(s->ctx, p, v, once, pr->iteration, &s->text)) {
                    style_t *st = text_style(s, p, sb_cstr(&s->text));
                    if (st) v = st;
                }
            }
        }
        if (ends[i]->frame) {
            int op = ends[i]->frame->composite == WA_COMPOSITE_AUTO ? e->composite : ends[i]->frame->composite;
            v = composited(s, p, under, v, op);
        }
        values[i] = v;
    }
    if (!values[1]) return css_computed_value(values[0], p, out, s->ctx->d);
    double span = ends[1]->offset - ends[0]->offset;
    double distance = span == 0 ? 0 : (progress - ends[0]->offset) / span;
    double t = ends[0]->frame ? css_easing_apply(&ends[0]->frame->easing, distance, false) : distance;
    size_t begin = out->n;
    if (css_anim_interpolate(s->ctx, p, values[0], values[1], t, out)) return true;
    out->n = begin;
    return css_computed_value(css_anim_discrete_second(p, values[0], values[1], t) ? values[1] : values[0], p, out, s->ctx->d);
}

/* ---------------------------------------------------------------- the cascade's side */
static bool motion_declared(const style_t *st, int *want) {
    for (uint32_t i = 0, n = css_motion_count(st, ML_NAME); i < n; i++)
        if (css_motion_item(st, ML_NAME, i).s) { *want |= 1; break; }
    for (uint32_t i = 0, n = css_motion_count(st, ML_TPROPERTY); i < n; i++) {
        if (!css_motion_item(st, ML_TPROPERTY, i).s) continue;
        double combined = fmax(css_motion_item(st, ML_TDURATION, i).f, 0) + css_motion_item(st, ML_TDELAY, i).f;
        if (combined > 0) { *want |= 2; break; }
    }
    return *want != 0;
}
int web_motion_wanted(const node_t *n, uint8_t pseudo, const style_t *base) {
    int want = 0;
    for (const struct web_effect *e = n->effects; e; e = e->target_next)
        if (e->pseudo == pseudo && e->animation) want |= 1;
    for (const struct web_animation *a = n->owned; a; a = a->owner_next)
        if (a->owner_pseudo == pseudo) want |= a->kind == WA_CSS_TRANSITION ? 2 : 1;
    if (base) motion_declared(base, &want);
    return want;
}
/* CSS Animations 2 §4.1, CSS Transitions 2 §4.1: style canceled the animation; the owning
   element stays until its cancel event is queued (web_animations_tick). */
static void css_disown(struct web_animation *a) {
    web_animation_cancel(a);
    a->disowned = true;
}
void web_motion_cancel(node_t *n, int pseudo) {
    for (struct web_animation *a = n->owned; a; a = a->owner_next)
        if (!a->disowned && (pseudo < 0 || a->owner_pseudo == pseudo)) css_disown(a);
}
static void timing_from_list(struct web_animation *a, const style_t *st, uint32_t i) {
    struct web_timing *t = &a->effect->timing, old = *t;
    if (!(a->overrides & WA_OVERRIDE_DURATION)) t->duration = css_motion_item(st, ML_DURATION, i).f;
    if (!(a->overrides & WA_OVERRIDE_DELAY)) t->delay = css_motion_item(st, ML_DELAY, i).f;
    if (!(a->overrides & WA_OVERRIDE_ITERATIONS)) t->iterations = css_motion_item(st, ML_ITERATIONS, i).f;
    if (!(a->overrides & WA_OVERRIDE_DIRECTION)) t->direction = css_motion_item(st, ML_DIRECTION, i).k;
    if (!(a->overrides & WA_OVERRIDE_FILL)) t->fill = css_motion_item(st, ML_FILL, i).k;
    if (!(a->overrides & WA_OVERRIDE_COMPOSITE)) a->effect->composite = css_motion_item(st, ML_COMPOSITION, i).k;
    if (memcmp(&old, t, sizeof old)) update_finished(a, false, false);
}
/* CSS Animations 2 §3.2 step 3: the rule's keyframes with the initial and final keyframes, which
   take the element's computed value of each animated property no keyframe at 0% (100%) sets, the
   animation's timing function and its composite (not the neutral value and linear easing of Web
   Animations' property-specific keyframes, §5.4.4). */
static void implicit_frame(const struct css_anim_ctx *ctx, struct web_keyframe *frames, uint32_t *n, double offset,
                           const uint8_t *animated, const struct css_easing *easing, uint8_t composite) {
    int nprops = css_prop_count();
    uint8_t present[nprops], set[nprops];
    memset(present, 0, sizeof present);
    struct web_keyframe *k = NULL;
    for (uint32_t i = 0; i < *n; i++) {
        struct web_keyframe *f = &frames[i];
        if (f->offset != offset) continue;
        for (uint32_t j = 0; j < f->count; j++) {
            if (!f->values[j].p) continue;
            css_prop_longhands(f->values[j].p, set);
            for (int p = 0; p < nprops; p++) present[p] |= set[p];
        }
        /* the keyframe at this offset with the default timing function and composite */
        if (!k && css_easing_same(&f->easing, easing) && (f->composite == WA_COMPOSITE_AUTO || f->composite == composite)) k = f;
    }
    uint32_t missing = 0;
    for (int p = 0; p < nprops; p++) missing += animated[p] && !present[p];
    if (!missing) return;
    if (!k) {
        /* a new initial keyframe is prepended, a new final keyframe appended */
        if (offset == 0) { memmove(frames + 1, frames, *n * sizeof *frames); k = frames; }
        else k = &frames[*n];
        (*n)++;
        *k = (struct web_keyframe){.offset = offset, .easing = *easing, .composite = composite};
    }
    struct web_keyframe_value *values = ar_alloc(ctx->a, (k->count + missing) * sizeof *values);
    if (k->count) memcpy(values, k->values, k->count * sizeof *values);
    k->values = values;
    sbuf text = {0};
    for (int p = 0; p < nprops; p++) {
        if (!animated[p] || present[p]) continue;
        const struct propdef *d = css_prop_at(p);
        text.n = 0;
        if (!css_computed_value(ctx->base, d, &text, ctx->d)) continue; /* it keeps the neutral keyframe */
        k->values[k->count++] = (struct web_keyframe_value){.p = d, .text = ar_strdup(ctx->a, sb_cstr(&text))};
    }
    sb_free(&text);
}
/* the declared keyframes completed, in ctx.a; declared itself when nothing is added */
static const struct web_keyframe *animation_frames(const struct css_anim_ctx *ctx, const struct web_keyframe *declared,
                                                   uint32_t *n, const struct css_easing *easing, uint8_t composite) {
    int nprops = css_prop_count();
    uint8_t animated[nprops], set[nprops];
    memset(animated, 0, sizeof animated);
    for (uint32_t i = 0; i < *n; i++)
        for (uint32_t j = 0; j < declared[i].count; j++) {
            if (!declared[i].values[j].p) continue;
            css_prop_longhands(declared[i].values[j].p, set);
            for (int p = 0; p < nprops; p++)
                if (set[p] && css_prop_animation(css_prop_at(p)) != CSS_ANIM_NONE) animated[p] = 1;
        }
    bool any = false;
    for (int p = 0; p < nprops && !any; p++) any = animated[p];
    if (!any || !ctx->base) return declared;
    struct web_keyframe *frames = ar_alloc(ctx->a, (*n + 2) * sizeof *frames);
    if (*n) memcpy(frames, declared, *n * sizeof *frames);
    implicit_frame(ctx, frames, n, 0, animated, easing, composite);
    implicit_frame(ctx, frames, n, 1, animated, easing, composite);
    return frames;
}
/* CSS Animations 2 §3: the CSSAnimation objects of an element and its animation-* lists */
void web_motion_animations(struct web_motion_site *site) {
    node_t *n = site->ctx.node;
    const style_t *st = site->ctx.base;
    web_doc *d = site->ctx.d;
    if (!d || !d->animations) return;
    size_t nold = 0;
    for (struct web_animation *a = n->owned; a; a = a->owner_next)
        if (a->kind == WA_CSS_ANIMATION && a->owner_pseudo == site->pseudo && !a->disowned) nold++;
    struct web_animation *old[nold ? nold : 1];
    bool used[nold ? nold : 1];
    nold = 0;
    for (struct web_animation *a = n->owned; a; a = a->owner_next)
        if (a->kind == WA_CSS_ANIMATION && a->owner_pseudo == site->pseudo && !a->disowned) { used[nold] = false; old[nold++] = a; }
    uint32_t count = css_motion_count(st, ML_NAME);
    for (uint32_t i = count; i-- > 0;) {
        const char *name = css_motion_item(st, ML_NAME, i).s;
        if (!name) continue;
        struct css_easing easing = css_motion_item(st, ML_EASING, i).e;
        uint32_t nframes = 0;
        const struct web_keyframe *frames = site->keyframes(site->arg, name, &easing, &nframes);
        if (!frames) continue; /* no @keyframes rule: no animation */
        frames = animation_frames(&site->ctx, frames, &nframes, &easing, css_motion_item(st, ML_COMPOSITION, i).k);
        struct web_animation *a = NULL;
        for (size_t j = nold; j-- > 0;)
            if (!used[j] && !strcmp(old[j]->name, name)) { a = old[j]; used[j] = true; break; }
        bool paused = css_motion_item(st, ML_PLAY, i).k != 0;
        if (a) {
            a->name_index = i;
            if (a->effect) {
                timing_from_list(a, st, i);
                if (!(a->overrides & WA_OVERRIDE_KEYFRAMES) && !keyframes_same(a->effect->frames, frames, nframes)) {
                    struct web_keyframes *k = web_keyframes_build(frames, nframes);
                    if (k) web_effect_set_keyframes(a->effect, k);
                }
            }
            if (!(a->overrides & WA_OVERRIDE_PLAY) && paused != a->css_paused) {
                if (paused) web_animation_pause(a);
                else web_animation_play(a, false);
            }
            a->css_paused = paused;
            continue;
        }
        /* records that fail half-made stay idle and unowned: the tick frees them */
        a = web_animation_new(d, WA_CSS_ANIMATION);
        struct web_effect *e = a ? web_effect_new(d) : NULL;
        if (!e) continue;
        a->effect = e; e->animation = a;
        struct web_keyframes *k = web_keyframes_build(frames, nframes);
        a->name = strdup(name);
        if (!k || !a->name) { keyframes_free(k); continue; }
        a->name_index = i;
        a->owner = n; a->owner_pseudo = site->pseudo; a->owner_next = n->owned; n->owned = a;
        e->frames = k;
        e->target = n; e->pseudo = site->pseudo; e->target_next = n->effects; n->effects = e;
        timing_from_list(a, st, i);
        a->css_paused = paused;
        if (paused) web_animation_pause(a);
        else web_animation_play(a, true);
    }
    for (size_t j = 0; j < nold; j++) if (!used[j]) css_disown(old[j]);
}

struct stack_entry { struct web_animation *a; struct web_effect_progress p; };
static int stack_order(const void *x, const void *y) {
    return composite_order(((const struct stack_entry *)x)->a, ((const struct stack_entry *)y)->a);
}
/* The effect stack of (node, pseudo) at the animation origin (every effect but CSS
   transitions) or at the transition origin (CSS transitions, for the longhands the animation
   origin does not set: CSS Transitions 1 §3), each longhand composed in composite order. */
size_t web_motion_sample(struct web_motion_site *site, bool transitions, uint8_t *animated) {
    node_t *n = site->ctx.node;
    size_t count = 0;
    for (struct web_effect *e = n->effects; e; e = e->target_next)
        if (e->pseudo == site->pseudo && e->animation && e->frames) count++;
    if (!count) return 0;
    struct stack_entry stack[count];
    size_t m = 0;
    int nprops = css_prop_count();
    uint8_t props[nprops]; memset(props, 0, sizeof props);
    for (struct web_effect *e = n->effects; e; e = e->target_next) {
        struct web_animation *a = e->animation;
        if (e->pseudo != site->pseudo || !a || !e->frames || a->replace == WA_REMOVED) continue;
        if ((a->kind == WA_CSS_TRANSITION && a->owner) != transitions) continue;
        struct web_effect_progress p; web_effect_compute(e, &p);
        if (!resolved(p.progress)) continue;
        stack[m++] = (struct stack_entry){a, p};
        for (int i = 0; i < nprops; i++) if (e->frames->properties[i]) props[i] = 1;
    }
    if (!m) return 0;
    if (m > 1) qsort(stack, m, sizeof *stack, stack_order);
    struct sampler s = {.ctx = &site->ctx};
    sbuf value = {0};
    size_t emitted = 0;
    for (int li = 0; li < nprops; li++) {
        if (!props[li] || (transitions && animated[li])) continue;
        const struct propdef *p = css_prop_at(li);
        const style_t *under = site->ctx.base;
        bool any = false;
        for (size_t i = 0; i < m; i++) {
            if (!stack[i].a->effect->frames->properties[li]) continue;
            value.n = 0;
            if (!effect_value(&s, stack[i].a->effect, &stack[i].p, li, under, &value)) continue;
            any = true;
            if (i + 1 < m) { const style_t *next = text_style(&s, p, sb_cstr(&value)); if (next) under = next; }
        }
        if (!any) continue;
        site->emit(site->arg, p, sb_cstr(&value));
        if (!transitions) animated[li] = 1;
        emitted++;
    }
    sb_free(&value); sb_free(&s.text); sb_free(&s.subst);
    return emitted;
}

/* ---------------------------------------------------------------- CSS Transitions (§3 of 1 and 2) */
static bool transitionable(const struct css_anim_ctx *ctx, const struct propdef *p, const style_t *a, const style_t *b,
                           bool allow_discrete) {
    if (css_prop_animation(p) == CSS_ANIM_NONE) return false;
    if (allow_discrete) return true;
    sbuf t = {0};
    bool ok = css_anim_interpolate(ctx, p, a, b, .5, &t);
    sb_free(&t);
    return ok;
}
static char *computed_text(const style_t *st, const struct propdef *p, web_doc *d) {
    sbuf b = {0};
    char *text = css_computed_value(st, p, &b, d) ? strdup(sb_cstr(&b)) : NULL;
    sb_free(&b);
    return text;
}
static struct web_animation *start_transition(struct web_motion_site *site, const struct propdef *p, const char *from,
                                              const char *to, const char *reversing, double shortening,
                                              double duration, double delay, const struct css_easing *easing) {
    web_doc *d = site->ctx.d;
    struct web_animation *a = web_animation_new(d, WA_CSS_TRANSITION);
    struct web_effect *e = a ? web_effect_new(d) : NULL;
    if (!a || !e) return NULL;
    struct web_keyframe_value v[2] = {{.p = p, .text = (char *)from}, {.p = p, .text = (char *)to}};
    struct web_keyframe frames[2] = {
        {.offset = 0, .easing = {.kind = CM_LINEAR}, .composite = WA_COMPOSITE_AUTO, .count = 1, .values = &v[0]},
        {.offset = 1, .easing = {.kind = CM_LINEAR}, .composite = WA_COMPOSITE_AUTO, .count = 1, .values = &v[1]}};
    e->frames = web_keyframes_build(frames, 2);
    a->name = strdup(css_prop_name(css_prop_index(p), NULL));
    a->to = strdup(to); a->reversing_from = strdup(reversing);
    a->property = p; a->shortening = shortening;
    a->generation = __atomic_add_fetch(&d->animations->transition_generation, 1, __ATOMIC_RELAXED);
    a->owner = site->ctx.node; a->owner_pseudo = site->pseudo; a->owner_next = a->owner->owned; a->owner->owned = a;
    e->timing = (struct web_timing){.delay = delay, .duration = duration, .iterations = 1, .fill = CM_FILL_BACKWARDS,
                                    .direction = CM_NORMAL, .easing = *easing};
    web_effect_set_target(e, site->ctx.node, site->pseudo);
    web_animation_set_effect(a, e);
    if (!e->frames || !a->name || !a->to || !a->reversing_from) { css_disown(a); return NULL; }
    web_animation_play(a, true);
    return a;
}
/* the current value of a running transition */
static char *transition_value(struct web_motion_site *site, struct web_animation *a) {
    struct sampler s = {.ctx = &site->ctx};
    struct web_effect_progress p;
    web_effect_compute(a->effect, &p);
    sbuf b = {0};
    char *text = effect_value(&s, a->effect, &p, css_prop_index(a->property), site->ctx.base, &b) ? strdup(sb_cstr(&b)) : NULL;
    sb_free(&b); sb_free(&s.text); sb_free(&s.subst);
    return text ? text : strdup(a->to);
}
/* animated: the longhands the animation origin sets. Their before-change value is the animation's
   value updated to the current time, which is their after-change value: no transition starts. */
void web_motion_transitions(struct web_motion_site *site, const style_t *before, const style_t *after, const uint8_t *animated) {
    node_t *n = site->ctx.node;
    web_doc *d = site->ctx.d;
    if (!d || !d->animations) return;
    if (before && before->display == D_NONE) before = NULL;
    int nprops = css_prop_count();
    int32_t match[nprops];
    for (int i = 0; i < nprops; i++) match[i] = -1;
    uint8_t set[nprops];
    uint32_t items = css_motion_count(after, ML_TPROPERTY);
    for (uint32_t i = 0; i < items; i++) {
        const char *name = css_motion_item(after, ML_TPROPERTY, i).s;
        if (!name) continue;
        if (!strcmp(name, "all")) { for (int p = 0; p < nprops; p++) match[p] = (int32_t)i; continue; }
        const struct propdef *p = css_prop_lookup(name, strlen(name));
        if (!p) continue;
        css_prop_longhands(p, set);
        for (int q = 0; q < nprops; q++) if (set[q]) match[q] = (int32_t)i;
    }
    uint8_t candidate[nprops]; memset(candidate, 0, sizeof candidate);
    for (int p = 0; p < nprops; p++)
        if (match[p] >= 0 && !animated[p] && css_prop_animation(css_prop_at(p)) != CSS_ANIM_NONE) candidate[p] = 1;
    for (struct web_animation *a = n->owned; a; a = a->owner_next)
        if (a->kind == WA_CSS_TRANSITION && a->owner_pseudo == site->pseudo && !a->disowned && a->property)
            candidate[css_prop_index(a->property)] = 1;
    struct css_anim_ctx after_ctx = site->ctx; after_ctx.base = after;
    for (int li = 0; li < nprops; li++) {
        if (!candidate[li]) continue;
        const struct propdef *p = css_prop_at(li);
        struct web_animation *running = NULL, *completed = NULL;
        for (struct web_animation *a = n->owned; a; a = a->owner_next) {
            if (a->kind != WA_CSS_TRANSITION || a->owner_pseudo != site->pseudo || a->disowned || a->property != p) continue;
            int state = web_animation_play_state(a);
            if (state == WA_FINISHED) completed = a;
            else if (state != WA_IDLE) running = a; /* one canceled by script is neither */
        }
        int32_t i = match[li];
        bool changed = before && !animated[li] && !css_anim_equal(p, before, after);
        if (!running && !completed && (!changed || i < 0)) continue; /* the usual case, before any text */
        double duration = i >= 0 ? fmax(css_motion_item(after, ML_TDURATION, (uint32_t)i).f, 0) : 0;
        double delay = i >= 0 ? css_motion_item(after, ML_TDELAY, (uint32_t)i).f : 0;
        bool allow_discrete = i >= 0 && css_motion_item(after, ML_TBEHAVIOR, (uint32_t)i).k;
        struct css_easing easing = i >= 0 ? css_motion_item(after, ML_TEASING, (uint32_t)i).e : (struct css_easing){.kind = CM_LINEAR};
        double combined = duration + delay;
        char *to = computed_text(after, p, d);
        if (!to) continue;
        if (!running && changed && i >= 0 && combined > 0 &&
            transitionable(&after_ctx, p, before, after, allow_discrete) && (!completed || strcmp(completed->to, to))) {
            /* §3 step 1 */
            if (completed) { css_disown(completed); completed = NULL; }
            char *from = computed_text(before, p, d);
            if (from) start_transition(site, p, from, to, from, 1, duration, delay, &easing);
            free(from);
        } else if (completed && strcmp(completed->to, to)) { css_disown(completed); completed = NULL; } /* step 2 */
        if (i < 0) { /* step 3 */
            if (running) css_disown(running);
            if (completed) css_disown(completed);
            free(to); continue;
        }
        if (running && strcmp(running->to, to)) { /* step 4 */
            char *current = transition_value(site, running);
            style_t *now = current ? ar_alloc(site->ctx.a, sizeof *now) : NULL;
            bool parsed = now && css_anim_parse(&after_ctx, p, current, strlen(current), false, now, NULL);
            if (!parsed || css_anim_equal(p, now, after) || !transitionable(&after_ctx, p, now, after, allow_discrete) ||
                combined <= 0) css_disown(running);
            else if (!strcmp(running->reversing_from, to)) {
                struct web_effect_progress pr; web_effect_compute(running->effect, &pr);
                double progress = resolved(pr.progress) ? pr.progress : 0;
                double factor = fabs(progress * running->shortening + (1 - running->shortening));
                factor = factor < 0 ? 0 : factor > 1 ? 1 : factor;
                char *old_end = strdup(running->to);
                css_disown(running);
                if (old_end) start_transition(site, p, current, to, old_end, factor, factor * duration,
                                              delay < 0 ? factor * delay : delay, &easing);
                free(old_end);
            } else {
                css_disown(running);
                start_transition(site, p, current, to, current, 1, duration, delay, &easing);
            }
            free(current);
        }
        free(to);
    }
}

/* ---------------------------------------------------------------- events (CSS Animations 2 §4.2, CSS Transitions 2 §5) */
static void queue_event(struct web_animations *x, struct web_animation *a, node_t *target, uint8_t pseudo, const char *type,
                        double current, double timeline, double elapsed, double scheduled) {
    if (x->nevents == x->cap) {
        size_t c = x->cap ? x->cap * 2 : 16;
        struct web_animation_event *v = realloc(x->events, c * sizeof *v);
        if (!v) return;
        x->events = v; x->cap = c;
    }
    x->events[x->nevents++] = (struct web_animation_event){.animation = a->id, .target = target, .pseudo = pseudo,
        .type = type, .current = current, .timeline = timeline, .elapsed = elapsed, .scheduled = scheduled,
        .name = target && a->name ? strdup(a->name) : NULL, .order = x->event_order++};
    x->script_dirty = true;
}
static double event_time(const struct web_animation *a, double elapsed) {
    if (!a->effect || !resolved(a->start) || a->rate == 0) return timeline_time(a);
    return a->start + (a->effect->timing.delay + elapsed) / a->rate;
}
static void css_events(struct web_animations *x, struct web_animation *a) {
    node_t *target = a->owner;
    int phase = WA_PHASE_IDLE;
    double iteration = NAN;
    if (web_animation_play_state(a) != WA_IDLE && a->effect) {
        struct web_effect_progress p; web_effect_compute(a->effect, &p);
        phase = p.phase; iteration = p.iteration;
        if (phase == WA_PHASE_IDLE) phase = WA_PHASE_BEFORE;
        if (a->kind == WA_CSS_TRANSITION && a->pending == WA_PENDING_PLAY) phase = WA_PHASE_BEFORE;
    }
    int was = a->event_phase;
    a->event_phase = (uint8_t)phase;
    double previous_iteration = a->event_iteration;
    a->event_iteration = iteration;
    bool iterated = a->kind == WA_CSS_ANIMATION && was == WA_PHASE_ACTIVE && phase == WA_PHASE_ACTIVE &&
        resolved(previous_iteration) && previous_iteration != iteration;
    if (!target || (was == phase && !iterated)) return;
    const struct web_timing *t = a->effect ? &a->effect->timing : NULL;
    double ad = t ? web_effect_active_duration(t) : 0, delay = t ? t->delay : 0;
    double start = fmax(fmin(-delay, ad), 0), end = fmax(fmin((t ? web_effect_end(t) : 0) - delay, ad), 0);
    uint8_t pseudo = a->owner_pseudo;
    double tl = timeline_time(a);
#define EV(type, elapsed) queue_event(x, a, target, pseudo, type, NAN, tl, elapsed, event_time(a, elapsed))
    if (a->kind == WA_CSS_ANIMATION) {
        bool was_idle_or_before = was == WA_PHASE_IDLE || was == WA_PHASE_BEFORE;
        if (phase == WA_PHASE_IDLE) {
            if (was != WA_PHASE_IDLE && was != WA_PHASE_AFTER)
                queue_event(x, a, target, pseudo, "animationcancel", NAN, tl, resolved(a->cancel_elapsed) ? a->cancel_elapsed : 0, tl);
        } else if (was_idle_or_before && phase == WA_PHASE_ACTIVE) EV("animationstart", start);
        else if (was_idle_or_before && phase == WA_PHASE_AFTER) { EV("animationstart", start); EV("animationend", end); }
        else if (was == WA_PHASE_ACTIVE && phase == WA_PHASE_BEFORE) EV("animationend", start);
        else if (was == WA_PHASE_ACTIVE && phase == WA_PHASE_ACTIVE) {
            double boundary = a->rate >= 0 ? iteration : iteration + 1;
            if (t) EV("animationiteration", (boundary - t->iteration_start) * iteration_duration(t));
        } else if (was == WA_PHASE_ACTIVE && phase == WA_PHASE_AFTER) EV("animationend", end);
        else if (was == WA_PHASE_AFTER && phase == WA_PHASE_ACTIVE) EV("animationstart", end);
        else if (was == WA_PHASE_AFTER && phase == WA_PHASE_BEFORE) { EV("animationstart", end); EV("animationend", start); }
    } else {
        if (phase == WA_PHASE_IDLE) {
            if (was != WA_PHASE_IDLE && was != WA_PHASE_AFTER)
                queue_event(x, a, target, pseudo, "transitioncancel", NAN, tl, resolved(a->cancel_elapsed) ? a->cancel_elapsed : 0, tl);
        } else if (was == WA_PHASE_IDLE) {
            EV("transitionrun", start);
            if (phase == WA_PHASE_ACTIVE) EV("transitionstart", start);
            else if (phase == WA_PHASE_AFTER) { EV("transitionstart", start); EV("transitionend", end); }
        } else if (was == WA_PHASE_BEFORE && phase == WA_PHASE_ACTIVE) EV("transitionstart", start);
        else if (was == WA_PHASE_BEFORE && phase == WA_PHASE_AFTER) { EV("transitionstart", start); EV("transitionend", end); }
        else if (was == WA_PHASE_ACTIVE && phase == WA_PHASE_AFTER) EV("transitionend", end);
        else if (was == WA_PHASE_ACTIVE && phase == WA_PHASE_BEFORE) EV("transitionend", start);
        else if (was == WA_PHASE_AFTER && phase == WA_PHASE_ACTIVE) EV("transitionstart", end);
        else if (was == WA_PHASE_AFTER && phase == WA_PHASE_BEFORE) { EV("transitionstart", end); EV("transitionend", start); }
    }
#undef EV
}
static void collect_outbox(struct web_animations *x, struct web_animation *a) {
    if (!a->outbox) return;
    static const char *const types[3] = {"finish", "cancel", "remove"};
    for (int i = 0; i < 3; i++) {
        if (!(a->outbox & (1 << i))) continue;
        double scheduled = a->out_time[i][1];
        if (i == 0 && resolved(a->start) && a->rate != 0) scheduled = a->start + effect_end(a) / a->rate;
        queue_event(x, a, NULL, PE_NONE, types[i], a->out_time[i][0], a->out_time[i][1], 0, scheduled);
    }
    a->outbox = 0;
}
/* Web Animations 1 §5.5 remove replaced animations */
static bool replaceable(const struct web_animation *a) {
    return (!a->owner || a->disowned) && a->replace == WA_ACTIVE && a->timeline && a->effect && a->effect->target && a->effect->frames &&
        web_animation_play_state(a) == WA_FINISHED;
}
static void remove_replaced(struct web_animations *x) {
    int nprops = css_prop_count();
    for (struct web_animation *a = x->first; a; a = a->next) {
        if (!replaceable(a)) continue;
        const struct web_effect *e = a->effect;
        bool all = true, any = false;
        for (int p = 0; p < nprops && all; p++) {
            if (!e->frames->properties[p]) continue;
            any = true;
            bool covered = false;
            for (const struct web_effect *o = e->target->effects; o && !covered; o = o->target_next)
                covered = o != e && o->pseudo == e->pseudo && o->animation && replaceable(o->animation) &&
                    composite_order(o->animation, a) > 0 && o->frames->properties[p];
            all = covered;
        }
        if (!any || !all) continue;
        a->replace = WA_REMOVED;
        outbox(a, OUT_REMOVE, web_animation_current(a), timeline_time(a));
        mark_target(a->effect);
    }
}
static bool script_runs(web_doc *d) { return d->js && web_js_enabled(d); }
static void drop_events(struct web_animations *x) {
    for (size_t i = 0; i < x->nevents; i++) free(x->events[i].name);
    x->nevents = 0;
}
static bool dead(const struct web_animation *a) {
    return !a->wrapped && !a->owner && !a->outbox && !a->finish_task && web_animation_play_state(a) == WA_IDLE;
}
void web_animations_cascading(web_doc *d, bool on) {
    struct web_animations *x = on ? web_animations_of(d) : d ? d->animations : NULL;
    if (!x) return;
    x->cascading = on;
    if (!on) web_animations_join(d);
}
void web_animations_tick(web_doc *d, uint64_t now) {
    struct web_animations *x = d ? d->animations : NULL;
    if (!x || __atomic_load_n(&d->native_cancelled, __ATOMIC_ACQUIRE)) return;
    x->cascading = false; /* a cascade that longjmp'd out left it set */
    web_animations_join(d);
    if (!x->origin_set) { x->origin = now; x->origin_set = true; }
    /* HTML "update the rendering" leaves a hidden document out of "update animations and send
       events": its timeline's current time stays until it is shown again. */
    if (!d->visible) return;
    double time = (double)(now >= x->origin ? now - x->origin : 0);
    if (!resolved(x->now) || time > x->now) x->now = time;
    x->last_update = now;
    if (!x->first) return;
    for (struct web_animation *a = x->first; a; a = a->next) {
        int state = web_animation_play_state(a);
        double hold = a->hold, start = a->start;
        if (a->pending && a->timeline) {
            if (a->pending == WA_PENDING_PLAY) run_pending_play(a); else run_pending_pause(a);
        }
        if (a->timeline) update_finished(a, false, false);
        if (a->kind != WA_SCRIPT) css_events(x, a);
        /* a disowned declarative animation leaves its element once its cancel event is queued */
        if (a->disowned && a->owner && a->event_phase == WA_PHASE_IDLE) unlink_owner(a);
        int after = web_animation_play_state(a);
        if (a->effect && a->effect->target &&
            (after == WA_RUNNING || after != state || !(hold == a->hold || (isnan(hold) && isnan(a->hold))) ||
             !(start == a->start || (isnan(start) && isnan(a->start)))))
            mark_target(a->effect);
    }
    remove_replaced(x);
    for (struct web_animation *a = x->first; a; a = a->next) collect_outbox(x, a);
    if (!script_runs(d)) {
        /* nothing can listen or wait: finish notifications run here, events go nowhere */
        for (struct web_animation *a = x->first; a; a = a->next) if (a->finish_task) web_animation_finish_notify(a);
        for (struct web_animation *a = x->first; a; a = a->next) a->outbox = 0;
        drop_events(x);
        x->script_dirty = false;
    }
    struct web_animation **link = &x->first, *last = NULL;
    while (*link) {
        struct web_animation *a = *link;
        if (dead(a)) { *link = a->next; animation_free(a); continue; }
        last = a; link = &a->next;
    }
    x->last = last;
}
/* outboxes into the event queue, after a script operation */
void web_animations_collect(web_doc *d) {
    struct web_animations *x = d ? d->animations : NULL;
    if (!x) return;
    web_animations_join(d);
    for (struct web_animation *a = x->first; a; a = a->next) collect_outbox(x, a);
}
int64_t web_animations_deadline(web_doc *d) {
    struct web_animations *x = d ? d->animations : NULL;
    if (!x || !d->visible || __atomic_load_n(&d->native_cancelled, __ATOMIC_ACQUIRE)) return -1;
    uint64_t now = uptime_ms();
    if (__atomic_load_n(&x->added, __ATOMIC_ACQUIRE)) return (int64_t)now;
    bool running = false;
    for (struct web_animation *a = x->first; a; a = a->next) {
        if ((a->pending && a->timeline) || a->outbox || (a->disowned && a->owner)) return (int64_t)now;
        if (a->timeline && a->effect && web_animation_play_state(a) == WA_RUNNING) running = true;
    }
    if (!running) return -1;
    uint64_t due = x->last_update + 16;
    return (int64_t)(due > now ? due : now);
}
bool web_animations_script_work(web_doc *d) {
    struct web_animations *x = d ? d->animations : NULL;
    return x && (x->nevents || __atomic_load_n(&x->script_dirty, __ATOMIC_RELAXED));
}
/* the script's time origin wins over the first tick's guess: a cascade can make the records first */
void web_animations_set_origin(web_doc *d, uint64_t origin) {
    struct web_animations *x = web_animations_of(d);
    if (x) { x->origin = origin; x->origin_set = true; }
}
size_t web_animations_take_events(web_doc *d, struct web_animation_event **out) {
    struct web_animations *x = d ? d->animations : NULL;
    *out = NULL;
    if (!x || !x->nevents) return 0;
    /* by scheduled time (unresolved first), then in the order queued */
    for (size_t i = 1; i < x->nevents; i++) {
        struct web_animation_event e = x->events[i];
        size_t j = i;
        for (; j > 0; j--) {
            const struct web_animation_event *p = &x->events[j - 1];
            bool later = resolved(p->scheduled) && (!resolved(e.scheduled) || p->scheduled > e.scheduled);
            if (!later) break;
            x->events[j] = x->events[j - 1];
        }
        x->events[j] = e;
    }
    *out = x->events;
    size_t n = x->nevents;
    x->events = NULL; x->nevents = x->cap = 0;
    return n;
}
void web_animation_release(struct web_animation *a) { if (a && a->wrapped) a->wrapped--; }
void web_effect_release(struct web_effect *e) {
    if (!e || !e->wrapped || --e->wrapped || e->animation) return;
    struct web_animations *x = e->doc;
    for (struct web_effect **p = &x->effects; *p; p = &(*p)->next) if (*p == e) { *p = e->next; effect_free(e); return; }
}
/* the node is freed: what it owns is canceled and what targets it loses its target */
void web_animations_node_free(node_t *n) {
    while (n->effects) {
        struct web_effect *e = n->effects;
        n->effects = e->target_next; e->target = NULL; e->target_next = NULL;
    }
    /* canceled without events: nothing is left to receive them */
    while (n->owned) {
        struct web_animation *a = n->owned;
        n->owned = a->owner_next; a->owner = NULL; a->owner_next = NULL;
        if (web_animation_play_state(a) != WA_IDLE) {
            if (a->ready_state == WA_PROMISE_PENDING) { a->ready_state = WA_PROMISE_REJECTED; ready_new(a, WA_PROMISE_RESOLVED); }
            if (a->finished_state == WA_PROMISE_PENDING) a->finished_state = WA_PROMISE_REJECTED;
            finished_new(a);
        }
        a->pending = WA_PENDING_NONE; apply_pending_rate(a);
        a->hold = a->start = NAN; a->disowned = true; a->event_phase = WA_PHASE_IDLE;
    }
    web_doc *d = n->owner;
    struct web_animations *x = d ? d->animations : NULL;
    if (x) for (size_t i = 0; i < x->nevents; i++) if (x->events[i].target == n) x->events[i].target = NULL;
}
void web_animations_doc_free(web_doc *d) {
    struct web_animations *x = d ? d->animations : NULL;
    if (!x) return;
    web_animations_join(d);
    while (x->first) {
        struct web_animation *a = x->first; x->first = a->next;
        unlink_owner(a);
        if (a->effect) { a->effect->animation = NULL; a->effect = NULL; }
        free(a->name); free(a->to); free(a->reversing_from); free(a);
    }
    while (x->effects) {
        struct web_effect *e = x->effects; x->effects = e->next;
        unlink_target(e); keyframes_free(e->frames); free(e);
    }
    web_animations_join(d); /* the retired keyframes */
    drop_events(x); free(x->events);
    free(x); d->animations = NULL;
}

/* commitStyles: each longhand the effect sets, at its current value over the base style */
bool web_animation_commit(struct web_animation *a, sbuf *out, int *count) {
    *count = 0;
    struct web_effect *e = a->effect;
    if (!e || !e->target || !e->frames || !e->target->style) return false;
    node_t *t = e->target;
    const style_t *base = t->animation_base_style ? t->animation_base_style : t->style;
    if (e->pseudo == PE_BEFORE) base = base->before;
    else if (e->pseudo == PE_AFTER) base = base->after;
    if (!base) return false;
    node_t *parent = doc_flat_parent(t);
    arena_t arena = {0}; jmp_buf trap; arena.trap = &trap;
    if (setjmp(trap)) { ar_free(&arena); return false; }
    struct css_anim_ctx ctx = {.base = base, .parent = e->pseudo != PE_NONE ? t->style : parent && parent->style ? parent->style : NULL,
        .node = t, .box = t->box, .rem = t->owner && t->owner->html && t->owner->html->style ? t->owner->html->style->font_size : 16,
        .vw = (float)(t->owner ? t->owner->styled_w : 0), .vh = (float)(t->owner ? t->owner->styled_h : 0), .a = &arena, .d = t->owner};
    struct sampler s = {.ctx = &ctx};
    struct web_effect_progress p; web_effect_compute(e, &p);
    sbuf value = {0};
    for (int li = 0; li < css_prop_count(); li++) {
        if (!e->frames->properties[li]) continue;
        value.n = 0;
        if (!effect_value(&s, e, &p, li, base, &value)) continue;
        sb_puts(out, css_prop_name(li, NULL)); sb_putc(out, 0);
        sb_puts(out, sb_cstr(&value)); sb_putc(out, 0);
        (*count)++;
    }
    sb_free(&value); sb_free(&s.text); sb_free(&s.subst); ar_free(&arena);
    return true;
}
