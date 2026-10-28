/* The engine side of tests/css_cascade_hosttest.c: one document of dom.c's nodes, the sheets of its
 * <style> elements parsed by css.c, css.c's cascade, and what a page reads of it, as natives on host:
 * the computed values (css_custom_value for a custom property, css_computed_longhand else), the
 * registrations (css_register_property, the call js.c's native_css_register_property makes),
 * CSS.supports (css_supports_*) and a style element's rules through cssom.c. Built with Nocturne's
 * headers, as the engine is; the host's stdio is css_cascade_hosttest.c's.
 *
 * Not js.c: getComputedStyle's resolved values (the used width, a matrix) are not computed here, and
 * there is no layout, no box, no animation, no event and no script element. */
#include "webi.h"
#include "cssom.h"
#include "quickjs.h"

enum { VIEWPORT_W = 800, VIEWPORT_H = 600 };
static web_doc *doc;
static node_t **nodes; /* a node's handle is its index */
static uint32_t node_count, node_capacity;
static char *sheet_signature; /* the style texts of the sheets the document has */

static uint32_t handle(node_t *n) {
    if (!n) return UINT32_MAX;
    for (uint32_t i = 0; i < node_count; i++) if (nodes[i] == n) return i;
    if (node_count == node_capacity) {
        node_capacity = node_capacity ? node_capacity * 2 : 256;
        nodes = realloc(nodes, node_capacity * sizeof *nodes);
    }
    nodes[node_count] = n;
    return node_count++;
}
static node_t *node_of(JSContext *ctx, JSValueConst v) {
    uint32_t i;
    if (JS_IsNull(v) || JS_IsUndefined(v) || JS_ToUint32(ctx, &i, v) < 0 || i >= node_count) return NULL;
    return nodes[i];
}

/* ---------------------------------------------------------------- the document */
static void document_new(void) {
    doc = calloc(1, sizeof *doc);
    doc->url = strdup("http://web-platform.test:8000/");
    doc->base = strdup(doc->url);
    doc->custom_registry_id = -1;
    doc->root = doc_node_create(doc, N_DOC, "#document", NULL, 0);
    node_t *html = doc_element_create(doc, WEB_NS_HTML, NULL, "html");
    doc_node_move(doc, doc->root, html, NULL);
    doc->html = html;
    node_t *head = doc_element_create(doc, WEB_NS_HTML, NULL, "head"), *body = doc_element_create(doc, WEB_NS_HTML, NULL, "body");
    doc_node_move(doc, html, head, NULL);
    doc_node_move(doc, html, body, NULL);
    doc->need_style = true;
}
static void text_of(node_t *n, sbuf *out) {
    for (node_t *c = n->first; c; c = c->next) if (c->type == N_TEXT && c->text) sb_put(out, c->text, c->textlen);
}
static bool style_element(const node_t *n) {
    return n->type == N_ELEM && n->namespace_id == NS_HTML && n->tag == T_style;
}
/* the connected <style> elements in tree order, shadow trees' after their hosts' children */
static void styles(node_t *n, pvec *out) {
    for (node_t *c = n->first; c; c = c->next) {
        if (style_element(c)) pv_push(out, c);
        if (c->type == N_ELEM) {
            styles(c, out);
            if (c->shadow_root) styles(c->shadow_root, out);
        }
    }
}
/* doc.c's sheet scan in the small: each style element's text a sheet, in tree order, a shadow
   tree's scoped to its root (css_sheet_scope). The ASTs are kept: computed styles may borrow them. */
static void sheets_publish(void) {
    pvec list = {0};
    styles(doc->root, &list);
    sbuf signature = {0};
    for (int i = 0; i < list.n; i++) {
        char key[32];
        snprintf(key, sizeof key, "\x01%p\x02", list.v[i]);
        sb_puts(&signature, key);
        text_of(list.v[i], &signature);
    }
    const char *now = sb_cstr(&signature);
    if (sheet_signature && !strcmp(sheet_signature, now)) { sb_free(&signature); pv_free(&list); return; }
    free(sheet_signature);
    sheet_signature = strdup(now);
    sb_free(&signature);
    css_styling_free(&doc->sty); /* the context and the list; the registrations are the document's */
    for (int i = 0; i < list.n; i++) {
        node_t *style = list.v[i];
        sbuf text = {0};
        text_of(style, &text);
        arena_t *a = calloc(1, sizeof *a);
        a->chunk_size = 4096;
        sheet_t *s = css_parse_sheet(a, text.p ? text.p : "", text.n, doc->url, i, NULL);
        sb_free(&text);
        node_t *root = doc_node_root(style, false);
        if (root && root->shadow_host) css_sheet_scope(s, root);
        pv_push(&doc->sty.sheets, s);
    }
    pv_free(&list);
    doc->sty.index_dirty = true;
    css_mark_dirty(doc, NULL);
    doc->need_style = true;
}
static void style_flush(void) {
    sheets_publish();
    if (!doc->need_style) return;
    css_cascade(doc, VIEWPORT_W, VIEWPORT_H);
    css_styles_release(doc);
    doc->need_style = false;
}

/* ---------------------------------------------------------------- natives */
static JSValue string_or_null(JSContext *ctx, const char *s) { return s ? JS_NewString(ctx, s) : JS_NULL; }
static const char *arg(JSContext *ctx, int argc, JSValueConst *argv, int i) {
    return i < argc && !JS_IsNull(argv[i]) && !JS_IsUndefined(argv[i]) ? JS_ToCString(ctx, argv[i]) : NULL;
}
static JSValue n_document(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self; (void)argc; (void)argv;
    node_t *head = doc->html->first, *body = head ? head->next : NULL;
    JSValue r = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, r, 0, JS_NewUint32(ctx, handle(doc->root)));
    JS_SetPropertyUint32(ctx, r, 1, JS_NewUint32(ctx, handle(doc->html)));
    JS_SetPropertyUint32(ctx, r, 2, JS_NewUint32(ctx, handle(head)));
    JS_SetPropertyUint32(ctx, r, 3, JS_NewUint32(ctx, handle(body)));
    return r;
}
/* element(namespace or null, local name) */
static JSValue n_element(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    const char *ns = arg(ctx, argc, argv, 0), *name = arg(ctx, argc, argv, 1);
    node_t *e = name ? doc_element_create(doc, ns ? ns : WEB_NS_HTML, NULL, name) : NULL;
    JS_FreeCString(ctx, ns); JS_FreeCString(ctx, name);
    return e ? JS_NewUint32(ctx, handle(e)) : JS_ThrowTypeError(ctx, "element");
}
static JSValue n_text(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    size_t n = 0;
    const char *text = argc ? JS_ToCStringLen(ctx, &n, argv[0]) : NULL;
    if (!text) return JS_EXCEPTION;
    node_t *t = doc_node_create(doc, N_TEXT, "#text", text, n);
    JS_FreeCString(ctx, text);
    return JS_NewUint32(ctx, handle(t));
}
/* insert(parent, child, before or null) */
static JSValue n_insert(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    node_t *p = argc > 0 ? node_of(ctx, argv[0]) : NULL, *c = argc > 1 ? node_of(ctx, argv[1]) : NULL;
    node_t *before = argc > 2 ? node_of(ctx, argv[2]) : NULL;
    if (!p || !c || !doc_node_move(doc, p, c, before)) return JS_ThrowTypeError(ctx, "insert");
    return JS_UNDEFINED;
}
static JSValue n_remove(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    node_t *n = argc ? node_of(ctx, argv[0]) : NULL;
    if (n && n->parent) doc_node_remove(doc, n);
    return JS_UNDEFINED;
}
/* attribute(element, name, value: a string sets it, null removes it, undefined reads it) */
static JSValue n_attribute(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    node_t *e = argc ? node_of(ctx, argv[0]) : NULL;
    const char *name = arg(ctx, argc, argv, 1);
    if (!e || !name) { JS_FreeCString(ctx, name); return JS_ThrowTypeError(ctx, "attribute"); }
    JSValue result = JS_UNDEFINED;
    if (argc > 2 && JS_IsUndefined(argv[2])) result = string_or_null(ctx, node_attr(e, name));
    else if (argc > 2 && JS_IsNull(argv[2])) {
        int i = doc_attr_index(e, NULL, name, false);
        if (i >= 0) doc_attr_remove(doc, e, i);
    } else {
        const char *value = arg(ctx, argc, argv, 2);
        doc_attr_set_ns(doc, e, NULL, NULL, name, value ? value : "");
        JS_FreeCString(ctx, value);
    }
    JS_FreeCString(ctx, name);
    return result;
}
/* tree(node): [type, name, parent, first, next, previous, shadow root, host] */
static JSValue n_tree(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    node_t *n = argc ? node_of(ctx, argv[0]) : NULL;
    if (!n) return JS_NULL;
    JSValue r = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, r, 0, JS_NewString(ctx, n->type == N_ELEM ? "element" : n->type == N_TEXT ? "text" :
                                                      n->type == N_DOC ? "document" : n->type == N_FRAGMENT ? "fragment" : "other"));
    JS_SetPropertyUint32(ctx, r, 1, string_or_null(ctx, n->type == N_ELEM ? (n->local_name ? n->local_name : n->name) : n->name));
    node_t *links[] = {n->parent, n->first, n->next, n->prev, n->type == N_ELEM ? n->shadow_root : NULL, n->shadow_host};
    for (uint32_t i = 0; i < 6; i++) JS_SetPropertyUint32(ctx, r, 2 + i, links[i] ? JS_NewUint32(ctx, handle(links[i])) : JS_NULL);
    JS_SetPropertyUint32(ctx, r, 8, n->type == N_TEXT ? JS_NewStringLen(ctx, n->text ? n->text : "", n->textlen) : JS_NULL);
    return r;
}
static JSValue n_shadow(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    node_t *host = argc ? node_of(ctx, argv[0]) : NULL;
    node_t *root = host ? doc_shadow_attach(doc, host, false, false, false, false, false) : NULL;
    return root ? JS_NewUint32(ctx, handle(root)) : JS_ThrowTypeError(ctx, "attachShadow");
}
static int pseudo_of(const char *p) {
    if (!p || !*p) return PE_NONE;
    if (!strcmp(p, "::before") || !strcmp(p, ":before")) return PE_BEFORE;
    if (!strcmp(p, "::after") || !strcmp(p, ":after")) return PE_AFTER;
    return -1;
}
/* computed(element, property, pseudo-element): the computed value's text (CSS Cascade 5 §4.4) */
static JSValue n_computed(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    node_t *e = argc ? node_of(ctx, argv[0]) : NULL;
    const char *property = arg(ctx, argc, argv, 1), *pseudo = arg(ctx, argc, argv, 2);
    JSValue result = JS_NewString(ctx, "");
    if (e && property && e->type == N_ELEM && doc_node_connected(e)) {
        style_flush();
        int pe = pseudo_of(pseudo);
        style_t *st = e->style;
        if (st && pe == PE_BEFORE) st = st->before;
        else if (st && pe == PE_AFTER) st = st->after;
        else if (st && pe != PE_NONE) st = NULL;
        if (st && property[0] == '-' && property[1] == '-') {
#ifdef OLD_SOURCE /* before css_custom_value: the computed list's own entry */
            const struct custom_prop *cp = css_custom_property(st, property);
            const char *v = cp ? cp->value : NULL;
#else
            const char *v = css_custom_value(doc, st, property);
#endif
            JS_FreeValue(ctx, result);
            result = JS_NewString(ctx, v ? v : "");
        } else if (st) {
            sbuf text = {0};
            const struct propdef *p = css_prop_lookup(property, strlen(property));
            int kind = css_computed_longhand(st, property, &text, doc);
            if (kind != 1 && p) { text.n = 0; if (!css_computed_value(st, p, &text, doc)) text.n = 0; }
            JS_FreeValue(ctx, result);
            result = JS_NewStringLen(ctx, text.p ? text.p : "", text.n);
            sb_free(&text);
        }
    }
    JS_FreeCString(ctx, property); JS_FreeCString(ctx, pseudo);
    return result;
}
struct names { JSContext *ctx; JSValue list; uint32_t n; };
static bool name_entry(void *arg, const char *name, const char *value) {
    struct names *l = arg;
    if (value) JS_SetPropertyUint32(l->ctx, l->list, l->n++, JS_NewString(l->ctx, name));
    return true;
}
/* customNames(element): the custom properties it has a value of (css_custom_values, as js.c lists them) */
static JSValue n_custom_names(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    node_t *e = argc ? node_of(ctx, argv[0]) : NULL;
    struct names l = {ctx, JS_NewArray(ctx), 0};
    if (e && e->type == N_ELEM && doc_node_connected(e)) {
        style_flush();
#ifdef OLD_SOURCE /* before css_custom_values: js.c walked the computed list */
        if (e->style) for (const struct custom_prop *p = e->style->vars; p; p = p->next) name_entry(&l, p->name, p->value);
#else
        if (e->style) css_custom_values(doc, e->style, name_entry, &l);
#endif
    }
    return l.list;
}
/* the same status js.c's native_css_register_property returns: 0, 1 a SyntaxError, 2 an
   InvalidModificationError */
static JSValue n_register(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    const char *name = arg(ctx, argc, argv, 0), *syntax = arg(ctx, argc, argv, 1), *initial = arg(ctx, argc, argv, 3);
    bool inherits = argc > 2 && JS_ToBool(ctx, argv[2]);
#ifdef OLD_SOURCE /* no registrations: the old js_css_supports.js has no CSS.registerProperty to call this */
    (void)inherits;
    JS_FreeCString(ctx, name); JS_FreeCString(ctx, syntax); JS_FreeCString(ctx, initial);
    return JS_ThrowTypeError(ctx, "no registrations before css_register_property");
#else
    int status = name && syntax ? css_register_property(doc, name, syntax, inherits, initial) : CSS_REGISTER_SYNTAX;
    JS_FreeCString(ctx, name); JS_FreeCString(ctx, syntax); JS_FreeCString(ctx, initial);
    if (status == CSS_REGISTER_MEMORY) return JS_ThrowOutOfMemory(ctx);
    return JS_NewInt32(ctx, status == CSS_REGISTER_OK ? 0 : status == CSS_REGISTER_DUPLICATE ? 2 : 1);
#endif
}
static JSValue n_supports(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    size_t an, bn = 0;
    const char *a = argc ? JS_ToCStringLen(ctx, &an, argv[0]) : NULL;
    if (!a) return JS_EXCEPTION;
    const char *b = argc > 1 ? JS_ToCStringLen(ctx, &bn, argv[1]) : NULL;
    bool result = b ? css_supports_declaration(a, an, b, bn) : css_supports_condition(a, an, true);
    JS_FreeCString(ctx, a); JS_FreeCString(ctx, b);
    return JS_NewBool(ctx, result);
}
/* rules(style element): its sheet's top-level rules as cssom.c reads them, [type, cssText, name,
   syntax, inherits, initialValue or null] */
static JSValue n_rules(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    node_t *e = argc ? node_of(ctx, argv[0]) : NULL;
    JSValue list = JS_NewArray(ctx);
    if (!e) return list;
    sbuf text = {0};
    text_of(e, &text);
    int error;
    struct cssom_sheet *s = cssom_construct(doc, &error);
    if (!s || cssom_replace_sync(s, text.p ? text.p : "", text.n)) { sb_free(&text); return list; }
    sb_free(&text);
    uint32_t count;
    cssom_child(s, 0, UINT32_MAX, &count);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t n;
        struct css_rule_info *r = cssom_child(s, 0, i, &n);
        if (!r) break;
        JSValue item = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, item, 0, JS_NewInt32(ctx, r->type));
        sbuf b = {0};
        cssom_serialize(s, r, &b);
        JS_SetPropertyUint32(ctx, item, 1, JS_NewStringLen(ctx, b.p ? b.p : "", b.n));
        static const char *const names[] = {"name", "syntax", "inherits", "initialValue"};
        for (uint32_t k = 0; k < 4; k++) {
            b.n = 0;
            bool found = cssom_attribute(s, r, names[k], &b);
            JS_SetPropertyUint32(ctx, item, 2 + k, found ? JS_NewStringLen(ctx, b.p ? b.p : "", b.n) : JS_NULL);
        }
        sb_free(&b);
        JS_SetPropertyUint32(ctx, list, i, item);
    }
    return list;
}
/* the inline style's declarations as js.c's element.style serializes them */
static JSValue n_inline(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    size_t n;
    const char *text = argc ? JS_ToCStringLen(ctx, &n, argv[0]) : NULL;
    if (!text) return JS_EXCEPTION;
    size_t clean_n;
    char *clean = css_preprocess(text, n, &clean_n);
    JS_FreeCString(ctx, text);
    sbuf block = {0};
    if (clean) css_decl_serialize(clean, clean_n, NULL, &block, NULL);
    free(clean);
    JSValue r = JS_NewStringLen(ctx, block.p ? block.p : "", block.n);
    sb_free(&block);
    return r;
}
static JSValue n_decl_get(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    size_t n;
    const char *block = argc ? JS_ToCStringLen(ctx, &n, argv[0]) : NULL, *property = arg(ctx, argc, argv, 1);
    JSValue r = JS_NewString(ctx, "");
    if (block && property) {
        const char *v; size_t vn; bool important;
        if (css_decl_find(block, n, property, &v, &vn, &important)) { JS_FreeValue(ctx, r); r = JS_NewStringLen(ctx, v, vn); }
    }
    JS_FreeCString(ctx, block); JS_FreeCString(ctx, property);
    return r;
}
static JSValue n_decl_set(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    size_t n;
    const char *block = argc ? JS_ToCStringLen(ctx, &n, argv[0]) : NULL, *property = arg(ctx, argc, argv, 1),
               *value = arg(ctx, argc, argv, 2), *priority = arg(ctx, argc, argv, 3);
    JSValue r = JS_UNDEFINED;
    if (block && property) {
        sbuf out = {0};
        int status = css_decl_set(block, n, property, value ? value : "", priority, NULL, &out);
        r = status > 0 ? JS_NewStringLen(ctx, out.p ? out.p : "", out.n) : JS_NewStringLen(ctx, block, n);
        sb_free(&out);
    }
    JS_FreeCString(ctx, block); JS_FreeCString(ctx, property); JS_FreeCString(ctx, value); JS_FreeCString(ctx, priority);
    return r;
}
static JSValue n_decl_names(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    size_t n;
    const char *block = argc ? JS_ToCStringLen(ctx, &n, argv[0]) : NULL;
    JSValue list = JS_NewArray(ctx);
    if (!block) return list;
    sbuf names = {0};
    sbuf text = {0};
    css_decl_serialize(block, n, NULL, &text, &names); /* the names in the block's order */
    sb_free(&text);
    uint32_t i = 0;
    for (size_t at = 0; at < names.n; at += strlen(names.p + at) + 1) JS_SetPropertyUint32(ctx, list, i++, JS_NewString(ctx, names.p + at));
    sb_free(&names);
    JS_FreeCString(ctx, block);
    return list;
}

void css_cascade_host_init(JSContext *ctx, JSValue host) {
    document_new();
    static const JSCFunctionListEntry natives[] = {
        JS_CFUNC_DEF("document", 0, n_document),
        JS_CFUNC_DEF("element", 2, n_element),
        JS_CFUNC_DEF("text", 1, n_text),
        JS_CFUNC_DEF("insert", 3, n_insert),
        JS_CFUNC_DEF("remove", 1, n_remove),
        JS_CFUNC_DEF("attribute", 3, n_attribute),
        JS_CFUNC_DEF("tree", 1, n_tree),
        JS_CFUNC_DEF("attachShadow", 1, n_shadow),
        JS_CFUNC_DEF("computed", 3, n_computed),
        JS_CFUNC_DEF("customNames", 1, n_custom_names),
        JS_CFUNC_DEF("cssRegisterProperty", 4, n_register),
        JS_CFUNC_DEF("cssSupports", 2, n_supports),
        JS_CFUNC_DEF("rules", 1, n_rules),
        JS_CFUNC_DEF("inlineText", 1, n_inline),
        JS_CFUNC_DEF("declGet", 2, n_decl_get),
        JS_CFUNC_DEF("declSet", 4, n_decl_set),
        JS_CFUNC_DEF("declNames", 1, n_decl_names),
    };
    JS_SetPropertyFunctionList(ctx, host, natives, sizeof natives / sizeof *natives);
}
