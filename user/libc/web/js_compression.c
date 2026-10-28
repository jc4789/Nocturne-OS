/* The Compression Standard's compression contexts (js_compression.h) over libc's
   codecs: DEFLATE with its zlib and gzip wrappers (deflate.c) and brotli
   (brotli_codec.h). Decompression is as strict as the standard's "Supported
   formats": one gzip member, a zlib header without a preset dictionary, nothing
   after the end. */
#include "js_compression.h"
#include "deflate.h"
#include "../brotli_codec.h"
#include <stdlib.h>
#include <string.h>

/* A call takes at most STEP_INPUT bytes and stops once STEP_OUTPUT bytes have come
   out: the script calls again, so a large chunk, or a small one that expands a lot,
   leaves interrupt points (Esc, reload, the task watchdog) and the native buffer
   stays bounded. Output reaches the script in ArrayBuffers of at most PIECE bytes. */
#define STEP_INPUT (1u << 20)
#define STEP_OUTPUT (1u << 20)
#define PIECE (64u * 1024u)
/* The middle points of both encoders: zlib's default level, and the brotli
   quality servers use for content compressed as it is sent. */
#define DEFLATE_LEVEL 6
#define BROTLI_QUALITY 6

/* CompressionFormat in IDL order */
enum { FORMAT_BROTLI, FORMAT_DEFLATE, FORMAT_DEFLATE_RAW, FORMAT_GZIP, FORMATS };
enum { STATUS_INVALID = -1, STATUS_MORE = 0, STATUS_END = 1, STATUS_STEP = 2 };

struct context {
    unsigned format;
    bool decompress;
    bool ended;   /* decompression: the compressed data ended; compression: finished */
    bool invalid;
    bool oom;     /* the output could not grow during this call */
    struct deflate_stream *deflate;
    struct inflate_stream *inflate;
    void *brotli; /* the encoder, or the decoder when decompressing */
    struct deflate_out out; /* this call's output */
};

static JSClassID context_class;

static void context_release(struct context *c) {
    if (c->deflate) deflate_end(c->deflate);
    if (c->inflate) inflate_end(c->inflate);
    if (c->brotli) (c->decompress ? brotli_decoder_end : brotli_encoder_end)(c->brotli);
    c->deflate = NULL;
    c->inflate = NULL;
    c->brotli = NULL;
    free(c->out.p);
    c->out = (struct deflate_out){.grow = true};
}

static void context_finalizer(JSRuntime *rt, JSValue value) {
    (void)rt;
    struct context *c = JS_GetOpaque(value, context_class);
    if (!c) return;
    context_release(c);
    free(c);
}

/* room for at least PIECE more bytes */
static bool reserve(struct context *c) {
    if (c->out.cap - c->out.len >= PIECE) return true;
    size_t cap = c->out.cap > PIECE ? c->out.cap * 2 : 2 * PIECE;
    uint8_t *p = realloc(c->out.p, cap);
    if (!p) {
        c->oom = true;
        return false;
    }
    c->out.p = p;
    c->out.cap = cap;
    return true;
}

static bool inflated(void *context, const uint8_t *bytes, size_t n) {
    struct context *c = context;
    if (c->out.cap - c->out.len < n) {
        size_t cap = c->out.cap ? c->out.cap : 2 * PIECE;
        while (cap - c->out.len < n) cap *= 2;
        uint8_t *p = realloc(c->out.p, cap);
        if (!p) {
            c->oom = true;
            return false;
        }
        c->out.p = p;
        c->out.cap = cap;
    }
    memcpy(c->out.p + c->out.len, bytes, n);
    c->out.len += n;
    return c->out.len < STEP_OUTPUT;
}

static JSValue context_new(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    int32_t format;
    if (argc < 2 || JS_ToInt32(ctx, &format, argv[0]) < 0) return JS_EXCEPTION;
    if (format < 0 || format >= FORMATS) return JS_ThrowTypeError(ctx, "Unsupported compression format");
    bool decompress = JS_ToBool(ctx, argv[1]) > 0;
    struct context *c = calloc(1, sizeof *c);
    if (!c) return JS_ThrowOutOfMemory(ctx);
    c->format = (unsigned)format;
    c->decompress = decompress;
    c->out.grow = true;
    enum deflate_wrap wrap = format == FORMAT_GZIP ? DEFLATE_GZIP : format == FORMAT_DEFLATE ? DEFLATE_ZLIB : DEFLATE_RAW;
    if (format == FORMAT_BROTLI) c->brotli = decompress ? brotli_decoder_begin() : brotli_encoder_begin(BROTLI_QUALITY);
    else if (decompress) c->inflate = inflate_begin(wrap, 0, inflated, c);
    else c->deflate = deflate_begin(DEFLATE_LEVEL, wrap);
    if (!c->brotli && !c->inflate && !c->deflate) {
        free(c);
        return JS_ThrowOutOfMemory(ctx);
    }
    JSValue token = JS_NewObjectClass(ctx, (int)context_class);
    if (JS_IsException(token)) {
        context_release(c);
        free(c);
        return token;
    }
    JS_SetOpaque(token, c);
    return token;
}

static int step_inflate(struct context *c, const uint8_t *in, size_t take, bool last, size_t *used) {
    switch (inflate_feed(c->inflate, in, take, last, used)) {
    case INFLATE_MORE: return STATUS_MORE;
    case INFLATE_END: return STATUS_END;
    case INFLATE_STOPPED: return STATUS_STEP;
    default: return STATUS_INVALID;
    }
}

static int step_brotli_decode(struct context *c, const uint8_t *in, size_t take, size_t *used) {
    static const uint8_t none;
    const uint8_t *p = in ? in : &none;
    size_t left = take;
    for (;;) {
        if (!reserve(c)) return STATUS_INVALID;
        uint8_t *o = c->out.p + c->out.len;
        size_t room = c->out.cap - c->out.len;
        int result = brotli_decode(c->brotli, &p, &left, &o, &room);
        c->out.len = (size_t)(o - c->out.p);
        *used = take - left;
        if (result < 0) return STATUS_INVALID;
        if (result > 0) return STATUS_END;
        if (room) return STATUS_MORE;  /* every byte taken */
        if (c->out.len >= STEP_OUTPUT) return STATUS_STEP;
    }
}

static int step_brotli_encode(struct context *c, const uint8_t *in, size_t take, bool last, size_t *used) {
    static const uint8_t none;
    const uint8_t *p = in ? in : &none;
    size_t left = take;
    for (;;) {
        if (!reserve(c)) return STATUS_INVALID;
        uint8_t *o = c->out.p + c->out.len;
        size_t room = c->out.cap - c->out.len, before = left, space = room;
        int result = brotli_encode(c->brotli, &p, &left, &o, &room, last);
        c->out.len = (size_t)(o - c->out.p);
        *used = take - left;
        if (result < 0) return STATUS_INVALID;
        if (result > 0) return last ? STATUS_END : STATUS_MORE;
        if (c->out.len >= STEP_OUTPUT) return STATUS_STEP;
        if (room && left == before && room == space) return STATUS_INVALID;  /* no progress */
    }
}

static JSValue context_feed(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    if (argc < 5) return JS_ThrowTypeError(ctx, "compressionFeed requires five arguments");
    struct context *c = JS_GetOpaque2(ctx, argv[0], context_class);
    if (!c) return JS_EXCEPTION;
    uint64_t offset, length;
    if (JS_ToIndex(ctx, &offset, argv[2]) < 0 || JS_ToIndex(ctx, &length, argv[3]) < 0) return JS_EXCEPTION;
    bool finish = JS_ToBool(ctx, argv[4]) > 0;
    const uint8_t *in = NULL;
    if (length) {
        size_t size;
        uint8_t *data = JS_GetArrayBuffer(ctx, &size, argv[1]);
        if (!data) return JS_EXCEPTION;
        if (offset > size || length > size - offset) return JS_ThrowRangeError(ctx, "Input outside its buffer");
        in = data + offset;
    }
    size_t take = length < STEP_INPUT ? (size_t)length : STEP_INPUT, used = 0;
    bool last = finish && take == length;
    int status;
    c->oom = false;
    c->out.len = 0;
    if (c->invalid) {
        status = STATUS_INVALID;
    } else if (c->ended) {
        status = STATUS_END;  /* nothing more is taken */
    } else if (c->inflate) {
        status = step_inflate(c, in, take, last, &used);
    } else if (c->brotli) {
        status = c->decompress ? step_brotli_decode(c, in, take, &used) : step_brotli_encode(c, in, take, last, &used);
    } else if (c->deflate) {
        if (deflate_feed(c->deflate, in, take, last, &c->out) < 0) c->oom = true;
        used = take;
        status = last ? STATUS_END : STATUS_MORE;
    } else {
        status = STATUS_INVALID;
    }
    if (c->oom) {
        context_release(c);
        c->invalid = true;
        return JS_ThrowOutOfMemory(ctx);
    }
    if (status == STATUS_MORE && take < length) status = STATUS_STEP;
    if (status == STATUS_INVALID) c->out.len = 0;  /* the standard throws before enqueuing */
    JSValue result = JS_NewArray(ctx);
    if (JS_IsException(result)) goto done;
    if (JS_DefinePropertyValueUint32(ctx, result, 0, JS_NewInt32(ctx, status), JS_PROP_C_W_E) < 0 ||
        JS_DefinePropertyValueUint32(ctx, result, 1, JS_NewInt64(ctx, (int64_t)used), JS_PROP_C_W_E) < 0)
        goto failed;
    uint32_t index = 2;
    for (size_t at = 0; at < c->out.len; at += PIECE) {
        size_t n = c->out.len - at < PIECE ? c->out.len - at : PIECE;
        JSValue piece = JS_NewArrayBufferCopy(ctx, c->out.p + at, n);
        if (JS_IsException(piece) || JS_DefinePropertyValueUint32(ctx, result, index++, piece, JS_PROP_C_W_E) < 0)
            goto failed;
    }
    goto done;
failed:
    JS_FreeValue(ctx, result);
    result = JS_EXCEPTION;
done:
    /* A context that is done gives its memory back now; one that goes on keeps none
       of this call's output. */
    if (status == STATUS_INVALID || status == STATUS_END || JS_IsException(result)) {
        context_release(c);
        if (status == STATUS_END) c->ended = true;
        else c->invalid = true;
    } else {
        free(c->out.p);
        c->out = (struct deflate_out){.grow = true};
    }
    return result;
}

static JSValue context_close(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    struct context *c = argc ? JS_GetOpaque(argv[0], context_class) : NULL;
    if (c) {
        context_release(c);
        c->invalid = true;
    }
    (void)ctx;
    return JS_UNDEFINED;
}

void web_js_compression_init(JSContext *ctx, JSValue host) {
    static const JSCFunctionListEntry functions[] = {
        JS_CFUNC_DEF("compressionNew", 2, context_new),
        JS_CFUNC_DEF("compressionFeed", 5, context_feed),
        JS_CFUNC_DEF("compressionEnd", 1, context_close),
    };
    JSRuntime *rt = JS_GetRuntime(ctx);
    if (!context_class) JS_NewClassID(&context_class);
    if (!JS_IsRegisteredClass(rt, context_class)) {
        JSClassDef definition = {.class_name = "NocturneCompression", .finalizer = context_finalizer};
        if (JS_NewClass(rt, context_class, &definition) < 0) return;
    }
    JS_SetPropertyFunctionList(ctx, host, functions, sizeof functions / sizeof *functions);
}
