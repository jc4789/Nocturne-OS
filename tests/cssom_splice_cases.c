/* cssom.c's changes rule by rule, on the host. After each insertRule,
   deleteRule and change of a rule's text, a sheet's cascade AST (its style
   rules with their selectors, declarations, conditions and layers; its
   @font-face, @keyframes and layer lists; the rule index the cascade matches
   with) and its rule tree are what one parse of the sheet's whole text gives;
   the ids CSSRule objects stand for still find their rules; the memory of
   replaced rules stays bounded; and inserting rules one by one costs the same
   per rule however many the sheet already holds (CSS-in-JS inserts thousands
   into one sheet). tests/check_cssom_rules_native.py builds this with css.c's
   internals in view and cssom.c beside it. Built with OLD_SOURCE against the
   source before the changes were made rule by rule (8cbe1194), where every
   change parsed the whole sheet again, the timing fails. */
#include "css.c"
#include "cssom.h"
#include <stdio.h>
#include <time.h>

static int passed, failed;
bool web_native_checkpoint(web_doc *d) { (void)d; return true; }
int web_resolve_url_owned(const char *base, const char *rel, char **out) {
    size_t a = strlen(base), b = strlen(rel);
    *out = malloc(a + b + 1);
    if (!*out) return -1;
    memcpy(*out, base, a); memcpy(*out + a, rel, b + 1);
    return 1;
}
static web_doc document_;
static node_t root_;
static void check(const char *label, bool ok, const char *got, const char *want) {
    if (ok) { passed++; return; }
    failed++;
    printf("FAIL %s\n", label);
    if (got || want) printf("  got:\n%s\n  want:\n%s\n", got ? got : "(null)", want ? want : "(null)");
}

/* ---------------------------------------------------------------- an AST, as text */
/* layer declarations by identity: the number each first stands at */
struct seen { const void **v; size_t n, cap; };
static size_t seen_at(struct seen *m, const void *p) {
    for (size_t i = 0; i < m->n; i++) if (m->v[i] == p) return i;
    if (m->n == m->cap) { m->cap = m->cap ? m->cap * 2 : 32; m->v = realloc(m->v, m->cap * sizeof *m->v); }
    m->v[m->n] = p;
    return m->n++;
}
static void put_media(sbuf *b, const struct mcond *m) {
    for (; m; m = m->up) {
        sb_puts(b, m->container ? " @container(" : " @media(");
        if (m->name) { sb_puts(b, m->name); sb_putc(b, ' '); }
        sb_puts(b, m->q ? m->q : "");
        sb_putc(b, ')');
    }
}
static void put_layer(sbuf *b, struct seen *layers, const struct css_layer_decl *l) {
    for (; l; l = l->parent) {
        char t[48];
        snprintf(t, sizeof t, " layer#%zu=", seen_at(layers, l));
        sb_puts(b, t);
        sb_puts(b, l->name ? l->name : "(anonymous)");
    }
}
static void put_decls(sbuf *b, const struct decl *d, int n) {
    for (int i = 0; i < n; i++) {
        char t[32];
        if (d[i].p) { snprintf(t, sizeof t, " p%d", css_prop_index(d[i].p)); sb_puts(b, t); }
        else { sb_putc(b, ' '); sb_puts(b, d[i].name ? d[i].name : ""); }
        sb_putc(b, '='); sb_puts(b, d[i].value ? d[i].value : "");
        if (d[i].important) sb_puts(b, " !important");
        sb_putc(b, ';');
    }
}
static size_t ordinal(const sheet_t *sh, const struct rule *r) {
    size_t i = 0;
    for (const struct rule *q = sh->first; q; q = q->next, i++) if (q == r) return i;
    return SIZE_MAX;
}
/* the lists the cascade reads, and whether each list's tail is its last element */
static void dump_ast(const sheet_t *sh, sbuf *b) {
    struct seen layers = {0};
    for (const struct css_layer_event *e = sh->layers_first; e; e = e->next) {
        sb_puts(b, "layer event"); put_layer(b, &layers, e->layer); put_media(b, e->media);
        if (e->import) sb_puts(b, " import");
        sb_putc(b, '\n');
        if (!e->next && e != sh->layers_last) sb_puts(b, "BAD layers_last\n");
    }
    if (!sh->layers_first && sh->layers_last) sb_puts(b, "BAD layers_last\n");
    for (const struct rule *r = sh->first; r; r = r->next) {
        sb_puts(b, "rule ");
        ser_list(b, r->sel);
        sb_puts(b, " {"); put_decls(b, r->decls, r->ndecls); sb_puts(b, " }");
        put_media(b, r->media); put_layer(b, &layers, r->layer);
        sb_putc(b, '\n');
        if (!r->next && r != sh->last) sb_puts(b, "BAD last\n");
    }
    if (!sh->first && sh->last) sb_puts(b, "BAD last\n");
    for (const struct font_face *f = sh->fonts; f; f = f->next) {
        char t[96];
        snprintf(t, sizeof t, "font %s %u-%u%s ranges=%zu", f->family ? f->family : "", f->weight_lo, f->weight_hi,
                 f->italic ? " italic" : "", f->range_count);
        sb_puts(b, t);
        for (const struct font_source *src = f->sources; src; src = src->next) { sb_puts(b, " src="); sb_puts(b, src->url ? src->url : ""); }
        for (int i = 0; i < CSS_FONT_DESCRIPTORS; i++) if (f->desc[i]) { sb_puts(b, " d="); sb_puts(b, f->desc[i]); }
        put_media(b, f->media); put_layer(b, &layers, f->layer);
        sb_putc(b, '\n');
        if (!f->next && f != sh->fonts_last) sb_puts(b, "BAD fonts_last\n");
    }
    if (!sh->fonts && sh->fonts_last) sb_puts(b, "BAD fonts_last\n");
    for (const struct opacity_keyframes *k = sh->motions; k; k = k->next) {
        sb_puts(b, "keyframes "); sb_puts(b, k->name ? k->name : "");
        for (const struct opacity_frame *f = k->first; f; f = f->next) {
            char t[32];
            snprintf(t, sizeof t, " %g{", f->offset);
            sb_puts(b, t); put_decls(b, f->decls, f->count); sb_puts(b, " }");
        }
        put_media(b, k->media); put_layer(b, &layers, k->layer);
        sb_putc(b, '\n');
        if (!k->next && k != sh->motions_last) sb_puts(b, "BAD motions_last\n");
    }
    if (!sh->motions && sh->motions_last) sb_puts(b, "BAD motions_last\n");
    for (const struct css_namespace *n = sh->namespaces; n; n = n->next) {
        sb_puts(b, "namespace "); sb_puts(b, n->prefix ? n->prefix : "(default)"); sb_putc(b, ' '); sb_puts(b, n->uri ? n->uri : "");
        sb_putc(b, '\n');
    }
    char t[48];
    snprintf(t, sizeof t, "import urls %u\n", sh->import_urls);
    sb_puts(b, t);
    /* distinct layer declarations have distinct ids within the sheet's count:
       a cascade's layer map is an array by id */
    for (size_t i = 0; i < layers.n; i++) {
        const struct css_layer_decl *l = layers.v[i];
        if (l->id >= sh->layer_count) sb_puts(b, "BAD layer id past layer_count\n");
        for (size_t j = 0; j < i; j++) if (((const struct css_layer_decl *)layers.v[j])->id == l->id) sb_puts(b, "BAD layer id twice\n");
    }
    free(layers.v);
}
/* the rule tree: each rule's kind, depth and serialization; the counts and tails */
static void dump_tree(const struct css_rule_info *r, int depth, sbuf *b) {
    for (; r; r = r->next) {
        char t[48];
        snprintf(t, sizeof t, "%*s%u: ", depth * 2, "", r->type);
        sb_puts(b, t);
        cssom_serialize(NULL, r, b);
        sb_putc(b, '\n');
        uint32_t n = 0;
        for (const struct css_rule_info *c = r->first; c; c = c->next) n++;
        if (n != r->child_count) sb_puts(b, "BAD child_count\n");
        dump_tree(r->first, depth + 1, b);
    }
}
static void dump_sheet(sheet_t *sh, sbuf *b) {
    uint32_t count, n = 0;
    struct css_rule_info *first = css_sheet_rules(sh, &count);
    for (struct css_rule_info *r = first; r; r = r->next) { n++; if (!r->next && r != sh->cssom_last) sb_puts(b, "BAD cssom_last\n"); }
    if (n != count) sb_puts(b, "BAD cssom_count\n");
    dump_tree(first, 0, b);
    dump_ast(sh, b);
}
/* the rule index as the cascade matches with it: each entry's bucket, rule,
   order and condition, sorted */
static int line_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
struct lines { char **v; size_t n, cap; };
static void line_add(struct lines *l, sbuf *b) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 64; l->v = realloc(l->v, l->cap * sizeof *l->v); }
    l->v[l->n++] = strdup(sb_cstr(b));
    b->n = 0;
}
static void dump_entries(const sheet_t *sh, const struct css_index *x, const char *bucket, const char *key,
                         const struct ient *e, struct lines *out) {
    sbuf b = {0};
    for (; e; e = e->next) {
        char t[96];
        snprintf(t, sizeof t, "%s %s rule#%zu order=%u pseudo=%u spec=%u sel=", bucket, key ? key : "", ordinal(sh, e->r),
                 e->order, (unsigned)e->pseudo, e->sel->spec);
        sb_puts(&b, t);
        ser_chain(&b, e->sel->right);
        if (e->media) put_media(&b, x->media[e->media - 1]);
        line_add(out, &b);
    }
    sb_free(&b);
}
static void dump_table(const sheet_t *sh, const struct css_index *x, const char *bucket, const struct htab *t, struct lines *out) {
    for (uint32_t i = 0; i < t->cap; i++)
        for (const struct bucket *k = t->v[i]; k; k = k->next) dump_entries(sh, x, bucket, k->key, k->list, out);
}
static void dump_index(sheet_t *sh, sbuf *b) {
    struct css_index *x = sheet_index(&document_, sh);
    if (!x) { sb_puts(b, "no index\n"); return; }
    struct lines l = {0};
    dump_table(sh, x, "id", &x->ids, &l);
    dump_table(sh, x, "class", &x->classes, &l);
    dump_table(sh, x, "tag", &x->tags, &l);
    dump_entries(sh, x, "universal", NULL, x->univ, &l);
    dump_entries(sh, x, "part", NULL, x->parts, &l);
    if (l.n) qsort(l.v, l.n, sizeof *l.v, line_cmp);
    for (size_t i = 0; i < l.n; i++) { sb_puts(b, l.v[i]); sb_putc(b, '\n'); free(l.v[i]); }
    free(l.v);
    char t[160];
    snprintf(t, sizeof t, "rules %u forms %u containers %d siblings %d fonts %s motions %s\n", x->rules, x->form_features,
             x->containers, x->siblings, x->fonts == sh->fonts ? "head" : "OTHER", x->motions == sh->motions ? "head" : "OTHER");
    sb_puts(b, t);
}

/* ---------------------------------------------------------------- the sheet against its whole text */
/* the sheet's text: its top-level rules' in order (as parsed until a change
   was spliced in), and where cssom.c's nested-declarations markers stand in it */
static void marks_of(const struct css_rule_info *r, const char *base, size_t shift, uint32_t *m, uint32_t *n) {
    for (; r; r = r->next) {
        if (r->marker && *n < 4096) m[(*n)++] = (uint32_t)(shift + (size_t)(r->text - base));
        marks_of(r->first, base, shift, m, n);
    }
}
static void whole(struct cssom_sheet *s, sbuf *b, uint32_t *m, uint32_t *n) {
    *n = 0;
#ifndef OLD_SOURCE
    if (!s->p.spliced)
#endif
    {
        sb_put(b, s->p.source, s->p.length);
        marks_of(css_sheet_rules(s->p.ast, NULL), s->p.source, 0, m, n);
        return;
    }
    for (struct css_rule_info *r = css_sheet_rules(s->p.ast, NULL); r; r = r->next) {
        size_t at = b->n;
        sb_put(b, r->text, r->text_len);
        sb_putc(b, '\n');
        const struct css_rule_info *next = r->next;
        r->next = NULL;
        marks_of(r, r->text, at, m, n);
        r->next = (struct css_rule_info *)next;
    }
}
/* each rule's children by index, as cssom.c keeps them, for the serializer */
static void children_of(arena_t *a, struct css_rule_info *r) {
    for (; r; r = r->next) if (r->child_count) {
        r->children = ar_alloc(a, r->child_count * sizeof *r->children);
        uint32_t i = 0;
        for (struct css_rule_info *c = r->first; c; c = c->next) r->children[i++] = c;
        children_of(a, r->first);
    }
}
static bool compare(const char *label, struct cssom_sheet *s, bool index) {
    sbuf text = {0}, got = {0}, want = {0};
    static uint32_t marks[4096];
    uint32_t nmarks;
    whole(s, &text, marks, &nmarks);
    arena_t a = {0};
    sheet_t *fresh = css_parse_cssom_sheet(&document_, &a, text.p ? text.p : "", text.n, s->base, marks, nmarks);
    children_of(&a, css_sheet_rules(fresh, NULL));
    dump_sheet(s->p.ast, &got);
    dump_sheet(fresh, &want);
    if (index) { dump_index(s->p.ast, &got); dump_index(fresh, &want); }
    bool same = got.n == want.n && !memcmp(got.p, want.p, got.n) && !strstr(sb_cstr(&got), "BAD");
    /* the sheet's rules by index, and the ids every rule stands for */
    uint32_t count, i = 0;
    for (struct css_rule_info *r = css_sheet_rules(s->p.ast, &count); r; r = r->next, i++)
        if (i >= s->p.count || s->p.top[i] != r) { same = false; sb_puts(&got, "BAD top\n"); break; }
    if (i != s->p.count) { same = false; sb_puts(&got, "BAD top count\n"); }
    for (struct css_rule_info *r = s->p.count ? s->p.top[0] : NULL; r;) {
        bool attached;
        if (cssom_rule(s, r->id, &attached) != r || !attached) { same = false; sb_puts(&got, "BAD id\n"); break; }
#ifndef OLD_SOURCE
        uint32_t n;
        struct css_rule_info **list = r->parent ? r->parent->children : s->p.top;
        n = r->parent ? r->parent->child_count : s->p.count;
        if (r->index >= n || list[r->index] != r) { same = false; sb_puts(&got, "BAD index\n"); break; }
#endif
        if (r->first) r = r->first;
        else { while (r && !r->next) r = r->parent; r = r ? r->next : NULL; }
    }
    check(label, same, sb_cstr(&got), sb_cstr(&want));
    ar_free(&a);
    sb_free(&text); sb_free(&got); sb_free(&want);
    return same;
}

/* ---------------------------------------------------------------- changes */
static uint32_t seed = 12345;
static uint32_t pick(uint32_t n) { seed = seed * 1103515245u + 12345u; return n ? (seed >> 8) % n : 0; }
static const char *const pool[] = {
    ".x%u { color: red; }", "@media (max-width: %upx) { .m%u { color: blue } .m%u:hover { margin: 1px } }",
    "@keyframes k%u { 50%% { opacity: .5 } to { opacity: 1 } }", "@font-face { font-family: F%u; src: url(f%u.woff2); }",
    "@layer l%u { .y%u { margin: %upx } }", "@layer { .z%u { padding: 1px } }", "@layer s%u, t%u;",
    ".p%u { & .c { color: red } color: blue; > .k%u { margin: 0 } }", "@supports (color: red) { .su%u { color: red } }",
    "@page { margin: 1cm }", "@property --p%u { syntax: '*'; inherits: false; }", "#i%u, .j%u > a { top: %upx }",
    "@container (min-width: %upx) { .q%u { color: red } }", "@scope (.s%u) { .t { color: red } }",
    "@layer base { .b%u { color: green } }", "@starting-style { .st%u { opacity: 0 } }",
    "@media print { @keyframes pk%u { from { opacity: 0 } } .pr%u { color: red } }"};
static void pool_rule(char *out, size_t n) {
    unsigned k = pick(1000);
    snprintf(out, n, pool[pick(sizeof pool / sizeof *pool)], k, k, k, k);
}
static struct css_rule_info *top_of_kind(struct cssom_sheet *s, uint32_t a, uint32_t b, uint32_t c) {
    if (!s->p.count) return NULL;
    for (uint32_t tries = 0; tries < 8; tries++) {
        struct css_rule_info *r = s->p.top[pick(s->p.count)];
        if (r->type == a || r->type == b || r->type == c) return r;
    }
    return NULL;
}
/* the changes made (CSSOM_OK) of those tried, by kind: insert and delete at the top, inside */
static unsigned made[4], tried[4];
static void tally(int kind, int error) { tried[kind]++; made[kind] += error == CSSOM_OK; }
static void change(struct cssom_sheet *s, unsigned op, char *what, size_t what_n) {
    char rule[256];
    uint32_t count = s->p.count;
    if (!s->constructed && op % 4 == 0) {
        /* @import, @namespace and @layer statements where the order lets them: the whole text */
        static const char *const prefix[] = {"@import url(i%u.css);", "@namespace n%u url(http://n%u.test/);", "@layer p%u;"};
        unsigned k = pick(1000);
        snprintf(rule, sizeof rule, prefix[pick(3)], k, k);
        uint32_t at = pick(count < 4 ? count + 1 : 4);
        snprintf(what, what_n, "insertRule(%s, %u)", rule, at);
        tally(0, cssom_insert(s, 0, rule, strlen(rule), at));
        return;
    }
    switch (op % 10) {
    case 0: case 1: {
        pool_rule(rule, sizeof rule);
        uint32_t at = pick(count + 1);
        snprintf(what, what_n, "insertRule(%s, %u)", rule, at);
        tally(0, cssom_insert(s, 0, rule, strlen(rule), at));
        break;
    }
    case 2: {
        if (!count) break;
        uint32_t at = pick(count), n;
        /* a CSSRule object stands for it: it moves to a tree of its own */
        struct css_rule_info *r = cssom_child(s, 0, at, &n);
        uint32_t id = r->id;
        sbuf before = {0}, after = {0};
        cssom_serialize(s, r, &before);
        snprintf(what, what_n, "deleteRule(%u)", at);
        int error = cssom_delete(s, 0, at);
        tally(1, error);
        if (error == CSSOM_OK) {
            bool attached = true;
            struct css_rule_info *gone = cssom_rule(s, id, &attached);
            if (gone) cssom_serialize(s, gone, &after);
            check("a removed rule keeps its id, alone", gone && !attached && after.n == before.n &&
                  !memcmp(after.p, before.p, before.n), sb_cstr(&after), sb_cstr(&before));
        }
        sb_free(&before); sb_free(&after);
        break;
    }
    case 3: {
        struct css_rule_info *r = top_of_kind(s, CSS_RULE_STYLE, 0, 0);
        if (!r) break;
        snprintf(rule, sizeof rule, ".sel%u, .other > b", pick(100));
        snprintf(what, what_n, "selectorText = %s", rule);
        cssom_set_attribute(s, r->id, "selectorText", rule, strlen(rule));
        break;
    }
    case 4: {
        struct css_rule_info *r = top_of_kind(s, CSS_RULE_STYLE, CSS_RULE_FONT_FACE, CSS_RULE_PAGE);
        if (!r) break;
        snprintf(rule, sizeof rule, r->type == CSS_RULE_FONT_FACE ? "font-family: G%u; src: url(g.woff2)" :
                 "color: rgb(%u, 0, 0); margin: 1px !important", pick(255));
        snprintf(what, what_n, "style.cssText = %s", rule);
        cssom_set_declarations(s, r->id, rule, strlen(rule));
        break;
    }
    case 5: {
        struct css_rule_info *r = top_of_kind(s, CSS_RULE_MEDIA, CSS_RULE_SUPPORTS, CSS_RULE_LAYER_BLOCK);
        if (!r) r = top_of_kind(s, CSS_RULE_STYLE, CSS_RULE_SCOPE, CSS_RULE_CONTAINER);
        if (!r) break;
        snprintf(rule, sizeof rule, r->type == CSS_RULE_STYLE ? "& .deep%u { color: red }" : ".in%u { color: red }", pick(100));
        uint32_t at = pick(r->child_count + 1);
        snprintf(what, what_n, "rule.insertRule(%s, %u)", rule, at);
        tally(2, cssom_insert(s, r->id, rule, strlen(rule), at));
        break;
    }
    case 6: {
        struct css_rule_info *r = top_of_kind(s, CSS_RULE_MEDIA, CSS_RULE_STYLE, CSS_RULE_LAYER_BLOCK);
        if (!r || !r->child_count) break;
        uint32_t at = pick(r->child_count);
        snprintf(what, what_n, "rule.deleteRule(%u)", at);
        tally(3, cssom_delete(s, r->id, at));
        break;
    }
    case 7: {
        struct css_rule_info *r = top_of_kind(s, CSS_RULE_MEDIA, 0, 0);
        if (!r) break;
        snprintf(rule, sizeof rule, "(min-width: %upx), print", pick(900));
        snprintf(what, what_n, "media.mediaText = %s", rule);
        sbuf out = {0};
        cssom_media(s, r->id, 1, rule, strlen(rule), &out);
        sb_free(&out);
        break;
    }
    case 8: {
        struct css_rule_info *r = top_of_kind(s, CSS_RULE_KEYFRAMES, 0, 0);
        if (!r || !r->child_count) break;
        snprintf(rule, sizeof rule, "%u%%", pick(101));
        snprintf(what, what_n, "keyframe.keyText = %s", rule);
        cssom_set_attribute(s, r->first->id, "keyText", rule, strlen(rule));
        break;
    }
    case 9: {
        /* a nested rule's own declarations, or nested declarations */
        struct css_rule_info *r = top_of_kind(s, CSS_RULE_STYLE, CSS_RULE_MEDIA, CSS_RULE_LAYER_BLOCK);
        if (!r || !r->child_count) break;
        struct css_rule_info *c = r->children[pick(r->child_count)];
        snprintf(rule, sizeof rule, "padding: %upx; color: blue", pick(50));
        snprintf(what, what_n, "nested style.cssText = %s", rule);
        cssom_set_declarations(s, c->id, rule, strlen(rule));
        break;
    }
    }
}
static struct cssom_sheet *sheet_of(const char *text) {
    int error;
    struct cssom_sheet *s = cssom_construct(&document_, &error);
    if (s && cssom_replace_sync(s, text, strlen(text))) return NULL;
    return s;
}
static void changes(const char *label, const char *text, unsigned rounds, bool constructed) {
    struct cssom_sheet *s = sheet_of(text);
    if (!s) { check(label, false, "no sheet", NULL); return; }
    s->constructed = constructed; /* a style element's sheet takes @import */
    compare(label, s, true);
    char what[512], name[640];
    for (unsigned i = 0; i < rounds; i++) {
        what[0] = 0;
        change(s, pick(1000), what, sizeof what);
        /* the index is built at some cascades: rules after it patch it */
        bool index = i % 7 == 0;
        snprintf(name, sizeof name, "%s, change %u: %s", label, i, what);
        if (!compare(name, s, index)) break;
    }
    /* rules after all others, into a built index */
    sheet_index(&document_, s->p.ast);
    for (unsigned i = 0; i < 20; i++) {
        char rule[256];
        snprintf(rule, sizeof rule, i % 2 ? ".tail%u { color: red }" : "@media (min-width: %upx) { .tm { color: red } }", i);
        cssom_insert(s, 0, rule, strlen(rule), s->p.count);
    }
    snprintf(name, sizeof name, "%s, rules appended to its index", label);
    compare(name, s, true);
    /* the sheet the changes made, as CSSOM serializes it: the same with
       OLD_SOURCE, which parsed the whole text at each change */
    uint64_t hash = 1469598103934665603ull;
    for (struct css_rule_info *r = css_sheet_rules(s->p.ast, NULL); r; r = r->next) {
        sbuf b = {0};
        cssom_serialize(s, r, &b);
        for (size_t i = 0; i < b.n; i++) hash = (hash ^ (unsigned char)b.p[i]) * 1099511628211ull;
        sb_free(&b);
    }
    printf("%s: serialized %016llx; ", label, (unsigned long long)hash);
    printf("insertRule %u of %u, deleteRule %u of %u, inside a rule %u of %u and %u of %u\n", made[0], tried[0],
           made[1], tried[1], made[2], tried[2], made[3], tried[3]);
    /* the check reads changed sheets, not refusals (the order of @import,
       @namespace and @layer statements refuses many of those) */
    check("insertRule and deleteRule calls change the sheet", made[0] * 4 > tried[0] && made[1] * 4 > tried[1], NULL, NULL);
    memset(made, 0, sizeof made); memset(tried, 0, sizeof tried);
}

/* ---------------------------------------------------------------- memory and time */
static double now(void) { struct timespec t; timespec_get(&t, TIME_UTC); return t.tv_sec + t.tv_nsec / 1e9; }
static double inserts(uint32_t count, bool middle, bool index) {
    struct cssom_sheet *s = sheet_of("");
    if (index) sheet_index(&document_, s->p.ast);
    char rule[96];
    double t0 = now();
    for (uint32_t i = 0; i < count; i++) {
        snprintf(rule, sizeof rule, ".c%u { color: red; margin: %upx; }", i, i);
        uint32_t n;
        cssom_child(s, 0, UINT32_MAX, &n);
        if (cssom_insert(s, 0, rule, strlen(rule), middle ? n / 2 : n) != CSSOM_OK) return -1;
    }
    return now() - t0;
}
static void timing(void) {
    /* each insertRule costs about one rule's parse: five times the rules, about five times the time */
    double small = inserts(1000, false, true), large = inserts(5000, false, true), middle = inserts(5000, true, false);
    printf("insertRule one by one: 1000 at the end %.3f s, 5000 at the end %.3f s (ratio %.1f), 5000 in the middle %.3f s\n",
           small, large, large / (small > 1e-6 ? small : 1e-6), middle);
    char got[64];
    snprintf(got, sizeof got, "ratio %.1f", large / (small > 1e-6 ? small : 1e-6));
    check("5000 insertRule at the end cost under ten times 1000", small >= 0 && large >= 0 && large < 10 * small + 0.05, got,
          "ratio under 10");
    check("5000 insertRule in the middle under a second", middle >= 0 && middle < 1.0, NULL, NULL);
}
#ifndef OLD_SOURCE
static void garbage(void) {
    struct cssom_sheet *s = sheet_of(".a { color: red } .b { color: blue } @media print { .c { color: green } }");
    uint32_t n, id = cssom_child(s, 0, 2, &n)->id, child = cssom_child(s, id, 0, &n)->id;
    char decls[96];
    size_t most = 0;
    for (unsigned i = 0; i < 4000; i++) {
        snprintf(decls, sizeof decls, "color: rgb(%u, %u, 0); margin: %upx", i % 256, i / 256, i);
        cssom_set_declarations(s, child, decls, strlen(decls));
        if (s->garbage > most) most = s->garbage;
    }
    char got[96];
    snprintf(got, sizeof got, "garbage at most %zu, live %zu", most, s->live);
    check("replaced rules' memory stays bounded (a whole parse frees it)", most <= s->live + 65536 + 4096, got, NULL);
    bool attached;
    struct css_rule_info *r = cssom_rule(s, child, &attached);
    sbuf b = {0};
    if (r) cssom_serialize(s, r, &b);
    check("its CSSRule ids keep their rules across", r && attached && strstr(sb_cstr(&b), "margin: 3999px"), sb_cstr(&b), NULL);
    sb_free(&b);
}
#endif

int main(void) {
    document_.root = &root_;
    document_.base = "https://example.test/";
    root_.owner = &document_;
    changes("a sheet of every kind of rule",
            "@layer base, theme;\n.a { color: red; }\n"
            "@media (min-width: 100px) { .b { margin: 1px; } .c:hover { padding: 2px } }\n"
            "@layer base { .d { color: blue } @layer inner { .e { color: green } } }\n"
            "@layer { .anon { color: pink } }\n"
            "@keyframes spin { from { opacity: 0 } to { opacity: 1 } }\n"
            "@font-face { font-family: X; src: url(x.woff2) format(\"woff2\"); }\n"
            ".n { color: red; & .child { color: blue; } > .kid { margin: 0 } color: green; }\n"
            "@supports (display: grid) { .g { display: grid } }\n"
            "@container (min-width: 10px) { .q { color: red } }\n"
            "@scope (.s) { .t { color: red } }\n", 600, true);
    changes("an empty sheet", "", 300, true);
    changes("a sheet with @namespace",
            "@namespace url(http://www.w3.org/1999/xhtml);\n@namespace svg url(http://www.w3.org/2000/svg);\n"
            "svg|rect { fill: red }\n.a { color: red }\n", 300, true);
    changes("a style element's sheet with @import, @namespace and @layer statements",
            "@layer a;\n@namespace url(http://www.w3.org/1999/xhtml);\n.a { color: red }\n@media print { .b { color: blue } }\n",
            400, false);
    /* rules outside every top-level rule's span: the whole text each time */
    changes("a sheet whose @document rules have no CSSOM rule",
            "@document url(https://example.test/) { .h { color: red } }\n.a { color: blue }\n"
            "@font-face x { font-family: Y; src: url(y.woff2) }\n", 200, true);
#ifndef OLD_SOURCE
    garbage();
#endif
    timing();
    printf("cssom splices: %d passed, %d failed\n", passed, failed);
    return failed != 0;
}
