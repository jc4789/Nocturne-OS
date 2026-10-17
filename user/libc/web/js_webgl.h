#pragma once
/* WebGL 1/2 rendering contexts on Mesa softpipe (ports/mesa/nocturne_gl).
 * The classes are realm-neutral: the Window realm attaches a context to an
 * HTMLCanvasElement, Worker realms to an OffscreenCanvas. */
#include "quickjs.h"
#include <stdbool.h>
#include <stdint.h>

struct web_webgl;

/* A TexImageSource decoded by the embedding realm: top-down,
 * non-premultiplied 0xAARRGGBB, borrowed for the duration of one call. */
struct web_webgl_pixels {
    const uint32_t *argb;
    unsigned width, height;
    bool tainted;
};
/* Resolves a value the realm's JS source hook returned (a DOM node or an
 * ImageBitmap token). 1: pixels filled, 0: not an image source (TypeError),
 * 2: an image that has no pixels yet, -1: exception pending. */
typedef int (*web_webgl_source_fn)(JSContext *ctx, JSValueConst value, struct web_webgl_pixels *out);

/* Installs the WebGL interfaces on the global object and the private
 * host.webglCreate / host.webglSources entry points. */
void web_js_webgl_init(JSContext *ctx, JSValueConst host, web_webgl_source_fn source);

/* Creates a context of version 1 or 2 for canvas (kept as `.canvas`),
 * reading WebGLContextAttributes from options. Returns JS_NULL when no
 * context can be made, JS_EXCEPTION when options threw. */
JSValue web_webgl_create(JSContext *ctx, JSValueConst canvas, int version, JSValueConst options,
                         unsigned width, unsigned height, struct web_webgl **out);

/* Native owners (a canvas element, an OffscreenCanvas placeholder link)
 * hold their own reference; the context survives until both the JS object
 * and every native owner are gone. */
void web_webgl_retain(struct web_webgl *gl);
void web_webgl_release(struct web_webgl *gl);
/* Called when rendering changes the drawing buffer; owner == NULL detaches. */
void web_webgl_set_owner(struct web_webgl *gl, void (*changed)(void *owner), void *owner);
/* Canvas width/height changed: the drawing buffer is reallocated. */
void web_webgl_resize(struct web_webgl *gl, unsigned width, unsigned height);
/* Count of drawing-buffer changes; equal values mean nothing new to show. */
uint64_t web_webgl_serial(const struct web_webgl *gl);
/* Copies the drawing buffer to dst (width x height, scaled if the drawing
 * buffer was clamped). With frame_end the buffer then follows
 * preserveDrawingBuffer: cleared unless preserved. */
bool web_webgl_present(struct web_webgl *gl, uint32_t *dst, unsigned width, unsigned height, bool frame_end);
/* debug_js: per-entry-point timing in the debug log. */
void web_webgl_set_profile(bool on);
