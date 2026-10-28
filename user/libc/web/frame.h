/* Native child browsing contexts. Windows are guarded by js.c; no JS DOM copy. */
#pragma once
#include "webi.h"
#include "sandbox.h"
struct web_frame {
    struct web_frame *next;
    web_doc *owner;
    node_t *element;
    node_t *window_token;
    web_doc *document;
    web_doc *retired;
    web_doc *navigation_owner;
    char *source, *srcdoc, *navigation, *navigation_origin, *sandbox;
    char *navigation_body, *navigation_type; /* the navigation's POST body and its Content-Type */
    size_t navigation_body_length;
    char *target_name, *name_attribute; /* navigable名と最後のHTML name属性を別所有 */
    uint64_t request;
    unsigned dom_order;
    bool notified, detached, failed, initial_failed;
    uint32_t pending_sandbox_flags;
    bool sandbox_initial;
    const char *commit_failure; /* static text: why web_frame_commit_response last refused */
    canvas_t paint;
    bool paint_failed;
    bool paint_visible;
    int paint_x,paint_y,viewport_w,viewport_h; /* crop offset; CSS viewport is unchanged */
    int scroll_x, scroll_y;
};
bool web_frame_element(const node_t *node);
struct web_frame *web_frame_walk_next(web_doc *root,struct web_frame *frame,bool descend);
node_t *web_frame_dom_next(node_t *root,node_t *node);
/* 1 ancestor match, 0 distinct, -1 allocation failure. */
int web_frame_ancestor_url(web_doc *parent,const char *url);
/* clip (left, top, right, bottom in root document space) is the part the
   paint covers; NULL means the root viewport. */
void web_frames_prepare_paint(web_doc *root,const double clip[4]);
void web_frames_commit_viewports(web_doc *document); /* committed parent content boxes, independent of paint */
void web_frames_finish_paint(web_doc *root);
struct web_frame *web_frame_find(web_doc *document, node_t *element);
struct web_frame *web_frame_ensure(web_doc *document, node_t *element);
void web_frame_retire(struct web_frame *frame);
void web_frames_detach_tree(web_doc *document,node_t *root);
bool web_frame_set_navigation(struct web_frame *frame,const char *url,web_doc *origin);
/* The request body (a form's POST entry list) and Content-Type of the navigation
   web_frame_set_navigation set last; NULL body: none (a GET). */
bool web_frame_set_navigation_body(struct web_frame *frame,const void *body,size_t length,const char *content_type);
/* HTML's rules for choosing a navigable, for a source document of top's tree and a target:
   the source (no target, _self), top (_top), the parent (_parent), or a frame of top's tree with
   that target name. NULL for _blank and for a name no frame of the page has, which another
   top-level traversable may have (the host's) or a new one takes. */
web_doc *web_frame_choose(web_doc *top,web_doc *source,const char *target);
bool web_frame_initial_blocked(const struct web_frame *frame,const char *source,const char *srcdoc);
bool web_frame_initial_create(struct web_frame *frame,const struct web_host *host);
/* A frame's document from markup the browser writes (about:blank, srcdoc,
   a text document): an HTML document. */
bool web_frame_commit(struct web_frame *frame, const char *html, size_t length,
                      const char *url, const char *charset, const struct web_host *host, bool inherited);
/* A frame's document from a response: its body, header block (NULL: none)
   and Content-Type (NULL: text/html), which picks the document's parser. */
bool web_frame_commit_response(struct web_frame *frame,const char *html,size_t length,
                      const char *url,const char *charset,const char *headers,const char *type,
                      const struct web_host *host,bool inherited);
web_doc *web_frames_detach(web_doc *document);
void web_frames_tick(web_doc *document, uint64_t now);
int64_t web_frames_deadline(web_doc *document);
bool web_frames_busy(web_doc *document);
void web_frames_loaded(web_doc *document, uint64_t id, const struct web_response *response);
void web_frames_socket_event(web_doc *document, uint64_t id, const struct web_socket_event *event);
bool web_frame_same_origin(web_doc *first, web_doc *second);
bool web_doc_fully_active(web_doc *document);
bool web_sandbox_document_allowed(struct web_frame *frame,const char *url,bool inherited);
bool web_sandbox_navigation_allowed(web_doc *source,web_doc *destination,bool trusted);
bool web_sandbox_forms_allowed(web_doc *document);
bool web_sandbox_auxiliary_allowed(web_doc *document);
bool web_frame_paint(node_t *element, canvas_t *canvas, int x, int y, int width, int height);
