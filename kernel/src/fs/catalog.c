/* The catalog's B+tree (fs/catalog.h). */
#include "fs/catalog.h"
#include "fs/space.h"
#include "mm/heap.h"

#define LEVELS 16
#define HEADER sizeof(struct store_node)
#define ITEM_MAX (4 + STORE_KEY_MAX + STORE_VALUE_MAX)

static struct store_node *node(struct cblock *b) { return (struct store_node *)b->data; }
static uint8_t *item(struct store_node *n, unsigned i) { return (uint8_t *)n + n->slot[i]; }
static uint16_t klen_of(const uint8_t *it) { return ((const uint16_t *)it)[0]; }
static uint16_t vlen_of(const uint8_t *it) { return ((const uint16_t *)it)[1]; }
static uint8_t *key_of(uint8_t *it) { return it + 4; }
static uint8_t *value_of(uint8_t *it) { return it + 4 + klen_of(it); }
static size_t item_size(const uint8_t *it) { return 4 + (size_t)klen_of(it) + vlen_of(it); }
static size_t room(struct store_node *n) { return n->low - (HEADER + 2 * (size_t)n->count); }
static size_t used(struct store_node *n) { return STORE_BLOCK - room(n); }

static int compare(const uint8_t *a, size_t an, const uint8_t *b, size_t bn) {
    int c = memcmp(a, b, MIN(an, bn));
    return c ? c : an < bn ? -1 : an > bn;
}

/* the first item whose key is not below key; *exact when it is key */
static unsigned search(struct store_node *n, const uint8_t *key, size_t klen, bool *exact) {
    unsigned lo = 0, hi = n->count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        uint8_t *it = item(n, mid);
        if (compare(key_of(it), klen_of(it), key, klen) < 0) lo = mid + 1;
        else hi = mid;
    }
    *exact = false;
    if (lo < n->count) {
        uint8_t *it = item(n, lo);
        *exact = !compare(key_of(it), klen_of(it), key, klen);
    }
    return lo;
}

/* the child of a branch whose keys hold key: the last item from the second on whose key is not above
   key, else the first. The first item's key bounds nothing (common/store_format.h): when its child
   leaves, the next one's key takes its place, and keys below it then go into that child, so it can
   be above keys of its own child and above the second item's key once that child splits. */
static unsigned child_index(struct store_node *n, const uint8_t *key, size_t klen) {
    unsigned lo = 1, hi = n->count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        uint8_t *it = item(n, mid);
        if (compare(key_of(it), klen_of(it), key, klen) <= 0) lo = mid + 1;
        else hi = mid;
    }
    return lo - 1;
}

static struct store_child child_of(struct store_node *n, unsigned i) {
    struct store_child c;
    memcpy(&c, value_of(item(n, i)), sizeof c);
    return c;
}

static void set_child(struct store_node *n, unsigned i, uint64_t address, uint64_t generation) {
    struct store_child c = {address, generation};
    memcpy(value_of(item(n, i)), &c, sizeof c);
}

static void put_item(struct store_node *n, unsigned at, const uint8_t *key, size_t klen, const void *value, size_t vlen) {
    size_t size = 4 + klen + vlen;
    n->low -= (uint16_t)size;
    uint8_t *it = (uint8_t *)n + n->low;
    ((uint16_t *)it)[0] = (uint16_t)klen;
    ((uint16_t *)it)[1] = (uint16_t)vlen;
    memcpy(it + 4, key, klen);
    memcpy(it + 4 + klen, value, vlen);
    memmove(&n->slot[at + 1], &n->slot[at], (n->count - at) * sizeof n->slot[0]);
    n->slot[at] = n->low;
    n->count++;
}

static void remove_item(struct store_node *n, unsigned at) {
    uint16_t off = n->slot[at];
    size_t size = item_size((uint8_t *)n + off);
    memmove((uint8_t *)n + n->low + size, (uint8_t *)n + n->low, off - n->low);
    memmove(&n->slot[at], &n->slot[at + 1], (n->count - at - 1) * sizeof n->slot[0]);
    n->count--;
    for (unsigned i = 0; i < n->count; i++)
        if (n->slot[i] < off) n->slot[i] += (uint16_t)size;
    n->low += (uint16_t)size;
}

static bool node_valid(struct volume *v, const uint8_t *data, uint64_t address, uint64_t generation) {
    if (!head_valid(v, data, STORE_MAGIC_NODE, address, generation)) return false;
    const struct store_node *n = (const void *)data;
    if (n->level >= LEVELS || n->low > STORE_BLOCK || HEADER + 2 * (size_t)n->count > n->low) return false;
    for (unsigned i = 0; i < n->count; i++) {
        if (n->slot[i] < n->low || n->slot[i] + 4u > STORE_BLOCK) return false;
        const uint8_t *it = data + n->slot[i];
        if (n->slot[i] + item_size(it) > STORE_BLOCK || klen_of(it) > STORE_KEY_MAX || vlen_of(it) > STORE_VALUE_MAX) return false;
        if (n->level && vlen_of(it) != sizeof(struct store_child)) return false;
    }
    return true;
}

static struct cblock *node_read(struct volume *v, uint64_t address, uint64_t generation, int *error) {
    if (address & TEMP_ADDRESS) {
        struct cblock *b = cache_find(v, address);
        if (!b) panic("store: catalog node %lx is not in memory", (unsigned long)address);
        return b;
    }
    struct cblock *b = cache_read(v, address, node_valid, generation, error);
    if (!b) kprintf("store: catalog node %lu cannot be read (%d)\n", (unsigned long)address, *error);
    return b;
}

static struct cblock *node_new(struct volume *v, uint16_t level, int *error) {
    struct cblock *b = cache_new(v, v->next_temp++, error);
    if (!b) return NULL;
    struct store_node *n = node(b);
    n->head.magic = STORE_MAGIC_NODE;
    n->head.address = b->address;
    n->head.generation = v->generation;
    n->level = level;
    n->low = STORE_BLOCK;
    v->changed = true;
    return b;
}

/* the way from the root to a leaf */
struct trail {
    struct cblock *b[LEVELS];
    unsigned at[LEVELS]; /* the child taken, or the item */
    int depth;           /* b[depth] is the leaf */
};

static void path_put(struct volume *v, struct trail *p) {
    for (int i = 0; i <= p->depth; i++) cache_put(v, p->b[i]);
    p->depth = -1;
}

/* a node of the path into the open generation: copied the first time, its parent pointed at it */
static int cow(struct volume *v, struct trail *p, int d) {
    struct cblock *old = p->b[d];
    if (node(old)->head.generation == v->generation) return 0;
    int error = 0;
    struct cblock *b = cache_new(v, v->next_temp++, &error);
    if (!b) return error;
    memcpy(b->data, old->data, STORE_BLOCK);
    node(b)->head.address = b->address;
    node(b)->head.generation = v->generation;
    int r = space_let_go(v, old->address, 1);
    if (r < 0) {
        cache_discard(v, b);
        return r;
    }
    if (d) set_child(node(p->b[d - 1]), p->at[d - 1], b->address, v->generation);
    else v->root = b->address, v->root_generation = v->generation;
    cache_put(v, old);
    p->b[d] = b;
    v->changed = true;
    return 0;
}

/* down to the leaf for key; writable: every node on the way into the open generation */
static int descend(struct volume *v, const uint8_t *key, size_t klen, bool writable, struct trail *p) {
    int error = 0;
    p->depth = -1;
    uint64_t address = v->root, generation = v->root_generation;
    for (int d = 0;; d++) {
        if (d == LEVELS) {
            path_put(v, p);
            return -N_ERR_IO;
        }
        struct cblock *b = node_read(v, address, generation, &error);
        if (!b) {
            path_put(v, p);
            return error;
        }
        p->b[d] = b;
        p->depth = d;
        if (d && node(b)->level + 1 != node(p->b[d - 1])->level) {
            path_put(v, p);
            return -N_ERR_IO;
        }
        if (writable && (error = cow(v, p, d)) < 0) {
            path_put(v, p);
            return error;
        }
        struct store_node *n = node(p->b[d]);
        if (!n->level) return 0;
        if (!n->count) {
            path_put(v, p);
            return -N_ERR_IO;
        }
        p->at[d] = child_index(n, key, klen);
        struct store_child c = child_of(n, p->at[d]);
        address = c.address;
        generation = c.generation;
    }
}

/* leftmost (or rightmost) from the node at depth d down to a leaf */
static int edge_down(struct volume *v, struct trail *p, int d, bool right) {
    int error = 0;
    while (node(p->b[d])->level) {
        struct store_node *n = node(p->b[d]);
        if (!n->count || d + 1 == LEVELS) return -N_ERR_IO;
        p->at[d] = right ? n->count - 1u : 0;
        struct store_child c = child_of(n, p->at[d]);
        struct cblock *b = node_read(v, c.address, c.generation, &error);
        if (!b) return error;
        p->b[++d] = b;
        p->depth = d;
    }
    return 0;
}

/* the leaf after (or before) the path's: false at the tree's end */
static int step_leaf(struct volume *v, struct trail *p, bool back) {
    int d = p->depth - 1;
    while (d >= 0 && (back ? p->at[d] == 0 : p->at[d] + 1u >= node(p->b[d])->count)) d--;
    if (d < 0) return -N_ERR_NOT_FOUND;
    for (int i = p->depth; i > d; i--) cache_put(v, p->b[i]);
    p->depth = d;
    if (back) p->at[d]--;
    else p->at[d]++;
    struct store_child c = child_of(node(p->b[d]), p->at[d]);
    int error = 0;
    struct cblock *b = node_read(v, c.address, c.generation, &error);
    if (!b) return error;
    p->b[++d] = b;
    p->depth = d;
    return edge_down(v, p, d, back);
}

static void copy_out(uint8_t *it, struct item *out) {
    out->klen = klen_of(it);
    out->vlen = vlen_of(it);
    memcpy(out->key, key_of(it), out->klen);
    memcpy(out->value, value_of(it), out->vlen);
}

int catalog_get(struct volume *v, const uint8_t *key, size_t klen, void *value, size_t cap) {
    struct trail p;
    int r = descend(v, key, klen, false, &p);
    if (r < 0) return r;
    bool exact;
    struct store_node *n = node(p.b[p.depth]);
    unsigned i = search(n, key, klen, &exact);
    r = -N_ERR_NOT_FOUND;
    if (exact) {
        uint8_t *it = item(n, i);
        memcpy(value, value_of(it), MIN(cap, (size_t)vlen_of(it)));
        r = vlen_of(it);
    }
    path_put(v, &p);
    return r;
}

int catalog_next(struct volume *v, const uint8_t *key, size_t klen, bool equal, const uint8_t *limit, size_t llen,
                 struct item *out) {
    struct trail p;
    int r = descend(v, key, klen, false, &p);
    if (r < 0) return r;
    bool exact;
    unsigned i = search(node(p.b[p.depth]), key, klen, &exact);
    if (exact && !equal) i++;
    while (i >= node(p.b[p.depth])->count) {
        if ((r = step_leaf(v, &p, false)) < 0) {
            path_put(v, &p);
            return r;
        }
        i = 0;
    }
    uint8_t *it = item(node(p.b[p.depth]), i);
    r = compare(key_of(it), klen_of(it), limit, llen) < 0 ? 0 : -N_ERR_NOT_FOUND;
    if (!r) copy_out(it, out);
    path_put(v, &p);
    return r;
}

int catalog_last(struct volume *v, const uint8_t *key, size_t klen, const uint8_t *floor, size_t flen, struct item *out) {
    struct trail p;
    int r = descend(v, key, klen, false, &p);
    if (r < 0) return r;
    bool exact;
    unsigned i = search(node(p.b[p.depth]), key, klen, &exact);
    while (!exact && i == 0) { /* everything in this leaf is above key */
        if ((r = step_leaf(v, &p, true)) < 0) {
            path_put(v, &p);
            return r;
        }
        i = node(p.b[p.depth])->count;
    }
    if (!exact) i--;
    uint8_t *it = item(node(p.b[p.depth]), i);
    r = compare(key_of(it), klen_of(it), floor, flen) >= 0 ? 0 : -N_ERR_NOT_FOUND;
    if (!r) copy_out(it, out);
    path_put(v, &p);
    return r;
}

/* ---- changes ---- */

/* What a change may split into, made before it changes anything, so that it cannot fail half way:
   a right half for each node that splits, a root over the old one when that splits too, and a
   block to split from. */
struct spare {
    struct cblock *b[LEVELS + 1];
    unsigned n;
    uint8_t *copy;
};

/* how many nodes an item of size bytes into the path's leaf may need; freed: the room an item it
   replaces there leaves */
static unsigned spare_count(struct trail *p, size_t size, size_t freed) {
    unsigned k = 0;
    for (int d = p->depth; d >= 0; d--, freed = 0) {
        if (room(node(p->b[d])) + freed >= size) return k;
        k++;
        size = 6 + STORE_KEY_MAX + sizeof(struct store_child); /* the separator into its parent, at most */
    }
    return k + 1;
}

/* k spare nodes; spare_put in every case */
static int spare_get(struct volume *v, struct spare *s, unsigned k) {
    s->n = 0;
    s->copy = k ? kmalloc(STORE_BLOCK) : NULL;
    int error = k && !s->copy ? -N_ERR_NO_MEMORY : 0;
    while (!error && s->n < k) {
        struct cblock *b = node_new(v, 0, &error);
        if (b) s->b[s->n++] = b;
    }
    return error;
}

static struct cblock *spare_take(struct spare *s, uint16_t level) {
    if (!s->n) panic("store: a catalog change has no node made for its split");
    struct cblock *b = s->b[--s->n];
    node(b)->level = level;
    return b;
}

/* the spare nodes the change did not take go */
static void spare_put(struct volume *v, struct spare *s) {
    while (s->n) cache_discard(v, s->b[--s->n]);
    kfree(s->copy);
}

/* n's items with one more at `at` (key, value), shared out over n (the left half) and right (the
   right half, a new node of n's level), by way of copy */
static void split(struct store_node *n, struct store_node *right, uint8_t *copy, unsigned at, const uint8_t *key,
                  size_t klen, const void *value, size_t vlen) {
    memcpy(copy, n, STORE_BLOCK);
    struct store_node *old = (void *)copy;
    unsigned total = old->count + 1u;
    size_t bytes = 0, all = used(old) + 6 + klen + vlen;
    unsigned cut = 0;
    /* the first items up to half the bytes stay */
    for (unsigned i = 0; i < total && cut == 0; i++) {
        size_t size = 2 + (i == at ? 4 + klen + vlen : item_size(item(old, i < at ? i : i - 1)));
        if (bytes + size > all / 2 && i) cut = i;
        bytes += size;
    }
    if (!cut) cut = total - 1;
    n->count = 0;
    n->low = STORE_BLOCK;
    for (unsigned i = 0; i < total; i++) {
        struct store_node *to = i < cut ? n : right;
        if (i == at) put_item(to, to->count, key, klen, value, vlen);
        else {
            uint8_t *it = item(old, i < at ? i : i - 1);
            put_item(to, to->count, key_of(it), klen_of(it), value_of(it), vlen_of(it));
        }
    }
}

/* item (key, value) into the node at depth d of the path, at its place `at`; splits upwards into
   the spare nodes */
static void place_item(struct volume *v, struct trail *p, int d, unsigned at, const uint8_t *key, size_t klen,
                       const void *value, size_t vlen, struct spare *s) {
    struct store_node *n = node(p->b[d]);
    if (room(n) >= 6 + klen + vlen) {
        put_item(n, at, key, klen, value, vlen);
        return;
    }
    struct cblock *rb = spare_take(s, n->level);
    struct store_node *right = node(rb);
    split(n, right, s->copy, at, key, klen, value, vlen);
    uint8_t *first = item(right, 0);
    uint8_t sep[STORE_KEY_MAX];
    size_t slen = klen_of(first);
    memcpy(sep, key_of(first), slen);
    struct store_child c = {rb->address, v->generation};
    cache_put(v, rb); /* a dirty node stays in memory without a reference */
    if (d == 0) { /* a new root over the two halves */
        struct cblock *root = spare_take(s, (uint16_t)(n->level + 1));
        uint8_t *lfirst = item(n, 0);
        struct store_child l = {p->b[0]->address, v->generation};
        put_item(node(root), 0, key_of(lfirst), klen_of(lfirst), &l, sizeof l);
        put_item(node(root), 1, sep, slen, &c, sizeof c);
        v->root = root->address;
        v->root_generation = v->generation;
        cache_put(v, root);
        return;
    }
    place_item(v, p, d - 1, p->at[d - 1] + 1, sep, slen, &c, sizeof c, s);
}

static int change(struct volume *v, const uint8_t *key, size_t klen, const void *value, size_t vlen, bool replace) {
    if (klen > STORE_KEY_MAX || vlen > STORE_VALUE_MAX) return -N_ERR_TOO_LARGE;
    int r = volume_writable(v);
    if (r < 0) return r;
    struct trail p;
    if ((r = descend(v, key, klen, true, &p)) < 0) return r;
    struct store_node *n = node(p.b[p.depth]);
    bool exact;
    unsigned at = search(n, key, klen, &exact);
    if (exact && !replace) {
        path_put(v, &p);
        return -N_ERR_EXISTS;
    }
    struct spare s;
    if ((r = spare_get(v, &s, spare_count(&p, 6 + klen + vlen, exact ? 2 + item_size(item(n, at)) : 0))) >= 0) {
        if (exact) remove_item(n, at);
        place_item(v, &p, p.depth, at, key, klen, value, vlen, &s);
    }
    spare_put(v, &s);
    path_put(v, &p);
    return r;
}

int catalog_put(struct volume *v, const uint8_t *key, size_t klen, const void *value, size_t vlen) {
    return change(v, key, klen, value, vlen, true);
}

int catalog_insert(struct volume *v, const uint8_t *key, size_t klen, const void *value, size_t vlen) {
    return change(v, key, klen, value, vlen, false);
}

int catalog_ready(struct volume *v, const uint8_t *key, size_t klen) {
    int r = volume_writable(v);
    if (r < 0) return r;
    struct trail p;
    if ((r = descend(v, key, klen, true, &p)) < 0) return r;
    path_put(v, &p);
    return 0;
}

/* a node out of the tree: its block (copied into this generation) was already let go */
static void drop_node(struct volume *v, struct trail *p, int d) {
    cache_discard(v, p->b[d]);
    p->b[d] = NULL;
}

int catalog_delete(struct volume *v, const uint8_t *key, size_t klen) {
    int r = volume_writable(v);
    if (r < 0) return r;
    struct trail p;
    if ((r = descend(v, key, klen, true, &p)) < 0) return r;
    struct store_node *n = node(p.b[p.depth]);
    bool exact;
    unsigned at = search(n, key, klen, &exact);
    if (!exact) {
        path_put(v, &p);
        return -N_ERR_NOT_FOUND;
    }
    remove_item(n, at);
    /* empty nodes leave their parents; small ones join a neighbour when both fit in one */
    int d = p.depth;
    for (; d > 0; d--) {
        n = node(p.b[d]);
        struct store_node *parent = node(p.b[d - 1]);
        if (!n->count) {
            remove_item(parent, p.at[d - 1]);
            drop_node(v, &p, d);
            continue;
        }
        if (used(n) >= STORE_BLOCK / 4 || parent->count < 2) break;
        unsigned left = p.at[d - 1] + 1u < parent->count ? p.at[d - 1] : p.at[d - 1] - 1u;
        unsigned other = left == p.at[d - 1] ? left + 1 : left;
        struct store_child c = child_of(parent, other);
        int error = 0;
        struct cblock *ob = node_read(v, c.address, c.generation, &error);
        if (!ob) break; /* left as it is: still a valid tree */
        struct store_node *o = node(ob);
        if (used(o) + used(n) - HEADER > STORE_BLOCK || o->level != n->level) {
            cache_put(v, ob);
            break;
        }
        /* the right one's items go to the left one, which comes into this generation */
        struct trail q = {.depth = d};
        for (int i = 0; i < d; i++) q.b[i] = p.b[i], q.at[i] = p.at[i];
        q.at[d - 1] = left;
        struct cblock *lb = left == p.at[d - 1] ? p.b[d] : ob, *rb = left == p.at[d - 1] ? ob : p.b[d];
        q.b[d] = lb;
        /* a block of an earlier generation is let go as a copy lets it go: the left one's by its
           copy, the right one's before its items leave it */
        if (lb == ob ? (error = cow(v, &q, d)) < 0
                     : !(ob->address & TEMP_ADDRESS) && (error = space_let_go(v, ob->address, 1)) < 0) {
            cache_put(v, ob);
            break;
        }
        lb = q.b[d];
        struct store_node *l = node(lb), *rn = node(rb);
        for (unsigned i = 0; i < rn->count; i++) {
            uint8_t *it = item(rn, i);
            put_item(l, l->count, key_of(it), klen_of(it), value_of(it), vlen_of(it));
        }
        remove_item(parent, left + 1);
        if (rb == ob && (ob->address & TEMP_ADDRESS)) cache_discard(v, ob);
        else if (rb == ob) cache_put(v, ob);
        else drop_node(v, &p, d);
        p.b[d] = lb;
        p.at[d - 1] = left;
    }
    /* a root with one child gives way to it */
    while (p.b[0] && node(p.b[0])->level && node(p.b[0])->count == 1) {
        struct store_child c = child_of(node(p.b[0]), 0);
        drop_node(v, &p, 0);
        v->root = c.address;
        v->root_generation = c.generation;
        int error = 0;
        struct cblock *b = node_read(v, c.address, c.generation, &error);
        if (!b) break;
        p.b[0] = b;
        for (int i = 1; i <= p.depth; i++) {
            if (p.b[i]) cache_put(v, p.b[i]);
            p.b[i] = NULL;
        }
        p.depth = 0;
    }
    if (p.b[0] && node(p.b[0])->level && !node(p.b[0])->count) { /* everything went */
        int error = 0;
        struct cblock *leaf = node_new(v, 0, &error);
        if (leaf) {
            drop_node(v, &p, 0);
            v->root = leaf->address;
            v->root_generation = v->generation;
            cache_put(v, leaf);
        }
    }
    for (int i = 0; i <= p.depth; i++)
        if (p.b[i]) cache_put(v, p.b[i]);
    v->changed = true;
    return 0;
}

/* ---- the whole tree ---- */

/* the node at address and what is under it; its keys from lo on and below hi (NULL: no bound) */
static int walk(struct volume *v, uint64_t address, uint64_t generation, int level, const uint8_t *lo, size_t lolen,
                const uint8_t *hi, size_t hilen, const struct catalog_visit *visit, void *ctx) {
    int r = 0;
    struct cblock *b = node_read(v, address, generation, &r);
    if (!b) return r;
    struct store_node *n = node(b);
    if ((level >= 0 && n->level != level) || (level >= 0 && !n->count)) r = -N_ERR_IO;
    if (r >= 0 && visit->node && !(address & TEMP_ADDRESS)) r = visit->node(ctx, address, n->level);
    for (unsigned i = 0; i < n->count && r >= 0; i++) {
        uint8_t *it = item(n, i);
        /* a branch's first key bounds nothing: it is not checked, nor its second against it */
        unsigned first = n->level ? 1u : 0u;
        if (i >= first && ((i > first && compare(key_of(item(n, i - 1)), klen_of(item(n, i - 1)), key_of(it), klen_of(it)) >= 0) ||
                           (lo && compare(key_of(it), klen_of(it), lo, lolen) < 0) ||
                           (hi && compare(key_of(it), klen_of(it), hi, hilen) >= 0))) {
            kprintf("store: catalog node %lu is out of order\n", (unsigned long)address);
            r = -N_ERR_IO;
            break;
        }
        if (n->level) {
            struct store_child c = child_of(n, i);
            uint8_t *next = i + 1u < n->count ? item(n, i + 1) : NULL;
            r = walk(v, c.address, c.generation, n->level - 1, i ? key_of(it) : lo, i ? klen_of(it) : lolen,
                     next ? key_of(next) : hi, next ? klen_of(next) : hilen, visit, ctx);
        } else if (visit->item) {
            struct item copy;
            copy_out(it, &copy);
            r = visit->item(ctx, &copy);
        }
    }
    cache_put(v, b);
    return r;
}

int catalog_walk(struct volume *v, const struct catalog_visit *visit, void *ctx) {
    return walk(v, v->root, v->root_generation, -1, NULL, 0, NULL, 0, visit, ctx);
}

/* ---- commits ---- */

static int place(struct volume *v, uint64_t temp, uint64_t generation, uint64_t *out) {
    struct cblock *b = cache_find(v, temp);
    if (!b) panic("store: catalog node %lx is not in memory", (unsigned long)temp);
    struct store_node *n = node(b);
    int r = 0;
    for (unsigned i = 0; n->level && i < n->count && r >= 0; i++) {
        struct store_child c = child_of(n, i);
        if (!(c.address & TEMP_ADDRESS)) continue;
        uint64_t at;
        if ((r = place(v, c.address, generation, &at)) >= 0) set_child(n, i, at, generation);
    }
    struct range got;
    if (r >= 0) r = space_alloc(v, 1, false, &got);
    if (r >= 0) r = space_use(v, got.start, 1);
    if (r < 0) {
        cache_put(v, b);
        return r;
    }
    cache_rename(v, b, got.start);
    head_seal(v, b->data, STORE_MAGIC_NODE, got.start, generation);
    cache_unwritten(v, b);
    *out = got.start;
    return 0;
}

int catalog_freeze(struct volume *v, uint64_t generation) {
    if (!(v->root & TEMP_ADDRESS)) return 0;
    uint64_t at;
    int r = place(v, v->root, generation, &at);
    if (r < 0) return r;
    v->root = at;
    v->root_generation = generation;
    return 0;
}

int catalog_format(struct volume *v) {
    int error = 0;
    struct cblock *b = node_new(v, 0, &error);
    if (!b) return error;
    v->root = b->address;
    v->root_generation = v->generation;
    cache_put(v, b);
    return 0;
}

int catalog_check(struct volume *v) {
    int error = 0;
    struct cblock *b = node_read(v, v->root, v->root_generation, &error);
    if (!b) return error;
    cache_put(v, b);
    return 0;
}
