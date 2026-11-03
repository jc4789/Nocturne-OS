/* Nocturne's native GPU interface. This is not an OpenGL/WebGL ABI. */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* Source compatibility only: neither value bounds the scalable interface.
 * The old batch's inline array length must retain its binary layout. */
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
/* Variable triangle-list frame. Vertex storage is supplied separately and
 * snapshotted before execution. Zero triangles means a clear-only frame. */
struct n_gpu_scene {uint32_t width,height,clear_argb,triangle_count;};
static inline size_t n_gpu_frame_bytes(uint32_t w,uint32_t h) {
    if(!w || !h || w>INT32_MAX || h>INT32_MAX || (size_t)w>SIZE_MAX/sizeof(uint32_t)/h)return 0;
    return (size_t)w*h*sizeof(uint32_t);
}
/* Packed ARGB replacement, integer crop and nearest-neighbour scaling.
 * No compositing, rotation, author shaders, or WebGL capability is implied.
 * The complete source is copied
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

/* ---- GPU のドライバーの像 ----
   Vulkan のドライバーはプログラムに結合せず、N_DRIVER_FOLDER の中の像（common/abi.h の SYS_IMAGE_MAP）として、
   使うプロセスが自分の身元で読み込む。特権の半分は別プロセスのサービスに残る。サービスの宣言が像の名を言い
   （struct n_service_status の client_image）、C ライブラリーの Vulkan ローダーがそれを読み込む。
   像の入口は const struct n_gpu_driver *(*)(const struct n_start *) で、下の表を返す。 */
#define N_DRIVER_FOLDER "C:\\Nocturne\\Drivers"
#define N_GPU_DRIVER_ABI 1u
struct n_gpu_scanout;
struct n_gpu_driver {
    uint32_t abi; /* N_GPU_DRIVER_ABI */
    uint32_t reserved;
    /* Vulkan のローダーとドライバーの間の三つ */
    int32_t (*negotiate)(uint32_t *version);
    void (*(*get_instance_proc_addr)(void *instance, const char *name))(void);
    void (*(*get_device_proc_addr)(void *device, const char *name))(void);
    /* 今、描ける装置があるか。0 か -N_ERR_*（-N_ERR_NOT_FOUND はこのドライバーの装置が無い）。 */
    int64_t (*ready)(void);
    /* ドライバーの書く記録を預かる。release は預かった行を返す（無ければ NULL）。行は log_free で返す。 */
    void (*log_hold)(void);
    char *(*log_release)(void);
    void (*log_free)(char *lines);
    /* 画面へ直接出す。image の番号は Vulkan の import でドライバーへ戻る。
       present は 1：出した、0：今は出せない、負：失敗。 */
    struct n_gpu_scanout *(*scanout_open)(void);
    void (*scanout_close)(struct n_gpu_scanout *s);
    int64_t (*scanout_create)(struct n_gpu_scanout *s, uint32_t *image, uint32_t *width, uint32_t *height, uint32_t *pitch,
                              uint64_t *bytes);
    void (*scanout_destroy)(struct n_gpu_scanout *s, uint32_t image);
    int64_t (*scanout_present)(struct n_gpu_scanout *s, uint32_t image);
};
