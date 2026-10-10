/* Unsupported HTML object resources use native fallback flow, not a mock
   object renderer. Run inside Nocturne for actual layout, hit and paint. */
#include <stdio.h>
#include "nocturne.h"
#include "webi.h"
#include "elements.h"

static int checks, failures;
static uint32_t pixels[480 * 240];
static void check(const char *name, bool pass) {
    checks++;
    if (!pass) { failures++; printf("FAIL objectfallback %s\n", name); }
}
static web_doc *load(const char *html) {
    web_doc *d = web_parse(html, strlen(html), "https://fallback.test/page", "utf-8");
    check("native document allocated", d != NULL);
    if (d) web_layout(d, 480, 240);
    return d;
}
static node_t *id(web_doc *d, const char *name) {
    char selector[96]; snprintf(selector, sizeof selector, "#%s", name);
    pvec found = {0};
    bool selected = css_select(d, d->root, selector, &found);
    node_t *n = selected && found.n ? found.v[0] : NULL;
    pv_free(&found);
    return n;
}
static bool text(box_t *b, const char *value) {
    if (!b) return false;
    if (b->kind == B_TEXT && b->text && b->len == strlen(value) && !memcmp(b->text, value, b->len)) return true;
    for (box_t *c = b->first; c; c = c->next) if (text(c, value)) return true;
    return false;
}
static bool rect(web_doc *d, node_t *n, int *x, int *y, int *w, int *h) {
    return n && web_node_rect(d, n, x, y, w, h);
}
static void paint(web_doc *d) {
    for (unsigned i = 0; i < sizeof pixels / sizeof pixels[0]; i++) pixels[i] = 0xffffffff;
    canvas_t canvas; gfx_init(&canvas, pixels, 480, 240, 480);
    web_paint(d, &canvas, 0, 0, 480, 240, 0, 0);
}

static void inline_fallback(void) {
    web_doc *d = load("<!doctype html><body style='margin:0;font-size:16px;line-height:20px'>"
        "<div id=line><span id=before>before </span><object id=object role=none>"
        "<a id=link href='/next'>fallbackword</a></object><span id=after> after</span></div>");
    if (!d) return;
    node_t *object = id(d, "object"), *link = id(d, "link"), *after = id(d, "after"), *line = id(d, "line");
    check("ordinary HTML object and anchor parsed", object && object->tag == T_object && !object->foreign &&
          link && link->tag == T_a && link->parent == object && !link->foreign);
    check("resource-less object uses inline initial display", object && object->style && object->style->display == D_INLINE);
    check("object fallback owns normal inline box", object && object->box && object->box->kind == B_INLINE && object->box->atomic == AT_NONE);
    check("fallback anchor owns normal inline box", link && link->box && link->box->kind == B_INLINE);
    check("fallback text reaches real box tree", object && text(object->box, "fallbackword"));
    check("fallback is findable native text", web_find(d, "fallbackword", 0) >= 0);
    int lx = 0, ly = 0, lw = 0, lh = 0, ax = 0, ay = 0, aw = 0, ah = 0;
    check("fallback anchor has real native rectangle", rect(d, link, &lx, &ly, &lw, &lh) && lw > 0 && lh > 0);
    check("fallback anchor is one compact line not 300x150", lw < 300 && lh <= 24 && line && line->box && line->box->h <= 24);
    check("following text remains on same inline line", rect(d, after, &ax, &ay, &aw, &ah) && ay == ly && ax >= lx + lw);
    check("fallback object has no replaced-only background", object && object->style && after && after->style &&
          object->style->bg_color == after->style->bg_color);
    struct web_hit hit = {0};
    check("fallback link retains native hit activation", lw > 0 && lh > 0 && web_hit_test(d, lx + lw / 2, ly + lh / 2, &hit) &&
          hit.kind == WEB_HIT_LINK && hit.node == link && hit.href && strstr(hit.href, "/next"));
    paint(d);
    int ink = 0;
    for (int y = 0; y < 24; y++) for (int x = 0; x < 400; x++) if ((pixels[y * 480 + x] & 0xffffff) != 0xffffff) ink++;
    check("fallback text paints actual pixels", ink > 0);
    check("old replaced placeholder rectangle is absent", (pixels[80 * 480 + 80] & 0xffffff) == 0xffffff);
    web_free(d);
}

static void unsupported_and_author_boxes(void) {
    web_doc *d = load("<!doctype html><body style='margin:0;font-size:16px;line-height:20px'>"
        "<div id=dataline><object id=data data='/unsupported-resource' type='application/x-unimplemented-test'>"
        "<span id=datatext>unsupported fallback</span></object></div>"
        "<div id=nestedline><object id=outer><object id=inner><a id=nested href='/nested'>nestedfallback</a></object></object></div>"
        "<div id=emptyline><object id=empty></object></div>"
        "<object id=block style='display:block;width:90px;height:24px;background:lime'><span id=blockchild>x</span></object>"
        "<object id=inlineblock style='display:inline-block;width:80px;height:26px;background:red'>y</object>");
    if (!d) return;
    node_t *data = id(d, "data"), *datatext = id(d, "datatext"), *dataline = id(d, "dataline"),
           *outer = id(d, "outer"), *inner = id(d, "inner"), *nested = id(d, "nested"),
           *empty = id(d, "empty"), *emptyline = id(d, "emptyline"), *block = id(d, "block"), *inlineblock = id(d, "inlineblock");
    check("unsupported data object is parsed with resource attributes", data && node_attr(data, "data") && node_attr(data, "type"));
    check("unsupported data uses native fallback child", data && data->box && data->box->kind == B_INLINE && datatext && datatext->box &&
          text(data->box, "unsupported fallback"));
    check("unsupported data has no invented intrinsic 300x150 size", dataline && dataline->box && dataline->box->h <= 24);
    check("nested unsupported objects retain both fallback wrappers", outer && inner && outer->box && inner->box &&
          outer->box->kind == B_INLINE && inner->box->kind == B_INLINE && nested && nested->box);
    check("nested fallback text reaches native layout", outer && text(outer->box, "nestedfallback") && web_find(d, "nestedfallback", 0) >= 0);
    check("empty fallback is ordinary empty flow", empty && empty->box && empty->box->kind == B_INLINE && !empty->box->first);
    check("empty fallback does not create a placeholder line", emptyline && emptyline->box && emptyline->box->h == 0);
    check("author block display is respected", block && block->style && block->style->display == D_BLOCK && block->box && block->box->kind == B_BLOCK);
    check("author block dimensions are respected", block && block->box && block->box->w == 90 && block->box->h == 24);
    check("author block keeps fallback children", block && text(block->box, "x") && id(d, "blockchild") && id(d, "blockchild")->box);
    check("author inline block is a real flow container", inlineblock && inlineblock->box && inlineblock->box->kind == B_ATOMIC &&
          inlineblock->box->atomic == AT_INLINE_BLOCK && text(inlineblock->box, "y"));
    check("author inline block dimensions are respected", inlineblock && inlineblock->box && inlineblock->box->w == 80 && inlineblock->box->h == 26);
    paint(d);
    int block_y = block && block->box ? (int)box_abs_y(block->box) : -1;
    check("author block fallback background paints lime", block_y >= 0 && block_y + 5 < 240 &&
          (pixels[(block_y + 5) * 480 + 70] & 0xffffff) == 0x00ff00);
    int bx = 0, by = 0, bw = 0, bh = 0;
    check("author inline block native rectangle", rect(d, inlineblock, &bx, &by, &bw, &bh) && bw == 80 && bh == 26);
    check("author inline block fallback background paints red", by >= 0 && by + 5 < 240 && bx >= 0 && bx + 60 < 480 &&
          (pixels[(by + 5) * 480 + bx + 60] & 0xffffff) == 0xff0000);
    web_free(d);
}

static void existing_replaced_elements(void) {
    web_doc *d = load("<!doctype html><body style='margin:0'><iframe id=frame style='border:0'></iframe>"
        "<embed id=embed><canvas id=canvas width=40 height=20></canvas>");
    if (!d) return;
    node_t *frame = id(d, "frame"), *embed = id(d, "embed"), *canvas = id(d, "canvas");
    check("iframe remains replaced placeholder", frame && frame->box && frame->box->kind == B_ATOMIC && frame->box->atomic == AT_PLACEHOLDER);
    check("iframe retains intrinsic dimensions", frame && frame->box && frame->box->w == 300 && frame->box->h == 150);
    check("embed remains replaced placeholder without object fallback semantics", embed && embed->box && embed->box->kind == B_ATOMIC &&
          embed->box->atomic == AT_PLACEHOLDER);
    check("embed retains intrinsic dimensions", embed && embed->box && embed->box->w == 300 && embed->box->h == 150);
    check("canvas retains its existing replaced path", canvas && canvas->box && canvas->box->kind == B_ATOMIC && canvas->box->atomic == AT_PLACEHOLDER);
    check("canvas retains explicit bitmap dimensions", canvas && canvas->box && canvas->box->w == 40 && canvas->box->h == 20);
    web_free(d);
}

int main(void) {
    inline_fallback(); unsupported_and_author_boxes(); existing_replaced_elements();
    printf("objectfallback: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
