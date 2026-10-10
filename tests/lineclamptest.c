/* Actual native legacy line-clamp: real line fragments, font-sized ellipsis,
   cross-paragraph budgets, hidden hit/raster and reversible CSS mutations. */
#include "nocturne.h"
#include "webi.h"
#include <stdio.h>
#include <math.h>

static int checks, failures, errors;
static uint32_t pixels[480 * 240];
static void check(const char *name, bool pass) {
    checks++;
    if (!pass) { failures++; printf("FAIL lineclamp %s\n", name); }
}
static node_t *id(web_doc *d, const char *value) {
    for (node_t *n = d->owned_nodes; n; n = n->owned_next) {
        const char *name = node_attr(n, "id");
        if (name && !strcmp(name, value) && doc_node_connected(n)) return n;
    }
    return NULL;
}
static web_doc *load(const char *html) {
    web_doc *d = web_parse(html, strlen(html), "https://line-clamp.test/", "utf-8");
    check("native document allocated", d != NULL);
    if (d) web_layout(d, 480, 240);
    return d;
}
static box_t *box(web_doc *d, const char *value) { node_t *n = id(d, value); return n ? n->box : NULL; }
static bool height(box_t *b, float value) { return b && fabsf(b->h - value) < 0.05f; }
static bool ellipsis(box_t *b) {
    if (!b) return false;
    for (int i = 0; i < b->nruns; i++)
        if (!b->runs[i].atomic && b->runs[i].n == 3 && !memcmp(b->runs[i].s, "\xe2\x80\xa6", 3)) return true;
    return false;
}
static bool hit(web_doc *d, node_t *n, int x, int y) {
    struct web_hit h = {0};
    return n && web_hit_test(d, x, y, &h) && h.kind == WEB_HIT_LINK && h.node == n;
}
static void paint(web_doc *d) {
    canvas_t c; gfx_init(&c, pixels, 480, 240, 480);
    web_paint(d, &c, 0, 0, 480, 240, 0, 0);
}
static bool supports(const char *property, const char *value) {
    return css_supports_declaration(property, strlen(property), value, strlen(value));
}
static void parsing(void) {
    check("supports positive integer", supports("-webkit-line-clamp", "3"));
    check("supports positive signed integer", supports("-webkit-line-clamp", "+2"));
    check("supports none", supports("-webkit-line-clamp", "none"));
    check("zero invalid", !supports("-webkit-line-clamp", "0"));
    check("negative invalid", !supports("-webkit-line-clamp", "-2"));
    check("fraction invalid", !supports("-webkit-line-clamp", "1.5"));
    check("integer unit invalid", !supports("-webkit-line-clamp", "2px"));
    check("multiple integers invalid", !supports("-webkit-line-clamp", "2 3"));
    check("unknown clamp invalid", !supports("-webkit-line-clamp", "wrong"));
    check("standard shorthand not falsely exposed", !supports("line-clamp", "3"));
    check("vertical orientation exposed", supports("-webkit-box-orient", "vertical"));
    check("horizontal orientation exposed", supports("-webkit-box-orient", "horizontal"));
    check("unknown orientation invalid", !supports("-webkit-box-orient", "wrong"));
    check("legacy block display exposed", supports("display", "-webkit-box"));
    check("legacy inline display exposed", supports("display", "-webkit-inline-box"));
}
static void basic_and_mutations(void) {
    web_doc *d = load("<!doctype html><style>html,body{margin:0}body{font-size:16px;line-height:20px}"
        ".sample{display:-webkit-box;-webkit-box-orient:vertical;width:180px}"
        "#two{-webkit-line-clamp:2}</style><div id=two class=sample>alpha<br>beta<br>"
        "<a id=tail href=/tail style='color:red;position:relative'>HIDDENLAST</a></div>"
        "<div id=following>following</div>");
    if (!d) return;
    node_t *n = id(d, "two"), *tail = id(d, "tail"); box_t *b = box(d, "two");
    check("legacy metadata is retained", n && n->style && n->style->legacy_box && n->style->line_clamp == 2 &&
        n->style->box_orient == BO_VERTICAL);
    check("vertical clamp creates BFC computed display", n && n->style && n->style->display == D_FLOW_ROOT);
    check("two actual lines set native height", height(b, 40) && b->nlines == 2 && b->clamp_truncated);
    check("final visible line contains real ellipsis run", ellipsis(b));
    check("following ordinary block uses reduced flow height", box(d, "following") && box(d, "following")->y == 40);
    check("hidden inline layer is suppressed", tail && tail->box && tail->box->clamp_hidden);
    check("hidden hyperlink cannot activate", !hit(d, tail, 10, 45));
    check("clipped content does not inflate scroll height", b && b->scroll_h == 40);
    check("clamp subtree cannot reuse pre-clamp cache", b && !b->layout_cache_safe && !b->layout_cache_valid);
    struct run *r = b && b->nruns ? &b->runs[b->nruns - 1] : NULL;
    wfont f = style_font(n->style);
    check("ellipsis uses native font advance", r && fabsf(r->w - wf_width(&f, "\xe2\x80\xa6", 3)) < 0.05f);
    check("ellipsis ends inside actual line width", r && r->x + r->w <= b->w + 0.05f);
    paint(d);
    unsigned red = 0, ink = 0;
    for (int y = 0; y < 80; y++) for (int x = 0; x < 180; x++) {
        uint32_t p = pixels[y * 480 + x] & 0xffffff;
        if (p == 0xff0000) red++;
        if (y < 40 && p != 0xffffff) ink++;
    }
    check("visible native lines and ellipsis rasterize", ink > 0);
    unsigned ellipsis_ink = 0;
    if (r) for (int y = 20; y < 40; y++)
        for (int x = (int)ceilf(r->x); x < (int)floorf(r->x + r->w) && x < 180; x++)
            if (x >= 0 && (pixels[y * 480 + x] & 0xffffff) != 0xffffff) ellipsis_ink++;
    check("ellipsis glyph itself paints native pixels", ellipsis_ink > 0);
    check("hidden red tail never rasterizes", red == 0);
    doc_node_attr(d, n, "style", "-webkit-line-clamp:none"); web_layout(d, 480, 240);
    b = box(d, "two"); tail = id(d, "tail");
    check("none restores full ordinary height", height(b, 60) && !b->clamp_truncated && !ellipsis(b));
    check("none clears hidden inline state", tail && tail->box && !tail->box->clamp_hidden);
    check("restored hyperlink activates", hit(d, tail, 10, 45));
    doc_node_attr(d, n, "style", "-webkit-line-clamp:1"); web_layout(d, 480, 240);
    check("new integer reclamps actual lines", height(box(d, "two"), 20) && ellipsis(box(d, "two")));
    doc_node_attr(d, n, "style", "-webkit-line-clamp:2;-webkit-box-orient:horizontal"); web_layout(d, 480, 240);
    check("horizontal orientation disables clamp", height(box(d, "two"), 60) && !box(d, "two")->clamp_truncated);
    doc_node_attr(d, n, "style", "display:block;-webkit-line-clamp:2"); web_layout(d, 480, 240);
    check("ordinary display clears authored legacy flag", n->style && !n->style->legacy_box && height(box(d, "two"), 60));
    doc_node_attr(d, n, "style", "display:-webkit-box;-webkit-line-clamp:2;font-size:24px;line-height:30px;width:220px");
    web_layout(d, 480, 240); b = box(d, "two");
    check("font and line height mutation recomputes clamp", height(b, 60) && b->nlines == 2);
    r = b && b->nruns ? &b->runs[b->nruns - 1] : NULL; f = style_font(n->style);
    check("new font sizes ellipsis without stale run cache", r && fabsf(r->w - wf_width(&f, "\xe2\x80\xa6", 3)) < 0.05f);
    web_free(d);
}
static void negative_cases(void) {
    web_doc *d = load("<!doctype html><style>html,body{margin:0}body{font-size:16px;line-height:20px}"
        ".s{display:-webkit-box;-webkit-box-orient:vertical;width:180px}</style>"
        "<div id=exact class=s style='-webkit-line-clamp:2'>one<br>two</div>"
        "<div id=short class=s style='-webkit-line-clamp:3'>one</div>"
        "<div id=zero class=s style='-webkit-line-clamp:0'>one<br>two<br>three</div>"
        "<div id=invalid class=s style='-webkit-line-clamp:2;-webkit-line-clamp:0;-webkit-line-clamp:-1;"
        "-webkit-line-clamp:1.5;-webkit-line-clamp:wrong'>one<br>two<br>three</div>"
        "<div id=none class=s style='-webkit-line-clamp:2;-webkit-line-clamp:none'>one<br>two<br>three</div>"
        "<div id=horizontal class=s style='-webkit-line-clamp:2;-webkit-box-orient:horizontal'>one<br>two<br>three</div>"
        "<div id=ordinary class=s style='display:block;-webkit-line-clamp:2'>one<br>two<br>three</div>"
        "<div id=inline class=s style='display:-webkit-inline-box;-webkit-line-clamp:2'>one<br>two<br>three</div>");
    if (!d) return;
    check("exact limit does not add false ellipsis", height(box(d, "exact"), 40) && !ellipsis(box(d, "exact")));
    check("short content does not add false ellipsis", height(box(d, "short"), 20) && !ellipsis(box(d, "short")));
    check("zero declaration leaves initial none", id(d, "zero")->style->line_clamp == 0 && height(box(d, "zero"), 60));
    check("invalid declarations preserve valid earlier value", id(d, "invalid")->style->line_clamp == 2 && height(box(d, "invalid"), 40));
    check("none explicitly overrides a valid integer", id(d, "none")->style->line_clamp == 0 && height(box(d, "none"), 60));
    check("horizontal layout is not clamped", height(box(d, "horizontal"), 60));
    check("regular block layout is not clamped", height(box(d, "ordinary"), 60));
    check("inline legacy clamp keeps a real inline block", box(d, "inline") && box(d, "inline")->kind == B_ATOMIC &&
        box(d, "inline")->atomic == AT_INLINE_BLOCK && height(box(d, "inline"), 40) && ellipsis(box(d, "inline")));
    web_free(d);
}
static void paragraphs_and_unicode(void) {
    web_doc *d = load("<!doctype html><style>html,body{margin:0}body{font-size:16px;line-height:20px}p{margin:0}"
        ".s{display:-webkit-box;-webkit-box-orient:vertical;width:180px}"
        "#paragraphs{-webkit-line-clamp:3}#second{margin-top:7px}#raised{-webkit-line-clamp:1}"
        "#unicode{-webkit-line-clamp:2;width:52px}</style>"
        "<div id=paragraphs class=s><p id=first>one<br>two</p><p id=second>"
        "<a id=visible href=/visible>third</a><br><a id=hidden href=/hidden>fourth</a></p><p id=last>fifth</p></div>"
        "<div id=raised class=s>base<span style='vertical-align:super'>raised</span><br>hidden</div>"
        "<div id=unicode class=s>あいうえおかきくけこさしすせそたちつてと</div>");
    if (!d) return;
    box_t *b = box(d, "paragraphs"), *first = box(d, "first"), *second = box(d, "second");
    check("paragraphs share a single three-line budget", b && height(b, 67) && first && first->nlines == 2 && second && second->nlines == 1);
    check("paragraph margins retain real flow geometry", second && second->y == 47);
    check("earlier paragraph has no ellipsis", first && !ellipsis(first));
    check("last visible paragraph owns final ellipsis", second && ellipsis(second) && height(second, 20));
    check("later paragraph and descendant layer are hidden", box(d, "last") && box(d, "last")->clamp_hidden &&
        box(d, "hidden") && box(d, "hidden")->clamp_hidden);
    check("surviving native link remains hit testable", hit(d, id(d, "visible"), 10, 52));
    check("elided paragraph link cannot activate", !hit(d, id(d, "hidden"), 10, 72));
    check("clamp metadata is not inherited by normal children", id(d, "visible")->style->line_clamp == 0 &&
        !id(d, "visible")->style->legacy_box);
    check("vertical-align does not consume another line", box(d, "raised") && box(d, "raised")->nlines == 1 && ellipsis(box(d, "raised")));
    box_t *u = box(d, "unicode"); bool utf8 = u && u->nlines == 2 && ellipsis(u), fits = u != NULL;
    if (u) for (int i = 0; i < u->nruns; i++) {
        struct run *r = &u->runs[i];
        if (r->atomic) continue;
        int bytes = 0;
        while (bytes < r->n) { uint32_t cp; int n = gfx_utf8_decode(r->s + bytes, &cp); if (n <= 0 || n > r->n - bytes) { utf8 = false; break; } bytes += n; }
        if (r->x + r->w > u->w + 0.05f) fits = false;
    }
    check("unicode truncation retains complete UTF8 codepoints", utf8);
    check("unicode and ellipsis use available native font width", fits);
    check("hidden later paragraph cannot produce a find scroll target", web_find(d, "fifth", 0) == -1);
    box_t *last = box(d, "last"); bool highlighted = false;
    if (last) for (int i = 0; i < last->nruns; i++) highlighted |= last->runs[i].highlight;
    check("hidden later paragraph receives no search highlight", last && !highlighted);
    doc_node_attr(d, id(d, "paragraphs"), "style", "-webkit-line-clamp:none"); web_layout(d, 480, 240);
    check("none restores the same paragraph as a native find target", web_find(d, "fifth", 0) >= 0 &&
        box(d, "last") && !box(d, "last")->clamp_hidden);
    web_free(d);
}
static void nested_budgets(void) {
    web_doc *d = load("<!doctype html><style>html,body{margin:0}body{font-size:16px;line-height:20px}p{margin:0}"
        ".s{display:-webkit-box;-webkit-box-orient:vertical;width:180px}#outer{-webkit-line-clamp:2}#inner{-webkit-line-clamp:1}</style>"
        "<div id=outer class=s><p id=a>outer first</p><div id=inner class=s>inner first<br>inner second<br>inner third</div>"
        "<p id=b>outer second</p><p id=c>outer third</p></div>");
    if (!d) return;
    check("nested BFC owns its independent line budget", height(box(d, "inner"), 20) && box(d, "inner")->nlines == 1 && ellipsis(box(d, "inner")));
    check("outer BFC does not double count inner line boxes", height(box(d, "outer"), 60) && !box(d, "b")->clamp_hidden && ellipsis(box(d, "b")));
    check("outer content after clamp is hidden", box(d, "c")->clamp_hidden);
    doc_node_attr(d, id(d, "inner"), "style", "-webkit-line-clamp:none"); web_layout(d, 480, 240);
    check("nested none restores its own fragments", height(box(d, "inner"), 60) && !ellipsis(box(d, "inner")));
    check("outer independent budget survives nested reflow", height(box(d, "outer"), 100) && box(d, "c")->clamp_hidden && ellipsis(box(d, "b")));
    web_free(d);
}
static void console(void *opaque, int level, const char *message) {
    if (!strncmp(message, "OK clamp ", 9)) { checks++; puts(message); }
    else if (!strncmp(message, "FAIL clamp ", 11)) { checks++; failures++; puts(message); }
    else if (level >= 2) { errors++; printf("ERROR lineclamp %s\n", message); }
}
static void cssom_selection(void) {
    struct web_host host = {.console = console, .js_task_budget_ms = 5000};
    const char *html = "<!doctype html><body style='margin:0;font-size:16px;line-height:20px'><div id=sample "
        "style='display:-webkit-box;-webkit-box-orient:vertical;-webkit-line-clamp:2;width:180px'>one<br>two<br>HIDDENTAIL</div></body>";
    web_doc *d = web_live(html, strlen(html), "https://line-clamp.test/", "utf-8", &host);
    check("CSSOM live document allocated", d != NULL); if (!d) return;
    for (int i = 0; i < 8; i++) { web_tick(d, uptime_ms()); msleep(1); }
    web_layout(d, 480, 240);
    const char *script = "const a=(n,v)=>console.log((v?'OK clamp ':'FAIL clamp ')+n),e=document.getElementById('sample'),s=getComputedStyle(e);"
        "a('CSS-supports-positive',CSS.supports('-webkit-line-clamp','2'));"
        "a('CSS-supports-zero-invalid',!CSS.supports('-webkit-line-clamp','0'));"
        "a('computed-integer',s.getPropertyValue('-webkit-line-clamp')==='2');"
        "a('computed-orientation',s.getPropertyValue('-webkit-box-orient')==='vertical');"
        "a('computed-active-display',s.display==='flow-root');"
        "a('DOM-content-preserved',e.textContent.includes('HIDDENTAIL'));"
        "const range=document.createRange();range.selectNodeContents(e);const selection=getSelection();"
        "selection.removeAllRanges();selection.addRange(range);a('selection-retains-elided-DOM',selection.toString().includes('HIDDENTAIL'));";
    check("CSSOM and selection assertions evaluate", web_console_eval(d, script, strlen(script)));
    paint(d);
    unsigned elided_ink = 0;
    for (int y = 40; y < 80; y++) for (int x = 0; x < 180; x++)
        if ((pixels[y * 480 + x] & 0xffffff) != 0xffffff) elided_ink++;
    check("selection paint cannot resurrect elided fragments", elided_ink == 0);
    const char *mutate = "e.style.setProperty('-webkit-line-clamp','none');const n=getComputedStyle(e);"
        "a('computed-none',n.getPropertyValue('-webkit-line-clamp')==='none');"
        "a('computed-inactive-legacy-display',n.display==='-webkit-box');"
        "e.style.display='block';a('computed-ordinary-display',getComputedStyle(e).display==='block');";
    check("CSSOM display and none mutations evaluate", web_console_eval(d, mutate, strlen(mutate)));
    check("no CSSOM native exception", errors == 0);
    web_free(d);
}
int main(void) {
    parsing(); basic_and_mutations(); negative_cases(); paragraphs_and_unicode(); nested_budgets(); cssom_selection();
    printf("lineclamp: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
