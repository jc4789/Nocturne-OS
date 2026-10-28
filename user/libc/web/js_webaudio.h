#pragma once
/* The Web Audio API's binding (js_webaudio.js above, webaudio.h below): host.webaudio,
 * the tokens js_webaudio.js holds for contexts, nodes, buffers and periodic waves, and the
 * tasks that carry what the renderers report back to the page. Window realms only, on the
 * page's thread. */
#include <stdbool.h>
#include <stdint.h>
#include "quickjs.h"

/* host.webaudio: the operations js_webaudio.js calls. */
void web_js_webaudio_init(JSContext *ctx, JSValueConst host);
/* The global names of the interfaces js_webaudio.js returns, each defined as a lazy property
 * of global that init makes (js.c runs the program the first time one is touched). */
bool web_audio_install(JSContext *ctx, JSValueConst global, const JSLazyProperty *init);

/* The realm's Web Audio tasks: an event a context's renderer posted, or a decodeAudioData
 * that finished. run does one step: 0 when there was nothing to do, 1 when it ran, -1 when
 * the page's callback threw (the exception is pending in ctx). */
bool web_audio_ready(JSContext *ctx);
int web_audio_run(JSContext *ctx);
/* now when a task is ready; otherwise -1, with the renderers' event words and the
 * resampling threads watched on web_wait_queue so their news wakes the browser. */
int64_t web_audio_deadline(JSContext *ctx, uint64_t now);
/* An AudioContext (not an OfflineAudioContext) of the realm is running: it sends sound out. */
bool web_audio_audible(JSContext *ctx);
/* The realm's Document is no longer fully active (its browsing context was retired): its
 * contexts close, its decodes are dropped, and no new context starts in it. */
void web_audio_realm_end(JSContext *ctx);
/* ctx is about to be freed: the same, and Web Audio forgets the realm. */
void web_audio_realm_free(JSContext *ctx);
