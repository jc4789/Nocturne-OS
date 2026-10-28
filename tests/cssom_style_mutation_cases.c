/* MutationObserver's attribute records for an element's inline style on the host
   (css/cssom/MutationObserver-style.html): the cssText setter updates the style
   attribute (CSSOM 6.7 "update style attribute for the CSS declaration block"), so
   each assignment queues a record even when the value stays the same, while a
   setProperty() that leaves the declarations as they were changes nothing and
   queues none. tests/check_cssom_rules_native.py copies out of js.c the native
   DOM call's mutation test (which calls observe), the cssText setter
   (style_block_access) and the attribute write it ends in (style_attribute_set),
   and builds them with js_mutations.h, css.c and the DOM. Built with OLD_SOURCE
   against the source before the cssText setter queued its record, the same cases
   run on what that source has. */
#include "webi.h"
#include "form_value.h"
#include "quickjs.h"
#include <stdio.h>
#include "js_dom_opcodes.h"
static int passed, failed;
static void check(bool ok, const char *label) {
    if (ok) passed++;
    else { failed++; printf("FAIL %s\n", label); }
}

/* the parts of js.c's state js_mutations.h and the copied functions read */
struct js_mutation_registration;
struct js_mutation_record;
struct web_js_state {
    JSContext *ctx;
    web_doc *doc;
    struct js_mutation_registration *mutation_registrations;
    struct js_mutation_record *mutation_first, *mutation_last;
    bool mutation_job_pending;
};
static struct web_js_state state_;
static struct web_js_state *state(JSContext *ctx) { (void)ctx; return &state_; }
static JSValue oom(JSContext *ctx) { return JS_ThrowOutOfMemory(ctx); }
static void exception(struct web_js_state *s) { (void)s; }
static JSValue custom_element_hook(struct web_js_state *s, const char *name, int argc, JSValueConst *argv) {
    (void)s; (void)name; (void)argc; (void)argv; return JS_UNDEFINED;
}
/* the cases pass no node in argv: the cssText setter's third argument is the text */
static node_t *node_opaque(JSValueConst v) { (void)v; return NULL; }
/* delivery to script and observe(), which these cases do not reach */
static JSValue wrap(struct web_js_state *s, node_t *n) { (void)n; return JS_ThrowInternalError(s->ctx, "unreached: wrap"); }
static node_t *unwrap(JSContext *ctx, JSValueConst v) { (void)ctx; (void)v; return NULL; }
static JSValue str_or_null(JSContext *ctx, const char *text) { return text ? JS_NewString(ctx, text) : JS_NULL; }
/* what dom.c's attribute change asks of the owners of controls, details and canvases, for
   the cases' div: none of them acts on it */
bool doc_control_selection_supported(const node_t *node) { (void)node; return false; }
enum web_input_kind web_input_type(const node_t *n) { (void)n; return WEB_INPUT_TEXT; }
void doc_details_attribute_changed(web_doc *d, node_t *details, const char *name, bool old_present) {
    (void)d; (void)details; (void)name; (void)old_present;
}
void web_select_attribute_changed(web_doc *d, node_t *node, const char *name, bool present) {
    (void)d; (void)node; (void)name; (void)present;
}
void web_canvas_attr_changed(node_t *node, const char *name) { (void)node; (void)name; }
/* doc.c's interruption point for long DOM work: nothing to interrupt here */
void doc_dom_budget(web_doc *d) { (void)d; }
/* html_lexbor_bridge.c's mirror of the parser's tree: the cases' element was not parsed */
void html_bridge_native_changed(node_t *node, unsigned flags) { (void)node; (void)flags; }
/* the getter's half of style_block_access, which these cases never reach */
#ifndef OLD_SOURCE
struct js_style_memo { char *serialized, *names; size_t serialized_n, names_n; };
static struct js_style_memo *style_memo_get(struct web_js_state *s, node_t *n) { (void)s; (void)n; return NULL; }
static JSValue style_text_block(JSContext *ctx, const char *text, size_t n, const char *names, size_t names_n) {
    (void)text; (void)n; (void)names; (void)names_n; return oom(ctx);
}
#else
static bool style_memo_block(struct web_js_state *s, node_t *n, sbuf *b) { (void)s; (void)n; (void)b; return false; }
/* the old attribute write kept js.c's memo of the block: none here */
static void style_memo_store(struct web_js_state *s, const node_t *n, const char *source, size_t source_n,
                             const char *block, size_t block_n) {
    (void)s; (void)n; (void)source; (void)source_n; (void)block; (void)block_n;
}
static JSValue style_text_block(JSContext *ctx, const char *text, size_t n) { (void)text; (void)n; return oom(ctx); }
#endif
#include "js_mutations.h"
#include "mutation-extract.h"

static int records(void) {
    int count = 0;
    for (struct js_mutation_record *r = state_.mutation_first; r; r = r->next) count++;
    return count;
}
static void deliver(void) {
    struct js_mutation_record *r = state_.mutation_first;
    while (r) { struct js_mutation_record *next = r->next; mutation_record_free(&state_, r); r = next; }
    state_.mutation_first = state_.mutation_last = NULL;
}
/* js.c's native DOM call around the cssText setter: el.style.cssText = text */
static void css_text_set(node_t *e, const char *text) {
    JSValue argv[3] = {JS_UNDEFINED, JS_UNDEFINED, JS_NewString(state_.ctx, text)};
    bool observed = dom_mutates(DOM_styleBlock, 3);
    struct js_mutation_snapshot c = observed ? mutation_before(&state_, DOM_styleBlock, e, 3, argv) : (struct js_mutation_snapshot){0};
    JSValue result = style_block_access(&state_, e, 3, argv);
    if (observed) mutation_after(&state_, &c, !JS_IsException(result));
    JS_FreeValue(state_.ctx, result);
    JS_FreeValue(state_.ctx, argv[2]);
}
/* el.style.setProperty(property, value): the declarations change through css.c's
   css_inline_set and only a change writes the attribute (js.c's style_access) */
static void property_set(node_t *e, const char *property, const char *value) {
    JSValue argv[5] = {JS_UNDEFINED, JS_UNDEFINED, JS_NewString(state_.ctx, property), JS_NewString(state_.ctx, value),
                       JS_UNDEFINED};
    bool observed = dom_mutates(DOM_style, 4);
    struct js_mutation_snapshot c = observed ? mutation_before(&state_, DOM_style, e, 4, argv) : (struct js_mutation_snapshot){0};
    const char *style = node_attr(e, "style");
    sbuf block = {0}, written = {0};
    size_t n = 0;
    char *clean = css_preprocess(style ? style : "", style ? strlen(style) : 0, &n);
#ifndef OLD_SOURCE
    const char *own = css_inline_block(e);
    if (own) sb_puts(&block, own); else css_decl_block(clean, n, NULL, &block);
#else
    css_decl_serialize(clean, n, NULL, &block, NULL);
#endif
    free(clean);
    bool ok = true;
    if (css_inline_set(e->owner, e, block.p ? block.p : "", block.n, property, value, NULL, &written) > 0)
        ok = style_attribute_set(&state_, e, &written);
    sb_free(&block); sb_free(&written);
    if (observed) mutation_after(&state_, &c, ok);
    JS_FreeValue(state_.ctx, argv[2]); JS_FreeValue(state_.ctx, argv[3]);
}

int main(void) {
    /* a div of the document, not yet in its tree (the records are the same; the
       restyle of a connected element is the cascade's, outside these cases) */
    static web_doc document_;
    static node_t root = {.type = N_DOC}, element = {.type = N_ELEM, .tag = T_div, .name = "div"};
    document_.root = &root; root.owner = element.owner = &document_;
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    state_.ctx = ctx; state_.doc = &document_;
    /* observe(el, {attributes: true}) */
    struct js_mutation_registration *observing = calloc(1, sizeof *observing);
    observing->node = &element; observing->observer = JS_NewObject(ctx); observing->flags = MO_ATTR;
    state_.mutation_registrations = observing;

    /* "Updating style property with the same value does not trigger an observation callback" */
    property_set(&element, "height", "100px");
    check(records() == 1, "setProperty: a new value queues a record");
    deliver();
    property_set(&element, "height", "100px");
    check(records() == 0, "setProperty: the same value queues none");
    deliver();
    /* "Updating cssText triggers an observation callback even if the value is the same" */
    css_text_set(&element, "height:100px");
    check(records() == 1, "cssText: an assignment queues a record");
    deliver();
    css_text_set(&element, "height:100px");
    check(records() == 1, "cssText: the same value again queues a record");
    deliver();
    const char *style = node_attr(&element, "style");
    check(style && !strcmp(style, "height: 100px;"), "cssText: the attribute is the block's serialization");

    printf("cssom style mutations: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
