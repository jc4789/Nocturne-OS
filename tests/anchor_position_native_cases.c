/* css-anchor-position-1 in the absolute positioning stage (layout.c) on the host: the real
   layout_abs, abs_place and anchor code over boxes placed by hand, their styles parsed by the real
   property table (cssprop.c). The cases are WPT's css/css-anchor-position pages (named at each
   group) whose expectations are offsets and sizes; the in-flow boxes stand where the page's
   layout puts them, and an abspos box's content sizes are given (no text layout here).

   Not the guest WPT: the cascade (@position-try rules in css.c, var(), CSS-wide keywords), box
   generation, inline layout, paint and getComputedStyle are not reached. */
#include <stdio.h>

static int checks, failed;
static const char *group;
static void check(bool ok, const char *what) {
    checks++;
    if (!ok) { failed++; printf("FAIL anchor-position %s: %s\n", group, what); }
}
static void near(double got, double want, const char *what) {
    char text[256];
    snprintf(text, sizeof text, "%s = %g, want %g", what, got, want);
    check(fabs(got - want) < 0.01, text);
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
/* no top layer, no fullscreen, no viewport-propagated overflow, no filters or transforms */
bool web_dialog_is_modal(web_doc *d, const node_t *n) { (void)d; (void)n; return false; }
bool web_dialog_layer_box(web_doc *d, box_t *b) { (void)d; (void)b; return false; }
int web_dialog_count(web_doc *d) { (void)d; return 0; }
node_t *web_dialog_at(web_doc *d, int i) { (void)d; (void)i; return NULL; }
bool doc_viewport_overflow_box(const web_doc *d, const box_t *b) { (void)d; (void)b; return false; }
bool box_filter_containing_block(const box_t *b) { (void)b; return false; }
/* no sticky boxes */
static bool box_sticky(const box_t *b) { (void)b; return false; }
static void sticky_offset(const box_t *s, float shift[2]) { (void)s; shift[0] = shift[1] = 0; }

/* an abspos box's contents: its min-content and max-content widths and its content height */
struct content { const box_t *b; float min_w, max_w, h; };
static struct content contents[256];
static int ncontents;
static const struct content *content_of(const box_t *b) {
    for (int i = 0; i < ncontents; i++) if (contents[i].b == b) return &contents[i];
    return NULL;
}
/* layout_inner's height rules (resolved_content_height, clamp_h) over the given content height */
static void layout_inner(box_t *b, struct bfc *f, float ox, float oy, float cbh) {
    (void)f; (void)ox; (void)oy;
    const struct content *c = content_of(b);
    float sh = resolved_content_height(b, cbh);
    b->h = clamp_h(b, sh >= 0 ? sh : c ? c->h : 0, cbh);
}
static float stf_width(box_t *b, float avail) {
    const struct content *c = content_of(b);
    float a = avail - b->m[1] - b->m[3] - hext(b);
    return fminf_(fmaxf_(c ? c->min_w : 0, a), c ? c->max_w : 0);
}
static void size_atomic(box_t *c, float cbw, float cbh) { (void)c; (void)cbw; (void)cbh; abort(); }
static void table_width(box_t *t, float cw) { (void)t; (void)cw; abort(); }
/* visual coordinates: box_abs with the scroll of the scroll containers and the default scroll
   shift of the anchor-positioned boxes on the containing-block chain (layout.c's visual_unstuck
   without sticky boxes, translations and marquees) */
static double visual(const box_t *b, unsigned axis) {
    double v = axis ? box_abs_y(b) : box_abs_x(b);
    for (const box_t *c = b; c; c = c->cb) {
        if (c->anchored) v += box_anchor_shift(c, axis);
        if (c != b && box_element_scrollable(c)) v -= axis ? c->node->scroll_y : c->node->scroll_x;
    }
    return v;
}
float box_visual_x(const box_t *b) { return (float)visual(b, 0); }
float box_visual_y(const box_t *b) { return (float)visual(b, 1); }

static web_doc doc;
static arena_t values;
static jmp_buf values_trap;
static style_t parent_style;
static uint32_t generation;
static box_t *boxes[256];
static int nboxes;
static box_t *root;

/* an element's computed style with the declarations "property: value; ..." (longhands) */
static style_t *style_of(const char *decls) {
    style_t *s = ar_alloc(&values, sizeof *s);
    css_style_init(s, &parent_style);
    css_style_finish(s, &parent_style, false);
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
        struct css_anim_ctx c = {.base = s, .parent = &parent_style, .rem = 16, .vw = 800, .vh = 600, .a = &values};
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
static void scene(float width, float height) {
    for (int i = 0; i < nboxes; i++) { box_anchor_free(boxes[i]); free(boxes[i]->node); free(boxes[i]); }
    nboxes = ncontents = 0;
    ar_free(&doc.lmem);
    memset(&doc, 0, sizeof doc);
    doc.width = (int)width; doc.height = (int)height;
    doc.lmem.trap = &values_trap;
    root = calloc(1, sizeof *root);
    root->kind = B_BLOCK; root->st = style_of("display: block");
    root->w = width; root->h = height;
    boxes[nboxes++] = root;
    root->node = calloc(1, sizeof *root->node);
    root->node->type = N_ELEM; root->node->owner = &doc; root->node->box = root; root->node->style = root->st;
    doc.root_box = root;
}
static box_t *element(box_t *parent, const char *decls) {
    box_t *b = calloc(1, sizeof *b);
    node_t *n = calloc(1, sizeof *n);
    b->st = style_of(decls);
    n->type = N_ELEM; n->owner = &doc; n->box = b; n->style = b->st;
    b->node = n; b->kind = B_BLOCK; b->parent = parent;
    if (parent->last) parent->last->next = b; else parent->first = b;
    parent->last = b;
    b->abspos = b->st->position == POS_ABSOLUTE || b->st->position == POS_FIXED;
    b->cb = parent;
    if (b->abspos) b->static_cb = parent;
    boxes[nboxes++] = b;
    return b;
}
static void restyle(box_t *b, const char *decls) { b->st = b->node->style = style_of(decls); }
static void content(box_t *b, float min_w, float max_w, float h) {
    contents[ncontents++] = (struct content){b, min_w, max_w, h};
}
/* an in-flow (or already placed) box's content box in its containing block's content coordinates */
static void at(box_t *b, float x, float y, float w, float h) {
    resolve_edges(b, b->cb ? b->cb->w : 0);
    b->x = x; b->y = y; b->w = w; b->h = h;
}
/* one layout's absolute positioning stage: these boxes in tree order */
static void layout(box_t **abs, int n) {
    default_layout = (struct layout_context){.document = &doc, .viewport_w = (float)doc.width,
        .viewport_h = (float)doc.height, .generation = ++generation};
    ar_free(&doc.lmem);
    for (int i = 0; i < n; i++) abs[i]->gen = GEN;
    for (int i = 0; i < n; i++) layout_abs(abs[i]);
    doc.layout_revision++;
}
/* offsetLeft/offsetTop against the containing block's padding edge, offsetWidth/offsetHeight */
static void offsets(const box_t *b, double left, double top, double width, double height, const char *name) {
    char what[160];
    const box_t *cb = b->cb;
    snprintf(what, sizeof what, "%s offsetLeft", name); near(b->x - b->p[3] - b->b[3] + cb->p[3], left, what);
    snprintf(what, sizeof what, "%s offsetTop", name); near(b->y - b->p[0] - b->b[0] + cb->p[0], top, what);
    if (width >= 0) { snprintf(what, sizeof what, "%s offsetWidth", name); near(b->w + hext(b), width, what); }
    if (height >= 0) { snprintf(what, sizeof what, "%s offsetHeight", name); near(b->h + vext(b), height, what); }
}
static double used_inset(const box_t *b, unsigned side) {
    float v;
    return box_anchor_inset(b, side, &v) ? v : NAN;
}
/* the computed style's position options, each named @position-try rule's style being the base
   declarations followed by the rule's (what css.c's position fallback origin gives) */
static void rules(box_t *b, const char *base, const char *const *names, const char *const *decls, int count) {
    const struct css_anchor *an = css_style_anchor(b->st);
    struct css_anchor *own = ar_alloc(&values, sizeof *own);
    *own = *an;
    struct css_position_try *options = ar_alloc(&values, (an->tries.count + 1) * sizeof *options);
    for (uint32_t i = 0; i < an->tries.count; i++) {
        options[i] = an->tries.option[i];
        for (int k = 0; options[i].rule && k < count; k++)
            if (!strcmp(options[i].rule, names[k])) {
                char text[1024];
                snprintf(text, sizeof text, "%s; %s", base, decls[k]);
                options[i].style = style_of(text);
            }
    }
    own->tries.option = options;
    b->st->anchor = own;
}

int main(void) {
    values.trap = &values_trap;
    if (setjmp(values_trap)) { printf("FAIL anchor-position: allocation\n"); return 1; }
    css_style_init(&parent_style, NULL);
    parent_style.font_size = 16;
    css_style_finish(&parent_style, NULL, true);

    /* position-area-basic.html: a 400x400 containing block, the anchor at (100, 150) 150x75 */
    group = "position-area-basic";
    {
        static const struct { const char *area; double l, t, w, h; } cases[] = {
            {"none", 0, 0, 0, 0}, {"span-all", 0, 0, 400, 400}, {"span-all span-all", 0, 0, 400, 400},
            {"top left", 0, 0, 100, 150}, {"top center", 100, 0, 150, 150}, {"top right", 250, 0, 150, 150},
            {"center left", 0, 150, 100, 75}, {"center center", 100, 150, 150, 75}, {"center right", 250, 150, 150, 75},
            {"bottom left", 0, 225, 100, 175}, {"bottom center", 100, 225, 150, 175}, {"bottom right", 250, 225, 150, 175},
            {"start start", 0, 0, 100, 150}, {"start center", 100, 0, 150, 150}, {"start end", 250, 0, 150, 150},
            {"center start", 0, 150, 100, 75}, {"center end", 250, 150, 150, 75}, {"end start", 0, 225, 100, 175},
            {"end center", 100, 225, 150, 175}, {"end end", 250, 225, 150, 175},
            {"self-start self-start", 0, 0, 100, 150}, {"self-start center", 100, 0, 150, 150},
            {"self-start self-end", 250, 0, 150, 150}, {"center self-start", 0, 150, 100, 75},
            {"center self-end", 250, 150, 150, 75}, {"self-end self-start", 0, 225, 100, 175},
            {"self-end center", 100, 225, 150, 175}, {"self-end self-end", 250, 225, 150, 175},
            {"y-start x-start", 0, 0, 100, 150}, {"y-start center", 100, 0, 150, 150}, {"y-start x-end", 250, 0, 150, 150},
            {"center x-start", 0, 150, 100, 75}, {"center x-end", 250, 150, 150, 75}, {"y-end x-start", 0, 225, 100, 175},
            {"y-end center", 100, 225, 150, 175}, {"y-end x-end", 250, 225, 150, 175},
            {"self-y-start self-x-start", 0, 0, 100, 150}, {"self-y-start center", 100, 0, 150, 150},
            {"self-y-start self-x-end", 250, 0, 150, 150}, {"center self-x-start", 0, 150, 100, 75},
            {"center self-x-end", 250, 150, 150, 75}, {"self-y-end self-x-start", 0, 225, 100, 175},
            {"self-y-end center", 100, 225, 150, 175}, {"self-y-end self-x-end", 250, 225, 150, 175},
            {"span-self-y-start span-self-x-end", 100, 0, 300, 225}, {"span-bottom span-all", 0, 150, 400, 250},
        };
        for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
            scene(800, 600);
            box_t *container = element(root, "position: absolute; width: 400px; height: 400px");
            at(container, 0, 0, 400, 400);
            box_t *anchored = element(container, "position: absolute");
            char decls[256];
            snprintf(decls, sizeof decls, "position: absolute; align-self: stretch; justify-self: stretch; "
                     "position-anchor: --anchor; position-area: %s", cases[i].area);
            restyle(anchored, decls);
            box_t *anchor = element(container, "anchor-name: --anchor; width: 150px; height: 75px");
            at(anchor, 100, 150, 150, 75);
            layout(&anchored, 1);
            offsets(anchored, cases[i].l, cases[i].t, cases[i].w, cases[i].h, cases[i].area);
        }
        /* no implicit anchor: position-area does not apply */
        scene(800, 600);
        box_t *container = element(root, "position: absolute; width: 400px; height: 400px");
        at(container, 0, 0, 400, 400);
        box_t *anchored = element(container, "position: absolute; align-self: stretch; justify-self: stretch; "
                                  "position-anchor: auto; position-area: span-all top");
        box_t *anchor = element(container, "anchor-name: --anchor; width: 150px; height: 75px");
        at(anchor, 100, 150, 150, 75);
        layout(&anchored, 1);
        offsets(anchored, 0, 0, 0, 0, "position-anchor: auto; span-all top");
    }

    /* position-area-align-justify.html: the area-specific default alignment (§4.1) in a 300x300
       containing block, a 100x100 absolutely positioned anchor at (100, 100), a 10x10 box with
       insets 10px 15px 20px 25px */
    group = "position-area-align-justify";
    {
        static const struct { const char *area; double l, t; } cases[] = {
            {"span-all", 145, 145}, {"top left", 75, 70}, {"top center", 150, 70}, {"top right", 225, 70},
            {"center left", 75, 140}, {"center center", 150, 140}, {"center right", 225, 140},
            {"bottom left", 75, 210}, {"bottom center", 150, 210}, {"bottom right", 225, 210},
            {"top span-left", 175, 70}, {"top span-right", 125, 70}, {"span-top left", 75, 170},
            {"span-bottom left", 75, 110},
        };
        for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
            scene(800, 600);
            box_t *container = element(root, "position: relative; width: 300px; height: 300px");
            at(container, 0, 0, 300, 300);
            box_t *anchor = element(container, "position: absolute; top: 100px; left: 100px; width: 100px; "
                                    "height: 100px; anchor-name: --anchor");
            box_t *anchored = element(container, "position: absolute");
            char decls[320];
            snprintf(decls, sizeof decls, "position: absolute; width: 10px; height: 10px; top: 10px; right: 15px; "
                     "bottom: 20px; left: 25px; position-anchor: --anchor; position-area: %s", cases[i].area);
            restyle(anchored, decls);
            box_t *abs[] = {anchor, anchored};
            layout(abs, 2);
            offsets(anchored, cases[i].l, cases[i].t, 10, 10, cases[i].area);
        }
    }

    /* anchor-center-htb-htb.html: justify-self: anchor-center, top: anchor(bottom); the available
       size is the inset-modified containing block, and overflow shifts within it (Box Alignment 3
       §4.4.1.2). The 100x100 containing block holds a relatively positioned 50x50 anchor at
       (40, 40); the box's text is wider than any space. */
    group = "anchor-center-htb-htb";
    {
        static const struct { const char *extra; double w, x; } cases[] = {
            {"", 100, 0}, {"max-width: 60px", 60, 35}, {"left: 20px", 80, 20}, {"right: 20px", 80, 0},
            {"right: -20px", 120, 0}, {"max-width: 100px; right: -20px", 100, 15}, {"right: -50px", 150, 0},
            {"left: 10px; right: 20px", 70, 10}, {"left: 10px; right: -20px", 110, 10},
            {"left: -10px; right: -50px", 160, -10},
        };
        for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
            scene(800, 600);
            box_t *container = element(root, "position: relative; width: 100px; height: 100px");
            at(container, 53, 53, 100, 100);
            box_t *anchor = element(container, "anchor-name: --anchor; position: relative; width: 50px; height: 50px; "
                                    "left: 40px; top: 40px");
            at(anchor, 0, 0, 50, 50);
            box_t *target = element(container, "position: absolute");
            char decls[320];
            snprintf(decls, sizeof decls, "position-anchor: --anchor; position: absolute; justify-self: anchor-center; "
                     "top: anchor(bottom); height: 20px; %s", cases[i].extra);
            restyle(target, decls);
            content(target, 8, 1000, 20);
            target->static_y = 50;
            layout(&target, 1);
            char name[96];
            snprintf(name, sizeof name, "[%s]", cases[i].extra);
            offsets(target, cases[i].x, 90, cases[i].w, 20, name);
        }
    }

    /* anchor-position-001.html: anchor() of two named anchors on all four insets */
    group = "anchor-position-001";
    {
        scene(800, 600);
        box_t *container = element(root, "position: relative");
        at(container, 8, 108, 784, 300);
        box_t *a1 = element(container, "anchor-name: --a1; width: 100px; height: 100px");
        at(a1, 100, 0, 100, 100);
        box_t *a2 = element(container, "anchor-name: --a2; width: 100px; height: 100px");
        at(a2, 500, 200, 100, 100);
        box_t *target = element(container, "position: absolute; left: anchor(--a1 right); top: anchor(--a1 bottom); "
                                "right: anchor(--a2 left); bottom: anchor(--a2 top)");
        layout(&target, 1);
        offsets(target, 200, 100, 300, 100, "target");
        near(used_inset(target, 0), 100, "used top");
        near(used_inset(target, 1), 284, "used right");
        near(used_inset(target, 2), 100, "used bottom");
        near(used_inset(target, 3), 200, "used left");
    }

    /* anchor() sides, percentages, fallbacks and anchor-size() (§3.2, §5.1): the anchor at
       (100, 50) 80x40 in a 400x300 containing block */
    group = "anchor-functions";
    {
        static const struct { const char *decls; double l, t, w, h; } cases[] = {
            {"left: anchor(--a center); top: anchor(--a 25%)", 140, 60, 10, 10},
            {"left: anchor(--a inside); top: anchor(--a outside)", 100, 90, 10, 10},
            {"right: anchor(--a inside); bottom: anchor(--a outside)", 170, 40, 10, 10},
            {"left: anchor(--a start); top: anchor(--a end)", 100, 90, 10, 10},
            {"left: anchor(--a self-end); top: anchor(--a self-start)", 180, 50, 10, 10},
            {"left: calc(anchor(--a right) + 5px); top: anchor(--a top)", 185, 50, 10, 10},
            /* a physical side of the other axis, or a missing anchor, is not resolvable: the fallback */
            {"left: anchor(--a top, 7px); top: anchor(--missing bottom, 9px)", 7, 9, 10, 10},
            /* without a fallback the declaration is invalid at computed-value time: auto */
            {"left: anchor(--missing left); top: 3px", 0, 3, 10, 10},
            {"left: 0px; top: 0px; width: anchor-size(--a width); height: anchor-size(--a block)", 0, 0, 80, 40},
            {"left: 0px; top: 0px; width: anchor-size(--a height); height: calc(anchor-size(--a) * 2)", 0, 0, 40, 80},
            {"left: 0px; top: 0px; width: anchor-size(--missing width, 33px); min-height: anchor-size(--a width)", 0, 0, 33, 80},
            {"left: 0px; top: 0px; margin-left: anchor-size(--a width)", 80, 0, 10, 10},
        };
        for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
            scene(800, 600);
            box_t *container = element(root, "position: relative; width: 400px; height: 300px");
            at(container, 0, 0, 400, 300);
            box_t *anchor = element(container, "anchor-name: --a; width: 80px; height: 40px");
            at(anchor, 100, 50, 80, 40);
            box_t *target = element(container, "position: absolute");
            char decls[400];
            snprintf(decls, sizeof decls, "position: absolute; width: 10px; height: 10px; %s", cases[i].decls);
            restyle(target, decls);
            layout(&target, 1);
            offsets(target, cases[i].l, cases[i].t, cases[i].w, cases[i].h, cases[i].decls);
        }
    }

    /* anchor-scope-basic.html: 10px-high anchors stacked in a 100x100 containing block */
    group = "anchor-scope-basic";
    {
        /* the anchor scopes its own name; its child anchors to it */
        scene(800, 600);
        box_t *main_box = element(root, "position: relative; width: 100px; height: 100px");
        at(main_box, 1, 1, 100, 100);
        box_t *a = element(main_box, "anchor-scope: --a; anchor-name: --a; height: 10px");
        at(a, 0, 0, 100, 10);
        box_t *p = element(a, "position: absolute; position-anchor: --a; top: anchor(bottom); left: anchor(left); "
                           "width: 5px; height: 5px");
        layout(&p, 1);
        near(used_inset(p, 0), 10, "inclusive subtree: top");

        /* a sibling cannot anchor into anchor-scope */
        scene(800, 600);
        main_box = element(root, "position: relative; width: 100px; height: 100px");
        at(main_box, 1, 1, 100, 100);
        for (int i = 0; i < 3; i++) at(element(main_box, "anchor-name: --a; height: 10px"), 0, 10 * i, 100, 10);
        at(element(main_box, "anchor-scope: --a; anchor-name: --a; height: 10px"), 0, 30, 100, 10);
        p = element(main_box, "position: absolute; position-anchor: --a; top: anchor(bottom); left: anchor(left); "
                    "width: 5px; height: 5px");
        layout(&p, 1);
        near(used_inset(p, 0), 30, "skips a scoped name: top");

        /* anchor-scope: all, and --a, on a common ancestor */
        for (int scoped = 0; scoped < 2; scoped++) {
            scene(800, 600);
            main_box = element(root, "position: relative; width: 100px; height: 100px");
            at(main_box, 1, 1, 100, 100);
            box_t *scope = element(main_box, scoped ? "anchor-scope: --a" : "anchor-scope: all");
            at(scope, 0, 0, 100, 40);
            for (int i = 0; i < 4; i++) at(element(scope, "anchor-name: --a; height: 10px"), 0, 10 * i, 100, 10);
            p = element(scope, "position: absolute; position-anchor: --a; top: anchor(bottom); left: anchor(left); "
                        "width: 5px; height: 5px");
            layout(&p, 1);
            near(used_inset(p, 0), 40, scoped ? "scope --a on a common ancestor: top" : "scope all on a common ancestor: top");
        }

        /* anchor-scope: --a scopes only --a (test_scope_a) */
        scene(800, 600);
        main_box = element(root, "position: relative; width: 100px; height: 100px");
        at(main_box, 1, 1, 100, 100);
        at(element(main_box, "anchor-name: --b; height: 10px"), 0, 0, 100, 10);
        at(element(main_box, "anchor-name: --a; height: 10px"), 0, 10, 100, 10);
        box_t *scope = element(main_box, "anchor-scope: --a");
        at(scope, 0, 20, 100, 30);
        at(element(scope, "anchor-name: --b; height: 10px"), 0, 0, 100, 10);
        at(element(scope, "anchor-name: --a, --b; height: 10px"), 0, 10, 100, 10);
        at(element(scope, "anchor-name: --a; height: 10px"), 0, 20, 100, 10);
        box_t *pa = element(main_box, "position: absolute; position-anchor: --a; top: anchor(bottom); left: anchor(left); "
                            "width: 5px; height: 5px");
        box_t *pb = element(main_box, "position: absolute; position-anchor: --b; top: anchor(bottom); left: anchor(left); "
                            "width: 5px; height: 5px");
        box_t *abs[] = {pa, pb};
        layout(abs, 2);
        near(used_inset(pa, 0), 20, "scope --a: --a top");
        near(used_inset(pb, 0), 40, "scope --a: --b top");
    }

    /* acceptable anchor elements (§2.3): an absolutely positioned anchor after the box in tree
       order is not one; one before it is, and is placed first */
    group = "acceptable-anchor";
    {
        scene(800, 600);
        box_t *container = element(root, "position: relative; width: 400px; height: 400px");
        at(container, 0, 0, 400, 400);
        box_t *early = element(container, "position: absolute; left: 10px; top: 20px; width: 30px; height: 30px; anchor-name: --a");
        box_t *target = element(container, "position: absolute; left: anchor(--a right, 1px); top: anchor(--a bottom, 2px); "
                                "width: 5px; height: 5px");
        box_t *late = element(container, "position: absolute; left: 200px; top: 200px; width: 30px; height: 30px; anchor-name: --a");
        /* the target first: the early anchor is placed for it */
        box_t *abs[] = {target, early, late};
        layout(abs, 3);
        offsets(target, 40, 50, 5, 5, "the earlier absolutely positioned anchor");
        /* only a later one: not acceptable, the fallback */
        scene(800, 600);
        container = element(root, "position: relative; width: 400px; height: 400px");
        at(container, 0, 0, 400, 400);
        target = element(container, "position: absolute; left: anchor(--a right, 1px); top: anchor(--a bottom, 2px); "
                         "width: 5px; height: 5px");
        late = element(container, "position: absolute; left: 200px; top: 200px; width: 30px; height: 30px; anchor-name: --a");
        box_t *abs2[] = {target, late};
        layout(abs2, 2);
        offsets(target, 1, 2, 5, 5, "a later absolutely positioned anchor");
        /* a descendant of the box is not an anchor for it */
        scene(800, 600);
        container = element(root, "position: relative; width: 400px; height: 400px");
        at(container, 0, 0, 400, 400);
        target = element(container, "position: absolute; left: anchor(--a right, 1px); top: 0px; width: 50px; height: 50px");
        box_t *inner = element(target, "anchor-name: --a; width: 10px; height: 10px");
        at(inner, 0, 0, 10, 10);
        layout(&target, 1);
        offsets(target, 1, 0, 50, 50, "the box's own descendant");
    }

    /* try-tactic-basic.html: an option (@position-try --pf { left: 10px; top: 20px }) and its try
       tactics; the base styles overflow (left: 99999px) in a 400x400 containing block */
    group = "try-tactic-basic";
    {
        static const struct { const char *tactics; double l, t, w, h; } cases[] = {
            {"", 10, 20, 30, 40}, {"flip-block", 10, 340, 30, 40}, {"flip-y", 10, 340, 30, 40},
            {"flip-inline", 360, 20, 30, 40}, {"flip-x", 360, 20, 30, 40},
            {"flip-block flip-inline", 360, 340, 30, 40}, {"flip-inline flip-block", 360, 340, 30, 40},
            {"flip-y flip-x", 360, 340, 30, 40}, {"flip-x flip-y", 360, 340, 30, 40},
            {"flip-start", 20, 10, 40, 30}, {"flip-block flip-start flip-inline", 20, 10, 40, 30},
            {"flip-inline flip-start flip-block", 20, 10, 40, 30}, {"flip-y flip-start flip-x", 20, 10, 40, 30},
            {"flip-x flip-start flip-y", 20, 10, 40, 30},
            {"flip-start flip-block", 20, 360, 40, 30}, {"flip-inline flip-start", 20, 360, 40, 30},
            {"flip-start flip-y", 20, 360, 40, 30}, {"flip-x flip-start", 20, 360, 40, 30},
            {"flip-start flip-inline", 340, 10, 40, 30}, {"flip-block flip-start", 340, 10, 40, 30},
            {"flip-start flip-x", 340, 10, 40, 30}, {"flip-y flip-start", 340, 10, 40, 30},
            {"flip-start flip-block flip-inline", 340, 360, 40, 30}, {"flip-start flip-inline flip-block", 340, 360, 40, 30},
            {"flip-inline flip-block flip-start", 340, 360, 40, 30}, {"flip-block flip-inline flip-start", 340, 360, 40, 30},
            {"flip-start flip-y flip-x", 340, 360, 40, 30}, {"flip-start flip-x flip-y", 340, 360, 40, 30},
            {"flip-x flip-y flip-start", 340, 360, 40, 30}, {"flip-y flip-x flip-start", 340, 360, 40, 30},
        };
        static const char *const names[] = {"--pf"}, *const decls[] = {"left: 10px; top: 20px"};
        for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
            scene(800, 600);
            box_t *cb = element(root, "position: absolute; width: 400px; height: 400px");
            at(cb, 1, 1, 400, 400);
            const char *base = "position: absolute; left: 99999px; width: 30px; height: 40px";
            char text[320];
            snprintf(text, sizeof text, "%s; position-try-fallbacks: --pf %s", base, cases[i].tactics);
            box_t *target = element(cb, text);
            rules(target, base, names, decls, 1);
            layout(&target, 1);
            offsets(target, cases[i].l, cases[i].t, cases[i].w, cases[i].h, cases[i].tactics[0] ? cases[i].tactics : "(none)");
        }
    }

    /* position-try-order-basic.html: options sorted by the size of the inset-modified containing
       block each gives (§6.2), in a 400x400 containing block with the anchor at (150, 200)
       150x150 and a 40x40 box whose base styles overflow (left: 450px) */
    group = "position-try-order-basic";
    {
        static const char *const names[] = {"--right", "--left", "--top", "--bottom", "--right-sweep", "--left-sweep",
                                            "--bottom-sweep", "--top-sweep"};
        static const char *const decls[] = {
            "top: auto; right: auto; bottom: auto; left: anchor(right)",
            "top: auto; right: anchor(left); bottom: auto; left: auto",
            "top: auto; right: auto; bottom: anchor(top); left: auto",
            "top: anchor(bottom); right: auto; bottom: auto; left: auto",
            "top: anchor(top); bottom: anchor(bottom); right: auto; left: anchor(right); align-self: center",
            "top: anchor(top); bottom: anchor(bottom); right: anchor(left); left: auto; align-self: center",
            "left: anchor(left); right: anchor(right); top: anchor(bottom); justify-self: center",
            "left: anchor(left); right: anchor(right); bottom: anchor(top); justify-self: center",
        };
        /* where each option alone puts the box */
        static const double at_of[][2] = {{300, 0}, {110, 0}, {0, 160}, {0, 350}, {300, 255}, {110, 255}, {205, 350}, {205, 160}};
        static const struct { const char *try; int expected; } cases[] = {
            {"--right", 0}, {"--left", 1}, {"--top", 2}, {"--bottom", 3},
            {"--right, --left, --bottom, --top", 0}, {"normal --top, --left, --bottom, --right", 2},
            {"most-block-size --right, --left", 0}, {"most-height --right, --left", 0},
            {"most-inline-size --right, --left", 1}, {"most-width --right, --left", 1},
            {"most-inline-size --bottom, --top", 3}, {"most-block-size --bottom, --top", 2},
            {"most-height --bottom, --top", 2},
            {"most-inline-size --right, --left, --bottom, --top", 3}, {"most-inline-size --right, --left, --top, --bottom", 2},
            {"most-block-size --bottom, --top, --right, --left", 0}, {"most-block-size --bottom, --top, --left, --right", 1},
            {"most-inline-size --left-sweep, --bottom-sweep", 5}, {"most-inline-size --bottom-sweep, --left-sweep", 6},
            {"most-block-size --left-sweep, --bottom-sweep", 5}, {"most-block-size --bottom-sweep, --left-sweep", 5},
            {"most-inline-size --right-sweep, --left-sweep, --bottom-sweep, --top-sweep", 5},
            {"most-block-size --right-sweep, --left-sweep, --bottom-sweep, --top-sweep", 7},
            {"most-inline-size --right-sweep, --left-sweep, --bottom-sweep, --top-sweep, --bottom", 3},
            {"most-block-size --right-sweep, --left-sweep, --bottom-sweep, --top-sweep, --right", 0},
        };
        for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
            scene(800, 600);
            box_t *cb = element(root, "position: absolute; width: 400px; height: 400px");
            at(cb, 1, 1, 400, 400);
            box_t *anchor = element(cb, "position: absolute; left: 150px; top: 200px; width: 150px; height: 150px; "
                                    "anchor-name: --a");
            const char *base = "position: absolute; left: 450px; width: 40px; height: 40px; position-anchor: --a";
            char text[400];
            const char *list = cases[i].try, *order = "normal";
            static const char *const orders[] = {"normal", "most-block-size", "most-height", "most-inline-size", "most-width"};
            for (size_t k = 0; k < sizeof orders / sizeof *orders; k++)
                if (!strncmp(list, orders[k], strlen(orders[k])) && list[strlen(orders[k])] == ' ') {
                    order = orders[k]; list += strlen(orders[k]) + 1;
                }
            snprintf(text, sizeof text, "%s; position-try-order: %s; position-try-fallbacks: %s", base, order, list);
            box_t *target = element(cb, text);
            rules(target, base, names, decls, 8);
            box_t *abs[] = {anchor, target};
            layout(abs, 2);
            offsets(target, at_of[cases[i].expected][0], at_of[cases[i].expected][1], 40, 40, cases[i].try);
        }
    }

    /* §6.5.1.1: once an option is chosen it stays while it does not overflow, and returns to the
       base styles only when the option overflows and they fit; a fallback-sensitive change forgets it */
    group = "last-successful-option";
    {
        scene(800, 600);
        box_t *cb = element(root, "position: absolute; width: 400px; height: 400px");
        at(cb, 0, 0, 400, 400);
        box_t *anchor = element(cb, "anchor-name: --a; width: 50px; height: 50px");
        at(anchor, 340, 100, 50, 50);
        const char *base = "position: absolute; width: 40px; height: 40px; position-anchor: --a; top: anchor(top)";
        char text[320];
        snprintf(text, sizeof text, "%s; left: anchor(right); position-try-fallbacks: flip-x", base);
        box_t *target = element(cb, text);
        layout(&target, 1);
        offsets(target, 300, 100, 40, 40, "flipped: the base overflows");
        /* the anchor moves left: the base would fit again, but the flipped option still fits */
        at(anchor, 100, 100, 50, 50);
        layout(&target, 1);
        offsets(target, 60, 100, 40, 40, "keeps the last successful option");
        /* the anchor moves to the left edge: the option overflows, the base fits */
        at(anchor, 0, 100, 50, 50);
        layout(&target, 1);
        offsets(target, 50, 100, 40, 40, "back to the base styles");
        /* a change of an accepted property forgets the last successful option */
        at(anchor, 340, 100, 50, 50);
        layout(&target, 1);
        offsets(target, 300, 100, 40, 40, "flipped again");
        at(anchor, 100, 100, 50, 50);
        snprintf(text, sizeof text, "%s; left: anchor(right); position-try-fallbacks: flip-x; height: 41px", base);
        restyle(target, text);
        layout(&target, 1);
        offsets(target, 150, 100, 40, 41, "a fallback-sensitive change returns to the base styles");
        /* no option fits: the current styles stay */
        at(anchor, 0, 100, 400, 50);
        layout(&target, 1);
        offsets(target, 400, 100, 40, 41, "nothing fits");
    }

    /* position-try-fallbacks: <position-area> options, and position-visibility (§6.6) */
    group = "area-options-visibility";
    {
        scene(800, 600);
        box_t *cb = element(root, "position: absolute; width: 400px; height: 400px");
        at(cb, 0, 0, 400, 400);
        box_t *anchor = element(cb, "anchor-name: --a; width: 100px; height: 50px");
        at(anchor, 100, 10, 100, 50);
        box_t *target = element(cb, "position: absolute; width: 60px; height: 40px; position-anchor: --a; "
                                "position-area: top; position-try-fallbacks: bottom, right");
        layout(&target, 1);
        /* top does not fit (10px above the anchor), bottom does: span-all horizontally, anchor-centered */
        offsets(target, 120, 60, 60, 40, "the bottom option");
        check(!box_anchor_hidden(target), "visible");

        restyle(target, "position: absolute; width: 60px; height: 40px; position-anchor: --a; position-area: top; "
                "position-visibility: no-overflow");
        layout(&target, 1);
        check(box_anchor_hidden(target), "no-overflow hides an overflowing box");

        restyle(target, "position: absolute; width: 60px; height: 40px; position-anchor: --missing; "
                "top: anchor(bottom, 0px); position-visibility: anchor-valid");
        layout(&target, 1);
        check(box_anchor_hidden(target), "anchor-valid hides a box whose default anchor is missing");
        restyle(target, "position: absolute; width: 60px; height: 40px; position-anchor: --missing; "
                "top: 0px; position-visibility: anchor-valid");
        layout(&target, 1);
        check(!box_anchor_hidden(target), "anchor-valid: no reference to the default anchor");
        restyle(target, "position: absolute; width: 60px; height: 40px; position-anchor: --missing; "
                "top: anchor(bottom, 0px); position-visibility: always");
        layout(&target, 1);
        check(!box_anchor_hidden(target), "always");

        /* anchor-visible (the initial value): the anchor clipped away by an overflow: hidden box
           between it and the containing block */
        scene(800, 600);
        cb = element(root, "position: relative; width: 400px; height: 400px");
        at(cb, 0, 0, 400, 400);
        box_t *clip = element(cb, "overflow: hidden; width: 100px; height: 100px");
        at(clip, 0, 0, 100, 100);
        anchor = element(clip, "anchor-name: --a; width: 20px; height: 20px");
        at(anchor, 10, 10, 20, 20);
        target = element(cb, "position: absolute; width: 10px; height: 10px; position-anchor: --a; position-area: bottom");
        layout(&target, 1);
        check(!box_anchor_hidden(target), "anchor-visible: the anchor is in view");
        at(anchor, 10, 150, 20, 20);
        layout(&target, 1);
        check(box_anchor_hidden(target), "anchor-visible: the anchor is clipped away");
        clip->node->scroll_y = 100;
        check(!box_anchor_hidden(target), "anchor-visible: scrolled back into view");
        restyle(anchor, "anchor-name: --a; width: 20px; height: 20px; visibility: hidden");
        check(box_anchor_hidden(target), "anchor-visible: an invisible anchor");
    }

    /* §3.3: the anchor in a scroll container, the box outside it. The anchor's remembered scroll
       offset is taken at the box's first layout; later scrolling shifts the box (its default scroll
       shift) without changing its layout */
    group = "scroll";
    {
        scene(800, 600);
        box_t *cb = element(root, "position: relative; width: 400px; height: 400px");
        at(cb, 0, 0, 400, 400);
        box_t *scroller = element(cb, "overflow: scroll; width: 200px; height: 200px");
        at(scroller, 0, 0, 200, 200);
        box_t *anchor = element(scroller, "anchor-name: --a; width: 50px; height: 50px");
        at(anchor, 20, 300, 50, 50);
        scroller->node->scroll_y = 250;
        box_t *target = element(cb, "position: absolute; width: 10px; height: 10px; position-anchor: --a; "
                                "top: anchor(bottom); left: anchor(right)");
        layout(&target, 1);
        /* laid out where the anchor is: 300 - 250 + 50 */
        offsets(target, 70, 100, 10, 10, "at the recalculation point");
        near(box_anchor_shift(target, 1), 0, "no shift yet");
        scroller->node->scroll_y = 280;
        near(box_anchor_shift(target, 1), -30, "the default scroll shift follows the anchor");
        near(box_visual_y(target), 70, "where it is shown");
        layout(&target, 1);
        offsets(target, 70, 100, 10, 10, "a later layout keeps the remembered offset");
        near(box_anchor_shift(target, 1), -30, "and the shift");
        /* a fallback-sensitive change is a new recalculation point */
        restyle(target, "position: absolute; width: 11px; height: 10px; position-anchor: --a; top: anchor(bottom); "
                "left: anchor(right)");
        layout(&target, 1);
        offsets(target, 70, 70, 11, 10, "a recalculation point takes the scroll now");
        near(box_anchor_shift(target, 1), 0, "no shift after it");
        /* not compensating in an axis with no reference to the default anchor's scroller there */
        restyle(target, "position: absolute; width: 10px; height: 10px; position-anchor: --a; top: 5px; "
                "left: anchor(right)");
        layout(&target, 1);
        scroller->node->scroll_y = 300;
        near(box_anchor_shift(target, 1), 0, "no compensation in the block axis");
        scroller->node->scroll_x = 0;
        near(box_anchor_shift(target, 0), 0, "nothing scrolled in the inline axis");
    }

    printf("anchor-position: %d checks, %d failures\n", checks, failed);
    return failed != 0;
}
