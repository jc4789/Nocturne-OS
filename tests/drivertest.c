/* The GPU's driver image (common/gpu_abi.h), mapped and started by the Vulkan loader of the C library:
   its own C library comes up in a process that runs, and Vulkan's instance starts on it. A program
   outside C:\Nocturne does this with the public calls. A machine without the GPU finds no device; that
   says nothing of drawing, which only that GPU can show. */
#include <nocturne.h>
#include <nocturne_gpu.h>
#include <stdio.h>
#include <string.h>
#include <threads.h>
#include <vulkan/vulkan_core.h>

static int failed;
static void check(const char *what, int ok) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failed++;
}

static PFN_vkEnumerateInstanceVersion enumerate_version;
static uint32_t version_seen;
/* A thread of this program's libc in the driver's: the image's libc makes its record then, and takes
   it down at the thread's end. */
static int in_the_driver(void *arg) {
    (void)arg;
    enumerate_version(&version_seen);
    return 0;
}

int main(void) {
    const struct n_gpu_driver *driver;
    n_result named = n_gpu_driver_get(&driver);
    printf("the driver the services name: %s\n", named < 0 ? n_result_text(named) : "started");
    if (named < 0) {
        VkInstance none = VK_NULL_HANDLE;
        VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        check("no driver: the loader's vkCreateInstance says so", vkCreateInstance(&info, NULL, &none) == VK_ERROR_INCOMPATIBLE_DRIVER);
    }
    n_result r = n_gpu_driver_load(N_DRIVER_FOLDER "\\nvk", &driver);
    printf("the image: %s\n", r < 0 ? n_result_text(r) : "started");
    check("the image starts", r >= 0 && driver);
    if (r < 0) return 1;
    int64_t ready = driver->ready();
    printf("ready: %s\n", ready < 0 ? n_result_text((n_result)ready) : "yes");
    check("the image reaches the kernel by its slots", ready >= 0 || ready == -N_ERR_NOT_FOUND || ready == -N_ERR_NOT_READY);
    PFN_vkGetInstanceProcAddr get = (PFN_vkGetInstanceProcAddr)vkGetInstanceProcAddr(NULL, "vkGetInstanceProcAddr");
    check("the loader answers for itself", get == vkGetInstanceProcAddr);
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)get(NULL, "vkCreateInstance");
    check("the loader gives the driver's vkCreateInstance", create && (void *)create != (void *)vkCreateInstance);
    VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &application};
    VkInstance instance = VK_NULL_HANDLE;
    VkResult made = create(&info, NULL, &instance);
    printf("vkCreateInstance: %d\n", (int)made);
    check("an instance starts", made == VK_SUCCESS && instance);
    if (made == VK_SUCCESS) {
        PFN_vkEnumeratePhysicalDevices devices = (PFN_vkEnumeratePhysicalDevices)get(instance, "vkEnumeratePhysicalDevices");
        uint32_t count = 0;
        VkResult listed = devices ? devices(instance, &count, NULL) : VK_ERROR_UNKNOWN;
        printf("devices: %u\n", (unsigned)count);
        check("the devices can be counted", listed == VK_SUCCESS);
        PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)get(instance, "vkDestroyInstance");
        check("the instance ends", destroy != NULL);
        if (destroy) destroy(instance, NULL);
    }
    enumerate_version = (PFN_vkEnumerateInstanceVersion)get(NULL, "vkEnumerateInstanceVersion");
    thrd_t thread;
    check("a thread goes into the driver", enumerate_version && thrd_create(&thread, in_the_driver, NULL) == thrd_success);
    thrd_join(thread, NULL);
    printf("version %u.%u\n", VK_API_VERSION_MAJOR(version_seen), VK_API_VERSION_MINOR(version_seen));
    check("and comes back with the version", version_seen >= VK_API_VERSION_1_0);
    printf("drivertest: %d failed\n", failed);
    return failed != 0;
}
