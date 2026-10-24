#pragma once
#include "webi.h"
#include "quickjs.h"
/* Called with a brand-checked native DOM node; argv begins with operation. */
JSValue web_canvas_native(JSContext *ctx, node_t *node, int argc, JSValueConst *argv);
/* Brand-checked source node, then coords (2/4/8 doubles), matrix (6 doubles),
 * global alpha, smoothing. Returns 1 for unusable/broken source, otherwise
 * undefined; the private JS bridge constructs the InvalidStateError. */
JSValue web_canvas_draw_image(JSContext *ctx, node_t *dst, node_t *src, int argc, JSValueConst *argv);
/* Private ImageBitmap token: independent QuickJS-owned pixels, no document
 * or DOM pointers survive creation. Native calls borrow a rooted token. */
bool web_image_bitmap_is(JSValueConst value);
bool web_image_bitmap_dimensions(JSValueConst value, unsigned *width, unsigned *height);
JSValue web_image_bitmap_native(JSContext *ctx, web_doc *document, node_t *source, int argc, JSValueConst *argv);
JSValue web_canvas_draw_bitmap(JSContext *ctx, node_t *dst, JSValueConst bitmap, int argc, JSValueConst *argv);
struct web_webgl_pixels;
/* WebGL TexImageSource pixels: source is a canvas or image node, or NULL
 * with bitmap an ImageBitmap token. Status as web_webgl_source_fn. */
int web_canvas_webgl_source(JSContext *ctx, node_t *source, JSValueConst bitmap, struct web_webgl_pixels *out);
void web_canvas_free(node_t *node);
void web_canvas_attr_changed(node_t *node, const char *name);
unsigned web_canvas_dimension(const node_t *node, const char *name);
/* Where the canvas's bitmap lands in its content box x, y, w x h under the
 * style's object-fit and object-position (as for images): fit = x, y, width,
 * height, which may reach past the box. false: the canvas or the box is empty. */
bool web_canvas_fit(const node_t *node, const style_t *st, int x, int y, int w, int h, int fit[4]);
/* The canvas's WebGL context (borrowed), NULL for 2D or none. */
struct web_webgl *web_canvas_webgl(const node_t *node);
/* Paints the bitmap fitted (web_canvas_fit) into the content box x, y, w x h. */
void web_canvas_paint(node_t *node, const style_t *st, canvas_t *dst, int x, int y, int w, int h);
