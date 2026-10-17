/* WebGL 1 and WebGL 2 on Mesa's softpipe rasterizer (ports/mesa/nocturne_gl).
 * The interfaces are native classes shared by the Window realm (canvas
 * elements) and Worker realms (OffscreenCanvas). OpenGL ES reads client
 * memory for vertex arrays without a buffer and for indices without an
 * element buffer, and accepts formats, targets and capabilities WebGL leaves
 * out; every entry point validates those cases and typed-array sizes before
 * calling into GL, so page content only ever reaches its own buffers. */
#include "js_webgl.h"
#include "js_memory.h"
#include "nocturne_gl.h"
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include "util/half_float.h"
#include "util/format_r11g11b10f.h"
#include "util/format_rgb9e5.h"
#include "nocturne.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#define WEBGL_UNPACK_FLIP_Y 0x9240
#define WEBGL_UNPACK_PREMULTIPLY_ALPHA 0x9241
#define WEBGL_CONTEXT_LOST 0x9242
#define WEBGL_UNPACK_COLORSPACE_CONVERSION 0x9243
#define WEBGL_BROWSER_DEFAULT 0x9244
#define WEBGL_UNMASKED_VENDOR 0x9245
#define WEBGL_UNMASKED_RENDERER 0x9246
#define WEBGL_MAX_CLIENT_WAIT_TIMEOUT 0x9247
#define WEBGL_DEPTH_STENCIL 0x84F9
#define WEBGL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define KHR_COMPLETION_STATUS 0x91B1

struct webgl_constant { const char *name; double value; };
#include "js_webgl_constants.inc"

/* ---- wrapper objects ---- */

enum {
    O_BUFFER, O_TEXTURE, O_FRAMEBUFFER, O_RENDERBUFFER, O_PROGRAM, O_SHADER, O_VAO, O_QUERY, O_SAMPLER, O_TF,
    O_TABLED, /* kinds above live in the per-context name tables */
    O_SYNC = O_TABLED, O_LOCATION, O_INFO, O_PRECISION, O_VAO_OES, O_KINDS
};
static const char *const kind_names[O_KINDS] = {
    "WebGLBuffer", "WebGLTexture", "WebGLFramebuffer", "WebGLRenderbuffer", "WebGLProgram", "WebGLShader",
    "WebGLVertexArrayObject", "WebGLQuery", "WebGLSampler", "WebGLTransformFeedback", "WebGLSync",
    "WebGLUniformLocation", "WebGLActiveInfo", "WebGLShaderPrecisionFormat", "WebGLVertexArrayObjectOES",
};
static JSClassID kind_class[O_KINDS], gl1_class, gl2_class, realm_class;

struct webgl;
struct wobj {
    struct webgl *gl;
    unsigned generation;
    uint8_t kind;
    bool deleted;
    uint8_t buffer_kind;  /* buffers: 0 never bound, 1 element array, 2 any other target */
    GLuint name;
    uint64_t id;          /* programs: identity checked by uniform locations */
    unsigned link_serial; /* programs: bumped by every linkProgram */
    char *source;         /* shaders: the source as given to shaderSource */
    GLsync sync;
    uint64_t program;     /* uniform locations */
    GLint location;
    GLenum type;          /* uniform locations and active infos */
    GLint size;           /* active infos */
    char *text;           /* active info names */
    GLint range_min, range_max, precision;
};

/* ---- extensions ---- */

enum {
    X_ANGLE_instanced_arrays, X_EXT_blend_minmax, X_EXT_color_buffer_float, X_EXT_color_buffer_half_float,
    X_EXT_float_blend, X_EXT_frag_depth, X_EXT_sRGB, X_EXT_texture_filter_anisotropic,
    X_KHR_parallel_shader_compile, X_OES_element_index_uint, X_OES_standard_derivatives, X_OES_texture_float,
    X_OES_texture_float_linear, X_OES_texture_half_float, X_OES_texture_half_float_linear,
    X_OES_vertex_array_object, X_WEBGL_color_buffer_float, X_WEBGL_debug_renderer_info, X_WEBGL_depth_texture,
    X_WEBGL_draw_buffers, X_WEBGL_lose_context, X_COUNT
};
struct extension {
    const char *name, *interface;
    uint8_t versions;     /* bit 0: WebGL 1, bit 1: WebGL 2 */
    const char *requires; /* space separated GL extensions, NULL: always */
};
static const struct extension extensions[X_COUNT] = {
    {"ANGLE_instanced_arrays", "ANGLEInstancedArrays", 1, "GL_EXT_instanced_arrays"},
    {"EXT_blend_minmax", "EXTBlendMinMax", 1, "GL_EXT_blend_minmax"},
    {"EXT_color_buffer_float", "EXTColorBufferFloat", 2, "GL_EXT_color_buffer_float"},
    {"EXT_color_buffer_half_float", "EXTColorBufferHalfFloat", 3, "GL_EXT_color_buffer_half_float"},
    {"EXT_float_blend", "EXTFloatBlend", 2, "GL_EXT_float_blend"},
    {"EXT_frag_depth", "EXTFragDepth", 1, "GL_EXT_frag_depth"},
    {"EXT_sRGB", "EXTsRGB", 1, "GL_EXT_sRGB"},
    {"EXT_texture_filter_anisotropic", "EXTTextureFilterAnisotropic", 3, "GL_EXT_texture_filter_anisotropic"},
    {"KHR_parallel_shader_compile", "KHRParallelShaderCompile", 3, NULL},
    {"OES_element_index_uint", "OESElementIndexUint", 1, "GL_OES_element_index_uint"},
    {"OES_standard_derivatives", "OESStandardDerivatives", 1, "GL_OES_standard_derivatives"},
    {"OES_texture_float", "OESTextureFloat", 1, "GL_OES_texture_float"},
    {"OES_texture_float_linear", "OESTextureFloatLinear", 3, "GL_OES_texture_float_linear"},
    {"OES_texture_half_float", "OESTextureHalfFloat", 1, "GL_OES_texture_half_float"},
    {"OES_texture_half_float_linear", "OESTextureHalfFloatLinear", 1, "GL_OES_texture_half_float_linear"},
    {"OES_vertex_array_object", "OESVertexArrayObject", 1, "GL_OES_vertex_array_object"},
    {"WEBGL_color_buffer_float", "WEBGLColorBufferFloat", 1, "GL_EXT_color_buffer_float"},
    {"WEBGL_debug_renderer_info", "WEBGLDebugRendererInfo", 3, NULL},
    {"WEBGL_depth_texture", "WEBGLDepthTexture", 1, "GL_OES_depth_texture GL_OES_packed_depth_stencil"},
    {"WEBGL_draw_buffers", "WEBGLDrawBuffers", 1, "GL_EXT_draw_buffers"},
    {"WEBGL_lose_context", "WEBGLLoseContext", 3, NULL},
};
static JSClassID ext_class[X_COUNT];

/* ---- contexts ---- */

struct pixel_store {
    GLint pack_alignment, pack_row_length, pack_skip_pixels, pack_skip_rows;
    GLint unpack_alignment, unpack_row_length, unpack_image_height, unpack_skip_pixels, unpack_skip_rows,
        unpack_skip_images;
};
struct webgl {
    unsigned refs;
    int version;
    JSRuntime *rt;
    struct ngl_context *ngl;
    unsigned generation;  /* bumped when a lost context is restored */
    bool lost, lost_by_extension, restore_allowed, lost_error, js_alive;
    struct {
        bool alpha, antialias, depth, desynchronized, fail_caveat, premultiplied, preserve, stencil;
        const char *power;
    } attrs;
    unsigned width, height; /* drawing buffer */
    JSValue canvas;
    JSValue *table[O_TABLED];
    GLuint table_size[O_TABLED];
    JSValue ext_object[X_COUNT];
    uint32_t available, enabled; /* extension bits */
    uint64_t next_id;
    GLenum errors[8];
    unsigned error_count;
    struct pixel_store store;
    bool flip_y, premultiply_alpha;
    GLenum colorspace_conversion;
    bool drawing_p3, unpack_p3;
    GLint max_attribs;
    uint8_t attrib_kind[64]; /* current generic attribute: 0 float, 1 int, 2 uint */
    GLsync *dead_syncs;
    size_t dead_count, dead_capacity;
    uint64_t serial;
    void (*changed)(void *owner);
    void *owner;
    uint32_t *stage;
    size_t stage_pixels;
    /* OffscreenCanvas owners without a DOM node: commit runs as a job once
       per presented frame, after the drawing buffer first changes. */
    JSValue commit;
    JSContext *commit_ctx;
    bool commit_armed;
};

static void gl_free(struct webgl *gl) {
    if (gl->ngl) ngl_context_destroy(gl->ngl);
    free(gl->dead_syncs);
    free(gl->stage);
    free(gl);
}
void web_webgl_retain(struct web_webgl *handle) { ((struct webgl *)handle)->refs++; }
void web_webgl_release(struct web_webgl *handle) {
    struct webgl *gl = (struct webgl *)handle;
    if (gl && !--gl->refs) gl_free(gl);
}
static void release(struct webgl *gl) { web_webgl_release((struct web_webgl *)gl); }

/* Binds the context for GL calls. Argument conversions may run page script
 * that switches or loses contexts, so this runs after them. */
static bool use(struct webgl *gl) {
    if (gl->lost || !gl->ngl) return false;
    if (ngl_get_current() != gl->ngl && !ngl_make_current(gl->ngl)) return false;
    while (gl->dead_count) glDeleteSync(gl->dead_syncs[--gl->dead_count]);
    return true;
}

static void synth(struct webgl *gl, GLenum error) {
    for (unsigned i = 0; i < gl->error_count; i++)
        if (gl->errors[i] == error) return;
    if (gl->error_count < sizeof gl->errors / sizeof *gl->errors) gl->errors[gl->error_count++] = error;
}

static void changed(struct webgl *gl) {
    GLint fb = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
    if (fb) return;
    gl->serial++;
    if (gl->changed) gl->changed(gl->owner);
}

/* ---- realm data (one per JSContext, kept as a class prototype) ---- */

static JSValue realm_get(JSContext *ctx, const char *key) {
    JSValue realm = JS_GetClassProto(ctx, realm_class);
    JSValue v = JS_GetPropertyStr(ctx, realm, key);
    JS_FreeValue(ctx, realm);
    return v;
}

static JSValue throw_dom(JSContext *ctx, const char *name, const char *message) {
    JSValue ctor = realm_get(ctx, "DOMException");
    if (!JS_IsFunction(ctx, ctor)) {
        JS_FreeValue(ctx, ctor);
        return JS_ThrowTypeError(ctx, "%s: %s", name, message);
    }
    JSValue args[2] = {JS_NewString(ctx, message), JS_NewString(ctx, name)};
    JSValue error = JS_CallConstructor(ctx, ctor, 2, args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, ctor);
    if (JS_IsException(error)) return error;
    return JS_Throw(ctx, error);
}

/* ---- typed arrays ---- */

enum { V_U8C, V_I8, V_U8, V_I16, V_U16, V_I32, V_U32, V_BI64, V_BU64, V_F16, V_F32, V_F64, V_DATAVIEW, V_TYPES };
static JSClassID view_class[V_TYPES], buffer_class, shared_class;

struct view {
    uint8_t *data;
    size_t bytes;
    int type;
    unsigned element;
};

static int view_type(JSValueConst v) {
    JSClassID id = JS_GetClassID(v);
    if (id == JS_INVALID_CLASS_ID) return -1;
    for (int i = 0; i < V_TYPES; i++)
        if (view_class[i] == id) return i;
    return -1;
}

/* 1: an ArrayBufferView (detached views report no bytes), 0: not a view. */
static int view_get(JSContext *ctx, JSValueConst v, struct view *out) {
    int type = view_type(v);
    memset(out, 0, sizeof *out);
    if (type < 0) return 0;
    out->type = type;
    JSValue buffer;
    size_t offset = 0, length = 0, element = 1;
    if (type == V_DATAVIEW) {
        JSValue getters[3] = {realm_get(ctx, "dvBuffer"), realm_get(ctx, "dvOffset"), realm_get(ctx, "dvLength")};
        buffer = JS_Call(ctx, getters[0], v, 0, NULL);
        JSValue o = JS_Call(ctx, getters[1], v, 0, NULL), l = JS_Call(ctx, getters[2], v, 0, NULL);
        double dv_offset = 0, dv_length = 0;
        if (JS_IsException(buffer) || JS_IsException(o) || JS_IsException(l) || JS_ToFloat64(ctx, &dv_offset, o) ||
            JS_ToFloat64(ctx, &dv_length, l)) {
            JS_FreeValue(ctx, JS_GetException(ctx)); /* detached */
            dv_offset = dv_length = 0;
        }
        offset = (size_t)dv_offset;
        length = (size_t)dv_length;
        for (int i = 0; i < 3; i++) JS_FreeValue(ctx, getters[i]);
        JS_FreeValue(ctx, o);
        JS_FreeValue(ctx, l);
    } else {
        buffer = JS_GetTypedArrayBuffer(ctx, v, &offset, &length, &element);
    }
    if (JS_IsException(buffer)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        out->element = (unsigned)element;
        return 1;
    }
    size_t size = 0;
    uint8_t *base = JS_GetArrayBuffer(ctx, &size, buffer);
    JS_FreeValue(ctx, buffer);
    if (!base) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        length = 0;
    } else if (offset > size || length > size - offset) {
        length = 0;
    }
    out->data = base ? base + offset : NULL;
    out->bytes = length;
    out->element = (unsigned)element;
    return 1;
}

/* BufferSource: ArrayBuffer, SharedArrayBuffer or ArrayBufferView. */
static int source_get(JSContext *ctx, JSValueConst v, struct view *out) {
    JSClassID id = JS_GetClassID(v);
    if (id != JS_INVALID_CLASS_ID && (id == buffer_class || id == shared_class)) {
        size_t size = 0;
        memset(out, 0, sizeof *out);
        out->data = JS_GetArrayBuffer(ctx, &size, v);
        if (!out->data) JS_FreeValue(ctx, JS_GetException(ctx));
        out->bytes = out->data ? size : 0;
        out->type = V_U8;
        out->element = 1;
        return 1;
    }
    return view_get(ctx, v, out);
}

/* Applies WebGL 2 srcOffset (elements) and length (elements, 0: rest). */
static bool view_range(struct view *v, uint64_t offset, uint64_t length) {
    uint64_t count = v->bytes / v->element;
    if (offset > count) return false;
    if (length && length > count - offset) return false;
    v->data += offset * v->element;
    v->bytes = (size_t)((length ? length : count - offset) * v->element);
    return true;
}

/* Float32List / Int32List / Uint32List: a matching typed array or a JS
 * sequence converted into owned storage. */
static int list_get(JSContext *ctx, JSValueConst v, int type, void **data, size_t *count, void **owned) {
    *owned = NULL;
    struct view view;
    if (view_get(ctx, v, &view)) {
        if (view.type != type) {
            JS_ThrowTypeError(ctx, "Unexpected typed array type");
            return -1;
        }
        *data = view.data;
        *count = view.bytes / 4;
        return 0;
    }
    if (!JS_IsObject(v)) {
        JS_ThrowTypeError(ctx, "Expected a sequence");
        return -1;
    }
    JSValue lv = JS_GetPropertyStr(ctx, v, "length");
    uint32_t length = 0;
    if (JS_IsException(lv) || JS_ToUint32(ctx, &length, lv)) {
        JS_FreeValue(ctx, lv);
        return -1;
    }
    JS_FreeValue(ctx, lv);
    uint32_t *out = length ? js_malloc(ctx, (size_t)length * 4) : NULL;
    if (length && !out) return -1;
    for (uint32_t i = 0; i < length; i++) {
        JSValue item = JS_GetPropertyUint32(ctx, v, i);
        int r;
        if (type == V_F32) {
            double d;
            r = JS_ToFloat64(ctx, &d, item);
            float f = (float)d;
            memcpy(&out[i], &f, 4);
        } else {
            r = JS_ToUint32(ctx, &out[i], item);
        }
        JS_FreeValue(ctx, item);
        if (r) {
            js_free(ctx, out);
            return -1;
        }
    }
    *data = out;
    *owned = out;
    *count = length;
    return 0;
}

/* ---- argument helpers ---- */

static int toi(JSContext *ctx, JSValueConst v, GLint *o) { return JS_ToInt32(ctx, o, v); }
static int tou(JSContext *ctx, JSValueConst v, GLuint *o) { return JS_ToUint32(ctx, o, v); }
static int tof(JSContext *ctx, JSValueConst v, GLfloat *o) {
    double d;
    if (JS_ToFloat64(ctx, &d, v)) return -1;
    *o = (GLfloat)d;
    return 0;
}
static int toi64(JSContext *ctx, JSValueConst v, int64_t *o) {
    double d;
    if (JS_ToFloat64(ctx, &d, v)) return -1;
    /* WebIDL long long: truncate, then wrap modulo 2^64. */
    if (!isfinite(d)) d = 0;
    d = trunc(d);
    if (d >= -9223372036854775808.0 && d < 9223372036854775808.0) *o = (int64_t)d;
    else *o = (int64_t)(uint64_t)fmod(d, 18446744073709551616.0);
    return 0;
}
static int ints(JSContext *ctx, int argc, JSValueConst *argv, int first, int count, GLint *out) {
    for (int i = 0; i < count; i++)
        if (toi(ctx, first + i < argc ? argv[first + i] : JS_UNDEFINED, &out[i])) return -1;
    return 0;
}
static int floats(JSContext *ctx, int argc, JSValueConst *argv, int first, int count, GLfloat *out) {
    for (int i = 0; i < count; i++)
        if (tof(ctx, first + i < argc ? argv[first + i] : JS_UNDEFINED, &out[i])) return -1;
    return 0;
}

/* Object arguments: NULL for null/undefined when nullable, TypeError for
 * anything that is not the interface. */
static int object_arg(JSContext *ctx, JSValueConst v, int kind, bool nullable, struct wobj **out) {
    *out = NULL;
    if (nullable && (JS_IsNull(v) || JS_IsUndefined(v))) return 0;
    struct wobj *o = JS_GetOpaque(v, kind_class[kind]);
    if (!o && kind == O_VAO) o = JS_GetOpaque(v, kind_class[O_VAO_OES]);
    if (!o) {
        JS_ThrowTypeError(ctx, "Expected %s", kind_names[kind]);
        return -1;
    }
    *out = o;
    return 0;
}
/* Objects from another context or an earlier (lost) generation. */
static bool owned(struct webgl *gl, struct wobj *o) {
    if (!o) return true;
    if (o->gl != gl || o->generation != gl->generation) {
        synth(gl, GL_INVALID_OPERATION);
        return false;
    }
    return true;
}
static GLuint name_of(const struct wobj *o) { return o ? o->name : 0; }

/* ---- per-context object tables: one wrapper per GL name ---- */

static bool table_put(JSContext *ctx, struct webgl *gl, int kind, GLuint name, JSValueConst value) {
    if (name >= gl->table_size[kind]) {
        GLuint size = gl->table_size[kind] ? gl->table_size[kind] : 16;
        while (size <= name) {
            if (size > UINT32_MAX / 2) return false;
            size *= 2;
        }
        JSValue *grown = realloc(gl->table[kind], (size_t)size * sizeof *grown);
        if (!grown) return false;
        for (GLuint i = gl->table_size[kind]; i < size; i++) grown[i] = JS_UNDEFINED;
        gl->table[kind] = grown;
        gl->table_size[kind] = size;
    }
    JS_FreeValue(ctx, gl->table[kind][name]);
    gl->table[kind][name] = JS_DupValue(ctx, value);
    return true;
}
static JSValue table_get(JSContext *ctx, struct webgl *gl, int kind, GLuint name) {
    if (!name || name >= gl->table_size[kind] || JS_IsUndefined(gl->table[kind][name])) return JS_NULL;
    return JS_DupValue(ctx, gl->table[kind][name]);
}
static struct wobj *table_object(struct webgl *gl, int kind, GLuint name) {
    if (!name || name >= gl->table_size[kind] || JS_IsUndefined(gl->table[kind][name])) return NULL;
    JSValue v = gl->table[kind][name];
    struct wobj *o = JS_GetOpaque(v, kind_class[kind]);
    if (!o && kind == O_VAO) o = JS_GetOpaque(v, kind_class[O_VAO_OES]);
    return o;
}
static void tables_clear(JSRuntime *rt, struct webgl *gl) {
    for (int k = 0; k < O_TABLED; k++) {
        for (GLuint i = 0; i < gl->table_size[k]; i++) JS_FreeValueRT(rt, gl->table[k][i]);
        free(gl->table[k]);
        gl->table[k] = NULL;
        gl->table_size[k] = 0;
    }
}

static JSValue wrapper_new(JSContext *ctx, struct webgl *gl, int kind, struct wobj **out) {
    int cls = kind == O_VAO && gl->version == 1 ? O_VAO_OES : kind;
    JSValue v = JS_NewObjectClass(ctx, (int)kind_class[cls]);
    if (JS_IsException(v)) return v;
    struct wobj *o = calloc(1, sizeof *o);
    if (!o) {
        JS_FreeValue(ctx, v);
        return JS_ThrowOutOfMemory(ctx);
    }
    o->gl = gl;
    gl->refs++;
    o->generation = gl->generation;
    o->kind = (uint8_t)kind;
    o->id = ++gl->next_id;
    JS_SetOpaque(v, o);
    *out = o;
    return v;
}
static void wrapper_finalizer(JSRuntime *rt, JSValue v) {
    (void)rt;
    struct wobj *o = NULL;
    for (int k = 0; k < O_KINDS && !o; k++) o = JS_GetOpaque(v, kind_class[k]);
    if (!o) return;
    struct webgl *gl = o->gl;
    if (o->sync && !o->deleted && gl && gl->ngl && o->generation == gl->generation) {
        if (gl->dead_count == gl->dead_capacity) {
            size_t capacity = gl->dead_capacity ? gl->dead_capacity * 2 : 16;
            GLsync *grown = realloc(gl->dead_syncs, capacity * sizeof *grown);
            if (grown) {
                gl->dead_syncs = grown;
                gl->dead_capacity = capacity;
            }
        }
        if (gl->dead_count < gl->dead_capacity) gl->dead_syncs[gl->dead_count++] = o->sync;
    }
    free(o->source);
    free(o->text);
    if (gl) release(gl);
    free(o);
}

/* ---- context object lifetime ---- */

static struct webgl *context_of(JSValueConst v, bool webgl2_only) {
    struct webgl *gl = JS_GetOpaque(v, gl2_class);
    if (!gl && !webgl2_only) gl = JS_GetOpaque(v, gl1_class);
    return gl;
}
static void context_finalizer(JSRuntime *rt, JSValue v) {
    struct webgl *gl = context_of(v, false);
    if (!gl) return;
    gl->js_alive = false;
    JS_FreeValueRT(rt, gl->canvas);
    gl->canvas = JS_UNDEFINED;
    JS_FreeValueRT(rt, gl->commit);
    gl->commit = JS_UNDEFINED;
    if (gl->owner == gl) gl->changed = NULL, gl->owner = NULL;
    for (int i = 0; i < X_COUNT; i++) {
        JS_FreeValueRT(rt, gl->ext_object[i]);
        gl->ext_object[i] = JS_UNDEFINED;
    }
    tables_clear(rt, gl);
    release(gl);
}
static void context_mark(JSRuntime *rt, JSValueConst v, JS_MarkFunc *mark) {
    struct webgl *gl = context_of(v, false);
    if (!gl) return;
    JS_MarkValue(rt, gl->canvas, mark);
    JS_MarkValue(rt, gl->commit, mark);
    for (int i = 0; i < X_COUNT; i++) JS_MarkValue(rt, gl->ext_object[i], mark);
    for (int k = 0; k < O_TABLED; k++)
        for (GLuint i = 0; i < gl->table_size[k]; i++) JS_MarkValue(rt, gl->table[k][i], mark);
}
static void extension_finalizer(JSRuntime *rt, JSValue v) {
    (void)rt;
    for (int i = 0; i < X_COUNT; i++) {
        struct webgl *gl = JS_GetOpaque(v, ext_class[i]);
        if (gl) {
            release(gl);
            return;
        }
    }
}

/* ---- context creation and the native owner interface ---- */

static bool has_gl_extension(const char *list, const char *wanted) {
    while (*wanted) {
        const char *end = strchr(wanted, ' ');
        size_t n = end ? (size_t)(end - wanted) : strlen(wanted);
        bool found = false;
        for (const char *p = list; p && (p = strstr(p, wanted)) != NULL; p += n) {
            bool start = p == list || p[-1] == ' ', stop = p[n] == ' ' || !p[n];
            if (start && stop && !strncmp(p, wanted, n)) {
                found = true;
                break;
            }
        }
        if (!found) return false;
        wanted += n;
        while (*wanted == ' ') wanted++;
    }
    return true;
}

static bool context_setup(struct webgl *gl) {
    char error[128];
    struct ngl_attribs attribs = {gl->attrs.alpha, gl->attrs.depth, gl->attrs.stencil, gl->attrs.antialias};
    gl->ngl = ngl_context_create(gl->version == 2 ? 3 : 2, &attribs, gl->width, gl->height, error, sizeof error);
    if (!gl->ngl) return false;
    if (!ngl_make_current(gl->ngl)) {
        ngl_context_destroy(gl->ngl);
        gl->ngl = NULL;
        return false;
    }
    struct ngl_attribs actual;
    ngl_get_attribs(gl->ngl, &actual);
    gl->attrs.depth = actual.depth;
    gl->attrs.stencil = actual.stencil;
    gl->attrs.antialias = actual.antialias;
    /* WebGL 2 always restarts primitives at the maximum index value. */
    if (gl->version == 2) glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &gl->max_attribs);
    if (gl->max_attribs > 64) gl->max_attribs = 64;
    memset(gl->attrib_kind, 0, sizeof gl->attrib_kind);
    const char *list = (const char *)glGetString(GL_EXTENSIONS);
    gl->available = 0;
    for (int i = 0; i < X_COUNT; i++)
        if ((extensions[i].versions & (1u << (gl->version - 1))) &&
            (!extensions[i].requires || has_gl_extension(list ? list : "", extensions[i].requires)))
            gl->available |= 1u << i;
    gl->store = (struct pixel_store){.pack_alignment = 4, .unpack_alignment = 4};
    gl->flip_y = gl->premultiply_alpha = false;
    gl->colorspace_conversion = WEBGL_BROWSER_DEFAULT;
    gl->error_count = 0;
    return true;
}

static int bool_member(JSContext *ctx, JSValueConst options, const char *key, bool fallback, bool *out) {
    JSValue v = JS_GetPropertyStr(ctx, options, key);
    if (JS_IsException(v)) return -1;
    *out = JS_IsUndefined(v) ? fallback : JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);
    return 0;
}

static void clamp_size(unsigned *w, unsigned *h) {
    /* Larger canvases get a proportionally smaller drawing buffer. */
    const unsigned limit = 8192;
    if (*w <= limit && *h <= limit) return;
    double scale = (double)limit / (*w > *h ? *w : *h);
    *w = (unsigned)(*w * scale);
    *h = (unsigned)(*h * scale);
    if (*w > limit) *w = limit;
    if (*h > limit) *h = limit;
}

JSValue web_webgl_create(JSContext *ctx, JSValueConst canvas, int version, JSValueConst options, unsigned width,
                         unsigned height, struct web_webgl **out) {
    *out = NULL;
    struct webgl probe = {0};
    probe.attrs.power = "default";
    if (!JS_IsUndefined(options) && !JS_IsNull(options)) {
        if (!JS_IsObject(options)) return JS_ThrowTypeError(ctx, "WebGLContextAttributes must be a dictionary");
        bool xr = false;
        /* Dictionary members are read in lexicographic order. */
        if (bool_member(ctx, options, "alpha", true, &probe.attrs.alpha) ||
            bool_member(ctx, options, "antialias", true, &probe.attrs.antialias) ||
            bool_member(ctx, options, "depth", true, &probe.attrs.depth) ||
            bool_member(ctx, options, "desynchronized", false, &probe.attrs.desynchronized) ||
            bool_member(ctx, options, "failIfMajorPerformanceCaveat", false, &probe.attrs.fail_caveat))
            return JS_EXCEPTION;
        JSValue power = JS_GetPropertyStr(ctx, options, "powerPreference");
        if (JS_IsException(power)) return power;
        if (!JS_IsUndefined(power)) {
            const char *s = JS_ToCString(ctx, power);
            JS_FreeValue(ctx, power);
            if (!s) return JS_EXCEPTION;
            static const char *const choices[] = {"default", "low-power", "high-performance"};
            const char *found = NULL;
            for (int i = 0; i < 3; i++)
                if (!strcmp(s, choices[i])) found = choices[i];
            JS_FreeCString(ctx, s);
            if (!found) return JS_ThrowTypeError(ctx, "Invalid powerPreference");
            probe.attrs.power = found;
        }
        if (bool_member(ctx, options, "premultipliedAlpha", true, &probe.attrs.premultiplied) ||
            bool_member(ctx, options, "preserveDrawingBuffer", false, &probe.attrs.preserve) ||
            bool_member(ctx, options, "stencil", false, &probe.attrs.stencil) ||
            bool_member(ctx, options, "xrCompatible", false, &xr))
            return JS_EXCEPTION;
    } else {
        probe.attrs.alpha = probe.attrs.antialias = probe.attrs.depth = probe.attrs.premultiplied = true;
    }
    /* Software rasterization is the major performance caveat the page
       asked to avoid; it then falls back to its own path. */
    if (probe.attrs.fail_caveat) return JS_NULL;
    struct webgl *gl = calloc(1, sizeof *gl);
    if (!gl) return JS_ThrowOutOfMemory(ctx);
    gl->attrs = probe.attrs;
    gl->version = version == 2 ? 2 : 1;
    gl->rt = JS_GetRuntime(ctx);
    gl->refs = 1;
    clamp_size(&width, &height);
    gl->width = width;
    gl->height = height;
    gl->canvas = JS_UNDEFINED;
    gl->commit = JS_UNDEFINED;
    for (int i = 0; i < X_COUNT; i++) gl->ext_object[i] = JS_UNDEFINED;
    struct ngl_context *previous = ngl_get_current();
    if (!context_setup(gl)) {
        free(gl);
        if (previous) ngl_make_current(previous);
        return JS_NULL;
    }
    JSValue object = JS_NewObjectClass(ctx, (int)(gl->version == 2 ? gl2_class : gl1_class));
    if (JS_IsException(object)) {
        gl_free(gl);
        return object;
    }
    gl->js_alive = true;
    gl->canvas = JS_DupValue(ctx, canvas);
    JS_SetOpaque(object, gl);
    *out = (struct web_webgl *)gl;
    return object;
}

void web_webgl_set_owner(struct web_webgl *handle, void (*hook)(void *), void *owner) {
    struct webgl *gl = (struct webgl *)handle;
    gl->changed = owner ? hook : NULL;
    gl->owner = owner;
}

void web_webgl_resize(struct web_webgl *handle, unsigned width, unsigned height) {
    struct webgl *gl = (struct webgl *)handle;
    clamp_size(&width, &height);
    if (width == gl->width && height == gl->height) return;
    gl->width = width;
    gl->height = height;
    if (gl->ngl && !gl->lost) {
        struct ngl_context *previous = ngl_get_current();
        ngl_resize(gl->ngl, width, height);
        if (previous && previous != ngl_get_current()) ngl_make_current(previous);
    }
    gl->serial++;
    if (gl->changed) gl->changed(gl->owner);
}

uint64_t web_webgl_serial(const struct web_webgl *handle) { return ((const struct webgl *)handle)->serial; }

static bool profile_on;
static void profile_present(uint64_t cycles);
static bool present(struct webgl *gl, uint32_t *dst, unsigned width, unsigned height, bool frame_end);
bool web_webgl_present(struct web_webgl *handle, uint32_t *dst, unsigned width, unsigned height, bool frame_end) {
    struct webgl *gl = (struct webgl *)handle;
    if (!gl->ngl || gl->lost || !width || !height) return false;
    if (!profile_on) return present(gl, dst, width, height, frame_end);
    uint64_t start = __builtin_ia32_rdtsc();
    bool ok = present(gl, dst, width, height, frame_end);
    profile_present(__builtin_ia32_rdtsc() - start);
    return ok;
}
static bool present(struct webgl *gl, uint32_t *dst, unsigned width, unsigned height, bool frame_end) {
    bool ok;
    if (width == gl->width && height == gl->height) {
        ok = ngl_read_drawing_buffer(gl->ngl, dst, width, gl->attrs.premultiplied);
    } else {
        size_t pixels = (size_t)gl->width * gl->height;
        if (!pixels) {
            memset(dst, 0, (size_t)width * height * sizeof *dst);
            return true;
        }
        if (gl->stage_pixels < pixels) {
            uint32_t *grown = realloc(gl->stage, pixels * sizeof *grown);
            if (!grown) return false;
            gl->stage = grown;
            gl->stage_pixels = pixels;
        }
        ok = ngl_read_drawing_buffer(gl->ngl, gl->stage, gl->width, gl->attrs.premultiplied);
        if (ok)
            for (unsigned y = 0; y < height; y++) {
                const uint32_t *row = gl->stage + (size_t)((uint64_t)y * gl->height / height) * gl->width;
                for (unsigned x = 0; x < width; x++) dst[(size_t)y * width + x] = row[(uint64_t)x * gl->width / width];
            }
    }
    if (ok && frame_end && !gl->attrs.preserve) ngl_clear_drawing_buffer(gl->ngl);
    return ok;
}

/* ---- method table plumbing ---- */

enum { L_UNDEF, L_NULL, L_FALSE, L_MINUS1, L_ZERO, L_ALWAYS, L_FB_UNSUPPORTED, L_WAIT_FAILED, L_INVALID_INDEX };
typedef JSValue method_fn(JSContext *ctx, struct webgl *gl, int argc, JSValueConst *argv);
struct method {
    const char *name;
    method_fn *fn;
    uint8_t length, version, lost;
};

static JSValue lost_value(int lost) {
    switch (lost) {
    case L_NULL: return JS_NULL;
    case L_FALSE: return JS_FALSE;
    case L_MINUS1: return JS_NewInt32(NULL, -1);
    case L_ZERO: return JS_NewInt32(NULL, 0);
    case L_FB_UNSUPPORTED: return JS_NewInt32(NULL, GL_FRAMEBUFFER_UNSUPPORTED);
    case L_WAIT_FAILED: return JS_NewInt32(NULL, GL_WAIT_FAILED);
    case L_INVALID_INDEX: return JS_NewFloat64(NULL, 4294967295.0);
    default: return JS_UNDEFINED;
    }
}

#define CUR(v) do { if (!use(gl)) return (v); } while (0)
#define FN(name) static JSValue name(JSContext *ctx, struct webgl *gl, int argc, JSValueConst *argv)
#define UNUSED_ARGS (void)ctx; (void)argc; (void)argv

/* ---- target/enum validation WebGL adds over OpenGL ES 3 ---- */

static bool buffer_target(struct webgl *gl, GLenum t) {
    if (t == GL_ARRAY_BUFFER || t == GL_ELEMENT_ARRAY_BUFFER) return true;
    return gl->version == 2 && (t == GL_COPY_READ_BUFFER || t == GL_COPY_WRITE_BUFFER || t == GL_PIXEL_PACK_BUFFER ||
                                t == GL_PIXEL_UNPACK_BUFFER || t == GL_TRANSFORM_FEEDBACK_BUFFER ||
                                t == GL_UNIFORM_BUFFER);
}
static GLenum buffer_binding(GLenum t) {
    switch (t) {
    case GL_ARRAY_BUFFER: return GL_ARRAY_BUFFER_BINDING;
    case GL_ELEMENT_ARRAY_BUFFER: return GL_ELEMENT_ARRAY_BUFFER_BINDING;
    case GL_COPY_READ_BUFFER: return GL_COPY_READ_BUFFER_BINDING;
    case GL_COPY_WRITE_BUFFER: return GL_COPY_WRITE_BUFFER_BINDING;
    case GL_PIXEL_PACK_BUFFER: return GL_PIXEL_PACK_BUFFER_BINDING;
    case GL_PIXEL_UNPACK_BUFFER: return GL_PIXEL_UNPACK_BUFFER_BINDING;
    case GL_TRANSFORM_FEEDBACK_BUFFER: return GL_TRANSFORM_FEEDBACK_BUFFER_BINDING;
    case GL_UNIFORM_BUFFER: return GL_UNIFORM_BUFFER_BINDING;
    default: return 0;
    }
}
static bool usage_ok(struct webgl *gl, GLenum u) {
    if (u == GL_STREAM_DRAW || u == GL_STATIC_DRAW || u == GL_DYNAMIC_DRAW) return true;
    return gl->version == 2 && (u == GL_STREAM_READ || u == GL_STREAM_COPY || u == GL_STATIC_READ ||
                                u == GL_STATIC_COPY || u == GL_DYNAMIC_READ || u == GL_DYNAMIC_COPY);
}
static bool capability_ok(struct webgl *gl, GLenum cap) {
    switch (cap) {
    case GL_BLEND: case GL_CULL_FACE: case GL_DEPTH_TEST: case GL_DITHER: case GL_POLYGON_OFFSET_FILL:
    case GL_SAMPLE_ALPHA_TO_COVERAGE: case GL_SAMPLE_COVERAGE: case GL_SCISSOR_TEST: case GL_STENCIL_TEST:
        return true;
    case GL_RASTERIZER_DISCARD: return gl->version == 2;
    default: return false;
    }
}
static bool texture_target(struct webgl *gl, GLenum t) {
    return t == GL_TEXTURE_2D || t == GL_TEXTURE_CUBE_MAP ||
           (gl->version == 2 && (t == GL_TEXTURE_3D || t == GL_TEXTURE_2D_ARRAY));
}
static bool framebuffer_target(struct webgl *gl, GLenum t) {
    return t == GL_FRAMEBUFFER || (gl->version == 2 && (t == GL_READ_FRAMEBUFFER || t == GL_DRAW_FRAMEBUFFER));
}
static bool ext(struct webgl *gl, int id) { return (gl->enabled >> id) & 1; }

/* ---- pixel layouts ---- */

static unsigned format_components(GLenum format) {
    switch (format) {
    case GL_RED: case GL_RED_INTEGER: case GL_ALPHA: case GL_LUMINANCE: case GL_DEPTH_COMPONENT:
    case GL_DEPTH_STENCIL: case GL_STENCIL_INDEX8:
        return 1;
    case GL_RG: case GL_RG_INTEGER: case GL_LUMINANCE_ALPHA: return 2;
    case GL_RGB: case GL_RGB_INTEGER: case GL_SRGB_EXT: return 3;
    case GL_RGBA: case GL_RGBA_INTEGER: case GL_SRGB_ALPHA_EXT: return 4;
    default: return 0;
    }
}
/* Bytes per pixel, 0 for an unknown format/type. */
static unsigned pixel_bytes(GLenum format, GLenum type) {
    unsigned n = format_components(format);
    if (!n) return 0;
    switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE: return n;
    case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT: case GL_HALF_FLOAT_OES: return 2 * n;
    case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: return 4 * n;
    case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_5_5_5_1: return 2;
    case GL_UNSIGNED_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_10F_11F_11F_REV: case GL_UNSIGNED_INT_5_9_9_9_REV:
    case GL_UNSIGNED_INT_24_8:
        return 4;
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV: return 8;
    default: return 0;
    }
}
/* The typed array type a pixel type must arrive in; -1 for none. */
static int pixel_view(GLenum type) {
    switch (type) {
    case GL_UNSIGNED_BYTE: return V_U8;
    case GL_BYTE: return V_I8;
    case GL_SHORT: return V_I16;
    case GL_UNSIGNED_SHORT: case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1: case GL_HALF_FLOAT: case GL_HALF_FLOAT_OES:
        return V_U16;
    case GL_INT: return V_I32;
    case GL_UNSIGNED_INT: case GL_UNSIGNED_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_10F_11F_11F_REV:
    case GL_UNSIGNED_INT_5_9_9_9_REV: case GL_UNSIGNED_INT_24_8:
        return V_U32;
    case GL_FLOAT: return V_F32;
    default: return -1;
    }
}
static bool view_matches(int view, GLenum type) {
    int want = pixel_view(type);
    return view == want || (want == V_U8 && view == V_U8C);
}

/* Bytes an image of w x h x d reaches into client memory under the current
 * pack or unpack parameters; false when WebGL 2 sub-rectangle parameters
 * reach outside their own row/image length (INVALID_OPERATION). */
static bool image_bytes(struct webgl *gl, bool pack, GLsizei w, GLsizei h, GLsizei d, unsigned bpp, uint64_t *need) {
    const struct pixel_store *s = &gl->store;
    uint64_t alignment = (uint64_t)(pack ? s->pack_alignment : s->unpack_alignment);
    uint64_t row_length = (uint64_t)(pack ? s->pack_row_length : s->unpack_row_length);
    uint64_t image_height = pack ? 0 : (uint64_t)s->unpack_image_height;
    uint64_t skip_pixels = (uint64_t)(pack ? s->pack_skip_pixels : s->unpack_skip_pixels);
    uint64_t skip_rows = (uint64_t)(pack ? s->pack_skip_rows : s->unpack_skip_rows);
    uint64_t skip_images = pack ? 0 : (uint64_t)s->unpack_skip_images;
    if (w < 0 || h < 0 || d < 0) {
        *need = 0;
        return true;
    }
    if (row_length && skip_pixels + (uint64_t)w > row_length) return false;
    if (image_height && skip_rows + (uint64_t)h > image_height) return false;
    if (!w || !h || !d) {
        *need = 0;
        return true;
    }
    if (!alignment) alignment = 1;
    uint64_t row = (row_length ? row_length : (uint64_t)w) * bpp;
    uint64_t stride = (row + alignment - 1) / alignment * alignment;
    uint64_t image = stride * (image_height ? image_height : (uint64_t)h);
    *need = skip_images * image + skip_rows * stride + skip_pixels * bpp + (uint64_t)(d - 1) * image +
            (uint64_t)(h - 1) * stride + (uint64_t)w * bpp;
    return true;
}

/* Temporarily unpack tightly packed client data; unpack_restore puts the
 * page's parameters back. */
static void unpack_tight(struct webgl *gl, GLint row_length, GLint image_height, bool keep_skips) {
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (gl->version == 2) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
        glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, image_height);
        if (!keep_skips) {
            glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
            glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
            glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);
        }
    }
}
static void unpack_restore(struct webgl *gl) {
    const struct pixel_store *s = &gl->store;
    glPixelStorei(GL_UNPACK_ALIGNMENT, s->unpack_alignment);
    if (gl->version == 2) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, s->unpack_row_length);
        glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, s->unpack_image_height);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, s->unpack_skip_pixels);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, s->unpack_skip_rows);
        glPixelStorei(GL_UNPACK_SKIP_IMAGES, s->unpack_skip_images);
    }
}

/* Writes one 8-bit RGBA pixel in format/type; false if not encodable. */
static bool encode_pixel(uint8_t *out, GLenum format, GLenum type, const uint8_t c[4]) {
    unsigned n = format_components(format);
    uint8_t src[4];
    switch (format) {
    case GL_RED: case GL_RED_INTEGER: case GL_LUMINANCE: src[0] = c[0]; break;
    case GL_ALPHA: src[0] = c[3]; break;
    case GL_LUMINANCE_ALPHA: src[0] = c[0]; src[1] = c[3]; break;
    case GL_RG: case GL_RG_INTEGER: src[0] = c[0]; src[1] = c[1]; break;
    case GL_RGB: case GL_RGB_INTEGER: case GL_SRGB_EXT: memcpy(src, c, 3); break;
    case GL_RGBA: case GL_RGBA_INTEGER: case GL_SRGB_ALPHA_EXT: memcpy(src, c, 4); break;
    default: return false;
    }
    switch (type) {
    case GL_UNSIGNED_BYTE: memcpy(out, src, n); return true;
    case GL_HALF_FLOAT: case GL_HALF_FLOAT_OES:
        for (unsigned i = 0; i < n; i++) {
            uint16_t h = _mesa_float_to_half(src[i] / 255.0f);
            memcpy(out + 2 * i, &h, 2);
        }
        return true;
    case GL_FLOAT:
        for (unsigned i = 0; i < n; i++) {
            float f = src[i] / 255.0f;
            memcpy(out + 4 * i, &f, 4);
        }
        return true;
    case GL_UNSIGNED_SHORT_5_6_5: {
        if (n != 3) return false;
        uint16_t v = (uint16_t)((src[0] * 31 + 127) / 255 << 11 | (src[1] * 63 + 127) / 255 << 5 | (src[2] * 31 + 127) / 255);
        memcpy(out, &v, 2);
        return true;
    }
    case GL_UNSIGNED_SHORT_4_4_4_4: {
        if (n != 4) return false;
        uint16_t v = (uint16_t)((src[0] * 15 + 127) / 255 << 12 | (src[1] * 15 + 127) / 255 << 8 |
                                (src[2] * 15 + 127) / 255 << 4 | (src[3] * 15 + 127) / 255);
        memcpy(out, &v, 2);
        return true;
    }
    case GL_UNSIGNED_SHORT_5_5_5_1: {
        if (n != 4) return false;
        uint16_t v = (uint16_t)((src[0] * 31 + 127) / 255 << 11 | (src[1] * 31 + 127) / 255 << 6 |
                                (src[2] * 31 + 127) / 255 << 1 | (src[3] >= 128));
        memcpy(out, &v, 2);
        return true;
    }
    case GL_UNSIGNED_INT_2_10_10_10_REV: {
        if (n != 4) return false;
        uint32_t v = (uint32_t)(src[3] * 3 + 127) / 255 << 30 | (uint32_t)(src[2] * 1023 + 127) / 255 << 20 |
                     (uint32_t)(src[1] * 1023 + 127) / 255 << 10 | (uint32_t)(src[0] * 1023 + 127) / 255;
        memcpy(out, &v, 4);
        return true;
    }
    case GL_UNSIGNED_INT_10F_11F_11F_REV: case GL_UNSIGNED_INT_5_9_9_9_REV: {
        if (n != 3) return false;
        float rgb[3] = {src[0] / 255.0f, src[1] / 255.0f, src[2] / 255.0f};
        uint32_t v = type == GL_UNSIGNED_INT_5_9_9_9_REV ? float3_to_rgb9e5(rgb) : float3_to_r11g11b10f(rgb);
        memcpy(out, &v, 4);
        return true;
    }
    default: return false;
    }
}

/* ---- TexImageSource uploads ---- */

static web_webgl_source_fn source_hook;

/* Converts a TexImageSource to tightly packed format/type pixels with the
 * WebGL unpack flags applied. 1: done, 0: not a source (TypeError thrown),
 * -1: exception, 2: GL error recorded. */
static int image_source(JSContext *ctx, struct webgl *gl, JSValueConst value, GLenum format, GLenum type,
                        uint8_t **out, unsigned *w, unsigned *h, unsigned *bpp) {
    JSValue resolver = realm_get(ctx, "source");
    JSValue resolved = JS_IsFunction(ctx, resolver) ? JS_Call(ctx, resolver, JS_UNDEFINED, 1, &value) : JS_UNDEFINED;
    JS_FreeValue(ctx, resolver);
    if (JS_IsException(resolved)) return -1;
    struct web_webgl_pixels pixels = {0};
    uint8_t *rgba_owned = NULL;
    const uint8_t *rgba = NULL;
    int r = 0;
    if (JS_IsArray(ctx, resolved) > 0) {
        /* [width, height, ArrayBuffer of RGBA bytes, tainted] (ImageData). */
        JSValue fields[4];
        for (int i = 0; i < 4; i++) fields[i] = JS_GetPropertyUint32(ctx, resolved, (uint32_t)i);
        uint32_t iw = 0, ih = 0;
        size_t size = 0;
        uint8_t *bytes = NULL;
        if (!JS_ToUint32(ctx, &iw, fields[0]) && !JS_ToUint32(ctx, &ih, fields[1]))
            bytes = JS_GetArrayBuffer(ctx, &size, fields[2]);
        if (bytes && (uint64_t)iw * ih * 4 <= size) {
            pixels.width = iw;
            pixels.height = ih;
            pixels.tainted = JS_ToBool(ctx, fields[3]);
            rgba = bytes;
            r = 1;
        } else {
            JS_FreeValue(ctx, JS_GetException(ctx));
            r = 0;
        }
        for (int i = 0; i < 4; i++) JS_FreeValue(ctx, fields[i]);
    } else if (!JS_IsUndefined(resolved) && source_hook) {
        r = source_hook(ctx, resolved, &pixels);
    }
    if (r < 0) {
        JS_FreeValue(ctx, resolved);
        return -1;
    }
    if (r == 0) {
        JS_FreeValue(ctx, resolved);
        JS_ThrowTypeError(ctx, "Expected a TexImageSource");
        return 0;
    }
    if (r == 2 || pixels.tainted) {
        JS_FreeValue(ctx, resolved);
        if (pixels.tainted) {
            throw_dom(ctx, "SecurityError", "The image is not origin-clean");
            return -1;
        }
        synth(gl, GL_INVALID_OPERATION);
        return 2;
    }
    *bpp = pixel_bytes(format, type);
    if (!*bpp || format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL) {
        JS_FreeValue(ctx, resolved);
        synth(gl, GL_INVALID_ENUM);
        return 2;
    }
    size_t count = (size_t)pixels.width * pixels.height;
    uint8_t *dst = count ? malloc(count * *bpp) : malloc(1);
    if (!dst) {
        JS_FreeValue(ctx, resolved);
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    bool premultiply = gl->premultiply_alpha;
    for (unsigned y = 0; y < pixels.height; y++) {
        unsigned sy = gl->flip_y ? pixels.height - 1 - y : y;
        for (unsigned x = 0; x < pixels.width; x++) {
            uint8_t c[4];
            if (rgba) {
                memcpy(c, rgba + ((size_t)sy * pixels.width + x) * 4, 4);
            } else {
                uint32_t p = pixels.argb[(size_t)sy * pixels.width + x];
                c[0] = (uint8_t)(p >> 16);
                c[1] = (uint8_t)(p >> 8);
                c[2] = (uint8_t)p;
                c[3] = (uint8_t)(p >> 24);
            }
            if (premultiply && c[3] != 255)
                for (int i = 0; i < 3; i++) c[i] = (uint8_t)((c[i] * c[3] + 127) / 255);
            if (!encode_pixel(dst + ((size_t)y * pixels.width + x) * *bpp, format, type, c)) {
                free(dst);
                free(rgba_owned);
                JS_FreeValue(ctx, resolved);
                synth(gl, GL_INVALID_OPERATION);
                return 2;
            }
        }
    }
    JS_FreeValue(ctx, resolved);
    *out = dst;
    *w = pixels.width;
    *h = pixels.height;
    return 1;
}

/* ---- WebGL 1 texture format rules (OpenGL ES 3 accepts more) ---- */

static bool webgl1_texture_ok(struct webgl *gl, GLenum internalformat, GLenum format, GLenum type) {
    if (gl->version == 2) {
        if (type == GL_HALF_FLOAT_OES) {
            synth(gl, GL_INVALID_ENUM);
            return false;
        }
        return true;
    }
    bool format_ok;
    switch (format) {
    case GL_ALPHA: case GL_LUMINANCE: case GL_LUMINANCE_ALPHA: case GL_RGB: case GL_RGBA: format_ok = true; break;
    case GL_DEPTH_COMPONENT: case GL_DEPTH_STENCIL: format_ok = ext(gl, X_WEBGL_depth_texture); break;
    case GL_SRGB_EXT: case GL_SRGB_ALPHA_EXT: format_ok = ext(gl, X_EXT_sRGB); break;
    default: format_ok = false;
    }
    bool type_ok;
    switch (type) {
    case GL_UNSIGNED_BYTE: case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1:
        type_ok = true;
        break;
    case GL_FLOAT: type_ok = ext(gl, X_OES_texture_float); break;
    case GL_HALF_FLOAT_OES: type_ok = ext(gl, X_OES_texture_half_float); break;
    case GL_UNSIGNED_SHORT: case GL_UNSIGNED_INT: case GL_UNSIGNED_INT_24_8:
        type_ok = ext(gl, X_WEBGL_depth_texture);
        break;
    default: type_ok = false;
    }
    if (!format_ok || !type_ok) {
        synth(gl, GL_INVALID_ENUM);
        return false;
    }
    if (internalformat != format) {
        synth(gl, GL_INVALID_OPERATION);
        return false;
    }
    return true;
}

/* ---- drawing validation ---- */

/* Rejects draws OpenGL ES would serve from client memory: enabled vertex
 * attributes without a buffer, and no program. */
static bool draw_ready(struct webgl *gl) {
    GLint program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    if (!program) {
        synth(gl, GL_INVALID_OPERATION);
        return false;
    }
    for (GLint i = 0; i < gl->max_attribs; i++) {
        GLint enabled = 0, buffer = 0;
        glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
        if (!enabled) continue;
        glGetVertexAttribiv((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buffer);
        if (!buffer) {
            synth(gl, GL_INVALID_OPERATION);
            return false;
        }
    }
    return true;
}

static unsigned index_size(struct webgl *gl, GLenum type) {
    switch (type) {
    case GL_UNSIGNED_BYTE: return 1;
    case GL_UNSIGNED_SHORT: return 2;
    case GL_UNSIGNED_INT: return gl->version == 2 || ext(gl, X_OES_element_index_uint) ? 4 : 0;
    default: return 0;
    }
}

/* Index data must come from the bound element buffer, within its size. */
static bool elements_ready(struct webgl *gl, GLsizei count, GLenum type, int64_t offset) {
    unsigned size = index_size(gl, type);
    if (!size) {
        synth(gl, GL_INVALID_ENUM);
        return false;
    }
    if (count < 0 || offset < 0) {
        synth(gl, GL_INVALID_VALUE);
        return false;
    }
    if (offset % size) {
        synth(gl, GL_INVALID_OPERATION);
        return false;
    }
    GLint buffer = 0;
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &buffer);
    if (!buffer) {
        synth(gl, GL_INVALID_OPERATION);
        return false;
    }
    GLint64 bytes = 0;
    if (gl->version == 2) {
        glGetBufferParameteri64v(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &bytes);
    } else {
        GLint b = 0;
        glGetBufferParameteriv(GL_ELEMENT_ARRAY_BUFFER, GL_BUFFER_SIZE, &b);
        bytes = b;
    }
    if ((uint64_t)offset + (uint64_t)count * size > (uint64_t)bytes) {
        synth(gl, GL_INVALID_OPERATION);
        return false;
    }
    return draw_ready(gl);
}

static bool mode_ok(struct webgl *gl, GLenum mode) {
    if (mode <= GL_TRIANGLE_FAN) return true;
    synth(gl, GL_INVALID_ENUM);
    return false;
}

FN(m_draw_arrays) {
    GLint v[3];
    if (ints(ctx, argc, argv, 0, 3, v)) return JS_EXCEPTION;
    CUR(JS_UNDEFINED);
    if (!mode_ok(gl, (GLenum)v[0])) return JS_UNDEFINED;
    if (v[1] < 0 || v[2] < 0) {
        synth(gl, GL_INVALID_VALUE);
        return JS_UNDEFINED;
    }
    if ((int64_t)v[1] + v[2] > INT32_MAX) {
        synth(gl, GL_INVALID_OPERATION);
        return JS_UNDEFINED;
    }
    if (!v[2] || !draw_ready(gl)) return JS_UNDEFINED;
    glDrawArrays((GLenum)v[0], v[1], v[2]);
    changed(gl);
    return JS_UNDEFINED;
}
FN(m_draw_elements) {
    GLint v[3];
    int64_t offset;
    if (ints(ctx, argc, argv, 0, 3, v) || toi64(ctx, argv[3], &offset)) return JS_EXCEPTION;
    CUR(JS_UNDEFINED);
    if (!mode_ok(gl, (GLenum)v[0]) || !elements_ready(gl, v[1], (GLenum)v[2], offset) || !v[1]) return JS_UNDEFINED;
    glDrawElements((GLenum)v[0], v[1], (GLenum)v[2], (const void *)(uintptr_t)offset);
    changed(gl);
    return JS_UNDEFINED;
}
static JSValue draw_arrays_instanced(JSContext *ctx, struct webgl *gl, int argc, JSValueConst *argv) {
    GLint v[4];
    if (ints(ctx, argc, argv, 0, 4, v)) return JS_EXCEPTION;
    CUR(JS_UNDEFINED);
    if (!mode_ok(gl, (GLenum)v[0])) return JS_UNDEFINED;
    if (v[1] < 0 || v[2] < 0 || v[3] < 0) {
        synth(gl, GL_INVALID_VALUE);
        return JS_UNDEFINED;
    }
    if (!v[2] || !v[3] || !draw_ready(gl)) return JS_UNDEFINED;
    glDrawArraysInstanced((GLenum)v[0], v[1], v[2], v[3]);
    changed(gl);
    return JS_UNDEFINED;
}
static JSValue draw_elements_instanced(JSContext *ctx, struct webgl *gl, int argc, JSValueConst *argv) {
    GLint v[3], instances;
    int64_t offset;
    if (ints(ctx, argc, argv, 0, 3, v) || toi64(ctx, argv[3], &offset) || toi(ctx, argv[4], &instances))
        return JS_EXCEPTION;
    CUR(JS_UNDEFINED);
    if (instances < 0) {
        synth(gl, GL_INVALID_VALUE);
        return JS_UNDEFINED;
    }
    if (!mode_ok(gl, (GLenum)v[0]) || !elements_ready(gl, v[1], (GLenum)v[2], offset) || !v[1] || !instances)
        return JS_UNDEFINED;
    glDrawElementsInstanced((GLenum)v[0], v[1], (GLenum)v[2], (const void *)(uintptr_t)offset, instances);
    changed(gl);
    return JS_UNDEFINED;
}
FN(m_draw_arrays_instanced) { return draw_arrays_instanced(ctx, gl, argc, argv); }
FN(m_draw_elements_instanced) { return draw_elements_instanced(ctx, gl, argc, argv); }
FN(m_draw_range_elements) {
    GLint v[5];
    int64_t offset;
    if (ints(ctx, argc, argv, 0, 5, v) || toi64(ctx, argv[5], &offset)) return JS_EXCEPTION;
    CUR(JS_UNDEFINED);
    if (!mode_ok(gl, (GLenum)v[0]) || !elements_ready(gl, v[3], (GLenum)v[4], offset) || !v[3]) return JS_UNDEFINED;
    glDrawRangeElements((GLenum)v[0], (GLuint)v[1], (GLuint)v[2], v[3], (GLenum)v[4], (const void *)(uintptr_t)offset);
    changed(gl);
    return JS_UNDEFINED;
}

/* ---- simple state ---- */

FN(m_active_texture) { GLint t; if (toi(ctx, argv[0], &t)) return JS_EXCEPTION; CUR(JS_UNDEFINED); glActiveTexture((GLenum)t); return JS_UNDEFINED; }
FN(m_blend_color) { GLfloat f[4]; if (floats(ctx, argc, argv, 0, 4, f)) return JS_EXCEPTION; CUR(JS_UNDEFINED); glBlendColor(f[0], f[1], f[2], f[3]); return JS_UNDEFINED; }
static bool blend_equation_ok(struct webgl *gl, GLenum e) {
    if (e == GL_FUNC_ADD || e == GL_FUNC_SUBTRACT || e == GL_FUNC_REVERSE_SUBTRACT) return true;
    if ((e == GL_MIN || e == GL_MAX) && (gl->version == 2 || ext(gl, X_EXT_blend_minmax))) return true;
    synth(gl, GL_INVALID_ENUM);
    return false;
}
FN(m_blend_equation) {
    GLint e; if (toi(ctx, argv[0], &e)) return JS_EXCEPTION; CUR(JS_UNDEFINED);
    if (blend_equation_ok(gl, (GLenum)e)) glBlendEquation((GLenum)e);
    return JS_UNDEFINED;
}
FN(m_blend_equation_separate) {
    GLint e[2]; if (ints(ctx, argc, argv, 0, 2, e)) return JS_EXCEPTION; CUR(JS_UNDEFINED);
    if (blend_equation_ok(gl, (GLenum)e[0]) && blend_equation_ok(gl, (GLenum)e[1])) glBlendEquationSeparate((GLenum)e[0], (GLenum)e[1]);
    return JS_UNDEFINED;
}
/* WebGL forbids mixing constant color and constant alpha factors. */
static bool blend_factors_ok(struct webgl *gl, GLenum a, GLenum b) {
    bool ca = a == GL_CONSTANT_COLOR || a == GL_ONE_MINUS_CONSTANT_COLOR, cb = b == GL_CONSTANT_COLOR || b == GL_ONE_MINUS_CONSTANT_COLOR;
    bool aa = a == GL_CONSTANT_ALPHA || a == GL_ONE_MINUS_CONSTANT_ALPHA, ab = b == GL_CONSTANT_ALPHA || b == GL_ONE_MINUS_CONSTANT_ALPHA;
    if ((ca && ab) || (aa && cb)) {
        synth(gl, GL_INVALID_OPERATION);
        return false;
    }
    return true;
}
FN(m_blend_func) {
    GLint f[2]; if (ints(ctx, argc, argv, 0, 2, f)) return JS_EXCEPTION; CUR(JS_UNDEFINED);
    if (blend_factors_ok(gl, (GLenum)f[0], (GLenum)f[1])) glBlendFunc((GLenum)f[0], (GLenum)f[1]);
    return JS_UNDEFINED;
}
FN(m_blend_func_separate) {
    GLint f[4]; if (ints(ctx, argc, argv, 0, 4, f)) return JS_EXCEPTION; CUR(JS_UNDEFINED);
    if (blend_factors_ok(gl, (GLenum)f[0], (GLenum)f[1])) glBlendFuncSeparate((GLenum)f[0], (GLenum)f[1], (GLenum)f[2], (GLenum)f[3]);
    return JS_UNDEFINED;
}
FN(m_clear) {
    GLuint mask; if (tou(ctx, argv[0], &mask)) return JS_EXCEPTION; CUR(JS_UNDEFINED);
    if (mask & ~(GLuint)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) {
        synth(gl, GL_INVALID_VALUE);
        return JS_UNDEFINED;
    }
    glClear(mask);
    changed(gl);
    return JS_UNDEFINED;
}
FN(m_clear_color) { GLfloat f[4]; if (floats(ctx, argc, argv, 0, 4, f)) return JS_EXCEPTION; CUR(JS_UNDEFINED); glClearColor(f[0], f[1], f[2], f[3]); return JS_UNDEFINED; }
FN(m_clear_depth) { GLfloat f; if (tof(ctx, argv[0], &f)) return JS_EXCEPTION; CUR(JS_UNDEFINED); glClearDepthf(f); return JS_UNDEFINED; }
FN(m_clear_stencil) { GLint s; if (toi(ctx, argv[0], &s)) return JS_EXCEPTION; CUR(JS_UNDEFINED); glClearStencil(s); return JS_UNDEFINED; }
FN(m_color_mask) {
    bool b[4]; for (int i = 0; i < 4; i++) b[i] = JS_ToBool(ctx, argv[i]);
    CUR(JS_UNDEFINED); glColorMask(b[0], b[1], b[2], b[3]); return JS_UNDEFINED;
}
FN(m_cull_face) { GLint m; if (toi(ctx, argv[0], &m)) return JS_EXCEPTION; CUR(JS_UNDEFINED); glCullFace((GLenum)m); return JS_UNDEFINED; }
FN(m_depth_func) { GLint m; if (toi(ctx, argv[0], &m)) return JS_EXCEPTION; CUR(JS_UNDEFINED); glDepthFunc((GLenum)m); return JS_UNDEFINED; }
FN(m_depth_mask) { bool b = JS_ToBool(ctx, argv[0]); CUR(JS_UNDEFINED); glDepthMask(b); return JS_UNDEFINED; }
FN(m_depth_range) {
    GLfloat f[2]; if (floats(ctx, argc, argv, 0, 2, f)) return JS_EXCEPTION; CUR(JS_UNDEFINED);
    if (f[0] > f[1]) {
        synth(gl, GL_INVALID_OPERATION);
        return JS_UNDEFINED;
    }
    glDepthRangef(f[0], f[1]);
    return JS_UNDEFINED;
}
FN(m_enable) {
    GLint c; if (toi(ctx, argv[0], &c)) return JS_EXCEPTION; CUR(JS_UNDEFINED);
    if (!capability_ok(gl, (GLenum)c)) synth(gl, GL_INVALID_ENUM);
    else glEnable((GLenum)c);
    return JS_UNDEFINED;
}
FN(m_disable) {
    GLint c; if (toi(ctx, argv[0], &c)) return JS_EXCEPTION; CUR(JS_UNDEFINED);
    if (!capability_ok(gl, (GLenum)c)) synth(gl, GL_INVALID_ENUM);
    else glDisable((GLenum)c);
    return JS_UNDEFINED;
}
FN(m_is_enabled) {
    GLint c; if (toi(ctx, argv[0], &c)) return JS_EXCEPTION; CUR(JS_FALSE);
    if (!capability_ok(gl, (GLenum)c)) {
        synth(gl, GL_INVALID_ENUM);
        return JS_FALSE;
    }
    return JS_NewBool(ctx, glIsEnabled((GLenum)c));
}
FN(m_finish) { UNUSED_ARGS; CUR(JS_UNDEFINED); glFinish(); return JS_UNDEFINED; }
FN(m_flush) { UNUSED_ARGS; CUR(JS_UNDEFINED); glFlush(); return JS_UNDEFINED; }
FN(m_front_face) { GLint m; if (toi(ctx, argv[0], &m)) return JS_EXCEPTION; CUR(JS_UNDEFINED); glFrontFace((GLenum)m); return JS_UNDEFINED; }
FN(m_hint) {
    GLint v[2]; if (ints(ctx, argc, argv, 0, 2, v)) return JS_EXCEPTION; CUR(JS_UNDEFINED);
    GLenum target = (GLenum)v[0];
    bool ok = target == GL_GENERATE_MIPMAP_HINT ||
              (target == GL_FRAGMENT_SHADER_DERIVATIVE_HINT && (gl->version == 2 || ext(gl, X_OES_standard_derivatives)));
    if (!ok) synth(gl, GL_INVALID_ENUM);
    else glHint(target, (GLenum)v[1]);
    return JS_UNDEFINED;
}
FN(m_line_width) { GLfloat f; if (tof(ctx, argv[0], &f)) return JS_EXCEPTION; CUR(JS_UNDEFINED); glLineWidth(f); return JS_UNDEFINED; }
FN(m_polygon_offset) { GLfloat f[2]; if (floats(ctx, argc, argv, 0, 2, f)) return JS_EXCEPTION; CUR(JS_UNDEFINED); glPolygonOffset(f[0], f[1]); return JS_UNDEFINED; }
FN(m_sample_coverage) {
    GLfloat f; if (tof(ctx, argv[0], &f)) return JS_EXCEPTION; bool b = JS_ToBool(ctx, argv[1]);
    CUR(JS_UNDEFINED); glSampleCoverage(f, b); return JS_UNDEFINED;
}
FN(m_scissor) { GLint v[4]; if (ints(ctx, argc, argv, 0, 4, v)) return JS_EXCEPTION; CUR(JS_UNDEFINED); glScissor(v[0], v[1], v[2], v[3]); return JS_UNDEFINED; }
FN(m_viewport) { GLint v[4]; if (ints(ctx, argc, argv, 0, 4, v)) return JS_EXCEPTION; CUR(JS_UNDEFINED); glViewport(v[0], v[1], v[2], v[3]); return JS_UNDEFINED; }
FN(m_stencil_func) { GLint v[3]; if (ints(ctx, argc, argv, 0, 3, v)) return JS_EXCEPTION; CUR(JS_UNDEFINED); glStencilFunc((GLenum)v[0], v[1], (GLuint)v[2]); return JS_UNDEFINED; }
FN(m_stencil_func_separate) { GLint v[4]; if (ints(ctx, argc, argv, 0, 4, v)) return JS_EXCEPTION; CUR(JS_UNDEFINED); glStencilFuncSeparate((GLenum)v[0], (GLenum)v[1], v[2], (GLuint)v[3]); return JS_UNDEFINED; }
FN(m_stencil_mask) { GLuint m; if (tou(ctx, argv[0], &m)) return JS_EXCEPTION; CUR(JS_UNDEFINED); glStencilMask(m); return JS_UNDEFINED; }
FN(m_stencil_mask_separate) { GLint f; GLuint m; if (toi(ctx, argv[0], &f) || tou(ctx, argv[1], &m)) return JS_EXCEPTION; CUR(JS_UNDEFINED); glStencilMaskSeparate((GLenum)f, m); return JS_UNDEFINED; }
FN(m_stencil_op) { GLint v[3]; if (ints(ctx, argc, argv, 0, 3, v)) return JS_EXCEPTION; CUR(JS_UNDEFINED); glStencilOp((GLenum)v[0], (GLenum)v[1], (GLenum)v[2]); return JS_UNDEFINED; }
FN(m_stencil_op_separate) { GLint v[4]; if (ints(ctx, argc, argv, 0, 4, v)) return JS_EXCEPTION; CUR(JS_UNDEFINED); glStencilOpSeparate((GLenum)v[0], (GLenum)v[1], (GLenum)v[2], (GLenum)v[3]); return JS_UNDEFINED; }

FN(m_pixel_storei) {
    GLint v[2]; if (ints(ctx, argc, argv, 0, 2, v)) return JS_EXCEPTION; CUR(JS_UNDEFINED);
    GLenum pname = (GLenum)v[0];
    GLint value = v[1];
    struct pixel_store *s = &gl->store;
    GLint *slot = NULL;
    switch (pname) {
    case WEBGL_UNPACK_FLIP_Y: gl->flip_y = value != 0; return JS_UNDEFINED;
    case WEBGL_UNPACK_PREMULTIPLY_ALPHA: gl->premultiply_alpha = value != 0; return JS_UNDEFINED;
    case WEBGL_UNPACK_COLORSPACE_CONVERSION:
        if (value != GL_NONE && value != WEBGL_BROWSER_DEFAULT) synth(gl, GL_INVALID_VALUE);
        else gl->colorspace_conversion = (GLenum)value;
        return JS_UNDEFINED;
    case GL_PACK_ALIGNMENT: slot = &s->pack_alignment; break;
    case GL_UNPACK_ALIGNMENT: slot = &s->unpack_alignment; break;
    }
    if (gl->version == 2) switch (pname) {
    case GL_PACK_ROW_LENGTH: slot = &s->pack_row_length; break;
    case GL_PACK_SKIP_PIXELS: slot = &s->pack_skip_pixels; break;
    case GL_PACK_SKIP_ROWS: slot = &s->pack_skip_rows; break;
    case GL_UNPACK_ROW_LENGTH: slot = &s->unpack_row_length; break;
    case GL_UNPACK_IMAGE_HEIGHT: slot = &s->unpack_image_height; break;
    case GL_UNPACK_SKIP_PIXELS: slot = &s->unpack_skip_pixels; break;
    case GL_UNPACK_SKIP_ROWS: slot = &s->unpack_skip_rows; break;
    case GL_UNPACK_SKIP_IMAGES: slot = &s->unpack_skip_images; break;
    }
    if (!slot) {
        synth(gl, GL_INVALID_ENUM);
        return JS_UNDEFINED;
    }
    bool alignment = pname == GL_PACK_ALIGNMENT || pname == GL_UNPACK_ALIGNMENT;
    if (alignment ? value != 1 && value != 2 && value != 4 && value != 8 : value < 0) {
        synth(gl, GL_INVALID_VALUE);
        return JS_UNDEFINED;
    }
    glPixelStorei(pname, value);
    *slot = value;
    return JS_UNDEFINED;
}

/* ---- errors, attributes, extensions ---- */

FN(m_get_error) {
    UNUSED_ARGS;
    if (gl->lost) {
        if (gl->lost_error) {
            gl->lost_error = false;
            return JS_NewInt32(ctx, WEBGL_CONTEXT_LOST);
        }
        return JS_NewInt32(ctx, GL_NO_ERROR);
    }
    if (gl->error_count) {
        GLenum e = gl->errors[0];
        memmove(gl->errors, gl->errors + 1, --gl->error_count * sizeof *gl->errors);
        return JS_NewInt32(ctx, (int32_t)e);
    }
    CUR(JS_NewInt32(ctx, GL_NO_ERROR));
    return JS_NewInt32(ctx, (int32_t)glGetError());
}
FN(m_is_context_lost) { UNUSED_ARGS; return JS_NewBool(ctx, gl->lost); }
FN(m_get_context_attributes) {
    UNUSED_ARGS;
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o)) return o;
    JS_SetPropertyStr(ctx, o, "alpha", JS_NewBool(ctx, gl->attrs.alpha));
    JS_SetPropertyStr(ctx, o, "antialias", JS_NewBool(ctx, gl->attrs.antialias));
    JS_SetPropertyStr(ctx, o, "depth", JS_NewBool(ctx, gl->attrs.depth));
    JS_SetPropertyStr(ctx, o, "desynchronized", JS_NewBool(ctx, gl->attrs.desynchronized));
    JS_SetPropertyStr(ctx, o, "failIfMajorPerformanceCaveat", JS_NewBool(ctx, gl->attrs.fail_caveat));
    JS_SetPropertyStr(ctx, o, "powerPreference", JS_NewString(ctx, gl->attrs.power));
    JS_SetPropertyStr(ctx, o, "premultipliedAlpha", JS_NewBool(ctx, gl->attrs.premultiplied));
    JS_SetPropertyStr(ctx, o, "preserveDrawingBuffer", JS_NewBool(ctx, gl->attrs.preserve));
    JS_SetPropertyStr(ctx, o, "stencil", JS_NewBool(ctx, gl->attrs.stencil));
    JS_SetPropertyStr(ctx, o, "xrCompatible", JS_FALSE);
    return o;
}
FN(m_get_supported_extensions) {
    UNUSED_ARGS;
    JSValue a = JS_NewArray(ctx);
    if (JS_IsException(a)) return a;
    uint32_t n = 0;
    for (int i = 0; i < X_COUNT; i++)
        if ((gl->available >> i) & 1) JS_SetPropertyUint32(ctx, a, n++, JS_NewString(ctx, extensions[i].name));
    return a;
}
FN(m_get_extension) {
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    int found = -1;
    for (int i = 0; i < X_COUNT; i++)
        if (((gl->available >> i) & 1) && !strcasecmp(name, extensions[i].name)) found = i;
    JS_FreeCString(ctx, name);
    (void)argc;
    if (found < 0 || gl->lost) return JS_NULL;
    if (JS_IsUndefined(gl->ext_object[found])) {
        JSValue o = JS_NewObjectClass(ctx, (int)ext_class[found]);
        if (JS_IsException(o)) return o;
        gl->refs++;
        JS_SetOpaque(o, gl);
        gl->ext_object[found] = o;
        gl->enabled |= 1u << found;
    }
    return JS_DupValue(ctx, gl->ext_object[found]);
}

/* ---- object creation, deletion, queries ---- */

static JSValue create_object(JSContext *ctx, struct webgl *gl, int kind) {
    if (!use(gl)) return JS_NULL;
    GLuint name = 0;
    switch (kind) {
    case O_BUFFER: glGenBuffers(1, &name); break;
    case O_TEXTURE: glGenTextures(1, &name); break;
    case O_FRAMEBUFFER: glGenFramebuffers(1, &name); break;
    case O_RENDERBUFFER: glGenRenderbuffers(1, &name); break;
    case O_PROGRAM: name = glCreateProgram(); break;
    case O_VAO: glGenVertexArrays(1, &name); break;
    case O_QUERY: glGenQueries(1, &name); break;
    case O_SAMPLER: glGenSamplers(1, &name); break;
    case O_TF: glGenTransformFeedbacks(1, &name); break;
    }
    if (!name) return JS_NULL;
    struct wobj *o;
    JSValue v = wrapper_new(ctx, gl, kind, &o);
    if (JS_IsException(v)) return v;
    o->name = name;
    if (!table_put(ctx, gl, kind, name, v)) {
        JS_FreeValue(ctx, v);
        return JS_ThrowOutOfMemory(ctx);
    }
    return v;
}
FN(m_create_buffer) { UNUSED_ARGS; return create_object(ctx, gl, O_BUFFER); }
FN(m_create_texture) { UNUSED_ARGS; return create_object(ctx, gl, O_TEXTURE); }
FN(m_create_framebuffer) { UNUSED_ARGS; return create_object(ctx, gl, O_FRAMEBUFFER); }
FN(m_create_renderbuffer) { UNUSED_ARGS; return create_object(ctx, gl, O_RENDERBUFFER); }
FN(m_create_program) { UNUSED_ARGS; return create_object(ctx, gl, O_PROGRAM); }
FN(m_create_vertex_array) { UNUSED_ARGS; return create_object(ctx, gl, O_VAO); }
FN(m_create_query) { UNUSED_ARGS; return create_object(ctx, gl, O_QUERY); }
FN(m_create_sampler) { UNUSED_ARGS; return create_object(ctx, gl, O_SAMPLER); }
FN(m_create_transform_feedback) { UNUSED_ARGS; return create_object(ctx, gl, O_TF); }
FN(m_create_shader) {
    GLint type; if (toi(ctx, argv[0], &type)) return JS_EXCEPTION; CUR(JS_NULL);
    if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER) {
        synth(gl, GL_INVALID_ENUM);
        return JS_NULL;
    }
    GLuint name = glCreateShader((GLenum)type);
    if (!name) return JS_NULL;
    struct wobj *o;
    JSValue v = wrapper_new(ctx, gl, O_SHADER, &o);
    if (JS_IsException(v)) return v;
    o->name = name;
    if (!table_put(ctx, gl, O_SHADER, name, v)) {
        JS_FreeValue(ctx, v);
        return JS_ThrowOutOfMemory(ctx);
    }
    return v;
}

static JSValue delete_object(JSContext *ctx, struct webgl *gl, JSValueConst value, int kind) {
    struct wobj *o;
    if (object_arg(ctx, value, kind, true, &o)) return JS_EXCEPTION;
    CUR(JS_UNDEFINED);
    if (!o || !owned(gl, o) || o->deleted) return JS_UNDEFINED;
    o->deleted = true;
    GLuint name = o->name;
    switch (kind) {
    case O_BUFFER: glDeleteBuffers(1, &name); break;
    case O_TEXTURE: glDeleteTextures(1, &name); break;
    case O_FRAMEBUFFER: glDeleteFramebuffers(1, &name); break;
    case O_RENDERBUFFER: glDeleteRenderbuffers(1, &name); break;
    case O_PROGRAM: glDeleteProgram(name); break;
    case O_SHADER: glDeleteShader(name); break;
    case O_VAO: glDeleteVertexArrays(1, &name); break;
    case O_QUERY: glDeleteQueries(1, &name); break;
    case O_SAMPLER: glDeleteSamplers(1, &name); break;
    case O_TF: glDeleteTransformFeedbacks(1, &name); break;
    }
    return JS_UNDEFINED;
}
FN(m_delete_buffer) { UNUSED_ARGS; return delete_object(ctx, gl, argv[0], O_BUFFER); }
FN(m_delete_texture) { UNUSED_ARGS; return delete_object(ctx, gl, argv[0], O_TEXTURE); }
FN(m_delete_framebuffer) { UNUSED_ARGS; return delete_object(ctx, gl, argv[0], O_FRAMEBUFFER); }
FN(m_delete_renderbuffer) { UNUSED_ARGS; return delete_object(ctx, gl, argv[0], O_RENDERBUFFER); }
FN(m_delete_program) { UNUSED_ARGS; return delete_object(ctx, gl, argv[0], O_PROGRAM); }
FN(m_delete_shader) { UNUSED_ARGS; return delete_object(ctx, gl, argv[0], O_SHADER); }
FN(m_delete_vertex_array) { UNUSED_ARGS; return delete_object(ctx, gl, argv[0], O_VAO); }
FN(m_delete_query) { UNUSED_ARGS; return delete_object(ctx, gl, argv[0], O_QUERY); }
FN(m_delete_sampler) { UNUSED_ARGS; return delete_object(ctx, gl, argv[0], O_SAMPLER); }
FN(m_delete_transform_feedback) { UNUSED_ARGS; return delete_object(ctx, gl, argv[0], O_TF); }

static JSValue is_object(JSContext *ctx, struct webgl *gl, JSValueConst value, int kind) {
    struct wobj *o;
    if (object_arg(ctx, value, kind, true, &o)) return JS_EXCEPTION;
    if (!o || o->gl != gl || o->generation != gl->generation || o->deleted || !use(gl)) return JS_FALSE;
    GLboolean r = GL_FALSE;
    switch (kind) {
    case O_BUFFER: r = glIsBuffer(o->name); break;
    case O_TEXTURE: r = glIsTexture(o->name); break;
    case O_FRAMEBUFFER: r = glIsFramebuffer(o->name); break;
    case O_RENDERBUFFER: r = glIsRenderbuffer(o->name); break;
    case O_PROGRAM: r = glIsProgram(o->name); break;
    case O_SHADER: r = glIsShader(o->name); break;
    case O_VAO: r = glIsVertexArray(o->name); break;
    case O_QUERY: r = glIsQuery(o->name); break;
    case O_SAMPLER: r = glIsSampler(o->name); break;
    case O_TF: r = glIsTransformFeedback(o->name); break;
    }
    return JS_NewBool(ctx, r);
}
FN(m_is_buffer) { UNUSED_ARGS; return is_object(ctx, gl, argv[0], O_BUFFER); }
FN(m_is_texture) { UNUSED_ARGS; return is_object(ctx, gl, argv[0], O_TEXTURE); }
FN(m_is_framebuffer) { UNUSED_ARGS; return is_object(ctx, gl, argv[0], O_FRAMEBUFFER); }
FN(m_is_renderbuffer) { UNUSED_ARGS; return is_object(ctx, gl, argv[0], O_RENDERBUFFER); }
FN(m_is_program) { UNUSED_ARGS; return is_object(ctx, gl, argv[0], O_PROGRAM); }
FN(m_is_shader) { UNUSED_ARGS; return is_object(ctx, gl, argv[0], O_SHADER); }
FN(m_is_vertex_array) { UNUSED_ARGS; return is_object(ctx, gl, argv[0], O_VAO); }
FN(m_is_query) { UNUSED_ARGS; return is_object(ctx, gl, argv[0], O_QUERY); }
FN(m_is_sampler) { UNUSED_ARGS; return is_object(ctx, gl, argv[0], O_SAMPLER); }
FN(m_is_transform_feedback) { UNUSED_ARGS; return is_object(ctx, gl, argv[0], O_TF); }

/* ---- bindings ---- */

static bool bindable(struct webgl *gl, struct wobj *o) {
    if (!owned(gl, o)) return false;
    if (o && o->deleted) {
        synth(gl, GL_INVALID_OPERATION);
        return false;
    }
    return true;
}
FN(m_bind_buffer) {
    GLint target; struct wobj *o;
    if (toi(ctx, argv[0], &target) || object_arg(ctx, argv[1], O_BUFFER, true, &o)) return JS_EXCEPTION;
    CUR(JS_UNDEFINED);
    if (!buffer_target(gl, (GLenum)target)) {
        synth(gl, GL_INVALID_ENUM);
        return JS_UNDEFINED;
    }
    if (!bindable(gl, o)) return JS_UNDEFINED;
    if (o) {
        uint8_t kind = target == GL_ELEMENT_ARRAY_BUFFER ? 1 : 2;
        if (o->buffer_kind && o->buffer_kind != kind) {
            synth(gl, GL_INVALID_OPERATION);
            return JS_UNDEFINED;
        }
        o->buffer_kind = kind;
    }
    glBindBuffer((GLenum)target, name_of(o));
    return JS_UNDEFINED;
}
FN(m_bind_buffer_base) {
    GLint v[2]; struct wobj *o;
    if (ints(ctx, argc, argv, 0, 2, v) || object_arg(ctx, argv[2], O_BUFFER, true, &o)) return JS_EXCEPTION;
    CUR(JS_UNDEFINED);
    if (v[0] != GL_TRANSFORM_FEEDBACK_BUFFER && v[0] != GL_UNIFORM_BUFFER) {
        synth(gl, GL_INVALID_ENUM);
        return JS_UNDEFINED;
    }
    if (!bindable(gl, o)) return JS_UNDEFINED;
    if (o) {
        if (o->buffer_kind == 1) {
            synth(gl, GL_INVALID_OPERATION);
            return JS_UNDEFINED;
        }
        o->buffer_kind = 2;
    }
    glBindBufferBase((GLenum)v[0], (GLuint)v[1], name_of(o));
    return JS_UNDEFINED;
}
FN(m_bind_buffer_range) {
    GLint v[2]; struct wobj *o; int64_t offset, size;
    if (ints(ctx, argc, argv, 0, 2, v) || object_arg(ctx, argv[2], O_BUFFER, true, &o) || toi64(ctx, argv[3], &offset) ||
        toi64(ctx, argv[4], &size))
        return JS_EXCEPTION;
    CUR(JS_UNDEFINED);
    if (v[0] != GL_TRANSFORM_FEEDBACK_BUFFER && v[0] != GL_UNIFORM_BUFFER) {
        synth(gl, GL_INVALID_ENUM);
        return JS_UNDEFINED;
    }
    if (!bindable(gl, o)) return JS_UNDEFINED;
    if (o) {
        if (o->buffer_kind == 1) {
            synth(gl, GL_INVALID_OPERATION);
            return JS_UNDEFINED;
        }
        o->buffer_kind = 2;
    }
    glBindBufferRange((GLenum)v[0], (GLuint)v[1], name_of(o), (GLintptr)offset, (GLsizeiptr)size);
    return JS_UNDEFINED;
}
FN(m_bind_framebuffer) {
    GLint target; struct wobj *o;
    if (toi(ctx, argv[0], &target) || object_arg(ctx, argv[1], O_FRAMEBUFFER, true, &o)) return JS_EXCEPTION;
    CUR(JS_UNDEFINED);
    if (!framebuffer_target(gl, (GLenum)target)) {
        synth(gl, GL_INVALID_ENUM);
        return JS_UNDEFINED;
    }
    if (bindable(gl, o)) glBindFramebuffer((GLenum)target, name_of(o));
    return JS_UNDEFINED;
}
FN(m_bind_renderbuffer) {
    GLint target; struct wobj *o;
    if (toi(ctx, argv[0], &target) || object_arg(ctx, argv[1], O_RENDERBUFFER, true, &o)) return JS_EXCEPTION;
    CUR(JS_UNDEFINED);
    if (target != GL_RENDERBUFFER) {
        synth(gl, GL_INVALID_ENUM);
        return JS_UNDEFINED;
    }
    if (bindable(gl, o)) glBindRenderbuffer(GL_RENDERBUFFER, name_of(o));
    return JS_UNDEFINED;
}
FN(m_bind_texture) {
    GLint target; struct wobj *o;
    if (toi(ctx, argv[0], &target) || object_arg(ctx, argv[1], O_TEXTURE, true, &o)) return JS_EXCEPTION;
    CUR(JS_UNDEFINED);
    if (!texture_target(gl, (GLenum)target)) {
        synth(gl, GL_INVALID_ENUM);
        return JS_UNDEFINED;
    }
    if (bindable(gl, o)) glBindTexture((GLenum)target, name_of(o));
    return JS_UNDEFINED;
}
FN(m_bind_vertex_array) {
    struct wobj *o;
    if (object_arg(ctx, argv[0], O_VAO, true, &o)) return JS_EXCEPTION;
    CUR(JS_UNDEFINED);
    if (bindable(gl, o)) glBindVertexArray(name_of(o));
    return JS_UNDEFINED;
}
FN(m_bind_sampler) {
    GLuint unit; struct wobj *o;
    if (tou(ctx, argv[0], &unit) || object_arg(ctx, argv[1], O_SAMPLER, true, &o)) return JS_EXCEPTION;
    CUR(JS_UNDEFINED);
    if (bindable(gl, o)) glBindSampler(unit, name_of(o));
    return JS_UNDEFINED;
}
FN(m_bind_transform_feedback) {
    GLint target; struct wobj *o;
    if (toi(ctx, argv[0], &target) || object_arg(ctx, argv[1], O_TF, true, &o)) return JS_EXCEPTION;
    CUR(JS_UNDEFINED);
    if (bindable(gl, o)) glBindTransformFeedback((GLenum)target, name_of(o));
    return JS_UNDEFINED;
}

/* ---- buffers ---- */

FN(m_buffer_data) {
    GLint target, usage;
    if (toi(ctx, argv[0], &target) || toi(ctx, argv[2], &usage)) return JS_EXCEPTION;
    uint32_t src_offset = 0, length = 0;
    bool ranged = gl->version == 2 && argc > 3;
    if (ranged && (tou(ctx, argv[3], &src_offset) || (argc > 4 && tou(ctx, argv[4], &length)))) return JS_EXCEPTION;
    int64_t size = -1;
    struct view view = {0};
    bool have_data = false;
    if (JS_IsObject(argv[1])) {
        if (!source_get(ctx, argv[1], &view)) return JS_ThrowTypeError(ctx, "bufferData requires a BufferSource or size");
        have_data = true;
    } else if (JS_IsNull(argv[1])) {
        CUR(JS_UNDEFINED);
        synth(gl, GL_INVALID_VALUE);
        return JS_UNDEFINED;
    } else if (toi64(ctx, argv[1], &size)) {
        return JS_EXCEPTION;
    }
    CUR(JS_UNDEFINED);
    if (!buffer_target(gl, (GLenum)target) || !usage_ok(gl, (GLenum)usage)) {
        synth(gl, GL_INVALID_ENUM);
        return JS_UNDEFINED;
    }
    if (have_data) {
        if (have_data && ranged && (view_type(argv[1]) < 0 || !view_range(&view, src_offset, length))) {
            synth(gl, GL_INVALID_VALUE);
            return JS_UNDEFINED;
        }
        glBufferData((GLenum)target, (GLsizeiptr)view.bytes, view.bytes ? view.data : NULL, (GLenum)usage);
    } else {
        if (size < 0) {
            synth(gl, GL_INVALID_VALUE);
            return JS_UNDEFINED;
        }
        glBufferData((GLenum)target, (GLsizeiptr)size, NULL, (GLenum)usage);
    }
    return JS_UNDEFINED;
}
FN(m_buffer_sub_data) {
    GLint target; int64_t offset;
    if (toi(ctx, argv[0], &target) || toi64(ctx, argv[1], &offset)) return JS_EXCEPTION;
    uint32_t src_offset = 0, length = 0;
    bool ranged = gl->version == 2 && argc > 3;
    if (ranged && (tou(ctx, argv[3], &src_offset) || (argc > 4 && tou(ctx, argv[4], &length)))) return JS_EXCEPTION;
    struct view view;
    if (!JS_IsObject(argv[2]) || !source_get(ctx, argv[2], &view))
        return JS_ThrowTypeError(ctx, "bufferSubData requires a BufferSource");
    CUR(JS_UNDEFINED);
    if (!buffer_target(gl, (GLenum)target)) {
        synth(gl, GL_INVALID_ENUM);
        return JS_UNDEFINED;
    }
    if (offset < 0 || (ranged && (view_type(argv[2]) < 0 || !view_range(&view, src_offset, length)))) {
        synth(gl, GL_INVALID_VALUE);
        return JS_UNDEFINED;
    }
    if (view.bytes) glBufferSubData((GLenum)target, (GLintptr)offset, (GLsizeiptr)view.bytes, view.data);
    return JS_UNDEFINED;
}
FN(m_copy_buffer_sub_data) {
    GLint t[2]; int64_t r[3];
    if (ints(ctx, argc, argv, 0, 2, t) || toi64(ctx, argv[2], &r[0]) || toi64(ctx, argv[3], &r[1]) ||
        toi64(ctx, argv[4], &r[2]))
        return JS_EXCEPTION;
    CUR(JS_UNDEFINED);
    if (!buffer_target(gl, (GLenum)t[0]) || !buffer_target(gl, (GLenum)t[1])) {
        synth(gl, GL_INVALID_ENUM);
        return JS_UNDEFINED;
    }
    GLint a = 0, b = 0;
    glGetIntegerv(buffer_binding((GLenum)t[0]), &a);
    glGetIntegerv(buffer_binding((GLenum)t[1]), &b);
    struct wobj *ra = table_object(gl, O_BUFFER, (GLuint)a), *wb = table_object(gl, O_BUFFER, (GLuint)b);
    if (ra && wb && ra->buffer_kind != wb->buffer_kind) {
        synth(gl, GL_INVALID_OPERATION);
        return JS_UNDEFINED;
    }
    glCopyBufferSubData((GLenum)t[0], (GLenum)t[1], (GLintptr)r[0], (GLintptr)r[1], (GLsizeiptr)r[2]);
    return JS_UNDEFINED;
}
FN(m_get_buffer_sub_data) {
    GLint target; int64_t offset; uint32_t dst_offset = 0, length = 0;
    if (toi(ctx, argv[0], &target) || toi64(ctx, argv[1], &offset)) return JS_EXCEPTION;
    if ((argc > 3 && tou(ctx, argv[3], &dst_offset)) || (argc > 4 && tou(ctx, argv[4], &length))) return JS_EXCEPTION;
    struct view view;
    if (!view_get(ctx, argv[2], &view)) return JS_ThrowTypeError(ctx, "getBufferSubData requires an ArrayBufferView");
    CUR(JS_UNDEFINED);
    if (!buffer_target(gl, (GLenum)target)) {
        synth(gl, GL_INVALID_ENUM);
        return JS_UNDEFINED;
    }
    if (offset < 0 || !view_range(&view, dst_offset, length)) {
        synth(gl, GL_INVALID_VALUE);
        return JS_UNDEFINED;
    }
    if (!view.bytes) return JS_UNDEFINED;
    void *map = glMapBufferRange((GLenum)target, (GLintptr)offset, (GLsizeiptr)view.bytes, GL_MAP_READ_BIT);
    if (!map) return JS_UNDEFINED;
    memcpy(view.data, map, view.bytes);
    glUnmapBuffer((GLenum)target);
    return JS_UNDEFINED;
}
FN(m_get_buffer_parameter) {
    GLint v[2]; if (ints(ctx, argc, argv, 0, 2, v)) return JS_EXCEPTION; CUR(JS_NULL);
    if (!buffer_target(gl, (GLenum)v[0]) || (v[1] != GL_BUFFER_SIZE && v[1] != GL_BUFFER_USAGE)) {
        synth(gl, GL_INVALID_ENUM);
        return JS_NULL;
    }
    if (v[1] == GL_BUFFER_SIZE && gl->version == 2) {
        GLint64 size = 0;
        glGetBufferParameteri64v((GLenum)v[0], GL_BUFFER_SIZE, &size);
        return JS_NewFloat64(ctx, (double)size);
    }
    GLint value = 0;
    glGetBufferParameteriv((GLenum)v[0], (GLenum)v[1], &value);
    return JS_NewInt32(ctx, value);
}

/* ---- shaders and programs ---- */

/* WebGL 1 accepts GLSL ES 1.00, WebGL 2 also 3.00; Mesa would compile
 * the later versions its context supports. */
static bool shader_version_ok(const char *s, int webgl) {
    for (;;) {
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n' || *s == '\f' || *s == '\v') s++;
        if (s[0] == '/' && s[1] == '/') {
            while (*s && *s != '\n') s++;
            continue;
        }
        if (s[0] == '/' && s[1] == '*') {
            const char *end = strstr(s + 2, "*/");
            if (!end) return true;
            s = end + 2;
            continue;
        }
        break;
    }
    if (*s != '#') return true;
    s++;
    while (*s == ' ' || *s == '\t') s++;
    if (strncmp(s, "version", 7)) return true;
    s += 7;
    while (*s == ' ' || *s == '\t') s++;
    long version = strtol(s, NULL, 10);
    return version == 100 || (webgl == 2 && version == 300);
}
FN(m_shader_source) {
    struct wobj *o;
    if (object_arg(ctx, argv[0], O_SHADER, false, &o)) return JS_EXCEPTION;
    size_t length;
    const char *text = JS_ToCStringLen(ctx, &length, argv[1]);
    if (!text) return JS_EXCEPTION;
    (void)argc;
    if (!use(gl) || !owned(gl, o) || o->deleted) {
        if (!gl->lost && o->deleted) synth(gl, GL_INVALID_VALUE);
        JS_FreeCString(ctx, text);
        return JS_UNDEFINED;
    }
    char *copy = malloc(length + 1);
    if (!copy) {
        JS_FreeCString(ctx, text);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(copy, text, length + 1);
    free(o->source);
    o->source = copy;
    const GLchar *strings[1] = {copy};
    GLint lengths[1] = {(GLint)length};
    glShaderSource(o->name, 1, strings, lengths);
    JS_FreeCString(ctx, text);
    return JS_UNDEFINED;
}
FN(m_compile_shader) {
    struct wobj *o;
    if (object_arg(ctx, argv[0], O_SHADER, false, &o)) return JS_EXCEPTION;
    UNUSED_ARGS;
    CUR(JS_UNDEFINED);
    if (!owned(gl, o)) return JS_UNDEFINED;
    if (o->deleted) {
        synth(gl, GL_INVALID_VALUE);
        return JS_UNDEFINED;
    }
    if (o->source && !shader_version_ok(o->source, gl->version)) {
        /* Let GL itself fail the compile so status and log stay coherent. */
        static const GLchar *rejected = "#version 100\n#error unsupported GLSL ES version for this WebGL version\n";
        glShaderSource(o->name, 1, &rejected, NULL);
        glCompileShader(o->name);
        const GLchar *strings[1] = {o->source};
        glShaderSource(o->name, 1, strings, NULL);
        return JS_UNDEFINED;
    }
    glCompileShader(o->name);
    return JS_UNDEFINED;
}
FN(m_get_shader_source) {
    struct wobj *o;
    if (object_arg(ctx, argv[0], O_SHADER, false, &o)) return JS_EXCEPTION;
    UNUSED_ARGS;
    CUR(JS_NULL);
    if (!owned(gl, o)) return JS_NULL;
    if (o->deleted) {
        synth(gl, GL_INVALID_VALUE);
        return JS_NULL;
    }
    return JS_NewString(ctx, o->source ? o->source : "");
}
static bool program_usable(struct webgl *gl, struct wobj *o) {
    if (!owned(gl, o)) return false;
    if (o && o->deleted) {
        synth(gl, GL_INVALID_VALUE);
        return false;
    }
    return true;
}
FN(m_attach_shader) {
    struct wobj *p, *s;
    if (object_arg(ctx, argv[0], O_PROGRAM, false, &p) || object_arg(ctx, argv[1], O_SHADER, false, &s)) return JS_EXCEPTION;
    UNUSED_ARGS;
    CUR(JS_UNDEFINED);
    if (program_usable(gl, p) && program_usable(gl, s)) glAttachShader(p->name, s->name);
    return JS_UNDEFINED;
}
FN(m_detach_shader) {
    struct wobj *p, *s;
    if (object_arg(ctx, argv[0], O_PROGRAM, false, &p) || object_arg(ctx, argv[1], O_SHADER, false, &s)) return JS_EXCEPTION;
    UNUSED_ARGS;
    CUR(JS_UNDEFINED);
    if (program_usable(gl, p) && program_usable(gl, s)) glDetachShader(p->name, s->name);
    return JS_UNDEFINED;
}
FN(m_link_program) {
    struct wobj *p;
    if (object_arg(ctx, argv[0], O_PROGRAM, false, &p)) return JS_EXCEPTION;
    UNUSED_ARGS;
    CUR(JS_UNDEFINED);
    if (!program_usable(gl, p)) return JS_UNDEFINED;
    p->link_serial++;
    glLinkProgram(p->name);
    return JS_UNDEFINED;
}
FN(m_validate_program) {
    struct wobj *p;
    if (object_arg(ctx, argv[0], O_PROGRAM, false, &p)) return JS_EXCEPTION;
    UNUSED_ARGS;
    CUR(JS_UNDEFINED);
    if (program_usable(gl, p)) glValidateProgram(p->name);
    return JS_UNDEFINED;
}
FN(m_use_program) {
    struct wobj *p;
    if (object_arg(ctx, argv[0], O_PROGRAM, true, &p)) return JS_EXCEPTION;
    UNUSED_ARGS;
    CUR(JS_UNDEFINED);
    if (program_usable(gl, p)) glUseProgram(name_of(p));
    return JS_UNDEFINED;
}
static bool name_ok(struct webgl *gl, const char *name, size_t length, bool *reserved) {
    *reserved = !strncmp(name, "webgl_", 6) || !strncmp(name, "_webgl_", 7);
    if (length > (gl->version == 2 ? 1024u : 256u)) {
        synth(gl, GL_INVALID_VALUE);
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 32 || c > 126 || c == '"' || c == '$' || c == '\'' || c == '@' || c == '\\' || c == '`') {
            synth(gl, GL_INVALID_VALUE);
            return false;
        }
    }
    return true;
}
FN(m_bind_attrib_location) {
    struct wobj *p; GLuint index; size_t length;
    if (object_arg(ctx, argv[0], O_PROGRAM, false, &p) || tou(ctx, argv[1], &index)) return JS_EXCEPTION;
    const char *name = JS_ToCStringLen(ctx, &length, argv[2]);
    if (!name) return JS_EXCEPTION;
    (void)argc;
    bool reserved;
    if (use(gl) && program_usable(gl, p) && name_ok(gl, name, length, &reserved)) {
        if (reserved) synth(gl, GL_INVALID_OPERATION);
        else glBindAttribLocation(p->name, index, name);
    }
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}
static JSValue location_query(JSContext *ctx, struct webgl *gl, JSValueConst *argv, bool fragment) {
    struct wobj *p; size_t length;
    if (object_arg(ctx, argv[0], O_PROGRAM, false, &p)) return JS_EXCEPTION;
    const char *name = JS_ToCStringLen(ctx, &length, argv[1]);
    if (!name) return JS_EXCEPTION;
    GLint r = -1;
    bool reserved;
    if (use(gl) && program_usable(gl, p) && name_ok(gl, name, length, &reserved) && !reserved)
        r = fragment ? glGetFragDataLocation(p->name, name) : glGetAttribLocation(p->name, name);
    JS_FreeCString(ctx, name);
    return JS_NewInt32(ctx, r);
}
FN(m_get_attrib_location) { (void)argc; return location_query(ctx, gl, argv, false); }
FN(m_get_frag_data_location) { (void)argc; return location_query(ctx, gl, argv, true); }

static JSValue string_log(JSContext *ctx, GLuint name, bool program) {
    GLint length = 0;
    if (program) glGetProgramiv(name, GL_INFO_LOG_LENGTH, &length);
    else glGetShaderiv(name, GL_INFO_LOG_LENGTH, &length);
    if (length <= 0) return JS_NewString(ctx, "");
    char *log = malloc((size_t)length);
    if (!log) return JS_ThrowOutOfMemory(ctx);
    GLsizei written = 0;
    if (program) glGetProgramInfoLog(name, length, &written, log);
    else glGetShaderInfoLog(name, length, &written, log);
    JSValue s = JS_NewStringLen(ctx, log, (size_t)written);
    free(log);
    return s;
}
FN(m_get_program_info_log) {
    struct wobj *p;
    if (object_arg(ctx, argv[0], O_PROGRAM, false, &p)) return JS_EXCEPTION;
    UNUSED_ARGS;
    CUR(JS_NULL);
    if (!program_usable(gl, p)) return JS_NULL;
    return string_log(ctx, p->name, true);
}
FN(m_get_shader_info_log) {
    struct wobj *s;
    if (object_arg(ctx, argv[0], O_SHADER, false, &s)) return JS_EXCEPTION;
    UNUSED_ARGS;
    CUR(JS_NULL);
    if (!program_usable(gl, s)) return JS_NULL;
    return string_log(ctx, s->name, false);
}
FN(m_get_program_parameter) {
    struct wobj *p; GLint pname;
    if (object_arg(ctx, argv[0], O_PROGRAM, false, &p) || toi(ctx, argv[1], &pname)) return JS_EXCEPTION;
    (void)argc;
    CUR(JS_NULL);
    if (!owned(gl, p)) return JS_NULL;
    if (pname == KHR_COMPLETION_STATUS && ext(gl, X_KHR_parallel_shader_compile)) return JS_TRUE; /* linking is synchronous */
    if (pname == GL_DELETE_STATUS) return JS_NewBool(ctx, p->deleted);
    if (p->deleted) {
        synth(gl, GL_INVALID_VALUE);
        return JS_NULL;
    }
    GLint value = 0;
    switch (pname) {
    case GL_LINK_STATUS: case GL_VALIDATE_STATUS:
        glGetProgramiv(p->name, (GLenum)pname, &value);
        return JS_NewBool(ctx, value);
    case GL_ATTACHED_SHADERS: case GL_ACTIVE_ATTRIBUTES: case GL_ACTIVE_UNIFORMS:
        glGetProgramiv(p->name, (GLenum)pname, &value);
        return JS_NewInt32(ctx, value);
    case GL_TRANSFORM_FEEDBACK_BUFFER_MODE: case GL_TRANSFORM_FEEDBACK_VARYINGS: case GL_ACTIVE_UNIFORM_BLOCKS:
        if (gl->version == 2) {
            glGetProgramiv(p->name, (GLenum)pname, &value);
            return JS_NewInt32(ctx, value);
        }
        break;
    }
    synth(gl, GL_INVALID_ENUM);
    return JS_NULL;
}
FN(m_get_shader_parameter) {
    struct wobj *s; GLint pname;
    if (object_arg(ctx, argv[0], O_SHADER, false, &s) || toi(ctx, argv[1], &pname)) return JS_EXCEPTION;
    (void)argc;
    CUR(JS_NULL);
    if (!owned(gl, s)) return JS_NULL;
    if (pname == KHR_COMPLETION_STATUS && ext(gl, X_KHR_parallel_shader_compile)) return JS_TRUE;
    if (pname == GL_DELETE_STATUS) return JS_NewBool(ctx, s->deleted);
    if (s->deleted) {
        synth(gl, GL_INVALID_VALUE);
        return JS_NULL;
    }
    GLint value = 0;
    if (pname == GL_COMPILE_STATUS) {
        glGetShaderiv(s->name, GL_COMPILE_STATUS, &value);
        return JS_NewBool(ctx, value);
    }
    if (pname == GL_SHADER_TYPE) {
        glGetShaderiv(s->name, GL_SHADER_TYPE, &value);
        return JS_NewInt32(ctx, value);
    }
    synth(gl, GL_INVALID_ENUM);
    return JS_NULL;
}
FN(m_get_attached_shaders) {
    struct wobj *p;
    if (object_arg(ctx, argv[0], O_PROGRAM, false, &p)) return JS_EXCEPTION;
    UNUSED_ARGS;
    CUR(JS_NULL);
    if (!program_usable(gl, p)) return JS_NULL;
    GLuint shaders[8];
    GLsizei count = 0;
    glGetAttachedShaders(p->name, 8, &count, shaders);
    JSValue a = JS_NewArray(ctx);
    for (GLsizei i = 0; i < count && !JS_IsException(a); i++)
        JS_SetPropertyUint32(ctx, a, (uint32_t)i, table_get(ctx, gl, O_SHADER, shaders[i]));
    return a;
}
static JSValue active_info(JSContext *ctx, struct webgl *gl, GLint size, GLenum type, const char *name, size_t length) {
    struct wobj *o;
    JSValue v = wrapper_new(ctx, gl, O_INFO, &o);
    if (JS_IsException(v)) return v;
    o->size = size;
    o->type = type;
    o->text = malloc(length + 1);
    if (!o->text) {
        JS_FreeValue(ctx, v);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(o->text, name, length);
    o->text[length] = 0;
    return v;
}
static JSValue active_query(JSContext *ctx, struct webgl *gl, JSValueConst *argv, int which) {
    struct wobj *p; GLuint index;
    if (object_arg(ctx, argv[0], O_PROGRAM, false, &p) || tou(ctx, argv[1], &index)) return JS_EXCEPTION;
    CUR(JS_NULL);
    if (!program_usable(gl, p)) return JS_NULL;
    GLint max = 0;
    GLenum pname = which == 0 ? GL_ACTIVE_ATTRIBUTE_MAX_LENGTH
                   : which == 1 ? GL_ACTIVE_UNIFORM_MAX_LENGTH : GL_TRANSFORM_FEEDBACK_VARYING_MAX_LENGTH;
    glGetProgramiv(p->name, pname, &max);
    if (max < 1) max = 1;
    char *name = malloc((size_t)max);
    if (!name) return JS_ThrowOutOfMemory(ctx);
    GLsizei length = 0;
    GLint size = 0;
    GLenum type = 0;
    name[0] = 0;
    if (which == 0) glGetActiveAttrib(p->name, index, max, &length, &size, &type, name);
    else if (which == 1) glGetActiveUniform(p->name, index, max, &length, &size, &type, name);
    else glGetTransformFeedbackVarying(p->name, index, max, &length, &size, &type, name);
    JSValue r = type ? active_info(ctx, gl, size, type, name, (size_t)length) : JS_NULL;
    free(name);
    return r;
}
FN(m_get_active_attrib) { (void)argc; return active_query(ctx, gl, argv, 0); }
FN(m_get_active_uniform) { (void)argc; return active_query(ctx, gl, argv, 1); }
FN(m_get_transform_feedback_varying) { (void)argc; return active_query(ctx, gl, argv, 2); }

FN(m_get_shader_precision_format) {
    GLint v[2]; if (ints(ctx, argc, argv, 0, 2, v)) return JS_EXCEPTION; CUR(JS_NULL);
    bool shader = v[0] == GL_VERTEX_SHADER || v[0] == GL_FRAGMENT_SHADER;
    bool precision = v[1] >= GL_LOW_FLOAT && v[1] <= GL_HIGH_INT;
    if (!shader || !precision) {
        synth(gl, GL_INVALID_ENUM);
        return JS_NULL;
    }
    GLint range[2] = {0, 0}, bits = 0;
    glGetShaderPrecisionFormat((GLenum)v[0], (GLenum)v[1], range, &bits);
    struct wobj *o;
    JSValue r = wrapper_new(ctx, gl, O_PRECISION, &o);
    if (JS_IsException(r)) return r;
    o->range_min = range[0];
    o->range_max = range[1];
    o->precision = bits;
    return r;
}

/* ---- uniforms ---- */

struct uniform_shape { GLenum type; uint8_t count, kind; }; /* kind: 0 float, 1 int, 2 uint, 3 bool */
static const struct uniform_shape uniform_shapes[] = {
    {GL_FLOAT, 1, 0}, {GL_FLOAT_VEC2, 2, 0}, {GL_FLOAT_VEC3, 3, 0}, {GL_FLOAT_VEC4, 4, 0},
    {GL_INT, 1, 1}, {GL_INT_VEC2, 2, 1}, {GL_INT_VEC3, 3, 1}, {GL_INT_VEC4, 4, 1},
    {GL_UNSIGNED_INT, 1, 2}, {GL_UNSIGNED_INT_VEC2, 2, 2}, {GL_UNSIGNED_INT_VEC3, 3, 2}, {GL_UNSIGNED_INT_VEC4, 4, 2},
    {GL_BOOL, 1, 3}, {GL_BOOL_VEC2, 2, 3}, {GL_BOOL_VEC3, 3, 3}, {GL_BOOL_VEC4, 4, 3},
    {GL_FLOAT_MAT2, 4, 0}, {GL_FLOAT_MAT3, 9, 0}, {GL_FLOAT_MAT4, 16, 0}, {GL_FLOAT_MAT2x3, 6, 0},
    {GL_FLOAT_MAT2x4, 8, 0}, {GL_FLOAT_MAT3x2, 6, 0}, {GL_FLOAT_MAT3x4, 12, 0}, {GL_FLOAT_MAT4x2, 8, 0},
    {GL_FLOAT_MAT4x3, 12, 0},
};
static const struct uniform_shape *uniform_shape(GLenum type) {
    static const struct uniform_shape sampler = {0, 1, 1};
    for (size_t i = 0; i < sizeof uniform_shapes / sizeof *uniform_shapes; i++)
        if (uniform_shapes[i].type == type) return &uniform_shapes[i];
    return &sampler; /* every sampler type reads as one int */
}
/* "a[0]" and "a" name the same uniform; "a[3]" its fourth element. */
static size_t uniform_base(const char *name, size_t length) {
    if (length && name[length - 1] == ']') {
        const char *open = memchr(name, '[', length);
        const char *last = open;
        while (open) {
            last = open;
            open = memchr(open + 1, '[', length - (size_t)(open + 1 - name));
        }
        if (last) return (size_t)(last - name);
    }
    return length;
}
FN(m_get_uniform_location) {
    struct wobj *p; size_t length;
    if (object_arg(ctx, argv[0], O_PROGRAM, false, &p)) return JS_EXCEPTION;
    const char *name = JS_ToCStringLen(ctx, &length, argv[1]);
    if (!name) return JS_EXCEPTION;
    (void)argc;
    JSValue r = JS_NULL;
    bool reserved;
    if (!use(gl) || !program_usable(gl, p) || !name_ok(gl, name, length, &reserved) || reserved) goto done;
    GLint linked = 0;
    glGetProgramiv(p->name, GL_LINK_STATUS, &linked);
    if (!linked) {
        synth(gl, GL_INVALID_OPERATION);
        goto done;
    }
    GLint location = glGetUniformLocation(p->name, name);
    if (location < 0) goto done;
    GLint count = 0, max = 0;
    glGetProgramiv(p->name, GL_ACTIVE_UNIFORMS, &count);
    glGetProgramiv(p->name, GL_ACTIVE_UNIFORM_MAX_LENGTH, &max);
    char *active = malloc((size_t)(max > 0 ? max : 1));
    if (!active) {
        r = JS_ThrowOutOfMemory(ctx);
        goto done;
    }
    GLenum type = 0;
    size_t base = uniform_base(name, length);
    for (GLint i = 0; i < count && !type; i++) {
        GLsizei n = 0; GLint size = 0; GLenum t = 0;
        glGetActiveUniform(p->name, (GLuint)i, max, &n, &size, &t, active);
        size_t ab = uniform_base(active, (size_t)n);
        if (ab == base && !strncmp(active, name, base)) type = t;
    }
    free(active);
    struct wobj *o;
    r = wrapper_new(ctx, gl, O_LOCATION, &o);
    if (JS_IsException(r)) goto done;
    o->program = p->id;
    o->link_serial = p->link_serial;
    o->location = location;
    o->type = type;
done:
    JS_FreeCString(ctx, name);
    return r;
}
/* A uniform location applies to the program it came from, current and not
 * relinked since. */
static bool location_ok(struct webgl *gl, struct wobj *loc) {
    if (!owned(gl, loc)) return false;
    GLint current = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &current);
    struct wobj *p = table_object(gl, O_PROGRAM, (GLuint)current);
    if (!p || p->id != loc->program || p->link_serial != loc->link_serial) {
        synth(gl, GL_INVALID_OPERATION);
        return false;
    }
    return true;
}
/* uniform{1,2,3,4}{f,i,ui}: magic = components | kind << 4 */
static JSValue uniform_scalar(JSContext *ctx, struct webgl *gl, int argc, JSValueConst *argv, int n, int kind) {
    struct wobj *loc;
    if (object_arg(ctx, argv[0], O_LOCATION, true, &loc)) return JS_EXCEPTION;
    GLfloat f[4]; GLint i[4]; GLuint u[4];
    for (int k = 0; k < n; k++) {
        JSValueConst a = k + 1 < argc ? argv[k + 1] : JS_UNDEFINED;
        if ((kind == 0 && tof(ctx, a, &f[k])) || (kind == 1 && toi(ctx, a, &i[k])) || (kind == 2 && tou(ctx, a, &u[k])))
            return JS_EXCEPTION;
    }
    CUR(JS_UNDEFINED);
    if (!loc || !location_ok(gl, loc)) return JS_UNDEFINED;
    GLint l = loc->location;
    if (kind == 0) {
        if (n == 1) glUniform1f(l, f[0]); else if (n == 2) glUniform2f(l, f[0], f[1]);
        else if (n == 3) glUniform3f(l, f[0], f[1], f[2]); else glUniform4f(l, f[0], f[1], f[2], f[3]);
    } else if (kind == 1) {
        if (n == 1) glUniform1i(l, i[0]); else if (n == 2) glUniform2i(l, i[0], i[1]);
        else if (n == 3) glUniform3i(l, i[0], i[1], i[2]); else glUniform4i(l, i[0], i[1], i[2], i[3]);
    } else {
        if (n == 1) glUniform1ui(l, u[0]); else if (n == 2) glUniform2ui(l, u[0], u[1]);
        else if (n == 3) glUniform3ui(l, u[0], u[1], u[2]); else glUniform4ui(l, u[0], u[1], u[2], u[3]);
    }
    return JS_UNDEFINED;
}
/* uniform*v and uniformMatrix*fv: rows/columns for matrices, n for vectors. */
static JSValue uniform_vector(JSContext *ctx, struct webgl *gl, int argc, JSValueConst *argv, int n, int kind,
                              int columns, int rows) {
    struct wobj *loc;
    if (object_arg(ctx, argv[0], O_LOCATION, true, &loc)) return JS_EXCEPTION;
    bool matrix = columns > 0, transpose = false;
    int data_arg = matrix ? 2 : 1;
    if (matrix) transpose = JS_ToBool(ctx, argv[1]);
    uint32_t src_offset = 0, src_length = 0;
    if (gl->version == 2 && ((argc > data_arg + 1 && tou(ctx, argv[data_arg + 1], &src_offset)) ||
                             (argc > data_arg + 2 && tou(ctx, argv[data_arg + 2], &src_length))))
        return JS_EXCEPTION;
    void *data, *owned_data;
    size_t count;
    if (list_get(ctx, argv[data_arg], kind == 0 ? V_F32 : kind == 1 ? V_I32 : V_U32, &data, &count, &owned_data))
        return JS_EXCEPTION;
    JSValue r = JS_UNDEFINED;
    if (!use(gl)) goto done;
    if (src_offset > count || (src_length && src_length > count - src_offset)) {
        synth(gl, GL_INVALID_VALUE);
        goto done;
    }
    count = src_length ? src_length : count - src_offset;
    data = (uint8_t *)data + (size_t)src_offset * 4;
    int width = matrix ? columns * rows : n;
    if (!count || count % (size_t)width || count / (size_t)width > INT32_MAX) {
        synth(gl, GL_INVALID_VALUE);
        goto done;
    }
    if (matrix && transpose && gl->version == 1) {
        synth(gl, GL_INVALID_VALUE);
        goto done;
    }
    if (!loc || !location_ok(gl, loc)) goto done;
    GLint l = loc->location;
    GLsizei c = (GLsizei)(count / (size_t)width);
    if (matrix) {
        const GLfloat *m = data;
        if (columns == 2 && rows == 2) glUniformMatrix2fv(l, c, transpose, m);
        else if (columns == 3 && rows == 3) glUniformMatrix3fv(l, c, transpose, m);
        else if (columns == 4 && rows == 4) glUniformMatrix4fv(l, c, transpose, m);
        else if (columns == 2 && rows == 3) glUniformMatrix2x3fv(l, c, transpose, m);
        else if (columns == 3 && rows == 2) glUniformMatrix3x2fv(l, c, transpose, m);
        else if (columns == 2 && rows == 4) glUniformMatrix2x4fv(l, c, transpose, m);
        else if (columns == 4 && rows == 2) glUniformMatrix4x2fv(l, c, transpose, m);
        else if (columns == 3 && rows == 4) glUniformMatrix3x4fv(l, c, transpose, m);
        else glUniformMatrix4x3fv(l, c, transpose, m);
    } else if (kind == 0) {
        const GLfloat *v = data;
        if (n == 1) glUniform1fv(l, c, v); else if (n == 2) glUniform2fv(l, c, v);
        else if (n == 3) glUniform3fv(l, c, v); else glUniform4fv(l, c, v);
    } else if (kind == 1) {
        const GLint *v = data;
        if (n == 1) glUniform1iv(l, c, v); else if (n == 2) glUniform2iv(l, c, v);
        else if (n == 3) glUniform3iv(l, c, v); else glUniform4iv(l, c, v);
    } else {
        const GLuint *v = data;
        if (n == 1) glUniform1uiv(l, c, v); else if (n == 2) glUniform2uiv(l, c, v);
        else if (n == 3) glUniform3uiv(l, c, v); else glUniform4uiv(l, c, v);
    }
done:
    js_free(ctx, owned_data);
    return r;
}
#define UNIFORM_S(fname, n, kind) FN(fname) { return uniform_scalar(ctx, gl, argc, argv, n, kind); }
#define UNIFORM_V(fname, n, kind) FN(fname) { return uniform_vector(ctx, gl, argc, argv, n, kind, 0, 0); }
#define UNIFORM_M(fname, c, r) FN(fname) { return uniform_vector(ctx, gl, argc, argv, 0, 0, c, r); }
UNIFORM_S(m_uniform1f, 1, 0) UNIFORM_S(m_uniform2f, 2, 0) UNIFORM_S(m_uniform3f, 3, 0) UNIFORM_S(m_uniform4f, 4, 0)
UNIFORM_S(m_uniform1i, 1, 1) UNIFORM_S(m_uniform2i, 2, 1) UNIFORM_S(m_uniform3i, 3, 1) UNIFORM_S(m_uniform4i, 4, 1)
UNIFORM_S(m_uniform1ui, 1, 2) UNIFORM_S(m_uniform2ui, 2, 2) UNIFORM_S(m_uniform3ui, 3, 2) UNIFORM_S(m_uniform4ui, 4, 2)
UNIFORM_V(m_uniform1fv, 1, 0) UNIFORM_V(m_uniform2fv, 2, 0) UNIFORM_V(m_uniform3fv, 3, 0) UNIFORM_V(m_uniform4fv, 4, 0)
UNIFORM_V(m_uniform1iv, 1, 1) UNIFORM_V(m_uniform2iv, 2, 1) UNIFORM_V(m_uniform3iv, 3, 1) UNIFORM_V(m_uniform4iv, 4, 1)
UNIFORM_V(m_uniform1uiv, 1, 2) UNIFORM_V(m_uniform2uiv, 2, 2) UNIFORM_V(m_uniform3uiv, 3, 2) UNIFORM_V(m_uniform4uiv, 4, 2)
UNIFORM_M(m_uniform_matrix2fv, 2, 2) UNIFORM_M(m_uniform_matrix3fv, 3, 3) UNIFORM_M(m_uniform_matrix4fv, 4, 4)
UNIFORM_M(m_uniform_matrix2x3fv, 2, 3) UNIFORM_M(m_uniform_matrix3x2fv, 3, 2) UNIFORM_M(m_uniform_matrix2x4fv, 2, 4)
UNIFORM_M(m_uniform_matrix4x2fv, 4, 2) UNIFORM_M(m_uniform_matrix3x4fv, 3, 4) UNIFORM_M(m_uniform_matrix4x3fv, 4, 3)

static JSValue typed_numbers(JSContext *ctx, JSTypedArrayEnum type, const void *data, unsigned count) {
    JSValue n = JS_NewInt32(ctx, (int32_t)count);
    JSValue a = JS_NewTypedArray(ctx, 1, &n, type);
    if (JS_IsException(a)) return a;
    size_t offset, length, element;
    JSValue buffer = JS_GetTypedArrayBuffer(ctx, a, &offset, &length, &element);
    size_t size;
    uint8_t *bytes = JS_IsException(buffer) ? NULL : JS_GetArrayBuffer(ctx, &size, buffer);
    if (bytes && length >= count * 4u) memcpy(bytes + offset, data, count * 4u);
    JS_FreeValue(ctx, buffer);
    return a;
}
FN(m_get_uniform) {
    struct wobj *p, *loc;
    if (object_arg(ctx, argv[0], O_PROGRAM, false, &p) || object_arg(ctx, argv[1], O_LOCATION, false, &loc))
        return JS_EXCEPTION;
    (void)argc;
    CUR(JS_NULL);
    if (!program_usable(gl, p) || !owned(gl, loc)) return JS_NULL;
    if (loc->program != p->id || loc->link_serial != p->link_serial) {
        synth(gl, GL_INVALID_OPERATION);
        return JS_NULL;
    }
    const struct uniform_shape *shape = uniform_shape(loc->type);
    union { GLfloat f[16]; GLint i[16]; GLuint u[16]; } v;
    memset(&v, 0, sizeof v);
    if (shape->kind == 0) glGetUniformfv(p->name, loc->location, v.f);
    else if (shape->kind == 2) glGetUniformuiv(p->name, loc->location, v.u);
    else glGetUniformiv(p->name, loc->location, v.i);
    if (shape->kind == 3) {
        if (shape->count == 1) return JS_NewBool(ctx, v.i[0]);
        JSValue a = JS_NewArray(ctx);
        for (unsigned k = 0; k < shape->count && !JS_IsException(a); k++)
            JS_SetPropertyUint32(ctx, a, k, JS_NewBool(ctx, v.i[k]));
        return a;
    }
    if (shape->count == 1)
        return shape->kind == 0 ? JS_NewFloat64(ctx, v.f[0])
               : shape->kind == 2 ? JS_NewFloat64(ctx, v.u[0]) : JS_NewInt32(ctx, v.i[0]);
    return typed_numbers(ctx, shape->kind == 0 ? JS_TYPED_ARRAY_FLOAT32 : shape->kind == 2 ? JS_TYPED_ARRAY_UINT32 : JS_TYPED_ARRAY_INT32,
                         &v, shape->count);
}

/* ---- vertex attributes ---- */

static bool attrib_index(struct webgl *gl, GLuint index) {
    if (index < (GLuint)gl->max_attribs) return true;
    synth(gl, GL_INVALID_VALUE);
    return false;
}
FN(m_enable_vertex_attrib_array) {
    GLuint i; if (tou(ctx, argv[0], &i)) return JS_EXCEPTION; UNUSED_ARGS; CUR(JS_UNDEFINED);
    if (attrib_index(gl, i)) glEnableVertexAttribArray(i);
    return JS_UNDEFINED;
}
FN(m_disable_vertex_attrib_array) {
    GLuint i; if (tou(ctx, argv[0], &i)) return JS_EXCEPTION; UNUSED_ARGS; CUR(JS_UNDEFINED);
    if (attrib_index(gl, i)) glDisableVertexAttribArray(i);
    return JS_UNDEFINED;
}
static unsigned attrib_type_size(struct webgl *gl, GLenum type, bool integer) {
    switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
    case GL_FLOAT: return integer ? 0 : 4;
    case GL_INT: case GL_UNSIGNED_INT: return gl->version == 2 ? 4 : 0;
    case GL_HALF_FLOAT: return gl->version == 2 && !integer ? 2 : 0;
    case GL_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_2_10_10_10_REV: return gl->version == 2 && !integer ? 4 : 0;
    default: return 0;
    }
}
static JSValue attrib_pointer(JSContext *ctx, struct webgl *gl, int argc, JSValueConst *argv, bool integer) {
    GLuint index; GLint size, type, stride; bool normalized = false; int64_t offset;
    int a = 0;
    if (tou(ctx, argv[a++], &index) || toi(ctx, argv[a++], &size) || toi(ctx, argv[a++], &type)) return JS_EXCEPTION;
    if (!integer) normalized = JS_ToBool(ctx, argv[a++]);
    if (toi(ctx, argv[a++], &stride) || toi64(ctx, argv[a], &offset)) return JS_EXCEPTION;
    (void)argc;
    CUR(JS_UNDEFINED);
    if (!attrib_index(gl, index)) return JS_UNDEFINED;
    unsigned bytes = attrib_type_size(gl, (GLenum)type, integer);
    if (!bytes) {
        synth(gl, GL_INVALID_ENUM);
        return JS_UNDEFINED;
    }
    if (size < 1 || size > 4 || stride < 0 || stride > 255 || offset < 0) {
        synth(gl, GL_INVALID_VALUE);
        return JS_UNDEFINED;
    }
    if ((type == GL_INT_2_10_10_10_REV || type == GL_UNSIGNED_INT_2_10_10_10_REV) && size != 4) {
        synth(gl, GL_INVALID_OPERATION);
        return JS_UNDEFINED;
    }
    if ((uint64_t)offset % bytes || (unsigned)stride % bytes) {
        synth(gl, GL_INVALID_OPERATION);
        return JS_UNDEFINED;
    }
    GLint buffer = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
    if (!buffer && offset) {
        synth(gl, GL_INVALID_OPERATION);
        return JS_UNDEFINED;
    }
    if (integer) glVertexAttribIPointer(index, size, (GLenum)type, stride, (const void *)(uintptr_t)offset);
    else glVertexAttribPointer(index, size, (GLenum)type, normalized, stride, (const void *)(uintptr_t)offset);
    return JS_UNDEFINED;
}
FN(m_vertex_attrib_pointer) { return attrib_pointer(ctx, gl, argc, argv, false); }
FN(m_vertex_attrib_i_pointer) { return attrib_pointer(ctx, gl, argc, argv, true); }
static JSValue vertex_attrib_divisor(JSContext *ctx, struct webgl *gl, JSValueConst *argv) {
    GLuint v[2];
    if (tou(ctx, argv[0], &v[0]) || tou(ctx, argv[1], &v[1])) return JS_EXCEPTION;
    CUR(JS_UNDEFINED);
    if (attrib_index(gl, v[0])) glVertexAttribDivisor(v[0], v[1]);
    return JS_UNDEFINED;
}
FN(m_vertex_attrib_divisor) { (void)argc; return vertex_attrib_divisor(ctx, gl, argv); }
static JSValue vertex_attrib_f(JSContext *ctx, struct webgl *gl, int argc, JSValueConst *argv, int n) {
    GLuint index; GLfloat f[4] = {0, 0, 0, 1};
    if (tou(ctx, argv[0], &index) || floats(ctx, argc, argv, 1, n, f)) return JS_EXCEPTION;
    CUR(JS_UNDEFINED);
    if (!attrib_index(gl, index)) return JS_UNDEFINED;
    glVertexAttrib4f(index, f[0], f[1], f[2], f[3]);
    gl->attrib_kind[index] = 0;
    return JS_UNDEFINED;
}
static JSValue vertex_attrib_fv(JSContext *ctx, struct webgl *gl, JSValueConst *argv, int n) {
    GLuint index; void *data, *owned_data; size_t count;
    if (tou(ctx, argv[0], &index) || list_get(ctx, argv[1], V_F32, &data, &count, &owned_data)) return JS_EXCEPTION;
    if (use(gl)) {
        if (count < (size_t)n) synth(gl, GL_INVALID_VALUE);
        else if (attrib_index(gl, index)) {
            GLfloat f[4] = {0, 0, 0, 1};
            memcpy(f, data, (size_t)n * 4);
            glVertexAttrib4f(index, f[0], f[1], f[2], f[3]);
            gl->attrib_kind[index] = 0;
        }
    }
    js_free(ctx, owned_data);
    return JS_UNDEFINED;
}
FN(m_vertex_attrib1f) { return vertex_attrib_f(ctx, gl, argc, argv, 1); }
FN(m_vertex_attrib2f) { return vertex_attrib_f(ctx, gl, argc, argv, 2); }
FN(m_vertex_attrib3f) { return vertex_attrib_f(ctx, gl, argc, argv, 3); }
FN(m_vertex_attrib4f) { return vertex_attrib_f(ctx, gl, argc, argv, 4); }
FN(m_vertex_attrib1fv) { (void)argc; return vertex_attrib_fv(ctx, gl, argv, 1); }
FN(m_vertex_attrib2fv) { (void)argc; return vertex_attrib_fv(ctx, gl, argv, 2); }
FN(m_vertex_attrib3fv) { (void)argc; return vertex_attrib_fv(ctx, gl, argv, 3); }
FN(m_vertex_attrib4fv) { (void)argc; return vertex_attrib_fv(ctx, gl, argv, 4); }
static JSValue vertex_attrib_i4(JSContext *ctx, struct webgl *gl, int argc, JSValueConst *argv, bool unsign, bool list) {
    GLuint index; GLuint v[4] = {0}; void *data = NULL, *owned_data = NULL; size_t count = 4;
    if (tou(ctx, argv[0], &index)) return JS_EXCEPTION;
    if (list) {
        if (list_get(ctx, argv[1], unsign ? V_U32 : V_I32, &data, &count, &owned_data)) return JS_EXCEPTION;
        if (count >= 4) memcpy(v, data, 16);
    } else {
        for (int i = 0; i < 4; i++)
            if (tou(ctx, 1 + i < argc ? argv[1 + i] : JS_UNDEFINED, &v[i])) return JS_EXCEPTION;
    }
    if (use(gl)) {
        if (count < 4) synth(gl, GL_INVALID_VALUE);
        else if (attrib_index(gl, index)) {
            if (unsign) glVertexAttribI4ui(index, v[0], v[1], v[2], v[3]);
            else glVertexAttribI4i(index, (GLint)v[0], (GLint)v[1], (GLint)v[2], (GLint)v[3]);
            gl->attrib_kind[index] = unsign ? 2 : 1;
        }
    }
    js_free(ctx, owned_data);
    return JS_UNDEFINED;
}
FN(m_vertex_attrib_i4i) { return vertex_attrib_i4(ctx, gl, argc, argv, false, false); }
FN(m_vertex_attrib_i4ui) { return vertex_attrib_i4(ctx, gl, argc, argv, true, false); }
FN(m_vertex_attrib_i4iv) { return vertex_attrib_i4(ctx, gl, argc, argv, false, true); }
FN(m_vertex_attrib_i4uiv) { return vertex_attrib_i4(ctx, gl, argc, argv, true, true); }
FN(m_get_vertex_attrib) {
    GLuint index; GLint pname;
    if (tou(ctx, argv[0], &index) || toi(ctx, argv[1], &pname)) return JS_EXCEPTION;
    (void)argc;
    CUR(JS_NULL);
    if (!attrib_index(gl, index)) return JS_NULL;
    GLint value = 0;
    switch (pname) {
    case GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING:
        glGetVertexAttribiv(index, (GLenum)pname, &value);
        return table_get(ctx, gl, O_BUFFER, (GLuint)value);
    case GL_VERTEX_ATTRIB_ARRAY_ENABLED: case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED:
        glGetVertexAttribiv(index, (GLenum)pname, &value);
        return JS_NewBool(ctx, value);
    case GL_VERTEX_ATTRIB_ARRAY_SIZE: case GL_VERTEX_ATTRIB_ARRAY_STRIDE: case GL_VERTEX_ATTRIB_ARRAY_TYPE:
        glGetVertexAttribiv(index, (GLenum)pname, &value);
        return JS_NewInt32(ctx, value);
    case GL_VERTEX_ATTRIB_ARRAY_DIVISOR:
        if (gl->version == 1 && !ext(gl, X_ANGLE_instanced_arrays)) break;
        glGetVertexAttribiv(index, (GLenum)pname, &value);
        return JS_NewInt32(ctx, value);
    case GL_VERTEX_ATTRIB_ARRAY_INTEGER:
        if (gl->version == 1) break;
        glGetVertexAttribiv(index, (GLenum)pname, &value);
        return JS_NewBool(ctx, value);
    case GL_CURRENT_VERTEX_ATTRIB: {
        union { GLfloat f[4]; GLint i[4]; GLuint u[4]; } v;
        uint8_t kind = gl->attrib_kind[index];
        if (kind == 1) glGetVertexAttribIiv(index, GL_CURRENT_VERTEX_ATTRIB, v.i);
        else if (kind == 2) glGetVertexAttribIuiv(index, GL_CURRENT_VERTEX_ATTRIB, v.u);
        else glGetVertexAttribfv(index, GL_CURRENT_VERTEX_ATTRIB, v.f);
        return typed_numbers(ctx, kind == 1 ? JS_TYPED_ARRAY_INT32 : kind == 2 ? JS_TYPED_ARRAY_UINT32 : JS_TYPED_ARRAY_FLOAT32, &v, 4);
    }
    }
    synth(gl, GL_INVALID_ENUM);
    return JS_NULL;
}
FN(m_get_vertex_attrib_offset) {
    GLuint index; GLint pname;
    if (tou(ctx, argv[0], &index) || toi(ctx, argv[1], &pname)) return JS_EXCEPTION;
    (void)argc;
    CUR(JS_NewInt32(ctx, 0));
    if (!attrib_index(gl, index)) return JS_NewInt32(ctx, 0);
    if (pname != GL_VERTEX_ATTRIB_ARRAY_POINTER) {
        synth(gl, GL_INVALID_ENUM);
        return JS_NewInt32(ctx, 0);
    }
    void *pointer = NULL;
    glGetVertexAttribPointerv(index, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pointer);
    return JS_NewFloat64(ctx, (double)(uintptr_t)pointer);
}

/* ---- textures ---- */

static bool tex_parameter_ok(struct webgl *gl, GLenum pname) {
    switch (pname) {
    case GL_TEXTURE_MAG_FILTER: case GL_TEXTURE_MIN_FILTER: case GL_TEXTURE_WRAP_S: case GL_TEXTURE_WRAP_T:
        return true;
    case GL_TEXTURE_MAX_ANISOTROPY_EXT: return ext(gl, X_EXT_texture_filter_anisotropic);
    case GL_TEXTURE_BASE_LEVEL: case GL_TEXTURE_COMPARE_FUNC: case GL_TEXTURE_COMPARE_MODE: case GL_TEXTURE_MAX_LEVEL:
    case GL_TEXTURE_MAX_LOD: case GL_TEXTURE_MIN_LOD: case GL_TEXTURE_WRAP_R:
        return gl->version == 2;
    case GL_TEXTURE_IMMUTABLE_FORMAT: case GL_TEXTURE_IMMUTABLE_LEVELS: return gl->version == 2; /* query only */
    default: return false;
    }
}
static JSValue tex_parameter(JSContext *ctx, struct webgl *gl, JSValueConst *argv, bool is_float) {
    GLint target, pname; GLfloat f = 0; GLint i = 0;
    if (toi(ctx, argv[0], &target) || toi(ctx, argv[1], &pname)) return JS_EXCEPTION;
    if (is_float ? tof(ctx, argv[2], &f) : toi(ctx, argv[2], &i)) return JS_EXCEPTION;
    CUR(JS_UNDEFINED);
    if (!texture_target(gl, (GLenum)target) || !tex_parameter_ok(gl, (GLenum)pname) ||
        pname == GL_TEXTURE_IMMUTABLE_FORMAT || pname == GL_TEXTURE_IMMUTABLE_LEVELS) {
        synth(gl, GL_INVALID_ENUM);
        return JS_UNDEFINED;
    }
    if (is_float) glTexParameterf((GLenum)target, (GLenum)pname, f);
    else glTexParameteri((GLenum)target, (GLenum)pname, i);
    return JS_UNDEFINED;
}
FN(m_tex_parameterf) { (void)argc; return tex_parameter(ctx, gl, argv, true); }
FN(m_tex_parameteri) { (void)argc; return tex_parameter(ctx, gl, argv, false); }
FN(m_get_tex_parameter) {
    GLint v[2]; if (ints(ctx, argc, argv, 0, 2, v)) return JS_EXCEPTION; CUR(JS_NULL);
    if (!texture_target(gl, (GLenum)v[0]) || !tex_parameter_ok(gl, (GLenum)v[1])) {
        synth(gl, GL_INVALID_ENUM);
        return JS_NULL;
    }
    if (v[1] == GL_TEXTURE_MAX_LOD || v[1] == GL_TEXTURE_MIN_LOD || v[1] == GL_TEXTURE_MAX_ANISOTROPY_EXT) {
        GLfloat f = 0;
        glGetTexParameterfv((GLenum)v[0], (GLenum)v[1], &f);
        return JS_NewFloat64(ctx, f);
    }
    GLint value = 0;
    glGetTexParameteriv((GLenum)v[0], (GLenum)v[1], &value);
    if (v[1] == GL_TEXTURE_IMMUTABLE_FORMAT) return JS_NewBool(ctx, value);
    return JS_NewInt32(ctx, value);
}
FN(m_generate_mipmap) {
    GLint t; if (toi(ctx, argv[0], &t)) return JS_EXCEPTION; UNUSED_ARGS; CUR(JS_UNDEFINED);
    if (!texture_target(gl, (GLenum)t)) synth(gl, GL_INVALID_ENUM);
    else glGenerateMipmap((GLenum)t);
    return JS_UNDEFINED;
}

/* Uploads from ArrayBufferView data with the WebGL unpack flags. data may
 * be NULL (texImage: zero-filled). */
struct upload {
    bool sub, three;
    GLenum target, format, type;
    GLint level, internalformat, x, y, z, border;
    GLsizei w, h, d;
};
static void upload_call(const struct upload *u, const void *pixels) {
    if (u->three) {
        if (u->sub) glTexSubImage3D(u->target, u->level, u->x, u->y, u->z, u->w, u->h, u->d, u->format, u->type, pixels);
        else glTexImage3D(u->target, u->level, u->internalformat, u->w, u->h, u->d, u->border, u->format, u->type, pixels);
    } else {
        if (u->sub) glTexSubImage2D(u->target, u->level, u->x, u->y, u->w, u->h, u->format, u->type, pixels);
        else glTexImage2D(u->target, u->level, u->internalformat, u->w, u->h, u->border, u->format, u->type, pixels);
    }
}
static bool unpack_buffer_bound(void) {
    GLint b = 0;
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &b);
    return b != 0;
}
static JSValue upload_view(JSContext *ctx, struct webgl *gl, const struct upload *u, JSValueConst value, uint32_t src_offset) {
    struct view view = {0};
    bool null = JS_IsNull(value) || JS_IsUndefined(value);
    if (!null && !view_get(ctx, value, &view)) return JS_ThrowTypeError(ctx, "Expected an ArrayBufferView");
    if (!use(gl)) return JS_UNDEFINED;
    if (!webgl1_texture_ok(gl, (GLenum)u->internalformat, u->format, u->type) && !u->sub) return JS_UNDEFINED;
    if (gl->version == 2 && unpack_buffer_bound()) {
        synth(gl, GL_INVALID_OPERATION);
        return JS_UNDEFINED;
    }
    unsigned bpp = pixel_bytes(u->format, u->type);
    if (!bpp) {
        synth(gl, GL_INVALID_ENUM);
        return JS_UNDEFINED;
    }
    if (u->w < 0 || u->h < 0 || u->d < 0) {
        synth(gl, GL_INVALID_VALUE);
        return JS_UNDEFINED;
    }
    if (null) {
        if (u->sub) {
            synth(gl, GL_INVALID_VALUE);
            return JS_UNDEFINED;
        }
        /* A re-specified image may reuse storage: upload zeros explicitly. */
        uint64_t bytes = (uint64_t)u->w * u->h * u->d * bpp;
        if (bytes > SIZE_MAX) {
            synth(gl, GL_OUT_OF_MEMORY);
            return JS_UNDEFINED;
        }
        void *zeros = bytes ? calloc(1, (size_t)bytes) : NULL;
        if (bytes && !zeros) {
            synth(gl, GL_OUT_OF_MEMORY);
            return JS_UNDEFINED;
        }
        unpack_tight(gl, 0, 0, false);
        upload_call(u, zeros);
        unpack_restore(gl);
        free(zeros);
        return JS_UNDEFINED;
    }
    if (!view_matches(view.type, u->type)) {
        synth(gl, GL_INVALID_OPERATION);
        return JS_UNDEFINED;
    }
    if (src_offset && !view_range(&view, src_offset, 0)) {
        synth(gl, GL_INVALID_VALUE);
        return JS_UNDEFINED;
    }
    uint64_t need;
    if (!image_bytes(gl, false, u->w, u->h, u->d, bpp, &need) || need > view.bytes) {
        synth(gl, GL_INVALID_OPERATION);
        return JS_UNDEFINED;
    }
    bool premultiply = gl->premultiply_alpha && u->type == GL_UNSIGNED_BYTE &&
                       (u->format == GL_RGBA || u->format == GL_LUMINANCE_ALPHA);
    if ((gl->flip_y || gl->premultiply_alpha) && u->three) {
        synth(gl, GL_INVALID_OPERATION);
        return JS_UNDEFINED;
    }
    if (!(gl->flip_y || premultiply) || !u->w || !u->h) {
        upload_call(u, view.data);
        return JS_UNDEFINED;
    }
    /* Flip/premultiply into a tightly packed copy. */
    const struct pixel_store *s = &gl->store;
    uint64_t row_bytes = (uint64_t)u->w * bpp;
    uint64_t row = (uint64_t)(s->unpack_row_length ? s->unpack_row_length : u->w) * bpp;
    uint64_t stride = (row + (uint64_t)s->unpack_alignment - 1) / (uint64_t)s->unpack_alignment * (uint64_t)s->unpack_alignment;
    const uint8_t *base = view.data + (uint64_t)s->unpack_skip_rows * stride + (uint64_t)s->unpack_skip_pixels * bpp;
    uint8_t *copy = malloc((size_t)(row_bytes * (uint64_t)u->h));
    if (!copy) {
        synth(gl, GL_OUT_OF_MEMORY);
        return JS_UNDEFINED;
    }
    for (GLsizei y = 0; y < u->h; y++) {
        const uint8_t *src = base + (uint64_t)(gl->flip_y ? u->h - 1 - y : y) * stride;
        uint8_t *dst = copy + (uint64_t)y * row_bytes;
        memcpy(dst, src, (size_t)row_bytes);
        if (premultiply) {
            unsigned n = u->format == GL_RGBA ? 4 : 2;
            for (GLsizei x = 0; x < u->w; x++) {
                uint8_t *p = dst + (size_t)x * n, a = p[n - 1];
                for (unsigned c = 0; c + 1 < n; c++) p[c] = (uint8_t)((p[c] * a + 127) / 255);
            }
        }
    }
    unpack_tight(gl, 0, 0, false);
    upload_call(u, copy);
    unpack_restore(gl);
    free(copy);
    return JS_UNDEFINED;
}
static JSValue upload_offset(JSContext *ctx, struct webgl *gl, const struct upload *u, JSValueConst value) {
    int64_t offset;
    if (toi64(ctx, value, &offset)) return JS_EXCEPTION;
    if (!use(gl)) return JS_UNDEFINED;
    if (!unpack_buffer_bound()) {
        synth(gl, GL_INVALID_OPERATION);
        return JS_UNDEFINED;
    }
    if (offset < 0) {
        synth(gl, GL_INVALID_VALUE);
        return JS_UNDEFINED;
    }
    upload_call(u, (const void *)(uintptr_t)offset);
    return JS_UNDEFINED;
}
/* TexImageSource: explicit sizes (WebGL 2) select a sub-rectangle with the
 * unpack skip parameters; otherwise the image size is used. */
static JSValue upload_image(JSContext *ctx, struct webgl *gl, struct upload *u, JSValueConst value, bool sized) {
    uint8_t *pixels = NULL;
    unsigned w = 0, h = 0, bpp = 0;
    if (gl->lost) return JS_UNDEFINED;
    if (!webgl1_texture_ok(gl, (GLenum)u->internalformat, u->format, u->type) && !u->sub) return JS_UNDEFINED;
    int r = image_source(ctx, gl, value, u->format, u->type, &pixels, &w, &h, &bpp);
    if (r <= 0) return JS_EXCEPTION;
    if (r == 2) return JS_UNDEFINED;
    if (!use(gl)) {
        free(pixels);
        return JS_UNDEFINED;
    }
    if (gl->version == 2 && unpack_buffer_bound()) {
        free(pixels);
        synth(gl, GL_INVALID_OPERATION);
        return JS_UNDEFINED;
    }
    if (!sized) {
        u->w = (GLsizei)w;
        u->h = (GLsizei)h;
        u->d = 1;
    }
    /* Rows of the decoded image are w pixels; validate the reach. */
    GLint row_length = gl->store.unpack_row_length ? gl->store.unpack_row_length : (GLint)w;
    struct pixel_store saved = gl->store;
    gl->store.unpack_alignment = 1;
    gl->store.unpack_row_length = row_length;
    uint64_t need = 0;
    bool fits = image_bytes(gl, false, u->w, u->h, u->d, bpp, &need) && need <= (uint64_t)w * h * bpp;
    gl->store = saved;
    if (u->w < 0 || u->h < 0 || u->d < 0) {
        synth(gl, GL_INVALID_VALUE);
    } else if (!fits) {
        synth(gl, GL_INVALID_OPERATION);
    } else {
        unpack_tight(gl, gl->version == 2 ? row_length : 0, gl->store.unpack_image_height, true);
        upload_call(u, pixels);
        unpack_restore(gl);
    }
    free(pixels);
    return JS_UNDEFINED;
}
/* The last argument selects the overload: number = PBO offset (WebGL 2),
 * null/ArrayBufferView = client data, any other object = TexImageSource. */
static JSValue upload_dispatch(JSContext *ctx, struct webgl *gl, struct upload *u, int argc, JSValueConst *argv, int at) {
    JSValueConst last = argv[at];
    uint32_t src_offset = 0;
    if (JS_IsNumber(last) || JS_IsBigInt(ctx, last)) {
        if (gl->version == 1) return JS_ThrowTypeError(ctx, "Expected an ArrayBufferView or null");
        return upload_offset(ctx, gl, u, last);
    }
    if (JS_IsNull(last) || JS_IsUndefined(last) || view_type(last) >= 0) {
        if (gl->version == 2 && argc > at + 1 && tou(ctx, argv[at + 1], &src_offset)) return JS_EXCEPTION;
        return upload_view(ctx, gl, u, last, src_offset);
    }
    if (gl->version == 1) return JS_ThrowTypeError(ctx, "Expected an ArrayBufferView or null");
    return upload_image(ctx, gl, u, last, true);
}
FN(m_tex_image_2d) {
    struct upload u = {.d = 1};
    GLint v[9];
    if (argc == 6) {
        if (ints(ctx, argc, argv, 0, 5, v)) return JS_EXCEPTION;
        u.target = (GLenum)v[0]; u.level = v[1]; u.internalformat = v[2]; u.format = (GLenum)v[3]; u.type = (GLenum)v[4];
        return upload_image(ctx, gl, &u, argv[5], false);
    }
    if (argc < 9) return JS_ThrowTypeError(ctx, "texImage2D: no overload takes %d arguments", argc);
    if (ints(ctx, argc, argv, 0, 8, v)) return JS_EXCEPTION;
    u.target = (GLenum)v[0]; u.level = v[1]; u.internalformat = v[2]; u.w = v[3]; u.h = v[4]; u.border = v[5];
    u.format = (GLenum)v[6]; u.type = (GLenum)v[7];
    return upload_dispatch(ctx, gl, &u, argc, argv, 8);
}
FN(m_tex_sub_image_2d) {
    struct upload u = {.sub = true, .d = 1};
    GLint v[9];
    if (argc == 7) {
        if (ints(ctx, argc, argv, 0, 6, v)) return JS_EXCEPTION;
        u.target = (GLenum)v[0]; u.level = v[1]; u.x = v[2]; u.y = v[3]; u.format = (GLenum)v[4]; u.type = (GLenum)v[5];
        return upload_image(ctx, gl, &u, argv[6], false);
    }
    if (argc < 9) return JS_ThrowTypeError(ctx, "texSubImage2D: no overload takes %d arguments", argc);
    if (ints(ctx, argc, argv, 0, 8, v)) return JS_EXCEPTION;
    u.target = (GLenum)v[0]; u.level = v[1]; u.x = v[2]; u.y = v[3]; u.w = v[4]; u.h = v[5];
    u.format = (GLenum)v[6]; u.type = (GLenum)v[7];
    return upload_dispatch(ctx, gl, &u, argc, argv, 8);
}
FN(m_tex_image_3d) {
    struct upload u = {.three = true};
    GLint v[9];
    if (ints(ctx, argc, argv, 0, 9, v)) return JS_EXCEPTION;
    u.target = (GLenum)v[0]; u.level = v[1]; u.internalformat = v[2]; u.w = v[3]; u.h = v[4]; u.d = v[5];
    u.border = v[6]; u.format = (GLenum)v[7]; u.type = (GLenum)v[8];
    return upload_dispatch(ctx, gl, &u, argc, argv, 9);
}
FN(m_tex_sub_image_3d) {
    struct upload u = {.three = true, .sub = true};
    GLint v[10];
    if (ints(ctx, argc, argv, 0, 10, v)) return JS_EXCEPTION;
    u.target = (GLenum)v[0]; u.level = v[1]; u.x = v[2]; u.y = v[3]; u.z = v[4]; u.w = v[5]; u.h = v[6]; u.d = v[7];
    u.format = (GLenum)v[8]; u.type = (GLenum)v[9];
    return upload_dispatch(ctx, gl, &u, argc, argv, 10);
}
/* No compressed texture format extension is offered, so every compressed
 * format is unknown to this context. */
FN(m_compressed_tex) { UNUSED_ARGS; CUR(JS_UNDEFINED); synth(gl, GL_INVALID_ENUM); return JS_UNDEFINED; }
FN(m_tex_storage_2d) {
    GLint v[5]; if (ints(ctx, argc, argv, 0, 5, v)) return JS_EXCEPTION; CUR(JS_UNDEFINED);
    glTexStorage2D((GLenum)v[0], v[1], (GLenum)v[2], v[3], v[4]);
    return JS_UNDEFINED;
}
FN(m_tex_storage_3d) {
    GLint v[6]; if (ints(ctx, argc, argv, 0, 6, v)) return JS_EXCEPTION; CUR(JS_UNDEFINED);
    glTexStorage3D((GLenum)v[0], v[1], (GLenum)v[2], v[3], v[4], v[5]);
    return JS_UNDEFINED;
}
FN(m_copy_tex_image_2d) {
    GLint v[8]; if (ints(ctx, argc, argv, 0, 8, v)) return JS_EXCEPTION; CUR(JS_UNDEFINED);
    if (gl->version == 1) {
        GLenum f = (GLenum)v[2];
        if (f != GL_ALPHA && f != GL_LUMINANCE && f != GL_LUMINANCE_ALPHA && f != GL_RGB && f != GL_RGBA) {
            synth(gl, GL_INVALID_ENUM);
            return JS_UNDEFINED;
        }
    }
    glCopyTexImage2D((GLenum)v[0], v[1], (GLenum)v[2], v[3], v[4], v[5], v[6], v[7]);
    return JS_UNDEFINED;
}
FN(m_copy_tex_sub_image_2d) {
    GLint v[8]; if (ints(ctx, argc, argv, 0, 8, v)) return JS_EXCEPTION; CUR(JS_UNDEFINED);
    glCopyTexSubImage2D((GLenum)v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    return JS_UNDEFINED;
}
FN(m_copy_tex_sub_image_3d) {
    GLint v[9]; if (ints(ctx, argc, argv, 0, 9, v)) return JS_EXCEPTION; CUR(JS_UNDEFINED);
    glCopyTexSubImage3D((GLenum)v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]);
    return JS_UNDEFINED;
}

/* ---- framebuffers and renderbuffers ---- */

static bool attachment_ok(struct webgl *gl, GLenum a) {
    if (a == GL_DEPTH_ATTACHMENT || a == GL_STENCIL_ATTACHMENT || a == WEBGL_DEPTH_STENCIL_ATTACHMENT ||
        a == GL_COLOR_ATTACHMENT0)
        return true;
    if (a > GL_COLOR_ATTACHMENT0 && a <= GL_COLOR_ATTACHMENT15) {
        if (gl->version == 1 && !ext(gl, X_WEBGL_draw_buffers)) return false;
        GLint max = 0;
        glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &max);
        return a - GL_COLOR_ATTACHMENT0 < (GLenum)max;
    }
    return false;
}
FN(m_framebuffer_texture_2d) {
    GLint v[3], level; struct wobj *t;
    if (ints(ctx, argc, argv, 0, 3, v) || object_arg(ctx, argv[3], O_TEXTURE, true, &t) || toi(ctx, argv[4], &level))
        return JS_EXCEPTION;
    CUR(JS_UNDEFINED);
    if (!framebuffer_target(gl, (GLenum)v[0]) || !attachment_ok(gl, (GLenum)v[1])) {
        synth(gl, GL_INVALID_ENUM);
        return JS_UNDEFINED;
    }
    if (!bindable(gl, t)) return JS_UNDEFINED;
    glFramebufferTexture2D((GLenum)v[0], (GLenum)v[1], (GLenum)v[2], name_of(t), level);
    return JS_UNDEFINED;
}
FN(m_framebuffer_texture_layer) {
    GLint v[2], level, layer; struct wobj *t;
    if (ints(ctx, argc, argv, 0, 2, v) || object_arg(ctx, argv[2], O_TEXTURE, true, &t) || toi(ctx, argv[3], &level) ||
        toi(ctx, argv[4], &layer))
        return JS_EXCEPTION;
    CUR(JS_UNDEFINED);
    if (!framebuffer_target(gl, (GLenum)v[0]) || !attachment_ok(gl, (GLenum)v[1])) {
        synth(gl, GL_INVALID_ENUM);
        return JS_UNDEFINED;
    }
    if (!bindable(gl, t)) return JS_UNDEFINED;
    glFramebufferTextureLayer((GLenum)v[0], (GLenum)v[1], name_of(t), level, layer);
    return JS_UNDEFINED;
}
FN(m_framebuffer_renderbuffer) {
    GLint v[3]; struct wobj *r;
    if (ints(ctx, argc, argv, 0, 3, v) || object_arg(ctx, argv[3], O_RENDERBUFFER, true, &r)) return JS_EXCEPTION;
    CUR(JS_UNDEFINED);
    if (!framebuffer_target(gl, (GLenum)v[0]) || !attachment_ok(gl, (GLenum)v[1]) || v[2] != GL_RENDERBUFFER) {
        synth(gl, GL_INVALID_ENUM);
        return JS_UNDEFINED;
    }
    if (!bindable(gl, r)) return JS_UNDEFINED;
    glFramebufferRenderbuffer((GLenum)v[0], (GLenum)v[1], GL_RENDERBUFFER, name_of(r));
    return JS_UNDEFINED;
}
FN(m_check_framebuffer_status) {
    GLint t; if (toi(ctx, argv[0], &t)) return JS_EXCEPTION; UNUSED_ARGS; CUR(JS_NewInt32(ctx, GL_FRAMEBUFFER_UNSUPPORTED));
    if (!framebuffer_target(gl, (GLenum)t)) {
        synth(gl, GL_INVALID_ENUM);
        return JS_NewInt32(ctx, 0);
    }
    return JS_NewInt32(ctx, (int32_t)glCheckFramebufferStatus((GLenum)t));
}
static GLenum renderbuffer_format(struct webgl *gl, GLenum f) {
    if (f == WEBGL_DEPTH_STENCIL) return GL_DEPTH24_STENCIL8;
    if (gl->version == 2) return f;
    switch (f) {
    case GL_RGBA4: case GL_RGB565: case GL_RGB5_A1: case GL_DEPTH_COMPONENT16: case GL_STENCIL_INDEX8: return f;
    case GL_RGBA32F: return ext(gl, X_WEBGL_color_buffer_float) ? f : 0;
    case GL_RGBA16F: case GL_RGB16F: return ext(gl, X_EXT_color_buffer_half_float) ? f : 0;
    case GL_SRGB8_ALPHA8: return ext(gl, X_EXT_sRGB) ? f : 0;
    default: return 0;
    }
}
FN(m_renderbuffer_storage) {
    GLint v[4]; if (ints(ctx, argc, argv, 0, 4, v)) return JS_EXCEPTION; CUR(JS_UNDEFINED);
    GLenum f = renderbuffer_format(gl, (GLenum)v[1]);
    if (v[0] != GL_RENDERBUFFER || !f) {
        synth(gl, GL_INVALID_ENUM);
        return JS_UNDEFINED;
    }
    glRenderbufferStorage(GL_RENDERBUFFER, f, v[2], v[3]);
    return JS_UNDEFINED;
}
FN(m_renderbuffer_storage_multisample) {
    GLint v[5]; if (ints(ctx, argc, argv, 0, 5, v)) return JS_EXCEPTION; CUR(JS_UNDEFINED);
    GLenum f = renderbuffer_format(gl, (GLenum)v[2]);
    if (v[0] != GL_RENDERBUFFER || !f) {
        synth(gl, GL_INVALID_ENUM);
        return JS_UNDEFINED;
    }
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, v[1], f, v[3], v[4]);
    return JS_UNDEFINED;
}
FN(m_get_renderbuffer_parameter) {
    GLint v[2]; if (ints(ctx, argc, argv, 0, 2, v)) return JS_EXCEPTION; CUR(JS_NULL);
    switch (v[1]) {
    case GL_RENDERBUFFER_WIDTH: case GL_RENDERBUFFER_HEIGHT: case GL_RENDERBUFFER_INTERNAL_FORMAT:
    case GL_RENDERBUFFER_RED_SIZE: case GL_RENDERBUFFER_GREEN_SIZE: case GL_RENDERBUFFER_BLUE_SIZE:
    case GL_RENDERBUFFER_ALPHA_SIZE: case GL_RENDERBUFFER_DEPTH_SIZE: case GL_RENDERBUFFER_STENCIL_SIZE:
        break;
    case GL_RENDERBUFFER_SAMPLES: if (gl->version == 2) break; /* fallthrough */
    default:
        synth(gl, GL_INVALID_ENUM);
        return JS_NULL;
    }
    if (v[0] != GL_RENDERBUFFER) {
        synth(gl, GL_INVALID_ENUM);
        return JS_NULL;
    }
    GLint value = 0;
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, (GLenum)v[1], &value);
    if (v[1] == GL_RENDERBUFFER_INTERNAL_FORMAT && value == GL_DEPTH24_STENCIL8 && gl->version == 1)
        value = WEBGL_DEPTH_STENCIL;
    return JS_NewInt32(ctx, value);
}
FN(m_get_framebuffer_attachment_parameter) {
    GLint v[3]; if (ints(ctx, argc, argv, 0, 3, v)) return JS_EXCEPTION; CUR(JS_NULL);
    if (!framebuffer_target(gl, (GLenum)v[0])) {
        synth(gl, GL_INVALID_ENUM);
        return JS_NULL;
    }
    GLint type = 0, value = 0;
    glGetFramebufferAttachmentParameteriv((GLenum)v[0], (GLenum)v[1], GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
    if (v[2] == GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME) {
        glGetFramebufferAttachmentParameteriv((GLenum)v[0], (GLenum)v[1], (GLenum)v[2], &value);
        if (type == GL_TEXTURE) return table_get(ctx, gl, O_TEXTURE, (GLuint)value);
        if (type == GL_RENDERBUFFER) return table_get(ctx, gl, O_RENDERBUFFER, (GLuint)value);
        return JS_NULL;
    }
    switch (v[2]) {
    case GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE: case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL:
    case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE:
        break;
    case GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING_EXT:
        if (gl->version == 2 || ext(gl, X_EXT_sRGB)) break;
        synth(gl, GL_INVALID_ENUM);
        return JS_NULL;
    case GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE:
        if (gl->version == 2 || ext(gl, X_WEBGL_color_buffer_float) || ext(gl, X_EXT_color_buffer_half_float)) break;
        synth(gl, GL_INVALID_ENUM);
        return JS_NULL;
    case GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE: case GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE:
    case GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE: case GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE:
    case GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE: case GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE:
    case GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER:
        if (gl->version == 2) break;
        /* fallthrough */
    default:
        synth(gl, GL_INVALID_ENUM);
        return JS_NULL;
    }
    glGetFramebufferAttachmentParameteriv((GLenum)v[0], (GLenum)v[1], (GLenum)v[2], &value);
    return JS_NewInt32(ctx, value);
}
static JSValue enum_list(JSContext *ctx, JSValueConst v, GLenum **out, GLsizei *count, void **owned_data) {
    void *data;
    size_t n;
    if (list_get(ctx, v, V_U32, &data, &n, owned_data)) return JS_EXCEPTION;
    if (n > 64) n = 64;
    *out = data;
    *count = (GLsizei)n;
    return JS_UNDEFINED;
}
FN(m_invalidate_framebuffer) {
    GLint target; GLenum *list; GLsizei count; void *owned_data;
    GLint r[4] = {0};
    if (toi(ctx, argv[0], &target) || JS_IsException(enum_list(ctx, argv[1], &list, &count, &owned_data))) return JS_EXCEPTION;
    bool sub = argc > 2;
    if (sub && ints(ctx, argc, argv, 2, 4, r)) {
        js_free(ctx, owned_data);
        return JS_EXCEPTION;
    }
    if (use(gl)) {
        if (sub) glInvalidateSubFramebuffer((GLenum)target, count, list, r[0], r[1], r[2], r[3]);
        else glInvalidateFramebuffer((GLenum)target, count, list);
    }
    js_free(ctx, owned_data);
    return JS_UNDEFINED;
}
FN(m_read_buffer) { GLint m; if (toi(ctx, argv[0], &m)) return JS_EXCEPTION; UNUSED_ARGS; CUR(JS_UNDEFINED); glReadBuffer((GLenum)m); return JS_UNDEFINED; }
static JSValue draw_buffers(JSContext *ctx, struct webgl *gl, JSValueConst v) {
    GLenum *list; GLsizei count; void *owned_data;
    if (JS_IsException(enum_list(ctx, v, &list, &count, &owned_data))) return JS_EXCEPTION;
    if (use(gl)) glDrawBuffers(count, list);
    js_free(ctx, owned_data);
    return JS_UNDEFINED;
}
FN(m_draw_buffers) { (void)argc; return draw_buffers(ctx, gl, argv[0]); }
FN(m_blit_framebuffer) {
    GLint v[10]; if (ints(ctx, argc, argv, 0, 10, v)) return JS_EXCEPTION; CUR(JS_UNDEFINED);
    glBlitFramebuffer(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], (GLbitfield)v[8], (GLenum)v[9]);
    changed(gl);
    return JS_UNDEFINED;
}
static JSValue clear_buffer(JSContext *ctx, struct webgl *gl, int argc, JSValueConst *argv, int kind) {
    GLint buffer, drawbuffer; uint32_t src_offset = 0;
    if (toi(ctx, argv[0], &buffer) || toi(ctx, argv[1], &drawbuffer)) return JS_EXCEPTION;
    if (argc > 3 && tou(ctx, argv[3], &src_offset)) return JS_EXCEPTION;
    void *data, *owned_data; size_t count;
    if (list_get(ctx, argv[2], kind == 0 ? V_F32 : kind == 1 ? V_I32 : V_U32, &data, &count, &owned_data)) return JS_EXCEPTION;
    if (use(gl)) {
        size_t need = buffer == GL_COLOR ? 4 : 1;
        if (src_offset > count || count - src_offset < need) {
            synth(gl, GL_INVALID_VALUE);
        } else {
            const void *values = (const uint8_t *)data + (size_t)src_offset * 4;
            if (kind == 0) glClearBufferfv((GLenum)buffer, drawbuffer, values);
            else if (kind == 1) glClearBufferiv((GLenum)buffer, drawbuffer, values);
            else glClearBufferuiv((GLenum)buffer, drawbuffer, values);
            changed(gl);
        }
    }
    js_free(ctx, owned_data);
    return JS_UNDEFINED;
}
FN(m_clear_bufferfv) { return clear_buffer(ctx, gl, argc, argv, 0); }
FN(m_clear_bufferiv) { return clear_buffer(ctx, gl, argc, argv, 1); }
FN(m_clear_bufferuiv) { return clear_buffer(ctx, gl, argc, argv, 2); }
FN(m_clear_bufferfi) {
    GLint buffer, drawbuffer, stencil; GLfloat depth;
    if (toi(ctx, argv[0], &buffer) || toi(ctx, argv[1], &drawbuffer) || tof(ctx, argv[2], &depth) || toi(ctx, argv[3], &stencil))
        return JS_EXCEPTION;
    (void)argc;
    CUR(JS_UNDEFINED);
    glClearBufferfi((GLenum)buffer, drawbuffer, depth, stencil);
    changed(gl);
    return JS_UNDEFINED;
}
FN(m_get_internalformat_parameter) {
    GLint v[3]; if (ints(ctx, argc, argv, 0, 3, v)) return JS_EXCEPTION; CUR(JS_NULL);
    if (v[0] != GL_RENDERBUFFER || v[2] != GL_SAMPLES) {
        synth(gl, GL_INVALID_ENUM);
        return JS_NULL;
    }
    GLint count = 0, samples[16] = {0};
    glGetInternalformativ(GL_RENDERBUFFER, (GLenum)v[1], GL_NUM_SAMPLE_COUNTS, 1, &count);
    if (count < 0) count = 0;
    if (count > 16) count = 16;
    if (count) glGetInternalformativ(GL_RENDERBUFFER, (GLenum)v[1], GL_SAMPLES, count, samples);
    return typed_numbers(ctx, JS_TYPED_ARRAY_INT32, samples, (unsigned)count);
}

/* ---- readPixels ---- */

FN(m_read_pixels) {
    GLint v[6];
    if (ints(ctx, argc, argv, 0, 6, v)) return JS_EXCEPTION;
    GLenum format = (GLenum)v[4], type = (GLenum)v[5];
    JSValueConst dst = argv[6];
    if (JS_IsNumber(dst) || JS_IsBigInt(ctx, dst)) {
        if (gl->version == 1) return JS_ThrowTypeError(ctx, "readPixels requires an ArrayBufferView");
        int64_t offset;
        if (toi64(ctx, dst, &offset)) return JS_EXCEPTION;
        CUR(JS_UNDEFINED);
        GLint pack = 0;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
        if (!pack) {
            synth(gl, GL_INVALID_OPERATION);
            return JS_UNDEFINED;
        }
        if (offset < 0) {
            synth(gl, GL_INVALID_VALUE);
            return JS_UNDEFINED;
        }
        glReadPixels(v[0], v[1], v[2], v[3], format, type, (void *)(uintptr_t)offset);
        return JS_UNDEFINED;
    }
    uint32_t dst_offset = 0;
    if (gl->version == 2 && argc > 7 && tou(ctx, argv[7], &dst_offset)) return JS_EXCEPTION;
    struct view view;
    if (!view_get(ctx, dst, &view)) {
        if (JS_IsNull(dst) || JS_IsUndefined(dst)) {
            CUR(JS_UNDEFINED);
            synth(gl, GL_INVALID_VALUE);
            return JS_UNDEFINED;
        }
        return JS_ThrowTypeError(ctx, "readPixels requires an ArrayBufferView");
    }
    CUR(JS_UNDEFINED);
    if (gl->version == 2) {
        GLint pack = 0;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack);
        if (pack) {
            synth(gl, GL_INVALID_OPERATION);
            return JS_UNDEFINED;
        }
    }
    unsigned bpp = pixel_bytes(format, type);
    if (!bpp) {
        synth(gl, GL_INVALID_ENUM);
        return JS_UNDEFINED;
    }
    if (!view_matches(view.type, type)) {
        synth(gl, GL_INVALID_OPERATION);
        return JS_UNDEFINED;
    }
    if (v[2] < 0 || v[3] < 0) {
        synth(gl, GL_INVALID_VALUE);
        return JS_UNDEFINED;
    }
    if (dst_offset && !view_range(&view, dst_offset, 0)) {
        synth(gl, GL_INVALID_VALUE);
        return JS_UNDEFINED;
    }
    uint64_t need;
    if (!image_bytes(gl, true, v[2], v[3], 1, bpp, &need) || need > view.bytes) {
        synth(gl, GL_INVALID_OPERATION);
        return JS_UNDEFINED;
    }
    if (!need) return JS_UNDEFINED;
    glReadPixels(v[0], v[1], v[2], v[3], format, type, view.data);
    return JS_UNDEFINED;
}

/* ---- WebGL 2 queries, samplers, syncs, transform feedback, blocks ---- */

FN(m_begin_query) {
    GLint target; struct wobj *q;
    if (toi(ctx, argv[0], &target) || object_arg(ctx, argv[1], O_QUERY, false, &q)) return JS_EXCEPTION;
    (void)argc;
    CUR(JS_UNDEFINED);
    if (bindable(gl, q)) glBeginQuery((GLenum)target, q->name);
    return JS_UNDEFINED;
}
FN(m_end_query) { GLint t; if (toi(ctx, argv[0], &t)) return JS_EXCEPTION; UNUSED_ARGS; CUR(JS_UNDEFINED); glEndQuery((GLenum)t); return JS_UNDEFINED; }
FN(m_get_query) {
    GLint v[2]; if (ints(ctx, argc, argv, 0, 2, v)) return JS_EXCEPTION; CUR(JS_NULL);
    if (v[1] != GL_CURRENT_QUERY) {
        synth(gl, GL_INVALID_ENUM);
        return JS_NULL;
    }
    GLint name = 0;
    glGetQueryiv((GLenum)v[0], GL_CURRENT_QUERY, &name);
    return table_get(ctx, gl, O_QUERY, (GLuint)name);
}
FN(m_get_query_parameter) {
    struct wobj *q; GLint pname;
    if (object_arg(ctx, argv[0], O_QUERY, false, &q) || toi(ctx, argv[1], &pname)) return JS_EXCEPTION;
    (void)argc;
    CUR(JS_NULL);
    if (!bindable(gl, q)) return JS_NULL;
    GLuint value = 0;
    if (pname != GL_QUERY_RESULT && pname != GL_QUERY_RESULT_AVAILABLE) {
        synth(gl, GL_INVALID_ENUM);
        return JS_NULL;
    }
    glGetQueryObjectuiv(q->name, (GLenum)pname, &value);
    return pname == GL_QUERY_RESULT_AVAILABLE ? JS_NewBool(ctx, value) : JS_NewFloat64(ctx, value);
}
static JSValue sampler_parameter(JSContext *ctx, struct webgl *gl, JSValueConst *argv, bool is_float) {
    struct wobj *s; GLint pname; GLfloat f = 0; GLint i = 0;
    if (object_arg(ctx, argv[0], O_SAMPLER, false, &s) || toi(ctx, argv[1], &pname)) return JS_EXCEPTION;
    if (is_float ? tof(ctx, argv[2], &f) : toi(ctx, argv[2], &i)) return JS_EXCEPTION;
    CUR(JS_UNDEFINED);
    if (!bindable(gl, s)) return JS_UNDEFINED;
    if (is_float) glSamplerParameterf(s->name, (GLenum)pname, f);
    else glSamplerParameteri(s->name, (GLenum)pname, i);
    return JS_UNDEFINED;
}
FN(m_sampler_parameterf) { (void)argc; return sampler_parameter(ctx, gl, argv, true); }
FN(m_sampler_parameteri) { (void)argc; return sampler_parameter(ctx, gl, argv, false); }
FN(m_get_sampler_parameter) {
    struct wobj *s; GLint pname;
    if (object_arg(ctx, argv[0], O_SAMPLER, false, &s) || toi(ctx, argv[1], &pname)) return JS_EXCEPTION;
    (void)argc;
    CUR(JS_NULL);
    if (!bindable(gl, s)) return JS_NULL;
    switch (pname) {
    case GL_TEXTURE_MAX_LOD: case GL_TEXTURE_MIN_LOD: {
        GLfloat f = 0;
        glGetSamplerParameterfv(s->name, (GLenum)pname, &f);
        return JS_NewFloat64(ctx, f);
    }
    case GL_TEXTURE_COMPARE_FUNC: case GL_TEXTURE_COMPARE_MODE: case GL_TEXTURE_MAG_FILTER: case GL_TEXTURE_MIN_FILTER:
    case GL_TEXTURE_WRAP_R: case GL_TEXTURE_WRAP_S: case GL_TEXTURE_WRAP_T: {
        GLint value = 0;
        glGetSamplerParameteriv(s->name, (GLenum)pname, &value);
        return JS_NewInt32(ctx, value);
    }
    }
    synth(gl, GL_INVALID_ENUM);
    return JS_NULL;
}
static struct wobj *sync_arg(JSContext *ctx, struct webgl *gl, JSValueConst v, bool *exception) {
    struct wobj *o;
    *exception = object_arg(ctx, v, O_SYNC, false, &o) != 0;
    if (*exception || !use(gl) || !owned(gl, o)) return NULL;
    if (o->deleted) {
        synth(gl, GL_INVALID_VALUE);
        return NULL;
    }
    return o;
}
FN(m_fence_sync) {
    GLint condition; GLuint flags;
    if (toi(ctx, argv[0], &condition) || tou(ctx, argv[1], &flags)) return JS_EXCEPTION;
    (void)argc;
    CUR(JS_NULL);
    GLsync sync = glFenceSync((GLenum)condition, flags);
    if (!sync) return JS_NULL;
    struct wobj *o;
    JSValue v = wrapper_new(ctx, gl, O_SYNC, &o);
    if (JS_IsException(v)) {
        glDeleteSync(sync);
        return v;
    }
    o->sync = sync;
    return v;
}
FN(m_is_sync) {
    struct wobj *o;
    if (object_arg(ctx, argv[0], O_SYNC, true, &o)) return JS_EXCEPTION;
    UNUSED_ARGS;
    return JS_NewBool(ctx, o && o->gl == gl && o->generation == gl->generation && !o->deleted && !gl->lost);
}
FN(m_delete_sync) {
    struct wobj *o;
    if (object_arg(ctx, argv[0], O_SYNC, true, &o)) return JS_EXCEPTION;
    UNUSED_ARGS;
    CUR(JS_UNDEFINED);
    if (!o || !owned(gl, o) || o->deleted) return JS_UNDEFINED;
    o->deleted = true;
    glDeleteSync(o->sync);
    return JS_UNDEFINED;
}
FN(m_client_wait_sync) {
    GLuint flags; int64_t timeout;
    if (tou(ctx, argv[1], &flags) || toi64(ctx, argv[2], &timeout)) return JS_EXCEPTION;
    bool exception;
    struct wobj *o = sync_arg(ctx, gl, argv[0], &exception);
    (void)argc;
    if (exception) return JS_EXCEPTION;
    if (!o) return JS_NewInt32(ctx, GL_WAIT_FAILED);
    /* MAX_CLIENT_WAIT_TIMEOUT_WEBGL is 0: a page never blocks on the GPU. */
    if (timeout > 0) {
        synth(gl, GL_INVALID_OPERATION);
        return JS_NewInt32(ctx, GL_WAIT_FAILED);
    }
    return JS_NewInt32(ctx, (int32_t)glClientWaitSync(o->sync, flags, 0));
}
FN(m_wait_sync) {
    GLuint flags; int64_t timeout;
    if (tou(ctx, argv[1], &flags) || toi64(ctx, argv[2], &timeout)) return JS_EXCEPTION;
    bool exception;
    struct wobj *o = sync_arg(ctx, gl, argv[0], &exception);
    (void)argc;
    if (exception) return JS_EXCEPTION;
    if (!o) return JS_UNDEFINED;
    if (timeout != -1) {
        synth(gl, GL_INVALID_VALUE);
        return JS_UNDEFINED;
    }
    glWaitSync(o->sync, flags, GL_TIMEOUT_IGNORED);
    return JS_UNDEFINED;
}
FN(m_get_sync_parameter) {
    GLint pname;
    if (toi(ctx, argv[1], &pname)) return JS_EXCEPTION;
    bool exception;
    struct wobj *o = sync_arg(ctx, gl, argv[0], &exception);
    (void)argc;
    if (exception) return JS_EXCEPTION;
    if (!o) return JS_NULL;
    if (pname != GL_OBJECT_TYPE && pname != GL_SYNC_STATUS && pname != GL_SYNC_CONDITION && pname != GL_SYNC_FLAGS) {
        synth(gl, GL_INVALID_ENUM);
        return JS_NULL;
    }
    GLint value = 0;
    GLsizei length = 0;
    glGetSynciv(o->sync, (GLenum)pname, 1, &length, &value);
    return JS_NewInt32(ctx, value);
}
FN(m_begin_transform_feedback) { GLint m; if (toi(ctx, argv[0], &m)) return JS_EXCEPTION; UNUSED_ARGS; CUR(JS_UNDEFINED); glBeginTransformFeedback((GLenum)m); return JS_UNDEFINED; }
FN(m_end_transform_feedback) { UNUSED_ARGS; CUR(JS_UNDEFINED); glEndTransformFeedback(); return JS_UNDEFINED; }
FN(m_pause_transform_feedback) { UNUSED_ARGS; CUR(JS_UNDEFINED); glPauseTransformFeedback(); return JS_UNDEFINED; }
FN(m_resume_transform_feedback) { UNUSED_ARGS; CUR(JS_UNDEFINED); glResumeTransformFeedback(); return JS_UNDEFINED; }
/* sequence<DOMString> → owned C strings. */
static int string_list(JSContext *ctx, JSValueConst v, char ***out, uint32_t *count) {
    *out = NULL;
    *count = 0;
    if (!JS_IsObject(v)) {
        JS_ThrowTypeError(ctx, "Expected a sequence of strings");
        return -1;
    }
    JSValue lv = JS_GetPropertyStr(ctx, v, "length");
    uint32_t n = 0;
    if (JS_IsException(lv) || JS_ToUint32(ctx, &n, lv)) {
        JS_FreeValue(ctx, lv);
        return -1;
    }
    JS_FreeValue(ctx, lv);
    char **list = n ? calloc(n, sizeof *list) : NULL;
    if (n && !list) {
        JS_ThrowOutOfMemory(ctx);
        return -1;
    }
    for (uint32_t i = 0; i < n; i++) {
        JSValue item = JS_GetPropertyUint32(ctx, v, i);
        const char *s = JS_IsException(item) ? NULL : JS_ToCString(ctx, item);
        JS_FreeValue(ctx, item);
        if (!s || !(list[i] = strdup(s))) {
            if (s) {
                JS_FreeCString(ctx, s);
                JS_ThrowOutOfMemory(ctx);
            }
            for (uint32_t k = 0; k < i; k++) free(list[k]);
            free(list);
            return -1;
        }
        JS_FreeCString(ctx, s);
    }
    *out = list;
    *count = n;
    return 0;
}
static void string_list_free(char **list, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) free(list[i]);
    free(list);
}
FN(m_transform_feedback_varyings) {
    struct wobj *p; char **names; uint32_t count; GLint mode;
    if (object_arg(ctx, argv[0], O_PROGRAM, false, &p) || toi(ctx, argv[2], &mode)) return JS_EXCEPTION;
    if (string_list(ctx, argv[1], &names, &count)) return JS_EXCEPTION;
    (void)argc;
    if (use(gl) && program_usable(gl, p)) glTransformFeedbackVaryings(p->name, (GLsizei)count, (const GLchar *const *)names, (GLenum)mode);
    string_list_free(names, count);
    return JS_UNDEFINED;
}
FN(m_get_uniform_indices) {
    struct wobj *p; char **names; uint32_t count;
    if (object_arg(ctx, argv[0], O_PROGRAM, false, &p)) return JS_EXCEPTION;
    if (string_list(ctx, argv[1], &names, &count)) return JS_EXCEPTION;
    (void)argc;
    JSValue r = JS_NULL;
    if (use(gl) && program_usable(gl, p)) {
        GLuint *indices = count ? calloc(count, sizeof *indices) : NULL;
        if (count && !indices) {
            string_list_free(names, count);
            return JS_ThrowOutOfMemory(ctx);
        }
        glGetUniformIndices(p->name, (GLsizei)count, (const GLchar *const *)names, indices);
        r = JS_NewArray(ctx);
        for (uint32_t i = 0; i < count && !JS_IsException(r); i++) JS_SetPropertyUint32(ctx, r, i, JS_NewFloat64(ctx, indices[i]));
        free(indices);
    }
    string_list_free(names, count);
    return r;
}
FN(m_get_active_uniforms) {
    struct wobj *p; GLint pname; void *data, *owned_data; size_t count;
    if (object_arg(ctx, argv[0], O_PROGRAM, false, &p) || toi(ctx, argv[2], &pname)) return JS_EXCEPTION;
    if (list_get(ctx, argv[1], V_U32, &data, &count, &owned_data)) return JS_EXCEPTION;
    (void)argc;
    JSValue r = JS_NULL;
    bool known = pname == GL_UNIFORM_TYPE || pname == GL_UNIFORM_SIZE || pname == GL_UNIFORM_BLOCK_INDEX ||
                 pname == GL_UNIFORM_OFFSET || pname == GL_UNIFORM_ARRAY_STRIDE || pname == GL_UNIFORM_MATRIX_STRIDE ||
                 pname == GL_UNIFORM_IS_ROW_MAJOR;
    if (use(gl) && program_usable(gl, p)) {
        if (!known || count > INT32_MAX) {
            synth(gl, GL_INVALID_ENUM);
        } else {
            GLint *values = count ? calloc(count, sizeof *values) : NULL;
            if (count && !values) {
                js_free(ctx, owned_data);
                return JS_ThrowOutOfMemory(ctx);
            }
            GLenum before = glGetError();
            if (before) synth(gl, before);
            glGetActiveUniformsiv(p->name, (GLsizei)count, data, (GLenum)pname, values);
            GLenum e = glGetError();
            if (e) {
                synth(gl, e);
            } else {
                r = JS_NewArray(ctx);
                for (size_t i = 0; i < count && !JS_IsException(r); i++) {
                    JSValue item = pname == GL_UNIFORM_IS_ROW_MAJOR ? JS_NewBool(ctx, values[i])
                                   : pname == GL_UNIFORM_TYPE ? JS_NewFloat64(ctx, (GLuint)values[i])
                                                              : JS_NewInt32(ctx, values[i]);
                    JS_SetPropertyUint32(ctx, r, (uint32_t)i, item);
                }
            }
            free(values);
        }
    }
    js_free(ctx, owned_data);
    return r;
}
FN(m_get_uniform_block_index) {
    struct wobj *p;
    if (object_arg(ctx, argv[0], O_PROGRAM, false, &p)) return JS_EXCEPTION;
    const char *name = JS_ToCString(ctx, argv[1]);
    if (!name) return JS_EXCEPTION;
    (void)argc;
    GLuint index = GL_INVALID_INDEX;
    if (use(gl) && program_usable(gl, p)) index = glGetUniformBlockIndex(p->name, name);
    JS_FreeCString(ctx, name);
    return JS_NewFloat64(ctx, index);
}
FN(m_get_active_uniform_block_parameter) {
    struct wobj *p; GLuint index; GLint pname;
    if (object_arg(ctx, argv[0], O_PROGRAM, false, &p) || tou(ctx, argv[1], &index) || toi(ctx, argv[2], &pname))
        return JS_EXCEPTION;
    (void)argc;
    CUR(JS_NULL);
    if (!program_usable(gl, p)) return JS_NULL;
    GLint value = 0;
    switch (pname) {
    case GL_UNIFORM_BLOCK_BINDING: case GL_UNIFORM_BLOCK_DATA_SIZE: case GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS:
        glGetActiveUniformBlockiv(p->name, index, (GLenum)pname, &value);
        return JS_NewFloat64(ctx, (GLuint)value);
    case GL_UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER: case GL_UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER:
        glGetActiveUniformBlockiv(p->name, index, (GLenum)pname, &value);
        return JS_NewBool(ctx, value);
    case GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES: {
        GLint count = 0;
        GLenum before = glGetError();
        if (before) synth(gl, before);
        glGetActiveUniformBlockiv(p->name, index, GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, &count);
        GLenum e = glGetError();
        if (e) {
            synth(gl, e);
            return JS_NULL;
        }
        GLint *indices = count > 0 ? calloc((size_t)count, sizeof *indices) : NULL;
        if (count > 0 && !indices) return JS_ThrowOutOfMemory(ctx);
        if (count > 0) glGetActiveUniformBlockiv(p->name, index, GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES, indices);
        JSValue r = typed_numbers(ctx, JS_TYPED_ARRAY_UINT32, indices, count > 0 ? (unsigned)count : 0);
        free(indices);
        return r;
    }
    }
    synth(gl, GL_INVALID_ENUM);
    return JS_NULL;
}
FN(m_get_active_uniform_block_name) {
    struct wobj *p; GLuint index;
    if (object_arg(ctx, argv[0], O_PROGRAM, false, &p) || tou(ctx, argv[1], &index)) return JS_EXCEPTION;
    (void)argc;
    CUR(JS_NULL);
    if (!program_usable(gl, p)) return JS_NULL;
    GLint max = 0;
    glGetProgramiv(p->name, GL_ACTIVE_UNIFORM_BLOCK_MAX_NAME_LENGTH, &max);
    if (max < 1) max = 1;
    char *name = malloc((size_t)max);
    if (!name) return JS_ThrowOutOfMemory(ctx);
    GLsizei length = 0;
    name[0] = 0;
    GLenum before = glGetError();
    if (before) synth(gl, before);
    glGetActiveUniformBlockName(p->name, index, max, &length, name);
    GLenum e = glGetError();
    JSValue r = e ? JS_NULL : JS_NewStringLen(ctx, name, (size_t)length);
    if (e) synth(gl, e);
    free(name);
    return r;
}
FN(m_uniform_block_binding) {
    struct wobj *p; GLuint v[2];
    if (object_arg(ctx, argv[0], O_PROGRAM, false, &p) || tou(ctx, argv[1], &v[0]) || tou(ctx, argv[2], &v[1]))
        return JS_EXCEPTION;
    (void)argc;
    CUR(JS_UNDEFINED);
    if (program_usable(gl, p)) glUniformBlockBinding(p->name, v[0], v[1]);
    return JS_UNDEFINED;
}
FN(m_get_indexed_parameter) {
    GLint target; GLuint index;
    if (toi(ctx, argv[0], &target) || tou(ctx, argv[1], &index)) return JS_EXCEPTION;
    (void)argc;
    CUR(JS_NULL);
    switch (target) {
    case GL_TRANSFORM_FEEDBACK_BUFFER_BINDING: case GL_UNIFORM_BUFFER_BINDING: {
        GLint name = 0;
        glGetIntegeri_v((GLenum)target, index, &name);
        return table_get(ctx, gl, O_BUFFER, (GLuint)name);
    }
    case GL_TRANSFORM_FEEDBACK_BUFFER_SIZE: case GL_TRANSFORM_FEEDBACK_BUFFER_START: case GL_UNIFORM_BUFFER_SIZE:
    case GL_UNIFORM_BUFFER_START: {
        GLint64 value = 0;
        glGetInteger64i_v((GLenum)target, index, &value);
        return JS_NewFloat64(ctx, (double)value);
    }
    }
    synth(gl, GL_INVALID_ENUM);
    return JS_NULL;
}

/* ---- getParameter ---- */

enum { P_BOOL = 1, P_BOOL4, P_INT, P_UINT, P_INT64, P_FLOAT, P_FLOAT2, P_FLOAT4, P_INT2, P_INT4, P_STRING, P_OBJECT, P_FORMATS };
struct parameter {
    GLenum pname;
    uint8_t kind, version, object;
    int8_t ext; /* -1: none */
};
#define P1(n, k) {n, k, 1, 0, -1}
#define P2(n, k) {n, k, 2, 0, -1}
#define PO(n, o, v) {n, P_OBJECT, v, o, -1}
#define PX(n, k, x) {n, k, 1, 0, x}
static const struct parameter parameters[] = {
    P1(GL_ACTIVE_TEXTURE, P_INT), P1(GL_ALIASED_LINE_WIDTH_RANGE, P_FLOAT2), P1(GL_ALIASED_POINT_SIZE_RANGE, P_FLOAT2),
    P1(GL_ALPHA_BITS, P_INT), PO(GL_ARRAY_BUFFER_BINDING, O_BUFFER, 1), P1(GL_BLEND, P_BOOL), P1(GL_BLEND_COLOR, P_FLOAT4),
    P1(GL_BLEND_DST_ALPHA, P_INT), P1(GL_BLEND_DST_RGB, P_INT), P1(GL_BLEND_EQUATION_ALPHA, P_INT),
    P1(GL_BLEND_EQUATION_RGB, P_INT), P1(GL_BLEND_SRC_ALPHA, P_INT), P1(GL_BLEND_SRC_RGB, P_INT), P1(GL_BLUE_BITS, P_INT),
    P1(GL_COLOR_CLEAR_VALUE, P_FLOAT4), P1(GL_COLOR_WRITEMASK, P_BOOL4), P1(GL_COMPRESSED_TEXTURE_FORMATS, P_FORMATS),
    P1(GL_CULL_FACE, P_BOOL), P1(GL_CULL_FACE_MODE, P_INT), PO(GL_CURRENT_PROGRAM, O_PROGRAM, 1), P1(GL_DEPTH_BITS, P_INT),
    P1(GL_DEPTH_CLEAR_VALUE, P_FLOAT), P1(GL_DEPTH_FUNC, P_INT), P1(GL_DEPTH_RANGE, P_FLOAT2), P1(GL_DEPTH_TEST, P_BOOL),
    P1(GL_DEPTH_WRITEMASK, P_BOOL), P1(GL_DITHER, P_BOOL), PO(GL_ELEMENT_ARRAY_BUFFER_BINDING, O_BUFFER, 1),
    PO(GL_FRAMEBUFFER_BINDING, O_FRAMEBUFFER, 1), P1(GL_FRONT_FACE, P_INT), P1(GL_GENERATE_MIPMAP_HINT, P_INT),
    P1(GL_GREEN_BITS, P_INT), P1(GL_IMPLEMENTATION_COLOR_READ_FORMAT, P_INT),
    P1(GL_IMPLEMENTATION_COLOR_READ_TYPE, P_INT), P1(GL_LINE_WIDTH, P_FLOAT),
    P1(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, P_INT), P1(GL_MAX_CUBE_MAP_TEXTURE_SIZE, P_INT),
    P1(GL_MAX_FRAGMENT_UNIFORM_VECTORS, P_INT), P1(GL_MAX_RENDERBUFFER_SIZE, P_INT),
    P1(GL_MAX_TEXTURE_IMAGE_UNITS, P_INT), P1(GL_MAX_TEXTURE_SIZE, P_INT), P1(GL_MAX_VARYING_VECTORS, P_INT),
    P1(GL_MAX_VERTEX_ATTRIBS, P_INT), P1(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, P_INT),
    P1(GL_MAX_VERTEX_UNIFORM_VECTORS, P_INT), P1(GL_MAX_VIEWPORT_DIMS, P_INT2), P1(GL_PACK_ALIGNMENT, P_INT),
    P1(GL_POLYGON_OFFSET_FACTOR, P_FLOAT), P1(GL_POLYGON_OFFSET_FILL, P_BOOL), P1(GL_POLYGON_OFFSET_UNITS, P_FLOAT),
    P1(GL_RED_BITS, P_INT), PO(GL_RENDERBUFFER_BINDING, O_RENDERBUFFER, 1), P1(GL_RENDERER, P_STRING),
    P1(GL_SAMPLE_ALPHA_TO_COVERAGE, P_BOOL), P1(GL_SAMPLE_BUFFERS, P_INT), P1(GL_SAMPLE_COVERAGE, P_BOOL),
    P1(GL_SAMPLE_COVERAGE_INVERT, P_BOOL), P1(GL_SAMPLE_COVERAGE_VALUE, P_FLOAT), P1(GL_SAMPLES, P_INT),
    P1(GL_SCISSOR_BOX, P_INT4), P1(GL_SCISSOR_TEST, P_BOOL), P1(GL_SHADING_LANGUAGE_VERSION, P_STRING),
    P1(GL_STENCIL_BACK_FAIL, P_INT), P1(GL_STENCIL_BACK_FUNC, P_INT), P1(GL_STENCIL_BACK_PASS_DEPTH_FAIL, P_INT),
    P1(GL_STENCIL_BACK_PASS_DEPTH_PASS, P_INT), P1(GL_STENCIL_BACK_REF, P_INT), P1(GL_STENCIL_BACK_VALUE_MASK, P_UINT),
    P1(GL_STENCIL_BACK_WRITEMASK, P_UINT), P1(GL_STENCIL_BITS, P_INT), P1(GL_STENCIL_CLEAR_VALUE, P_INT),
    P1(GL_STENCIL_FAIL, P_INT), P1(GL_STENCIL_FUNC, P_INT), P1(GL_STENCIL_PASS_DEPTH_FAIL, P_INT),
    P1(GL_STENCIL_PASS_DEPTH_PASS, P_INT), P1(GL_STENCIL_REF, P_INT), P1(GL_STENCIL_TEST, P_BOOL),
    P1(GL_STENCIL_VALUE_MASK, P_UINT), P1(GL_STENCIL_WRITEMASK, P_UINT), P1(GL_SUBPIXEL_BITS, P_INT),
    PO(GL_TEXTURE_BINDING_2D, O_TEXTURE, 1), PO(GL_TEXTURE_BINDING_CUBE_MAP, O_TEXTURE, 1),
    P1(GL_UNPACK_ALIGNMENT, P_INT), P1(GL_VENDOR, P_STRING), P1(GL_VERSION, P_STRING), P1(GL_VIEWPORT, P_INT4),
    /* WebGL 2 */
    PO(GL_COPY_READ_BUFFER_BINDING, O_BUFFER, 2), PO(GL_COPY_WRITE_BUFFER_BINDING, O_BUFFER, 2),
    PO(GL_READ_FRAMEBUFFER_BINDING, O_FRAMEBUFFER, 2), P2(GL_FRAGMENT_SHADER_DERIVATIVE_HINT, P_INT),
    P2(GL_MAX_3D_TEXTURE_SIZE, P_INT), P2(GL_MAX_ARRAY_TEXTURE_LAYERS, P_INT), P2(GL_MAX_COLOR_ATTACHMENTS, P_INT),
    P2(GL_MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS, P_INT64), P2(GL_MAX_COMBINED_UNIFORM_BLOCKS, P_INT),
    P2(GL_MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS, P_INT64), P2(GL_MAX_DRAW_BUFFERS, P_INT),
    P2(GL_MAX_ELEMENT_INDEX, P_INT64), P2(GL_MAX_ELEMENTS_INDICES, P_INT), P2(GL_MAX_ELEMENTS_VERTICES, P_INT),
    P2(GL_MAX_FRAGMENT_INPUT_COMPONENTS, P_INT), P2(GL_MAX_FRAGMENT_UNIFORM_BLOCKS, P_INT),
    P2(GL_MAX_FRAGMENT_UNIFORM_COMPONENTS, P_INT), P2(GL_MAX_PROGRAM_TEXEL_OFFSET, P_INT), P2(GL_MAX_SAMPLES, P_INT),
    P2(GL_MAX_SERVER_WAIT_TIMEOUT, P_INT64), P2(GL_MAX_TEXTURE_LOD_BIAS, P_FLOAT),
    P2(GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS, P_INT), P2(GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS, P_INT),
    P2(GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS, P_INT), P2(GL_MAX_UNIFORM_BLOCK_SIZE, P_INT64),
    P2(GL_MAX_UNIFORM_BUFFER_BINDINGS, P_INT), P2(GL_MAX_VARYING_COMPONENTS, P_INT),
    P2(GL_MAX_VERTEX_OUTPUT_COMPONENTS, P_INT), P2(GL_MAX_VERTEX_UNIFORM_BLOCKS, P_INT),
    P2(GL_MAX_VERTEX_UNIFORM_COMPONENTS, P_INT), P2(GL_MIN_PROGRAM_TEXEL_OFFSET, P_INT), P2(GL_PACK_ROW_LENGTH, P_INT),
    P2(GL_PACK_SKIP_PIXELS, P_INT), P2(GL_PACK_SKIP_ROWS, P_INT), PO(GL_PIXEL_PACK_BUFFER_BINDING, O_BUFFER, 2),
    PO(GL_PIXEL_UNPACK_BUFFER_BINDING, O_BUFFER, 2), P2(GL_RASTERIZER_DISCARD, P_BOOL), P2(GL_READ_BUFFER, P_INT),
    PO(GL_SAMPLER_BINDING, O_SAMPLER, 2), PO(GL_TEXTURE_BINDING_2D_ARRAY, O_TEXTURE, 2),
    PO(GL_TEXTURE_BINDING_3D, O_TEXTURE, 2), P2(GL_TRANSFORM_FEEDBACK_ACTIVE, P_BOOL),
    PO(GL_TRANSFORM_FEEDBACK_BINDING, O_TF, 2), PO(GL_TRANSFORM_FEEDBACK_BUFFER_BINDING, O_BUFFER, 2),
    P2(GL_TRANSFORM_FEEDBACK_PAUSED, P_BOOL), PO(GL_UNIFORM_BUFFER_BINDING, O_BUFFER, 2),
    P2(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, P_INT), P2(GL_UNPACK_IMAGE_HEIGHT, P_INT), P2(GL_UNPACK_ROW_LENGTH, P_INT),
    P2(GL_UNPACK_SKIP_IMAGES, P_INT), P2(GL_UNPACK_SKIP_PIXELS, P_INT), P2(GL_UNPACK_SKIP_ROWS, P_INT),
    PO(GL_VERTEX_ARRAY_BINDING, O_VAO, 2),
    /* extensions */
    PX(GL_FRAGMENT_SHADER_DERIVATIVE_HINT_OES, P_INT, X_OES_standard_derivatives),
    PX(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, P_FLOAT, X_EXT_texture_filter_anisotropic),
    PX(WEBGL_UNMASKED_VENDOR, P_STRING, X_WEBGL_debug_renderer_info),
    PX(WEBGL_UNMASKED_RENDERER, P_STRING, X_WEBGL_debug_renderer_info),
    PX(GL_MAX_COLOR_ATTACHMENTS, P_INT, X_WEBGL_draw_buffers), PX(GL_MAX_DRAW_BUFFERS, P_INT, X_WEBGL_draw_buffers),
};

static JSValue string_parameter(JSContext *ctx, struct webgl *gl, GLenum pname) {
    switch (pname) {
    case GL_VENDOR: return JS_NewString(ctx, "Nocturne");
    case GL_RENDERER: return JS_NewString(ctx, "Nocturne WebGL");
    case GL_VERSION:
        return JS_NewString(ctx, gl->version == 2 ? "WebGL 2.0 (OpenGL ES 3.0 Mesa softpipe)" : "WebGL 1.0 (OpenGL ES 2.0 Mesa softpipe)");
    case GL_SHADING_LANGUAGE_VERSION:
        return JS_NewString(ctx, gl->version == 2 ? "WebGL GLSL ES 3.00 (OpenGL ES GLSL ES 3.0 Mesa softpipe)"
                                                  : "WebGL GLSL ES 1.0 (OpenGL ES GLSL ES 1.0 Mesa softpipe)");
    case WEBGL_UNMASKED_VENDOR: return JS_NewString(ctx, (const char *)glGetString(GL_VENDOR));
    case WEBGL_UNMASKED_RENDERER: return JS_NewString(ctx, (const char *)glGetString(GL_RENDERER));
    }
    return JS_NULL;
}
FN(m_get_parameter) {
    GLint pname_value; if (toi(ctx, argv[0], &pname_value)) return JS_EXCEPTION; UNUSED_ARGS; CUR(JS_NULL);
    GLenum pname = (GLenum)pname_value;
    switch (pname) {
    case WEBGL_UNPACK_FLIP_Y: return JS_NewBool(ctx, gl->flip_y);
    case WEBGL_UNPACK_PREMULTIPLY_ALPHA: return JS_NewBool(ctx, gl->premultiply_alpha);
    case WEBGL_UNPACK_COLORSPACE_CONVERSION: return JS_NewInt32(ctx, (int32_t)gl->colorspace_conversion);
    case WEBGL_MAX_CLIENT_WAIT_TIMEOUT: if (gl->version == 2) return JS_NewInt32(ctx, 0); break;
    case GL_VERTEX_ARRAY_BINDING_OES:
        if (gl->version == 1 && ext(gl, X_OES_vertex_array_object)) {
            GLint name = 0;
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &name);
            return table_get(ctx, gl, O_VAO, (GLuint)name);
        }
        break;
    }
    if (pname >= GL_DRAW_BUFFER0 && pname <= GL_DRAW_BUFFER15 && (gl->version == 2 || ext(gl, X_WEBGL_draw_buffers))) {
        GLint max = 0, value = 0;
        glGetIntegerv(GL_MAX_DRAW_BUFFERS, &max);
        if ((GLint)(pname - GL_DRAW_BUFFER0) < max) {
            glGetIntegerv(pname, &value);
            return JS_NewInt32(ctx, value);
        }
    }
    const struct parameter *p = NULL;
    for (size_t i = 0; i < sizeof parameters / sizeof *parameters && !p; i++) {
        const struct parameter *c = &parameters[i];
        if (c->pname != pname || c->version > gl->version) continue;
        if (c->ext >= 0 && !(c->version == 1 && gl->version == 2 && c->ext == X_WEBGL_draw_buffers) && !ext(gl, c->ext)) continue;
        p = c;
    }
    if (!p) {
        synth(gl, GL_INVALID_ENUM);
        return JS_NULL;
    }
    union { GLint i[4]; GLfloat f[4]; GLboolean b[4]; GLint64 l; } v;
    memset(&v, 0, sizeof v);
    switch (p->kind) {
    case P_BOOL: glGetBooleanv(pname, v.b); return JS_NewBool(ctx, v.b[0]);
    case P_BOOL4: {
        glGetBooleanv(pname, v.b);
        JSValue a = JS_NewArray(ctx);
        for (uint32_t k = 0; k < 4 && !JS_IsException(a); k++) JS_SetPropertyUint32(ctx, a, k, JS_NewBool(ctx, v.b[k]));
        return a;
    }
    case P_INT: glGetIntegerv(pname, v.i); return JS_NewInt32(ctx, v.i[0]);
    case P_UINT: glGetIntegerv(pname, v.i); return JS_NewFloat64(ctx, (GLuint)v.i[0]);
    case P_INT64: glGetInteger64v(pname, &v.l); return JS_NewFloat64(ctx, (double)v.l);
    case P_FLOAT: glGetFloatv(pname, v.f); return JS_NewFloat64(ctx, v.f[0]);
    case P_FLOAT2: glGetFloatv(pname, v.f); return typed_numbers(ctx, JS_TYPED_ARRAY_FLOAT32, v.f, 2);
    case P_FLOAT4: glGetFloatv(pname, v.f); return typed_numbers(ctx, JS_TYPED_ARRAY_FLOAT32, v.f, 4);
    case P_INT2: glGetIntegerv(pname, v.i); return typed_numbers(ctx, JS_TYPED_ARRAY_INT32, v.i, 2);
    case P_INT4: glGetIntegerv(pname, v.i); return typed_numbers(ctx, JS_TYPED_ARRAY_INT32, v.i, 4);
    case P_STRING: return string_parameter(ctx, gl, pname);
    case P_FORMATS: return typed_numbers(ctx, JS_TYPED_ARRAY_UINT32, v.i, 0);
    case P_OBJECT: glGetIntegerv(pname, v.i); return table_get(ctx, gl, p->object, (GLuint)v.i[0]);
    }
    return JS_NULL;
}

/* ---- the method tables ---- */

#define M(n, f, len, ver) {n, f, len, ver, L_UNDEF}
#define MN(n, f, len, ver) {n, f, len, ver, L_NULL}
#define MF(n, f, len, ver) {n, f, len, ver, L_FALSE}
#define ML(n, f, len, ver, lost) {n, f, len, ver, lost}
static const struct method methods[] = {
    M("activeTexture", m_active_texture, 1, 1), M("attachShader", m_attach_shader, 2, 1),
    M("bindAttribLocation", m_bind_attrib_location, 3, 1), M("bindBuffer", m_bind_buffer, 2, 1),
    M("bindFramebuffer", m_bind_framebuffer, 2, 1), M("bindRenderbuffer", m_bind_renderbuffer, 2, 1),
    M("bindTexture", m_bind_texture, 2, 1), M("blendColor", m_blend_color, 4, 1),
    M("blendEquation", m_blend_equation, 1, 1), M("blendEquationSeparate", m_blend_equation_separate, 2, 1),
    M("blendFunc", m_blend_func, 2, 1), M("blendFuncSeparate", m_blend_func_separate, 4, 1),
    M("bufferData", m_buffer_data, 3, 1), M("bufferSubData", m_buffer_sub_data, 3, 1),
    ML("checkFramebufferStatus", m_check_framebuffer_status, 1, 1, L_FB_UNSUPPORTED), M("clear", m_clear, 1, 1),
    M("clearColor", m_clear_color, 4, 1), M("clearDepth", m_clear_depth, 1, 1), M("clearStencil", m_clear_stencil, 1, 1),
    M("colorMask", m_color_mask, 4, 1), M("compileShader", m_compile_shader, 1, 1),
    M("compressedTexImage2D", m_compressed_tex, 7, 1), M("compressedTexSubImage2D", m_compressed_tex, 8, 1),
    M("copyTexImage2D", m_copy_tex_image_2d, 8, 1), M("copyTexSubImage2D", m_copy_tex_sub_image_2d, 8, 1),
    MN("createBuffer", m_create_buffer, 0, 1), MN("createFramebuffer", m_create_framebuffer, 0, 1),
    MN("createProgram", m_create_program, 0, 1), MN("createRenderbuffer", m_create_renderbuffer, 0, 1),
    MN("createShader", m_create_shader, 1, 1), MN("createTexture", m_create_texture, 0, 1),
    M("cullFace", m_cull_face, 1, 1), M("deleteBuffer", m_delete_buffer, 1, 1),
    M("deleteFramebuffer", m_delete_framebuffer, 1, 1), M("deleteProgram", m_delete_program, 1, 1),
    M("deleteRenderbuffer", m_delete_renderbuffer, 1, 1), M("deleteShader", m_delete_shader, 1, 1),
    M("deleteTexture", m_delete_texture, 1, 1), M("depthFunc", m_depth_func, 1, 1), M("depthMask", m_depth_mask, 1, 1),
    M("depthRange", m_depth_range, 2, 1), M("detachShader", m_detach_shader, 2, 1), M("disable", m_disable, 1, 1),
    M("disableVertexAttribArray", m_disable_vertex_attrib_array, 1, 1), M("drawArrays", m_draw_arrays, 3, 1),
    M("drawElements", m_draw_elements, 4, 1), M("enable", m_enable, 1, 1),
    M("enableVertexAttribArray", m_enable_vertex_attrib_array, 1, 1), M("finish", m_finish, 0, 1),
    M("flush", m_flush, 0, 1), M("framebufferRenderbuffer", m_framebuffer_renderbuffer, 4, 1),
    M("framebufferTexture2D", m_framebuffer_texture_2d, 5, 1), M("frontFace", m_front_face, 1, 1),
    M("generateMipmap", m_generate_mipmap, 1, 1), MN("getActiveAttrib", m_get_active_attrib, 2, 1),
    MN("getActiveUniform", m_get_active_uniform, 2, 1), MN("getAttachedShaders", m_get_attached_shaders, 1, 1),
    ML("getAttribLocation", m_get_attrib_location, 2, 1, L_MINUS1),
    MN("getBufferParameter", m_get_buffer_parameter, 2, 1),
    ML("getContextAttributes", m_get_context_attributes, 0, 1, L_NULL),
    ML("getError", m_get_error, 0, 1, L_ALWAYS), MN("getExtension", m_get_extension, 1, 1),
    MN("getFramebufferAttachmentParameter", m_get_framebuffer_attachment_parameter, 3, 1),
    MN("getParameter", m_get_parameter, 1, 1), MN("getProgramInfoLog", m_get_program_info_log, 1, 1),
    MN("getProgramParameter", m_get_program_parameter, 2, 1),
    MN("getRenderbufferParameter", m_get_renderbuffer_parameter, 2, 1),
    MN("getShaderInfoLog", m_get_shader_info_log, 1, 1), MN("getShaderParameter", m_get_shader_parameter, 2, 1),
    MN("getShaderPrecisionFormat", m_get_shader_precision_format, 2, 1),
    MN("getShaderSource", m_get_shader_source, 1, 1), MN("getSupportedExtensions", m_get_supported_extensions, 0, 1),
    MN("getTexParameter", m_get_tex_parameter, 2, 1), MN("getUniform", m_get_uniform, 2, 1),
    MN("getUniformLocation", m_get_uniform_location, 2, 1), MN("getVertexAttrib", m_get_vertex_attrib, 2, 1),
    ML("getVertexAttribOffset", m_get_vertex_attrib_offset, 2, 1, L_ZERO), M("hint", m_hint, 2, 1),
    MF("isBuffer", m_is_buffer, 1, 1), ML("isContextLost", m_is_context_lost, 0, 1, L_ALWAYS),
    MF("isEnabled", m_is_enabled, 1, 1), MF("isFramebuffer", m_is_framebuffer, 1, 1),
    MF("isProgram", m_is_program, 1, 1), MF("isRenderbuffer", m_is_renderbuffer, 1, 1),
    MF("isShader", m_is_shader, 1, 1), MF("isTexture", m_is_texture, 1, 1), M("lineWidth", m_line_width, 1, 1),
    M("linkProgram", m_link_program, 1, 1), M("pixelStorei", m_pixel_storei, 2, 1),
    M("polygonOffset", m_polygon_offset, 2, 1), M("readPixels", m_read_pixels, 7, 1),
    M("renderbufferStorage", m_renderbuffer_storage, 4, 1), M("sampleCoverage", m_sample_coverage, 2, 1),
    M("scissor", m_scissor, 4, 1), M("shaderSource", m_shader_source, 2, 1), M("stencilFunc", m_stencil_func, 3, 1),
    M("stencilFuncSeparate", m_stencil_func_separate, 4, 1), M("stencilMask", m_stencil_mask, 1, 1),
    M("stencilMaskSeparate", m_stencil_mask_separate, 2, 1), M("stencilOp", m_stencil_op, 3, 1),
    M("stencilOpSeparate", m_stencil_op_separate, 4, 1), M("texImage2D", m_tex_image_2d, 6, 1),
    M("texParameterf", m_tex_parameterf, 3, 1), M("texParameteri", m_tex_parameteri, 3, 1),
    M("texSubImage2D", m_tex_sub_image_2d, 7, 1), M("uniform1f", m_uniform1f, 2, 1), M("uniform1fv", m_uniform1fv, 2, 1),
    M("uniform1i", m_uniform1i, 2, 1), M("uniform1iv", m_uniform1iv, 2, 1), M("uniform2f", m_uniform2f, 3, 1),
    M("uniform2fv", m_uniform2fv, 2, 1), M("uniform2i", m_uniform2i, 3, 1), M("uniform2iv", m_uniform2iv, 2, 1),
    M("uniform3f", m_uniform3f, 4, 1), M("uniform3fv", m_uniform3fv, 2, 1), M("uniform3i", m_uniform3i, 4, 1),
    M("uniform3iv", m_uniform3iv, 2, 1), M("uniform4f", m_uniform4f, 5, 1), M("uniform4fv", m_uniform4fv, 2, 1),
    M("uniform4i", m_uniform4i, 5, 1), M("uniform4iv", m_uniform4iv, 2, 1),
    M("uniformMatrix2fv", m_uniform_matrix2fv, 3, 1), M("uniformMatrix3fv", m_uniform_matrix3fv, 3, 1),
    M("uniformMatrix4fv", m_uniform_matrix4fv, 3, 1), M("useProgram", m_use_program, 1, 1),
    M("validateProgram", m_validate_program, 1, 1), M("vertexAttrib1f", m_vertex_attrib1f, 2, 1),
    M("vertexAttrib1fv", m_vertex_attrib1fv, 2, 1), M("vertexAttrib2f", m_vertex_attrib2f, 3, 1),
    M("vertexAttrib2fv", m_vertex_attrib2fv, 2, 1), M("vertexAttrib3f", m_vertex_attrib3f, 4, 1),
    M("vertexAttrib3fv", m_vertex_attrib3fv, 2, 1), M("vertexAttrib4f", m_vertex_attrib4f, 5, 1),
    M("vertexAttrib4fv", m_vertex_attrib4fv, 2, 1), M("vertexAttribPointer", m_vertex_attrib_pointer, 6, 1),
    M("viewport", m_viewport, 4, 1),
    /* WebGL 2 */
    M("beginQuery", m_begin_query, 2, 2), M("beginTransformFeedback", m_begin_transform_feedback, 1, 2),
    M("bindBufferBase", m_bind_buffer_base, 3, 2), M("bindBufferRange", m_bind_buffer_range, 5, 2),
    M("bindSampler", m_bind_sampler, 2, 2), M("bindTransformFeedback", m_bind_transform_feedback, 2, 2),
    M("bindVertexArray", m_bind_vertex_array, 1, 2), M("blitFramebuffer", m_blit_framebuffer, 10, 2),
    M("clearBufferfi", m_clear_bufferfi, 4, 2), M("clearBufferfv", m_clear_bufferfv, 3, 2),
    M("clearBufferiv", m_clear_bufferiv, 3, 2), M("clearBufferuiv", m_clear_bufferuiv, 3, 2),
    ML("clientWaitSync", m_client_wait_sync, 3, 2, L_WAIT_FAILED),
    M("compressedTexImage3D", m_compressed_tex, 8, 2), M("compressedTexSubImage3D", m_compressed_tex, 10, 2),
    M("copyBufferSubData", m_copy_buffer_sub_data, 5, 2), M("copyTexSubImage3D", m_copy_tex_sub_image_3d, 9, 2),
    MN("createQuery", m_create_query, 0, 2), MN("createSampler", m_create_sampler, 0, 2),
    MN("createTransformFeedback", m_create_transform_feedback, 0, 2), MN("createVertexArray", m_create_vertex_array, 0, 2),
    M("deleteQuery", m_delete_query, 1, 2), M("deleteSampler", m_delete_sampler, 1, 2), M("deleteSync", m_delete_sync, 1, 2),
    M("deleteTransformFeedback", m_delete_transform_feedback, 1, 2), M("deleteVertexArray", m_delete_vertex_array, 1, 2),
    M("drawArraysInstanced", m_draw_arrays_instanced, 4, 2), M("drawBuffers", m_draw_buffers, 1, 2),
    M("drawElementsInstanced", m_draw_elements_instanced, 5, 2), M("drawRangeElements", m_draw_range_elements, 6, 2),
    M("endQuery", m_end_query, 1, 2), M("endTransformFeedback", m_end_transform_feedback, 0, 2),
    MN("fenceSync", m_fence_sync, 2, 2), M("framebufferTextureLayer", m_framebuffer_texture_layer, 5, 2),
    MN("getActiveUniformBlockName", m_get_active_uniform_block_name, 2, 2),
    MN("getActiveUniformBlockParameter", m_get_active_uniform_block_parameter, 3, 2),
    MN("getActiveUniforms", m_get_active_uniforms, 3, 2), M("getBufferSubData", m_get_buffer_sub_data, 3, 2),
    ML("getFragDataLocation", m_get_frag_data_location, 2, 2, L_MINUS1),
    MN("getIndexedParameter", m_get_indexed_parameter, 2, 2),
    MN("getInternalformatParameter", m_get_internalformat_parameter, 3, 2), MN("getQuery", m_get_query, 2, 2),
    MN("getQueryParameter", m_get_query_parameter, 2, 2), MN("getSamplerParameter", m_get_sampler_parameter, 2, 2),
    MN("getSyncParameter", m_get_sync_parameter, 2, 2),
    MN("getTransformFeedbackVarying", m_get_transform_feedback_varying, 2, 2),
    ML("getUniformBlockIndex", m_get_uniform_block_index, 2, 2, L_INVALID_INDEX),
    MN("getUniformIndices", m_get_uniform_indices, 2, 2), M("invalidateFramebuffer", m_invalidate_framebuffer, 2, 2),
    M("invalidateSubFramebuffer", m_invalidate_framebuffer, 6, 2), MF("isQuery", m_is_query, 1, 2),
    MF("isSampler", m_is_sampler, 1, 2), MF("isSync", m_is_sync, 1, 2),
    MF("isTransformFeedback", m_is_transform_feedback, 1, 2), MF("isVertexArray", m_is_vertex_array, 1, 2),
    M("pauseTransformFeedback", m_pause_transform_feedback, 0, 2), M("readBuffer", m_read_buffer, 1, 2),
    M("renderbufferStorageMultisample", m_renderbuffer_storage_multisample, 5, 2),
    M("resumeTransformFeedback", m_resume_transform_feedback, 0, 2),
    M("samplerParameterf", m_sampler_parameterf, 3, 2), M("samplerParameteri", m_sampler_parameteri, 3, 2),
    M("texImage3D", m_tex_image_3d, 10, 2), M("texStorage2D", m_tex_storage_2d, 5, 2),
    M("texStorage3D", m_tex_storage_3d, 6, 2), M("texSubImage3D", m_tex_sub_image_3d, 11, 2),
    M("transformFeedbackVaryings", m_transform_feedback_varyings, 3, 2), M("uniform1ui", m_uniform1ui, 2, 2),
    M("uniform1uiv", m_uniform1uiv, 2, 2), M("uniform2ui", m_uniform2ui, 3, 2), M("uniform2uiv", m_uniform2uiv, 2, 2),
    M("uniform3ui", m_uniform3ui, 4, 2), M("uniform3uiv", m_uniform3uiv, 2, 2), M("uniform4ui", m_uniform4ui, 5, 2),
    M("uniform4uiv", m_uniform4uiv, 2, 2), M("uniformBlockBinding", m_uniform_block_binding, 3, 2),
    M("uniformMatrix2x3fv", m_uniform_matrix2x3fv, 3, 2), M("uniformMatrix2x4fv", m_uniform_matrix2x4fv, 3, 2),
    M("uniformMatrix3x2fv", m_uniform_matrix3x2fv, 3, 2), M("uniformMatrix3x4fv", m_uniform_matrix3x4fv, 3, 2),
    M("uniformMatrix4x2fv", m_uniform_matrix4x2fv, 3, 2), M("uniformMatrix4x3fv", m_uniform_matrix4x3fv, 3, 2),
    M("vertexAttribDivisor", m_vertex_attrib_divisor, 2, 2), M("vertexAttribI4i", m_vertex_attrib_i4i, 5, 2),
    M("vertexAttribI4iv", m_vertex_attrib_i4iv, 2, 2), M("vertexAttribI4ui", m_vertex_attrib_i4ui, 5, 2),
    M("vertexAttribI4uiv", m_vertex_attrib_i4uiv, 2, 2), M("vertexAttribIPointer", m_vertex_attrib_i_pointer, 5, 2),
    M("waitSync", m_wait_sync, 3, 2),
};

/* debug_js only: inclusive time per entry point and in presentation, written
 * to the debug log every two seconds of wall time with the costliest five. */
#define METHOD_COUNT (sizeof methods / sizeof *methods)
static uint64_t profile_cycles[METHOD_COUNT + 1], profile_calls[METHOD_COUNT + 1], profile_tsc, profile_ms;
void web_webgl_set_profile(bool on) { profile_on = on; }
static void profile_add(size_t slot, uint64_t cycles) {
    profile_cycles[slot] += cycles;
    profile_calls[slot]++;
    uint64_t now = uptime_ms(), tsc = __builtin_ia32_rdtsc();
    if (!profile_ms) { profile_ms = now; profile_tsc = tsc; return; }
    if (now - profile_ms < 2000) return;
    double per_ms = (double)(tsc - profile_tsc) / (double)(now - profile_ms);
    char line[512];
    int len = snprintf(line, sizeof line, "[webgl] profile %llu ms:", (unsigned long long)(now - profile_ms));
    for (int k = 0; k < 5; k++) {
        size_t best = SIZE_MAX;
        for (size_t i = 0; i <= METHOD_COUNT; i++)
            if (profile_calls[i] && (best == SIZE_MAX || profile_cycles[i] > profile_cycles[best])) best = i;
        if (best == SIZE_MAX) break;
        if (len > 0 && (size_t)len < sizeof line)
            len += snprintf(line + len, sizeof line - (size_t)len, " %s %.0f ms/%llu", best == METHOD_COUNT ? "present" : methods[best].name,
                            (double)profile_cycles[best] / per_ms, (unsigned long long)profile_calls[best]);
        profile_calls[best] = 0;
    }
    fprintf(stderr, "%s\n", line);
    memset(profile_cycles, 0, sizeof profile_cycles);
    memset(profile_calls, 0, sizeof profile_calls);
    profile_ms = now;
    profile_tsc = tsc;
}
static void profile_present(uint64_t cycles) { profile_add(METHOD_COUNT, cycles); }

static JSValue trampoline(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    const struct method *m = &methods[magic];
    struct webgl *gl = context_of(this_val, m->version == 2);
    if (!gl) return JS_ThrowTypeError(ctx, "%s called on an object that is not a WebGL%s context", m->name, m->version == 2 ? "2" : "");
    if (argc < m->length) return JS_ThrowTypeError(ctx, "%s requires %d arguments, got %d", m->name, m->length, argc);
    if (gl->lost && m->lost != L_ALWAYS) {
        /* Conversions still run (and may throw) before the lost result. */
        return lost_value(m->lost);
    }
    if (!profile_on) return m->fn(ctx, gl, argc, argv);
    uint64_t start = __builtin_ia32_rdtsc();
    JSValue result = m->fn(ctx, gl, argc, argv);
    profile_add((size_t)magic, __builtin_ia32_rdtsc() - start);
    return result;
}

/* ---- attributes ---- */

static JSValue get_canvas(JSContext *ctx, JSValueConst this_val) {
    struct webgl *gl = context_of(this_val, false);
    if (!gl) return JS_ThrowTypeError(ctx, "Illegal WebGL receiver");
    return JS_DupValue(ctx, gl->canvas);
}
static JSValue get_drawing_buffer(JSContext *ctx, JSValueConst this_val, int magic) {
    struct webgl *gl = context_of(this_val, false);
    if (!gl) return JS_ThrowTypeError(ctx, "Illegal WebGL receiver");
    if (gl->lost) return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, (int32_t)(magic ? gl->height : gl->width));
}
static JSValue get_color_space(JSContext *ctx, JSValueConst this_val, int magic) {
    struct webgl *gl = context_of(this_val, false);
    if (!gl) return JS_ThrowTypeError(ctx, "Illegal WebGL receiver");
    return JS_NewString(ctx, (magic ? gl->unpack_p3 : gl->drawing_p3) ? "display-p3" : "srgb");
}
static JSValue set_color_space(JSContext *ctx, JSValueConst this_val, JSValueConst value, int magic) {
    struct webgl *gl = context_of(this_val, false);
    if (!gl) return JS_ThrowTypeError(ctx, "Illegal WebGL receiver");
    const char *s = JS_ToCString(ctx, value);
    if (!s) return JS_EXCEPTION;
    /* Invalid enumeration values are ignored by the setter. */
    if (!strcmp(s, "srgb") || !strcmp(s, "display-p3")) {
        bool p3 = s[0] == 'd';
        if (magic) gl->unpack_p3 = p3;
        else gl->drawing_p3 = p3;
    }
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

/* ---- extension objects ---- */

static struct webgl *extension_of(JSValueConst v, int id) { return JS_GetOpaque(v, ext_class[id]); }
#define EXT_FN(fname, id, minargs, body) \
    static JSValue fname(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) { \
        struct webgl *gl = extension_of(this_val, id); \
        if (!gl) return JS_ThrowTypeError(ctx, "Illegal extension receiver"); \
        if (argc < minargs) return JS_ThrowTypeError(ctx, "Not enough arguments"); \
        body \
    }
EXT_FN(x_draw_arrays_instanced, X_ANGLE_instanced_arrays, 4,
       if (gl->lost) return JS_UNDEFINED; return draw_arrays_instanced(ctx, gl, argc, argv);)
EXT_FN(x_draw_elements_instanced, X_ANGLE_instanced_arrays, 5,
       if (gl->lost) return JS_UNDEFINED; return draw_elements_instanced(ctx, gl, argc, argv);)
EXT_FN(x_vertex_attrib_divisor, X_ANGLE_instanced_arrays, 2,
       if (gl->lost) return JS_UNDEFINED; return vertex_attrib_divisor(ctx, gl, argv);)
EXT_FN(x_create_vertex_array, X_OES_vertex_array_object, 0,
       (void)argv; if (gl->lost) return JS_NULL; return create_object(ctx, gl, O_VAO);)
EXT_FN(x_delete_vertex_array, X_OES_vertex_array_object, 1,
       if (gl->lost) return JS_UNDEFINED; return delete_object(ctx, gl, argv[0], O_VAO);)
EXT_FN(x_is_vertex_array, X_OES_vertex_array_object, 1,
       if (gl->lost) return JS_FALSE; return is_object(ctx, gl, argv[0], O_VAO);)
EXT_FN(x_bind_vertex_array, X_OES_vertex_array_object, 1,
       if (gl->lost) return JS_UNDEFINED; return m_bind_vertex_array(ctx, gl, argc, argv);)
EXT_FN(x_draw_buffers, X_WEBGL_draw_buffers, 1,
       if (gl->lost) return JS_UNDEFINED; return draw_buffers(ctx, gl, argv[0]);)

static void fire_context_event(JSContext *ctx, struct webgl *gl, const char *type, bool cancelable, bool *prevented) {
    JSValue ctor = realm_get(ctx, "event");
    if (!JS_IsFunction(ctx, ctor)) {
        JS_FreeValue(ctx, ctor);
        return;
    }
    JSValue init = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, init, "cancelable", JS_NewBool(ctx, cancelable));
    JSValue args[2] = {JS_NewString(ctx, type), init};
    JSValue event = JS_CallConstructor(ctx, ctor, 2, args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, ctor);
    if (JS_IsException(event)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    JSValue dispatch = JS_GetPropertyStr(ctx, gl->canvas, "dispatchEvent");
    JSValue result = JS_IsFunction(ctx, dispatch) ? JS_Call(ctx, dispatch, gl->canvas, 1, (JSValueConst *)&event) : JS_UNDEFINED;
    if (JS_IsException(result)) JS_FreeValue(ctx, JS_GetException(ctx));
    if (prevented) {
        JSValue d = JS_GetPropertyStr(ctx, event, "defaultPrevented");
        *prevented = JS_ToBool(ctx, d) > 0;
        JS_FreeValue(ctx, d);
    }
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, dispatch);
    JS_FreeValue(ctx, event);
}
/* Queued tasks: data[0] is the extension object, magic 0 = lost, 1 = restored. */
static JSValue context_task(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic, JSValue *data) {
    (void)this_val; (void)argc; (void)argv;
    struct webgl *gl = extension_of(data[0], X_WEBGL_lose_context);
    if (!gl || !gl->js_alive) return JS_UNDEFINED;
    if (magic == 0) {
        bool prevented = false;
        fire_context_event(ctx, gl, "webglcontextlost", true, &prevented);
        gl->restore_allowed = prevented;
    } else if (gl->lost && gl->restore_allowed) {
        tables_clear(gl->rt, gl);
        gl->generation++;
        gl->enabled = 0;
        for (int i = 0; i < X_COUNT; i++) {
            JS_FreeValue(ctx, gl->ext_object[i]);
            gl->ext_object[i] = JS_UNDEFINED;
        }
        if (!context_setup(gl)) return JS_UNDEFINED;
        gl->lost = gl->lost_by_extension = gl->restore_allowed = false;
        gl->serial++;
        if (gl->changed) gl->changed(gl->owner);
        fire_context_event(ctx, gl, "webglcontextrestored", false, NULL);
    }
    return JS_UNDEFINED;
}
static void queue_context_task(JSContext *ctx, JSValueConst ext_object, int which) {
    JSValue defer = realm_get(ctx, "defer");
    if (!JS_IsFunction(ctx, defer)) {
        JS_FreeValue(ctx, defer);
        return;
    }
    JSValue fn = JS_NewCFunctionData(ctx, context_task, 0, which, 1, (JSValueConst *)&ext_object);
    JSValue args[2] = {fn, JS_NewInt32(ctx, 0)};
    JSValue r = JS_Call(ctx, defer, JS_UNDEFINED, 2, args);
    if (JS_IsException(r)) JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, defer);
}
EXT_FN(x_lose_context, X_WEBGL_lose_context, 0, (void)argv;
       if (gl->lost) { synth(gl, GL_INVALID_OPERATION); return JS_UNDEFINED; }
       gl->lost = gl->lost_by_extension = gl->lost_error = true;
       gl->error_count = 0;
       if (gl->ngl) { ngl_context_destroy(gl->ngl); gl->ngl = NULL; }
       gl->dead_count = 0;
       queue_context_task(ctx, this_val, 0);
       return JS_UNDEFINED;)
EXT_FN(x_restore_context, X_WEBGL_lose_context, 0, (void)argv;
       if (!gl->lost || !gl->lost_by_extension) { synth(gl, GL_INVALID_OPERATION); return JS_UNDEFINED; }
       queue_context_task(ctx, this_val, 1);
       return JS_UNDEFINED;)

static const JSCFunctionListEntry ext_angle[] = {
    JS_CFUNC_DEF("drawArraysInstancedANGLE", 4, x_draw_arrays_instanced),
    JS_CFUNC_DEF("drawElementsInstancedANGLE", 5, x_draw_elements_instanced),
    JS_CFUNC_DEF("vertexAttribDivisorANGLE", 2, x_vertex_attrib_divisor),
    JS_PROP_INT32_DEF("VERTEX_ATTRIB_ARRAY_DIVISOR_ANGLE", 0x88FE, JS_PROP_ENUMERABLE),
};
static const JSCFunctionListEntry ext_minmax[] = {
    JS_PROP_INT32_DEF("MIN_EXT", 0x8007, JS_PROP_ENUMERABLE), JS_PROP_INT32_DEF("MAX_EXT", 0x8008, JS_PROP_ENUMERABLE),
};
static const JSCFunctionListEntry ext_half_float_buffer[] = {
    JS_PROP_INT32_DEF("RGBA16F_EXT", 0x881A, JS_PROP_ENUMERABLE), JS_PROP_INT32_DEF("RGB16F_EXT", 0x881B, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE_EXT", 0x8211, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("UNSIGNED_NORMALIZED_EXT", 0x8C17, JS_PROP_ENUMERABLE),
};
static const JSCFunctionListEntry ext_srgb[] = {
    JS_PROP_INT32_DEF("SRGB_EXT", 0x8C40, JS_PROP_ENUMERABLE), JS_PROP_INT32_DEF("SRGB_ALPHA_EXT", 0x8C42, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("SRGB8_ALPHA8_EXT", 0x8C43, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING_EXT", 0x8210, JS_PROP_ENUMERABLE),
};
static const JSCFunctionListEntry ext_anisotropic[] = {
    JS_PROP_INT32_DEF("TEXTURE_MAX_ANISOTROPY_EXT", 0x84FE, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("MAX_TEXTURE_MAX_ANISOTROPY_EXT", 0x84FF, JS_PROP_ENUMERABLE),
};
static const JSCFunctionListEntry ext_parallel[] = {
    JS_PROP_INT32_DEF("COMPLETION_STATUS_KHR", 0x91B1, JS_PROP_ENUMERABLE),
};
static const JSCFunctionListEntry ext_derivatives[] = {
    JS_PROP_INT32_DEF("FRAGMENT_SHADER_DERIVATIVE_HINT_OES", 0x8B8B, JS_PROP_ENUMERABLE),
};
static const JSCFunctionListEntry ext_half_float[] = {
    JS_PROP_INT32_DEF("HALF_FLOAT_OES", 0x8D61, JS_PROP_ENUMERABLE),
};
static const JSCFunctionListEntry ext_vao[] = {
    JS_CFUNC_DEF("createVertexArrayOES", 0, x_create_vertex_array),
    JS_CFUNC_DEF("deleteVertexArrayOES", 1, x_delete_vertex_array),
    JS_CFUNC_DEF("isVertexArrayOES", 1, x_is_vertex_array), JS_CFUNC_DEF("bindVertexArrayOES", 1, x_bind_vertex_array),
    JS_PROP_INT32_DEF("VERTEX_ARRAY_BINDING_OES", 0x85B5, JS_PROP_ENUMERABLE),
};
static const JSCFunctionListEntry ext_float_buffer[] = {
    JS_PROP_INT32_DEF("RGBA32F_EXT", 0x8814, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE_EXT", 0x8211, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("UNSIGNED_NORMALIZED_EXT", 0x8C17, JS_PROP_ENUMERABLE),
};
static const JSCFunctionListEntry ext_renderer_info[] = {
    JS_PROP_INT32_DEF("UNMASKED_VENDOR_WEBGL", 0x9245, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("UNMASKED_RENDERER_WEBGL", 0x9246, JS_PROP_ENUMERABLE),
};
static const JSCFunctionListEntry ext_depth_texture[] = {
    JS_PROP_INT32_DEF("UNSIGNED_INT_24_8_WEBGL", 0x84FA, JS_PROP_ENUMERABLE),
};
static const JSCFunctionListEntry ext_draw_buffers[] = {
    JS_CFUNC_DEF("drawBuffersWEBGL", 1, x_draw_buffers),
    JS_PROP_INT32_DEF("MAX_COLOR_ATTACHMENTS_WEBGL", 0x8CDF, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("MAX_DRAW_BUFFERS_WEBGL", 0x8824, JS_PROP_ENUMERABLE),
};
static const JSCFunctionListEntry ext_lose[] = {
    JS_CFUNC_DEF("loseContext", 0, x_lose_context), JS_CFUNC_DEF("restoreContext", 0, x_restore_context),
};
struct extension_members { const JSCFunctionListEntry *list; int count; };
#define LIST(a) {a, (int)(sizeof a / sizeof *a)}
static const struct extension_members extension_members[X_COUNT] = {
    [X_ANGLE_instanced_arrays] = LIST(ext_angle), [X_EXT_blend_minmax] = LIST(ext_minmax),
    [X_EXT_color_buffer_half_float] = LIST(ext_half_float_buffer), [X_EXT_sRGB] = LIST(ext_srgb),
    [X_EXT_texture_filter_anisotropic] = LIST(ext_anisotropic), [X_KHR_parallel_shader_compile] = LIST(ext_parallel),
    [X_OES_standard_derivatives] = LIST(ext_derivatives), [X_OES_texture_half_float] = LIST(ext_half_float),
    [X_OES_vertex_array_object] = LIST(ext_vao), [X_WEBGL_color_buffer_float] = LIST(ext_float_buffer),
    [X_WEBGL_debug_renderer_info] = LIST(ext_renderer_info), [X_WEBGL_depth_texture] = LIST(ext_depth_texture),
    [X_WEBGL_draw_buffers] = LIST(ext_draw_buffers), [X_WEBGL_lose_context] = LIST(ext_lose),
};

/* ---- wrapper attributes ---- */

static struct wobj *wrapper_of(JSValueConst v, int kind) { return JS_GetOpaque(v, kind_class[kind]); }
static JSValue info_get(JSContext *ctx, JSValueConst this_val, int magic) {
    struct wobj *o = wrapper_of(this_val, O_INFO);
    if (!o) return JS_ThrowTypeError(ctx, "Illegal WebGLActiveInfo receiver");
    if (magic == 0) return JS_NewInt32(ctx, o->size);
    if (magic == 1) return JS_NewInt32(ctx, (int32_t)o->type);
    return JS_NewString(ctx, o->text ? o->text : "");
}
static JSValue precision_get(JSContext *ctx, JSValueConst this_val, int magic) {
    struct wobj *o = wrapper_of(this_val, O_PRECISION);
    if (!o) return JS_ThrowTypeError(ctx, "Illegal WebGLShaderPrecisionFormat receiver");
    return JS_NewInt32(ctx, magic == 0 ? o->range_min : magic == 1 ? o->range_max : o->precision);
}
static const JSCFunctionListEntry info_members[] = {
    JS_CGETSET_MAGIC_DEF("size", info_get, NULL, 0), JS_CGETSET_MAGIC_DEF("type", info_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("name", info_get, NULL, 2),
};
static const JSCFunctionListEntry precision_members[] = {
    JS_CGETSET_MAGIC_DEF("rangeMin", precision_get, NULL, 0), JS_CGETSET_MAGIC_DEF("rangeMax", precision_get, NULL, 1),
    JS_CGETSET_MAGIC_DEF("precision", precision_get, NULL, 2),
};
static const JSCFunctionListEntry context_members[] = {
    JS_CGETSET_DEF("canvas", get_canvas, NULL),
    JS_CGETSET_MAGIC_DEF("drawingBufferWidth", get_drawing_buffer, NULL, 0),
    JS_CGETSET_MAGIC_DEF("drawingBufferHeight", get_drawing_buffer, NULL, 1),
    JS_CGETSET_MAGIC_DEF("drawingBufferColorSpace", get_color_space, set_color_space, 0),
    JS_CGETSET_MAGIC_DEF("unpackColorSpace", get_color_space, set_color_space, 1),
};

/* ---- host entry points ---- */

/* host.webglBind({source, event, defer, DOMException}) records realm
 * services the native code calls back into. */
static JSValue host_bind(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "webglBind requires services");
    JSValue realm = JS_GetClassProto(ctx, realm_class);
    static const char *const keys[] = {"source", "event", "defer", "DOMException"};
    for (int i = 0; i < 4; i++) {
        JSValue v = JS_GetPropertyStr(ctx, argv[0], keys[i]);
        if (JS_IsException(v)) {
            JS_FreeValue(ctx, realm);
            return v;
        }
        JS_SetPropertyStr(ctx, realm, keys[i], v);
    }
    JS_FreeValue(ctx, realm);
    return JS_UNDEFINED;
}
static JSValue commit_job(JSContext *ctx, int argc, JSValueConst *argv) {
    (void)argc;
    JSValue r = JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);
    if (JS_IsException(r)) return r;
    JS_FreeValue(ctx, r);
    return JS_UNDEFINED;
}
static void commit_hook(void *owner) {
    struct webgl *gl = owner;
    if (gl->commit_armed || !gl->js_alive || !JS_IsFunction(gl->commit_ctx, gl->commit)) return;
    if (!JS_EnqueueJob(gl->commit_ctx, commit_job, 1, (JSValueConst *)&gl->commit)) gl->commit_armed = true;
}
/* host.webglCreate(canvas, version, options, width, height[, commit]) for
 * realms without a DOM canvas (Worker OffscreenCanvas). commit is called
 * from a job after rendering first changes the drawing buffer following a
 * webglPresent (or creation). */
static JSValue host_create(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint32_t version = 1, width = 0, height = 0;
    if (argc < 5 || JS_ToUint32(ctx, &version, argv[1]) || JS_ToUint32(ctx, &width, argv[3]) ||
        JS_ToUint32(ctx, &height, argv[4]))
        return argc < 5 ? JS_ThrowTypeError(ctx, "webglCreate requires 5 arguments") : JS_EXCEPTION;
    struct web_webgl *handle;
    JSValue object = web_webgl_create(ctx, argv[0], (int)version, argv[2], width, height, &handle);
    if (handle && argc > 5 && JS_IsFunction(ctx, argv[5])) {
        struct webgl *gl = (struct webgl *)handle;
        gl->commit = JS_DupValue(ctx, argv[5]);
        gl->commit_ctx = ctx;
        web_webgl_set_owner(handle, commit_hook, gl);
    }
    return object;
}
/* host.webglResize(context, width, height) and host.webglPresent(context,
 * Int32Array header, Uint32Array pixels) for OffscreenCanvas owners. */
static JSValue host_resize(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    uint32_t width, height;
    struct webgl *gl = argc >= 3 ? context_of(argv[0], false) : NULL;
    if (!gl) return JS_ThrowTypeError(ctx, "webglResize requires a WebGL context");
    if (JS_ToUint32(ctx, &width, argv[1]) || JS_ToUint32(ctx, &height, argv[2])) return JS_EXCEPTION;
    web_webgl_resize((struct web_webgl *)gl, width, height);
    return JS_UNDEFINED;
}
static JSValue host_serial(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    struct webgl *gl = argc >= 1 ? context_of(argv[0], false) : NULL;
    if (!gl) return JS_ThrowTypeError(ctx, "webglSerial requires a WebGL context");
    return JS_NewFloat64(ctx, (double)gl->serial);
}
/* Copies the drawing buffer into a Uint32Array of width*height pixels
 * (0xAARRGGBB) and ends the frame. Returns false when nothing was read. */
static JSValue host_present(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    struct webgl *gl = argc >= 4 ? context_of(argv[0], false) : NULL;
    if (!gl) return JS_ThrowTypeError(ctx, "webglPresent requires a WebGL context");
    uint32_t width, height;
    if (JS_ToUint32(ctx, &width, argv[2]) || JS_ToUint32(ctx, &height, argv[3])) return JS_EXCEPTION;
    struct view view;
    if (!view_get(ctx, argv[1], &view) || view.type != V_U32) return JS_ThrowTypeError(ctx, "webglPresent requires a Uint32Array");
    gl->commit_armed = false;
    if ((uint64_t)width * height * 4 > view.bytes || !width || !height) return JS_FALSE;
    struct ngl_context *previous = ngl_get_current();
    bool ok = web_webgl_present((struct web_webgl *)gl, (uint32_t *)view.data, width, height, true);
    if (previous && previous != ngl_get_current()) ngl_make_current(previous);
    return JS_NewBool(ctx, ok);
}

/* ---- installation ---- */

static JSValue illegal_constructor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}
static void define_constants(JSContext *ctx, JSValueConst target, const struct webgl_constant *list, size_t count) {
    for (size_t i = 0; i < count; i++)
        JS_DefinePropertyValueStr(ctx, target, list[i].name, JS_NewFloat64(ctx, list[i].value), JS_PROP_ENUMERABLE);
}
static void set_tag(JSContext *ctx, JSValueConst proto, const char *name) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue symbol = JS_GetPropertyStr(ctx, global, "Symbol");
    JSValue tag = JS_GetPropertyStr(ctx, symbol, "toStringTag");
    JSAtom atom = JS_ValueToAtom(ctx, tag);
    JS_DefinePropertyValue(ctx, proto, atom, JS_NewString(ctx, name), JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, atom);
    JS_FreeValue(ctx, tag);
    JS_FreeValue(ctx, symbol);
    JS_FreeValue(ctx, global);
}
/* An interface object with a prototype; exposed on the global when global. */
static JSValue interface(JSContext *ctx, JSValueConst global, const char *name, JSClassID cls, bool exposed) {
    JSValue proto = JS_NewObject(ctx);
    set_tag(ctx, proto, name);
    if (exposed) {
        JSValue ctor = JS_NewCFunction2(ctx, illegal_constructor, name, 0, JS_CFUNC_constructor_or_func, 0);
        JS_SetConstructor(ctx, ctor, proto);
        JS_DefinePropertyValueStr(ctx, global, name, ctor, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    }
    JS_SetClassProto(ctx, cls, JS_DupValue(ctx, proto));
    return proto;
}

void web_js_webgl_init(JSContext *ctx, JSValueConst host, web_webgl_source_fn source) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    source_hook = source;
    if (!gl1_class) {
        JS_NewClassID(&gl1_class);
        JS_NewClassID(&gl2_class);
        JS_NewClassID(&realm_class);
        for (int i = 0; i < O_KINDS; i++) JS_NewClassID(&kind_class[i]);
        for (int i = 0; i < X_COUNT; i++) JS_NewClassID(&ext_class[i]);
    }
    if (!JS_IsRegisteredClass(rt, gl1_class)) {
        JSClassDef context1 = {"WebGLRenderingContext", context_finalizer, context_mark, NULL, NULL};
        JSClassDef context2 = {"WebGL2RenderingContext", context_finalizer, context_mark, NULL, NULL};
        JSClassDef realm = {"WebGLRealm", NULL, NULL, NULL, NULL};
        JS_NewClass(rt, gl1_class, &context1);
        JS_NewClass(rt, gl2_class, &context2);
        JS_NewClass(rt, realm_class, &realm);
        for (int i = 0; i < O_KINDS; i++) {
            JSClassDef def = {kind_names[i], wrapper_finalizer, NULL, NULL, NULL};
            JS_NewClass(rt, kind_class[i], &def);
        }
        for (int i = 0; i < X_COUNT; i++) {
            JSClassDef def = {extensions[i].interface, extension_finalizer, NULL, NULL, NULL};
            JS_NewClass(rt, ext_class[i], &def);
        }
    }
    /* Typed array, ArrayBuffer and DataView classes, detected from samples. */
    JSValue zero = JS_NewInt32(ctx, 0);
    for (int i = 0; i < V_DATAVIEW; i++) {
        JSValue sample = JS_NewTypedArray(ctx, 1, &zero, (JSTypedArrayEnum)i);
        view_class[i] = JS_GetClassID(sample);
        JS_FreeValue(ctx, sample);
    }
    JSValue plain = JS_NewArrayBufferCopy(ctx, NULL, 0);
    buffer_class = JS_GetClassID(plain);
    JSValue shared = JS_NewArrayBuffer(ctx, NULL, 0, NULL, NULL, 1);
    shared_class = JS_GetClassID(shared);
    JS_FreeValue(ctx, shared);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue realm = JS_NewObjectClass(ctx, (int)realm_class);
    JSValue dataview = JS_GetPropertyStr(ctx, global, "DataView");
    JSValue dv = JS_CallConstructor(ctx, dataview, 1, (JSValueConst *)&plain);
    view_class[V_DATAVIEW] = JS_GetClassID(dv);
    JS_FreeValue(ctx, dv);
    JSValue dv_proto = JS_GetPropertyStr(ctx, dataview, "prototype");
    static const char *const dv_names[3] = {"buffer", "byteOffset", "byteLength"};
    static const char *const dv_keys[3] = {"dvBuffer", "dvOffset", "dvLength"};
    for (int i = 0; i < 3; i++) {
        JSPropertyDescriptor desc;
        JSAtom atom = JS_NewAtom(ctx, dv_names[i]);
        if (JS_GetOwnProperty(ctx, &desc, dv_proto, atom) > 0) {
            JS_SetPropertyStr(ctx, realm, dv_keys[i], desc.getter);
            JS_FreeValue(ctx, desc.setter);
            JS_FreeValue(ctx, desc.value);
        }
        JS_FreeAtom(ctx, atom);
    }
    JS_FreeValue(ctx, dv_proto);
    JS_FreeValue(ctx, dataview);
    JS_FreeValue(ctx, plain);
    JS_SetClassProto(ctx, realm_class, realm);

    /* Context interfaces. WebGL2RenderingContext does not inherit from
       WebGLRenderingContext; both carry the shared base members. */
    JSValue proto1 = interface(ctx, global, "WebGLRenderingContext", gl1_class, true);
    JSValue proto2 = interface(ctx, global, "WebGL2RenderingContext", gl2_class, true);
    for (size_t i = 0; i < sizeof methods / sizeof *methods; i++) {
        const struct method *m = &methods[i];
        JSValue fn1 = JS_NewCFunctionMagic(ctx, trampoline, m->name, m->length, JS_CFUNC_generic_magic, (int)i);
        if (m->version == 1) JS_DefinePropertyValueStr(ctx, proto1, m->name, JS_DupValue(ctx, fn1), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, proto2, m->name, fn1, JS_PROP_C_W_E);
    }
    JS_SetPropertyFunctionList(ctx, proto1, context_members, sizeof context_members / sizeof *context_members);
    JS_SetPropertyFunctionList(ctx, proto2, context_members, sizeof context_members / sizeof *context_members);
    size_t n1 = sizeof webgl1_constants / sizeof *webgl1_constants, n2 = sizeof webgl2_constants / sizeof *webgl2_constants;
    JSValue ctor1 = JS_GetPropertyStr(ctx, global, "WebGLRenderingContext");
    JSValue ctor2 = JS_GetPropertyStr(ctx, global, "WebGL2RenderingContext");
    define_constants(ctx, proto1, webgl1_constants, n1);
    define_constants(ctx, ctor1, webgl1_constants, n1);
    define_constants(ctx, proto2, webgl1_constants, n1);
    define_constants(ctx, ctor2, webgl1_constants, n1);
    define_constants(ctx, proto2, webgl2_constants, n2);
    define_constants(ctx, ctor2, webgl2_constants, n2);
    JS_FreeValue(ctx, ctor1);
    JS_FreeValue(ctx, ctor2);
    JS_FreeValue(ctx, proto1);
    JS_FreeValue(ctx, proto2);

    for (int i = 0; i < O_KINDS; i++) {
        bool exposed = i != O_VAO_OES;
        JSValue proto = interface(ctx, global, kind_names[i], kind_class[i], exposed);
        if (i == O_INFO) JS_SetPropertyFunctionList(ctx, proto, info_members, 3);
        if (i == O_PRECISION) JS_SetPropertyFunctionList(ctx, proto, precision_members, 3);
        JS_FreeValue(ctx, proto);
    }
    for (int i = 0; i < X_COUNT; i++) {
        JSValue proto = interface(ctx, global, extensions[i].interface, ext_class[i], false);
        if (extension_members[i].list)
            JS_SetPropertyFunctionList(ctx, proto, extension_members[i].list, extension_members[i].count);
        JS_FreeValue(ctx, proto);
    }
    JS_FreeValue(ctx, global);

    static const JSCFunctionListEntry functions[] = {
        JS_CFUNC_DEF("webglBind", 1, host_bind), JS_CFUNC_DEF("webglCreate", 5, host_create),
        JS_CFUNC_DEF("webglResize", 3, host_resize), JS_CFUNC_DEF("webglSerial", 1, host_serial),
        JS_CFUNC_DEF("webglPresent", 4, host_present),
    };
    JS_SetPropertyFunctionList(ctx, host, functions, sizeof functions / sizeof *functions);
}
