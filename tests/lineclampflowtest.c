#include <stdio.h>
#include <string.h>
#include <math.h>
#include <web.h>
#include "webi.h"

/* New boundary only: anonymous inline groups adjoining list paragraphs and
 * their ordinary/flex/grid parent flow. The 83-check clamp suite is separate. */
static int checks, failures;
static void check(const char *name, bool pass) {
    checks++;
    if (!pass) { failures++; printf("FAIL lineclampflow %s\n", name); }
}
static node_t *id(web_doc *d, const char *value) {
    for (node_t *n = d->owned_nodes; n; n = n->owned_next) {
        const char *name = node_attr(n, "id");
        if (name && !strcmp(name, value) && doc_node_connected(n)) return n;
    }
    return NULL;
}
static box_t *box(web_doc *d, const char *value) {
    node_t *n = id(d, value); return n ? n->box : NULL;
}
static bool near(float value, float expected) { return fabsf(value - expected) < .05f; }
static bool height(box_t *b, float expected) { return b && near(b->h, expected); }
static web_doc *load(const char *parent, const char *constraint) {
    char html[2048];
    snprintf(html, sizeof html,
        "<style>html,body{margin:0;font:16px/20px sans-serif}"
        "#article{width:180px;%s}"
        "#preview{display:-webkit-box;-webkit-box-orient:vertical;-webkit-line-clamp:6;"
        "overflow:hidden;%s}"
        "#preview p{all:revert;display:inline!important;font-size:inherit;font-weight:inherit;"
        "margin:0;padding:0}"
        "#preview ul,#preview li{margin:0;padding:0;list-style:none}"
        "#following{height:10px}</style>"
        "<article id=article><div id=preview><p id=inline>first<br>second<br>third<br>fourth</p>"
        "<ul id=list><li>fifth</li><li id=sixth>sixth</li><li id=seventh>seventh</li></ul>"
        "<p id=tail>tail</p></div><div id=following></div></article>", parent, constraint);
    web_doc *d = web_parse(html, strlen(html), "https://line-clamp-flow.test/", "utf-8");
    check("new mixed-flow document allocated", d != NULL);
    if (d) { d->profile_enabled = true; web_layout(d, 480, 240); }
    return d;
}
static void anonymous_and_list(void) {
    web_doc *d = load("", "");
    if (!d) return;
    box_t *b = box(d, "preview"), *list = box(d, "list"), *following = box(d, "following");
    node_t *inline_node = id(d, "inline");
    struct clamp_trace *t = b ? b->clamp_trace : NULL;
    check("authored inline paragraphs retain one enclosing clamp", b && b->st->legacy_box &&
        b->st->line_clamp == 6 && inline_node && inline_node->style && inline_node->style->display == D_INLINE);
    check("inline group is a real anonymous formatting context", b && b->first &&
        b->first->anon && b->first->inline_ctx && b->first->nlines == 4);
    check("line budget crosses anonymous group into list", b && b->clamp_truncated &&
        box(d, "sixth") && !box(d, "sixth")->clamp_hidden);
    check("mixed clamp content height is six actual lines", height(b, 120));
    check("partially visible list reduces its own flow height", height(list, 40));
    check("third list line is elided", box(d, "seventh") && box(d, "seventh")->clamp_hidden);
    check("following anonymous inline group is elided", box(d, "tail") && box(d, "tail")->clamp_hidden);
    check("last line containing-block chain reaches clamp", t && t->cb_reached && t->lines == 6);
    check("numeric trace retains original full mixed height", t && near(t->full_h, 160));
    check("numeric trace separates cut from height constraints", t && near(t->cut_h, 120) &&
        near(t->post_h, 120) && t->sh < 0 && t->usedh < 0);
    check("ordinary next sibling consumes only clamped height", following && near(following->y, 120));
    check("ordinary parent flow height reflects reduced child", height(box(d, "article"), 130));
    printf("lineclampflow mixed full=%.2f cut=%.2f post=%.2f parent=%.2f\n",
        t ? t->full_h : -1, t ? t->cut_h : -1, t ? t->post_h : -1,
        box(d, "article") ? box(d, "article")->h : -1);
    doc_node_attr(d, id(d, "preview"), "style", "-webkit-line-clamp:none");
    web_layout(d, 480, 240);
    b = box(d, "preview"); following = box(d, "following");
    check("none restores mixed inline-list flow height", height(b, 160));
    check("none restores list and following inline fragments", box(d, "seventh") &&
        !box(d, "seventh")->clamp_hidden && box(d, "tail") && !box(d, "tail")->clamp_hidden);
    check("none restores ordinary next sibling placement", following && near(following->y, 160));
    check("inactive clamp has no diagnostic arena allocation", b && !b->clamp_trace);
    web_free(d);
}
static void parent_flow(const char *parent, const char *name) {
    web_doc *d = load(parent, "");
    if (!d) return;
    box_t *b = box(d, "preview"), *following = box(d, "following");
    const struct clamp_trace *t = b ? b->clamp_trace : NULL;
    check(name, height(b, 120));
    check("parent ordered item placement uses reduced height", following && near(following->y, 120));
    check("parent auto height uses reduced mixed-flow item", height(box(d, "article"), 130));
    check("parent flow does not synthesize a definite full-content height", t &&
        near(t->full_h, 160) && near(t->cut_h, 120) && near(t->post_h, 120) && t->sh < 0);
    web_free(d);
}
static void constraint(const char *value, bool definite) {
    web_doc *d = load("", value);
    if (!d) return;
    box_t *b = box(d, "preview"), *following = box(d, "following");
    const struct clamp_trace *t = b ? b->clamp_trace : NULL;
    check("authored height constraint is not erased by clamp", height(b, 200));
    check("trace distinguishes clamp cut and author constraint", t && near(t->cut_h, 120) &&
        near(t->post_h, 200) && (definite ? near(t->sh, 200) : t->sh < 0));
    check("next sibling respects real authored constraint", following && near(following->y, 200));
    web_free(d);
}
int main(void) {
    anonymous_and_list();
    parent_flow("display:flex;flex-direction:column", "column flex mixed clamp height");
    parent_flow("display:grid;grid-template-columns:180px", "grid mixed clamp height");
    constraint("min-height:200px", false);
    constraint("height:200px", true);
    printf("lineclampflow: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
