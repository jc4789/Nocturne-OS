/* Cases for the product text geometry (tests/check_text_geometry_native.py): the processed
   text's source map (box.c), UTF-16 offsets of counted data (doc.c), the line layout's text
   and inline fragments, boundaries and DOM positions (layout.c), and find in page's text and
   the text control's line (paint.c). Fonts are fixed: 10 px per code point, ascent 0.8 em,
   descent 0.2 em; boxes are placed at their x and y. */
static int failures, passes;
#define CHECK(c) do { if (c) passes++; else { failures++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool near(double a, double b) { return fabs(a - b) < 1e-4; }

static style_t S, before_style, after_style;

struct rects { int n; double v[8][4]; const box_t *space[8]; };
static bool collect(void *opaque, const box_t *space, double x, double y, double w, double h) {
    struct rects *r = opaque;
    if (r->n < 8) { r->v[r->n][0] = x; r->v[r->n][1] = y; r->v[r->n][2] = w; r->v[r->n][3] = h; r->space[r->n] = space; }
    r->n++;
    return true;
}
static bool rect_is(const struct rects *r, int i, double x, double y, double w, double h) {
    return i < r->n && near(r->v[i][0], x) && near(r->v[i][1], y) && near(r->v[i][2], w) && near(r->v[i][3], h);
}
static struct rects text_rects(node_t *n, uint32_t start, uint32_t end) {
    struct rects r = {0};
    CHECK(layout_text_rects(n, start, end, true, collect, &r));
    return r;
}

/* A Text node, its text box (processed as layout would) and the block whose runs show it. */
struct scene { node_t node; box_t text, block; struct run runs[4]; };
static void scene_text(struct scene *s, arena_t *a, const char *data, int white_space) {
    memset(s, 0, sizeof *s);
    S.white_space = (uint8_t)white_space;
    s->node.type = N_TEXT; s->node.text = (char *)data; s->node.textlen = strlen(data); s->node.box = &s->text;
    s->text.kind = B_TEXT; s->text.node = &s->node; s->text.st = &S; s->text.parent = &s->block;
    size_t length = 0;
    s->text.text = ws_process(a, data, s->node.textlen, &S, &length, NULL); s->text.len = length;
    s->block.kind = B_BLOCK; s->block.inline_ctx = true; s->block.x = 100; s->block.y = 40;
    s->block.runs = s->runs;
}
static void scene_run(struct scene *s, size_t from, int bytes, float x, float baseline, float w) {
    struct run *r = &s->runs[s->block.nruns];
    *r = (struct run){.x = x, .y = baseline, .w = w, .s = s->text.text + from, .n = bytes, .st = &S, .node = &s->node};
    if (!s->text.frag_end) s->text.frag_first = s->block.nruns;
    s->text.frag_end = ++s->block.nruns;
}

static void source_map_cases(arena_t *a) {
    struct scene s;
    scene_text(&s, a, "  a  b\tc\n", WS_NORMAL);
    CHECK(s.text.len == 7 && !memcmp(s.text.text, " a b c ", 7));
    uint32_t *map = box_text_source_map(a, &s.text);
    static const uint32_t collapsed[] = {0, 2, 3, 5, 6, 7, 8, 9};
    CHECK(map && !memcmp(map, collapsed, sizeof collapsed));

    scene_text(&s, a, "ab\xC2\xAD" "cd", WS_NORMAL); /* a soft hyphen is not shown */
    map = box_text_source_map(a, &s.text);
    static const uint32_t shy[] = {0, 1, 4, 5, 6};
    CHECK(s.text.len == 4 && map && !memcmp(map, shy, sizeof shy));

    scene_text(&s, a, "a\tb", WS_PRE); /* a tab expands to its stop: every space is the tab's */
    map = box_text_source_map(a, &s.text);
    CHECK(s.text.len == 9 && map && map[0] == 0 && map[1] == 1 && map[7] == 1 && map[8] == 2 && map[9] == 3);

    scene_text(&s, a, "a  \n  b", WS_PRE_LINE); /* spaces around a kept line break go */
    map = box_text_source_map(a, &s.text);
    static const uint32_t pre_line[] = {0, 3, 6, 7};
    CHECK(s.text.len == 3 && !memcmp(s.text.text, "a\nb", 3) && map && !memcmp(map, pre_line, sizeof pre_line));

    S.text_transform = TT_UPPER; /* in place, byte for byte */
    scene_text(&s, a, "ab", WS_NORMAL);
    map = box_text_source_map(a, &s.text);
    CHECK(!memcmp(s.text.text, "AB", 2) && map && map[0] == 0 && map[1] == 1 && map[2] == 2);
    S.text_transform = TT_NONE;

    scene_text(&s, a, "abc", WS_NORMAL); /* the data changed after the box was built */
    s.text.text = "abd";
    CHECK(!box_text_source_map(a, &s.text));
}

static void utf16_cases(void) {
    const char *star = "\xF0\x9F\x8C\xA0" "a" "\xF0\x9F\x8C\xA0"; /* U+1F320 a U+1F320 */
    size_t n = strlen(star);
    CHECK(doc_text_utf16(star, n, 4) == 2 && doc_text_utf16(star, n, 5) == 3 && doc_text_utf16(star, n, n) == 5);
    CHECK(doc_text_byte(star, n, 1, false) == 0 && doc_text_byte(star, n, 1, true) == 4);
    CHECK(doc_text_byte(star, n, 3, false) == 5 && doc_text_byte(star, n, 4, true) == 9 && doc_text_byte(star, n, 9, false) == 9);
    const char nul[] = {'a', 0, 'b'}; /* counted data may hold U+0000 */
    CHECK(doc_text_utf16(nul, 3, 3) == 3 && doc_text_byte(nul, 3, 2, false) == 2);
    const char *lone = "\xED\xA0\x80" "x"; /* WTF-8: an isolated surrogate is one unit */
    CHECK(doc_text_utf16(lone, 4, 3) == 1 && doc_text_byte(lone, 4, 1, false) == 3);
}

static void text_rect_cases(arena_t *a) {
    struct scene s;
    scene_text(&s, a, "hello world", WS_NORMAL);
    scene_run(&s, 0, 5, 0, 8, 50);   /* "hello" on the first line; its space ends the line */
    /* Another node's run between: each lookup checks the run's own node and text. */
    node_t other = {0}; other.type = N_TEXT;
    s.runs[1] = (struct run){.x = 60, .y = 8, .w = 10, .s = "z", .n = 1, .st = &S, .node = &other};
    s.block.nruns = 2;
    scene_run(&s, 6, 5, 0, 28, 50);  /* "world" on the second */
    struct rects r = text_rects(&s.node, 0, UINT32_MAX);
    CHECK(r.n == 2 && rect_is(&r, 0, 100, 40, 50, 10) && rect_is(&r, 1, 100, 60, 50, 10) && r.space[0] == &s.block);
    r = text_rects(&s.node, 0, 11);
    CHECK(r.n == 2 && rect_is(&r, 0, 100, 40, 50, 10) && rect_is(&r, 1, 100, 60, 50, 10));
    r = text_rects(&s.node, 1, 3);
    CHECK(r.n == 1 && rect_is(&r, 0, 110, 40, 20, 10));
    r = text_rects(&s.node, 3, 8);
    CHECK(r.n == 2 && rect_is(&r, 0, 130, 40, 20, 10) && rect_is(&r, 1, 100, 60, 20, 10));
    r = text_rects(&s.node, 2, 2); /* identical boundary points: a zero-width caret box */
    CHECK(r.n == 1 && rect_is(&r, 0, 120, 40, 0, 10));
    r = text_rects(&s.node, 5, 6); /* only the collapsed line-end space: where it lies */
    CHECK(r.n == 1 && rect_is(&r, 0, 150, 40, 0, 10));
    r = text_rects(&s.node, 6, 6); /* the next line's start, not the previous line's end */
    CHECK(r.n == 1 && rect_is(&r, 0, 100, 60, 0, 10));
    r = text_rects(&s.node, 5, 5);
    CHECK(r.n == 1 && rect_is(&r, 0, 150, 40, 0, 10));

    /* Inside a surrogate pair, or a grapheme cluster, the whole of it counts. */
    scene_text(&s, a, "\xF0\x9F\x8C\xA0" "a" "\xF0\x9F\x8C\xA0", WS_NORMAL);
    scene_run(&s, 0, 9, 0, 8, 30);
    r = text_rects(&s.node, 1, 5);
    CHECK(r.n == 1 && rect_is(&r, 0, 100, 40, 30, 10));
    r = text_rects(&s.node, 0, 4);
    CHECK(r.n == 1 && rect_is(&r, 0, 100, 40, 30, 10));
    r = text_rects(&s.node, 1, 4);
    CHECK(r.n == 1 && rect_is(&r, 0, 100, 40, 30, 10));
    r = text_rects(&s.node, 2, 3);
    CHECK(r.n == 1 && rect_is(&r, 0, 110, 40, 10, 10));
    scene_text(&s, a, "e\xCC\x81x", WS_NORMAL); /* e, U+0301 COMBINING ACUTE ACCENT, x */
    scene_run(&s, 0, 4, 0, 8, 30);
    r = text_rects(&s.node, 1, 2);
    CHECK(r.n == 1 && rect_is(&r, 0, 100, 40, 20, 10));

    /* Collapsed leading white space: offsets map through the source map. */
    scene_text(&s, a, "  ab", WS_NORMAL);
    scene_run(&s, 1, 2, 0, 8, 20);
    r = text_rects(&s.node, 2, 3);
    CHECK(r.n == 1 && rect_is(&r, 0, 100, 40, 10, 10));
    r = text_rects(&s.node, 0, 1); /* nothing of it rendered: a zero-width box at the run */
    CHECK(r.n == 1 && rect_is(&r, 0, 100, 40, 0, 10));
    node_t *node = NULL; uint32_t offset = 99;
    CHECK(layout_run_position(&s.block, 0, 15, &node, &offset) && node == &s.node && offset == 3);
    CHECK(layout_run_position(&s.block, 0, 4, &node, &offset) && node == &s.node && offset == 2);

    /* No box, or a box without fragments: nothing. */
    s.text.frag_end = 0;
    r = text_rects(&s.node, 0, UINT32_MAX);
    CHECK(r.n == 0);
}

static void boundary_cases(arena_t *a) {
    struct scene s;
    scene_text(&s, a, "aaa", WS_NORMAL);
    scene_run(&s, 0, 3, 0, 8, 30);
    /* At a cluster's boundary: the next one. At its exact middle: the earlier one. */
    CHECK(layout_run_boundary(&s.runs[0], 20) == 2);
    CHECK(layout_run_boundary(&s.runs[0], 15) == 1);
    CHECK(layout_run_boundary(&s.runs[0], -1) == 0 && layout_run_boundary(&s.runs[0], 31) == 3);
    CHECK(near(layout_run_x(&s.runs[0], 0), 0) && near(layout_run_x(&s.runs[0], 2), 20) && near(layout_run_x(&s.runs[0], 3), 30));
}

static void generated_and_inline_cases(void) {
    node_t element = {0}, children[2] = {{0}}, other = {0};
    style_t own = S;
    own.before = &before_style; own.after = &after_style;
    element.type = N_ELEM; element.style = &own; element.first = &children[0]; children[0].next = &children[1];
    box_t block = {0}, inline_box = {0};
    block.kind = B_BLOCK; block.inline_ctx = true; block.x = 10; block.y = 20;
    struct run runs[2] = {
        {.x = 0, .y = 8, .w = 10, .s = "x", .n = 1, .st = &after_style, .node = &element},
        {.x = 10, .y = 8, .w = 10, .s = "y", .n = 1, .st = &before_style, .node = &element},
    };
    block.runs = runs; block.nruns = 2;
    node_t *node = NULL; uint32_t offset = 99;
    CHECK(layout_run_position(&block, 0, 3, &node, &offset) && node == &element && offset == 2);
    CHECK(layout_run_position(&block, 1, 3, &node, &offset) && node == &element && offset == 0);

    struct deco decos[4] = {
        {.x = 5, .y = 1, .w = 30, .h = 12, .st = &own, .node = &element},
        {.x = 40, .y = 1, .w = 5, .h = 12, .st = &S, .node = &other},
        {.x = 0, .y = 21, .w = 8, .h = 12, .st = &before_style, .node = &element}, /* its ::before */
        {.x = 0, .y = 41, .w = 9, .h = 12, .st = &own, .node = &element},          /* past its range */
    };
    block.decos = decos; block.ndecos = 4;
    inline_box.kind = B_INLINE; inline_box.node = &element; inline_box.parent = &block;
    inline_box.frag_first = 0; inline_box.frag_end = 3;
    element.box = &inline_box;
    struct rects r = {0};
    CHECK(layout_inline_rects(&element, true, collect, &r));
    CHECK(r.n == 1 && rect_is(&r, 0, 15, 21, 30, 12) && r.space[0] == &block);
    CHECK(layout_inline_root(&inline_box) == &block);
}

static void position_cases(arena_t *a) {
    struct scene s;
    scene_text(&s, a, "hello world", WS_NORMAL);
    scene_run(&s, 0, 5, 0, 8, 50);
    scene_run(&s, 6, 5, 0, 28, 50);
    node_t div = {0}; div.type = N_ELEM; div.style = &S; s.block.node = &div;
    node_t *node = NULL; uint32_t offset = 99;
    /* Beside the second line: its nearest boundary, its end. */
    CHECK(layout_position_at(&s.block, 300, 65, &node, &offset) && node == &s.node && offset == 11);
    /* Above every line: the nearest line by distance, then the nearest boundary in it. */
    CHECK(layout_position_at(&s.block, 112, 10, &node, &offset) && node == &s.node && offset == 1);
    /* From the text box: its formatting root's lines. */
    CHECK(layout_position_at(&s.text, 125, 62, &node, &offset) && node == &s.node && offset == 8);

    /* A replaced element: before it, or after it right of its middle. */
    node_t parent = {0}, text = {0}, img = {0};
    parent.type = N_ELEM; parent.first = &text; text.next = &img; text.parent = img.parent = &parent;
    img.type = N_ELEM;
    box_t image = {0};
    image.kind = B_ATOMIC; image.atomic = AT_IMG; image.node = &img; image.x = 200; image.y = 0; image.w = 40; image.h = 20;
    image.st = &S;
    CHECK(layout_position_at(&image, 230, 5, &node, &offset) && node == &parent && offset == 2);
    CHECK(layout_position_at(&image, 205, 5, &node, &offset) && node == &parent && offset == 1);
}

static void find_cases(void) {
    box_t block = {0};
    struct run runs[2] = {
        {.x = 0, .y = 8, .w = 50, .s = "Hello", .n = 5, .st = &S},
        {.x = 0, .y = 28, .w = 50, .s = "World", .n = 5, .st = &S},
    };
    block.runs = runs; block.nruns = 2;
    sbuf t; int *owner;
    CHECK(find_block_text(&block, &t, &owner));
    CHECK(t.n == 11 && !memcmp(t.p, "Hello World", 11) && owner[4] == 0 && owner[5] == -1 && owner[6] == 1);
    free(owner); sb_free(&t);
    CHECK(highlight(&block, 3, 5)); /* "lo Wo" */
    CHECK(runs[0].highlight && runs[0].highlight_first == 3 && runs[0].highlight_end == 5);
    CHECK(runs[1].highlight && runs[1].highlight_first == 0 && runs[1].highlight_end == 2);
}

static void input_cases(void) {
    struct input_line L;
    const char *secret = "a" "\xF0\x9F\x8C\xA0" "b"; /* a bullet per character */
    input_line_begin(&L, &S, secret, true, 100, -1);
    CHECK(!strcmp(L.drawn, "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2") && near(L.scroll, 0));
    CHECK(near(input_line_x(&L, &S, 5), 20));
    CHECK(input_line_byte(&L, &S, 25) == 5 && input_line_byte(&L, &S, 26) == 6 && input_line_byte(&L, &S, 4) == 0);
    input_line_end(&L);
    input_line_begin(&L, &S, "abc", false, 15, 3); /* the caret stays in view */
    CHECK(near(L.scroll, 17) && input_line_byte(&L, &S, 12) == 1);
    input_line_end(&L);
}

/* js.c's Range walk, with its element rectangles recorded (collect_client_rects is stubbed)
   and its Text rectangles from the real layout_text_rects. */
static node_t pool[48];
static int pool_used;
static style_t contents_style;
static box_t some_box;
static node_t *add(node_t *parent, int type) {
    node_t *n = &pool[pool_used++];
    memset(n, 0, sizeof *n);
    n->type = (uint8_t)type; n->parent = parent;
    if (parent) { if (parent->last) { parent->last->next = n; n->prev = parent->last; } else parent->first = n; parent->last = n; }
    return n;
}
static node_t *element(node_t *parent, bool box) {
    node_t *n = add(parent, N_ELEM);
    n->style = box ? &S : &contents_style; n->box = box ? &some_box : NULL;
    return n;
}
static node_t *text(node_t *parent, const char *data) {
    node_t *n = add(parent, N_TEXT);
    n->text = (char *)data; n->textlen = strlen(data);
    return n;
}
static bool walk(node_t *sc, uint32_t so, node_t *ec, uint32_t eo) {
    struct client_rect_output out = {0};
    element_count = fragment_count = 0;
    return range_client_rects(&out, sc, so, ec, eo, 0, 0);
}
static void range_walk_cases(arena_t *a) {
    contents_style = S; contents_style.display = D_CONTENTS;
    /* WPT range-bounding-client-rect-with-nested-text: four text rectangles and the two
       zero-size elements the range contains whose parent it does not. */
    pool_used = 0;
    node_t *container = element(NULL, true);
    struct scene hello, world;
    node_t *n1 = element(container, true), *first = element(n1, true);
    node_t *hello_text = text(first, "Hello");
    text(container, "\n  ");
    node_t *n2 = element(container, true), *d2 = element(n2, true); text(d2, "Firefox");
    node_t *n3 = element(container, true), *d3 = element(n3, true); text(d3, "Firefox again");
    node_t *n4 = element(container, true), *last = element(n4, true);
    node_t *world_text = text(last, "World");
    /* The rendered Text nodes are the pool's; their boxes and runs come from scenes. */
    scene_text(&hello, a, "Hello", WS_NORMAL); scene_run(&hello, 0, 5, 0, 8, 50);
    hello_text->box = &hello.text; hello.text.node = hello_text; hello.runs[0].node = hello_text;
    scene_text(&world, a, "World", WS_NORMAL); scene_run(&world, 0, 5, 0, 28, 50);
    world_text->box = &world.text; world.text.node = world_text; world.runs[0].node = world_text;
    CHECK(walk(hello_text, 0, world_text, 5));
    CHECK(element_count == 2 && elements[0] == n2 && elements[1] == n3);
    CHECK(fragment_count == 2 && near(fragments[0][2], 50) && near(fragments[1][1], 60));

    /* WPT range-bounding-client-rect-with-display-contents: through display:contents
       elements (no box) to their children. */
    pool_used = 0;
    container = element(NULL, true);
    node_t *before = element(container, true);
    node_t *contents = element(container, false), *a1 = element(contents, true);
    node_t *c1 = element(contents, false), *c2 = element(c1, false), *a2 = element(c2, true);
    element(a2, true); /* covered by a2's own rectangles */
    element(container, true);
    CHECK(walk(container, 1, container, 2));
    CHECK(element_count == 2 && elements[0] == a1 && elements[1] == a2);
    (void)before;

    /* Boundary points between children select nothing between them; a partially selected
       element (an ancestor of the end) gives no rectangles of its own. */
    CHECK(walk(container, 1, container, 1) && element_count == 0);
    CHECK(walk(container, 0, a2, 0) && element_count == 2 && elements[0] == before && elements[1] == a1);

    /* A collapsed range in a Text node: one zero-width rectangle. */
    pool_used = 0;
    container = element(NULL, true);
    node_t *only = text(container, "Hello");
    struct scene one;
    scene_text(&one, a, "Hello", WS_NORMAL); scene_run(&one, 0, 5, 0, 8, 50);
    only->box = &one.text; one.text.node = only; one.runs[0].node = only;
    CHECK(walk(only, 1, only, 1) && fragment_count == 1 && near(fragments[0][0], 110) && near(fragments[0][2], 0));
    CHECK(walk(only, 1, only, 4) && fragment_count == 1 && near(fragments[0][2], 30));
    CHECK(walk(container, 0, container, 1) && fragment_count == 1 && near(fragments[0][2], 50) && element_count == 0);
}

int main(void) {
    S.font_size = 10;
    before_style = after_style = S;
    arena_t a = {0};
    source_map_cases(&a);
    utf16_cases();
    text_rect_cases(&a);
    boundary_cases(&a);
    generated_and_inline_cases();
    position_cases(&a);
    find_cases();
    input_cases();
    range_walk_cases(&a);
    ar_free(&a);
    printf("text geometry: %d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
