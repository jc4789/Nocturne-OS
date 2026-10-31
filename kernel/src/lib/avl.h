/* An AVL tree of nodes inside their owners' records (intrusive: it allocates nothing), so that its
   height stays below 1.45 log2 n, and with it every walk and the recursion of insert and remove.
   The owner orders it (before: a strict total order, no two nodes of the tree equal under it, which
   stays as it was for a node while the node is in the tree) and may keep in each node a summary of
   its subtree (fix: from the node's own fields and its children's summaries, the longest span under
   it, the earliest deadline...), which the tree keeps right through every rotation. The owner walks
   the tree itself for its own questions (left, right) and guards it. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

struct avl_node {
    struct avl_node *left, *right;
    int height;
};

struct avl_tree {
    struct avl_node *root;
};

struct avl_order {
    bool (*before)(const struct avl_node *a, const struct avl_node *b);
    void (*fix)(struct avl_node *n); /* NULL when the owner keeps no summary */
};

void avl_insert(struct avl_tree *t, const struct avl_order *o, struct avl_node *n);
void avl_remove(struct avl_tree *t, const struct avl_order *o, struct avl_node *n); /* n is in t */
struct avl_node *avl_first(const struct avl_tree *t); /* NULL when t is empty */
