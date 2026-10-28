/* URL and URLSearchParams' native half (js_url.h) over web_url.c. */
#include "js_url.h"
#include "js_encoding.h"
#include "web_url.h"

static JSClassID parser_class;

static void parser_finalizer(JSRuntime *rt, JSValue holder) {
    (void)rt;
    web_url_parser_free(JS_GetOpaque(holder, parser_class));
}

/* The realm's parser, made when it is first needed. */
static struct web_url_parser *realm_parser(JSContext *ctx, JSValueConst holder) {
    struct web_url_parser *parser = JS_GetOpaque(holder, parser_class);
    if (!parser && (parser = web_url_parser_new())) JS_SetOpaque(holder, parser);
    if (!parser) JS_ThrowOutOfMemory(ctx);
    return parser;
}

static JSValue record(JSContext *ctx, int result, struct web_url *url) {
    if (result != 1) {
        web_url_free(url);
        return result < 0 ? JS_ThrowOutOfMemory(ctx) : JS_NULL;
    }
    JSValue href = JS_NewStringLen(ctx, url->href ? url->href : "", url->length);
    size_t mark[WEB_URL_MARKS];
    memcpy(mark, url->mark, sizeof mark);
    web_url_free(url);
    if (JS_IsException(href)) return href;
    JSValue array = JS_NewArray(ctx);
    if (JS_IsException(array)) { JS_FreeValue(ctx, href); return array; }
    if (JS_DefinePropertyValueUint32(ctx, array, 0, href, JS_PROP_C_W_E) < 0) goto failed;
    for (int i = 0; i < WEB_URL_MARKS; i++)
        if (JS_DefinePropertyValueUint32(ctx, array, (uint32_t)i + 1, JS_NewInt32(ctx, (int32_t)mark[i]),
                                         JS_PROP_C_W_E) < 0) goto failed;
    return array;
failed:
    JS_FreeValue(ctx, array);
    return JS_EXCEPTION;
}

/* An encoding's name for the query (HTML's encoding-parse passes its
   document's character encoding), or undefined for UTF-8. */
static bool encoding_argument(JSContext *ctx, int argc, JSValueConst *argv, int at, web_encoding *encoding) {
    *encoding = WEB_ENCODING_UTF8;
    if (argc <= at || JS_IsUndefined(argv[at])) return true;
    size_t length;
    const char *name = JS_ToCStringLen(ctx, &length, argv[at]);
    if (!name) return false;
    web_encoding found = web_encoding_get(name, length);
    JS_FreeCString(ctx, name);
    if (found) *encoding = found;
    return true;
}

static JSValue native_parse(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic, JSValue *data) {
    (void)self; (void)magic;
    struct web_url_parser *parser = realm_parser(ctx, data[0]);
    web_encoding encoding;
    if (!parser || !encoding_argument(ctx, argc, argv, 2, &encoding)) return JS_EXCEPTION;
    size_t length, base_length = 0;
    const char *input = JS_ToCStringLen(ctx, &length, argv[0]), *base = NULL;
    if (!input) return JS_EXCEPTION;
    if (!JS_IsUndefined(argv[1]) && !(base = JS_ToCStringLen(ctx, &base_length, argv[1]))) {
        JS_FreeCString(ctx, input);
        return JS_EXCEPTION;
    }
    struct web_url url = {0};
    int result = web_url_parse(parser, input, length, base, base_length, encoding, &url);
    JS_FreeCString(ctx, input);
    JS_FreeCString(ctx, base);
    return record(ctx, result, &url);
}

static JSValue native_set(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv, int magic, JSValue *data) {
    (void)self; (void)magic;
    struct web_url_parser *parser = realm_parser(ctx, data[0]);
    int32_t part;
    web_encoding encoding;
    if (!parser || JS_ToInt32(ctx, &part, argv[1]) < 0 || !encoding_argument(ctx, argc, argv, 3, &encoding))
        return JS_EXCEPTION;
    if (part < WEB_URL_PROTOCOL || part > WEB_URL_HASH) return JS_ThrowRangeError(ctx, "Unknown URL part");
    size_t href_length, length;
    const char *href = JS_ToCStringLen(ctx, &href_length, argv[0]);
    const char *value = href ? JS_ToCStringLen(ctx, &length, argv[2]) : NULL;
    if (!value) {
        JS_FreeCString(ctx, href);
        return JS_EXCEPTION;
    }
    struct web_url url = {0};
    int result = web_url_set(parser, href, href_length, (enum web_url_part)part, value, length, encoding, &url);
    JS_FreeCString(ctx, href);
    JS_FreeCString(ctx, value);
    return record(ctx, result, &url);
}

struct form_pairs { JSContext *ctx; JSValue list; uint32_t count; };

static bool form_string(struct form_pairs *pairs, const char *bytes, size_t length) {
    JSValue text = web_js_utf8_text(pairs->ctx, (const uint8_t *)bytes, length);
    return !JS_IsException(text) &&
           JS_DefinePropertyValueUint32(pairs->ctx, pairs->list, pairs->count++, text, JS_PROP_C_W_E) >= 0;
}

static bool form_pair(void *opaque, const char *name, size_t name_length, const char *value, size_t value_length) {
    return form_string(opaque, name, name_length) && form_string(opaque, value, value_length);
}

static JSValue form_parse(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self; (void)argc;
    size_t length;
    const char *input = JS_ToCStringLen(ctx, &length, argv[0]);
    if (!input) return JS_EXCEPTION;
    struct form_pairs pairs = {ctx, JS_NewArray(ctx), 0};
    int result = JS_IsException(pairs.list) ? 0 : web_url_form_parse(input, length, form_pair, &pairs);
    JS_FreeCString(ctx, input);
    if (result == 1) return pairs.list;
    JS_FreeValue(ctx, pairs.list);
    return result < 0 ? JS_ThrowOutOfMemory(ctx) : JS_EXCEPTION;
}

static JSValue form_serialize(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self; (void)argc;
    int64_t count;
    JSValue length = JS_GetPropertyStr(ctx, argv[0], "length");
    int status = JS_ToInt64(ctx, &count, length);
    JS_FreeValue(ctx, length);
    if (status < 0) return JS_EXCEPTION;
    sbuf out = {0};
    for (int64_t i = 0; i < count; i++) {
        size_t n;
        JSValue item = JS_GetPropertyUint32(ctx, argv[0], (uint32_t)i);
        const char *text = JS_IsException(item) ? NULL : JS_ToCStringLen(ctx, &n, item);
        JS_FreeValue(ctx, item);
        if (!text) { sb_free(&out); return JS_EXCEPTION; }
        if (i) sb_putc(&out, i & 1 ? '=' : '&');
        url_encode_form(&out, text, n);
        JS_FreeCString(ctx, text);
    }
    JSValue result = JS_NewStringLen(ctx, out.p ? out.p : "", out.n);
    sb_free(&out);
    return result;
}

void web_js_url_init(JSContext *ctx, JSValue host) {
    static const JSCFunctionListEntry functions[] = {
        JS_CFUNC_DEF("urlFormParse", 1, form_parse),
        JS_CFUNC_DEF("urlFormSerialize", 1, form_serialize),
    };
    JSRuntime *rt = JS_GetRuntime(ctx);
    if (!parser_class) JS_NewClassID(&parser_class);
    if (!JS_IsRegisteredClass(rt, parser_class)) {
        JSClassDef definition = {.class_name = "NocturneURLParser", .finalizer = parser_finalizer};
        if (JS_NewClass(rt, parser_class, &definition) < 0) return;
    }
    JSValue holder = JS_NewObjectClass(ctx, (int)parser_class);
    if (JS_IsException(holder)) return;
    JS_SetPropertyStr(ctx, host, "urlParse", JS_NewCFunctionData(ctx, native_parse, 3, 0, 1, &holder));
    JS_SetPropertyStr(ctx, host, "urlSet", JS_NewCFunctionData(ctx, native_set, 4, 0, 1, &holder));
    JS_FreeValue(ctx, holder);
    JS_SetPropertyFunctionList(ctx, host, functions, sizeof functions / sizeof *functions);
}
