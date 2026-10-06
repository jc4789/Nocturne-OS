/* Nocturne's web engine (libc/web/): HTML parsing, CSS, layout and painting. No JavaScript.

   The embedder does the networking. A typical page load:
     web_doc *d = web_parse(html, len, final_url, charset);
     while ((u = web_pending_stylesheet(d))) web_stylesheet_loaded(d, css_or_NULL, len);
     web_layout(d, width, height);              -> paint it
     for each i < web_image_count(d) with web_image_wanted(d, i):
         web_image_loaded(d, i, bytes_or_NULL, n); web_layout(...) again now and then
   Coordinates passed to and returned by these functions are document coordinates: (0, 0) is the
   top-left of the page, before scrolling. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "gfx.h"
#include "abi.h"

typedef struct web_doc web_doc;
typedef struct node web_node; /* an element of the document */

/* charset: from the Content-Type header, or NULL (a <meta charset> or UTF-8 is used) */
web_doc *web_parse(const char *html, size_t len, const char *url, const char *charset);
void web_free(web_doc *d);
const char *web_title(web_doc *d); /* "" if none */
const char *web_url(web_doc *d);
/* <meta http-equiv="refresh" content="N; url=...">: the absolute URL, or NULL */
const char *web_refresh_url(web_doc *d, int *delay_s);

/* resolve a (possibly relative) URL against base into out; false if it cannot be made absolute */
bool web_resolve_url(const char *base, const char *rel, char *out, size_t n);

/* ---- resources ---- */
const char *web_pending_stylesheet(web_doc *d);                   /* next stylesheet to fetch, or NULL */
void web_stylesheet_loaded(web_doc *d, const char *css, size_t n); /* css NULL: it failed */
int web_image_count(web_doc *d);
const char *web_image_url(web_doc *d, int i);
bool web_image_wanted(web_doc *d, int i); /* not loaded or failed yet */
void web_image_loaded(web_doc *d, int i, const void *data, size_t n); /* data NULL: it failed */

/* ---- layout and painting ---- */
/* lay the page out for a viewport; returns the document height */
int web_layout(web_doc *d, int width, int height);
int web_doc_height(web_doc *d);
/* paint the document rectangle at (doc_x, doc_y) into the canvas rectangle (x, y, w, h) */
void web_paint(web_doc *d, canvas_t *c, int x, int y, int w, int h, int doc_x, int doc_y);

/* ---- interaction ---- */
enum { WEB_HIT_NONE, WEB_HIT_LINK, WEB_HIT_TEXT_INPUT, WEB_HIT_CHECKBOX, WEB_HIT_RADIO, WEB_HIT_SUBMIT,
       WEB_HIT_BUTTON, WEB_HIT_SELECT, WEB_HIT_TEXTAREA };
struct web_hit {
    int kind;
    const char *href; /* absolute URL of the link under the point, or NULL */
    web_node *node;   /* the form control or link element */
};
bool web_hit_test(web_doc *d, int x, int y, struct web_hit *hit);
int web_anchor_y(web_doc *d, const char *fragment); /* y of the element with that id/name, or -1 */

/* form controls */
void web_focus(web_doc *d, web_node *n); /* NULL: nothing focused */
web_node *web_focused(web_doc *d);
/* a key for the focused text control: 0 ignored, 1 changed (repaint), 2 Enter: submit its form */
int web_key(web_doc *d, const struct gui_event *e);
void web_toggle(web_doc *d, web_node *n); /* checkbox or radio click */
/* the request a form submission makes: *url is malloc'd; *body is malloc'd for POST, NULL for GET.
   submitter is the clicked button, or any control of the form. */
bool web_submit(web_doc *d, web_node *submitter, char **url, char **body);
int web_select_options(web_doc *d, web_node *sel, const char **labels, int max, int *selected);
void web_select_set(web_doc *d, web_node *sel, int index);
/* document rectangle of an element (its first box), for placing popups */
bool web_node_rect(web_doc *d, web_node *n, int *x, int *y, int *w, int *h);

/* find in page: the next match at or after (doc) y = from_y (wrapping around); highlights it and
   returns its y, or -1. An empty string clears the highlight. */
int web_find(web_doc *d, const char *text, int from_y);
