#pragma once
/* Nocturne's OpenGL ES frontend on Mesa softpipe. A context owns its own
 * drawing buffer in RAM; the caller draws with the ordinary gl* entry points
 * while the context is current and copies the result out with
 * ngl_read_drawing_buffer. Every function runs on the calling thread. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ngl_attribs {
    bool alpha, depth, stencil, antialias;
};

struct ngl_context;

/* es_major is 2 (OpenGL ES 2.0, for WebGL 1) or 3 (OpenGL ES 3.0, for
 * WebGL 2). The drawing buffer starts at width x height. On failure NULL is
 * returned and a reason is written to error. */
struct ngl_context *ngl_context_create(int es_major, const struct ngl_attribs *attribs, unsigned width,
                                       unsigned height, char *error, size_t error_size);
void ngl_context_destroy(struct ngl_context *context);

/* Binds the context and its drawing buffer to this thread; NULL unbinds. */
bool ngl_make_current(struct ngl_context *context);
struct ngl_context *ngl_get_current(void);

/* The attributes actually provided (depth/stencil may differ from the
 * request when only one of them was asked for). */
void ngl_get_attribs(const struct ngl_context *context, struct ngl_attribs *out);
/* The selected backend's known caveat (currently CPU rasterization); not a
 * claim about measured GPU speed or a context's requested power preference. */
bool ngl_has_major_performance_caveat(const struct ngl_context *context);

/* Reallocates the drawing buffer; its contents become transparent black. */
bool ngl_resize(struct ngl_context *context, unsigned width, unsigned height);
void ngl_get_size(const struct ngl_context *context, unsigned *width, unsigned *height);

/* Finishes queued rendering and copies the color buffer top-down into
 * width x height pixels of 0xAARRGGBB at dst (pitch in pixels). With
 * premultiplied set the stored color is premultiplied and is divided out;
 * without alpha the result is opaque. */
bool ngl_read_drawing_buffer(struct ngl_context *context, uint32_t *dst, size_t pitch, bool premultiplied);

/* Clears color, depth and stencil of the drawing buffer to zero without
 * touching GL state (used after presenting when the buffer is not preserved). */
void ngl_clear_drawing_buffer(struct ngl_context *context);

/* A monotonically increasing count bumped whenever rendering may have changed
 * the drawing buffer (draw, clear, blit into the default framebuffer). The
 * binding records changes; this is the shared place to keep the count. */
void ngl_mark_dirty(struct ngl_context *context);
uint64_t ngl_dirty_serial(const struct ngl_context *context);

/* Direct scanout: the drawing buffer copied on the GPU into images of the
 * screen (nvrm's NVRM_SCANOUT) the display shows instead of the desktop while
 * the system shows the app's fullscreen window directly (EV_DIRECT). NULL from
 * create when the context does not draw on a GPU whose driver shows the
 * screen. */
struct ngl_scanout;
struct ngl_scanout *ngl_scanout_create(struct ngl_context *context);
void ngl_scanout_destroy(struct ngl_scanout *scanout);
void ngl_scanout_size(const struct ngl_scanout *scanout, unsigned *width, unsigned *height);
/* Copies the drawing buffer into the next image, scaled to x, y, width x
 * height of the screen and black around it, and has it shown from the next
 * vertical blank. Waits for the rendering and while an image presented before
 * still waits for its blank. 1 shown; 0 not shown because the system does not
 * show the window directly now; -1 failed. Unless 1, the caller presents the
 * frame through its window (ngl_read_drawing_buffer). */
int ngl_scanout_present(struct ngl_scanout *scanout, struct ngl_context *context, int x, int y, unsigned width,
                        unsigned height);

#ifdef __cplusplus
}
#endif
