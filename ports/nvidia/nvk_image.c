/* The NVK driver image, C:\Nocturne\Drivers\nvk: what a program's Vulkan loader and GL get from it
   (struct n_gpu_driver, common/gpu_abi.h). Vulkan's commands are NVK's. The rest speaks to nvrm
   through libnvrm, and turns the RM's status codes into the table's. */
#include <stdlib.h>
#include "gpu_abi.h"
#include "nocturne.h"
#include "nvrm.h"
#include "nvstatus.h"
#include "util/log.h"

extern int32_t vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *version);
extern void (*vk_icdGetInstanceProcAddr(void *instance, const char *name))(void);
extern void (*vk_common_GetDeviceProcAddr(void *device, const char *name))(void);

static int64_t ready(void) {
    struct nvrm *rm;
    int64_t why = nvrm_connect_ready_status(&rm);
    if (!rm) return why;
    nvrm_disconnect(rm);
    return 0;
}

static void log_free(char *lines) { free(lines); }

static struct n_gpu_scanout *scanout_open(void) { return (struct n_gpu_scanout *)nvrm_connect_ready(); }

static void scanout_close(struct n_gpu_scanout *s) { nvrm_disconnect((struct nvrm *)s); }

static int64_t scanout_create(struct n_gpu_scanout *s, uint32_t *image, uint32_t *width, uint32_t *height, uint32_t *pitch,
                              uint64_t *bytes) {
    return nvrm_scanout_create((struct nvrm *)s, image, width, height, pitch, bytes) == NV_OK ? 0 : -N_ERR_IO;
}

static void scanout_destroy(struct n_gpu_scanout *s, uint32_t image) { nvrm_scanout_destroy((struct nvrm *)s, image); }

static int64_t scanout_present(struct n_gpu_scanout *s, uint32_t image) {
    uint32_t status = nvrm_scanout_present((struct nvrm *)s, image);
    return status == NV_OK ? 1 : status == NV_ERR_INVALID_STATE ? 0 : -N_ERR_IO;
}

__attribute__((visibility("hidden"))) const struct n_gpu_driver __image_exports = {
    .abi = N_GPU_DRIVER_ABI,
    .negotiate = vk_icdNegotiateLoaderICDInterfaceVersion,
    .get_instance_proc_addr = vk_icdGetInstanceProcAddr,
    .get_device_proc_addr = vk_common_GetDeviceProcAddr,
    .ready = ready,
    .log_hold = mesa_log_hold,
    .log_release = mesa_log_release,
    .log_free = log_free,
    .scanout_open = scanout_open,
    .scanout_close = scanout_close,
    .scanout_create = scanout_create,
    .scanout_destroy = scanout_destroy,
    .scanout_present = scanout_present,
};
