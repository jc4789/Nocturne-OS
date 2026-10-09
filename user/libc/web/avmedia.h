#pragma once
#include "webi.h"
#include "quickjs.h"
JSValue web_avmedia_call(JSContext *ctx, web_doc *doc, node_t *node, const char *operation,
                        int argc, JSValueConst *argv);
void web_avmedia_tick(web_doc *doc, uint64_t now);
/* Native-only supply for existing MSE/Range children, globally throttled/fair
 * and non-reentrant. No child or URL open starts from an interrupt hook.
 * No script/event/DOM edit. Only a previously committed, independent video
 * snapshot may publish through the embedder's guarded native patch callback. */
void web_avmedia_service(uint64_t now);
/* Nestable guard for argument getters/conversions and other native half-state. */
void web_avmedia_enter(void);
void web_avmedia_leave(void);
int64_t web_avmedia_deadline(web_doc *doc, uint64_t now);
void web_avmedia_free(web_doc *doc);
/* Native process-owned child reap: valid even when no document exists. */
void web_avmedia_background(uint64_t now_ms);
int64_t web_avmedia_background_deadline(uint64_t now_ms);
bool web_avmedia_paint(web_doc *doc, node_t *node, canvas_t *canvas, int x, int y, int w, int h);
bool web_avmedia_size(node_t *node, int *width, int *height);
/* Called only within ordinary top-document paint. Probes are clipped to the
 * visible video span and never run from service/interrupts. */
void web_avmedia_snapshot_begin(web_doc *doc);
void web_avmedia_snapshot_reject(void);
bool web_avmedia_snapshot_prepare(web_doc *doc,canvas_t *committed,canvas_t *probe,
                                 int *x,int *y,int scroll_x,int scroll_y);
void web_avmedia_snapshot_probe(unsigned phase);
void web_avmedia_snapshot_finish(void);
