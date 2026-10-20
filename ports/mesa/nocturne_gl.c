/* Nocturne's OpenGL ES frontend: Mesa's state tracker on Zink over the Vulkan
 * driver linked into the program (NVK) when it finds a GPU, otherwise on the
 * softpipe rasterizer, with drawing buffers kept in ordinary heap memory. One
 * screen serves every context of the process; each context owns a drawable
 * whose color and depth/stencil textures it allocates on demand. */
#include "nocturne_gl.h"

#include "frontend/api.h"
#include "frontend/sw_winsys.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "softpipe/sp_public.h"
#ifdef GALLIUM_ZINK
#include "zink/zink_public.h"

/* Zink opens "vulkan" through util/u_dl.c, which only refers weakly to the Vulkan driver's entry
 * point; this reference is what links the driver (NVK) into a program that draws with OpenGL. */
extern void *vk_icdGetInstanceProcAddr(void *instance, const char *name);
__attribute__((used)) static void *(*const vulkan_driver)(void *, const char *) = vk_icdGetInstanceProcAddr;
#endif
#include "state_tracker/st_context.h"
#include "util/format/u_format.h"
#include "util/u_atomic.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/simple_mtx.h"

#include "util/os_file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nocturne.h"

/* ---- the Mesa util/os_file.c services the GL state tracker and driconf use ---- */

/* A whole file in a buffer the caller frees, followed by a NUL (driconf reads the program file
   with it to hash it). */
char *os_read_file(const char *filename, size_t *size) {
    void *data = NULL;
    size_t length = 0;
    if (n_document_load(filename, &data, &length) < 0) return NULL;
    char *copy = length < SIZE_MAX ? malloc(length + 1) : NULL;
    if (copy) {
        if (length) memcpy(copy, data, length);
        copy[length] = 0;
        if (size) *size = length;
    }
    if (data) n_memory_release(data, length);
    return copy;
}

/* Shader capture (MESA_SHADER_CAPTURE_PATH) writes each program to a new
   file and never replaces an existing one. Plain C stdio: a name that
   already opens is taken. */
FILE *os_file_create_unique(const char *filename, int filemode) {
    (void)filemode;
    FILE *existing = fopen(filename, "rb");
    if (existing) {
        fclose(existing);
        return NULL;
    }
    return fopen(filename, "w");
}

/* ---- memory winsys: display targets are plain heap allocations ---- */

struct mem_displaytarget {
    void *data;
    unsigned stride;
};

static bool mem_is_displaytarget_format_supported(struct sw_winsys *ws, unsigned usage, enum pipe_format format) {
    (void)ws; (void)usage;
    return format == PIPE_FORMAT_R8G8B8A8_UNORM || format == PIPE_FORMAT_R8G8B8X8_UNORM ||
           format == PIPE_FORMAT_B8G8R8A8_UNORM || format == PIPE_FORMAT_B8G8R8X8_UNORM;
}

static struct sw_displaytarget *mem_displaytarget_create(struct sw_winsys *ws, unsigned usage,
                                                         enum pipe_format format, unsigned width,
                                                         unsigned height, unsigned alignment,
                                                         const void *front_private, unsigned *stride) {
    (void)ws; (void)usage; (void)front_private;
    struct mem_displaytarget *dt = CALLOC_STRUCT(mem_displaytarget);
    if (!dt) return NULL;
    if (!alignment) alignment = 1;
    uint64_t row = (uint64_t)util_format_get_stride(format, width);
    row = (row + alignment - 1) / alignment * alignment;
    uint64_t bytes = row * util_format_get_nblocksy(format, height);
    if (!row || row > UINT32_MAX || bytes > SIZE_MAX || !(dt->data = calloc(1, (size_t)bytes))) {
        FREE(dt);
        return NULL;
    }
    dt->stride = (unsigned)row;
    *stride = dt->stride;
    return (struct sw_displaytarget *)dt;
}

static void *mem_displaytarget_map(struct sw_winsys *ws, struct sw_displaytarget *dt, unsigned flags) {
    (void)ws; (void)flags;
    return ((struct mem_displaytarget *)dt)->data;
}

static void mem_displaytarget_unmap(struct sw_winsys *ws, struct sw_displaytarget *dt) { (void)ws; (void)dt; }

static void mem_displaytarget_display(struct sw_winsys *ws, struct sw_displaytarget *dt, void *context_private,
                                      unsigned nboxes, struct pipe_box *box) {
    /* Presentation is the caller's copy out of the drawing buffer. */
    (void)ws; (void)dt; (void)context_private; (void)nboxes; (void)box;
}

static void mem_displaytarget_destroy(struct sw_winsys *ws, struct sw_displaytarget *dt) {
    (void)ws;
    struct mem_displaytarget *m = (struct mem_displaytarget *)dt;
    free(m->data);
    FREE(m);
}

static void mem_destroy(struct sw_winsys *ws) { (void)ws; }

static struct sw_winsys mem_winsys = {
    .destroy = mem_destroy,
    .is_displaytarget_format_supported = mem_is_displaytarget_format_supported,
    .displaytarget_create = mem_displaytarget_create,
    .displaytarget_map = mem_displaytarget_map,
    .displaytarget_unmap = mem_displaytarget_unmap,
    .displaytarget_display = mem_displaytarget_display,
    .displaytarget_destroy = mem_displaytarget_destroy,
};

/* ---- the process-wide screen ---- */

static simple_mtx_t screen_lock = SIMPLE_MTX_INITIALIZER;
static struct pipe_frontend_screen *fscreen;
static bool screen_on_gpu; /* Zink: the drawing buffers live in GPU memory */

static struct pipe_frontend_screen *get_screen(void) {
    simple_mtx_lock(&screen_lock);
    if (!fscreen) {
        struct pipe_screen *screen = NULL;
#ifdef GALLIUM_ZINK
        /* Fails (with one log line) when the Vulkan driver finds no GPU. */
        screen = zink_create_screen(&mem_winsys, NULL);
        screen_on_gpu = screen != NULL;
#endif
        if (!screen) screen = softpipe_create_screen(&mem_winsys);
        struct pipe_frontend_screen *fs = screen ? CALLOC_STRUCT(pipe_frontend_screen) : NULL;
        if (fs) {
            fs->screen = screen;
            fscreen = fs;
        } else if (screen) {
            screen->destroy(screen);
        }
    }
    simple_mtx_unlock(&screen_lock);
    return fscreen;
}

/* ---- contexts and their drawables ---- */

struct ngl_context {
    struct pipe_frontend_drawable base; /* first: st hands this pointer back */
    struct st_visual visual;
    struct ngl_attribs attribs;
    struct st_context *st;
    struct pipe_resource *textures[ST_ATTACHMENT_COUNT];
    unsigned width, height;           /* requested drawing buffer size */
    unsigned tex_width, tex_height;   /* size the textures were made at */
    uint64_t dirty;
};

static uint32_t drawable_ids;
/* The calling thread's current context, in a native thread-local slot (no ELF
   TLS, so tcc -run programs can link this). */
static struct ngl_context *current_get(void) { return thread_local_get(N_TLS_GL_CONTEXT); }
static void current_set(struct ngl_context *c) { thread_local_set(N_TLS_GL_CONTEXT, c); }

static void drop_textures(struct ngl_context *c) {
    for (unsigned i = 0; i < ST_ATTACHMENT_COUNT; i++) pipe_resource_reference(&c->textures[i], NULL);
}

static void init_texture(struct pipe_context *pipe, struct pipe_resource *texture, uint32_t value);

static bool drawable_validate(struct st_context *st, struct pipe_frontend_drawable *drawable,
                              const enum st_attachment_type *statts, unsigned count,
                              struct pipe_resource **out, struct pipe_resource **resolve) {
    (void)resolve;
    struct ngl_context *c = (struct ngl_context *)drawable;
    struct pipe_screen *screen = drawable->fscreen->screen;
    if (c->tex_width != c->width || c->tex_height != c->height) {
        drop_textures(c);
        c->tex_width = c->width;
        c->tex_height = c->height;
    }
    for (unsigned i = 0; i < count; i++) {
        enum st_attachment_type a = statts[i];
        if (!c->textures[a]) {
            struct pipe_resource templ;
            memset(&templ, 0, sizeof templ);
            templ.target = PIPE_TEXTURE_2D;
            templ.width0 = c->width ? c->width : 1;
            templ.height0 = c->height ? c->height : 1;
            templ.depth0 = 1;
            templ.array_size = 1;
            if (a == ST_ATTACHMENT_DEPTH_STENCIL) {
                templ.format = c->visual.depth_stencil_format;
                templ.bind = PIPE_BIND_DEPTH_STENCIL;
            } else {
                templ.format = c->visual.color_format;
                templ.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW;
            }
            if (templ.format == PIPE_FORMAT_NONE) return false;
            c->textures[a] = screen->resource_create(screen, &templ);
            if (!c->textures[a]) return false;
            /* A new buffer is defined before anything can see it: the driver gives no content, and
               softpipe's zeros are wrong for depth. Color (0,0,0,0), depth 1, stencil 0. */
            init_texture(st->pipe, c->textures[a], a == ST_ATTACHMENT_DEPTH_STENCIL ? 0x00ffffff : 0);
        }
        pipe_resource_reference(&out[i], c->textures[a]);
    }
    return true;
}

static bool drawable_flush_front(struct st_context *st, struct pipe_frontend_drawable *drawable,
                                 enum st_attachment_type statt) {
    (void)st; (void)drawable; (void)statt;
    return true;
}

static void set_error(char *error, size_t size, const char *message) {
    if (error && size) snprintf(error, size, "%s", message);
}

struct ngl_context *ngl_context_create(int es_major, const struct ngl_attribs *attribs, unsigned width,
                                       unsigned height, char *error, size_t error_size) {
    if (es_major != 2 && es_major != 3) {
        set_error(error, error_size, "unsupported OpenGL ES version");
        return NULL;
    }
    struct pipe_frontend_screen *fs = get_screen();
    if (!fs) {
        set_error(error, error_size, "no OpenGL ES screen could be created");
        return NULL;
    }
    struct ngl_context *c = CALLOC_STRUCT(ngl_context);
    if (!c) {
        set_error(error, error_size, "out of memory");
        return NULL;
    }
    c->attribs = *attribs;
    c->attribs.antialias = false; /* a hint; the drawing buffer is single-sampled */
    c->visual.color_format = attribs->alpha ? PIPE_FORMAT_R8G8B8A8_UNORM : PIPE_FORMAT_R8G8B8X8_UNORM;
    if (attribs->stencil) {
        c->visual.depth_stencil_format = PIPE_FORMAT_Z24_UNORM_S8_UINT;
        c->attribs.depth = c->attribs.stencil = true;
    } else if (attribs->depth) {
        c->visual.depth_stencil_format = PIPE_FORMAT_Z24X8_UNORM;
    } else {
        c->visual.depth_stencil_format = PIPE_FORMAT_NONE;
    }
    c->visual.accum_format = PIPE_FORMAT_NONE;
    c->visual.buffer_mask = ST_ATTACHMENT_BACK_LEFT_MASK;
    if (c->visual.depth_stencil_format != PIPE_FORMAT_NONE)
        c->visual.buffer_mask |= ST_ATTACHMENT_DEPTH_STENCIL_MASK;
    c->width = width;
    c->height = height;

    c->base.visual = &c->visual;
    c->base.fscreen = fs;
    c->base.validate = drawable_validate;
    c->base.flush_front = drawable_flush_front;
    c->base.ID = p_atomic_inc_return(&drawable_ids);
    p_atomic_set(&c->base.stamp, 1);

    struct st_context_attribs st_attribs;
    memset(&st_attribs, 0, sizeof st_attribs);
    st_attribs.profile = API_OPENGLES2;
    st_attribs.major = es_major;
    st_attribs.minor = 0;
    st_attribs.visual = c->visual;
    enum st_context_error result = ST_CONTEXT_SUCCESS;
    c->st = st_api_create_context(fs, &st_attribs, &result, NULL);
    if (!c->st) {
        set_error(error, error_size,
                  result == ST_CONTEXT_ERROR_BAD_VERSION ? "OpenGL ES version not available" : "out of memory");
        FREE(c);
        return NULL;
    }
    c->st->frontend_context = c;
    return c;
}

void ngl_context_destroy(struct ngl_context *c) {
    if (!c) return;
    struct ngl_context *previous = current_get();
    if (previous != c) ngl_make_current(c);
    st_context_flush(c->st, 0, NULL, NULL, NULL);
    st_api_make_current(NULL, NULL, NULL);
    current_set(NULL);
    st_destroy_context(c->st);
    st_api_destroy_drawable(&c->base);
    drop_textures(c);
    FREE(c);
    if (previous && previous != c) ngl_make_current(previous);
}

bool ngl_make_current(struct ngl_context *c) {
    if (c == current_get()) return true;
    if (!c) {
        st_api_make_current(NULL, NULL, NULL);
        current_set(NULL);
        return true;
    }
    if (!st_api_make_current(c->st, &c->base, &c->base)) return false;
    current_set(c);
    return true;
}

struct ngl_context *ngl_get_current(void) { return current_get(); }

void ngl_get_attribs(const struct ngl_context *c, struct ngl_attribs *out) { *out = c->attribs; }

bool ngl_resize(struct ngl_context *c, unsigned width, unsigned height) {
    struct pipe_screen *screen = c->base.fscreen->screen;
    unsigned limit = screen->caps.max_texture_2d_size;
    if (width > limit || height > limit) return false;
    c->width = width;
    c->height = height;
    /* The textures are rebuilt, and initialized (init_texture), at the next validation. */
    drop_textures(c);
    c->tex_width = c->tex_height = 0;
    p_atomic_inc(&c->base.stamp);
    if (c == current_get()) st_invalidate_buffers(c->st);
    c->dirty++;
    return true;
}

void ngl_get_size(const struct ngl_context *c, unsigned *width, unsigned *height) {
    *width = c->width;
    *height = c->height;
}

bool ngl_read_drawing_buffer(struct ngl_context *c, uint32_t *dst, size_t pitch, bool premultiplied) {
    if (!c->width || !c->height) return true;
    struct pipe_resource *color = c->textures[ST_ATTACHMENT_BACK_LEFT];
    if (!color || c->tex_width != c->width || c->tex_height != c->height) {
        /* Nothing was drawn since the buffer was (re)made: it is transparent black. */
        for (unsigned y = 0; y < c->height; y++) memset(dst + y * pitch, 0, c->width * sizeof *dst);
        return true;
    }
    struct pipe_context *pipe = c->st->pipe;
    st_context_flush(c->st, ST_FLUSH_WAIT, NULL, NULL, NULL);
    struct pipe_transfer *transfer;
    const uint8_t *map = pipe_texture_map(pipe, color, 0, 0, PIPE_MAP_READ, 0, 0, c->width, c->height, &transfer);
    if (!map) return false;
    bool opaque = c->visual.color_format == PIPE_FORMAT_R8G8B8X8_UNORM;
    for (unsigned y = 0; y < c->height; y++) {
        const uint8_t *s = map + (size_t)y * transfer->stride;
        uint32_t *d = dst + y * pitch;
        for (unsigned x = 0; x < c->width; x++, s += 4) {
            unsigned r = s[0], g = s[1], b = s[2], a = opaque ? 255 : s[3];
            if (premultiplied && a != 255) {
                if (!a) {
                    r = g = b = 0;
                } else {
                    r = r >= a ? 255 : (r * 255 + a / 2) / a;
                    g = g >= a ? 255 : (g * 255 + a / 2) / a;
                    b = b >= a ? 255 : (b * 255 + a / 2) / a;
                }
            }
            d[x] = (uint32_t)a << 24 | r << 16 | g << 8 | b;
        }
    }
    pipe->texture_unmap(pipe, transfer);
    return true;
}

static void fill_texture(struct pipe_context *pipe, struct pipe_resource *texture, uint32_t value) {
    struct pipe_transfer *transfer;
    uint8_t *map = pipe_texture_map(pipe, texture, 0, 0, PIPE_MAP_WRITE | PIPE_MAP_DISCARD_WHOLE_RESOURCE, 0, 0,
                                    texture->width0, texture->height0, &transfer);
    if (!map) return;
    for (unsigned y = 0; y < texture->height0; y++) {
        uint32_t *row = (uint32_t *)(map + (size_t)y * transfer->stride);
        if (!value) {
            memset(row, 0, texture->width0 * sizeof *row);
        } else {
            for (unsigned x = 0; x < texture->width0; x++) row[x] = value;
        }
    }
    pipe->texture_unmap(pipe, transfer);
}

/* On the GPU the clear is queued behind the rendering: no wait and no trip through the CPU. */
static void clear_texture(struct pipe_context *pipe, struct pipe_resource *texture, uint32_t value) {
    struct pipe_box box;
    u_box_2d(0, 0, texture->width0, texture->height0, &box);
    pipe->clear_texture(pipe, texture, 0, &box, &value);
}

/* The initial content of a drawing buffer made by drawable_validate: queued on the GPU, written
   directly by the CPU for softpipe. Nothing is pending on a resource that was just created, so
   neither waits. */
static void init_texture(struct pipe_context *pipe, struct pipe_resource *texture, uint32_t value) {
    if (screen_on_gpu) clear_texture(pipe, texture, value);
    else fill_texture(pipe, texture, value);
}

void ngl_clear_drawing_buffer(struct ngl_context *c) {
    struct pipe_context *pipe = c->st->pipe;
    struct pipe_resource *color = c->textures[ST_ATTACHMENT_BACK_LEFT];
    struct pipe_resource *depth_stencil = c->textures[ST_ATTACHMENT_DEPTH_STENCIL];
    /* The values are in the textures' own formats. Z24 in the low bits: depth 1.0 is 0xffffff,
       stencil 0 above it. */
    void (*fill)(struct pipe_context *, struct pipe_resource *, uint32_t) = clear_texture;
    if (!screen_on_gpu) {
        st_context_flush(c->st, ST_FLUSH_WAIT, NULL, NULL, NULL);
        fill = fill_texture;
    }
    if (color) fill(pipe, color, 0);
    if (depth_stencil) fill(pipe, depth_stencil, 0x00ffffff);
}

void ngl_mark_dirty(struct ngl_context *c) { c->dirty++; }

uint64_t ngl_dirty_serial(const struct ngl_context *c) { return c->dirty; }
