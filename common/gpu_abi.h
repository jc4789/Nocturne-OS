/* Nocturne's bounded native GPU interface. This is not an OpenGL/WebGL ABI. */
#pragma once
#include <stdint.h>

#define N_GPU_MAX_SIDE 512u
#define N_GPU_MAX_TRIANGLES 32u
#define N_GPU_CLEAR 1u
#define N_GPU_TRIANGLE 2u
#define N_GPU_CAP_CLEAR 1u
#define N_GPU_CAP_TRIANGLE 2u
#define N_GPU_CAP_BATCH 4u
#define N_GPU_CAP_BLIT 8u
#define N_GPU_BACKEND_NONE 0u
#define N_GPU_BACKEND_VIRGL 1u
#define N_GPU_BACKEND_CPU 2u /* library fallback only, never advertised as a GPU */

struct n_gpu_vertex {
    int32_t x, y, z, w; /* homogeneous clip coordinates, signed 16.16 */
    uint32_t argb;
};
struct n_gpu_render {
    uint32_t width, height, operation, clear_argb;
    struct n_gpu_vertex vertex[3];
};
/* One independent frame: clear once, then ordered triangle-list primitives.
 * No depth test or alpha blending (same semantics as N_GPU_TRIANGLE).
 * Fixed 1936-byte input snapshot; unused vertices are ignored. Zero triangles
 * means clear only. The device reuses its target, shaders and vertex buffer,
 * uploading once and reading back once under one fenced submission. */
struct n_gpu_batch {
    uint32_t width, height, clear_argb, triangle_count;
    struct n_gpu_vertex vertex[N_GPU_MAX_TRIANGLES * 3];
};
/* Packed ARGB replacement, integer crop and nearest-neighbour scaling.
 * No compositing, rotation, author shaders, or WebGL capability is implied.
 * Source and output are bounded to 512 squared. The complete source is copied
 * into private device storage before submission or any output write, so alias
 * is supported. Exactly eight uint32_t fields; user pointers are syscall args. */
struct n_gpu_blit {
    uint32_t source_width, source_height;
    uint32_t source_x, source_y, source_w, source_h;
    uint32_t width, height;
};
struct n_gpu_info {
    uint32_t backend, capabilities, max_width, max_height;
    uint64_t submissions, failures;
    char name[48];
};
