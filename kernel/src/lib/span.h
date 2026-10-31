/* Disjoint spans [start, start + length) of one number line (addresses), ordered by start in an AVL
   tree (lib/avl.h) whose every node knows the longest span under it, so that a span long enough is
   found in O(log n) steps, the lowest first. Intrusive: a span is part of its owner's record; the
   tree allocates nothing, and its owner guards it. */
#pragma once
#include <stdint.h>
#include "lib/avl.h"

struct span {
    struct avl_node node;
    uint64_t start, length;
    uint64_t longest; /* the longest length in its subtree, its own included */
};

struct span_tree {
    struct avl_tree tree;
};

/* s->start and s->length (not 0) set; s overlaps no span of t. */
void span_insert(struct span_tree *t, struct span *s);
void span_remove(struct span_tree *t, struct span *s);
/* The span that holds at; NULL when none does. */
struct span *span_at(const struct span_tree *t, uint64_t at);
/* The last span starting below at, and the first starting at at or above it; NULL when none. */
struct span *span_below(const struct span_tree *t, uint64_t at);
struct span *span_from(const struct span_tree *t, uint64_t at);
/* The lowest span at least length long; NULL when none is. */
struct span *span_fit(const struct span_tree *t, uint64_t length);
