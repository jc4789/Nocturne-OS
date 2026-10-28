/* CSS counters (CSS Lists 3 §4, CSS Generated Content 3 §1-§2, CSS Counter Styles 3) on the host:
   the real counter.c (the one owner that counts and writes the texts) over boxes built by hand,
   their styles parsed by the real property table (cssprop.c), which boxes count decided by box.c's
   own counting_style and marker_generated. tests/check_counters_native.py writes the cases (WPT's
   css/css-lists reftests, each named at its case) after this harness and compiles them together.

   Not the guest WPT: the cascade (css.c's selectors, the UA style sheet and the presentational
   hints of ol and li, which the cases write as the declarations css.c maps them to), box
   generation (box.c's tree: a display: contents element is made here as no box with its children's
   boxes under the box above), white-space processing of the texts,
   layout and paint are not reached. The flat tree is the DOM tree (no shadow trees). */

static int checks, failed;
static const char *group;
static void check(bool ok, const char *what) {
    checks++;
    if (!ok) { failed++; printf("FAIL counters %s: %s\n", group, what); }
}

/* reached only from properties these cases do not set */
bool web_native_checkpoint(web_doc *d) { (void)d; return true; }
void web_avmedia_checkpoint(void) {}
void *thread_local_get(unsigned slot) { (void)slot; return NULL; }
unsigned parallel_worker_index(void) { return 0; }
bool css_valid_declaration(const char *p, size_t pn, const char *v, size_t vn) { (void)p; (void)pn; (void)v; (void)vn; return false; }
bool css_container_names_valid(const char *s, size_t n) { (void)s; (void)n; return false; }
void css_transform_list_matrix_3d(const struct css_transform *t, double m[16]) { (void)t; (void)m; abort(); }
void css_transform_list_matrix(const struct css_transform *t, float w, float h, double m[16]) { (void)t; (void)w; (void)h; (void)m; abort(); }
/* the flat tree without shadow trees: the DOM parent */
node_t *doc_flat_parent(node_t *n) { return n ? n->parent : NULL; }

/* counter.c's writes into the box tree (box.c's box_counter_text and box_counter_marker), kept
   as they come: the box written, its text, an outside marker's shape and how many times */
struct written { const box_t *b; char text[256]; size_t n; uint8_t shape; int times; };
static struct written written[2048];
static int nwritten;
static struct written *written_of(const box_t *b, bool add) {
    for (int i = 0; i < nwritten; i++) if (written[i].b == b) return &written[i];
    if (!add || nwritten == (int)(sizeof written / sizeof *written)) return NULL;
    written[nwritten] = (struct written){.b = b};
    return &written[nwritten++];
}
static void keep(const box_t *b, const char *s, size_t n, uint8_t shape) {
    struct written *w = written_of(b, true);
    if (!w) { check(false, "room for the written texts"); return; }
    w->n = n < sizeof w->text - 1 ? n : sizeof w->text - 1;
    memcpy(w->text, s, w->n);
    w->text[w->n] = 0;
    w->shape = shape;
    w->times++;
}
void box_counter_text(box_t *t, const char *s, size_t n) { keep(t, s, n, 0); }
void box_counter_marker(web_doc *d, box_t *item, const char *s, size_t n, uint8_t shape) {
    check(d != NULL, "a marker is written with its document");
    keep(item, s, n, shape);
}

static web_doc doc;
static arena_t values;
static jmp_buf values_trap;
static style_t root_style;
static box_t *boxes[2048];
static int nboxes;
static bool pseudos[2048], unboxed[2048]; /* a pseudo-element's box; a display: contents element's record */
static box_t *root;
struct want { const box_t *b; const char *text; int at; };
static struct want wants[1024];
static int nwants;

/* a computed style inheriting from parent with the declarations "property: value; ..." */
static style_t *style_of(const style_t *parent, const char *decls) {
    style_t *s = ar_alloc(&values, sizeof *s);
    css_style_init(s, parent);
    css_style_finish(s, parent, false);
    char text[1024];
    snprintf(text, sizeof text, "%s", decls);
    for (char *d = strtok(text, ";"); d; d = strtok(NULL, ";")) {
        char *colon = strchr(d, ':');
        if (!colon) continue;
        *colon = 0;
        char *name = d, *value = colon + 1;
        while (*name == ' ' || *name == '\n') name++;
        for (char *e = name + strlen(name); e > name && e[-1] == ' '; ) *--e = 0;
        const struct propdef *p = css_prop_lookup(name, strlen(name));
        struct css_anim_ctx c = {.base = s, .parent = parent, .rem = 16, .vw = 800, .vh = 600, .a = &values};
        style_t next;
        if (!p || !css_anim_parse(&c, p, value, strlen(value), false, &next, NULL)) {
            char what[256];
            snprintf(what, sizeof what, "the declaration %s:%s parses", name, value);
            check(false, what);
            continue;
        }
        *s = next;
    }
    return s;
}
static box_t *new_box(box_t *parent, node_t *node, const style_t *st, bool pseudo) {
    box_t *b = calloc(1, sizeof *b);
    if (!b || nboxes == (int)(sizeof boxes / sizeof *boxes)) { printf("FAIL counters: room for the boxes\n"); exit(1); }
    b->node = node;
    b->st = (style_t *)st;
    b->kind = pseudo ? B_INLINE : B_BLOCK;
    b->parent = parent;
    if (parent) {
        if (parent->last) parent->last->next = b; else parent->first = b;
        parent->last = b;
    }
    pseudos[nboxes] = pseudo;
    unboxed[nboxes] = false;
    boxes[nboxes++] = b;
    return b;
}
static void scene(const char *name) {
    for (int i = 0; i < nboxes; i++) {
        if (!pseudos[i]) free(boxes[i]->node);
        free(boxes[i]->counter_text);
        free(boxes[i]);
    }
    nboxes = nwritten = nwants = 0;
    ar_free(&values);
    memset(&doc, 0, sizeof doc);
    group = name;
    css_style_init(&root_style, NULL);
    root_style.font_size = 16;
    css_style_finish(&root_style, NULL, true);
    node_t *n = calloc(1, sizeof *n);
    n->type = N_ELEM; n->owner = &doc;
    root = new_box(NULL, n, style_of(&root_style, "display: block"), false);
    n->box = root; n->style = root->st;
    doc.root_box = root;
}
static bool is_unboxed(const box_t *b) {
    for (int i = 0; i < nboxes; i++) if (boxes[i] == b) return unboxed[i];
    return false;
}
/* an element's principal box, the last child of parent's (of the nearest box above parent when
   that is a display: contents element, which generates none) */
static box_t *element(box_t *parent, const char *decls) {
    node_t *n = calloc(1, sizeof *n);
    n->type = N_ELEM; n->owner = &doc; n->parent = parent->node;
    if (parent->node->last) parent->node->last->next = n; else parent->node->first = n;
    n->prev = parent->node->last;
    parent->node->last = n;
    const style_t *st = style_of(parent->st, decls);
    if (st->display == D_CONTENTS) {
        /* no box: a record of its node and style for its children, outside the box tree */
        box_t *record = new_box(NULL, n, st, false);
        unboxed[nboxes - 1] = true;
        record->parent = parent;
        n->style = record->st;
        return record;
    }
    box_t *host = parent;
    while (is_unboxed(host)) host = host->parent;
    box_t *b = new_box(host, n, st, false);
    n->box = b; n->style = b->st;
    return b;
}
/* the list item's ::marker with declarations of its own (css.c gives it the item's style with
   the UA ::marker rule, then the author's) */
static void own_marker(box_t *item, const char *decls) {
    style_t *m = ar_alloc(&values, sizeof *m);
    css_style_marker(m, item->st);
    struct css_anim_ctx c = {.base = m, .parent = item->st, .rem = 16, .vw = 800, .vh = 600, .a = &values};
    char text[1024];
    snprintf(text, sizeof text, "%s", decls);
    for (char *d = strtok(text, ";"); d; d = strtok(NULL, ";")) {
        char *colon = strchr(d, ':');
        if (!colon) continue;
        *colon = 0;
        char *name = d, *value = colon + 1;
        while (*name == ' ') name++;
        for (char *e = name + strlen(name); e > name && e[-1] == ' '; ) *--e = 0;
        const struct propdef *p = css_prop_lookup(name, strlen(name));
        style_t next;
        c.base = m;
        if (!p || !css_anim_parse(&c, p, value, strlen(value), false, &next, NULL)) {
            char what[256];
            snprintf(what, sizeof what, "the ::marker declaration %s:%s parses", name, value);
            check(false, what);
            continue;
        }
        *m = next;
    }
    item->st->marker = m;
}
/* the element's ::before (made before its children) or ::after (after them): a box of the
   element's node whose generated text counter.c writes into its text box */
static box_t *pseudo(box_t *e, const char *decls) {
    box_t *b = new_box(e, e->node, style_of(e->st, decls), true);
    b->counter_text = calloc(1, sizeof *b->counter_text);
    b->counter_text->kind = B_TEXT;
    return b;
}
/* the text counter.c writes for b: a pseudo-element's content, an element's marker */
static void want(box_t *b, const char *text) {
    if (nwants == (int)(sizeof wants / sizeof *wants)) { printf("FAIL counters: room for the expectations\n"); exit(1); }
    wants[nwants] = (struct want){b->counter_text ? b->counter_text : b, text, nwants};
    nwants++;
}
/* box.c's decisions: a list item's box and an element with counter values count, a list item's
   ::marker counts when it generates a box, a ::before or ::after counts; counter_tree marks the
   boxes above */
static void count(void) {
    for (int i = 0; i < nboxes; i++) {
        box_t *b = boxes[i];
        if (unboxed[i]) continue;
        if (pseudos[i]) b->counter_kind = BOX_COUNTER_PSEUDO;
        else if (b->st->display == D_LIST_ITEM)
            b->counter_kind = BOX_COUNTER_ELEMENT | (marker_generated(b->st) ? BOX_COUNTER_MARKER : 0);
        else if (counting_style(b->st)) b->counter_kind = BOX_COUNTER_ELEMENT;
        if (b->counter_kind) for (box_t *x = b; x; x = x->parent) x->counter_tree = true;
    }
    doc.counters_dirty = true;
    web_counters_update(&doc);
    check(!doc.counters_dirty, "the update counts to the end");
    for (int i = 0; i < nwants; i++) {
        const struct written *w = written_of(wants[i].b, false);
        char what[640];
        snprintf(what, sizeof what, "text %d is \"%s\", got \"%s\"", wants[i].at + 1, wants[i].text, w ? w->text : "(none)");
        check(w && !strcmp(w->text, wants[i].text), what);
        if (w && w->times != 1) {
            snprintf(what, sizeof what, "text %d written once, %d times", wants[i].at + 1, w->times);
            check(false, what);
        }
    }
    /* a second update with nothing changed writes nothing */
    int before = nwritten;
    web_counters_update(&doc);
    check(nwritten == before, "an update with nothing dirty writes nothing");
}

static void cases(void);
int main(void) {
    values.trap = &values_trap;
    if (setjmp(values_trap)) { printf("FAIL counters: allocation\n"); return 1; }
    cases();
    printf("counters: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
