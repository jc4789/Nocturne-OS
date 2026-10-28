/* The one time model of CSS Transitions, CSS Animations and Web Animations (Web Animations 1
   §4: timelines, animations, animation effects; CSS Animations 2 and CSS Transitions 2 for the
   declarative ones). animation.c owns every record; the cascade (css.c) asks it to update the
   declarative animations of an element and to sample the effects into the animation and
   transition origins; the script facade (js_animations.js through js_animation_native.h) is
   a view of the same records, so getAnimations() returns CSSAnimation and CSSTransition objects
   that are these. Times are milliseconds; NAN is unresolved. */
#pragma once
#include "webi.h"

enum { WA_SCRIPT, WA_CSS_ANIMATION, WA_CSS_TRANSITION };
enum { WA_IDLE, WA_RUNNING, WA_PAUSED, WA_FINISHED };          /* play state */
enum { WA_ACTIVE, WA_REMOVED, WA_PERSISTED };                  /* replace state */
enum { WA_PENDING_NONE, WA_PENDING_PLAY, WA_PENDING_PAUSE };
enum { WA_PROMISE_PENDING, WA_PROMISE_RESOLVED, WA_PROMISE_REJECTED };
enum { WA_FILL_AUTO = 4 };                                      /* beside CM_FILL_* */
enum { WA_COMPOSITE_AUTO = 3 };                                 /* beside CM_COMPOSITE_* */
enum { WA_PHASE_BEFORE, WA_PHASE_ACTIVE, WA_PHASE_AFTER, WA_PHASE_IDLE };
/* The API overrides of a CSSAnimation (CSS Animations 2 §4.1): what the script set wins over
   the animation-* properties from then on. */
enum { WA_OVERRIDE_DURATION = 1, WA_OVERRIDE_ITERATIONS = 2, WA_OVERRIDE_DIRECTION = 4, WA_OVERRIDE_DELAY = 8,
       WA_OVERRIDE_FILL = 16, WA_OVERRIDE_KEYFRAMES = 32, WA_OVERRIDE_PLAY = 64, WA_OVERRIDE_COMPOSITE = 128,
       WA_OVERRIDE_END_DELAY = 256, WA_OVERRIDE_ITERATION_START = 512, WA_OVERRIDE_EASING = 1024 };

struct web_timing {
    double delay, end_delay, iteration_start, iterations, duration; /* duration NAN: auto */
    uint8_t fill, direction;                                        /* CM_FILL_* or WA_FILL_AUTO, CM_* */
    struct css_easing easing;
};
/* One property value of a keyframe as written (a shorthand stays a shorthand); its
   longhands, the properties it animates, are computed once. */
struct web_keyframe_value { const struct propdef *p; char *text; uint16_t *longhands; uint16_t nlonghands; };
struct web_keyframe {
    double offset, computed; /* offset NAN: null (computed spaces it) */
    struct css_easing easing;
    uint8_t composite;       /* CM_COMPOSITE_* or WA_COMPOSITE_AUTO */
    uint32_t count;
    struct web_keyframe_value *values;
};
struct web_keyframes {
    uint32_t count;
    struct web_keyframe *v;
    uint8_t *properties;     /* css_prop_count() flags: the longhands any keyframe sets */
    struct web_keyframes *garbage_next;
};
struct web_effect {
    uint32_t id;
    struct web_animations *doc;
    struct web_effect *next;           /* every effect of the document */
    struct web_animation *animation;   /* at most one */
    node_t *target; uint8_t pseudo;    /* PE_NONE, PE_BEFORE, PE_AFTER */
    struct web_effect *target_next;    /* target->effects */
    struct web_timing timing;
    struct web_keyframes *frames;
    uint8_t composite, iteration_composite; /* CM_COMPOSITE_*; iteration: replace or accumulate */
    bool wrapped;                      /* a script object stands for it until it is released */
};
struct web_animation {
    uint32_t id;
    uint8_t kind, pending, replace;
    bool timeline;                     /* a document timeline; false: no timeline */
    double timeline_origin;            /* DocumentTimeline originTime: its zero on the document's */
    struct web_animations *doc;
    struct web_animation *next;        /* every animation, in creation order */
    struct web_effect *effect;
    double start, hold, rate, pending_rate, previous_current; /* pending_rate NAN: none */
    uint64_t sequence;                 /* the global animation list position (script composite order) */
    /* the promises, which the script facade mirrors by generation */
    uint32_t ready_gen, finished_gen;
    uint8_t ready_state, ready_prev, finished_state, finished_prev;
    bool finish_task;                  /* the finish notification steps are queued as a microtask */
    bool wrapped;
    /* playback events to queue (finish, cancel, remove), with their times */
    uint8_t outbox;
    double out_time[3][2];
    /* declarative: the owning element, and what made it */
    node_t *owner; uint8_t owner_pseudo;
    struct web_animation *owner_next;  /* owner->owned */
    char *name;                        /* CSSAnimation: animation-name; CSSTransition: transition-property */
    uint32_t name_index;               /* position in animation-name */
    uint32_t overrides;                /* WA_OVERRIDE_* */
    bool css_paused;                   /* animation-play-state was paused at the last update */
    bool disowned;                     /* style canceled it: no longer owned, the element kept for the cancel event */
    double cancel_elapsed;             /* the active time at the cancel (fill both), for its CSS event */
    const struct propdef *property;    /* CSSTransition */
    char *to, *reversing_from;         /* end value, reversing-adjusted start value */
    double shortening;
    uint64_t generation;               /* transition generation */
    uint8_t event_phase;               /* CSS events: the phase and iteration last reported */
    double event_iteration;
};
struct web_animation_event {
    uint32_t animation;                /* the Animation (playback events) */
    node_t *target;                    /* CSS events: the owning element */
    uint8_t pseudo;
    const char *type;
    double current, timeline, elapsed, scheduled;
    char *name;
    uint64_t order;
};
struct web_animations {
    web_doc *doc;
    struct web_animation *first, *last, *added; /* added: created by cascade workers, joined later */
    struct web_effect *effects, *added_effects;
    struct web_keyframes *garbage;
    uint64_t sequence, transition_generation, event_order;
    uint32_t next_id;
    bool origin_set, script_dirty, cascading; /* cascading: workers run; no restyle marks */
    uint64_t origin;                   /* uptime_ms() of the timeline's time zero */
    double now;                        /* the document timeline's current time */
    uint64_t last_update;
    struct web_animation_event *events;
    size_t nevents, cap;
};

struct web_animations *web_animations_of(web_doc *d);
void web_animations_join(web_doc *d);
/* Web Animations 1 §4.4 procedures */
struct web_animation *web_animation_new(web_doc *d, uint8_t kind);
struct web_effect *web_effect_new(web_doc *d);
void web_effect_set_target(struct web_effect *e, node_t *target, uint8_t pseudo);
void web_effect_set_keyframes(struct web_effect *e, struct web_keyframes *k);
struct web_keyframes *web_keyframes_build(const struct web_keyframe *frames, uint32_t count);
bool web_animation_set_effect(struct web_animation *a, struct web_effect *e);
void web_animation_set_timeline(struct web_animation *a, bool document, double origin);
void web_effect_timing_changed(struct web_effect *e); /* after updateTiming and the like: the finished state, the style */
int web_animation_play(struct web_animation *a, bool auto_rewind); /* <0: InvalidStateError */
int web_animation_pause(struct web_animation *a);
int web_animation_finish(struct web_animation *a);
void web_animation_cancel(struct web_animation *a);
int web_animation_reverse(struct web_animation *a);
void web_animation_update_rate(struct web_animation *a, double rate);
void web_animation_set_rate(struct web_animation *a, double rate);
int web_animation_set_current(struct web_animation *a, double t); /* <0: TypeError */
void web_animation_set_start(struct web_animation *a, double t);
void web_animation_finish_notify(struct web_animation *a);
void web_animation_persist(struct web_animation *a);
double web_animation_current(const struct web_animation *a);
int web_animation_play_state(const struct web_animation *a);
bool web_animation_relevant(const struct web_animation *a);
double web_effect_end(const struct web_timing *t);
double web_effect_active_duration(const struct web_timing *t);
struct web_effect_progress { double local, active, progress, iteration; int phase; };
void web_effect_compute(const struct web_effect *e, struct web_effect_progress *out);
struct web_animation *web_animation_by_id(web_doc *d, uint32_t id);
struct web_effect *web_effect_by_id(web_doc *d, uint32_t id);
void web_animation_release(struct web_animation *a);
void web_effect_release(struct web_effect *e);
/* the relevant animations in composite order (Web Animations 1 §5.4.2) of the document (root NULL:
   targets in it), else of root: pseudo >= 0, its element (PE_NONE) or that pseudo-element; pseudo < 0,
   all of root's and, with subtree, its descendants' in the same tree (not across a shadow boundary) */
size_t web_animations_list(web_doc *d, node_t *root, bool subtree, int pseudo, struct web_animation ***out);
/* commitStyles: the effect's current values, longhand by longhand, as text pairs */
bool web_animation_commit(struct web_animation *a, sbuf *out, int *count);
/* the doc's queued events in dispatch order; the caller frees the names and the array */
size_t web_animations_take_events(web_doc *d, struct web_animation_event **out);
void web_animations_collect(web_doc *d); /* the playback events of script operations, into the queue */
