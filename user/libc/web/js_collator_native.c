#include "js_collator_native.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* This is a table-format decoder, not a locale implementation or a watchdog
 * exemption. Both native loops are bounded by the private per-table 4 MiB
 * input limit. All owned bytes use QuickJS allocation/accounting and follow
 * the ArrayBuffer through detach, GC, context destruction and OOM paths. */
#define COLLATION_PACKED_LIMIT (4u * 1024u * 1024u)

/* value+1 makes uninitialized entries invalid, including padding '='. */
static const uint8_t base64[128] = {
    ['A']=1, ['B']=2, ['C']=3, ['D']=4, ['E']=5, ['F']=6, ['G']=7, ['H']=8,
    ['I']=9, ['J']=10, ['K']=11, ['L']=12, ['M']=13, ['N']=14, ['O']=15, ['P']=16,
    ['Q']=17, ['R']=18, ['S']=19, ['T']=20, ['U']=21, ['V']=22, ['W']=23, ['X']=24,
    ['Y']=25, ['Z']=26,
    ['a']=27, ['b']=28, ['c']=29, ['d']=30, ['e']=31, ['f']=32, ['g']=33, ['h']=34,
    ['i']=35, ['j']=36, ['k']=37, ['l']=38, ['m']=39, ['n']=40, ['o']=41, ['p']=42,
    ['q']=43, ['r']=44, ['s']=45, ['t']=46, ['u']=47, ['v']=48, ['w']=49, ['x']=50,
    ['y']=51, ['z']=52,
    ['0']=53, ['1']=54, ['2']=55, ['3']=56, ['4']=57, ['5']=58, ['6']=59,
    ['7']=60, ['8']=61, ['9']=62, ['+']=63, ['/']=64
};

struct collation_decoder {
    uint32_t *output;
    size_t count;
    uint32_t value;
    unsigned shift;
};

static inline bool consume(struct collation_decoder *s, uint8_t byte) {
    /* A uint32 LEB128 has at most five bytes and no overlong zero ending. */
    if (s->shift == 28 && (byte & 0xf0)) return false;
    s->value |= (uint32_t)(byte & 127) << s->shift;
    if (byte & 128) {
        s->shift += 7;
    } else {
        if (s->shift && !(byte & 127)) return false;
        if (s->output) s->output[s->count] = s->value;
        s->count++;
        s->value = 0;
        s->shift = 0;
    }
    return true;
}

static bool decode(const uint8_t *text, size_t length, uint32_t *output, size_t *count) {
    struct collation_decoder state = {.output = output};
    if (length & 3) return false;
    for (size_t i = 0; i < length; i += 4) {
        unsigned a = text[i], b = text[i + 1], c = text[i + 2], d = text[i + 3];
        if ((a | b | c | d) > 127 || !base64[a] || !base64[b]) return false;
        a = base64[a] - 1; b = base64[b] - 1;
        if (!consume(&state, (uint8_t)((a << 2) | (b >> 4)))) return false;
        if (c == '=') {
            if (i + 4 != length || d != '=' || (b & 15)) return false;
            break;
        }
        if (!base64[c]) return false;
        c = base64[c] - 1;
        if (!consume(&state, (uint8_t)((b << 4) | (c >> 2)))) return false;
        if (d == '=') {
            if (i + 4 != length || (c & 3)) return false;
            break;
        }
        if (!base64[d]) return false;
        d = base64[d] - 1;
        if (!consume(&state, (uint8_t)((c << 6) | d))) return false;
    }
    if (state.shift) return false;
    *count = state.count;
    return true;
}

static void collation_buffer_free(JSRuntime *rt, void *opaque, void *ptr) {
    (void)opaque;
    js_free_rt(rt, ptr);
}

static JSValue collation_unpack(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Packed collation data must be a string");
    size_t length = 0;
    const char *text = JS_ToCStringLen(ctx, &length, argv[0]);
    if (!text) return JS_EXCEPTION;
    if (length > COLLATION_PACKED_LIMIT) {
        JS_FreeCString(ctx, text);
        return JS_ThrowRangeError(ctx, "Packed collation table exceeds 4 MiB");
    }
    size_t count = 0;
    if (!decode((const uint8_t *)text, length, NULL, &count)) {
        JS_FreeCString(ctx, text);
        return JS_ThrowTypeError(ctx, "Invalid packed collation data");
    }
    if (!count) {
        JS_FreeCString(ctx, text);
        return JS_NewArrayBufferCopy(ctx, NULL, 0);
    }
    /* count <= 3*length/4, with length capped at 4 MiB: no size overflow. */
    uint32_t *output = js_malloc(ctx, count * sizeof(*output));
    if (!output) { JS_FreeCString(ctx, text); return JS_EXCEPTION; }
    size_t written = 0;
    bool valid = decode((const uint8_t *)text, length, output, &written);
    JS_FreeCString(ctx, text);
    if (!valid || written != count) {
        js_free(ctx, output);
        return JS_ThrowInternalError(ctx, "Collation data changed during decode");
    }
    JSValue result = JS_NewArrayBuffer(ctx, (uint8_t *)output, count * sizeof(*output),
                                       collation_buffer_free, NULL, 0);
    /* QuickJS does not take external-buffer ownership on construction failure. */
    if (JS_IsException(result)) js_free(ctx, output);
    return result;
}

void web_js_collator_init(JSContext *ctx, JSValue host) {
    static const JSCFunctionListEntry functions[] = {
        JS_CFUNC_DEF("collationUnpack", 1, collation_unpack),
    };
    JS_SetPropertyFunctionList(ctx, host, functions, sizeof functions / sizeof functions[0]);
}
