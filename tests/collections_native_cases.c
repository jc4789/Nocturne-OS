/* The DOM collections' actual native half (user/libc/web/js_collections_native.h)
   and JS half (js_collections.js) on the product QuickJS, over a small native
   tree that stands in for dom.c: elements and text nodes with null-namespace
   attributes, children, an owner Document and its DOM revision. Only that tree
   and js.c's few accessors are shims. check_collections_native.py builds it;
   collections_native_cases.js holds the expectations.
   usage: collections_native_cases <js_collections.js> <cases.js> */
#include <nocturne.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"

/* The engine's Atomics.wait and its clock; the cases never wait. */
uint64_t n_clock_uptime_ms(void) { return 0; }
n_result wait_on_address(volatile uint32_t *address, uint32_t expected, unsigned ms) { (void)address; (void)expected; (void)ms; return -N_ERR_TIMEOUT; }
n_result wake_address(volatile uint32_t *address, unsigned count) { (void)address; (void)count; return 0; }

enum { N_ELEM = 1, N_TEXT = 3, N_DOC = 9 };
#define WEB_NS_HTML "http://www.w3.org/1999/xhtml"
struct attr { char *local, *value, *namespace_uri; };
typedef struct web_doc web_doc;
typedef struct node {
    int type; web_doc *owner; struct attr *attrs; int nattrs; bool html;
    JSValue wrapper; struct node *parent, **children; int nchildren, cchildren;
    struct node *all_next;
} node_t;
struct web_doc { node_t *root; uint64_t dom_revision; web_doc *all_next; };
struct web_js_state { web_doc *doc; bool starting; };
static struct web_js_state the_state;
static JSClassID node_class;
static JSContext *the_ctx;
static node_t *all_nodes;
static web_doc *all_docs;
static unsigned long child_reads, attr_reads;

/* js.c's accessors the native half uses. */
static struct web_js_state *state(JSContext *ctx) { (void)ctx; return &the_state; }
static node_t *node_opaque(JSValueConst v) { return JS_GetOpaque(v, node_class); }
static JSValue wrap(struct web_js_state *s, node_t *n) { (void)s; return n ? JS_DupValue(the_ctx, n->wrapper) : JS_NULL; }
static bool dom_authority_denied(struct web_js_state *s, web_doc *d) { (void)s; (void)d; return false; }
static JSValue history_security_error(JSContext *ctx, const char *message) { return JS_ThrowTypeError(ctx, "%s", message); }
static void doc_dom_budget(web_doc *d) { (void)d; }
static const char *doc_element_namespace(const node_t *n) {
    if (!n || n->type != N_ELEM) return NULL;
    return n->html ? WEB_NS_HTML : "urn:x-nocturne-test";
}
static int doc_attr_index(node_t *n, const char *ns, const char *name, bool namespaced) {
    (void)namespaced;
    attr_reads++;
    if (!n || n->type != N_ELEM) return -1;
    for (int i = 0; i < n->nattrs; i++)
        if (!ns && !n->attrs[i].namespace_uri && !strcmp(n->attrs[i].local, name)) return i;
    return -1;
}
#include "user/libc/web/js_collections_native.h"

static node_t *new_node(JSContext *ctx, int type, web_doc *owner, bool html) {
    node_t *n = calloc(1, sizeof *n);
    n->type = type; n->owner = owner; n->html = html;
    n->wrapper = JS_NewObjectClass(ctx, node_class);
    JS_SetOpaque(n->wrapper, n);
    n->all_next = all_nodes; all_nodes = n;
    return n;
}
static node_t *arg_node(JSContext *ctx, JSValueConst v) {
    node_t *n = node_opaque(v);
    if (!n) JS_ThrowTypeError(ctx, "Invalid DOM receiver");
    return n;
}
static web_doc *arg_doc(JSContext *ctx, JSValueConst v) {
    node_t *n = arg_node(ctx, v);
    if (n && n->type != N_DOC) { JS_ThrowTypeError(ctx, "Document required"); return NULL; }
    return n ? n->owner : NULL;
}
static void detach(node_t *c) {
    node_t *o = c->parent;
    if (!o) return;
    for (int i = 0; i < o->nchildren; i++)
        if (o->children[i] == c) { memmove(o->children + i, o->children + i + 1, (o->nchildren - i - 1) * sizeof *o->children); o->nchildren--; break; }
    c->parent = NULL;
}
/* T.doc(): a new Document. */
static JSValue t_doc(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    web_doc *d = calloc(1, sizeof *d);
    d->all_next = all_docs; all_docs = d;
    d->root = new_node(ctx, N_DOC, d, false);
    return JS_DupValue(ctx, d->root->wrapper);
}
/* T.element(document, html) and T.text(document). */
static JSValue t_element(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    web_doc *d = arg_doc(ctx, argv[0]);
    if (!d) return JS_EXCEPTION;
    return JS_DupValue(ctx, new_node(ctx, N_ELEM, d, JS_ToBool(ctx, argv[1]) > 0)->wrapper);
}
static JSValue t_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    web_doc *d = arg_doc(ctx, argv[0]);
    if (!d) return JS_EXCEPTION;
    return JS_DupValue(ctx, new_node(ctx, N_TEXT, d, false)->wrapper);
}
/* T.attr(element, name, value or null, quiet): quiet changes no revision. */
static JSValue t_attr(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    node_t *n = arg_node(ctx, argv[0]);
    if (!n) return JS_EXCEPTION;
    if (n->type != N_ELEM) return JS_ThrowTypeError(ctx, "Element required");
    const char *name = JS_ToCString(ctx, argv[1]);
    if (!name) return JS_EXCEPTION;
    int i = -1;
    for (int k = 0; k < n->nattrs; k++) if (!strcmp(n->attrs[k].local, name)) i = k;
    if (JS_IsNull(argv[2])) {
        if (i >= 0) { free(n->attrs[i].local); free(n->attrs[i].value); n->attrs[i] = n->attrs[--n->nattrs]; }
    } else {
        const char *value = JS_ToCString(ctx, argv[2]);
        if (!value) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
        if (i < 0) {
            n->attrs = realloc(n->attrs, (n->nattrs + 1) * sizeof *n->attrs);
            i = n->nattrs++;
            n->attrs[i] = (struct attr){strdup(name), NULL, NULL};
        }
        free(n->attrs[i].value); n->attrs[i].value = strdup(value);
        JS_FreeCString(ctx, value);
    }
    JS_FreeCString(ctx, name);
    if (JS_ToBool(ctx, argv[3]) <= 0) n->owner->dom_revision++;
    return JS_UNDEFINED;
}
/* T.insert(parent, child, index or -1, quiet). */
static JSValue t_insert(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    node_t *p = arg_node(ctx, argv[0]), *c = p ? arg_node(ctx, argv[1]) : NULL;
    int32_t at;
    if (!c || JS_ToInt32(ctx, &at, argv[2]) < 0) return JS_EXCEPTION;
    detach(c);
    if (p->nchildren == p->cchildren) {
        p->cchildren = p->cchildren ? p->cchildren * 2 : 8;
        p->children = realloc(p->children, p->cchildren * sizeof *p->children);
    }
    if (at < 0 || at > p->nchildren) at = p->nchildren;
    memmove(p->children + at + 1, p->children + at, (p->nchildren - at) * sizeof *p->children);
    p->children[at] = c; p->nchildren++; c->parent = p;
    if (JS_ToBool(ctx, argv[3]) <= 0) p->owner->dom_revision++;
    return JS_UNDEFINED;
}
/* T.remove(child, quiet). */
static JSValue t_remove(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    node_t *c = arg_node(ctx, argv[0]);
    if (!c) return JS_EXCEPTION;
    node_t *o = c->parent;
    detach(c);
    if (o && JS_ToBool(ctx, argv[1]) <= 0) o->owner->dom_revision++;
    return JS_UNDEFINED;
}
/* T.adopt(node, document): the node's owner changes and neither revision. */
static JSValue t_adopt(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    node_t *n = arg_node(ctx, argv[0]);
    web_doc *d = n ? arg_doc(ctx, argv[1]) : NULL;
    if (!d) return JS_EXCEPTION;
    n->owner = d;
    return JS_UNDEFINED;
}
/* T.revision(document) and T.setRevision(document, bigint). */
static JSValue t_revision(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    web_doc *d = arg_doc(ctx, argv[0]);
    return d ? JS_NewBigUint64(ctx, d->dom_revision) : JS_EXCEPTION;
}
static JSValue t_set_revision(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    web_doc *d = arg_doc(ctx, argv[0]);
    int64_t value;
    if (!d || JS_ToBigInt64(ctx, &value, argv[1]) < 0) return JS_EXCEPTION;
    d->dom_revision = (uint64_t)value;
    return JS_UNDEFINED;
}
/* T.childReads() and T.attrReads(): the native reads made so far. */
static JSValue t_child_reads(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_NewFloat64(ctx, (double)child_reads);
}
static JSValue t_attr_reads(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_NewFloat64(ctx, (double)attr_reads);
}
/* rawDom.get(node, key) for the keys js_collections.js reads, native like DOM_get. */
static JSValue t_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    node_t *n = arg_node(ctx, argv[0]);
    if (!n) return JS_EXCEPTION;
    const char *key = JS_ToCString(ctx, argv[1]);
    if (!key) return JS_EXCEPTION;
    JSValue result;
    if (!strcmp(key, "nodeType")) result = JS_NewInt32(ctx, n->type);
    else if (!strcmp(key, "childNodes")) {
        child_reads++;
        result = JS_NewArray(ctx);
        for (int i = 0; i < n->nchildren; i++)
            JS_DefinePropertyValueUint32(ctx, result, i, JS_DupValue(ctx, n->children[i]->wrapper), JS_PROP_C_W_E);
    } else result = JS_ThrowTypeError(ctx, "unexpected rawDom.get %s", key);
    JS_FreeCString(ctx, key);
    return result;
}
/* rawDom.attrNS(element, null, name): the attribute's value or null. */
static JSValue t_attr_ns(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc;
    node_t *n = arg_node(ctx, argv[0]);
    if (!n) return JS_EXCEPTION;
    const char *name = JS_ToCString(ctx, argv[2]);
    if (!name) return JS_EXCEPTION;
    int i = doc_attr_index(n, NULL, name, true);
    JS_FreeCString(ctx, name);
    return i >= 0 && n->attrs[i].value ? JS_NewString(ctx, n->attrs[i].value) : JS_NULL;
}
static JSValue t_print(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    for (int i = 0; i < argc; i++) { const char *s = JS_ToCString(ctx, argv[i]); fputs(s ? s : "?", stdout); JS_FreeCString(ctx, s); }
    fputc('\n', stdout); fflush(stdout);
    return JS_UNDEFINED;
}
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1);
    if (fread(b, 1, n, f) != (size_t)n) { perror(path); exit(2); }
    b[n] = 0; fclose(f);
    return b;
}
static void report_exception(JSContext *ctx) {
    JSValue e = JS_GetException(ctx);
    const char *s = JS_ToCString(ctx, e);
    printf("EXCEPTION %s\n", s ? s : "?"); JS_FreeCString(ctx, s);
    JSValue st = JS_GetPropertyStr(ctx, e, "stack");
    const char *t = JS_ToCString(ctx, st);
    if (t) printf("%s\n", t);
    JS_FreeCString(ctx, t); JS_FreeValue(ctx, st); JS_FreeValue(ctx, e);
}
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: collections_native_cases <js_collections.js> <cases.js>\n"); return 2; }
    JSRuntime *rt = JS_NewRuntime();
    JS_NewClassID(&node_class);
    JSClassDef node_def = {.class_name = "Node"};
    JS_NewClass(rt, node_class, &node_def);
    JSContext *ctx = the_ctx = JS_NewContext(rt);
    JS_SetContextOpaque(ctx, &the_state);
    JSValue global = JS_GetGlobalObject(ctx), tree = JS_NewObject(ctx), host = JS_NewObject(ctx);
    static const JSCFunctionListEntry functions[] = {
        JS_CFUNC_DEF("doc", 0, t_doc), JS_CFUNC_DEF("element", 2, t_element), JS_CFUNC_DEF("text", 1, t_text),
        JS_CFUNC_DEF("attr", 4, t_attr), JS_CFUNC_DEF("insert", 4, t_insert), JS_CFUNC_DEF("remove", 2, t_remove),
        JS_CFUNC_DEF("adopt", 2, t_adopt), JS_CFUNC_DEF("revision", 1, t_revision), JS_CFUNC_DEF("setRevision", 2, t_set_revision),
        JS_CFUNC_DEF("childReads", 0, t_child_reads), JS_CFUNC_DEF("attrReads", 0, t_attr_reads),
        JS_CFUNC_DEF("get", 2, t_get), JS_CFUNC_DEF("attrNS", 3, t_attr_ns), JS_CFUNC_DEF("print", 1, t_print),
    };
    JS_SetPropertyFunctionList(ctx, tree, functions, sizeof functions / sizeof *functions);
    if (collections_native_init(ctx, host) < 0) { report_exception(ctx); return 1; }
    JS_SetPropertyStr(ctx, global, "T", tree);
    JS_SetPropertyStr(ctx, global, "__host", host);
    JS_FreeValue(ctx, global);
    /* js_collections.js runs as js_bootstrap.js includes it: host, rawDom and
       reflectedAttr in scope, in strict code. */
    char *collections = read_file(argv[1]);
    static const char head[] = "(function(host){'use strict';const rawDom={get:T.get,attrNS:T.attrNS};"
                               "function reflectedAttr(node,name){return rawDom.attrNS(node,null,name);}\n";
    static const char tail[] = "\n;globalThis.collectionBridge=collectionBridge;})(__host);";
    size_t length = strlen(head) + strlen(collections) + strlen(tail) + 1;
    char *source = malloc(length);
    snprintf(source, length, "%s%s%s", head, collections, tail);
    JSValue r = JS_Eval(ctx, source, strlen(source), "js_collections.js", JS_EVAL_TYPE_GLOBAL);
    int status = 0;
    if (JS_IsException(r)) { report_exception(ctx); status = 1; }
    JS_FreeValue(ctx, r);
    if (!status) {
        char *cases = read_file(argv[2]);
        r = JS_Eval(ctx, cases, strlen(cases), "collections_native_cases.js", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(r)) { report_exception(ctx); status = 1; }
        else if (JS_ToInt32(ctx, &status, r) < 0) status = 1;
        JS_FreeValue(ctx, r);
        free(cases);
    }
    free(collections); free(source);
    /* The tree lets its wrappers go; the runtime's own check that no object
       is left (JS_FreeRuntime asserts it) then covers the collections'
       finalizer and gc_mark. */
    for (node_t *n = all_nodes; n; n = n->all_next) { JS_FreeValue(ctx, n->wrapper); n->wrapper = JS_UNDEFINED; }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    printf("collections native: runtime freed\n");
    return status;
}
