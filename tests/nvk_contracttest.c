/* Nocturne実製品のVulkan公開契約とMesa描画の回帰試験。
 * GPU不在での成功はNVIDIA命令実行や高速化を証明しない。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GLES2/gl2.h>
#include <vulkan/vulkan.h>
#include "../ports/mesa/nocturne_gl.h"

extern void *vk_icdGetInstanceProcAddr(void *instance, const char *name);
static unsigned failures;
#define CHECK(x) do { if (!(x)) { printf("失敗 %d: %s\n", __LINE__, #x); failures++; } } while (0)

static void headless_contract(void) {
    PFN_vkEnumerateInstanceExtensionProperties enumerate = (void *)vk_icdGetInstanceProcAddr(NULL, "vkEnumerateInstanceExtensionProperties");
    PFN_vkCreateInstance create = (void *)vk_icdGetInstanceProcAddr(NULL, "vkCreateInstance");
    CHECK(enumerate && create);
    if (!enumerate || !create) return;
    uint32_t count = 0;
    CHECK(enumerate(NULL, &count, NULL) == VK_SUCCESS);
    VkExtensionProperties *extensions = calloc(count, sizeof(*extensions));
    CHECK(extensions != NULL);
    if (!extensions) return;
    CHECK(enumerate(NULL, &count, extensions) == VK_SUCCESS);
    bool surface = false, headless = false;
    for (uint32_t i = 0; i < count; i++) {
        surface |= strcmp(extensions[i].extensionName, VK_KHR_SURFACE_EXTENSION_NAME) == 0;
        headless |= strcmp(extensions[i].extensionName, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME) == 0;
    }
    free(extensions);
    CHECK(surface && headless);
    const char *names[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = sizeof(names) / sizeof(names[0]), .ppEnabledExtensionNames = names};
    VkInstance instance = VK_NULL_HANDLE;
    CHECK(create(&ci, NULL, &instance) == VK_SUCCESS);
    if (!instance) return;
    PFN_vkCreateHeadlessSurfaceEXT create_surface = (void *)vk_icdGetInstanceProcAddr(instance, "vkCreateHeadlessSurfaceEXT");
    PFN_vkDestroySurfaceKHR destroy_surface = (void *)vk_icdGetInstanceProcAddr(instance, "vkDestroySurfaceKHR");
    PFN_vkDestroyInstance destroy = (void *)vk_icdGetInstanceProcAddr(instance, "vkDestroyInstance");
    CHECK(create_surface && destroy_surface && destroy);
    if (create_surface && destroy_surface) {
        VkHeadlessSurfaceCreateInfoEXT sci = {.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
        VkSurfaceKHR s = VK_NULL_HANDLE;
        CHECK(create_surface(instance, &sci, NULL, &s) == VK_SUCCESS && s != VK_NULL_HANDLE);
        if (s) destroy_surface(instance, s, NULL);
    }
    if (destroy) destroy(instance, NULL);
}

static void context_contract(void) {
    const struct ngl_attribs attribs = {.alpha = true, .depth = true};
    char error[128];
    struct ngl_context *first = ngl_context_create(2, &attribs, 1, 1, error, sizeof(error));
    CHECK(first != NULL);
    if (!first) { printf("context: %s\n", error); return; }
    CHECK(ngl_make_current(first));
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    CHECK(glGetError() == GL_NO_ERROR);
    printf("描画backend: %s / caveat=%u\n", glGetString(GL_RENDERER), ngl_has_major_performance_caveat(first));
    struct ngl_context *second = ngl_context_create(2, &attribs, 1, 1, error, sizeof(error));
    CHECK(second != NULL);
    if (second) {
        CHECK(ngl_make_current(second));
        glClearColor(0, 1, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        uint32_t pixel = 0;
        CHECK(ngl_read_drawing_buffer(second, &pixel, 1, false) && pixel == 0xff00ff00);
        CHECK(ngl_make_current(first));
        CHECK(ngl_read_drawing_buffer(first, &pixel, 1, false) && pixel == 0xffff0000);
        ngl_context_destroy(second);
        CHECK(ngl_get_current() == first);
    }
    ngl_context_destroy(first);
}

int main(void) {
    headless_contract();
    context_contract();
    printf("NVK/Mesa native契約試験: %u件失敗\n", failures);
    return failures != 0;
}
