#pragma once
#include "gpu_abi.h"
#include "gfx.h"
#include <stddef.h>
#include <stdbool.h>
int gpu_info(struct n_gpu_info *out);
/* Strict device path, -1/errno on failure. No implicit fallback. */
int gpu_render(const struct n_gpu_render *request, uint32_t *pixels, size_t bytes);
/* RAM renderer with a real virgl path when available. Returns device/CPU backend. */
int gfx_render3d(canvas_t *dst, const struct n_gpu_render *request);
