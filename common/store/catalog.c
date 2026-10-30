/* The catalog's B+tree (store/catalog.h).

   The tree latch (v->tree) guards the root and every branch, and which leaf holds which keys; each
   leaf's own latch guards its items. A read holds the tree latch shared and its leaf's latch
   shared. A change first tries the same way down, holding the tree latch shared and its leaf's
   latch alone: when the leaf is already in the open generation (so is every node above it) and
   the change neither splits it nor empties or shrinks it below a quarter, it changes the leaf in
   place. Otherwise it goes down again with the tree latch alone, copying the nodes of the way into
   the open generation, and splits or joins as it must.

   The nodes of the open generation are only in memory: the catalog holds each from the call that
   makes it until the commit places it (or until it leaves the tree), and counts them in
   v->temp_nodes. Every other node comes from the medium. A call that finds one not in memory, or a
   space map block it must change not in memory, lets go of every latch and block it holds, reads
   it, keeps it held, and starts again. */
#include "store/catalog.h"
#include "store/space.h"

#define LEVELS 16
#define HEADER sizeof(struct store_node)
#define PINS (2 * LEVELS)
/* a call found something it needs not in memory (struct miss): it starts again once it is */
#define MISS (-1000)

static struct store_node *node(struct store_block *b) { return (struct store_node *)b->data; }
static uint8_t *item(struct store_node *n, unsigned i) { return (uint8_t *)n + n->slot[i]; }
static uint16_t klen_of(const uint8_t *it) { return ((const uint16_t *)it)[0]; }
static uint16_t vlen_of(const uint8_t *it) { return ((const uint16_t *)it)[1]; }
static uint8_t *key_of(uint8_t *it) { return it + 4; }
static uint8_t *value_of(uint8_t *it) { return it + 4 + klen_of(it); }
static size_t item_size(const uint8_t *it) { return 4 + (size_t)klen_of(it) + vlen_of(it); }
static size_t room(struct store_node *n) { return n->low - (HEADER + 2 * (size_t)n->count); }
static size_t used(struct store_node *n) { return STORE_BLOCK - room(n); }

static int compare(const uint8_t *a, size_t an, const uint8_t *b, size_t bn) {
    int c = memcmp(a, b, STORE_MIN(an, bn));
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

/* ---- blocks ---- */

static void put(struct volume *v, struct store_block *b) {
    if (b) v->blocks->put(v->blocks, b);
}

/* What a call needs from the medium before it can go on. */
struct miss {
    uint64_t address, generation;
    bool map; /* the space map block for block `address`, else the node there */
};

/* Blocks read for a call that missed them, held until it ends: when it starts again, it finds
   them. The oldest goes when there is no room: the call then reads it again if it must. A node
   that could not be read is the last one missed (failed). */
struct pins {
    struct store_block *b[PINS];
    unsigned n;
    bool failed;
    struct miss last;
};

static void unpin(struct volume *v, struct pins *p) {
    while (p->n) put(v, p->b[--p->n]);
}

/* what m names into memory, with no latch held */
static int fetch(struct volume *v, const struct miss *m, struct pins *p) {
    if (m->map) return space_load(v, m->address, 1);
    int error = -N_ERR_IO;
    struct store_block *b = v->blocks->read(v->blocks, v, m->address, node_valid, m->generation, &error);
    if (!b) {
        /* The tree moves on while nothing is latched: the node may have left it and its block gone
           to something else since it was missed. Only a node the tree still names when the call
           starts again is damaged. */
        if (!p->failed || p->last.address != m->address || p->last.generation != m->generation) {
            p->failed = true;
            p->last = *m;
            return 0;
        }
        if (error != -N_ERR_NO_MEMORY) v->env->trouble(STORE_TROUBLE_NODE, m->address, error); /* memory's, not the volume's */
        return error;
    }
    if (p->n == PINS) {
        put(v, p->b[0]);
        memmove(p->b, p->b + 1, (PINS - 1) * sizeof *p->b);
        p->n--;
    }
    p->b[p->n++] = b;
    return 0;
}

/* Damage of the tree found in memory: a node no read can make whole. */
static int damaged(struct volume *v, uint64_t address) {
    v->env->trouble(STORE_TROUBLE_NODE, address, -N_ERR_IO);
    return -N_ERR_IO;
}

/* The node at address of generation, held, when memory has it; else NULL: MISS (*m what to read),
   or an error. The tree latch is held: the nodes it reaches are the tree's, and only it changes
   which. */
static struct store_block *node_find(struct volume *v, uint64_t address, uint64_t generation, struct miss *m, int *r) {
    struct store_block *b = v->blocks->find(v->blocks, address);
    if (b && node(b)->head.generation == generation) return b;
    if (b || (address & TEMP_ADDRESS)) { /* the open generation's nodes are always there, and as named */
        if (b) put(v, b);
        *r = volume_broken(v, STORE_TROUBLE_RULE, address, -N_ERR_IO);
        return NULL;
    }
    *m = (struct miss){address, generation, false};
    *r = MISS;
    return NULL;
}

/* one more hold on a block in memory that the catalog holds already */
static struct store_block *again(struct volume *v, struct store_block *b) {
    struct store_block *same = v->blocks->find(v->blocks, b->address);
    if (same != b) {
        if (same) put(v, same);
        volume_broken(v, STORE_TROUBLE_RULE, b->address, -N_ERR_IO);
        return NULL;
    }
    return same;
}

/* A node of the open generation (a copy of from, or empty at level), held by the catalog: the
   caller's to place in the tree or to drop. */
static struct store_block *node_new(struct volume *v, uint16_t level, const struct store_block *from, int *error) {
    uint64_t address = __atomic_fetch_add(&v->next_temp, 1, __ATOMIC_RELAXED);
    struct store_block *b = v->blocks->make(v->blocks, address, NULL, error);
    if (!b) return NULL;
    struct store_node *n = node(b);
    if (from) {
        memcpy(b->data, from->data, STORE_BLOCK);
    } else {
        n->level = level;
        n->low = STORE_BLOCK;
    }
    n->head.magic = STORE_MAGIC_NODE;
    n->head.address = address;
    n->head.generation = v->generation;
    __atomic_fetch_add(&v->temp_nodes, 1, __ATOMIC_RELAXED);
    volume_changed(v);
    return b;
}

/* A node of the open generation out of the tree, or never in it: the catalog's hold goes. */
static void node_drop(struct volume *v, struct store_block *b) {
    v->blocks->discard(v->blocks, b);
    put(v, b);
    __atomic_fetch_sub(&v->temp_nodes, 1, __ATOMIC_RELAXED);
}

/* ---- the way down ---- */

/* the way from the root to a leaf, each node held */
struct trail {
    struct store_block *b[LEVELS];
    unsigned at[LEVELS]; /* the child taken, or the item */
    int depth;           /* b[depth] is the leaf */
};

static void path_put(struct volume *v, struct trail *p) {
    for (int i = 0; i <= p->depth; i++)
        if (p->b[i]) put(v, p->b[i]);
    p->depth = -1;
}

/* A node of the path into the open generation (the tree latch held alone): copied the first time,
   its parent pointed at the copy, its old block let go. */
static int cow(struct volume *v, struct trail *p, int d, struct miss *m) {
    struct store_block *old = p->b[d];
    if (node(old)->head.generation == v->generation) return 0;
    int r = 0;
    struct store_block *b = node_new(v, 0, old, &r);
    if (!b) return r;
    if ((r = space_let_go(v, old->address, 1)) < 0) {
        node_drop(v, b);
        if (r == -N_ERR_NOT_READY) {
            *m = (struct miss){old->address, 0, true};
            r = MISS;
        }
        return r;
    }
    if (d) set_child(node(p->b[d - 1]), p->at[d - 1], b->address, v->generation);
    else v->root = b->address, v->root_generation = v->generation;
    put(v, old);
    p->b[d] = again(v, b);
    return p->b[d] ? 0 : -N_ERR_IO;
}

/* Down to the leaf for key, with the tree latch held (alone when writable: every node on the way
   into the open generation). */
static int descend(struct volume *v, const uint8_t *key, size_t klen, bool writable, struct trail *p, struct miss *m) {
    int r = 0;
    p->depth = -1;
    uint64_t address = v->root, generation = v->root_generation;
    for (int d = 0;; d++) {
        struct store_block *b = d < LEVELS ? node_find(v, address, generation, m, &r) : NULL;
        if (d == LEVELS) r = damaged(v, address);
        if (!b) {
            path_put(v, p);
            return r;
        }
        p->b[d] = b;
        p->depth = d;
        if (d && node(b)->level + 1 != node(p->b[d - 1])->level) {
            path_put(v, p);
            return damaged(v, address);
        }
        if (writable && (r = cow(v, p, d, m)) < 0) {
            path_put(v, p);
            return r;
        }
        struct store_node *n = node(p->b[d]);
        if (!n->level) return 0;
        if (!n->count) {
            path_put(v, p);
            return damaged(v, address);
        }
        p->at[d] = child_index(n, key, klen);
        struct store_child c = child_of(n, p->at[d]);
        address = c.address;
        generation = c.generation;
    }
}

/* leftmost (or rightmost) from the node at depth d down to a leaf */
static int edge_down(struct volume *v, struct trail *p, int d, bool right, struct miss *m) {
    int r = 0;
    while (node(p->b[d])->level) {
        struct store_node *n = node(p->b[d]);
        if (!n->count || d + 1 == LEVELS) return damaged(v, p->b[d]->address);
        p->at[d] = right ? n->count - 1u : 0;
        struct store_child c = child_of(n, p->at[d]);
        struct store_block *b = node_find(v, c.address, c.generation, m, &r);
        if (!b) return r;
        p->b[++d] = b;
        p->depth = d;
    }
    return 0;
}

/* The leaf after (or before) the path's, with the tree latch held shared: the old leaf's latch
   (shared) let go, the new one's taken. -N_ERR_NOT_FOUND at the tree's end, the old leaf's latch
   still held then; any other failure leaves no leaf latched. */
static int step_leaf(struct volume *v, struct trail *p, bool back, struct miss *m) {
    int d = p->depth - 1;
    while (d >= 0 && (back ? p->at[d] == 0 : p->at[d] + 1u >= node(p->b[d])->count)) d--;
    if (d < 0) return -N_ERR_NOT_FOUND;
    latch_unshare(&p->b[p->depth]->latch);
    for (int i = p->depth; i > d; i--) put(v, p->b[i]);
    p->depth = d;
    if (back) p->at[d]--;
    else p->at[d]++;
    struct store_child c = child_of(node(p->b[d]), p->at[d]);
    int r = 0;
    struct store_block *b = node_find(v, c.address, c.generation, m, &r);
    if (!b) return r;
    p->b[++d] = b;
    p->depth = d;
    if ((r = edge_down(v, p, d, back, m)) < 0) return r;
    latch_share(&p->b[p->depth]->latch);
    return 0;
}

static void copy_out(uint8_t *it, struct item *out) {
    out->klen = klen_of(it);
    out->vlen = vlen_of(it);
    memcpy(out->key, key_of(it), out->klen);
    memcpy(out->value, value_of(it), out->vlen);
}

/* ---- reads ---- */

static int get_once(struct volume *v, const uint8_t *key, size_t klen, void *value, size_t cap, struct miss *m) {
    struct trail p;
    latch_share(&v->tree);
    int r = descend(v, key, klen, false, &p, m);
    if (r >= 0) {
        struct store_block *leaf = p.b[p.depth];
        latch_share(&leaf->latch);
        bool exact;
        struct store_node *n = node(leaf);
        unsigned i = search(n, key, klen, &exact);
        r = -N_ERR_NOT_FOUND;
        if (exact) {
            uint8_t *it = item(n, i);
            memcpy(value, value_of(it), STORE_MIN(cap, (size_t)vlen_of(it)));
            r = vlen_of(it);
        }
        latch_unshare(&leaf->latch);
        path_put(v, &p);
    }
    latch_unshare(&v->tree);
    return r;
}

int catalog_get(struct volume *v, const uint8_t *key, size_t klen, void *value, size_t cap) {
    struct pins pins = {0};
    struct miss m;
    int r;
    while ((r = get_once(v, key, klen, value, cap, &m)) == MISS && (r = fetch(v, &m, &pins)) >= 0) {}
    unpin(v, &pins);
    return r;
}

static int next_once(struct volume *v, const uint8_t *key, size_t klen, bool equal, const uint8_t *limit, size_t llen,
                     struct item *out, struct miss *m) {
    struct trail p;
    latch_share(&v->tree);
    int r = descend(v, key, klen, false, &p, m);
    if (r < 0) {
        latch_unshare(&v->tree);
        return r;
    }
    latch_share(&p.b[p.depth]->latch);
    bool exact, latched = true;
    unsigned i = search(node(p.b[p.depth]), key, klen, &exact);
    if (exact && !equal) i++;
    while (i >= node(p.b[p.depth])->count) {
        if ((r = step_leaf(v, &p, false, m)) < 0) {
            latched = r == -N_ERR_NOT_FOUND;
            break;
        }
        i = 0;
    }
    if (r >= 0) {
        uint8_t *it = item(node(p.b[p.depth]), i);
        r = compare(key_of(it), klen_of(it), limit, llen) < 0 ? 0 : -N_ERR_NOT_FOUND;
        if (!r) copy_out(it, out);
    }
    if (latched) latch_unshare(&p.b[p.depth]->latch);
    path_put(v, &p);
    latch_unshare(&v->tree);
    return r;
}

int catalog_next(struct volume *v, const uint8_t *key, size_t klen, bool equal, const uint8_t *limit, size_t llen,
                 struct item *out) {
    struct pins pins = {0};
    struct miss m;
    int r;
    while ((r = next_once(v, key, klen, equal, limit, llen, out, &m)) == MISS && (r = fetch(v, &m, &pins)) >= 0) {}
    unpin(v, &pins);
    return r;
}

static int last_once(struct volume *v, const uint8_t *key, size_t klen, const uint8_t *floor, size_t flen,
                     struct item *out, struct miss *m) {
    struct trail p;
    latch_share(&v->tree);
    int r = descend(v, key, klen, false, &p, m);
    if (r < 0) {
        latch_unshare(&v->tree);
        return r;
    }
    latch_share(&p.b[p.depth]->latch);
    bool exact, latched = true;
    unsigned i = search(node(p.b[p.depth]), key, klen, &exact);
    while (!exact && i == 0) { /* everything in this leaf is above key */
        if ((r = step_leaf(v, &p, true, m)) < 0) {
            latched = r == -N_ERR_NOT_FOUND;
            break;
        }
        i = node(p.b[p.depth])->count;
    }
    if (r >= 0) {
        if (!exact) i--;
        uint8_t *it = item(node(p.b[p.depth]), i);
        r = compare(key_of(it), klen_of(it), floor, flen) >= 0 ? 0 : -N_ERR_NOT_FOUND;
        if (!r) copy_out(it, out);
    }
    if (latched) latch_unshare(&p.b[p.depth]->latch);
    path_put(v, &p);
    latch_unshare(&v->tree);
    return r;
}

int catalog_last(struct volume *v, const uint8_t *key, size_t klen, const uint8_t *floor, size_t flen, struct item *out) {
    struct pins pins = {0};
    struct miss m;
    int r;
    while ((r = last_once(v, key, klen, floor, flen, out, &m)) == MISS && (r = fetch(v, &m, &pins)) >= 0) {}
    unpin(v, &pins);
    return r;
}

/* ---- changes ---- */

/* What a change may split into, made before it changes anything, so that it cannot fail half way:
   a right half for each node that splits, a root over the old one when that splits too, and a
   block to split from. */
struct spare {
    struct store_block *b[LEVELS + 1];
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
    s->copy = k ? v->env->alloc(STORE_BLOCK) : NULL;
    int error = k && !s->copy ? -N_ERR_NO_MEMORY : 0;
    while (!error && s->n < k) {
        struct store_block *b = node_new(v, 0, NULL, &error);
        if (b) s->b[s->n++] = b;
    }
    return error;
}

/* a spare node into the tree: the catalog's hold on it is the tree's from now on */
static struct store_block *spare_take(struct volume *v, struct spare *s, uint16_t level) {
    if (!s->n) {
        volume_broken(v, STORE_TROUBLE_RULE, 0, -N_ERR_IO);
        return NULL;
    }
    struct store_block *b = s->b[--s->n];
    node(b)->level = level;
    return b;
}

/* the spare nodes the change did not take go */
static void spare_put(struct volume *v, struct spare *s) {
    while (s->n) node_drop(v, s->b[--s->n]);
    v->env->free(s->copy);
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
static int place_item(struct volume *v, struct trail *p, int d, unsigned at, const uint8_t *key, size_t klen,
                      const void *value, size_t vlen, struct spare *s) {
    struct store_node *n = node(p->b[d]);
    if (room(n) >= 6 + klen + vlen) {
        put_item(n, at, key, klen, value, vlen);
        return 0;
    }
    struct store_block *rb = spare_take(v, s, n->level);
    if (!rb) return -N_ERR_IO;
    struct store_node *right = node(rb);
    split(n, right, s->copy, at, key, klen, value, vlen);
    uint8_t *first = item(right, 0);
    uint8_t sep[STORE_KEY_MAX];
    size_t slen = klen_of(first);
    memcpy(sep, key_of(first), slen);
    struct store_child c = {rb->address, v->generation};
    if (d == 0) { /* a new root over the two halves */
        struct store_block *root = spare_take(v, s, (uint16_t)(n->level + 1));
        if (!root) return -N_ERR_IO;
        uint8_t *lfirst = item(n, 0);
        struct store_child l = {p->b[0]->address, v->generation};
        put_item(node(root), 0, key_of(lfirst), klen_of(lfirst), &l, sizeof l);
        put_item(node(root), 1, sep, slen, &c, sizeof c);
        v->root = root->address;
        v->root_generation = v->generation;
        return 0;
    }
    return place_item(v, p, d - 1, p->at[d - 1] + 1, sep, slen, &c, sizeof c, s);
}

/* The value change makes of item at of n, into made (STORE_VALUE_MAX bytes): what the change
   writes from then on. */
static int amended(struct store_node *n, unsigned at, catalog_change *change, void *ctx, uint8_t *made,
                   const void **value, size_t *vlen) {
    uint8_t *it = item(n, at);
    memcpy(made, value_of(it), vlen_of(it));
    int r = change(ctx, made, vlen_of(it));
    if (r < 0) return r;
    if (r > (int)STORE_VALUE_MAX) return -N_ERR_TOO_LARGE;
    *value = made;
    *vlen = (size_t)r;
    return 0;
}

/* The way a change goes when it cannot stay in its leaf: with the tree latch alone. key gets value
   (vlen bytes), or (value NULL) change makes it from the one it has; replace: an item already there
   may be replaced. */
static int change_whole(struct volume *v, const uint8_t *key, size_t klen, const void *value, size_t vlen, bool replace,
                        catalog_change *change, void *ctx, struct miss *m) {
    struct trail p;
    uint8_t *made = NULL;
    latch_take(&v->tree);
    int r = descend(v, key, klen, true, &p, m);
    if (r >= 0) {
        struct store_node *n = node(p.b[p.depth]);
        bool exact;
        unsigned at = search(n, key, klen, &exact);
        if (exact && !replace) r = -N_ERR_EXISTS;
        else if (!exact && change) r = -N_ERR_NOT_FOUND;
        else if (change && !(made = v->env->alloc(STORE_VALUE_MAX))) r = -N_ERR_NO_MEMORY;
        if (r >= 0 && change) r = amended(n, at, change, ctx, made, &value, &vlen);
        struct spare s = {0};
        if (r >= 0 &&
            (r = spare_get(v, &s, spare_count(&p, 6 + klen + vlen, exact ? 2 + item_size(item(n, at)) : 0))) >= 0) {
            if (exact) remove_item(n, at);
            if ((r = place_item(v, &p, p.depth, at, key, klen, value, vlen, &s)) >= 0) volume_changed(v);
        }
        spare_put(v, &s);
        path_put(v, &p);
    }
    latch_give(&v->tree);
    v->env->free(made);
    return r < 0 ? r : 0;
}

/* The change tried in its leaf, with the tree latch shared: 1 when it does not fit there (nothing
   changed), else its outcome. */
static int change_in_leaf(struct volume *v, const uint8_t *key, size_t klen, const void *value, size_t vlen, bool replace,
                          catalog_change *change, void *ctx, struct miss *m) {
    struct trail p;
    uint8_t made[STORE_VALUE_MAX];
    latch_share(&v->tree);
    int r = descend(v, key, klen, false, &p, m);
    if (r >= 0) {
        struct store_block *leaf = p.b[p.depth];
        latch_take(&leaf->latch);
        struct store_node *n = node(leaf);
        bool exact;
        unsigned at = search(n, key, klen, &exact);
        size_t freed = exact ? 2 + item_size(item(n, at)) : 0;
        if (exact && !replace) r = -N_ERR_EXISTS;
        else if (!exact && change) r = -N_ERR_NOT_FOUND;
        if (r >= 0 && change) r = amended(n, at, change, ctx, made, &value, &vlen);
        if (r >= 0 && (n->head.generation != v->generation || room(n) + freed < 6 + klen + vlen)) r = 1;
        if (!r) {
            if (exact) remove_item(n, at);
            put_item(n, at, key, klen, value, vlen);
            volume_changed(v);
        }
        latch_give(&leaf->latch);
        path_put(v, &p);
    }
    latch_unshare(&v->tree);
    return r;
}

static int change_once(struct volume *v, const uint8_t *key, size_t klen, const void *value, size_t vlen, bool replace,
                       catalog_change *change, void *ctx, struct miss *m) {
    int r = change_in_leaf(v, key, klen, value, vlen, replace, change, ctx, m);
    return r == 1 ? change_whole(v, key, klen, value, vlen, replace, change, ctx, m) : r < 0 ? r : 0;
}

static int change(struct volume *v, const uint8_t *key, size_t klen, const void *value, size_t vlen, bool replace,
                  catalog_change *amend, void *ctx) {
    if (klen > STORE_KEY_MAX || vlen > STORE_VALUE_MAX) return -N_ERR_TOO_LARGE;
    int r = volume_writable(v);
    if (r < 0) return r;
    struct pins pins = {0};
    struct miss m;
    while ((r = change_once(v, key, klen, value, vlen, replace, amend, ctx, &m)) == MISS &&
           (r = fetch(v, &m, &pins)) >= 0) {}
    unpin(v, &pins);
    return r;
}

int catalog_put(struct volume *v, const uint8_t *key, size_t klen, const void *value, size_t vlen) {
    return change(v, key, klen, value, vlen, true, NULL, NULL);
}

int catalog_insert(struct volume *v, const uint8_t *key, size_t klen, const void *value, size_t vlen) {
    return change(v, key, klen, value, vlen, false, NULL, NULL);
}

int catalog_amend(struct volume *v, const uint8_t *key, size_t klen, catalog_change *amend, void *ctx) {
    return change(v, key, klen, NULL, 0, true, amend, ctx);
}

static int ready_once(struct volume *v, const uint8_t *key, size_t klen, struct miss *m) {
    struct trail p;
    latch_share(&v->tree);
    int r = descend(v, key, klen, false, &p, m);
    /* a leaf's generation is its own for good: no latch of the leaf's to read it */
    bool there = r >= 0 && node(p.b[p.depth])->head.generation == v->generation;
    if (r >= 0) path_put(v, &p);
    latch_unshare(&v->tree);
    if (r < 0 || there) return r;
    latch_take(&v->tree);
    if ((r = descend(v, key, klen, true, &p, m)) >= 0) path_put(v, &p);
    latch_give(&v->tree);
    return r;
}

int catalog_ready(struct volume *v, const uint8_t *key, size_t klen) {
    int r = volume_writable(v);
    if (r < 0) return r;
    struct pins pins = {0};
    struct miss m;
    while ((r = ready_once(v, key, klen, &m)) == MISS && (r = fetch(v, &m, &pins)) >= 0) {}
    unpin(v, &pins);
    return r;
}

/* a node of the path out of the tree (copied into this generation, so its block was let go) */
static void drop_node(struct volume *v, struct trail *p, int d) {
    node_drop(v, p->b[d]);
    put(v, p->b[d]);
    p->b[d] = NULL;
}

/* The node at depth d of the path, small after a delete, into a neighbour when both fit in one
   (the tree latch held alone): false when it stays as it is, still a whole tree. A neighbour that
   is not in memory, or whose space map block is not, is left as it is: joining is never needed. */
static bool join(struct volume *v, struct trail *p, int d) {
    struct store_node *n = node(p->b[d]), *parent = node(p->b[d - 1]);
    if (used(n) >= STORE_BLOCK / 4 || parent->count < 2) return false;
    unsigned left = p->at[d - 1] + 1u < parent->count ? p->at[d - 1] : p->at[d - 1] - 1u;
    unsigned other = left == p->at[d - 1] ? left + 1 : left;
    struct store_child c = child_of(parent, other);
    struct store_block *ob = v->blocks->find(v->blocks, c.address);
    if (!ob || node(ob)->head.generation != c.generation) {
        if (ob) put(v, ob);
        return false;
    }
    struct store_node *o = node(ob);
    /* a branch's first key bounds nothing: the right one's first child joins the left one under
       the parent's key for the right one, which is below every key under it */
    uint8_t *bound = item(parent, left + 1);
    size_t first = klen_of(item(left == p->at[d - 1] ? o : n, 0));
    size_t grown = n->level && klen_of(bound) > first ? klen_of(bound) - first : 0;
    if (used(o) + used(n) - HEADER + grown > STORE_BLOCK || o->level != n->level) {
        put(v, ob);
        return false;
    }
    /* the right one's items go to the left one, which comes into this generation */
    struct trail q = {.depth = d};
    for (int i = 0; i < d; i++) q.b[i] = p->b[i], q.at[i] = p->at[i];
    q.at[d - 1] = left;
    struct store_block *lb = left == p->at[d - 1] ? p->b[d] : ob, *rb = left == p->at[d - 1] ? ob : p->b[d];
    q.b[d] = lb;
    /* a block of an earlier generation is let go as a copy lets it go: the left one's by its
       copy, the right one's before its items leave it */
    struct miss unused;
    bool rb_old = rb == ob && node(ob)->head.generation != v->generation;
    if (lb == ob ? cow(v, &q, d, &unused) < 0 : rb_old && space_let_go(v, ob->address, 1) < 0) {
        put(v, q.b[d] == lb ? ob : q.b[d]);
        return false;
    }
    lb = q.b[d];
    struct store_node *l = node(lb), *rn = node(rb);
    for (unsigned i = 0; i < rn->count; i++) {
        uint8_t *it = item(rn, i), *key = i || !rn->level ? it : bound;
        put_item(l, l->count, key_of(key), klen_of(key), value_of(it), vlen_of(it));
    }
    remove_item(parent, left + 1);
    if (rb == ob && !rb_old) node_drop(v, ob), put(v, ob);
    else if (rb == ob) put(v, ob);
    else drop_node(v, p, d);
    p->b[d] = lb;
    p->at[d - 1] = left;
    return true;
}

/* The delete with the tree latch alone: empty nodes leave their parents, small ones join a
   neighbour, and a root with one child gives way to it. */
static int delete_whole(struct volume *v, const uint8_t *key, size_t klen, struct miss *m) {
    struct trail p;
    latch_take(&v->tree);
    int r = descend(v, key, klen, true, &p, m);
    if (r < 0) {
        latch_give(&v->tree);
        return r;
    }
    struct store_node *n = node(p.b[p.depth]);
    bool exact;
    unsigned at = search(n, key, klen, &exact);
    if (!exact) {
        path_put(v, &p);
        latch_give(&v->tree);
        return -N_ERR_NOT_FOUND;
    }
    remove_item(n, at);
    for (int d = p.depth; d > 0; d--) {
        if (!node(p.b[d])->count) {
            remove_item(node(p.b[d - 1]), p.at[d - 1]);
            drop_node(v, &p, d);
            continue;
        }
        if (!join(v, &p, d)) break;
    }
    while (p.b[0] && node(p.b[0])->level && node(p.b[0])->count == 1) {
        struct store_child c = child_of(node(p.b[0]), 0);
        struct store_block *b = v->blocks->find(v->blocks, c.address);
        if (!b || node(b)->head.generation != c.generation) {
            if (b) put(v, b);
            break;
        }
        drop_node(v, &p, 0);
        v->root = c.address;
        v->root_generation = c.generation;
        for (int i = 1; i <= p.depth; i++) {
            if (p.b[i]) put(v, p.b[i]);
            p.b[i] = NULL;
        }
        p.b[0] = b;
        p.depth = 0;
    }
    if (p.b[0] && node(p.b[0])->level && !node(p.b[0])->count) { /* everything went */
        int error = 0;
        struct store_block *leaf = node_new(v, 0, NULL, &error);
        if (leaf) {
            drop_node(v, &p, 0);
            v->root = leaf->address;
            v->root_generation = v->generation;
        }
    }
    path_put(v, &p);
    volume_changed(v);
    latch_give(&v->tree);
    return 0;
}

/* The delete in its leaf, with the tree latch shared: 1 when it would empty or shrink the leaf
   below a quarter, or the leaf is not in the open generation (nothing changed). */
static int delete_in_leaf(struct volume *v, const uint8_t *key, size_t klen, struct miss *m) {
    struct trail p;
    latch_share(&v->tree);
    int r = descend(v, key, klen, false, &p, m);
    if (r >= 0) {
        struct store_block *leaf = p.b[p.depth];
        latch_take(&leaf->latch);
        struct store_node *n = node(leaf);
        bool exact;
        unsigned at = search(n, key, klen, &exact);
        if (!exact) r = -N_ERR_NOT_FOUND;
        else if (n->head.generation != v->generation) r = 1;
        else if (p.depth && (n->count == 1 || used(n) - 2 - item_size(item(n, at)) < STORE_BLOCK / 4)) r = 1;
        if (!r) {
            remove_item(n, at);
            volume_changed(v);
        }
        latch_give(&leaf->latch);
        path_put(v, &p);
    }
    latch_unshare(&v->tree);
    return r;
}

static int delete_once(struct volume *v, const uint8_t *key, size_t klen, struct miss *m) {
    int r = delete_in_leaf(v, key, klen, m);
    return r == 1 ? delete_whole(v, key, klen, m) : r;
}

int catalog_delete(struct volume *v, const uint8_t *key, size_t klen) {
    int r = volume_writable(v);
    if (r < 0) return r;
    struct pins pins = {0};
    struct miss m;
    while ((r = delete_once(v, key, klen, &m)) == MISS && (r = fetch(v, &m, &pins)) >= 0) {}
    unpin(v, &pins);
    return r;
}

/* ---- the whole tree ---- */

/* the node at address, read when it is not in memory (nothing changes the tree meanwhile) */
static struct store_block *node_read(struct volume *v, uint64_t address, uint64_t generation, int *r) {
    struct miss m;
    struct store_block *b = node_find(v, address, generation, &m, r);
    if (b || *r != MISS) return b;
    if (!(b = v->blocks->read(v->blocks, v, address, node_valid, generation, r)))
        v->env->trouble(STORE_TROUBLE_NODE, address, *r);
    return b;
}

/* the node at address and what is under it; its keys from lo on and below hi (NULL: no bound) */
static int walk(struct volume *v, uint64_t address, uint64_t generation, int level, const uint8_t *lo, size_t lolen,
                const uint8_t *hi, size_t hilen, const struct catalog_visit *visit, void *ctx) {
    int r = -N_ERR_IO;
    struct store_block *b = node_read(v, address, generation, &r);
    if (!b) return r;
    r = 0;
    struct store_node *n = node(b);
    if ((level >= 0 && n->level != level) || (level >= 0 && !n->count)) r = damaged(v, address);
    if (r >= 0 && visit->node && !(address & TEMP_ADDRESS)) r = visit->node(ctx, address, n->level);
    for (unsigned i = 0; i < n->count && r >= 0; i++) {
        uint8_t *it = item(n, i);
        /* a branch's first key bounds nothing: it is not checked, nor its second against it */
        unsigned first = n->level ? 1u : 0u;
        if (i >= first && ((i > first && compare(key_of(item(n, i - 1)), klen_of(item(n, i - 1)), key_of(it), klen_of(it)) >= 0) ||
                           (lo && compare(key_of(it), klen_of(it), lo, lolen) < 0) ||
                           (hi && compare(key_of(it), klen_of(it), hi, hilen) >= 0))) {
            v->env->trouble(STORE_TROUBLE_ORDER, address, 0);
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
    put(v, b);
    return r;
}

int catalog_walk(struct volume *v, const struct catalog_visit *visit, void *ctx) {
    return walk(v, v->root, v->root_generation, -1, NULL, 0, NULL, 0, visit, ctx);
}

/* ---- commits ---- */

/* The next of the blocks a commit's nodes go to. */
struct blocks_taken {
    const struct ranges *list;
    size_t i;
    uint64_t k;
};

static int place(struct volume *v, uint64_t temp, uint64_t generation, struct blocks_taken *t, uint64_t *out) {
    struct store_block *b = v->blocks->find(v->blocks, temp);
    if (!b) return volume_broken(v, STORE_TROUBLE_RULE, temp, -N_ERR_IO);
    struct store_node *n = node(b);
    int r = 0;
    for (unsigned i = 0; n->level && i < n->count && r >= 0; i++) {
        struct store_child c = child_of(n, i);
        if (!(c.address & TEMP_ADDRESS)) continue;
        uint64_t at;
        if ((r = place(v, c.address, generation, t, &at)) >= 0) set_child(n, i, at, generation);
    }
    while (r >= 0 && t->i < t->list->n && t->k == t->list->r[t->i].count) t->i++, t->k = 0;
    if (r >= 0 && t->i == t->list->n) r = volume_broken(v, STORE_TROUBLE_RULE, temp, -N_ERR_NO_SPACE);
    uint64_t at = r >= 0 ? t->list->r[t->i].start + t->k++ : 0;
    if (r >= 0) r = space_use(v, at, 1);
    if (r < 0) {
        put(v, b);
        return r;
    }
    v->blocks->rename(v->blocks, b, at);
    head_seal(v, b->data, STORE_MAGIC_NODE, at, generation);
    volume_dirty(v, b); /* the catalog's hold: the commit's from now on */
    put(v, b);
    __atomic_fetch_sub(&v->temp_nodes, 1, __ATOMIC_RELAXED);
    *out = at;
    return 0;
}

int catalog_freeze(struct volume *v, uint64_t generation, const struct ranges *blocks) {
    if (!(v->root & TEMP_ADDRESS)) return 0;
    struct blocks_taken t = {blocks, 0, 0};
    uint64_t at;
    int r = place(v, v->root, generation, &t, &at);
    if (r < 0) return r;
    v->root = at;
    v->root_generation = generation;
    return 0;
}

/* a node of the open generation and every one under it go */
static void temp_free(struct volume *v, uint64_t address) {
    struct store_block *b = v->blocks->find(v->blocks, address);
    if (!b) return;
    struct store_node *n = node(b);
    for (unsigned i = 0; n->level && i < n->count; i++) {
        struct store_child c = child_of(n, i);
        if (c.address & TEMP_ADDRESS) temp_free(v, c.address);
    }
    node_drop(v, b);
    put(v, b);
}

void catalog_destroy(struct volume *v) {
    if (v->root & TEMP_ADDRESS) temp_free(v, v->root);
}

int catalog_format(struct volume *v) {
    int error = 0;
    struct store_block *b = node_new(v, 0, NULL, &error);
    if (!b) return error;
    v->root = b->address;
    v->root_generation = v->generation;
    return 0;
}

int catalog_check(struct volume *v) {
    int r = -N_ERR_IO;
    struct store_block *b = node_read(v, v->root, v->root_generation, &r);
    if (!b) return r;
    put(v, b);
    return 0;
}
