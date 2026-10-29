/* Cases for the product CSSOM View geometry (tests/check_cssom_view_native.py): GeometryUtils'
   complete transform through boxes, transforms, scrolling and frames, the box options, inline
   fragments, the documents check, checkVisibility and scrollParent. Boxes are placed at their
   x, y (content edge); a case sets the transforms, fragments and scroll positions it uses. */
static int failures, passes;
#define CHECK(c) do { if (c) passes++; else { failures++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool near(double a, double b) { return fabs(a - b) < 1e-6; }

static style_t block_style, inline_style, scroll_style, fixed_style;
struct page { web_doc doc; struct web_js_state js; node_t document, html, body; box_t root, html_box, body_box; };
static void page_init(struct page *p) {
    memset(p, 0, sizeof *p);
    p->doc.live = true; p->doc.js = &p->js; p->js.doc = &p->doc;
    p->doc.width = 800; p->doc.height = 600;
    p->document.type = N_DOC; p->document.owner = &p->doc; p->doc.root = &p->document;
    p->html.type = N_ELEM; p->html.owner = &p->doc; p->html.parent = &p->document; p->html.style = &block_style;
    p->body.type = N_ELEM; p->body.owner = &p->doc; p->body.parent = &p->html; p->body.style = &block_style;
    p->doc.html = &p->html; p->doc.body = &p->body;
    p->root.st = &block_style; p->doc.root_box = &p->root;
    p->html_box.st = &block_style; p->html_box.node = &p->html; p->html_box.cb = &p->root; p->html_box.parent = &p->root; p->html.box = &p->html_box;
    p->body_box.st = &block_style; p->body_box.node = &p->body; p->body_box.cb = &p->html_box; p->body_box.parent = &p->html_box; p->body.box = &p->body_box;
}
/* an element with a block box: content edge at (x, y), w x h content, uniform p, b, m */
static void element(struct page *p, node_t *n, box_t *b, node_t *parent, box_t *container,
                    double x, double y, double w, double h, double pad, double border, double margin) {
    memset(n, 0, sizeof *n); memset(b, 0, sizeof *b);
    n->type = N_ELEM; n->owner = &p->doc; n->parent = parent; n->style = &block_style; n->box = b;
    b->node = n; b->st = &block_style; b->kind = B_BLOCK; b->cb = container; b->parent = container;
    b->x = x; b->y = y; b->w = w; b->h = h;
    for (int i = 0; i < 4; i++) { b->p[i] = pad; b->b[i] = border; b->m[i] = margin; }
}
static void transform_set(const box_t *b, double a, double c, double d, double e, double tx, double ty) {
    transformed[transform_count] = b;
    double m[6] = {a, c, d, e, tx, ty};
    memcpy(matrices[transform_count++], m, sizeof m);
}
static void fragment_add(node_t *n, const box_t *space, double x, double y, double w, double h) {
    fragment_node[fragment_count] = n; fragment_space[fragment_count] = space;
    fragment_rect[fragment_count][0] = x; fragment_rect[fragment_count][1] = y;
    fragment_rect[fragment_count][2] = w; fragment_rect[fragment_count][3] = h;
    fragment_count++;
}
static void reset(void) { transform_count = fragment_count = 0; }

static void complete_transform(void) {
    struct page p; page_init(&p); reset();
    node_t a; box_t ab;
    /* content edge (30, 40), padding 5, border 2: border-box corner at (23, 33) */
    element(&p, &a, &ab, &p.body, &p.body_box, 30, 40, 100, 50, 5, 2, 7);
    double m[6];
    CHECK(geometry_transform(&p.js, &a, false, m) == GEOMETRY_OK);
    double x = 0, y = 0; geometry_apply(m, &x, &y);
    CHECK(near(x, 23) && near(y, 33));
    /* the viewport scroll is subtracted: client coordinates */
    p.js.sx = 10; p.js.sy = 20;
    CHECK(geometry_transform(&p.js, &a, false, m) == GEOMETRY_OK);
    x = 0; y = 0; geometry_apply(m, &x, &y);
    CHECK(near(x, 13) && near(y, 13));
    p.js.sx = p.js.sy = 0;
    /* a transform on the element and one on its parent: the element's applies first */
    transform_set(&ab, 0, 1, -1, 0, 0, 0);            /* rotate 90deg about the origin */
    transform_set(&p.body_box, 2, 0, 0, 2, 0, 0);     /* then scale 2 */
    CHECK(geometry_transform(&p.js, &a, false, m) == GEOMETRY_OK);
    x = 0; y = 0; geometry_apply(m, &x, &y);
    CHECK(near(x, -66) && near(y, 46));               /* (23,33) -> (-33,23) -> (-66,46) */
    /* box_transform_rect bounds the quad of the whole chain */
    double r[4] = {0, 0, 10, 20}; box_transform_rect(&ab, r);
    CHECK(near(r[0], -40) && near(r[1], 0) && near(r[2], 40) && near(r[3], 20));
    /* the inverse maps back */
    double inv[6]; geometry_invert(m, inv); x = -66; y = 46; geometry_apply(inv, &x, &y);
    CHECK(near(x, 0) && near(y, 0));
    /* a non-invertible transform maps to NaN */
    double flat[6] = {0, 0, 0, 0, 3, 4}; geometry_invert(flat, inv); x = 1; y = 1; geometry_apply(inv, &x, &y);
    CHECK(isnan(x) && isnan(y));
    reset();
    /* a Document's space is its viewport */
    CHECK(geometry_transform(&p.js, &p.document, false, m) == GEOMETRY_OK);
    x = 5; y = 6; geometry_apply(m, &x, &y); CHECK(near(x, 5) && near(y, 6));
    /* an element without a box: NotFound, or its rendered ancestor when asked */
    node_t bare = {0}; bare.type = N_ELEM; bare.owner = &p.doc; bare.parent = &a;
    CHECK(geometry_transform(&p.js, &bare, false, m) == GEOMETRY_NOT_FOUND);
    CHECK(geometry_transform(&p.js, &bare, true, m) == GEOMETRY_OK);
    x = 0; y = 0; geometry_apply(m, &x, &y); CHECK(near(x, 23) && near(y, 33));
    /* a Text node uses its element's space */
    node_t text = {0}; text.type = N_TEXT; text.owner = &p.doc; text.parent = &a;
    CHECK(geometry_transform(&p.js, &text, false, m) == GEOMETRY_OK);
    x = 0; y = 0; geometry_apply(m, &x, &y); CHECK(near(x, 23) && near(y, 33));
}

static void box_options(void) {
    struct page p; page_init(&p); reset();
    node_t a, t; box_t ab, tb;
    element(&p, &a, &ab, &p.body, &p.body_box, 30, 40, 100, 50, 5, 2, 7);
    element(&p, &t, &tb, &p.body, &p.body_box, 300, 400, 10, 10, 1, 1, 0);
    double x, y;
    geometry_box_offset(&a, GEOMETRY_MARGIN, false, &x, &y); CHECK(near(x, -7) && near(y, -7));
    geometry_box_offset(&a, GEOMETRY_PADDING, false, &x, &y); CHECK(near(x, 2) && near(y, 2));
    geometry_box_offset(&a, GEOMETRY_CONTENT, false, &x, &y); CHECK(near(x, 7) && near(y, 7));
    geometry_box_offset(&p.document, GEOMETRY_CONTENT, false, &x, &y); CHECK(near(x, 0) && near(y, 0));
    /* a's content-box origin in t's padding box: (30,40) - (299,399) */
    double xy[2] = {0, 0};
    CHECK(geometry_convert(&p.js, &t, &a, GEOMETRY_CONTENT, GEOMETRY_PADDING, false, xy, 1) == GEOMETRY_OK);
    CHECK(near(xy[0], 30 - 299) && near(xy[1], 40 - 399));
    /* getBoxQuads: the margin box relative to the document */
    double *quads = NULL; size_t count = 0;
    CHECK(geometry_box_quads(&p.js, &a, GEOMETRY_MARGIN, &p.document, &quads, &count) == GEOMETRY_OK && count == 1);
    CHECK(quads && near(quads[0], 16) && near(quads[1], 26) && near(quads[4], 16 + 128) && near(quads[5], 26 + 78));
    free(quads);
    /* the content box relative to t: t's border-box corner is (298, 398) */
    CHECK(geometry_box_quads(&p.js, &a, GEOMETRY_CONTENT, &t, &quads, &count) == GEOMETRY_OK && count == 1);
    CHECK(quads && near(quads[0], 30 - 298) && near(quads[1], 40 - 398) && near(quads[4], 130 - 298) && near(quads[5], 90 - 398));
    free(quads);
    /* the Document's quad is its layout viewport */
    CHECK(geometry_box_quads(&p.js, &p.document, GEOMETRY_BORDER, &p.document, &quads, &count) == GEOMETRY_OK && count == 1);
    CHECK(quads && near(quads[4], 800) && near(quads[5], 600));
    free(quads);
    /* an unrendered element has no quads; an unrendered relativeTo is NotFound */
    node_t bare = {0}; bare.type = N_ELEM; bare.owner = &p.doc; bare.parent = &p.body;
    CHECK(geometry_box_quads(&p.js, &bare, GEOMETRY_BORDER, &p.document, &quads, &count) == GEOMETRY_OK && count == 0);
    CHECK(geometry_box_quads(&p.js, &a, GEOMETRY_BORDER, &bare, &quads, &count) == GEOMETRY_NOT_FOUND);
}

static void inline_fragments(void) {
    struct page p; page_init(&p); reset();
    node_t span; box_t sb;
    element(&p, &span, &sb, &p.body, &p.body_box, 0, 0, 0, 0, 3, 2, 5);
    sb.kind = B_INLINE;
    /* two line fragments (border boxes) of the inline, in the body's space */
    fragment_add(&span, &p.body_box, 10, 20, 60, 18);
    fragment_add(&span, &p.body_box, 0, 40, 30, 18);
    double *quads = NULL; size_t count = 0;
    CHECK(geometry_box_quads(&p.js, &span, GEOMETRY_BORDER, &p.document, &quads, &count) == GEOMETRY_OK && count == 2);
    CHECK(quads && near(quads[0], 10) && near(quads[1], 20) && near(quads[8], 0) && near(quads[9], 40) && near(quads[12], 30));
    free(quads);
    /* content: start edges on the first fragment, end edges on the last, block edges on both */
    CHECK(geometry_box_quads(&p.js, &span, GEOMETRY_CONTENT, &p.document, &quads, &count) == GEOMETRY_OK && count == 2);
    CHECK(quads && near(quads[0], 15) && near(quads[1], 25) && near(quads[2], 70) && near(quads[5], 33));
    CHECK(quads && near(quads[8], 0) && near(quads[9], 45) && near(quads[12], 25) && near(quads[13], 53));
    free(quads);
    CHECK(geometry_box_quads(&p.js, &span, GEOMETRY_MARGIN, &p.document, &quads, &count) == GEOMETRY_OK && count == 2);
    CHECK(quads && near(quads[0], 5) && near(quads[1], 15) && near(quads[2], 70) && near(quads[12], 35));
    free(quads);
    /* a Text node's quads are its client rectangles in the document's viewport */
    node_t text = {0}; text.type = N_TEXT; text.owner = &p.doc; text.parent = &span;
    fragment_add(&text, &p.body_box, 12, 22, 40, 14);
    p.js.sy = 5;
    CHECK(geometry_box_quads(&p.js, &text, GEOMETRY_CONTENT, &p.document, &quads, &count) == GEOMETRY_OK && count == 1);
    CHECK(quads && near(quads[0], 12) && near(quads[1], 17) && near(quads[4], 52) && near(quads[5], 31));
    free(quads);
}

static void frames(void) {
    struct page outer, inner; page_init(&outer); page_init(&inner); reset();
    node_t frame; box_t fb;
    element(&outer, &frame, &fb, &outer.body, &outer.body_box, 100, 200, 300, 150, 4, 1, 0);
    fb.content_dy = 0;
    inner.doc.frame_parent = &outer.doc; inner.doc.frame_element = &frame;
    node_t a; box_t ab;
    element(&inner, &a, &ab, &inner.body, &inner.body_box, 10, 20, 5, 5, 0, 0, 0);
    double m[6], x = 0, y = 0;
    CHECK(geometry_transform(&inner.js, &a, false, m) == GEOMETRY_OK);
    geometry_apply(m, &x, &y);
    CHECK(near(x, 110) && near(y, 220));             /* the frame's content edge is (100, 200) */
    /* both chains reach the outer document: allowed when same origin, SecurityError when not */
    CHECK(geometry_documents_allowed(&inner.doc, &outer.doc));
    double xy[2] = {0, 0};
    CHECK(geometry_convert(&outer.js, &outer.document, &a, GEOMETRY_BORDER, GEOMETRY_BORDER, false, xy, 1) == GEOMETRY_OK);
    CHECK(near(xy[0], 110) && near(xy[1], 220));
    inner.js.foreign = true;
    CHECK(!geometry_documents_allowed(&inner.doc, &outer.doc));
    CHECK(geometry_convert(&outer.js, &outer.document, &a, GEOMETRY_BORDER, GEOMETRY_BORDER, false, xy, 1) == GEOMETRY_SECURITY);
}

static void visibility(void) {
    struct page p; page_init(&p); reset();
    node_t a, child; box_t ab, cb;
    style_t faded = block_style, hidden = block_style, cv = block_style;
    faded.opacity = 0; hidden.visibility = 1; cv.content_visibility = CV_HIDDEN;
    element(&p, &a, &ab, &p.body, &p.body_box, 0, 0, 10, 10, 0, 0, 0);
    element(&p, &child, &cb, &a, &ab, 0, 0, 5, 5, 0, 0, 0);
    CHECK(geometry_check_visibility(&p.js, &child, 7));
    a.style = &faded;
    CHECK(geometry_check_visibility(&p.js, &child, 0) && !geometry_check_visibility(&p.js, &child, 1));
    a.style = &block_style; child.style = &hidden;
    CHECK(geometry_check_visibility(&p.js, &child, 1) && !geometry_check_visibility(&p.js, &child, 2));
    child.style = &block_style; a.style = &cv;
    CHECK(!geometry_check_visibility(&p.js, &child, 0));
    a.style = &block_style; child.box = NULL;
    CHECK(!geometry_check_visibility(&p.js, &child, 0));
}

static void scroll_parents(void) {
    struct page p; page_init(&p); reset();
    node_t scroller, a, fixed; box_t sb, ab, fb;
    element(&p, &scroller, &sb, &p.body, &p.body_box, 0, 0, 100, 100, 0, 0, 0);
    sb.st = &scroll_style; scroller.style = &scroll_style;
    element(&p, &a, &ab, &scroller, &sb, 0, 0, 10, 10, 0, 0, 0);
    CHECK(scroll_parent(&p.doc, &a) == &scroller);
    CHECK(scroll_parent(&p.doc, &scroller) == &p.html);          /* the scrollingElement */
    CHECK(scroll_parent(&p.doc, &p.body) == NULL && scroll_parent(&p.doc, &p.html) == NULL);
    element(&p, &fixed, &fb, &a, &p.root, 0, 0, 10, 10, 0, 0, 0);
    fb.st = &fixed_style; fixed.style = &fixed_style;
    CHECK(scroll_parent(&p.doc, &fixed) == NULL);
    p.doc.quirks = true;
    CHECK(scrolling_element(&p.doc) == &p.body);                 /* body not potentially scrollable */
}

int main(void) {
    block_style.display = D_BLOCK; inline_style.display = D_INLINE;
    block_style.opacity = inline_style.opacity = 1; /* the initial opacity */
    scroll_style = block_style; scroll_style.overflow = OV_AUTO;
    fixed_style = block_style; fixed_style.position = POS_FIXED;
    complete_transform();
    box_options();
    inline_fragments();
    frames();
    visibility();
    scroll_parents();
    printf("cssom view native: %d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
