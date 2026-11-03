/* The GPU's driver image of a process (common/gpu_abi.h; user/libc/vulkan.c). */
#pragma once
#include "gpu_abi.h"
#include "nocturne.h"

/* The driver the system's GPU service names, mapped when first asked for. A process has one.
   -N_ERR_NOT_FOUND where no service names one; it may come later, so a caller asks again. */
n_result n_gpu_driver_get(const struct n_gpu_driver **out);
/* The same from this image, whatever the services say. */
n_result n_gpu_driver_load(const char *path, const struct n_gpu_driver **out);
