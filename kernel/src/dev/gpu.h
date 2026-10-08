#pragma once
#include "kernel.h"
#include "gpu_abi.h"
/* Runs after PCI enumeration, before users start. Never replaces the boot display. */
void gpu_init(void);
void gpu_get_info(struct n_gpu_info *info);
/* Request is a kernel copy; output is validated by the syscall owner. */
int gpu_render(const struct n_gpu_render *request, uint32_t *out, size_t bytes);
int gpu_render_batch(const struct n_gpu_batch *request, uint32_t *out, size_t bytes);
/* Packed ARGB replace/nearest. Whole source is snapshotted into private DMA
 * before submit/output; the syscall owner validates both user spans. */
int gpu_blit(const struct n_gpu_blit *request, const uint32_t *source, size_t source_bytes,
             uint32_t *out, size_t bytes);
