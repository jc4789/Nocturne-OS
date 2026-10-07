#pragma once
#include "webi.h"
#include "quickjs.h"
JSValue web_avmedia_call(JSContext *ctx, web_doc *doc, node_t *node, const char *operation,
                        int argc, JSValueConst *argv);
void web_avmedia_tick(web_doc *doc, uint64_t now);
int64_t web_avmedia_deadline(web_doc *doc, uint64_t now);
void web_avmedia_free(web_doc *doc);
bool web_avmedia_paint(web_doc *doc, node_t *node, canvas_t *canvas, int x, int y, int w, int h);
bool web_avmedia_size(node_t *node, int *width, int *height);
