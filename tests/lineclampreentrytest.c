#include <stdio.h>
#include <string.h>
#include <math.h>
#include <web.h>
#include "webi.h"

/* New boundary only: one layout_doc measures and then re-enters a flex/grid
 * item. Old successful clamp groups are not run here. min-height:1px makes
 * the item a definite-height consumer even when the stretch size is equal. */
static int checks, failures;
static void check(const char *name, bool pass) {
    checks++;
    if (!pass) { failures++; printf("FAIL lineclampreentry %s\n", name); }
}
static bool near(float a, float b) { return fabsf(a - b) < .05f; }
static node_t *id(web_doc *d, const char *name) {
    for (node_t *n = d->owned_nodes; n; n = n->owned_next) {
        const char *v = node_attr(n, "id");
        if (v && !strcmp(v, name) && doc_node_connected(n)) return n;
    }
    return NULL;
}
static box_t *box(web_doc *d, const char *name) { node_t *n = id(d, name); return n ? n->box : NULL; }
static bool ellipsis(box_t *b) {
    if (!b || b->clamp_hidden) return false;
    for (int i = 0; i < b->nruns; i++)
        if (b->runs[i].s && b->runs[i].n == 3 && !memcmp(b->runs[i].s, "\xe2\x80\xa6", 3)) return true;
    for (box_t *c = b->first; c; c = c->next) if (ellipsis(c)) return true;
    return false;
}
static web_doc *load(const char *outer, const char *constraint, bool overflow) {
    char html[2304];
    snprintf(html, sizeof html,
        "<style>html,body{margin:0;font:16px/20px sans-serif}"
        "#outer{width:180px;%s}#wrap{width:180px;min-height:1px}"
        "#preview{display:-webkit-box;-webkit-box-orient:vertical;-webkit-line-clamp:6;"
        "overflow:hidden;%s}"
        "#preview p{all:revert;display:inline!important;font-size:inherit;font-weight:inherit;"
        "margin:0;padding:0}#preview ul,#preview li{margin:0;padding:0;list-style:none}"
        "#following{height:10px}#plain{display:flow-root;width:180px}</style>"
        "<div id=outer><article id=wrap><div id=preview><p>first<br>second<br>third<br>fourth</p>"
        "<ul id=list><li>fifth</li><li id=sixth>sixth</li>%s</ul>%s</div>"
        "<div id=following></div></article></div><div id=plain>ordinary<br>cached</div>",
        outer, constraint, overflow ? "<li id=seventh>elided-seventh</li>" : "",
        overflow ? "<p id=tail>elided-tail</p>" : "");
    web_doc *d = web_parse(html, strlen(html), "https://line-clamp-reentry.test/", "utf-8");
    check("new reentry document allocated", d != NULL);
    if (d) { d->profile_enabled = true; web_layout(d, 480, 320); }
    return d;
}
static void reentry(const char *outer, const char *constraint, float final_h, bool overflow) {
    web_doc *d = load(outer, constraint, overflow);
    if (!d) return;
    box_t *b = box(d, "preview"), *wrap = box(d, "wrap"), *list = box(d, "list"),
        *following = box(d, "following");
    const struct clamp_trace *t = b ? b->clamp_trace : NULL;
    check("parent really re-enters its item with a definite stretch input", wrap &&
        wrap->stretch_height_dependent && near(wrap->cached_usedh, final_h + 10));
    check("full line result is rebuilt before final clamp", t && near(t->full_h, overflow ? 160 : 120));
    check("fresh overflow predicate survives the second invocation", t &&
        t->eligible && t->applied == overflow && b->clamp_truncated == overflow);
    check("six visible lines retain their actual containing-block ancestry", t &&
        t->lines == 6 && t->cb_reached && near(t->last_y + t->line_bottom, 120));
    check("cut height is independent from an authored height constraint", t &&
        near(t->cut_h, 120) && near(t->post_h, final_h) && near(b->h, final_h));
    check("final parent flow consumes the new clamp height", following && near(following->y, final_h) &&
        wrap && near(wrap->h, final_h + 10));
    check("partially visible list is reduced on every invocation", list && near(list->h, 40));
    check("ellipsis is re-created only for real overflow", ellipsis(b) == overflow);
    check("later paragraph and list box remain elided after reentry", !overflow ||
        (box(d, "seventh") && box(d, "seventh")->clamp_hidden && box(d, "tail") &&
         box(d, "tail")->clamp_hidden && web_find(d, "elided-tail", 0) < 0));
    printf("lineclampreentry full=%.2f cut=%.2f post=%.2f applied=%u parent_used=%.2f\n",
        t ? t->full_h : -1, t ? t->cut_h : -1, t ? t->post_h : -1, t ? t->applied : 0,
        wrap ? wrap->cached_usedh : -1);
    web_free(d);
}
static void nested(void) {
    const char *html =
        "<style>html,body{margin:0;font:16px/20px sans-serif}"
        "#outer{display:flex;width:180px}#wrap{width:180px;min-height:1px}"
        "#preview,#inner{display:-webkit-box;-webkit-box-orient:vertical;overflow:hidden}"
        "#preview{-webkit-line-clamp:3}#inner{-webkit-line-clamp:2}"
        "p{margin:0;padding:0}#following{height:10px}</style>"
        "<div id=outer><div id=wrap><div id=preview>first"
        "<div id=inner>nested-first<br>nested-second<br>nested-elided</div>"
        "<p id=last>second<br>third<br>outer-elided</p><p id=tail>after-elided</p>"
        "</div><div id=following></div></div></div>";
    web_doc *d = web_parse(html, strlen(html), "https://line-clamp-reentry.test/nested", "utf-8");
    check("nested reentry document allocated", d != NULL);
    if (!d) return;
    d->profile_enabled = true; web_layout(d, 480, 320);
    box_t *b = box(d, "preview"), *inner = box(d, "inner"), *last = box(d, "last"), *wrap = box(d, "wrap");
    const struct clamp_trace *t = b ? b->clamp_trace : NULL, *i = inner ? inner->clamp_trace : NULL;
    check("nested wrapper genuinely reaches the second layout", wrap && near(wrap->cached_usedh, 110));
    check("outer ordered budget remains independent from the nested BFC", t && t->applied &&
        t->lines == 3 && near(t->full_h, 140) && near(t->cut_h, 100) && near(b->h, 100));
    check("nested clamp rebuilds its own private budget after visibility reset", i && i->applied &&
        i->lines == 2 && near(i->full_h, 60) && near(inner->h, 40) && inner->nlines == 2);
    check("both clamp contexts retain their own ellipsis", ellipsis(inner) && ellipsis(last));
    check("nested reduced geometry is not restored without its visibility", inner &&
        !inner->clamp_hidden && inner->clamp_truncated && !inner->layout_cache_valid);
    check("outer line fragment ancestry crosses the nested BFC in normal flow", t &&
        t->cb_reached && near(t->last_y, 60) && near(t->line_bottom, 40));
    check("outer tail stays hidden after the parent's final placement", box(d, "tail") &&
        box(d, "tail")->clamp_hidden && web_find(d, "after-elided", 0) < 0);
    check("following sibling sees both independently reduced clamp contexts", box(d, "following") &&
        near(box(d, "following")->y, 100) && wrap && near(wrap->h, 110));
    web_free(d);
}
static void independent_jobs(void) {
    char html[4096];
    size_t used = (size_t)snprintf(html, sizeof html,
        "<style>html,body{margin:0;font:16px/20px sans-serif}"
        "#outer{display:flex;width:1440px}.wrap{width:180px;min-height:1px;flex:none}"
        ".preview{display:-webkit-box;-webkit-box-orient:vertical;-webkit-line-clamp:2;"
        "overflow:hidden}p{margin:0;padding:0}</style><div id=outer>");
    for (int i = 0; i < 8; i++) used += (size_t)snprintf(html + used, sizeof html - used,
        "<div id=w%d class=wrap><div id=b%d class=preview><p>first</p><p>second</p>"
        "<p id=t%d>third</p></div></div>", i, i, i);
    snprintf(html + used, sizeof html - used, "</div>");
    web_doc *d = web_parse(html, strlen(html), "https://line-clamp-reentry.test/jobs", "utf-8");
    check("independent sibling jobs document allocated", d != NULL);
    if (!d) return;
    d->profile_enabled = true; web_layout(d, 1600, 320);
    unsigned applied = 0, hidden = 0, ancestry = 0, rebuilt = 0;
    for (int j = 0; j < 8; j++) {
        char name[16]; snprintf(name, sizeof name, "b%d", j); box_t *b = box(d, name);
        const struct clamp_trace *t = b ? b->clamp_trace : NULL;
        snprintf(name, sizeof name, "w%d", j); box_t *w = box(d, name);
        snprintf(name, sizeof name, "t%d", j); box_t *tail = box(d, name);
        if (t && t->applied && near(b->h, 40) && ellipsis(b)) applied |= 1u << j;
        if (tail && tail->clamp_hidden) hidden |= 1u << j;
        if (t && t->cb_reached && t->lines == 2) ancestry |= 1u << j;
        if (t && near(t->full_h, 60) && w && near(w->cached_usedh, 40)) rebuilt |= 1u << j;
    }
    check("every private job result survives final supervisor reentry", applied == 255);
    check("every job independently reapplies tail visibility", hidden == 255);
    check("every job keeps native line ancestry and its own budget", ancestry == 255);
    check("eight actual measurement-to-final transitions rebuild full results", rebuilt == 255);
    web_free(d);
}
static void unrelated_cache(void) {
    web_doc *d = load("display:flex", "", true);
    if (!d) return;
    box_t *b = box(d, "preview"), *plain = box(d, "plain");
    struct run *old_runs = plain ? plain->runs : NULL;
    check("unrelated ordinary BFC has a reusable line result", plain && old_runs && plain->layout_cache_valid);
    /* This is a new changed-input boundary, not a repeat of a successful
       fixture: descendants are freshly laid out while a sibling stays cached. */
    doc_node_attr(d, id(d, "preview"), "style", "-webkit-line-clamp:5"); web_layout(d, 480, 320);
    b = box(d, "preview"); plain = box(d, "plain");
    check("changed budget reduces reentrant real fragments", b && b->clamp_truncated && near(b->h, 100));
    check("new budget hides sixth and later list paragraphs", box(d, "sixth") &&
        box(d, "sixth")->clamp_hidden && box(d, "seventh") && box(d, "seventh")->clamp_hidden);
    check("nonclamp sibling keeps the exact cached run allocation", plain && plain->runs == old_runs &&
        plain->layout_cache_valid && near(plain->h, 40));
    check("local visibility reset never elides the unrelated sibling", plain && !plain->clamp_hidden &&
        web_find(d, "ordinary", 0) >= 0);
    web_free(d);
}
int main(void) {
    reentry("display:flex", "", 120, true);
    reentry("display:grid;grid-template-columns:180px", "", 120, true);
    reentry("display:flex", "height:200px", 200, true);
    reentry("display:flex", "", 120, false);
    nested(); independent_jobs(); unrelated_cache();
    printf("lineclampreentry: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
