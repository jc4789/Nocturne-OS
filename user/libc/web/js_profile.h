/* Where a runtime's JavaScript time goes, for --debug-js.
 *
 * At each interpreter interrupt the time since the previous sample is counted
 * for the function then running, so a function's time includes the native
 * calls it makes. The time between tasks is not counted: a task starts the
 * clock again. Functions are told apart by their bytecode, named when first
 * seen, and reported by their place in the source (name, line and column):
 * the copies of one function in several realms are one entry. */
#pragma once
#include <stdint.h>
#include "quickjs.h"

struct js_profile;
/* A task begins: the time since the last sample was spent outside JavaScript. */
void js_profile_start(struct js_profile *profile);
/* Counts the time since the last sample for the function running in rt.
   Creates *profile at the first sample; does nothing when that fails. */
void js_profile_sample(struct js_profile **profile, JSRuntime *rt, JSContext *ctx);
/* The counted time in ms; 0 for no profile. */
uint64_t js_profile_ms(const struct js_profile *profile);
/* Logs the functions with the most time and the time of each script, then
   starts counting afresh. */
void js_profile_report(struct js_profile *profile, void (*log)(void *opaque, const char *line), void *opaque);
void js_profile_free(struct js_profile *profile);
