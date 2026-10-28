/* The cascade's side of CSSImportRule.styleSheet on the host: doc.c's resource
   scan with a style element whose sheet has an @import rule, the imported
   resource in doc.c's stylesheet cache. CSSOM "fetch an @import" (CSS Cascade 5)
   gives the rule a sheet of its own; its changes and its disabled flag must
   reach the cascade, which reads that sheet's one parse (the sheet the cascade
   instances is the CSSOM sheet's AST, not a second parse of the resource).
   tests/check_cssom_rules_native.py builds this with css.c, cssom.c and the DOM.
   Built with OLD_SOURCE against the source before CSSImportRule.styleSheet,
   the cases that need the imported sheet fail.

   What stands in for the product here: the URL parser (web_url.c: absolute URLs
   as they are), origins (http.c: the text up to the path) and the effective URL
   (frame.c: the document's URL), and js.c's checkpoint in long native work (no
   script to interrupt: go on). */
#include "doc.c"

static int passed, failed;
static void check(bool ok, const char *name) {
    if (ok) passed++;
    else { failed++; printf("FAIL %s\n", name); }
}
int web_resolve_url_owned(const char *base, const char *rel, char **out) {
    (void)base;
    *out = strstr(rel, "://") ? strdup(rel) : NULL;
    return *out ? 1 : 0;
}
/* web_url.c's place (the source before 3435c5b4 asked http.c) */
#ifdef OLD_SOURCE
enum http_url_result http_origin_owned(const char *raw, char **origin) {
#define WEB_URL_OPAQUE HTTP_URL_OPAQUE
#define WEB_URL_TUPLE HTTP_URL_TUPLE
#define WEB_URL_OOM HTTP_URL_OOM
#else
enum web_url_result web_url_origin(const char *raw, char **origin) {
#endif
    *origin = NULL;
    const char *host = raw ? strstr(raw, "://") : NULL;
    if (!host) return WEB_URL_OPAQUE;
    const char *path = strchr(host + 3, '/');
    *origin = strndup(raw, path ? (size_t)(path - raw) : strlen(raw));
    return *origin ? WEB_URL_TUPLE : WEB_URL_OOM;
}
const char *web_effective_url(web_doc *d) { return d->url; }
bool web_native_checkpoint(web_doc *d) { return d && !d->native_cancelled; }

static web_doc document_;
static node_t nodes[3];
static const char sheet_text[] = "@import url(\"https://parent.test/a.css\"); .own { color: green }";
static struct cached_css resource = {.url = "https://parent.test/a.css", .base = "https://parent.test/a.css",
                                     .body = ".child { color: red }", .done = true};

/* the sheets one resource scan gives the cascade, in cascade order */
static int restyle(void) {
    document_.sty.sheets.n = 0;
    scan(&document_, document_.root, NULL);
    return document_.sty.sheets.n;
}
static sheet_t *cascaded(int i) { return document_.sty.sheets.v[i]; }
static uint32_t rules_of(sheet_t *s) { uint32_t n; css_sheet_rules(s, &n); return n; }

int main(void) {
    nodes[0].type = N_DOC;
    nodes[1].type = N_ELEM; nodes[1].tag = T_style; nodes[1].parent = &nodes[0];
    nodes[0].first = nodes[0].last = &nodes[1];
    nodes[2].type = N_TEXT; nodes[2].text = (char *)sheet_text; nodes[2].textlen = strlen(sheet_text);
    nodes[2].parent = &nodes[1]; nodes[1].first = nodes[1].last = &nodes[2];
    for (int i = 0; i < 3; i++) nodes[i].owner = &document_;
    document_.root = &nodes[0]; document_.live = true; document_.dom_revision = 1;
    document_.url = document_.base = "https://parent.test/page";
    resource.n = strlen(resource.body);
    pv_push(&document_.css_cache, &resource);

    /* without CSSOM the resource's cached parse is the imported sheet (a parse for
       the cascade alone, which keeps no CSSOM rule list) */
    check(restyle() == 2, "a style sheet and the sheet its @import loads cascade");
    int error;
    struct cssom_sheet *sheet = cssom_style_sheet(&document_, &nodes[1], &error);
    check(sheet != NULL, "the style element has a CSSOM sheet");
    uint32_t count;
    struct css_rule_info *import = sheet ? cssom_child(sheet, 0, 0, &count) : NULL;
    check(import && import->type == CSS_RULE_IMPORT, "its first rule is the @import rule");
#ifdef OLD_SOURCE
    (void)import;
    check(false, "CSSImportRule.styleSheet: the old source gives the @import rule no sheet");
    check(false, "a change to the imported sheet reaches the cascade");
    check(false, "a disabled imported sheet leaves the cascade");
#else
    struct cssom_sheet *child = import ? cssom_import_sheet(sheet, import, &error) : NULL;
    check(child && child->parent == sheet && child->owner_rule == import->id,
          "CSSImportRule.styleSheet: the @import rule's sheet, its parent the rule's sheet");
    check(child && restyle() == 2 && css_sheet_rules(cascaded(0), NULL) == css_sheet_rules(child->p.ast, NULL),
          "the cascade instances the imported sheet's own parse, the one CSSOM reads");
    if (child) check(!cssom_insert(child, 0, ".added { color: blue }", 22, 1), "insertRule into the imported sheet");
    check(child && restyle() == 2 && rules_of(cascaded(0)) == 2 &&
          css_sheet_rules(cascaded(0), NULL) == css_sheet_rules(child->p.ast, NULL),
          "a change to the imported sheet reaches the cascade");
    if (child) child->disabled = true;
    check(restyle() == 1 && rules_of(cascaded(0)) == 2, "a disabled imported sheet leaves the cascade, its parent stays");
    if (child) child->disabled = false;
#endif
    if (sheet) check(!cssom_delete(sheet, 0, 0), "deleteRule the @import rule");
    check(restyle() == 1 && rules_of(cascaded(0)) == 1, "a removed @import rule's sheet reaches no cascade");
    printf("cssom import cascade: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
