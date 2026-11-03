/* A program's Vulkan loader. The driver is not linked into the program: it is the image the GPU
   service names (client_image of struct n_service_status), mapped when first asked for, one per
   process. The loader's commands are those a program links by name; the others it gets from
   vkGetInstanceProcAddr, which are the driver's own, with nothing in between. Without a driver the
   commands say so as Vulkan says it. */
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <vulkan/vulkan_core.h>
#include "nocturne_gpu.h"
#include "nocturne_services.h"

const struct n_start *__libc_start_info(void);

/* The interface version of Vulkan's loader and driver this loader speaks. */
#define LOADER_ICD_VERSION 5u

static mtx_t driver_lock = { 0, mtx_plain, 0, 0 };
static const struct n_gpu_driver *driver;
static n_result driver_failure; /* a start that failed is not repeated: it left its copy mapped */

n_result n_gpu_driver_load(const char *path, const struct n_gpu_driver **out) {
    mtx_lock(&driver_lock);
    n_result r = driver_failure;
    if (!driver && r >= 0) {
        n_handle document = n_document_open(path, N_DOCUMENT_READ);
        struct n_image image;
        r = document < 0 ? document : n_image_map(document, &image);
        if (document >= 0) n_handle_close(document);
        if (r >= 0) {
            const struct n_gpu_driver *(*start)(const struct n_start *) = (const struct n_gpu_driver *(*)(const struct n_start *))image.entry;
            const struct n_gpu_driver *started = start(__libc_start_info());
            uint32_t version = LOADER_ICD_VERSION;
            if (!started || started->abi != N_GPU_DRIVER_ABI) r = -N_ERR_WRONG_GENERATION;
            else if (started->negotiate(&version) != VK_SUCCESS) r = -N_ERR_UNSUPPORTED;
            else driver = started;
            driver_failure = r < 0 ? r : 0;
        }
    }
    *out = driver;
    mtx_unlock(&driver_lock);
    return r < 0 ? r : 0;
}

n_result n_gpu_driver_get(const struct n_gpu_driver **out) {
    mtx_lock(&driver_lock);
    *out = driver;
    mtx_unlock(&driver_lock);
    if (*out) return 0;
    struct n_service_status *all;
    n_result count = n_services_list(&all);
    if (count < 0) return count;
    n_result r = -N_ERR_NOT_FOUND;
    for (n_result i = 0; i < count && r == -N_ERR_NOT_FOUND; i++) {
        if (!all[i].client_image[0] || all[i].state == N_SERVICE_ABSENT) continue;
        char path[sizeof N_DRIVER_FOLDER + N_SERVICE_PROGRAM_MAX + 1];
        r = n_path_join(N_DRIVER_FOLDER, all[i].client_image, path, sizeof path);
        if (r >= 0) r = n_gpu_driver_load(path, out);
    }
    free(all);
    return r < 0 ? r : 0;
}

static PFN_vkVoidFunction driver_command(const char *name) {
    const struct n_gpu_driver *d;
    return n_gpu_driver_get(&d) < 0 ? NULL : d->get_instance_proc_addr(NULL, name);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator,
                                                VkInstance *instance) {
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)driver_command("vkCreateInstance");
    return create ? create(info, allocator, instance) : VK_ERROR_INCOMPATIBLE_DRIVER;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t *version) {
    PFN_vkEnumerateInstanceVersion enumerate = (PFN_vkEnumerateInstanceVersion)driver_command("vkEnumerateInstanceVersion");
    if (enumerate) return enumerate(version);
    *version = VK_API_VERSION_1_0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char *layer, uint32_t *count,
                                                                      VkExtensionProperties *properties) {
    if (layer) return VK_ERROR_LAYER_NOT_PRESENT;
    PFN_vkEnumerateInstanceExtensionProperties enumerate =
        (PFN_vkEnumerateInstanceExtensionProperties)driver_command("vkEnumerateInstanceExtensionProperties");
    if (enumerate) return enumerate(NULL, count, properties);
    *count = 0;
    return VK_SUCCESS;
}

/* Layers are a loader's, and this one loads none. */
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t *count, VkLayerProperties *properties) {
    (void)properties;
    *count = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *name) {
    const struct n_gpu_driver *d;
    return n_gpu_driver_get(&d) < 0 ? NULL : d->get_device_proc_addr(device, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *name) {
    if (!name) return NULL;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (!instance && !strcmp(name, "vkEnumerateInstanceLayerProperties")) return (PFN_vkVoidFunction)vkEnumerateInstanceLayerProperties;
    const struct n_gpu_driver *d;
    if (n_gpu_driver_get(&d) >= 0) return d->get_instance_proc_addr(instance, name);
    if (instance) return NULL;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)vkCreateInstance;
    if (!strcmp(name, "vkEnumerateInstanceVersion")) return (PFN_vkVoidFunction)vkEnumerateInstanceVersion;
    if (!strcmp(name, "vkEnumerateInstanceExtensionProperties")) return (PFN_vkVoidFunction)vkEnumerateInstanceExtensionProperties;
    return NULL;
}
