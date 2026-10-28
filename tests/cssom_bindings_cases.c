/* CSSOM's script side on the host: the product's js_stylesheets.js in the
   product's QuickJS, over the product's bridge (js_cssom.h) and rule tree
   (cssom.c, css.c's parse), with a small document of style elements in place
   of dom.c's tree building and doc.c's resources. tests/cssom_bindings_cases.js
   holds the cases; each names the WPT page or specification section it reads.
   tests/check_cssom_rules_native.py builds this (js.c's style_text_access and
   style_text_block are copied out of js.c into bindings-extract.h).

   What stands in for the product here: the resources an @import rule loads
   (doc.c's cache and data: decoding: a table), origins (http.c: the text up to
   the path), the URL parser (web_url.c: absolute URLs as they are, others
   against the base's directory; a URL with "test:test" is not a URL, as the URL
   parser finds the port "test"), and a rule's CSSStyleDeclaration (js_bootstrap.js:
   a few methods over the same backend). */
#include "cssom.c"
#include <stdio.h>
#include "quickjs.h"
#include "bindings-extract.h"

struct web_js_state { JSContext *ctx; web_doc *doc; bool disabled; };
static JSValue oom(JSContext *ctx) { return JS_ThrowOutOfMemory(ctx); }
static int checks, failures;
static void check(bool ok, const char *name) {
    checks++;
    if (!ok) { failures++; printf("FAIL %s\n", name); }
}

/* ---- the document */
enum { NODES = 8 };
static node_t nodes[NODES];
static struct attr attributes[NODES][2];
static web_doc document_;
static struct web_js_state state_;
static JSClassID node_class;
static JSValue wrappers[NODES];
bool web_js_enabled(web_doc *d) { return d && d->live; }
const char *web_effective_url(web_doc *d) { return d->url; }
void doc_sync_tree(web_doc *d) { d->resources_dirty = false; }
static void flush_layout(struct web_js_state *s) { (void)s; }
static void attr(int id, const char *name, const char *value) {
    attributes[id][0] = (struct attr){.name = name, .raw = name, .local = name, .value = value};
    nodes[id].attrs = attributes[id]; nodes[id].nattrs = 1;
}
static void child(int parent, int id) {
    node_t *p = &nodes[parent], *n = &nodes[id];
    n->parent = p; n->prev = p->last;
    if (p->last) p->last->next = n; else p->first = n;
    p->last = n;
}
static int node_index(const node_t *n) { for (int i = 0; i < NODES; i++) if (n == &nodes[i]) return i; return -1; }
static JSValue wrap(struct web_js_state *s, node_t *n) {
    int i = n ? node_index(n) : -1;
    if (i < 0) return JS_NULL;
    if (!JS_IsUndefined(wrappers[i])) return JS_DupValue(s->ctx, wrappers[i]);
    const char *name = n->type == N_DOC ? "Document" : "HTMLStyleElement";
    JSValue global = JS_GetGlobalObject(s->ctx), ctor = JS_GetPropertyStr(s->ctx, global, name),
            proto = JS_GetPropertyStr(s->ctx, ctor, "prototype"), object = JS_NewObjectProtoClass(s->ctx, proto, node_class);
    JS_SetOpaque(object, n);
    JS_FreeValue(s->ctx, proto); JS_FreeValue(s->ctx, ctor); JS_FreeValue(s->ctx, global);
    wrappers[i] = JS_DupValue(s->ctx, object);
    return object;
}
static node_t *unwrap(JSContext *ctx, JSValueConst v) { return JS_GetOpaque2(ctx, v, node_class); }
/* the URL parser's place: absolute URLs as they are, others against base's directory */
int web_resolve_url_owned(const char *base, const char *rel, char **out) {
    *out = NULL;
    if (strstr(rel, "test:test")) return 0;
    if (strstr(rel, "://") || !strncmp(rel, "data:", 5)) { *out = strdup(rel); return *out ? 1 : -1; }
    const char *slash = strrchr(base, '/');
    if (!strstr(base, "://") || !slash) return 0;
    size_t a = (size_t)(slash + 1 - base), b = strlen(rel);
    *out = malloc(a + b + 1);
    if (!*out) return -1;
    memcpy(*out, base, a); memcpy(*out + a, rel, b + 1);
    return 1;
}
/* web_url.c's place: a URL's origin is its text before the path (the source
   before 3435c5b4 asked http.c) */
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
/* doc.c's place: CSP (html_policy.c) blocks no style element's text here */
bool doc_style_csp_allowed(web_doc *d, node_t *style, const char *text, size_t length) {
    (void)d; (void)style; (void)text; (void)length;
    return true;
}
/* doc.c's place: the resources @import rules name, and links (none here) */
static const struct { const char *url, *base, *text; } resources[] = {
    {"data:text/css,.child%7Bcolor:red%7D", NULL, ".child{color:red}"},
    {"https://parent.test/import.css", "https://parent.test/import.css", "@import url(\"nested.css\"); .imported{color:green}"},
    {"https://parent.test/nested.css", "https://parent.test/nested.css", ".nested{}"},
    {"https://cross.test/x.css", "https://cross.test/x.css", ".secret{color:red}"},
};
int doc_cssom_import_source(web_doc *d, const char *url, struct cssom_link_source *source) {
    memset(source, 0, sizeof *source);
    for (size_t i = 0; i < sizeof resources / sizeof *resources; i++) if (!strcmp(url, resources[i].url)) {
        source->url = strdup(url);
        source->text = resources[i].text; source->length = strlen(resources[i].text);
        source->base = resources[i].base ? resources[i].base : d->base;
        return source->url ? CSSOM_OK : CSSOM_OOM;
    }
    return CSSOM_INACTIVE;
}
int doc_cssom_link_source(web_doc *d, node_t *n, struct cssom_link_source *source) {
    (void)d; (void)n; memset(source, 0, sizeof *source); return CSSOM_INACTIVE;
}

#include "js_cssom.h"

/* ---- what js_stylesheets.js reads from the bindings around it */
static JSValue raw(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    node_t *n = argc > 1 ? unwrap(ctx, argv[1]) : NULL;
    if (!n) return JS_EXCEPTION;
    return cssom_dom(&state_, n, argc, argv);
}
static JSValue fixture(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *op = argc ? JS_ToCString(ctx, argv[0]) : NULL;
    if (!op) return JS_EXCEPTION;
    JSValue result = JS_UNDEFINED;
    if (!strcmp(op, "document")) result = wrap(&state_, &nodes[0]);
    else if (!strcmp(op, "style")) { int32_t i = 0; JS_ToInt32(ctx, &i, argv[1]); result = wrap(&state_, &nodes[1 + 2 * i]); }
    else if (!strcmp(op, "tag")) {
        node_t *n = unwrap(ctx, argv[1]);
        result = JS_NewString(ctx, n && n->type == N_ELEM && n->tag == T_style ? "style" : "other");
    } else if (!strcmp(op, "attr")) result = JS_NULL;
    else if (!strcmp(op, "restyled")) {
        /* whether a change since the last call left the document to style again */
        result = JS_NewBool(ctx, document_.need_style);
        document_.need_style = false;
    } else if (!strcmp(op, "newestBase")) {
        struct cssom_sheet *s = nodes[0].cssom_sheets;
        result = s && s->base ? JS_NewString(ctx, s->base) : JS_NULL;
    } else if (!strcmp(op, "declarations")) {
        /* a top-level rule's declarations in its sheet's text: what the cascade parses */
        int32_t i = 0, at = 0;
        JS_ToInt32(ctx, &i, argv[1]); JS_ToInt32(ctx, &at, argv[2]);
        int error;
        uint32_t count;
        struct cssom_sheet *s = cssom_style_sheet(&document_, &nodes[1 + 2 * i], &error);
        struct css_rule_info *rule = s ? cssom_child(s, 0, (uint32_t)at, &count) : NULL;
        const char *text = NULL;
        size_t n = 0;
        if (rule) cssom_declarations(rule, &text, &n);
        result = text ? JS_NewStringLen(ctx, text, n) : JS_NULL;
    }
    JS_FreeCString(ctx, op);
    return result;
}
static JSValue jscheck(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    const char *name = argc > 1 ? JS_ToCString(ctx, argv[1]) : NULL;
    check(argc && JS_ToBool(ctx, argv[0]) > 0, name ? name : "unnamed");
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}
static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *s = malloc((size_t)n + 1);
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) { free(s); s = NULL; }
    fclose(f);
    if (s) s[n] = 0;
    return s;
}
static bool run(JSContext *ctx, const char *text, const char *name) {
    JSValue v = JS_Eval(ctx, text, strlen(text), name, JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsException(v)) { JS_FreeValue(ctx, v); return true; }
    JSValue e = JS_GetException(ctx), stack = JS_GetPropertyStr(ctx, e, "stack");
    const char *m = JS_ToCString(ctx, e), *t = JS_ToCString(ctx, stack);
    printf("EXCEPTION %s: %s\n%s\n", name, m ? m : "?", t ? t : "");
    JS_FreeCString(ctx, m); JS_FreeCString(ctx, t); JS_FreeValue(ctx, stack); JS_FreeValue(ctx, e);
    failures++;
    return false;
}

static const char *const sheets[] = {
    /* WPT css/cssom/cssimportrule*.html, CSSImportRule-*.html */
    "@import url(\"data:text/css,.child%7Bcolor:red%7D\") screen;"
    "@import url(\"https://parent.test/import.css\");"
    "@import url(\"https://parent.test/pending.css\");"
    "@import url(\"https://cross.test/x.css\");"
    "@import url(\"https://parent.test/import.css\") supports(invalid: value);"
    /* css/css-cascade/layer-rules-cssom.html */
    "@layer a, b.c;"
    /* css/css-conditional/at-media-matches-001.html, at-supports-matches.html */
    "@media all {} @media not all {} @media (min-width: 500px) {} @media (min-width: 5000px) {} @media {}"
    "@supports (color: green) { div {} } @supports (invalid: value) { div {} }"
    /* css/css-animations/KeyframeEffect-getKeyframes.tentative.html: !important in a keyframe is ignored */
    "@keyframes k { to { margin-top: 150px !important; margin-bottom: 200px; } from { color: red !important; color: blue; } }"
    /* css/css-nesting/parsing.html */
    ".foo { > .bar {} & + .baz, &.qux {} :is(.bar, &.baz) {} && {} [title=\"a&b\"] {} .x & {} >.y {} & > section, & > article {} .p, .q {} &:HOVER {} }",
    "@import url(\"https://parent.test/import.css\");",
};

/* argv[1]: the js_stylesheets.js to run (the tree's, or an old revision's) */
int main(int argc, char **argv) {
    for (int i = 0; i < NODES; i++) wrappers[i] = JS_UNDEFINED;
    nodes[0].type = N_DOC;
    for (int i = 0; i < 2; i++) {
        node_t *style = &nodes[1 + 2 * i], *text = &nodes[2 + 2 * i];
        style->type = N_ELEM; style->tag = T_style;
        text->type = N_TEXT; text->text = (char *)sheets[i]; text->textlen = strlen(sheets[i]);
        child(0, 1 + 2 * i); child(1 + 2 * i, 2 + 2 * i);
    }
    for (int i = 0; i < NODES; i++) nodes[i].owner = &document_;
    (void)attr;
    document_.root = &nodes[0]; document_.live = true; document_.dom_revision = 1;
    document_.url = "https://parent.test/page"; document_.base = "https://parent.test/page";
    document_.width = 800; document_.height = 600;
    JSRuntime *rt = JS_NewRuntime();
    JS_NewClassID(&node_class);
    JSClassDef nodes_class = {.class_name = "FixtureNode"};
    JS_NewClass(rt, node_class, &nodes_class);
    JSContext *ctx = JS_NewContext(rt);
    state_.ctx = ctx; state_.doc = &document_; document_.js = &state_;
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "fixture", JS_NewCFunction(ctx, fixture, "fixture", 2));
    JS_SetPropertyStr(ctx, global, "fixtureRaw", JS_NewCFunction(ctx, raw, "fixtureRaw", 8));
    JS_SetPropertyStr(ctx, global, "check", JS_NewCFunction(ctx, jscheck, "check", 2));
    JS_FreeValue(ctx, global);
    char *bindings = slurp(argc > 1 ? argv[1] : "user/libc/web/js_stylesheets.js"), *prelude = slurp("tests/cssom_bindings_prelude.js"),
         *cases = slurp("tests/cssom_bindings_cases.js");
    if (!bindings || !prelude || !cases) { printf("cannot read the scripts\n"); return 2; }
    /* js_stylesheets.js runs inside the bindings' closure, beside its helpers */
    size_t n = strlen(prelude) + strlen(bindings) + 16;
    char *program = malloc(n);
    snprintf(program, n, "%s\n%s\n})();\n", prelude, bindings);
    if (run(ctx, program, "js_stylesheets.js")) run(ctx, cases, "cssom_bindings_cases.js");
    printf("cssom bindings: %d passed, %d failed\n", checks - failures, failures);
    return failures ? 1 : 0;
}
