#pragma once
#include "gpu_abi.h"
#include "gfx.h"
#include "nocturne.h"
#include <stddef.h>
#include <stdbool.h>
/* Every call returns 0 (or the backend, below), or a negative n_result, and
 * leaves errno alone. */
n_result gpu_info(struct n_gpu_info *out);
/* Strict device path. No implicit fallback. */
n_result gpu_render(const struct n_gpu_render *request, uint32_t *pixels, size_t bytes);
n_result gpu_render_batch(const struct n_gpu_batch *request, uint32_t *pixels, size_t bytes);
n_result gpu_render_list(const struct n_gpu_scene *request,const struct n_gpu_vertex *vertices,uint32_t *pixels,size_t bytes);
n_result gfx_render3d_list(canvas_t *dst,const struct n_gpu_scene *request,const struct n_gpu_vertex *vertices);
/* Strict bounded texture crop/scale, ARGB replace (not source-over).
 * Packed source and output; alias is safe. No implicit CPU fallback. */
n_result gpu_blit(const struct n_gpu_blit *request, const uint32_t *source, size_t source_bytes,
                  uint32_t *pixels, size_t bytes);
/* RAM renderer with a real virgl path when available. Returns device/CPU backend. */
n_result gfx_render3d(canvas_t *dst, const struct n_gpu_render *request);
/* Complete scene in one upload/draw/readback. Full packed RAM destinations
 * avoid the temporary bitmap/blit; clipped destinations preserve their clip.
 * Device failure falls back to the same ordered CPU triangle list. */
n_result gfx_render3d_batch(canvas_t *dst, const struct n_gpu_batch *request);
