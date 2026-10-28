/* DOM XPath's native half, included by js.c: a compiled XPath expression is a
   native object owned by js_xpath.js's XPathExpression, and DOM_xpath
   evaluates one against a context node (xpath.c). js_xpath.js owns the Web IDL
   conversions, the XPathNSResolver callback and XPathResult's state. */
#include "xpath.h"

static JSClassID xpath_expression_class;

static void xpath_expression_finalizer(JSRuntime *rt, JSValue object) {
    (void)rt;
    xpath_free(JS_GetOpaque(object, xpath_expression_class));
}

struct xpath_lookup { JSContext *ctx; JSValueConst lookup; };

/* js_xpath.js's lookup returns the namespace for a prefix, or null when the
   resolver binds none (it reports a resolver's own exception itself). */
static int xpath_resolve(void *opaque, const char *prefix, size_t length, char **uri, size_t *uri_length) {
    struct xpath_lookup *l = opaque;
    JSValue argument = JS_NewStringLen(l->ctx, prefix, length);
    if (JS_IsException(argument)) return -1;
    JSValue result = JS_Call(l->ctx, l->lookup, JS_UNDEFINED, 1, (JSValueConst *)&argument);
    JS_FreeValue(l->ctx, argument);
    if (JS_IsException(result)) return -1;
    if (JS_IsNull(result) || JS_IsUndefined(result)) return 0;
    size_t n = 0;
    const char *text = JS_ToCStringLen(l->ctx, &n, result);
    JS_FreeValue(l->ctx, result);
    if (!text) return -1;
    char *copy = n < SIZE_MAX ? malloc(n + 1) : NULL;
    if (!copy) { JS_FreeCString(l->ctx, text); JS_ThrowOutOfMemory(l->ctx); return -1; }
    memcpy(copy, text, n);
    copy[n] = 0;
    JS_FreeCString(l->ctx, text);
    *uri = copy;
    *uri_length = n;
    return 1;
}

/* host.xpathCompile(expression, lookup | null): the compiled expression, or
   [status, detail] for an expression the DOM rejects (xpath.h's statuses). */
static JSValue native_xpath_compile(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    struct web_js_state *s = state(ctx);
    if (argc < 2 || (!JS_IsNull(argv[1]) && !JS_IsFunction(ctx, argv[1])))
        return JS_ThrowTypeError(ctx, "XPath compilation requires an expression and a lookup");
    if (!xpath_expression_class) JS_NewClassID(&xpath_expression_class);
    if (!JS_IsRegisteredClass(s->rt, xpath_expression_class)) {
        JSClassDef definition = {.class_name = "NocturneXPathExpression", .finalizer = xpath_expression_finalizer};
        if (JS_NewClass(s->rt, xpath_expression_class, &definition) < 0) return oom(ctx);
    }
    size_t length = 0;
    const char *source = JS_ToCStringLen(ctx, &length, argv[0]);
    if (!source) return JS_EXCEPTION;
    struct xpath_lookup lookup = {ctx, argv[1]};
    struct xpath_expr *expr = NULL;
    char detail[160];
    enum xpath_status status = xpath_compile(source, length, JS_IsNull(argv[1]) ? NULL : xpath_resolve, &lookup,
                                             &expr, detail, sizeof detail);
    JS_FreeCString(ctx, source);
    switch (status) {
    case XPATH_OK: break;
    case XPATH_EXCEPTION: return JS_EXCEPTION;
    case XPATH_OOM: return oom(ctx);
    case XPATH_DEPTH: return JS_ThrowInternalError(ctx, "The XPath expression is nested too deeply");
    default: {
        JSValue failure = JS_NewArray(ctx);
        if (JS_IsException(failure)) return failure;
        if (JS_DefinePropertyValueUint32(ctx, failure, 0, JS_NewInt32(ctx, status), JS_PROP_C_W_E) < 0 ||
            JS_DefinePropertyValueUint32(ctx, failure, 1, JS_NewString(ctx, detail), JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, failure);
            return JS_EXCEPTION;
        }
        return failure;
    }
    }
    JSValue object = JS_NewObjectClass(ctx, xpath_expression_class);
    if (JS_IsException(object)) { xpath_free(expr); return object; }
    JS_SetOpaque(object, expr);
    return object;
}

/* DOM_xpath(context, expression, type): [resultType, value] with value a
   number, string, boolean or an Array of Nodes in document order; an Int32
   status (xpath.h) for a context node or result type the DOM rejects. */
static JSValue xpath_dom(struct web_js_state *s, node_t *context, int argc, JSValueConst *argv) {
    JSContext *ctx = s->ctx;
    struct xpath_expr *expr = argc > 2 && xpath_expression_class ? JS_GetOpaque(argv[2], xpath_expression_class) : NULL;
    if (!expr) return JS_ThrowTypeError(ctx, "Compiled XPath expression required");
    int32_t type = 0;
    if (argc > 3 && JS_ToInt32(ctx, &type, argv[3]) < 0) return JS_EXCEPTION;
    struct xpath_result result;
    enum xpath_status status = xpath_evaluate(expr, context, type < 0 ? UINT32_MAX : (unsigned)type, &result);
    switch (status) {
    case XPATH_OK: break;
    case XPATH_OOM: return oom(ctx);
    case XPATH_CANCELLED: return JS_ThrowInternalError(ctx, "XPath evaluation interrupted");
    case XPATH_DEPTH: return JS_ThrowInternalError(ctx, "The XPath expression is nested too deeply");
    default: return JS_NewInt32(ctx, status);
    }
    JSValue value;
    switch (result.type) {
    case XPATH_NUMBER_TYPE: value = JS_NewFloat64(ctx, result.number); break;
    case XPATH_STRING_TYPE: value = JS_NewStringLen(ctx, result.string ? result.string : "", result.string_length); break;
    case XPATH_BOOLEAN_TYPE: value = JS_NewBool(ctx, result.boolean); break;
    default:
        value = JS_NewArray(ctx);
        for (size_t i = 0; !JS_IsException(value) && i < result.count; i++) {
            JSValue node = wrap(s, result.nodes[i]);
            if (JS_IsException(node) || JS_DefinePropertyValueUint32(ctx, value, (uint32_t)i, node, JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, value);
                value = JS_EXCEPTION;
            }
        }
    }
    unsigned resolved = result.type;
    xpath_result_free(&result);
    if (JS_IsException(value)) return value;
    JSValue pair = JS_NewArray(ctx);
    if (JS_IsException(pair)) { JS_FreeValue(ctx, value); return pair; }
    if (JS_DefinePropertyValueUint32(ctx, pair, 0, JS_NewInt32(ctx, (int32_t)resolved), JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, value);
        JS_FreeValue(ctx, pair);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueUint32(ctx, pair, 1, value, JS_PROP_C_W_E) < 0) {
        JS_FreeValue(ctx, pair);
        return JS_EXCEPTION;
    }
    return pair;
}
