/* DevTools inspection internals shared by the cascade, the JS realm and
   inspect.c. Read-only: nothing here restyles, relayouts for the inspector's
   own sake, or runs author script. The public half is web_inspect.h. */
#ifndef NOCTURNE_WEB_INSPECT_PRIVATE_H
#define NOCTURNE_WEB_INSPECT_PRIVATE_H

#include "webi.h"

/* One matched style rule, then its declarations, in cascade order. A false
   return from either callback stops the walk (allocation failure). */
struct css_inspect_sink {
    void *ctx;
    bool (*rule)(void *ctx, const char *selector, const char *href, bool ua,
                 uint32_t specificity, uint32_t order);
    bool (*decl)(void *ctx, const char *name, const char *value, bool important,
                 bool inherited);
};
/* The element's style rules whose media conditions hold at the indexed
   viewport, from the same parser AST and selector matcher as the cascade. */
bool css_inspect_rules(web_doc *d, node_t *e, const struct css_inspect_sink *sink);
/* element.style: the immutable inline declaration snapshot. */
bool css_inspect_inline(web_doc *d, node_t *e, const struct css_inspect_sink *sink);
/* The stylesheet's request URL (NULL: an inline <style> or constructed sheet). */
void css_sheet_href(sheet_t *sheet, const char *href);
bool css_prop_inherited(const struct propdef *p);
/* "name: value\n" for every computed longhand, as getComputedStyle reports
   them. Caller frees. NULL when the element is not rendered in a realm. */
char *web_js_inspect_computed(web_doc *d, node_t *n);

#endif
