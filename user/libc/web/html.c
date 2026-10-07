/* Native tag IDs and node helpers. HTML tokenization/tree building lives in html_lexbor.c. */
#include <stdio.h>
#include "webi.h"

const char *const tag_names[T_COUNT] = {
    "",
#define X(n) #n,
    WEB_TAGS(X)
#undef X
};

static int tag_order[T_COUNT]; /* tag ids sorted by name, for bsearch */
static bool tags_sorted;

static int cmp_tag(const void *a, const void *b) { return strcmp(tag_names[*(const int *)a], tag_names[*(const int *)b]); }

int tag_lookup(const char *name, size_t n) {
    if (!tags_sorted) {
        for (int i = 0; i < T_COUNT - 1; i++) tag_order[i] = i + 1;
        qsort(tag_order, T_COUNT - 1, sizeof(int), cmp_tag);
        tags_sorted = true;
    }
    char low[16];
    if (n >= sizeof low) return T_UNKNOWN;
    for (size_t i = 0; i < n; i++) low[i] = (char)lower((unsigned char)name[i]);
    name = low;
    int lo = 0, hi = T_COUNT - 2;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const char *t = tag_names[tag_order[mid]];
        int c = strncmp(name, t, n);
        if (c == 0) c = t[n] ? -1 : 0;
        if (c == 0) return tag_order[mid];
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return T_UNKNOWN;
}

/* ---------------------------------------------------------------- node helpers */
const char *node_attr(const node_t *n, const char *name) {
    for (int i = 0; i < n->nattrs; i++)
        if (!strcmp(n->attrs[i].name, name)) return n->attrs[i].value;
    return NULL;
}

bool node_has_class(const node_t *n, const char *cls) {
    for (int i = 0; i < n->nclasses; i++)
        if (!strcmp(n->classes[i], cls)) return true;
    return false;
}

node_t *node_ancestor(node_t *n, int tag) {
    for (; n; n = n->parent)
        if (n->type == N_ELEM && n->tag == tag) return n;
    return NULL;
}

void node_text_content(const node_t *n, sbuf *out) {
    if (n->type == N_TEXT) {
        sb_put(out, n->text, n->textlen);
        return;
    }
    for (node_t *c = n->first; c; c = c->next) node_text_content(c, out);
}
