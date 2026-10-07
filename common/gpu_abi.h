/* Nocturne's bounded native GPU interface. This is not an OpenGL/WebGL ABI. */
#pragma once
#include <stdint.h>

#define N_GPU_MAX_SIDE 512u
#define N_GPU_CLEAR 1u
#define N_GPU_TRIANGLE 2u
#define N_GPU_CAP_CLEAR 1u
#define N_GPU_CAP_TRIANGLE 2u
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
struct n_gpu_info {
    uint32_t backend, capabilities, max_width, max_height;
    uint64_t submissions, failures;
    char name[48];
};
