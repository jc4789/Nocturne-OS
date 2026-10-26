#pragma once
#include "webi.h"
#include "quickjs.h"
JSValue web_avmedia_call(JSContext *ctx, web_doc *doc, node_t *node, const char *operation,
                        int argc, JSValueConst *argv);
void web_avmedia_tick(web_doc *doc, uint64_t now);
/* Debug-only anonymous state transitions; disabled by default. */
void web_avmedia_debug(bool enabled);
/* Invalidate only this document's committed scene before partial layout.
 * Native media supply continues; no script, DOM edit, or allocation occurs. */
void web_avmedia_layout_begin(web_doc *doc);
/* Native-only supply for existing MSE/Range children, globally throttled/fair
 * and non-reentrant. No child or URL open starts from an interrupt hook.
 * No script/event/DOM edit. Only a previously committed, independent video
 * snapshot may publish through the embedder's guarded native patch callback. */
void web_avmedia_service(uint64_t now);
/* Cheap native safepoint: no uptime syscall without a serviceable registry. */
void web_avmedia_checkpoint(void);
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
/* The next media element owned by d that has native media state, after
 * previous (NULL starts); diagnostics read the registry, not every node. */
node_t *web_avmedia_node_next(web_doc *d, node_t *previous);
bool web_avmedia_pip_ready(web_doc *,node_t *);
bool web_avmedia_pip_paint(web_doc *,node_t *,canvas_t *);
/* Called only within ordinary top-document paint. Probes are clipped to the
 * visible video span and never run from service/interrupts. Snapshot capacity
 * follows that actual canvas span; 13 bytes/pixel are charged to the existing
 * media allocator. Invalid geometry, overflow or allocation failure uses
 * ordinary paint, without an independent fixed pixel ceiling. */
/* canvas is the window surface the document paints; a video painted into any
 * other surface (a transform's private raster) is not on screen where its
 * box is, so it cannot be patched there. */
void web_avmedia_snapshot_begin(web_doc *doc,const canvas_t *canvas);
void web_avmedia_snapshot_reject(void);
/* Bounded-effect layers reject only their actual clipped screen extent.
 * Probe coordinates are translated to the committed candidate surface. */
void web_avmedia_snapshot_reject_region_at(const canvas_t *,int,int,int,int,int line);
#define web_avmedia_snapshot_reject_region(c,x,y,w,h) web_avmedia_snapshot_reject_region_at(c,x,y,w,h,__LINE__)
/* Per-pixel linear compositing over the video (group opacity, a rounded clip
 * edge): the black/white probes then give each pixel's blend exactly enough
 * to recompose a new frame without paint. */
void web_avmedia_snapshot_blend_region(const canvas_t *,int,int,int,int);
bool web_avmedia_snapshot_prepare(web_doc *doc,canvas_t *committed,canvas_t *probe,
                                 int *x,int *y,int scroll_x,int scroll_y);
void web_avmedia_snapshot_probe(unsigned phase);
bool web_avmedia_snapshot_is_probe(void);
void web_avmedia_snapshot_finish(void);
