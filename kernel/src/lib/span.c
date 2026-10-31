/* The span tree (lib/span.h), on the AVL tree (lib/avl.h). */
#include "kernel.h"
#include "lib/span.h"

#define SPAN(n) ((struct span *)((char *)(n) - __builtin_offsetof(struct span, node)))

static struct span *of(const struct avl_node *n) { return n ? SPAN(n) : NULL; }
static uint64_t longest(const struct avl_node *n) { return n ? SPAN(n)->longest : 0; }

static bool before(const struct avl_node *a, const struct avl_node *b) { return SPAN(a)->start < SPAN(b)->start; }

static void fix(struct avl_node *n) {
    struct span *s = SPAN(n);
    s->longest = MAX(s->length, MAX(longest(n->left), longest(n->right)));
}

static const struct avl_order order = {.before = before, .fix = fix};

void span_insert(struct span_tree *t, struct span *s) {
    ASSERT(s->length);
    avl_insert(&t->tree, &order, &s->node);
}

void span_remove(struct span_tree *t, struct span *s) { avl_remove(&t->tree, &order, &s->node); }

struct span *span_at(const struct span_tree *t, uint64_t at) {
    for (const struct avl_node *n = t->tree.root; n;) {
        struct span *s = SPAN(n);
        if (at < s->start) n = n->left;
        else if (at - s->start >= s->length) n = n->right;
        else return s;
    }
    return NULL;
}

struct span *span_below(const struct span_tree *t, uint64_t at) {
    const struct avl_node *found = NULL;
    for (const struct avl_node *n = t->tree.root; n;) {
        if (SPAN(n)->start < at) {
            found = n;
            n = n->right;
        } else {
            n = n->left;
        }
    }
    return of(found);
}

struct span *span_from(const struct span_tree *t, uint64_t at) {
    const struct avl_node *found = NULL;
    for (const struct avl_node *n = t->tree.root; n;) {
        if (SPAN(n)->start >= at) {
            found = n;
            n = n->left;
        } else {
            n = n->right;
        }
    }
    return of(found);
}

/* The longest of each subtree is exact: where the lower side holds a fit, the lowest is there. */
struct span *span_fit(const struct span_tree *t, uint64_t length) {
    const struct avl_node *n = t->tree.root;
    if (longest(n) < length) return NULL;
    for (;;) {
        if (longest(n->left) >= length) n = n->left;
        else if (SPAN(n)->length >= length) return SPAN(n);
        else n = n->right;
    }
}
