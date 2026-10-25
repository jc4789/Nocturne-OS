/* Zinkの起動の初めと、起動できない理由の記録を、製品のコードのまま検査する：
   u_dl.c（NocturneでVulkanのローダーを務める部分）、生成されたzink_instance.c（インスタンスの作成）、
   log.c（Nocturne GLがZinkの理由を預かる仕組み）、vk_log.c（預かり中はNVKのvk_error*()も記録へ出す）。
   ICDは偽物だが、大域コマンドはNVKと同じにしてある：vkEnumerateInstanceVersion、
   vkEnumerateInstanceExtensionProperties、vkCreateInstanceは有り、vkEnumerateInstanceLayerPropertiesは無い
   （NVKにnvk_EnumerateInstanceLayerPropertiesは無く、生成された入口の表では弱参照のNULLになる）。
   実機8回目では、この欠けのためにZinkが理由を一行も書かずにインスタンスの作成をやめていた。 */
#include <stdarg.h>
#include <stdio.h>
#include <nocturne.h>
#include "util/log.h"
#include "util/u_debug.h"
#include "util/u_dl.h"
#include "vk_log.h"
#include "zink_instance.h"
#include "zink_screen.h"

static unsigned failures;
#define CHECK(x) do { if (!(x)) { printf("失敗 %d: %s\n", __LINE__, #x); failures++; } } while (0)

/* ---- Mesaの記録の出口：stderr（FILEの記録）とシステムの記録 ---- */
static char file_log[2048], system_log[2048];
static unsigned system_sends;
FILE *stderr;
static void append(char *log, size_t size, const char *text) {
    size_t used = strlen(log);
    snprintf(log + used, size - used, "%s", text);
}
int fprintf(FILE *f, const char *format, ...) {
    CHECK(f == stderr);
    char line[1024];
    va_list va; va_start(va, format); int n = vsnprintf(line, sizeof line, format, va); va_end(va);
    append(file_log, sizeof file_log, line);
    return n;
}
int fputs(const char *s, FILE *f) { CHECK(f == stderr); append(file_log, sizeof file_log, s); return 0; }
int fflush(FILE *f) { (void)f; return 0; }
n_handle n_system_log_open(void) { return 7; }
n_result n_channel_send(n_handle h, const void *data, size_t n, n_handle attach) {
    CHECK(h == 7 && attach == -1 && strlen(data) == n);
    append(system_log, sizeof system_log, data);
    system_sends++;
    return 0;
}
static void clear_logs(void) { file_log[0] = system_log[0] = 0; system_sends = 0; }

/* ---- log.cの読む設定：何も設定されていない ---- */
const char *os_get_option(const char *name) { (void)name; return NULL; }
uint64_t parse_debug_string(const char *debug, const struct debug_control *control) {
    (void)debug; (void)control; return 0;
}
uint64_t debug_get_flags_option(const char *name, const struct debug_named_value *flags, uint64_t dfault) {
    (void)name; (void)flags; return dfault;
}
void call_once(once_flag *flag, void (*func)(void)) {
    if (!flag->state) { flag->state = 1; func(); }
}
const char *util_get_process_name(void) { return "zink-start-hosttest"; }
/* u_printf.cはホストのMinGWでは翻訳できない：その非Windows版と同じく、vsnprintfに長さを問う */
size_t u_printf_length(const char *fmt, va_list untouched_args) {
    va_list args; va_copy(args, untouched_args);
    char junk;
    int size = vsnprintf(&junk, 1, fmt, args);
    va_end(args);
    return size < 0 ? 0 : (size_t)size;
}
uint32_t zink_debug; /* ZINK_DEBUGは設定されていない（zink_screen.cの持つ値） */
void VKAPI_PTR zink_stub_function_not_loaded(void) { CHECK(!"読み込まれていない関数"); }
/* デバッグのメッセンジャーは無い：vk_log.cがこれらを呼べば誤り */
void vk_debug_message(struct vk_instance *instance, VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                      VkDebugUtilsMessageTypeFlagsEXT types, const VkDebugUtilsMessengerCallbackDataEXT *data) {
    (void)instance; (void)severity; (void)types; (void)data; CHECK(!"メッセンジャーへ送った");
}
void vk_debug_message_instance(struct vk_instance *instance, VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                               VkDebugUtilsMessageTypeFlagsEXT types, const char *id, int32_t number,
                               const char *message) {
    (void)instance; (void)severity; (void)types; (void)id; (void)number; (void)message;
}
void vk_debug_report(struct vk_debug_report *report, VkDebugReportFlagsEXT flags, const struct vk_object_base *object,
                     size_t location, int32_t code, const char *prefix, const char *message) {
    (void)report; (void)flags; (void)object; (void)location; (void)code; (void)prefix; (void)message;
    CHECK(!"debug_reportへ送った");
}

/* ---- NVKの大域コマンドだけを持つICD ---- */
static VkResult create_result = VK_SUCCESS;
static bool icd_lists_extensions = true;
static uint32_t created_layers = UINT32_MAX, created_extensions;
static bool created_with_properties2;
static int fake_instance;
static VKAPI_ATTR VkResult VKAPI_CALL icd_enumerate_version(uint32_t *version) {
    *version = VK_API_VERSION_1_4;
    return VK_SUCCESS;
}
static const char *const icd_extensions[] = {
    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME, VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
};
static VKAPI_ATTR VkResult VKAPI_CALL icd_enumerate_extensions(const char *layer, uint32_t *count,
                                                              VkExtensionProperties *properties) {
    CHECK(layer == NULL);
    const uint32_t total = sizeof icd_extensions / sizeof icd_extensions[0];
    if (!properties) { *count = total; return VK_SUCCESS; }
    uint32_t n = *count < total ? *count : total;
    for (uint32_t i = 0; i < n; i++) {
        memset(&properties[i], 0, sizeof properties[i]);
        snprintf(properties[i].extensionName, sizeof properties[i].extensionName, "%s", icd_extensions[i]);
        properties[i].specVersion = 1;
    }
    *count = n;
    return n < total ? VK_INCOMPLETE : VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL icd_create_instance(const VkInstanceCreateInfo *info,
                                                         const VkAllocationCallbacks *allocator,
                                                         VkInstance *instance) {
    CHECK(allocator == NULL && info->sType == VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
    created_layers = info->enabledLayerCount;
    created_extensions = info->enabledExtensionCount;
    created_with_properties2 = false;
    for (uint32_t i = 0; i < info->enabledExtensionCount; i++)
        if (!strcmp(info->ppEnabledExtensionNames[i], VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
            created_with_properties2 = true;
    *instance = create_result == VK_SUCCESS ? (VkInstance)&fake_instance : VK_NULL_HANDLE;
    return create_result;
}
static unsigned instance_lookups;
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name) {
    instance_lookups++;
    if (instance)
        return NULL; /* この試験ではインスタンスの関数は問わない */
    if (!strcmp(name, "vkEnumerateInstanceVersion")) return (PFN_vkVoidFunction)icd_enumerate_version;
    if (!strcmp(name, "vkEnumerateInstanceExtensionProperties"))
        return icd_lists_extensions ? (PFN_vkVoidFunction)icd_enumerate_extensions : NULL;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)icd_create_instance;
    return NULL; /* vkEnumerateInstanceLayerPropertiesを含め、NVKに無いもの */
}
static VKAPI_ATTR void VKAPI_CALL device_proc(void) {}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_common_GetDeviceProcAddr(VkDevice device, const char *name) {
    (void)device; (void)name; return (PFN_vkVoidFunction)device_proc;
}

static VkInstance create(struct zink_screen *screen, struct zink_instance_info *info, char **lines) {
    memset(info, 0, sizeof *info);
    info->loader_version = VK_API_VERSION_1_3;
    created_layers = UINT32_MAX;
    mesa_log_hold();
    VkInstance instance = zink_create_instance(screen, info);
    *lines = mesa_log_release();
    CHECK(!mesa_log_holding());
    return instance;
}

int main(void) {
    /* ローダー役：Vulkanの名前だけを開き、ローダーの関数を先に、残りをドライバーに答えさせる。 */
    CHECK(util_dl_open("libvulkan.so.1") == NULL && util_dl_open(NULL) == NULL);
    struct util_dl_library *lib = util_dl_open("vulkan");
    CHECK(lib != NULL);
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)util_dl_get_proc_address(lib, "vkGetInstanceProcAddr");
    CHECK(gipa != NULL && gipa(NULL, "vkGetInstanceProcAddr") == (PFN_vkVoidFunction)gipa);
    CHECK(util_dl_get_proc_address(lib, "vkGetDeviceProcAddr") == (util_dl_proc)vk_common_GetDeviceProcAddr);
    CHECK(util_dl_get_proc_address(lib, "vkCreateInstance") == (util_dl_proc)icd_create_instance);
    CHECK(gipa(NULL, "vkEnumerateInstanceVersion") == (PFN_vkVoidFunction)icd_enumerate_version);
    CHECK(gipa(NULL, NULL) == NULL && gipa(NULL, "vkNoSuchCommand") == NULL);
    /* 層の一覧はローダーのもの：層は無く、VK_SUCCESS。 */
    PFN_vkEnumerateInstanceLayerProperties layers =
        (PFN_vkEnumerateInstanceLayerProperties)gipa(NULL, "vkEnumerateInstanceLayerProperties");
    CHECK(layers != NULL);
    uint32_t layer_count = 5;
    VkLayerProperties layer_props[1];
    if (layers) {
        CHECK(layers(&layer_count, NULL) == VK_SUCCESS && layer_count == 0);
        layer_count = 1;
        CHECK(layers(&layer_count, layer_props) == VK_SUCCESS && layer_count == 0);
    }
    CHECK(util_dl_get_proc_address(lib, "vkEnumerateInstanceLayerProperties") == (util_dl_proc)layers);
    /* インスタンスへの問いはドライバーへ渡す（ローダー役は答えない）。 */
    unsigned before = instance_lookups;
    CHECK(gipa((VkInstance)&fake_instance, "vkEnumerateInstanceLayerProperties") == NULL &&
          instance_lookups == before + 1);

    struct zink_screen *screen = calloc(1, sizeof *screen);
    screen->vk_GetInstanceProcAddr = gipa;
    screen->loader_lib = lib;
    struct zink_instance_info info;
    char *lines;

    /* 本物のzink_create_instance：インスタンスが作られ、層は付けず、理由の行も無い。 */
    clear_logs();
    VkInstance instance = create(screen, &info, &lines);
    CHECK(instance == (VkInstance)&fake_instance && lines == NULL);
    CHECK(created_layers == 0 && created_with_properties2 && info.have_KHR_get_physical_device_properties2);
    CHECK(info.have_KHR_external_memory_capabilities && !info.have_layer_KHRONOS_validation);
    CHECK(system_sends == 0 && !file_log[0]);
    free(lines);

    /* ローダーに関数が無い：理由を一行預け、預かり中は何も書かない。 */
    icd_lists_extensions = false;
    instance = create(screen, &info, &lines);
    CHECK(instance == VK_NULL_HANDLE && created_layers == UINT32_MAX);
    CHECK(lines && !strcmp(lines, "MESA: error: ZINK: the Vulkan loader has no vkEnumerateInstanceExtensionProperties\n"));
    CHECK(system_sends == 0 && !file_log[0]);
    /* 預かった行は、書かれるときにシステムの記録へ一度でまとめて、FILEの記録へも同じ形で出る。 */
    mesa_log_write_lines(lines);
    CHECK(system_sends == 1 && lines && !strcmp(system_log, lines) && !strcmp(file_log, lines));
    free(lines);
    icd_lists_extensions = true;

    /* vkCreateInstanceの失敗は、その結果の名前とともに。 */
    clear_logs();
    create_result = VK_ERROR_INCOMPATIBLE_DRIVER;
    instance = create(screen, &info, &lines);
    CHECK(instance == VK_NULL_HANDLE && created_layers == 0);
    CHECK(lines && !strcmp(lines, "MESA: error: ZINK: vkCreateInstance failed (VK_ERROR_INCOMPATIBLE_DRIVER)\n"));
    CHECK(system_sends == 0);
    free(lines);
    create_result = VK_SUCCESS;

    /* 推測されたドライバー名で開かれたZinkは、失敗を黙る（上流のまま）。 */
    screen->driver_name_is_inferred = true;
    icd_lists_extensions = false;
    instance = create(screen, &info, &lines);
    CHECK(instance == VK_NULL_HANDLE && lines == NULL);
    screen->driver_name_is_inferred = false;
    icd_lists_extensions = true;

    /* 預かっていないときは、その場でシステムの記録とFILEの記録へ一行ずつ。 */
    clear_logs();
    mesa_loge("ZINK: %s", "a line not held");
    CHECK(system_sends == 1 && !strcmp(system_log, "MESA: error: ZINK: a line not held\n"));
    CHECK(!strcmp(file_log, "MESA: error: ZINK: a line not held\n"));
    /* 何も預かっていなければ何も書かない。 */
    clear_logs();
    mesa_log_hold();
    CHECK(mesa_log_release() == NULL);
    mesa_log_write_lines(NULL);
    mesa_log_write_lines("");
    CHECK(system_sends == 0 && !file_log[0]);
    /* 記録の段階より細かい行は預からない（既定はinfoまで）。 */
    mesa_log_hold();
    mesa_logd("debug only");
    mesa_logi("an info line");
    lines = mesa_log_release();
    CHECK(lines && !strcmp(lines, "MESA: info: an info line\n"));
    free(lines);

    /* NVKのvk_errorf()：普段（リリース版）は何も書かない。 */
    struct vk_instance vk_instance;
    memset(&vk_instance, 0, sizeof vk_instance);
    vk_instance.base.type = VK_OBJECT_TYPE_INSTANCE;
    vk_instance.base.client_visible = true;
    list_inithead(&vk_instance.debug_utils.callbacks);
    list_inithead(&vk_instance.debug_report.callbacks);
    clear_logs();
    CHECK(vk_errorf(&vk_instance.base, VK_ERROR_INITIALIZATION_FAILED, "a channel: %s", "x") ==
          VK_ERROR_INITIALIZATION_FAILED);
    CHECK(system_sends == 0 && !file_log[0]);
    /* 預かり中は、場所と結果の名前つきで預かる（インスタンスの有る場合も無い場合も）。 */
    mesa_log_hold();
    int error_line = __LINE__ + 1;
    CHECK(vk_errorf(&vk_instance.base, VK_ERROR_INITIALIZATION_FAILED, "a channel: %s", "x") ==
          VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY) == VK_ERROR_OUT_OF_HOST_MEMORY);
    lines = mesa_log_release();
    char expected[512];
    snprintf(expected, sizeof expected,
             "MESA: warning: %s:%d: a channel: x (VK_ERROR_INITIALIZATION_FAILED)\n"
             "MESA: warning: %s:%d: VK_ERROR_OUT_OF_HOST_MEMORY\n",
             __FILE__, error_line, __FILE__, error_line + 2);
    CHECK(lines && !strcmp(lines, expected));
    CHECK(system_sends == 0 && !file_log[0]);
    free(lines);

    free(screen);
    printf("Zink起動ホスト試験: %u件失敗（実GPU・NVK本体・他threadの記録は未検証）\n", failures);
    return failures != 0;
}
