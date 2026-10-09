/* Native child browsing contexts. Windows are guarded by js.c; no JS DOM copy. */
#pragma once
#include "webi.h"
#define WEB_FRAMES_MAX 32u
#define WEB_FRAME_DEPTH_MAX 8u
#define WEB_FRAME_NAVIGATIONS_MAX 128u
struct web_frame {
    struct web_frame *next;
    node_t *element;
    node_t *window_token;
    web_doc *document;
    web_doc *retired;
    web_doc *navigation_owner;
    char *source, *srcdoc, *navigation, *navigation_origin, *sandbox;
    uint64_t request;
    unsigned dom_order;
    bool notified, detached, failed, initial_failed;
    int scroll_x, scroll_y;
};
bool web_frame_element(const node_t *node);
struct web_frame *web_frame_find(web_doc *document, node_t *element);
struct web_frame *web_frame_ensure(web_doc *document, node_t *element);
void web_frame_retire(struct web_frame *frame);
void web_frames_detach_tree(web_doc *document,node_t *root);
bool web_frame_set_navigation(struct web_frame *frame,const char *url,web_doc *origin);
bool web_frame_initial_blocked(const struct web_frame *frame,const char *source,const char *srcdoc);
bool web_frame_initial_create(struct web_frame *frame,const struct web_host *host);
bool web_frame_commit(struct web_frame *frame, const char *html, size_t length,
                      const char *url, const char *charset, const struct web_host *host, bool inherited);
void web_frames_free(web_doc *document);
void web_frames_tick(web_doc *document, uint64_t now);
int64_t web_frames_deadline(web_doc *document);
bool web_frames_busy(web_doc *document);
void web_frames_loaded(web_doc *document, uint64_t id, const struct web_response *response);
bool web_frame_same_origin(web_doc *first, web_doc *second);
bool web_frame_paint(node_t *element, canvas_t *canvas, int x, int y, int width, int height);
