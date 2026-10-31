/* The AVL tree (lib/avl.h). */
#include "kernel.h"
#include "lib/avl.h"

static int height(const struct avl_node *n) { return n ? n->height : 0; }

static void fix(const struct avl_order *o, struct avl_node *n) {
    n->height = 1 + MAX(height(n->left), height(n->right));
    if (o->fix) o->fix(n);
}

static struct avl_node *rotate_right(const struct avl_order *o, struct avl_node *n) {
    struct avl_node *l = n->left;
    n->left = l->right;
    l->right = n;
    fix(o, n);
    fix(o, l);
    return l;
}

static struct avl_node *rotate_left(const struct avl_order *o, struct avl_node *n) {
    struct avl_node *r = n->right;
    n->right = r->left;
    r->left = n;
    fix(o, n);
    fix(o, r);
    return r;
}

/* n's subtrees are balanced and differ in height by at most 2: n balanced, its new root. */
static struct avl_node *balance(const struct avl_order *o, struct avl_node *n) {
    fix(o, n);
    int lean = height(n->left) - height(n->right);
    if (lean > 1) {
        if (height(n->left->left) < height(n->left->right)) n->left = rotate_left(o, n->left);
        return rotate_right(o, n);
    }
    if (lean < -1) {
        if (height(n->right->right) < height(n->right->left)) n->right = rotate_right(o, n->right);
        return rotate_left(o, n);
    }
    return n;
}

static struct avl_node *put(const struct avl_order *o, struct avl_node *at, struct avl_node *n) {
    if (!at) return n;
    if (o->before(n, at)) at->left = put(o, at->left, n);
    else at->right = put(o, at->right, n);
    return balance(o, at);
}

void avl_insert(struct avl_tree *t, const struct avl_order *o, struct avl_node *n) {
    n->left = n->right = NULL;
    fix(o, n);
    t->root = put(o, t->root, n);
}

/* at's subtree without its first node, which *first receives. */
static struct avl_node *take_first(const struct avl_order *o, struct avl_node *at, struct avl_node **first) {
    if (!at->left) {
        *first = at;
        return at->right;
    }
    at->left = take_first(o, at->left, first);
    return balance(o, at);
}

static struct avl_node *cut(const struct avl_order *o, struct avl_node *at, struct avl_node *n) {
    ASSERT(at); /* n is in the tree */
    if (at == n) {
        if (!at->left) return at->right;
        if (!at->right) return at->left;
        struct avl_node *next;
        struct avl_node *right = take_first(o, at->right, &next);
        next->left = at->left;
        next->right = right;
        return balance(o, next);
    }
    if (o->before(n, at)) at->left = cut(o, at->left, n);
    else at->right = cut(o, at->right, n);
    return balance(o, at);
}

void avl_remove(struct avl_tree *t, const struct avl_order *o, struct avl_node *n) {
    t->root = cut(o, t->root, n);
    n->left = n->right = NULL;
}

struct avl_node *avl_first(const struct avl_tree *t) {
    struct avl_node *n = t->root;
    if (n)
        while (n->left) n = n->left;
    return n;
}
