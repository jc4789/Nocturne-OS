#pragma once
#include "gpu_abi.h"
#include "gfx.h"
#include <stddef.h>
#include <stdbool.h>
int gpu_info(struct n_gpu_info *out);
/* Strict device path, -1/errno on failure. No implicit fallback. */
int gpu_render(const struct n_gpu_render *request, uint32_t *pixels, size_t bytes);
int gpu_render_batch(const struct n_gpu_batch *request, uint32_t *pixels, size_t bytes);
/* Strict bounded texture crop/scale, ARGB replace (not source-over).
 * Packed source and output; alias is safe. No implicit CPU fallback. */
int gpu_blit(const struct n_gpu_blit *request, const uint32_t *source, size_t source_bytes,
             uint32_t *pixels, size_t bytes);
/* RAM renderer with a real virgl path when available. Returns device/CPU backend. */
int gfx_render3d(canvas_t *dst, const struct n_gpu_render *request);
/* Complete scene in one upload/draw/readback. Full packed RAM destinations
 * avoid the temporary bitmap/blit; clipped destinations preserve their clip.
 * Device failure falls back to the same ordered CPU triangle list. */
int gfx_render3d_batch(canvas_t *dst, const struct n_gpu_batch *request);
