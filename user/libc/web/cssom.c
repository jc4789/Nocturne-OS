#include "cssom.h"
#include <stdio.h>
#include <weburl.h>

/* ---------------------------------------------------------------- parses
   Every change to a sheet's rules is a change to its text: the text with the
   change spliced in is parsed again, in one pass, into a new cascade AST and
   rule tree, and the old tree's ids move to the rules of the new one that
   stand where they stood. So the cascade can never differ from what CSSOM
   shows, and other CSSRule objects keep their identity across insertRule and
   deleteRule. A removed rule a CSSRule object stands for moves to a parse of
   its own (cssom_tree), where it keeps working without a sheet. */
static struct css_rule_info *next_rule(struct css_rule_info *r) {
    if (r->first) return r->first;
    while (r && !r->next) r = r->parent;
    return r ? r->next : NULL;
}
static size_t rule_at(const struct cssom_parse *p, const struct css_rule_info *r) { return (size_t)(r->text - p->source); }
static size_t rule_end(const struct cssom_parse *p, const struct css_rule_info *r) { return rule_at(p, r) + r->text_len; }
static struct css_rule_info **children(const struct cssom_parse *p, const struct css_rule_info *r, uint32_t *count) {
    if (!r) { *count = p->count; return p->top; }
    *count = r->child_count; return r->children;
}
static void index_children(struct cssom_parse *p) {
    struct css_rule_info *first = css_sheet_rules(p->ast, &p->count);
    p->top = p->count ? ar_alloc(&p->arena, (size_t)p->count * sizeof *p->top) : NULL;
    uint32_t i = 0;
    for (struct css_rule_info *r = first; r; r = r->next) p->top[i++] = r;
    for (struct css_rule_info *r = first; r; r = next_rule(r)) if (r->child_count) {
        r->children = ar_alloc(&p->arena, (size_t)r->child_count * sizeof *r->children);
        uint32_t j = 0;
        for (struct css_rule_info *c = r->first; c; c = c->next) r->children[j++] = c;
    }
}
static void parse_free(struct cssom_parse *p) { ar_free(&p->arena); memset(p, 0, sizeof *p); }
/* the parse now lives at p: the AST names its arena by address */
static void parse_moved(struct cssom_parse *p) { if (p->ast) css_sheet_arena_moved(p->ast, &p->arena); }
static int parse_text(web_doc *d, const char *base, struct cssom_parse *out, const char *text, size_t n,
                      const uint32_t *markers, uint32_t nmarkers) {
    memset(out, 0, sizeof *out);
    if (n == SIZE_MAX || (n && !text)) return CSSOM_OOM;
    jmp_buf trap;
    out->arena.trap = &trap;
    if (setjmp(trap)) { out->arena.trap = NULL; parse_free(out); return CSSOM_OOM; }
    out->ast = css_parse_cssom_sheet(d, &out->arena, n ? text : "", n, base ? base : "", markers, nmarkers);
    out->source = css_sheet_source(out->ast, &out->length);
    index_children(out);
    out->arena.trap = NULL;
    return CSSOM_OK;
}
/* What end of input closes (CSS Syntax: blocks and strings), as the parser's
   own scan counts it: each open brace, and an open string's quote first. */
static void closers(const char *s, size_t n, sbuf *out, bool *stray) {
    size_t depth = 0;
    char quote = 0;
    *stray = false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\\') { i++; continue; }
        if (quote) { if (c == quote || c == '\n') quote = 0; continue; }
        if (c == '"' || c == '\'') quote = c;
        else if (c == '{') depth++;
        else if (c == '}') { if (depth) depth--; else *stray = true; }
    }
    if (quote) sb_putc(out, quote);
    while (depth--) sb_putc(out, '}');
}
/* A sheet's first parse: its rules closed where end of input closed them, so
   that later text can follow every rule. */
static int parse_sheet_text(web_doc *d, const char *base, struct cssom_parse *out, const char *text, size_t n) {
    int error = parse_text(d, base, out, text, n, NULL, 0);
    if (error) return error;
    sbuf tail = {0};
    bool stray;
    closers(out->source, out->length, &tail, &stray);
    if (!tail.n) return CSSOM_OK;
    size_t total = out->length + tail.n;
    char *closed = total < SIZE_MAX ? malloc(total + 1) : NULL;
    if (!closed) { sb_free(&tail); parse_free(out); return CSSOM_OOM; }
    memcpy(closed, out->source, out->length);
    memcpy(closed + out->length, tail.p, tail.n);
    sb_free(&tail);
    struct cssom_parse again;
    error = parse_text(d, base, &again, closed, total, NULL, 0);
    free(closed);
    parse_free(out);
    if (error) return error;
    *out = again;
    parse_moved(out);
    return CSSOM_OK;
}

/* ---------------------------------------------------------------- ids */
struct cssom_slot { uint32_t id; struct css_rule_info *info; struct cssom_tree *tree; };
static uint32_t slot_hash(uint32_t id, uint32_t mask) { return (uint32_t)(id * 2654435761u) & mask; }
static struct cssom_slot *slot_find(const struct cssom_sheet *s, uint32_t id) {
    if (!id || !s->slots) return NULL;
    for (uint32_t i = slot_hash(id, s->slot_mask);; i = (i + 1) & s->slot_mask) {
        if (!s->slots[i].id) return NULL;
        if (s->slots[i].id == id) return &s->slots[i];
    }
}
static size_t parse_ids(const struct cssom_parse *p) {
    size_t n = 0;
    for (struct css_rule_info *r = p->count ? p->top[0] : NULL; r; r = next_rule(r)) n += r->id != 0;
    return n;
}
static void slots_put(struct cssom_slot *v, uint32_t mask, const struct cssom_parse *p, struct cssom_tree *tree) {
    for (struct css_rule_info *r = p->count ? p->top[0] : NULL; r; r = next_rule(r)) if (r->id) {
        uint32_t i = slot_hash(r->id, mask);
        while (v[i].id) i = (i + 1) & mask;
        v[i] = (struct cssom_slot){r->id, r, tree};
    }
}
/* the index of every id the sheet and its removed rules hold */
static bool slots_build(struct cssom_sheet *s) {
    size_t n = parse_ids(&s->p);
    for (struct cssom_tree *t = s->detached; t; t = t->next) n += parse_ids(&t->p);
    size_t size = 16;
    while (size < 2 * n + 2) { if (size > UINT32_MAX / 2) return false; size *= 2; }
    struct cssom_slot *v = calloc(size, sizeof *v);
    if (!v) return false;
    slots_put(v, (uint32_t)(size - 1), &s->p, NULL);
    for (struct cssom_tree *t = s->detached; t; t = t->next) slots_put(v, (uint32_t)(size - 1), &t->p, t);
    free(s->slots);
    s->slots = v; s->slot_mask = (uint32_t)(size - 1);
    return true;
}
static bool assign_ids(struct cssom_sheet *s, struct cssom_parse *p) {
    for (struct css_rule_info *r = p->count ? p->top[0] : NULL; r; r = next_rule(r)) {
        if (s->next_rule_id == UINT32_MAX) return false;
        r->id = ++s->next_rule_id;
    }
    return true;
}

/* ---------------------------------------------------------------- the trees, flattened
   A tree in preorder with each rule's depth determines its shape. */
struct flat { struct css_rule_info **v; uint32_t *depth; size_t n, cap; };
static void flat_free(struct flat *f) { free(f->v); free(f->depth); memset(f, 0, sizeof *f); }
static bool flatten(struct css_rule_info *start, bool single, struct flat *f) {
    uint32_t depth = 0;
    for (struct css_rule_info *r = start; r;) {
        if (f->n == f->cap) {
            size_t cap = f->cap ? f->cap * 2 : 64;
            if (cap > SIZE_MAX / sizeof *f->v) return false;
            struct css_rule_info **v = realloc(f->v, cap * sizeof *v);
            if (!v) return false;
            f->v = v;
            uint32_t *dv = realloc(f->depth, cap * sizeof *dv);
            if (!dv) return false;
            f->depth = dv; f->cap = cap;
        }
        f->v[f->n] = r; f->depth[f->n++] = depth;
        if (r->first) { r = r->first; depth++; continue; }
        for (;;) {
            if (!depth) { r = single ? NULL : r->next; break; }
            if (r->next) { r = r->next; break; }
            r = r->parent; depth--;
        }
    }
    return true;
}
static size_t flat_find(const struct flat *f, const struct css_rule_info *r) {
    for (size_t i = 0; i < f->n; i++) if (f->v[i] == r) return i;
    return SIZE_MAX;
}
static size_t flat_subtree(const struct flat *f, size_t at) {
    size_t end = at + 1;
    while (end < f->n && f->depth[end] > f->depth[at]) end++;
    return end - at;
}
static void carry(struct css_rule_info *to, const struct css_rule_info *from) {
    to->id = from->id; to->exposed = from->exposed;
}

/* ---------------------------------------------------------------- edits */
struct edit { size_t at, end; const char *text; size_t n; size_t marker; /* in text; SIZE_MAX: none */ };
static int marker_cmp(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}
/* p's text with the edits (in order, apart) made, and where its markers are */
static bool apply_edits(const struct cssom_parse *p, const struct edit *e, int count,
                        char **text, size_t *n, uint32_t **markers, uint32_t *nmarkers) {
    size_t total = p->length;
    for (int i = 0; i < count; i++) {
        if (e[i].n > SIZE_MAX / 2 || total > SIZE_MAX / 2) return false;
        total = total - (e[i].end - e[i].at) + e[i].n;
    }
    if (total >= UINT32_MAX) return false;
    size_t old = 0, cap = 0;
    for (struct css_rule_info *r = p->count ? p->top[0] : NULL; r; r = next_rule(r)) old += r->marker;
    cap = old + (size_t)count;
    uint32_t *m = cap ? malloc(cap * sizeof *m) : NULL;
    char *out = malloc(total + 1);
    if ((cap && !m) || !out) { free(m); free(out); return false; }
    size_t used = 0, pos = 0, k = 0;
    for (int i = 0; i < count; i++) {
        memcpy(out + used, p->source + pos, e[i].at - pos); used += e[i].at - pos;
        if (e[i].marker != SIZE_MAX) m[k++] = (uint32_t)(used + e[i].marker);
        if (e[i].n) memcpy(out + used, e[i].text, e[i].n);
        used += e[i].n; pos = e[i].end;
    }
    memcpy(out + used, p->source + pos, p->length - pos); used += p->length - pos;
    out[used] = 0;
    for (struct css_rule_info *r = p->count ? p->top[0] : NULL; r; r = next_rule(r)) if (r->marker) {
        size_t at = rule_at(p, r), shifted = at;
        bool gone = false;
        for (int i = 0; i < count; i++) {
            if (at >= e[i].at && at < e[i].end) gone = true;
            else if (e[i].end <= at) shifted = shifted - (e[i].end - e[i].at) + e[i].n;
        }
        if (!gone) m[k++] = (uint32_t)shifted;
    }
    if (k) qsort(m, k, sizeof *m, marker_cmp);
    *text = out; *n = used; *markers = m; *nmarkers = (uint32_t)k;
    return true;
}

/* ---------------------------------------------------------------- removed rules */
static bool plain_nested(const struct css_rule_info *r) { return r->type == CSS_RULE_NESTED_DECLARATIONS && !r->marker; }
/* CSS_NESTED_DECLARATIONS_MARKER with these declarations as its block */
static void marker_text(sbuf *out, const char *decls, size_t n) {
    sb_puts(out, CSS_NESTED_DECLARATIONS_MARKER " { ");
    sb_put(out, decls, n);
    sb_puts(out, " }");
}
static uint32_t depth_of(const struct css_rule_info *r) {
    uint32_t depth = 0;
    for (; r->parent; r = r->parent) depth++;
    return depth;
}
/* The text a rule of p parses the same in, outside p: the sheet's @namespace
   rules a style rule's selectors may name, then each ancestor's text up to its
   block; *before the length of all that. The caller appends the rule and
   closes the ancestors' blocks. */
static uint32_t context_text(const struct cssom_parse *p, const struct css_rule_info *parent, uint32_t kind,
                             sbuf *out, uint32_t *namespaces) {
    *namespaces = 0;
    if (kind != CSS_RULE_IMPORT && kind != CSS_RULE_NAMESPACE)
        for (uint32_t i = 0; i < p->count; i++) if (p->top[i]->type == CSS_RULE_NAMESPACE) {
            sb_put(out, p->top[i]->text, p->top[i]->text_len); sb_putc(out, '\n'); ++*namespaces;
        }
    uint32_t depth = parent ? depth_of(parent) + 1 : 0;
    const struct css_rule_info **chain = depth ? malloc(depth * sizeof *chain) : NULL;
    if (depth && !chain) return UINT32_MAX;
    uint32_t i = depth;
    for (const struct css_rule_info *a = parent; a; a = a->parent) chain[--i] = a;
    for (i = 0; i < depth; i++) { sb_put(out, chain[i]->text, chain[i]->block_at); sb_putc(out, '\n'); }
    free(chain);
    return depth;
}
/* the rule at the place context_text left for it */
static struct css_rule_info *context_rule(const struct cssom_parse *p, uint32_t namespaces, uint32_t depth) {
    if (namespaces >= p->count) return NULL;
    struct css_rule_info *r = p->top[namespaces];
    for (uint32_t i = 0; i < depth && r; i++) r = r->child_count == 1 ? r->first : NULL;
    return r;
}
/* A removed rule some CSSRule object stands for, with its subtree, in a parse
   of its own; NULL when its text does not parse the same alone (its ids then
   stand for nothing). */
static struct cssom_tree *detach(struct cssom_sheet *s, const struct cssom_parse *old, struct css_rule_info *root) {
    sbuf text = {0};
    uint32_t namespaces, depth = context_text(old, root->parent, root->type, &text, &namespaces);
    if (depth == UINT32_MAX) { sb_free(&text); return NULL; }
    size_t base = text.n, count = 0;
    uint32_t *markers = NULL;
    struct flat before = {0};
    if (!flatten(root, true, &before)) goto fail;
    for (size_t i = 0; i < before.n; i++) count += before.v[i]->marker || (i == 0 && plain_nested(root));
    markers = count ? malloc(count * sizeof *markers) : NULL;
    if (count && !markers) goto fail;
    count = 0;
    if (plain_nested(root)) {
        markers[count++] = (uint32_t)base;
        marker_text(&text, root->text + root->decls_at, root->decls_len);
    } else {
        for (size_t i = 0; i < before.n; i++) if (before.v[i]->marker)
            markers[count++] = (uint32_t)(base + rule_at(old, before.v[i]) - rule_at(old, root));
        sb_put(&text, root->text, root->text_len);
    }
    for (uint32_t i = 0; i < depth; i++) sb_puts(&text, "\n}");
    struct cssom_tree *tree = text.n < UINT32_MAX ? calloc(1, sizeof *tree) : NULL;
    if (!tree) goto fail;
    if (parse_text(s->document, s->base, &tree->p, sb_cstr(&text), text.n, markers, (uint32_t)count)) { free(tree); goto fail; }
    parse_moved(&tree->p);
    struct css_rule_info *fresh = context_rule(&tree->p, namespaces, depth);
    struct flat after = {0};
    bool same = fresh && flatten(fresh, true, &after) && after.n == before.n;
    for (size_t i = 0; same && i < before.n; i++)
        same = before.v[i]->type == after.v[i]->type && before.depth[i] == after.depth[i];
    if (same) for (size_t i = 0; i < before.n; i++) carry(after.v[i], before.v[i]);
    flat_free(&after);
    if (!same) { parse_free(&tree->p); free(tree); goto fail; }
    tree->root_id = root->id;
    flat_free(&before); free(markers); sb_free(&text);
    return tree;
fail:
    flat_free(&before); free(markers); sb_free(&text);
    return NULL;
}
static void tree_free(struct cssom_tree *t) { parse_free(&t->p); free(t); }

/* ---------------------------------------------------------------- a change, made */
enum change_kind { CHANGE_SAME, CHANGE_INSERT, CHANGE_DELETE };
struct change {
    struct cssom_tree *tree;           /* NULL: the sheet's own rules */
    struct edit edits[2];
    int count;
    enum change_kind kind;
    struct css_rule_info *node;        /* SAME: the rule whose text changed; DELETE: the rule removed */
    struct css_rule_info *container;   /* INSERT: into it (NULL: the top), at index */
    uint32_t index;
    const char *prelude;               /* SAME: the rule's new prelude, which its new parse keeps */
    size_t prelude_n;
    size_t insert_at, insert_n;        /* INSERT: where the new rule's text stands in the new text */
};
static bool same_rule(const struct flat *a, size_t i, const struct flat *b, size_t j) {
    return a->depth[i] == b->depth[j] && a->v[i]->type == b->v[j]->type;
}
static int rebuild(struct cssom_sheet *s, struct change *c) {
    struct cssom_parse *target = c->tree ? &c->tree->p : &s->p;
    char *text; size_t n; uint32_t *markers; uint32_t nmarkers;
    if (!apply_edits(target, c->edits, c->count, &text, &n, &markers, &nmarkers)) return CSSOM_OOM;
    struct cssom_parse fresh;
    int error = parse_text(s->document, s->base, &fresh, text, n, markers, nmarkers);
    free(text); free(markers);
    if (error) return error;
    struct flat o = {0}, f = {0};
    struct cssom_tree *removed = NULL;
    error = CSSOM_OOM;
    if (!flatten(target->count ? target->top[0] : NULL, false, &o) ||
        !flatten(fresh.count ? fresh.top[0] : NULL, false, &f)) goto fail;
    size_t at = 0, gone = 0;
    uint32_t depth = 0;
    error = CSSOM_SYNTAX;
    if (c->kind == CHANGE_SAME) at = c->node ? flat_find(&o, c->node) : 0;
    else if (c->kind == CHANGE_DELETE) { at = flat_find(&o, c->node); if (at != SIZE_MAX) gone = flat_subtree(&o, at); }
    else {
        uint32_t count;
        struct css_rule_info **list = children(target, c->container, &count);
        size_t container = c->container ? flat_find(&o, c->container) : SIZE_MAX;
        if (c->container && container == SIZE_MAX) goto fail;
        depth = c->container ? o.depth[container] + 1 : 0;
        at = c->index < count ? flat_find(&o, list[c->index]) :
             c->container ? container + flat_subtree(&o, container) : o.n;
    }
    if (at == SIZE_MAX || f.n + gone < o.n) goto fail;
    size_t added = f.n + gone - o.n;
    if ((c->kind == CHANGE_INSERT) != (added != 0)) goto fail;
    for (size_t i = 0; i < at; i++) if (!same_rule(&o, i, &f, i)) goto fail;
    for (size_t i = at + gone; i < o.n; i++) if (!same_rule(&o, i, &f, i - gone + added)) goto fail;
    if (added) {
        /* the inserted rule, whole, where its text went: not a merge with its neighbours */
        if (f.depth[at] != depth || rule_at(&fresh, f.v[at]) != c->insert_at || f.v[at]->text_len != c->insert_n) goto fail;
        for (size_t j = at + 1; j < at + added; j++) if (f.depth[j] <= depth) goto fail;
    }
    if (c->prelude) {
        const struct css_rule_info *r = at < f.n ? f.v[at] : NULL;
        if (!r || r->prelude_len != c->prelude_n || memcmp(r->text + r->prelude_at, c->prelude, c->prelude_n)) goto fail;
    }
    error = CSSOM_OOM;
    if (added > UINT32_MAX - s->next_rule_id) goto fail;
    for (size_t i = 0; i < at; i++) carry(f.v[i], o.v[i]);
    for (size_t i = at + gone; i < o.n; i++) carry(f.v[i - gone + added], o.v[i]);
    for (size_t j = at; j < at + added; j++) f.v[j]->id = ++s->next_rule_id;
    bool exposed = false;
    for (size_t i = at; i < at + gone; i++) exposed |= o.v[i]->exposed;
    if (exposed) removed = detach(s, target, o.v[at]);
    struct cssom_parse previous = *target;
    *target = fresh; parse_moved(target);
    if (removed) { removed->next = s->detached; s->detached = removed; }
    if (!slots_build(s)) {
        if (removed) { s->detached = removed->next; tree_free(removed); }
        fresh = *target; *target = previous; parse_moved(target); parse_moved(&fresh);
        error = CSSOM_OOM; goto fail;
    }
    /* Unpublish the cascade's borrowed AST pointers before the old AST goes. */
    if (!c->tree) cssom_changed(s);
    parse_free(&previous);
    flat_free(&o); flat_free(&f);
    return CSSOM_OK;
fail:
    flat_free(&o); flat_free(&f);
    parse_free(&fresh);
    return error;
}

/* ---------------------------------------------------------------- sheets */
static void invalidate(web_doc *d) {
    css_styling_free(&d->sty);
    d->resources_dirty = d->dirty = d->need_style = true;
    d->layout_valid = false;
}
void cssom_changed(struct cssom_sheet *s) {
    if (s->constructed) invalidate(s->document);
    else if (s->associated && s->owner->cssom_current == s && s->owner->owner == s->document)
        invalidate(s->document);
}
/* a media query list as CSSOM serializes it (css.c's matchMedia serializer) */
static bool media_serialize(const char *text, size_t n, sbuf *out) {
    if (!n) return true;
    if (n > (SIZE_MAX - 16) / 9) return false;
    char *query = malloc(n + 1), *serialized = malloc(n * 9 + 16);
    if (!query || !serialized) { free(query); free(serialized); return false; }
    memcpy(query, text, n); query[n] = 0;
    for (size_t i = 0; i < n; i++) if (!query[i]) query[i] = ' ';
    css_media_evaluate(query, 1024, 768, true, serialized, n * 9 + 16);
    sb_puts(out, serialized);
    free(query); free(serialized);
    return true;
}
static int sheet_media(struct cssom_sheet *s, const char *text) {
    sbuf b = {0};
    if (text && !media_serialize(text, strlen(text), &b)) { sb_free(&b); return CSSOM_OOM; }
    char *media = b.n ? strndup(b.p, b.n) : NULL;
    sb_free(&b);
    if (b.n && !media) return CSSOM_OOM;
    free(s->media); s->media = media;
    return CSSOM_OK;
}
static int sheet_parse(struct cssom_sheet *s, const char *text, size_t length) {
    int error = parse_sheet_text(s->document, s->base, &s->p, text, length);
    if (error) return error;
    parse_moved(&s->p);
    if (!assign_ids(s, &s->p) || !slots_build(s)) { parse_free(&s->p); return CSSOM_OOM; }
    return CSSOM_OK;
}
static void sheet_free(struct cssom_sheet *s) {
    for (struct cssom_tree *t = s->detached; t;) { struct cssom_tree *next = t->next; tree_free(t); t = next; }
    parse_free(&s->p); free(s->slots); free(s->base); free(s->media); free(s->dom_media);
    free(s->dom_source); free(s->dom_href); free(s->href); free(s);
}
struct cssom_sheet *cssom_construct(web_doc *d, int *error) {
    *error = CSSOM_OK;
    node_t *owner = d->root;
    if (owner->cssom_serial == UINT32_MAX) { *error = CSSOM_OOM; return NULL; }
    struct cssom_sheet *s = calloc(1, sizeof *s);
    if (!s) { *error = CSSOM_OOM; return NULL; }
    s->document = d; s->base = strdup(d->base ? d->base : "");
    *error = s->base ? sheet_parse(s, "", 0) : CSSOM_OOM;
    if (*error) { sheet_free(s); return NULL; }
    s->owner = owner; s->id = ++owner->cssom_serial;
    s->constructed = s->origin_clean = true;
    s->next = owner->cssom_sheets; owner->cssom_sheets = s;
    return s;
}
int cssom_adopt(node_t *root, struct cssom_sheet **sheets, uint32_t count) {
    for (uint32_t i = 0; i < count; i++)
        if (!sheets[i] || !sheets[i]->constructed || sheets[i]->document != root->owner) return CSSOM_NOT_ALLOWED;
    struct cssom_sheet **copy = count ? malloc((size_t)count * sizeof *copy) : NULL;
    if (count && !copy) return CSSOM_OOM;
    if (count) memcpy(copy, sheets, (size_t)count * sizeof *copy);
    free(root->adopted_sheets); root->adopted_sheets = copy; root->adopted_count = count;
    invalidate(root->owner); return CSSOM_OK;
}
/* replace(text) and replaceSync(text): the sheet's rules become text's, without
   its @import rules; the old ones leave the sheet. */
int cssom_replace_sync(struct cssom_sheet *s, const char *text, size_t length) {
    if (!s || !s->constructed) return CSSOM_NOT_ALLOWED;
    struct cssom_parse fresh;
    int error = parse_sheet_text(s->document, s->base, &fresh, text, length);
    if (error) return error;
    parse_moved(&fresh);
    struct edit edits[64];
    int count = 0;
    for (uint32_t i = 0; i < fresh.count;) {
        for (count = 0; i < fresh.count && count < 64; i++) if (fresh.top[i]->type == CSS_RULE_IMPORT)
            edits[count++] = (struct edit){rule_at(&fresh, fresh.top[i]), rule_end(&fresh, fresh.top[i]), "", 0, SIZE_MAX};
        if (!count) break;
        char *t; size_t n; uint32_t *markers, nmarkers;
        if (!apply_edits(&fresh, edits, count, &t, &n, &markers, &nmarkers)) { parse_free(&fresh); return CSSOM_OOM; }
        struct cssom_parse without;
        error = parse_text(s->document, s->base, &without, t, n, markers, nmarkers);
        free(t); free(markers);
        parse_free(&fresh);
        if (error) return error;
        fresh = without; parse_moved(&fresh);
        i = 0;
    }
    bool exposed = false;
    for (struct css_rule_info *r = s->p.count ? s->p.top[0] : NULL; r && !exposed; r = next_rule(r)) exposed = r->exposed;
    struct cssom_tree *old = exposed ? calloc(1, sizeof *old) : NULL;
    if (exposed && !old) { parse_free(&fresh); return CSSOM_OOM; }
    struct cssom_parse previous = s->p;
    s->p = fresh; parse_moved(&s->p);
    if (old) { old->p = previous; parse_moved(&old->p); old->next = s->detached; s->detached = old; }
    if (!assign_ids(s, &s->p) || !slots_build(s)) {
        if (old) { s->detached = old->next; previous = old->p; free(old); }
        fresh = s->p; s->p = previous; parse_moved(&s->p); parse_free(&fresh);
        slots_build(s);
        return CSSOM_OOM;
    }
    cssom_changed(s); /* unpublish borrowed AST pointers before releasing */
    if (!old) parse_free(&previous);
    return CSSOM_OK;
}
static bool active(web_doc *d, node_t *n) {
    return d && n && n->owner == d && n->type == N_ELEM && !n->foreign && (n->tag == T_style || n->tag == T_link) &&
        d->live && !d->inert && doc_node_root(n, true) == d->root && !node_ancestor(n,T_template);
}
bool cssom_owner_disabled(node_t *n) {
    return n && (n->style_disabled || (n->tag==T_link && !n->style_disabled_set && node_attr(n,"disabled")));
}
static int link_origin_clean(web_doc *d,const struct cssom_link_source *source,bool *clean) {
    *clean=false;
    if(!strncasecmp(source->url,"data:",5)){*clean=true;return CSSOM_OK;}
    char *origin=NULL,*request=NULL,*final=NULL;
    enum web_url_result a=web_url_origin(web_effective_url(d),&origin),
        b=web_url_origin(source->url,&request),c=web_url_origin(source->base,&final);
    int error=(a==WEB_URL_OOM||b==WEB_URL_OOM||c==WEB_URL_OOM)?CSSOM_OOM:CSSOM_OK;
    *clean=!error&&a==WEB_URL_TUPLE&&b==WEB_URL_TUPLE&&c==WEB_URL_TUPLE&&
        !strcmp(origin,request)&&!strcmp(origin,final);
    free(origin);free(request);free(final);return error;
}
static bool available(web_doc *d,node_t *n,int *error) {
    if(!active(d,n))return false;
    const char *type=node_attr(n,"type");
    if(type&&*type&&!str_ieq(type,"text/css"))return false;
    if(n->tag==T_style)return true;
    struct cssom_link_source source;
    int status=doc_cssom_link_source(d,n,&source);
    free(source.url);free(source.owned_text);
    if(status==CSSOM_OOM)*error=status;
    return status==CSSOM_OK;
}
node_t *cssom_list_owner(web_doc *d,node_t *root,uint32_t index,bool select,uint32_t *count,int *error) {
    *error=CSSOM_OK;*count=0;
    if(!d||!root||root->owner!=d||!d->live||d->inert)return NULL;
    /* Ordinary native tree order, not the flattened styling tree: a document
       excludes its shadow sheets and a ShadowRoot excludes nested shadows. */
    for(node_t *n=root->first;n;) {
        if(available(d,n,error)) {
            if(select&&*count==index)return n;
            if(*count==UINT32_MAX){*error=CSSOM_OOM;return NULL;}
            (*count)++;
        }
        if(*error)return NULL;
        if(n->first && !(n->type==N_ELEM&&!n->foreign&&n->tag==T_template)){n=n->first;continue;}
        while(n!=root&&!n->next)n=n->parent;
        if(n==root)break;n=n->next;
    }
    return NULL;
}
struct cssom_sheet *cssom_style_find(node_t *n, uint32_t id) {
    for (struct cssom_sheet *s = n ? n->cssom_sheets : NULL; s; s = s->next) if (s->id == id) return s;
    return NULL;
}
static bool append_text(char **p, size_t *used, size_t *capacity, const char *text, size_t n) {
    if (*used == SIZE_MAX || n > SIZE_MAX - *used - 1) return false;
    size_t need = *used + n + 1;
    if (need > *capacity) {
        size_t cap = *capacity ? *capacity : 256;
        while (cap < need) cap = cap > SIZE_MAX/2 ? need : cap*2;
        char *q = realloc(*p, cap); if (!q) return false; *p = q; *capacity = cap;
    }
    if (n) memcpy(*p + *used, text, n);
    *used += n; (*p)[*used] = 0; return true;
}
static bool owner_text(node_t *owner, char **out, size_t *length) {
    char *text = NULL; size_t used = 0, cap = 0;
    for (node_t *n = owner->first; n;) {
        if (n->type == N_TEXT && !append_text(&text,&used,&cap,n->text ? n->text : "",n->textlen)) { free(text); return false; }
        if (n->first) { n = n->first; continue; }
        while (n != owner && !n->next) n = n->parent;
        if (n == owner) break; n = n->next;
    }
    if (!text && !append_text(&text,&used,&cap,"",0)) return false;
    *out = text; *length = used; return true;
}
/* The MediaList of an element's sheet is set from its media attribute, and set
   again when the attribute changes. */
static int owner_media(struct cssom_sheet *s, node_t *n) {
    const char *media = node_attr(n, "media");
    if (s->dom_media && !strcmp(s->dom_media, media ? media : "")) return CSSOM_OK;
    char *copy = strdup(media ? media : "");
    if (!copy) return CSSOM_OOM;
    int error = sheet_media(s, media);
    if (error) { free(copy); return error; }
    free(s->dom_media); s->dom_media = copy;
    return CSSOM_OK;
}
struct cssom_sheet *cssom_style_sheet(web_doc *d, node_t *n, int *error) {
    *error = CSSOM_OK;
    if (!active(d,n)) { *error = CSSOM_INACTIVE; return NULL; }
    const char *type = node_attr(n,"type");
    if (type && *type && !str_ieq(type,"text/css")) {
        cssom_style_text_changed(n); *error = CSSOM_INACTIVE; return NULL;
    }
    struct cssom_link_source input={0};bool link=n->tag==T_link;
    if(link) {
        *error=doc_cssom_link_source(d,n,&input);
        if(*error)return NULL;
    }
    if (n->cssom_current && n->cssom_current->associated && n->cssom_current->document == d) {
        struct cssom_sheet *current=n->cssom_current;
        if (current->observed_dom_revision==d->dom_revision &&
            (!link || (current->dom_href && !strcmp(current->dom_href,input.url)))) {
            free(input.url);free(input.owned_text);return current;
        }
        char *text; size_t length;
        if(link){text=(char*)input.text;length=input.length;}
        else if (!owner_text(n,&text,&length)) { *error=CSSOM_OOM;free(input.url);free(input.owned_text);return NULL; }
        bool same=length==current->dom_length&&!memcmp(text,current->dom_source,length)&&
            (!link||(current->dom_href&&!strcmp(current->dom_href,input.url)));
        if(!link)free(text);
        if (same) {
            *error=owner_media(current,n);
            current->observed_dom_revision=d->dom_revision;free(input.url);free(input.owned_text);
            return *error?NULL:current;
        }
        current->disabled=cssom_owner_disabled(n);current->associated=false;n->cssom_current=NULL;
    }
    if (n->cssom_serial == UINT32_MAX) { *error = CSSOM_OOM;goto fail_input; }
    char *text; size_t length;
    if(link) {
        length=input.length;text=malloc(length+1);
        if(!text){*error=CSSOM_OOM;goto fail_input;}
        memcpy(text,input.text,length);text[length]=0;
    }else if (!owner_text(n,&text,&length)) { *error = CSSOM_OOM; goto fail_input; }
    struct cssom_sheet *s = calloc(1,sizeof *s);
    if (!s) { free(text); *error = CSSOM_OOM;goto fail_input; }
    s->origin_clean=true; s->document=d;
    s->base=strdup(link?input.base:d->base?d->base:"");
    if(!s->base)*error=CSSOM_OOM;
    if(link && !*error) {
        *error=link_origin_clean(d,&input,&s->origin_clean);
        s->href=strdup(!strncasecmp(input.url,"data:",5)?input.url:input.base);
        if(!s->href)*error=CSSOM_OOM;
    }
    if(!*error)*error=owner_media(s,n);
    if(!*error)*error=sheet_parse(s,text,length);
    if (*error) { free(text);sheet_free(s);goto fail_input; }
    s->dom_source=text;s->dom_length=length;s->observed_dom_revision=d->dom_revision;
    s->dom_href=input.url;input.url=NULL;free(input.owned_text);input.owned_text=NULL;
    s->owner=n; s->id=++n->cssom_serial; s->associated=true; s->disabled=cssom_owner_disabled(n);
    if (n->cssom_current) n->cssom_current->associated=false;
    s->next=n->cssom_sheets; n->cssom_sheets=n->cssom_current=s;
    d->resources_dirty=d->dirty=d->need_style=true;
    return s;
fail_input:
    free(input.url);free(input.owned_text);return NULL;
}
void cssom_style_text_changed(node_t *n) {
    for (;n;n=n->parent) if (!n->foreign && n->type==N_ELEM && n->tag==T_style && n->cssom_current) {
        n->cssom_current->disabled=cssom_owner_disabled(n);n->cssom_current->associated=false; n->cssom_current=NULL;
    }
}
void cssom_style_lifecycle(node_t *root) {
    for (node_t *n=root;n;) {
        if (n->cssom_current) { n->cssom_current->disabled=cssom_owner_disabled(n);n->cssom_current->associated=false; n->cssom_current=NULL; }
        if (n->first) { n=n->first;continue; }
        if (n->shadow_root) { n=n->shadow_root;continue; }
        for (;;) {
            if(n==root){n=NULL;break;}
            if(n->parent){
                if(n->next){n=n->next;break;}
                node_t *parent=n->parent;
                if(parent->shadow_root){n=parent->shadow_root;break;}
                n=parent;
            }else if(n->shadow_host)n=n->shadow_host;
            else {n=NULL;break;}
        }
    }
}
void cssom_style_free(node_t *n) {
    free(n->adopted_sheets); n->adopted_sheets = NULL; n->adopted_count = 0;
    for (struct cssom_sheet *s=n->cssom_sheets;s;) { struct cssom_sheet *next=s->next; sheet_free(s); s=next; }
    n->cssom_sheets=n->cssom_current=NULL;
}

/* ---------------------------------------------------------------- rules by id */
static struct css_rule_info *find(struct cssom_sheet *s, uint32_t id, struct cssom_tree **tree) {
    struct cssom_slot *slot = slot_find(s, id);
    if (tree) *tree = slot ? slot->tree : NULL;
    return slot ? slot->info : NULL;
}
static struct cssom_parse *parse_of(struct cssom_sheet *s, struct cssom_tree *tree) { return tree ? &tree->p : &s->p; }
struct css_rule_info *cssom_rule(struct cssom_sheet *s, uint32_t id, bool *attached) {
    struct cssom_tree *tree;
    struct css_rule_info *r = find(s, id, &tree);
    if (attached) *attached = r && !tree;
    return r;
}
struct css_rule_info *cssom_child(struct cssom_sheet *s, uint32_t parent, uint32_t index, uint32_t *count) {
    struct cssom_tree *tree = NULL;
    struct css_rule_info *container = parent ? find(s, parent, &tree) : NULL;
    *count = 0;
    if (parent && !container) return NULL;
    struct css_rule_info **list = children(parse_of(s, tree), container, count);
    if (index >= *count) return NULL;
    list[index]->exposed = true;
    return list[index];
}
struct css_rule_info *cssom_parent_rule(struct cssom_sheet *s, const struct css_rule_info *r) {
    struct cssom_tree *tree;
    if (!r || !find(s, r->id, &tree) || (tree && tree->root_id == r->id)) return NULL;
    struct css_rule_info *parent = r->parent && r->parent->id ? r->parent : NULL;
    if (parent) parent->exposed = true;
    return parent;
}

/* ---------------------------------------------------------------- reading a rule's text */
/* s's whitespace runs outside strings as one space, s already trimmed */
static void put_collapsed(sbuf *out, const char *s, size_t n) {
    bool space = false;
    char quote = 0;
    size_t start = out->n;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (quote) {
            sb_putc(out, c);
            if (c == '\\' && i + 1 < n) sb_putc(out, s[++i]);
            else if (c == quote) quote = 0;
            continue;
        }
        if (is_space((unsigned char)c)) { space = true; continue; }
        if (space && out->n > start) sb_putc(out, ' ');
        space = false;
        sb_putc(out, c);
        if (c == '\\' && i + 1 < n) sb_putc(out, s[++i]);
        else if (c == '"' || c == '\'') quote = c;
    }
}
/* the end of the comma-separated piece of s that begins at at */
static size_t piece_end(const char *s, size_t at, size_t n) {
    int depth = 0;
    char quote = 0;
    for (size_t i = at; i < n; i++) {
        char c = s[i];
        if (c == '\\') { i++; continue; }
        if (quote) { if (c == quote) quote = 0; continue; }
        if (c == '"' || c == '\'') quote = c;
        else if (c == '(' || c == '[') depth++;
        else if ((c == ')' || c == ']') && depth) depth--;
        else if (c == ',' && !depth) return i;
    }
    return n;
}
static void trim(const char *s, size_t *a, size_t *z) {
    while (*a < *z && is_space((unsigned char)s[*a])) ++*a;
    while (*z > *a && is_space((unsigned char)s[*z - 1])) --*z;
}
/* whether c stands in s outside strings */
static bool has_char(const char *s, size_t n, const char *set) {
    char quote = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\\') { i++; continue; }
        if (quote) { if (c == quote) quote = 0; continue; }
        if (c == '"' || c == '\'') quote = c;
        else if (strchr(set, c)) return true;
    }
    return false;
}
static const char *prelude_text(const struct css_rule_info *r) { return r->text + r->prelude_at; }
static void prelude(const struct css_rule_info *r, sbuf *out) { put_collapsed(out, prelude_text(r), r->prelude_len); }
/* A style rule inside a style rule (CSS Nesting), not inside an @scope nearer it */
static bool nested(const struct css_rule_info *r) {
    for (const struct css_rule_info *a = r->parent; a; a = a->parent) {
        if (a->type == CSS_RULE_STYLE) return true;
        if (a->type == CSS_RULE_SCOPE) return false;
    }
    return false;
}
/* CSSStyleRule.selectorText: the parsed selectors serialized; a nested rule's
   relative selectors absolutized with the nesting selector (CSS Nesting 3) */
static void selector_text(const struct css_rule_info *r, sbuf *out) {
    if (!nested(r)) {
        size_t at = out->n;
        css_rule_selector_serialize(r, out);
        if (out->n == at) prelude(r, out);
        return;
    }
    const char *t = prelude_text(r);
    size_t n = r->prelude_len;
    for (size_t at = 0;;) {
        size_t end = piece_end(t, at, n), a = at, z = end;
        trim(t, &a, &z);
        if (at) sb_puts(out, ", ");
        if (!has_char(t + a, z - a, "&")) sb_puts(out, "& ");
        put_collapsed(out, t + a, z - a);
        if (end >= n) break;
        at = end + 1;
    }
}
/* a media query list as CSSOM serializes it */
static bool media_of(const char *text, size_t n, sbuf *out) {
    size_t clean_n;
    char *clean = css_preprocess(text ? text : "", text ? n : 0, &clean_n);
    if (!clean) return false;
    bool ok = media_serialize(clean, clean_n, out);
    free(clean);
    return ok;
}
static bool keyword_name(const char *name) {
    static const char *const words[] = {"none", "initial", "inherit", "unset", "revert", "revert-layer", "default"};
    for (size_t i = 0; i < sizeof words / sizeof *words; i++) if (str_ieq(name, words[i])) return true;
    return false;
}
/* a <keyframes-name>'s value: an identifier's or a string's */
static void name_value(const char *t, size_t n, sbuf *out) {
    if (n >= 2 && (t[0] == '"' || t[0] == '\'')) css_unescape(out, t + 1, n - 2);
    else css_unescape(out, t, n);
}
/* CSS Animations: a name serializes as an identifier unless it is one of
   the words an identifier cannot be */
static void put_keyframes_name(sbuf *out, const char *name) {
    if (*name && !keyword_name(name)) css_serialize_ident(out, name); else css_serialize_string(out, name);
}
/* CSSKeyframeRule.keyText of a keyframe selector: each offset a percentage;
   false: not a keyframe selector */
static bool key_text(const char *s, size_t n, sbuf *out) {
    size_t start = out->n;
    for (size_t at = 0;;) {
        size_t end = piece_end(s, at, n), a = at, z = end;
        trim(s, &a, &z);
        if (a == z) return false;
        if (out->n > start) sb_puts(out, ", ");
        if (z - a == 4 && !strncasecmp(s + a, "from", 4)) sb_puts(out, "0%");
        else if (z - a == 2 && !strncasecmp(s + a, "to", 2)) sb_puts(out, "100%");
        else {
            char number[64], *stop;
            size_t length = z - a - 1;
            if (s[z - 1] != '%' || !length || length >= sizeof number) return false;
            memcpy(number, s + a, length); number[length] = 0;
            if (!strchr("0123456789.+", number[0])) return false;
            double v = strtod(number, &stop);
            if (*stop || !(v >= 0 && v <= 100)) return false;
            char text[40];
            snprintf(text, sizeof text, "%.6g%%", v);
            sb_puts(out, text);
        }
        if (end >= n) break;
        at = end + 1;
    }
    return true;
}
/* an at-rule's name, as written, lowercased */
static void at_name(const struct css_rule_info *r, sbuf *out) {
    for (uint32_t i = 1; i < r->text_len && !is_space((unsigned char)r->text[i]) && r->text[i] != '{' && r->text[i] != ';'; i++)
        sb_putc(out, (char)lower((unsigned char)r->text[i]));
}
/* where an at-rule's name ends: its prelude follows */
static uint32_t name_end(const struct css_rule_info *r) {
    uint32_t i = 1;
    while (i < r->text_len && !is_space((unsigned char)r->text[i]) && r->text[i] != '{' && r->text[i] != ';' &&
           r->text[i] != '(' && r->text[i] != '"' && r->text[i] != '\'') i += r->text[i] == '\\' ? 2 : 1;
    return i < r->text_len ? i : r->text_len;
}

/* ---------------------------------------------------------------- declarations */
static bool has_declarations(const struct css_rule_info *r) {
    switch (r->type) {
    case CSS_RULE_STYLE: case CSS_RULE_PAGE: case CSS_RULE_KEYFRAME: case CSS_RULE_FONT_FACE: case CSS_RULE_MARGIN:
    case CSS_RULE_COUNTER_STYLE: case CSS_RULE_PROPERTY: case CSS_RULE_FONT_PALETTE_VALUES: case CSS_RULE_NESTED_DECLARATIONS:
        return true;
    default: return false;
    }
}
void cssom_declarations(const struct css_rule_info *r, const char **text, size_t *length) {
    bool own = r && has_declarations(r);
    *text = own ? r->text + r->decls_at : "";
    *length = own ? r->decls_len : 0;
}
/* @font-face: its descriptors (CSS Fonts 4), each serialized by css.c's
   FontFace serializer; src a non-empty list, its url()s as strings */
static bool font_face_check(const char *name, const char *value, size_t n, sbuf *serialized) {
    static const struct { const char *name; enum css_font_descriptor which; } names[] = {
        {"font-family", CSS_FONT_FAMILY}, {"font-style", CSS_FONT_STYLE}, {"font-weight", CSS_FONT_WEIGHT},
        {"font-stretch", CSS_FONT_WIDTH}, {"font-width", CSS_FONT_WIDTH}, {"unicode-range", CSS_FONT_UNICODE_RANGE},
        {"font-feature-settings", CSS_FONT_FEATURE_SETTINGS}, {"font-variation-settings", CSS_FONT_VARIATION_SETTINGS},
        {"font-display", CSS_FONT_DISPLAY}, {"ascent-override", CSS_FONT_ASCENT_OVERRIDE},
        {"descent-override", CSS_FONT_DESCENT_OVERRIDE}, {"line-gap-override", CSS_FONT_LINE_GAP_OVERRIDE},
        {"size-adjust", CSS_FONT_SIZE_ADJUST}};
    if (!strcmp(name, "src")) {
        if (!n || has_char(value, n, "{};!")) return false;
        if (!serialized) return true;
        for (size_t i = 0; i < n;) {
            if (n - i > 4 && !strncasecmp(value + i, "url(", 4)) {
                size_t a = i + 4, close = a;
                while (close < n && value[close] != ')') close++;
                size_t z = close;
                trim(value, &a, &z);
                sb_puts(serialized, "url(");
                if (a < z && (value[a] == '"' || value[a] == '\'')) sb_put(serialized, value + a, z - a);
                else {
                    sbuf url = {0};
                    css_unescape(&url, value + a, z - a);
                    css_serialize_string(serialized, sb_cstr(&url));
                    sb_free(&url);
                }
                sb_putc(serialized, ')');
                i = close < n ? close + 1 : n;
                continue;
            }
            if (is_space((unsigned char)value[i])) {
                if (serialized->n && serialized->p[serialized->n - 1] != ' ') sb_putc(serialized, ' ');
                i++;
                continue;
            }
            sb_putc(serialized, value[i++]);
        }
        while (serialized->n && serialized->p[serialized->n - 1] == ' ') serialized->n--;
        return true;
    }
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) if (!strcmp(name, names[i].name)) {
        char *text = NULL;
        int status = css_font_descriptor(names[i].which, value, n, &text);
        if (status > 0 && serialized) sb_puts(serialized, text);
        free(text);
        return status > 0;
    }
    return false;
}
/* @page: the properties, with the page descriptors (CSS Paged Media 3) */
static bool page_check(const char *name, const char *value, size_t n, sbuf *serialized) {
    if (!strcmp(name, "size") || !strcmp(name, "page-orientation") || !strcmp(name, "marks") || !strcmp(name, "bleed")) {
        if (!n || has_char(value, n, "{};!")) return false;
        if (serialized) put_collapsed(serialized, value, n);
        return true;
    }
    if (!css_valid_declaration(name, strlen(name), value, n)) return false;
    if (serialized) css_decl_value(name, value, n, serialized);
    return true;
}
/* @counter-style, @property and @font-palette-values: their descriptors as written */
static bool descriptor_check(const char *name, const char *value, size_t n, sbuf *serialized) {
    (void)name;
    if (!n || has_char(value, n, "{};!")) return false;
    if (serialized) put_collapsed(serialized, value, n);
    return true;
}
css_decl_check *cssom_declaration_check(const struct css_rule_info *r) {
    switch (r ? r->type : 0) {
    case CSS_RULE_FONT_FACE: return font_face_check;
    case CSS_RULE_PAGE: return page_check;
    case CSS_RULE_COUNTER_STYLE: case CSS_RULE_PROPERTY: case CSS_RULE_FONT_PALETTE_VALUES: return descriptor_check;
    default: return NULL;
    }
}
static void declarations(const struct css_rule_info *r, sbuf *out) {
    const char *text; size_t n;
    cssom_declarations(r, &text, &n);
    css_decl_serialize(text, n, cssom_declaration_check(r), out, NULL);
}
/* a descriptor's value in a rule's declaration block; false: none */
static bool descriptor(const struct css_rule_info *r, const char *name, sbuf *out) {
    sbuf block = {0};
    declarations(r, &block);
    const char *v; size_t n; bool important;
    bool found = css_decl_find(block.p ? block.p : "", block.n, name, &v, &n, &important);
    if (found) sb_put(out, v, n);
    sb_free(&block);
    return found;
}

/* ---------------------------------------------------------------- @import, @namespace, @font-feature-values */
static void import_text(const struct css_rule_import *im, const char *media, size_t media_n, sbuf *out) {
    sb_puts(out, "@import url(");
    css_serialize_string(out, im->href ? im->href : "");
    sb_putc(out, ')');
    if (im->layer) {
        sb_puts(out, " layer");
        if (*im->layer) { sb_putc(out, '('); sb_puts(out, im->layer); sb_putc(out, ')'); }
    }
    if (im->supports) { sb_puts(out, " supports("); sb_puts(out, im->supports); sb_putc(out, ')'); }
    if (media_n) { sb_putc(out, ' '); sb_put(out, media, media_n); }
    sb_putc(out, ';');
}
static bool import_media(const struct css_rule_info *r, sbuf *out) {
    const char *media = r->import && r->import->media ? r->import->media : "";
    return media_of(media, strlen(media), out);
}
static bool namespace_parts(const struct css_rule_info *r, sbuf *prefix, sbuf *uri, bool *has_prefix) {
    arena_t scratch = {0};
    jmp_buf trap;
    scratch.trap = &trap;
    const char *p = NULL, *u = NULL;
    bool ok = false;
    *has_prefix = false;
    if (!setjmp(trap) && css_namespace_rule(&scratch, r->text, r->text_len, &p, &u)) {
        ok = true;
        if (p) { *has_prefix = true; sb_puts(prefix, p); }
        sb_puts(uri, u ? u : "");
    }
    ar_free(&scratch);
    return ok;
}
static const char *const feature_blocks[] = {
    "annotation", "ornaments", "stylistic", "swash", "character-variant", "styleset", "historical-forms"};
#define FEATURE_BLOCKS (sizeof feature_blocks / sizeof *feature_blocks)
/* a @font-feature-values rule's entries: for each block, "name\0values\0"
   pairs in order, a later declaration of a name replacing an earlier one */
struct features { sbuf entries[FEATURE_BLOCKS]; };
static void features_free(struct features *f) { for (size_t i = 0; i < FEATURE_BLOCKS; i++) sb_free(&f->entries[i]); }
static int feature_block(const char *name, size_t n) {
    for (size_t i = 0; i < FEATURE_BLOCKS; i++)
        if (strlen(feature_blocks[i]) == n && !strncasecmp(name, feature_blocks[i], n)) return (int)i;
    return -1;
}
/* drop name's pair from entries */
static void feature_drop(sbuf *entries, const char *name) {
    for (size_t at = 0; at < entries->n;) {
        size_t key = strlen(entries->p + at) + 1, values = strlen(entries->p + at + key) + 1;
        if (!strcmp(entries->p + at, name)) {
            memmove(entries->p + at, entries->p + at + key + values, entries->n - at - key - values);
            entries->n -= key + values;
        } else at += key + values;
    }
}
static void feature_put(sbuf *entries, const char *name, const char *values) {
    feature_drop(entries, name);
    sb_put(entries, name, strlen(name) + 1);
    sb_put(entries, values, strlen(values) + 1);
}
static void features_read(const struct css_rule_info *r, struct features *f) {
    memset(f, 0, sizeof *f);
    const char *s = r->text + r->block_at, *e = s + r->block_len;
    while (s < e) {
        while (s < e && (is_space((unsigned char)*s) || *s == ';')) s++;
        if (s >= e) break;
        const char *open = s;
        while (open < e && *open != '{' && *open != ';') open++;
        if (open >= e || *open != '{') { s = open < e ? open + 1 : e; continue; }
        const char *close = open + 1;
        for (int depth = 1; close < e; close++) {
            if (*close == '{') depth++;
            else if (*close == '}' && !--depth) break;
        }
        int block = -1;
        if (*s == '@') {
            const char *a = s + 1, *z = open;
            while (z > a && is_space((unsigned char)z[-1])) z--;
            block = feature_block(a, (size_t)(z - a));
        }
        for (const char *d = open + 1; block >= 0 && d < close;) {
            const char *end = d;
            while (end < close && *end != ';') end++;
            const char *colon = d;
            while (colon < end && *colon != ':') colon++;
            size_t na = (size_t)(d - (open + 1)), nz = (size_t)(colon - (open + 1));
            const char *base = open + 1;
            trim(base, &na, &nz);
            if (colon < end && na < nz) {
                sbuf name = {0}, values = {0};
                css_unescape(&name, base + na, nz - na);
                bool valid = name.n > 0;
                for (const char *v = colon + 1; valid && v < end;) {
                    while (v < end && is_space((unsigned char)*v)) v++;
                    if (v >= end) break;
                    const char *w = v;
                    while (w < end && *w >= '0' && *w <= '9') w++;
                    if (w == v || (w < end && !is_space((unsigned char)*w))) { valid = false; break; }
                    if (values.n) sb_putc(&values, ' ');
                    sb_put(&values, v, (size_t)(w - v));
                    v = w;
                }
                if (valid && values.n) feature_put(&f->entries[block], sb_cstr(&name), sb_cstr(&values));
                sb_free(&name); sb_free(&values);
            }
            d = end < close ? end + 1 : close;
        }
        s = close < e ? close + 1 : e;
    }
}
static void features_text(const struct features *f, sbuf *out) {
    for (size_t i = 0; i < FEATURE_BLOCKS; i++) if (f->entries[i].n) {
        sb_puts(out, " @"); sb_puts(out, feature_blocks[i]); sb_puts(out, " {");
        for (size_t at = 0; at < f->entries[i].n;) {
            const char *name = f->entries[i].p + at, *values = name + strlen(name) + 1;
            sb_putc(out, ' '); css_serialize_ident(out, name); sb_puts(out, ": "); sb_puts(out, values); sb_putc(out, ';');
            at = (size_t)(values - f->entries[i].p) + strlen(values) + 1;
        }
        sb_puts(out, " }");
    }
}

/* ---------------------------------------------------------------- serialize a CSS rule */
static void serialize(const struct css_rule_info *r, sbuf *out);
/* a group rule's block (CSS Conditional 3): each child on a line of its own */
static void group(const struct css_rule_info *r, sbuf *out) {
    sb_puts(out, " {");
    for (uint32_t i = 0; i < r->child_count; i++) {
        size_t at = out->n;
        sb_puts(out, "\n  ");
        size_t child = out->n;
        serialize(r->children[i], out);
        if (out->n == child) out->n = at;
    }
    sb_puts(out, "\n}");
}
/* a block of declarations alone: "{ decls }" */
static void declaration_block(const struct css_rule_info *r, sbuf *out) {
    sb_puts(out, " {");
    size_t at = out->n;
    sb_putc(out, ' ');
    declarations(r, out);
    if (out->n == at + 1) out->n = at;
    sb_puts(out, " }");
}
static void serialize(const struct css_rule_info *r, sbuf *out) {
    switch (r->type) {
    case CSS_RULE_STYLE: {
        /* CSSOM 6.4.3, CSSStyleRule */
        selector_text(r, out);
        sbuf decls = {0};
        declarations(r, &decls);
        if (!r->child_count) {
            sb_puts(out, " {");
            if (decls.n) { sb_putc(out, ' '); sb_put(out, decls.p, decls.n); }
            sb_puts(out, " }");
        } else {
            sb_puts(out, " {");
            if (decls.n) { sb_puts(out, "\n  "); sb_put(out, decls.p, decls.n); }
            for (uint32_t i = 0; i < r->child_count; i++) {
                size_t at = out->n;
                sb_puts(out, "\n  ");
                size_t child = out->n;
                serialize(r->children[i], out);
                if (out->n == child) out->n = at;
            }
            sb_puts(out, "\n}");
        }
        sb_free(&decls);
        break;
    }
    case CSS_RULE_NESTED_DECLARATIONS: declarations(r, out); break;
    case CSS_RULE_IMPORT: {
        sbuf media = {0};
        import_media(r, &media);
        if (r->import) import_text(r->import, media.p, media.n, out);
        sb_free(&media);
        break;
    }
    case CSS_RULE_NAMESPACE: {
        sbuf prefix = {0}, uri = {0};
        bool has_prefix;
        if (namespace_parts(r, &prefix, &uri, &has_prefix)) {
            sb_puts(out, "@namespace ");
            if (has_prefix) { css_serialize_ident(out, sb_cstr(&prefix)); sb_putc(out, ' '); }
            sb_puts(out, "url("); css_serialize_string(out, sb_cstr(&uri)); sb_puts(out, ");");
        }
        sb_free(&prefix); sb_free(&uri);
        break;
    }
    case CSS_RULE_MEDIA:
        sb_puts(out, "@media ");
        media_of(prelude_text(r), r->prelude_len, out);
        group(r, out);
        break;
    case CSS_RULE_SUPPORTS: sb_puts(out, "@supports "); prelude(r, out); group(r, out); break;
    case CSS_RULE_CONTAINER: sb_puts(out, "@container "); prelude(r, out); group(r, out); break;
    case CSS_RULE_LAYER_BLOCK:
        sb_puts(out, "@layer");
        if (r->prelude_len) { sb_putc(out, ' '); prelude(r, out); }
        group(r, out);
        break;
    case CSS_RULE_SCOPE:
        sb_puts(out, "@scope");
        if (r->prelude_len) { sb_putc(out, ' '); prelude(r, out); }
        group(r, out);
        break;
    case CSS_RULE_STARTING_STYLE: sb_puts(out, "@starting-style"); group(r, out); break;
    case CSS_RULE_LAYER_STATEMENT: {
        sb_puts(out, "@layer ");
        const char *t = prelude_text(r);
        for (size_t at = 0, first = 1;; first = 0) {
            size_t end = piece_end(t, at, r->prelude_len), a = at, z = end;
            trim(t, &a, &z);
            if (!first) sb_puts(out, ", ");
            put_collapsed(out, t + a, z - a);
            if (end >= r->prelude_len) break;
            at = end + 1;
        }
        sb_putc(out, ';');
        break;
    }
    case CSS_RULE_FONT_FACE: sb_puts(out, "@font-face"); declaration_block(r, out); break;
    case CSS_RULE_PAGE: {
        sb_puts(out, "@page");
        if (r->prelude_len) { sb_putc(out, ' '); prelude(r, out); }
        sb_puts(out, " {");
        size_t at = out->n;
        sb_putc(out, ' ');
        declarations(r, out);
        if (out->n == at + 1) out->n = at;
        for (uint32_t i = 0; i < r->child_count; i++) { sb_putc(out, ' '); serialize(r->children[i], out); }
        sb_puts(out, " }");
        break;
    }
    case CSS_RULE_MARGIN: sb_putc(out, '@'); at_name(r, out); declaration_block(r, out); break;
    case CSS_RULE_KEYFRAMES: {
        sbuf name = {0};
        name_value(prelude_text(r), r->prelude_len, &name);
        sb_puts(out, "@keyframes ");
        put_keyframes_name(out, sb_cstr(&name));
        sb_free(&name);
        group(r, out);
        break;
    }
    case CSS_RULE_KEYFRAME:
        if (!key_text(prelude_text(r), r->prelude_len, out)) prelude(r, out);
        declaration_block(r, out);
        break;
    case CSS_RULE_COUNTER_STYLE:
        sb_puts(out, "@counter-style "); prelude(r, out); declaration_block(r, out); break;
    case CSS_RULE_FONT_PALETTE_VALUES:
        sb_puts(out, "@font-palette-values "); prelude(r, out); declaration_block(r, out); break;
    case CSS_RULE_PROPERTY: {
        /* CSS Properties and Values API: its descriptors in their order */
        sb_puts(out, "@property "); prelude(r, out); sb_puts(out, " {");
        static const char *const order[] = {"syntax", "inherits", "initial-value"};
        for (size_t i = 0; i < 3; i++) {
            sbuf value = {0};
            if (descriptor(r, order[i], &value)) {
                sb_putc(out, ' '); sb_puts(out, order[i]); sb_puts(out, ": "); sb_put(out, value.p, value.n); sb_putc(out, ';');
            }
            sb_free(&value);
        }
        sb_puts(out, " }");
        break;
    }
    case CSS_RULE_FONT_FEATURE_VALUES: {
        sb_puts(out, "@font-feature-values "); prelude(r, out); sb_puts(out, " {");
        struct features f;
        features_read(r, &f);
        features_text(&f, out);
        features_free(&f);
        sb_puts(out, " }");
        break;
    }
    default: break;
    }
}
void cssom_serialize(struct cssom_sheet *s, const struct css_rule_info *r, sbuf *out) {
    (void)s;
    if (r) serialize(r, out);
}

/* ---------------------------------------------------------------- attributes */
static const struct { const char *idl, *descriptor; } counter_descriptors[] = {
    {"system", "system"}, {"symbols", "symbols"}, {"additiveSymbols", "additive-symbols"}, {"negative", "negative"},
    {"prefix", "prefix"}, {"suffix", "suffix"}, {"range", "range"}, {"pad", "pad"}, {"speakAs", "speak-as"},
    {"fallback", "fallback"}};
static const char *counter_descriptor(const char *idl) {
    for (size_t i = 0; i < sizeof counter_descriptors / sizeof *counter_descriptors; i++)
        if (!strcmp(idl, counter_descriptors[i].idl)) return counter_descriptors[i].descriptor;
    return NULL;
}
/* @container's prelude: [<container-name>] <container-query>? */
static size_t container_name(const struct css_rule_info *r) {
    const char *t = prelude_text(r);
    size_t n = r->prelude_len, i = 0;
    while (i < n && !is_space((unsigned char)t[i]) && t[i] != '(') i += t[i] == '\\' ? 2 : 1;
    if (i > n) i = n;
    if (!i || (i < n && t[i] == '(')) return 0;
    static const char *const keywords[] = {"not", "and", "or", "none"};
    for (size_t k = 0; k < 4; k++) if (strlen(keywords[k]) == i && !strncasecmp(t, keywords[k], i)) return 0;
    return i;
}
/* @scope's prelude: [(<scope-start>)]? [to (<scope-end>)]? */
static bool scope_part(const struct css_rule_info *r, bool end, sbuf *out) {
    const char *t = prelude_text(r);
    size_t n = r->prelude_len, i = 0;
    while (i < n && is_space((unsigned char)t[i])) i++;
    bool start_seen = i < n && t[i] == '(';
    for (int part = 0; part < 2; part++) {
        while (i < n && is_space((unsigned char)t[i])) i++;
        if (part == 1) {
            if (n - i < 2 || strncasecmp(t + i, "to", 2)) return false;
            i += 2;
            while (i < n && is_space((unsigned char)t[i])) i++;
        } else if (!start_seen) continue;
        if (i >= n || t[i] != '(') return false;
        size_t close = i + 1;
        for (int depth = 1; close < n; close++) {
            if (t[close] == '(') depth++;
            else if (t[close] == ')' && !--depth) break;
        }
        if ((part == 1) == end) {
            size_t a = i + 1, z = close < n ? close : n;
            trim(t, &a, &z);
            put_collapsed(out, t + a, z - a);
            return true;
        }
        i = close < n ? close + 1 : n;
    }
    return false;
}
bool cssom_attribute(struct cssom_sheet *s, const struct css_rule_info *r, const char *name, sbuf *out) {
    (void)s;
    if (!r) return false;
    uint32_t type = r->type;
    if (!strcmp(name, "selectorText")) {
        if (type == CSS_RULE_STYLE) selector_text(r, out);
        else if (type == CSS_RULE_PAGE) prelude(r, out);
        else return false;
        return true;
    }
    if (!strcmp(name, "keyText") && type == CSS_RULE_KEYFRAME) {
        if (!key_text(prelude_text(r), r->prelude_len, out)) prelude(r, out);
        return true;
    }
    if (!strcmp(name, "name")) {
        if (type == CSS_RULE_KEYFRAMES || type == CSS_RULE_COUNTER_STYLE) name_value(prelude_text(r), r->prelude_len, out);
        else if (type == CSS_RULE_PROPERTY || type == CSS_RULE_FONT_PALETTE_VALUES || type == CSS_RULE_LAYER_BLOCK) prelude(r, out);
        else if (type == CSS_RULE_MARGIN) at_name(r, out);
        else return false;
        return true;
    }
    if (!strcmp(name, "fontFamily")) {
        if (type == CSS_RULE_FONT_FEATURE_VALUES) prelude(r, out);
        else if (type == CSS_RULE_FONT_PALETTE_VALUES) descriptor(r, "font-family", out);
        else return false;
        return true;
    }
    if (!strcmp(name, "basePalette") && type == CSS_RULE_FONT_PALETTE_VALUES) { descriptor(r, "base-palette", out); return true; }
    if (!strcmp(name, "overrideColors") && type == CSS_RULE_FONT_PALETTE_VALUES) { descriptor(r, "override-colors", out); return true; }
    if (!strcmp(name, "conditionText")) {
        if (type == CSS_RULE_MEDIA) media_of(prelude_text(r), r->prelude_len, out);
        else if (type == CSS_RULE_SUPPORTS || type == CSS_RULE_CONTAINER) prelude(r, out);
        else return false;
        return true;
    }
    if (type == CSS_RULE_CONTAINER && (!strcmp(name, "containerName") || !strcmp(name, "containerQuery"))) {
        size_t n = container_name(r), a = n, z = r->prelude_len;
        if (name[9] == 'N') { sbuf raw = {0}; sb_put(&raw, prelude_text(r), n); css_unescape(out, raw.p ? raw.p : "", raw.n); sb_free(&raw); }
        else { trim(prelude_text(r), &a, &z); put_collapsed(out, prelude_text(r) + a, z - a); }
        return true;
    }
    if (type == CSS_RULE_SCOPE && (!strcmp(name, "start") || !strcmp(name, "end"))) return scope_part(r, name[0] == 'e', out);
    if (type == CSS_RULE_IMPORT && r->import) {
        if (!strcmp(name, "href")) { sb_puts(out, r->import->href ? r->import->href : ""); return true; }
        if (!strcmp(name, "layerName")) { if (!r->import->layer) return false; sb_puts(out, r->import->layer); return true; }
        if (!strcmp(name, "supportsText")) { if (!r->import->supports) return false; sb_puts(out, r->import->supports); return true; }
        return false;
    }
    if (type == CSS_RULE_NAMESPACE && (!strcmp(name, "prefix") || !strcmp(name, "namespaceURI"))) {
        sbuf prefix = {0}, uri = {0};
        bool has_prefix;
        namespace_parts(r, &prefix, &uri, &has_prefix);
        sbuf *part = name[0] == 'p' ? &prefix : &uri;
        sb_put(out, part->p ? part->p : "", part->n);
        sb_free(&prefix); sb_free(&uri);
        return true;
    }
    if (type == CSS_RULE_LAYER_STATEMENT && !strcmp(name, "nameList")) {
        const char *t = prelude_text(r);
        for (size_t at = 0;;) {
            size_t end = piece_end(t, at, r->prelude_len), a = at, z = end;
            trim(t, &a, &z);
            put_collapsed(out, t + a, z - a);
            sb_putc(out, 0);
            if (end >= r->prelude_len) break;
            at = end + 1;
        }
        return true;
    }
    if (type == CSS_RULE_PROPERTY) {
        sbuf value = {0};
        if (!strcmp(name, "syntax")) {
            if (descriptor(r, "syntax", &value) && value.n >= 2 && (value.p[0] == '"' || value.p[0] == '\''))
                css_unescape(out, value.p + 1, value.n - 2);
            sb_free(&value);
            return true;
        }
        if (!strcmp(name, "inherits")) {
            sb_puts(out, descriptor(r, "inherits", &value) && value.n == 4 && !strncasecmp(value.p, "true", 4) ? "true" : "false");
            sb_free(&value);
            return true;
        }
        if (!strcmp(name, "initialValue")) {
            bool found = descriptor(r, "initial-value", &value);
            if (found) sb_put(out, value.p, value.n);
            sb_free(&value);
            return found;
        }
        return false;
    }
    if (type == CSS_RULE_COUNTER_STYLE) {
        const char *d = counter_descriptor(name);
        if (!d) return false;
        descriptor(r, d, out);
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------- changes to one rule's text */
static int change_text(struct cssom_sheet *s, struct cssom_tree *tree, struct css_rule_info *r, size_t at, size_t end,
                       const char *text, size_t n, const char *prelude_new, size_t prelude_n, bool marker) {
    struct change c = {.tree = tree, .count = 1, .kind = CHANGE_SAME, .node = r, .prelude = prelude_new, .prelude_n = prelude_n};
    c.edits[0] = (struct edit){at, end, text, n, marker ? 0 : SIZE_MAX};
    return rebuild(s, &c);
}
/* a rule's prelude replaced by text (comment-free, trimmed) */
static int set_prelude(struct cssom_sheet *s, struct cssom_tree *tree, struct css_rule_info *r, const char *text, size_t n) {
    struct cssom_parse *p = parse_of(s, tree);
    size_t at = rule_at(p, r) + r->prelude_at;
    /* an empty prelude stands where the name ends: keep the two apart */
    sbuf b = {0};
    bool apart = !r->prelude_len && n && at > 0 && !is_space((unsigned char)p->source[at - 1]);
    if (apart) sb_putc(&b, ' ');
    sb_put(&b, text, n);
    if (!r->prelude_len && n && r->block) sb_putc(&b, ' ');
    int error = change_text(s, tree, r, at, at + r->prelude_len, b.p ? b.p : "", b.n, text, n, false);
    sb_free(&b);
    return error;
}
/* text as the parser reads it, trimmed (malloc) */
static char *clean_text(const char *text, size_t length, size_t *n) {
    size_t clean_n;
    char *clean = css_preprocess(text ? text : "", text ? length : 0, &clean_n);
    if (!clean) return NULL;
    size_t a = 0, z = clean_n;
    trim(clean, &a, &z);
    memmove(clean, clean + a, z - a);
    clean[z - a] = 0;
    *n = z - a;
    return clean;
}
int cssom_set_attribute(struct cssom_sheet *s, uint32_t id, const char *name, const char *value, size_t length) {
    struct cssom_tree *tree;
    struct css_rule_info *r = find(s, id, &tree);
    if (!r) return CSSOM_OK;
    size_t n;
    char *text = clean_text(value, length, &n);
    if (!text) return CSSOM_OOM;
    int error = CSSOM_OK;
    sbuf b = {0};
    bool structural = has_char(text, n, "{};");
    if (!strcmp(name, "selectorText") && (r->type == CSS_RULE_STYLE || r->type == CSS_RULE_PAGE)) {
        /* parse a group of selectors (or page selectors) in the rule's place: on failure nothing changes */
        if (!structural && (n || r->type == CSS_RULE_PAGE)) error = set_prelude(s, tree, r, text, n);
        if (error == CSSOM_SYNTAX) error = CSSOM_OK;
    } else if (!strcmp(name, "keyText") && r->type == CSS_RULE_KEYFRAME) {
        error = structural || !key_text(text, n, &b) ? CSSOM_SYNTAX : set_prelude(s, tree, r, b.p, b.n);
    } else if (!strcmp(name, "name") && (r->type == CSS_RULE_KEYFRAMES || r->type == CSS_RULE_COUNTER_STYLE)) {
        /* the value as the name (not its CSS text): an identifier or a string */
        sbuf raw = {0};
        sb_put(&raw, value ? value : "", value ? length : 0);
        if (r->type == CSS_RULE_KEYFRAMES) put_keyframes_name(&b, sb_cstr(&raw));
        else if (raw.n && !str_ieq(sb_cstr(&raw), "none")) css_serialize_ident(&b, sb_cstr(&raw));
        sb_free(&raw);
        if (b.n) error = set_prelude(s, tree, r, b.p, b.n);
        if (error == CSSOM_SYNTAX) error = CSSOM_OK;
    } else if (!strcmp(name, "fontFamily") && r->type == CSS_RULE_FONT_FEATURE_VALUES) {
        if (!structural && n) error = set_prelude(s, tree, r, text, n);
        if (error == CSSOM_SYNTAX) error = CSSOM_OK;
    } else if (r->type == CSS_RULE_COUNTER_STYLE && counter_descriptor(name)) {
        const char *d; size_t dn;
        cssom_declarations(r, &d, &dn);
        sbuf block = {0};
        css_decl_serialize(d, dn, descriptor_check, &block, NULL);
        int status = css_decl_set(block.p ? block.p : "", block.n, counter_descriptor(name), text, NULL, descriptor_check, &b);
        sb_free(&block);
        if (status > 0) error = cssom_set_declarations(s, id, b.p ? b.p : "", b.n);
    }
    sb_free(&b);
    free(text);
    return error;
}
int cssom_set_declarations(struct cssom_sheet *s, uint32_t id, const char *value, size_t length) {
    struct cssom_tree *tree;
    struct css_rule_info *r = find(s, id, &tree);
    if (!r || !has_declarations(r)) return CSSOM_OK;
    size_t n;
    char *text = clean_text(value, length, &n);
    if (!text) return CSSOM_OOM;
    sbuf decls = {0};
    bool ok = css_decl_serialize(text, n, cssom_declaration_check(r), &decls, NULL);
    free(text);
    if (!ok) { sb_free(&decls); return CSSOM_OOM; }
    int error = CSSOM_OK;
    if (css_decl_balanced(decls.p ? decls.p : "", decls.n)) {
        struct cssom_parse *p = parse_of(s, tree);
        if (plain_nested(r)) {
            /* its own text cannot keep it apart once changed: the marker form */
            sbuf m = {0};
            marker_text(&m, decls.p ? decls.p : "", decls.n);
            error = change_text(s, tree, r, rule_at(p, r), rule_end(p, r), m.p, m.n, NULL, 0, true);
            sb_free(&m);
        } else {
            size_t at = rule_at(p, r) + r->decls_at;
            error = change_text(s, tree, r, at, at + r->decls_len, decls.p ? decls.p : "", decls.n, NULL, 0, false);
        }
        if (error == CSSOM_SYNTAX) error = CSSOM_OK;
    }
    sb_free(&decls);
    return error;
}

/* ---------------------------------------------------------------- MediaList */
static void media_split(const char *t, size_t n, void (*each)(const char *, size_t, void *), void *data) {
    for (size_t at = 0; n;) {
        size_t end = piece_end(t, at, n), a = at, z = end;
        trim(t, &a, &z);
        if (a < z) each(t + a, z - a, data);
        if (end >= n) break;
        at = end + 1;
    }
}
struct media_edit { const char *m; size_t mn; sbuf out; bool found, keep_others; };
static void media_keep(const char *q, size_t n, void *data) {
    struct media_edit *e = data;
    bool same = n == e->mn && !memcmp(q, e->m, n);
    if (same) e->found = true;
    if (same && !e->keep_others) return;
    if (e->out.n) sb_puts(&e->out, ", ");
    sb_put(&e->out, q, n);
}
static int media_store(struct cssom_sheet *s, struct cssom_tree *tree, struct css_rule_info *r, const char *list, size_t n) {
    if (!r) {
        char *media = n ? strndup(list, n) : NULL;
        if (n && !media) return CSSOM_OOM;
        free(s->media); s->media = media;
        cssom_changed(s);
        return CSSOM_OK;
    }
    struct cssom_parse *p = parse_of(s, tree);
    if (r->type == CSS_RULE_MEDIA) {
        /* the prelude, between the name and the block */
        size_t at = rule_at(p, r) + name_end(r), end = rule_at(p, r) + r->block_at - 1;
        sbuf b = {0};
        sb_putc(&b, ' '); sb_put(&b, list, n); sb_putc(&b, ' ');
        int error = change_text(s, tree, r, at, end, b.p, b.n, list, n, false);
        sb_free(&b);
        return error;
    }
    sbuf b = {0};
    import_text(r->import, list, n, &b);
    int error = change_text(s, tree, r, rule_at(p, r), rule_end(p, r), b.p, b.n, NULL, 0, false);
    sb_free(&b);
    return error;
}
int cssom_media(struct cssom_sheet *s, uint32_t id, int op, const char *text, size_t length, sbuf *out) {
    struct cssom_tree *tree = NULL;
    struct css_rule_info *r = id ? find(s, id, &tree) : NULL;
    if (id && (!r || (r->type != CSS_RULE_MEDIA && (r->type != CSS_RULE_IMPORT || !r->import)))) return CSSOM_OK;
    sbuf current = {0};
    if (!r) sb_puts(&current, s->media ? s->media : "");
    else if (r->type == CSS_RULE_MEDIA) media_of(prelude_text(r), r->prelude_len, &current);
    else import_media(r, &current);
    int error = CSSOM_OK;
    if (op == 0) sb_put(out, current.p ? current.p : "", current.n);
    else {
        sbuf parsed = {0};
        if (!media_of(text, length, &parsed)) error = CSSOM_OOM;
        else if (op == 1) error = media_store(s, tree, r, parsed.p ? parsed.p : "", parsed.n);
        else if (!parsed.n || piece_end(parsed.p, 0, parsed.n) < parsed.n) {
            /* parse a media query: a list of other than one query is none, and nothing changes */
        } else {
            struct media_edit e = {parsed.p, parsed.n, {0}, false, op == 2};
            media_split(current.p ? current.p : "", current.n, media_keep, &e);
            if (op == 2 && !e.found) {
                if (e.out.n) sb_puts(&e.out, ", ");
                sb_put(&e.out, parsed.p, parsed.n);
            }
            if (op == 3 && !e.found) error = CSSOM_NOT_FOUND;
            else if (op == 3 || !e.found) error = media_store(s, tree, r, e.out.p ? e.out.p : "", e.out.n);
            sb_free(&e.out);
        }
        sb_free(&parsed);
        if (error == CSSOM_SYNTAX) error = CSSOM_OK;
    }
    sb_free(&current);
    return error;
}

/* ---------------------------------------------------------------- @keyframes and @font-feature-values */
int64_t cssom_keyframe_find(struct cssom_sheet *s, uint32_t id, const char *key, size_t length) {
    struct css_rule_info *r = find(s, id, NULL);
    if (!r || r->type != CSS_RULE_KEYFRAMES) return -1;
    sbuf wanted = {0};
    size_t n;
    char *text = clean_text(key, length, &n);
    int64_t found = -1;
    if (text && key_text(text, n, &wanted)) {
        for (uint32_t i = 0; i < r->child_count; i++) {
            sbuf k = {0};
            const struct css_rule_info *c = r->children[i];
            if (key_text(prelude_text(c), c->prelude_len, &k) && k.n == wanted.n && !memcmp(k.p, wanted.p, k.n)) found = i;
            sb_free(&k);
        }
    }
    free(text);
    sb_free(&wanted);
    return found;
}
bool cssom_feature_values(struct cssom_sheet *s, const struct css_rule_info *r, const char *block, sbuf *out) {
    (void)s;
    int which = block ? feature_block(block, strlen(block)) : -1;
    if (!r || r->type != CSS_RULE_FONT_FEATURE_VALUES || which < 0) return false;
    struct features f;
    features_read(r, &f);
    sb_put(out, f.entries[which].p ? f.entries[which].p : "", f.entries[which].n);
    features_free(&f);
    return true;
}
int cssom_set_feature_values(struct cssom_sheet *s, uint32_t id, const char *block, const char *name,
                             const uint32_t *values, uint32_t count) {
    struct cssom_tree *tree;
    struct css_rule_info *r = find(s, id, &tree);
    int which = block ? feature_block(block, strlen(block)) : -1;
    if (!r || r->type != CSS_RULE_FONT_FEATURE_VALUES || which < 0) return CSSOM_OK;
    struct features f;
    features_read(r, &f);
    if (!name) f.entries[which].n = 0;
    else if (!values) feature_drop(&f.entries[which], name);
    else {
        sbuf list = {0};
        for (uint32_t i = 0; i < count; i++) {
            char number[16];
            snprintf(number, sizeof number, "%s%u", i ? " " : "", (unsigned)values[i]);
            sb_puts(&list, number);
        }
        feature_put(&f.entries[which], name, sb_cstr(&list));
        sb_free(&list);
    }
    /* the rule's block, rewritten */
    struct cssom_parse *p = parse_of(s, tree);
    sbuf b = {0};
    features_text(&f, &b);
    sb_putc(&b, ' ');
    features_free(&f);
    size_t at = rule_at(p, r) + r->block_at;
    int error = change_text(s, tree, r, at, at + r->block_len, b.p, b.n, NULL, 0, false);
    sb_free(&b);
    return error == CSSOM_SYNTAX ? CSSOM_OK : error;
}

/* ---------------------------------------------------------------- insert and remove a CSS rule (CSSOM 6.4.2) */
/* A nested rule list: a style rule's, or a group rule's below one */
static bool nested_list(const struct css_rule_info *container) {
    for (const struct css_rule_info *a = container; a; a = a->parent) if (a->type == CSS_RULE_STYLE) return true;
    return false;
}
/* parse a CSS rule in the list's place: text closed as end of input would
   close it, into out; its kind; marker when it became nested declarations */
static int candidate(struct cssom_sheet *s, const struct cssom_parse *p, const struct css_rule_info *container,
                     const char *text, size_t length, sbuf *out, uint32_t *kind, bool *marker) {
    *marker = false; *kind = 0;
    size_t n;
    char *clean = clean_text(text, length, &n);
    if (!clean) return CSSOM_OOM;
    if (!n) { free(clean); return CSSOM_SYNTAX; }
    sbuf t = {0}, tail = {0};
    bool stray;
    sb_put(&t, clean, n);
    free(clean);
    closers(t.p, t.n, &tail, &stray);
    if (stray) { sb_free(&t); sb_free(&tail); return CSSOM_SYNTAX; }
    sb_put(&t, tail.p ? tail.p : "", tail.n);
    sb_free(&tail);
    if (t.p[t.n - 1] != '}' && t.p[t.n - 1] != ';') sb_putc(&t, ';');
    int error;
    /* @import and @namespace parse only at the top, before other rules */
    struct cssom_parse alone;
    error = parse_text(s->document, s->base, &alone, t.p, t.n, NULL, 0);
    if (error) { sb_free(&t); return error; }
    if (alone.count == 1 && rule_at(&alone, alone.top[0]) == 0 && alone.top[0]->text_len == t.n &&
        (alone.top[0]->type == CSS_RULE_IMPORT || alone.top[0]->type == CSS_RULE_NAMESPACE))
        *kind = alone.top[0]->type;
    parse_free(&alone);
    if (!*kind) {
        sbuf context = {0};
        uint32_t namespaces, depth = context_text(p, container, 0, &context, &namespaces);
        if (depth == UINT32_MAX) { sb_free(&t); sb_free(&context); return CSSOM_OOM; }
        size_t base = context.n;
        sb_put(&context, t.p, t.n);
        for (uint32_t i = 0; i < depth; i++) sb_puts(&context, "\n}");
        struct cssom_parse in;
        error = parse_text(s->document, s->base, &in, context.p, context.n, NULL, 0);
        sb_free(&context);
        if (error) { sb_free(&t); return error; }
        struct css_rule_info *const *list = in.top + (namespaces < in.count ? namespaces : in.count);
        uint32_t count = namespaces < in.count ? in.count - namespaces : 0;
        if (container) {
            struct css_rule_info *copy = context_rule(&in, namespaces, depth - 1);
            list = copy ? copy->children : NULL;
            count = copy ? copy->child_count : 0;
        }
        if (count == 1 && rule_at(&in, list[0]) == base && list[0]->text_len == t.n) *kind = list[0]->type;
        parse_free(&in);
    }
    if (*kind == CSS_RULE_NESTED_DECLARATIONS || (!*kind && nested_list(container))) {
        /* insert a CSS rule, step 4: nested declarations of what parses as a declaration block */
        sbuf decls = {0};
        bool ok = css_decl_serialize(t.p, t.n, NULL, &decls, NULL);
        sb_free(&t);
        if (!ok) { sb_free(&decls); return CSSOM_OOM; }
        if (!decls.n || !css_decl_balanced(decls.p, decls.n)) { sb_free(&decls); return CSSOM_SYNTAX; }
        marker_text(out, decls.p, decls.n);
        sb_free(&decls);
        *kind = CSS_RULE_NESTED_DECLARATIONS; *marker = true;
        return CSSOM_OK;
    }
    if (!*kind) { sb_free(&t); return CSSOM_SYNTAX; }
    sb_put(out, t.p, t.n);
    sb_free(&t);
    return CSSOM_OK;
}
/* insert a CSS rule, steps 6 and 7, at the top of a sheet (CSS Cascade 5,
   CSS Namespaces 3: @import and @namespace precede every other rule but
   @layer statements, @import first) */
static int top_order(const struct cssom_parse *p, uint32_t kind, uint32_t index) {
    bool import_before = false, namespace_before = false, import_after = false, namespace_after = false, other = false;
    for (uint32_t i = 0; i < p->count; i++) {
        uint32_t t = p->top[i]->type;
        if (i < index) {
            if (kind == CSS_RULE_IMPORT && t != CSS_RULE_IMPORT && t != CSS_RULE_LAYER_STATEMENT) return CSSOM_HIERARCHY;
            if (kind == CSS_RULE_IMPORT && t == CSS_RULE_LAYER_STATEMENT && import_before) return CSSOM_HIERARCHY;
            if (kind == CSS_RULE_NAMESPACE && t != CSS_RULE_IMPORT && t != CSS_RULE_NAMESPACE && t != CSS_RULE_LAYER_STATEMENT)
                return CSSOM_HIERARCHY;
            import_before |= t == CSS_RULE_IMPORT; namespace_before |= t == CSS_RULE_NAMESPACE;
        } else {
            import_after |= t == CSS_RULE_IMPORT; namespace_after |= t == CSS_RULE_NAMESPACE;
        }
        other |= t != CSS_RULE_IMPORT && t != CSS_RULE_NAMESPACE;
    }
    if (kind == CSS_RULE_IMPORT) return CSSOM_OK;
    if (kind == CSS_RULE_NAMESPACE) return import_after ? CSSOM_HIERARCHY : other ? CSSOM_INVALID_STATE : CSSOM_OK;
    if (kind == CSS_RULE_LAYER_STATEMENT)
        return (import_before && import_after) || (namespace_after && (namespace_before || import_before)) ?
            CSSOM_HIERARCHY : CSSOM_OK;
    return import_after || namespace_after ? CSSOM_HIERARCHY : CSSOM_OK;
}
int cssom_insert(struct cssom_sheet *s, uint32_t parent, const char *text, size_t length, uint32_t index) {
    if (!s) return CSSOM_INACTIVE;
    struct cssom_tree *tree = NULL;
    struct css_rule_info *container = parent ? find(s, parent, &tree) : NULL;
    if (parent && !container) return CSSOM_INVALID_STATE;
    struct cssom_parse *p = parse_of(s, tree);
    uint32_t count;
    struct css_rule_info **list = children(p, container, &count);
    if (index > count) return CSSOM_INDEX;
    if (count == UINT32_MAX || s->next_rule_id == UINT32_MAX) return CSSOM_OOM;
    sbuf rule = {0};
    uint32_t kind;
    bool marker;
    int error = candidate(s, p, container, text, length, &rule, &kind, &marker);
    if (!error && kind == CSS_RULE_IMPORT && s->constructed && !container) error = CSSOM_SYNTAX;
    if (!error && container && (kind == CSS_RULE_IMPORT || kind == CSS_RULE_NAMESPACE)) error = CSSOM_HIERARCHY;
    if (!error && !container) error = top_order(p, kind, index);
    if (error) { sb_free(&rule); return error; }
    size_t at = index < count ? rule_at(p, list[index]) :
        container ? rule_at(p, container) + container->block_at + container->block_len : p->length;
    /* text before it that does not end a declaration or a rule: end it */
    size_t k = at;
    while (k > 0 && is_space((unsigned char)p->source[k - 1])) k--;
    sbuf b = {0};
    if (k > 0 && !strchr(";{}", p->source[k - 1])) sb_putc(&b, ';');
    sb_putc(&b, '\n');
    size_t prefix = b.n;
    sb_put(&b, rule.p, rule.n);
    sb_putc(&b, '\n');
    struct change c = {.tree = tree, .count = 1, .kind = CHANGE_INSERT, .container = container, .index = index,
                       .insert_at = at + prefix, .insert_n = rule.n};
    c.edits[0] = (struct edit){at, at, b.p, b.n, marker ? prefix : SIZE_MAX};
    error = rebuild(s, &c);
    sb_free(&b); sb_free(&rule);
    return error;
}
int cssom_delete(struct cssom_sheet *s, uint32_t parent, uint32_t index) {
    if (!s) return CSSOM_INACTIVE;
    struct cssom_tree *tree = NULL;
    struct css_rule_info *container = parent ? find(s, parent, &tree) : NULL;
    if (parent && !container) return CSSOM_INVALID_STATE;
    struct cssom_parse *p = parse_of(s, tree);
    uint32_t count;
    struct css_rule_info **list = children(p, container, &count);
    if (index >= count) return CSSOM_INDEX;
    struct css_rule_info *r = list[index];
    if (!container && r->type == CSS_RULE_NAMESPACE)
        for (uint32_t i = 0; i < count; i++)
            if (list[i]->type != CSS_RULE_IMPORT && list[i]->type != CSS_RULE_NAMESPACE) return CSSOM_INVALID_STATE;
    struct change c = {.tree = tree, .count = 1, .kind = CHANGE_DELETE, .node = r};
    c.edits[0] = (struct edit){rule_at(p, r), rule_end(p, r), "", 0, SIZE_MAX};
    /* Nested declarations that the removal would bring beside the rule's own
       declarations, or beside other nested declarations, keep apart as the
       marker form. */
    struct css_rule_info *next = index + 1 < count ? list[index + 1] : NULL;
    sbuf m = {0};
    if (next && plain_nested(next) &&
        ((index == 0 && container && (container->type == CSS_RULE_STYLE || container->type == CSS_RULE_PAGE)) ||
         (index > 0 && plain_nested(list[index - 1])))) {
        sbuf decls = {0};
        declarations(next, &decls);
        marker_text(&m, decls.p ? decls.p : "", decls.n);
        sb_free(&decls);
        c.edits[1] = (struct edit){rule_at(p, next), rule_end(p, next), m.p, m.n, 0};
        c.count = 2;
    }
    int error = rebuild(s, &c);
    sb_free(&m);
    return error;
}
