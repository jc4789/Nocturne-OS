/* DevTools inspection: read-only views of a live document for the browser's
   Elements, Styles, Computed and Layout panels. Nothing here runs author
   script or mutates the document. Node handles stay valid for the document's
   lifetime (nodes are owned by their document until web_free). */
#ifndef NOCTURNE_WEB_INSPECT_H
#define NOCTURNE_WEB_INSPECT_H

#include <stdbool.h>
#include <stddef.h>
#include "web.h"

enum { WEB_INSPECT_DOCUMENT, WEB_INSPECT_ELEMENT, WEB_INSPECT_TEXT, WEB_INSPECT_COMMENT,
       WEB_INSPECT_DOCTYPE, WEB_INSPECT_SHADOW_ROOT, WEB_INSPECT_OTHER };

/* Tree walk in DevTools order: an element's shadow root comes before its
   light children; whitespace-only text is skipped. */
web_node *web_inspect_root(web_doc *d);
web_node *web_inspect_first(web_node *n);
web_node *web_inspect_next(web_node *n);
web_node *web_inspect_parent(web_node *n);
int web_inspect_kind(web_node *n);
const char *web_inspect_name(web_node *n);  /* tag name as written; "" otherwise */
int web_inspect_attr_count(web_node *n);
bool web_inspect_attr(web_node *n, int i, const char **name, const char **value);
/* Text/comment data, or the shadow root mode ("open"/"closed"). */
const char *web_inspect_text(web_node *n, size_t *length);
bool web_inspect_contains(web_node *ancestor, web_node *n);

/* Document-space border box plus used margin/border/padding (top right
   bottom left), as the layout produced them. */
struct web_inspect_box { int x, y, w, h; float margin[4], border[4], padding[4]; };
bool web_inspect_box(web_doc *d, web_node *n, struct web_inspect_box *out);

/* Styles pane. Rules are in display order: element.style, then matched rules
   from highest to lowest cascade precedence, then inherited sections from
   the nearest ancestor outward (with only inherited properties). */
struct web_inspect_decl { char *name, *value; bool important, overridden; };
struct web_inspect_rule {
    char *selector, *source;   /* source: stylesheet URL, "<style>" or "user agent stylesheet" */
    web_node *inherited_from;  /* NULL: matches the element itself */
    struct web_inspect_decl *decls;
    int count;
    bool ua;
};
struct web_inspect_styles { struct web_inspect_rule *rules; int count; };
bool web_inspect_styles(web_doc *d, web_node *n, struct web_inspect_styles *out);
void web_inspect_styles_free(struct web_inspect_styles *styles);
/* "name: value\n" lines of every computed longhand. Caller frees. */
char *web_inspect_computed(web_doc *d, web_node *n);

#endif
