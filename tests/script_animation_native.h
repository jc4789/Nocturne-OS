/* A script animation made through the one time model (animation.h), as the script facade
   (js_animation_native.h) makes the records of
     a = new Animation(new KeyframeEffect(target, [{property: value}, {property: value}],
                                          {duration: 1000, pseudoElement}), document.timeline);
     a.currentTime = 0;
   paused at its start (Web Animations 1 §4.4.5: setting the current time of an idle animation
   holds it), so it holds value with no pending task. The test holds it as a script object does;
   web_animation_cancel(a) removes its effect. NULL: the value does not parse, or no memory. */
#include "animation.h"

static struct web_animation *script_animation_hold(web_doc *d, node_t *target, uint8_t pseudo, const char *property,
                                                   const char *value) {
    const struct propdef *p = css_prop_lookup(property, strlen(property));
    if (!p || !css_value_valid(p, value, strlen(value)) || !web_animations_of(d)) return NULL;
    struct web_effect *e = web_effect_new(d);
    struct web_animation *a = e ? web_animation_new(d, WA_SCRIPT) : NULL;
    if (!a) return NULL;
    a->wrapped = 1;
    web_animations_join(d);
    struct web_keyframe_value v = {.p = p, .text = (char *)value};
    struct web_keyframe frames[2] = {
        {.offset = 0, .easing = {.kind = CM_LINEAR}, .composite = WA_COMPOSITE_AUTO, .count = 1, .values = &v},
        {.offset = 1, .easing = {.kind = CM_LINEAR}, .composite = WA_COMPOSITE_AUTO, .count = 1, .values = &v}};
    struct web_keyframes *k = web_keyframes_build(frames, 2);
    if (!k) return NULL;
    web_effect_set_keyframes(e, k);
    e->timing.duration = 1000;
    web_effect_set_target(e, target, pseudo);
    web_animation_set_effect(a, e);
    return web_animation_set_current(a, 0) == 0 ? a : NULL;
}
