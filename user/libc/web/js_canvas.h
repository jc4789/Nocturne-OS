#pragma once
#include "webi.h"
#include "quickjs.h"
/* Called with a brand-checked native DOM node; argv begins with operation. */
JSValue web_canvas_native(JSContext *ctx, node_t *node, int argc, JSValueConst *argv);
void web_canvas_free(node_t *node);
void web_canvas_attr_changed(node_t *node, const char *name);
unsigned web_canvas_dimension(const node_t *node, const char *name);
void web_canvas_paint(node_t *node, canvas_t *dst, int x, int y, int w, int h);
