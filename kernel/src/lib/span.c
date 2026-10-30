/* The span tree (lib/span.h): an AVL tree, so its height stays below 1.45 log2 n, and with it the
   recursion of insert and remove. */
#include "kernel.h"
#include "lib/span.h"

static int height(const struct span *s) { return s ? s->height : 0; }
static uint64_t longest(const struct span *s) { return s ? s->longest : 0; }

static void fix(struct span *s) {
    s->height = 1 + MAX(height(s->left), height(s->right));
    s->longest = MAX(s->length, MAX(longest(s->left), longest(s->right)));
}

static struct span *rotate_right(struct span *s) {
    struct span *l = s->left;
    s->left = l->right;
    l->right = s;
    fix(s);
    fix(l);
    return l;
}

static struct span *rotate_left(struct span *s) {
    struct span *r = s->right;
    s->right = r->left;
    r->left = s;
    fix(s);
    fix(r);
    return r;
}

/* s's subtrees are balanced and differ in height by at most 2: s balanced, its new root. */
static struct span *balance(struct span *s) {
    fix(s);
    int lean = height(s->left) - height(s->right);
    if (lean > 1) {
        if (height(s->left->left) < height(s->left->right)) s->left = rotate_left(s->left);
        return rotate_right(s);
    }
    if (lean < -1) {
        if (height(s->right->right) < height(s->right->left)) s->right = rotate_right(s->right);
        return rotate_left(s);
    }
    return s;
}

static struct span *put(struct span *n, struct span *s) {
    if (!n) return s;
    if (s->start < n->start) n->left = put(n->left, s);
    else n->right = put(n->right, s);
    return balance(n);
}

void span_insert(struct span_tree *t, struct span *s) {
    ASSERT(s->length);
    s->left = s->right = NULL;
    s->height = 1;
    s->longest = s->length;
    t->root = put(t->root, s);
}

/* n's subtree without its lowest span, which *lowest receives. */
static struct span *take_lowest(struct span *n, struct span **lowest) {
    if (!n->left) {
        *lowest = n;
        return n->right;
    }
    n->left = take_lowest(n->left, lowest);
    return balance(n);
}

static struct span *cut(struct span *n, struct span *s) {
    ASSERT(n); /* s is in the tree */
    if (n == s) {
        if (!n->left) return n->right;
        if (!n->right) return n->left;
        struct span *next;
        struct span *right = take_lowest(n->right, &next);
        next->left = n->left;
        next->right = right;
        return balance(next);
    }
    if (s->start < n->start) n->left = cut(n->left, s);
    else n->right = cut(n->right, s);
    return balance(n);
}

void span_remove(struct span_tree *t, struct span *s) { t->root = cut(t->root, s); }

struct span *span_at(const struct span_tree *t, uint64_t at) {
    for (struct span *n = t->root; n;) {
        if (at < n->start) n = n->left;
        else if (at - n->start >= n->length) n = n->right;
        else return n;
    }
    return NULL;
}

struct span *span_below(const struct span_tree *t, uint64_t at) {
    struct span *found = NULL;
    for (struct span *n = t->root; n;) {
        if (n->start < at) {
            found = n;
            n = n->right;
        } else {
            n = n->left;
        }
    }
    return found;
}

struct span *span_from(const struct span_tree *t, uint64_t at) {
    struct span *found = NULL;
    for (struct span *n = t->root; n;) {
        if (n->start >= at) {
            found = n;
            n = n->left;
        } else {
            n = n->right;
        }
    }
    return found;
}

/* The longest of each subtree is exact: where the lower side holds a fit, the lowest is there. */
struct span *span_fit(const struct span_tree *t, uint64_t length) {
    struct span *n = t->root;
    if (longest(n) < length) return NULL;
    for (;;) {
        if (longest(n->left) >= length) n = n->left;
        else if (n->length >= length) return n;
        else n = n->right;
    }
}
