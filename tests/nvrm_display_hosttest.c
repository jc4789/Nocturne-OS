/* nvrm's screen (ports/nvidia/display.c), the production file, against a fake KAPI, a fake RM
   kernel client, a fake RM file and fake native handle services. It checks the order of the
   bring-up and of the tear-down, the choice of display and mode, that nothing is given to the
   system when a step fails, the reconnection of displays, and direct scanout: apps' images
   made, presented only while the system lets the app be shown directly (one flip waiting at a
   time), the pointer as the cursor over them, the desktop shown again when the system says so,
   images freed off the screen (DESTROY, the connection's end, stop) and IMPORT's ownership and
   RM sharing. No GPU, NVKMS, QEMU or real hardware runs.
   Run with tests/nvrm_display_hosttest.py (the flags come from ports/nvidia/nvrm.mk). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ports/nvidia/display.c"

static int failures;
#define CHECK(expr) do { if (!(expr)) { \
    printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; \
} } while (0)

/* ---------------------------------------------------------------- what happened, in order */
static char calls[16384];
static void called(const char *fmt, ...) {
    size_t n = strlen(calls);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(calls + n, sizeof calls - n, fmt, ap);
    va_end(ap);
    n = strlen(calls);
    if (n + 1 < sizeof calls) { calls[n] = ' '; calls[n + 1] = 0; }
}
/* Every word of seq appears in calls, in this order. */
static bool in_order(const char *seq) {
    char words[512];
    snprintf(words, sizeof words, "%s", seq);
    const char *at = calls;
    for (char *w = strtok(words, " "); w; w = strtok(NULL, " ")) {
        char needle[80];
        snprintf(needle, sizeof needle, "%s ", w);
        const char *found = strstr(at, needle);
        while (found && found != calls && found[-1] != ' ') found = strstr(found + 1, needle);
        if (!found) { printf("  missing or out of order: %s\n  calls: %s\n", w, calls); return false; }
        at = found + strlen(needle);
    }
    return true;
}
static bool never(const char *word) {
    char needle[80];
    snprintf(needle, sizeof needle, "%s ", word);
    for (const char *f = strstr(calls, needle); f; f = strstr(f + 1, needle))
        if (f == calls || f[-1] == ' ') { printf("  unexpected: %s\n  calls: %s\n", word, calls); return false; }
    return true;
}

static char logs[65536];
void nvrm_log(const char *fmt, ...) {
    size_t n = strlen(logs);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(logs + n, sizeof logs - n, fmt, ap);
    va_end(ap);
    n = strlen(logs);
    if (n + 1 < sizeof logs) { logs[n] = '\n'; logs[n + 1] = 0; }
}

/* ---------------------------------------------------------------- the machine */
#define W 1920u
#define H 1080u
#define GOP_PITCH 8192u
#define BAR1_BUS 0xd0000000ull
#define GOP_BUS (BAR1_BUS + 0x100000ull)
#define VRAM_BUS (BAR1_BUS + 0x4000000ull)
#define GOP_VIEW 40
#define SURFACE_VIEW 41

struct nvrm_device nvrm_dev;
static uint8_t gop[GOP_PITCH * H];
static uint8_t vram[7680u * H + 4096u];
/* What KAPI maps of the other memory it allocates: the cursor and apps' images. */
#define EXTRA_VRAM 6
static uint8_t extra_vram[EXTRA_VRAM][7680u * H];
static uint64_t clock_ns;

uint64_t nvrm_now_ns(void) { return clock_ns; }
void n_sleep_ms(uint64_t ms) { clock_ns += ms * 1000000u; }
const char *n_result_text(n_result r) { (void)r; return "error"; }

static uint64_t view_phys[4], view_bytes[4];
static uint32_t view_caching[4], view_access[4];
static int views;
n_handle nvrm_bar_view(uint64_t phys, uint64_t bytes, uint32_t nv_caching, uint32_t access, uint64_t *view_offset) {
    if (views < 4) { view_phys[views] = phys; view_bytes[views] = bytes; view_caching[views] = nv_caching; view_access[views] = access; }
    views++;
    *view_offset = 0;
    if (phys == GOP_BUS) { called("view-gop"); return GOP_VIEW; }
    if (phys == VRAM_BUS) { called("view-surface"); return SURFACE_VIEW; }
    return -N_ERR_INVALID;
}
static bool mapping_not_bar;
bool nvrm_kernel_mapping_phys(const void *address, uint64_t *phys) {
    const uint8_t *p = address;
    if (mapping_not_bar || p < vram || p >= vram + sizeof vram) return false;
    *phys = VRAM_BUS + (uint64_t)(p - vram);
    return true;
}
n_result n_shared_map(n_handle shared, size_t bytes, int access, void **mapping) {
    if (shared != GOP_VIEW || access != N_MEMORY_READ || bytes < (size_t)GOP_PITCH * (H - 1) + W * 4) return -N_ERR_INVALID;
    called("map-gop");
    *mapping = gop;
    return 0;
}
n_result n_memory_release(void *addr, size_t bytes) { (void)bytes; if (addr == gop) called("unmap-gop"); return 0; }
n_result n_handle_close(n_handle h) { called("close%d", (int)h); return 0; }

static n_result give_result;
static bool direct_refused;      /* the system refuses N_SCANOUT_DIRECT */
static struct n_scanout given_desc;
static n_handle given_surface;
n_result n_pci_give_screen(n_handle pci, n_handle surface, const struct n_scanout *desc) {
    (void)pci;
    if (surface == N_SCANOUT_BOOT) { called("give-boot"); return 0; }
    called((desc->format & N_SCANOUT_DIRECT) ? "give-surface-direct" : "give-surface");
    if (direct_refused && (desc->format & N_SCANOUT_DIRECT)) return -N_ERR_INVALID;
    given_surface = surface;
    if (desc) given_desc = *desc;
    return give_result;
}

/* What the system says the screen shows (n_pci_screen_state). A wait on a state already seen
   stops the state thread, so the test runs state_thread() once per change. */
static struct n_screen_state screen;
static n_result screen_result;
n_result n_pci_screen_state(n_handle pci, uint64_t seen, struct n_screen_state *out, int64_t timeout_ms) {
    CHECK(pci == nvrm_dev.pci);
    if (screen_result) {
        if (timeout_ms > 0) __atomic_store_n(&scr.state_stop, true, __ATOMIC_RELEASE);
        return screen_result;
    }
    *out = screen;
    if (seen != screen.serial) return 0;
    if (timeout_ms > 0) __atomic_store_n(&scr.state_stop, true, __ATOMIC_RELEASE);
    return -N_ERR_TIMEOUT;
}
static void screen_says(uint32_t app, bool pointer, int32_t x, int32_t y) {
    screen.serial++;
    screen.app = app;
    screen.pointer_shown = pointer;
    screen.pointer_x = x;
    screen.pointer_y = y;
}
static void screen_pointer_image(uint32_t seed) {
    for (uint32_t i = 0; i < N_SCREEN_POINTER_SIZE * N_SCREEN_POINTER_SIZE; i++) screen.pointer[i] = seed + i;
    screen.pointer_serial++;
    screen.serial++;
}
/* The state thread, run until it has followed the latest state. */
static void follow_screen(void) {
    __atomic_store_n(&scr.state_stop, false, __ATOMIC_RELEASE);
    state_thread(NULL);
}
static uint64_t vblank_count, vblank_ts;
static int vblank_calls;
n_result n_pci_screen_vblank(n_handle pci, uint64_t count, uint64_t timestamp_ns) {
    (void)pci;
    vblank_calls++;
    vblank_count = count;
    vblank_ts = timestamp_ns;
    return 0;
}

/* Single-threaded: the display thread is created but never run; the test calls rescan(). */
int thrd_create(thrd_t *thr, thrd_start_t func, void *arg) { (void)thr; (void)func; (void)arg; called("thread"); return thrd_success; }
int thrd_join(thrd_t thr, int *res) { (void)thr; (void)res; called("join"); return thrd_success; }
int mtx_init(mtx_t *m, int type) { (void)m; (void)type; return thrd_success; }
void mtx_destroy(mtx_t *m) { (void)m; }
int mtx_lock(mtx_t *m) { (void)m; return thrd_success; }
int mtx_unlock(mtx_t *m) { (void)m; return thrd_success; }
int cnd_init(cnd_t *c) { (void)c; return thrd_success; }
void cnd_destroy(cnd_t *c) { (void)c; }
int cnd_signal(cnd_t *c) { (void)c; return thrd_success; }
int cnd_wait(cnd_t *c, mtx_t *m) { (void)c; (void)m; return thrd_success; }

/* ---------------------------------------------------------------- RM's kernel client */
#define NVKMS_CLIENT 0xc1d0000au /* the RM client NVKMS's memory belongs to */
static uint32_t rm_boot_mask;
static bool rm_boot_fails;
/* NV04_SHARE: what NVKMS's memory is shared with (0 none), and the policies asked for. */
static NvHandle shared_with, shared_object;
static NVOS57_PARAMETERS last_share[2];
static int shares;
void NV_API_CALL rm_kernel_rmapi_op(nvidia_stack_t *sp, void *ops_cmd) {
    (void)sp;
    nvidia_kernel_rmapi_ops_t *ops = ops_cmd;
    switch (ops->op) {
    case NV04_ALLOC:
        if (ops->params.alloc.hClass == NV01_ROOT) *(NvHandle *)NvP64_VALUE(ops->params.alloc.pAllocParms) = 0xc1d00001;
        called("rm-alloc-%x", (unsigned)ops->params.alloc.hClass);
        ops->params.alloc.status = NV_OK;
        break;
    case NV04_CONTROL:
        called("rm-control-%x", (unsigned)ops->params.control.cmd);
        ops->params.control.status = NV_OK;
        if (ops->params.control.cmd == NV0073_CTRL_CMD_SYSTEM_GET_BOOT_DISPLAYS) {
            NV0073_CTRL_SYSTEM_GET_BOOT_DISPLAYS_PARAMS *p = NvP64_VALUE(ops->params.control.params);
            p->bootDisplayMask = rm_boot_mask;
            if (rm_boot_fails) ops->params.control.status = NV_ERR_NOT_SUPPORTED;
        }
        break;
    case NV01_FREE:
        called("rm-free");
        ops->params.free.status = NV_OK;
        break;
    case NV04_SHARE: {
        NVOS57_PARAMETERS *sh = &ops->params.share;
        if (shares < 2) last_share[shares] = *sh;
        shares++;
        bool revoke = sh->sharePolicy.action & RS_SHARE_ACTION_FLAG_REVOKE;
        called(revoke ? "rm-unshare" : "rm-share");
        CHECK(sh->hClient == NVKMS_CLIENT && sh->sharePolicy.type == RS_SHARE_TYPE_CLIENT &&
              (sh->sharePolicy.action & RS_SHARE_ACTION_FLAG_COMPOSE) &&
              (sh->sharePolicy.accessMask.limbs[0] & (1u << RS_ACCESS_DUP_OBJECT)));
        shared_with = revoke ? 0 : sh->sharePolicy.target;
        shared_object = revoke ? 0 : sh->hObject;
        sh->status = NV_OK;
        break;
    }
    default:
        abort();
    }
}

/* ---------------------------------------------------------------- the fake KAPI */
struct fake_mode { uint32_t w, h, rr; bool valid, preferred, interlaced, reject; };
struct fake_display {
    NvKmsKapiDisplay handle;
    uint32_t head_mask;
    bool connected;
    struct fake_mode modes[8];
    unsigned mode_count;
};
static struct fake_display displays[4];
static unsigned display_count;
static bool flip_pending_forever, apply_check_fails, alloc_fails;
static struct NvKmsKapiRequestedModeSetConfig last_request;
static NvU32 interest;
static NVRgInterruptCallbackProc vblank_proc;
static int dev_obj, vblank_obj;
/* KAPI's memory and surfaces, in the order allocated (the desktop's surface first). */
#define POOL 16
static int mem_pool[POOL], surf_pool[POOL];
static unsigned mem_used, surf_used, mem_freed, surf_destroyed;
static struct NvKmsKapiAllocateMemoryParams alloc_log[POOL];
static struct NvKmsKapiCreateSurfaceParams surface_log[POOL];
static bool cursor_caps_missing;
static unsigned pending_polls;   /* polls before the flip in progress completes */
static NvBool k_pending(const struct NvKmsKapiDevice *d, const NvU32 head, const NvU32 layer, NvBool *pending);

/* RM's handles of KAPI's memory (nvkms_kapi_rm.c reads them from KAPI's structures). */
#define NVKMS_MEMORY(i) (0xcafe0000u + (i))
bool nvrm_nvkms_memory_handle(const struct NvKmsKapiDevice *device, const struct NvKmsKapiMemory *memory,
                              uint32_t *client, uint32_t *object) {
    CHECK(device == (const struct NvKmsKapiDevice *)&dev_obj);
    *client = NVKMS_CLIENT;
    *object = NVKMS_MEMORY((unsigned)((const int *)memory - mem_pool));
    return true;
}

/* The app's RM file (rm_api.c): only NV_ESC_RM_DUP_OBJECT, which RM allows while NVKMS's memory
   is shared with the destination client. */
#define APP_FILE ((void *)&app_files[0])
#define OTHER_FILE ((void *)&app_files[1])
static int app_files[2];
static NVOS55_PARAMETERS last_dup;
static void *dup_file;
static uint32_t dup_result;
uint32_t nvrm_rm_ioctl(void *file, uint32_t escape, void *args, uint32_t size) {
    CHECK(escape == NV_ESC_RM_DUP_OBJECT && size == sizeof(NVOS55_PARAMETERS));
    called("rm-dup");
    NVOS55_PARAMETERS *dup = args;
    dup_file = file;
    last_dup = *dup;
    dup->status = dup_result ? dup_result
                : shared_with == dup->hClient && shared_object == dup->hObjectSrc ? NV_OK : NV_ERR_INSUFFICIENT_PERMISSIONS;
    if (dup->status == NV_OK && !dup->hObject) dup->hObject = 0xd0000001u;
    return NV_OK;
}

static struct fake_display *find_display(NvKmsKapiDisplay d) {
    for (unsigned i = 0; i < display_count; i++) if (displays[i].handle == d) return &displays[i];
    return NULL;
}

static struct NvKmsKapiDevice *k_allocate_device(const struct NvKmsKapiAllocateDeviceParams *p) {
    called("allocateDevice");
    CHECK(p->gpuId == nvrm_dev.nv.gpu_id && p->eventCallback == kapi_event);
    return (struct NvKmsKapiDevice *)&dev_obj;
}
static void k_free_device(struct NvKmsKapiDevice *d) { (void)d; called("freeDevice"); }
static NvBool k_grab(struct NvKmsKapiDevice *d) { (void)d; called("grabOwnership"); return NV_TRUE; }
static void k_release(struct NvKmsKapiDevice *d) { (void)d; called("releaseOwnership"); }
static NvBool k_interest(const struct NvKmsKapiDevice *d, const NvU32 mask) {
    (void)d;
    called("declareEventInterest-%x", (unsigned)mask);
    interest = mask;
    return NV_TRUE;
}
static NvBool k_resources(struct NvKmsKapiDevice *d, struct NvKmsKapiDeviceResourcesInfo *info) {
    (void)d;
    called("getDeviceResourcesInfo");
    memset(info, 0, sizeof *info);
    info->numHeads = 4;
    for (int h = 0; h < 4; h++) info->numLayers[h] = 3;
    info->numConnectors = display_count;
    info->caps.pitchAlignment = 256;
    info->caps.hasVideoMemory = 1;
    if (!cursor_caps_missing) {
        info->caps.supportedCursorSurfaceMemoryFormats = 1ull << NvKmsSurfaceMemoryFormatA8R8G8B8;
        info->caps.maxCursorSizeInPixels = 256;
        info->caps.validCursorCompositionModes = 1u << NVKMS_COMPOSITION_BLENDING_MODE_OPAQUE |
                                                 1u << NVKMS_COMPOSITION_BLENDING_MODE_NON_PREMULT_ALPHA;
    }
    info->supportedSurfaceMemoryFormats[NVKMS_KAPI_LAYER_PRIMARY_IDX] =
        1ull << NvKmsSurfaceMemoryFormatX8R8G8B8 | 1ull << NvKmsSurfaceMemoryFormatX8B8G8R8;
    info->vtFbBaseAddress = GOP_BUS;
    info->vtFbSize = (NvU64)GOP_PITCH * H;
    return NV_TRUE;
}
static NvBool k_displays(struct NvKmsKapiDevice *d, NvU32 *n, NvKmsKapiDisplay *handles) {
    (void)d;
    if (!handles) { *n = display_count; return NV_TRUE; }
    called("getDisplays");
    if (*n < display_count) return NV_FALSE;
    for (unsigned i = 0; i < display_count; i++) handles[i] = displays[i].handle;
    *n = display_count;
    return NV_TRUE;
}
static NvBool k_connector(struct NvKmsKapiDevice *d, NvKmsKapiConnector c, struct NvKmsKapiConnectorInfo *info) {
    (void)d;
    info->handle = c;
    info->type = NVKMS_CONNECTOR_TYPE_DP;
    info->physicalIndex = c;
    return NV_TRUE;
}
static NvBool k_static(struct NvKmsKapiDevice *d, NvKmsKapiDisplay h, struct NvKmsKapiStaticDisplayInfo *info) {
    (void)d;
    struct fake_display *f = find_display(h);
    if (!f) return NV_FALSE;
    info->handle = h;
    info->connectorHandle = h;
    info->headMask = f->head_mask;
    return NV_TRUE;
}
static NvBool k_dynamic(struct NvKmsKapiDevice *d, struct NvKmsKapiDynamicDisplayParams *p) {
    (void)d;
    struct fake_display *f = find_display(p->handle);
    if (!f) return NV_FALSE;
    p->connected = f->connected;
    if (f->connected) {
        static const uint8_t edid[128] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0x10, 0xac, 0x34, 0x12,
            [54] = 0, 0, 0, 0xfc, 0, 'T', 'E', 'S', 'T', ' ', 'M', 'O', 'N', 0x0a, ' ', ' ', ' ', ' ' };
        memcpy(p->edid.buffer, edid, sizeof edid);
        p->edid.bufferSize = sizeof edid;
    }
    return NV_TRUE;
}
static int k_mode(struct NvKmsKapiDevice *d, NvKmsKapiDisplay h, NvU32 index, struct NvKmsKapiDisplayMode *mode,
                  NvBool *valid, NvBool *preferred) {
    (void)d;
    struct fake_display *f = find_display(h);
    if (!f) return -1;
    if (index >= f->mode_count) return 0;
    const struct fake_mode *m = &f->modes[index];
    memset(mode, 0, sizeof *mode);
    mode->timings.hVisible = m->w;
    mode->timings.vVisible = m->h;
    mode->timings.refreshRate = m->rr;
    mode->timings.flags.interlaced = m->interlaced;
    *valid = m->valid;
    *preferred = m->preferred;
    return 1;
}
static NvBool k_validate(struct NvKmsKapiDevice *d, NvKmsKapiDisplay h, const struct NvKmsKapiDisplayMode *mode) {
    (void)d;
    struct fake_display *f = find_display(h);
    for (unsigned i = 0; f && i < f->mode_count; i++) {
        const struct fake_mode *m = &f->modes[i];
        if (m->w == mode->timings.hVisible && m->h == mode->timings.vVisible && m->rr == mode->timings.refreshRate)
            return !m->reject;
    }
    return NV_FALSE;
}
static struct NvKmsKapiMemory *k_alloc(struct NvKmsKapiDevice *d, struct NvKmsKapiAllocateMemoryParams *p) {
    (void)d;
    called("allocateMemory");
    if (alloc_fails || mem_used == POOL) return NULL;
    alloc_log[mem_used] = *p;
    return (struct NvKmsKapiMemory *)&mem_pool[mem_used++];
}
static void k_free_memory(struct NvKmsKapiDevice *d, struct NvKmsKapiMemory *m) {
    (void)d; (void)m;
    called("freeMemory");
    mem_freed++;
}
/* The desktop's surface is vram; the others take extra_vram in turn. */
static uint8_t *memory_pixels(const struct NvKmsKapiMemory *m) {
    unsigned i = (unsigned)((const int *)m - mem_pool);
    return i == 0 ? vram : i - 1 < EXTRA_VRAM ? extra_vram[i - 1] : NULL;
}
static NvBool k_map(const struct NvKmsKapiDevice *d, const struct NvKmsKapiMemory *m, NvKmsKapiMappingType t, void **pp) {
    (void)d;
    called("mapMemory");
    CHECK(t == NVKMS_KAPI_MAPPING_TYPE_KERNEL);
    *pp = memory_pixels(m);
    return *pp != NULL;
}
static void k_unmap(const struct NvKmsKapiDevice *d, const struct NvKmsKapiMemory *m, NvKmsKapiMappingType t, const void *p) {
    (void)d; (void)t;
    CHECK(p == memory_pixels(m));
    called("unmapMemory");
}
static struct NvKmsKapiSurface *k_surface(struct NvKmsKapiDevice *d, struct NvKmsKapiCreateSurfaceParams *p) {
    (void)d;
    called("createSurface");
    if (surf_used == POOL) return NULL;
    surface_log[surf_used] = *p;
    return (struct NvKmsKapiSurface *)&surf_pool[surf_used++];
}
static void k_destroy_surface(struct NvKmsKapiDevice *d, struct NvKmsKapiSurface *s) {
    (void)d; (void)s;
    called("destroySurface");
    surf_destroyed++;
}
static NvBool k_apply(struct NvKmsKapiDevice *d, const struct NvKmsKapiRequestedModeSetConfig *req,
                      struct NvKmsKapiModeSetReplyConfig *reply_config, const NvBool commit) {
    (void)d;
    called(commit ? "apply-commit" : "apply-check");
    if (!commit && apply_check_fails) return NV_FALSE;
    if (commit) last_request = *req;
    reply_config->flipResult = NV_KMS_FLIP_RESULT_SUCCESS;
    return NV_TRUE;
}
static NvBool k_pending(const struct NvKmsKapiDevice *d, const NvU32 head, const NvU32 layer, NvBool *pending) {
    (void)d; (void)layer;
    /* A flip that never completes is polled every millisecond: record its first poll only. */
    static uint64_t pending_since = UINT64_MAX;
    if (!flip_pending_forever || pending_since != scr.vblanks) called("flipPending-%u", (unsigned)head);
    pending_since = flip_pending_forever ? scr.vblanks : UINT64_MAX;
    *pending = flip_pending_forever || pending_polls;
    if (pending_polls) pending_polls--;
    return NV_TRUE;
}
static struct NvKmsKapiVblankIntrCallback *k_vblank_on(struct NvKmsKapiDevice *d, const NvU32 head,
                                                        NVRgInterruptCallbackProc proc, NvU64 param) {
    (void)d; (void)param;
    called("registerVblank-%u", (unsigned)head);
    vblank_proc = proc;
    return (struct NvKmsKapiVblankIntrCallback *)&vblank_obj;
}
static void k_vblank_off(struct NvKmsKapiDevice *d, const NvU32 head, struct NvKmsKapiVblankIntrCallback *cb) {
    (void)d;
    CHECK(cb == (struct NvKmsKapiVblankIntrCallback *)&vblank_obj);
    called("unregisterVblank-%u", (unsigned)head);
    vblank_proc = NULL;
}
static void k_console_disabled(struct NvKmsKapiDevice *d) { (void)d; called("framebufferConsoleDisabled"); }

NvBool nvKmsKapiGetFunctionsTable(struct NvKmsKapiFunctionsTable *t) {
    if (strcmp(t->versionString, NV_VERSION_STRING) != 0) return NV_FALSE;
    t->allocateDevice = k_allocate_device;
    t->freeDevice = k_free_device;
    t->grabOwnership = k_grab;
    t->releaseOwnership = k_release;
    t->declareEventInterest = k_interest;
    t->getDeviceResourcesInfo = k_resources;
    t->getDisplays = k_displays;
    t->getConnectorInfo = k_connector;
    t->getStaticDisplayInfo = k_static;
    t->getDynamicDisplayInfo = k_dynamic;
    t->getDisplayMode = k_mode;
    t->validateDisplayMode = k_validate;
    t->allocateMemory = k_alloc;
    t->freeMemory = k_free_memory;
    t->mapMemory = k_map;
    t->unmapMemory = k_unmap;
    t->createSurface = k_surface;
    t->destroySurface = k_destroy_surface;
    t->applyModeSetConfig = k_apply;
    t->getFlipPendingStatus = k_pending;
    t->registerVblankIntrCallback = k_vblank_on;
    t->unregisterVblankIntrCallback = k_vblank_off;
    t->framebufferConsoleDisabled = k_console_disabled;
    return NV_TRUE;
}

/* ---------------------------------------------------------------- scenarios */
static void reset(void) {
    memset(&scr, 0, sizeof scr);
    scr.view = -1;
    calls[0] = logs[0] = 0;
    views = 0;
    give_result = 0;
    direct_refused = cursor_caps_missing = false;
    memset(&screen, 0, sizeof screen);
    screen.serial = 1;
    screen.hot_x = screen.hot_y = 8;
    screen_result = 0;
    shared_with = shared_object = 0;
    shares = 0;
    memset(last_share, 0, sizeof last_share);
    memset(&last_dup, 0, sizeof last_dup);
    dup_file = NULL;
    dup_result = 0;
    mem_used = surf_used = mem_freed = surf_destroyed = 0;
    pending_polls = 0;
    memset(extra_vram, 0xee, sizeof extra_vram);
    given_surface = -1;
    memset(&given_desc, 0, sizeof given_desc);
    flip_pending_forever = apply_check_fails = alloc_fails = mapping_not_bar = false;
    rm_boot_mask = 0x2;
    rm_boot_fails = false;
    vblank_calls = 0;
    memset(&last_request, 0, sizeof last_request);
    memset(&nvrm_dev, 0, sizeof nvrm_dev);
    nvrm_dev.pci = 3;
    nvrm_dev.nv.gpu_id = 0x100;
    nvrm_dev.info.framebuffer_address = GOP_BUS;
    nvrm_dev.info.framebuffer_size = (uint64_t)GOP_PITCH * H;
    nvrm_dev.info.framebuffer_bar = 1;
    nvrm_dev.info.framebuffer_width = W;
    nvrm_dev.info.framebuffer_height = H;
    nvrm_dev.info.framebuffer_pitch = GOP_PITCH;
    nvrm_dev.info.framebuffer_bpp = 32;
    nvrm_dev.info.bar[1].address = BAR1_BUS;
    nvrm_dev.info.bar[1].size = 256ull << 20;
    nvrm_dev.info.bar[1].flags = N_PCI_BAR_MEMORY | N_PCI_BAR_PREFETCH | N_PCI_BAR_64BIT;
    /* display 0x1: not connected; 0x2: lit by the firmware, heads 1 and 2; 0x4: another monitor. */
    display_count = 3;
    displays[0] = (struct fake_display){ .handle = 0x1, .head_mask = 0xf, .connected = false };
    displays[1] = (struct fake_display){
        .handle = 0x2, .head_mask = 0x6, .connected = true, .mode_count = 6,
        .modes = {
            { 2560, 1440, 144000, true, true, false, false },  /* preferred: the refresh wanted */
            { W, H, 60000, true, false, false, false },
            { W, H, 143998, false, false, false, false },      /* not valid */
            { W, H, 144000, true, false, true, false },        /* interlaced */
            { W, H, 120000, true, false, false, false },       /* nearest valid progressive */
            { 1280, 720, 144000, true, false, false, false },
        } };
    displays[2] = (struct fake_display){
        .handle = 0x4, .head_mask = 0x1, .connected = true, .mode_count = 1,
        .modes = { { 3840, 2160, 60000, true, true, false, false } } };
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) ((uint32_t *)(gop + (size_t)y * GOP_PITCH))[x] = y << 16 | x;
    memset(vram, 0xee, sizeof vram);
}

static const struct NvKmsKapiHeadRequestedConfig *head_cfg(unsigned head) {
    return &last_request.headRequestedConfig[head];
}

static void bring_up_and_tear_down(void) {
    reset();
    CHECK(nvrm_display_start());
    CHECK(in_order("allocateDevice grabOwnership declareEventInterest-3 getDeviceResourcesInfo "
                   "rm-alloc-0 rm-control-205 rm-alloc-80 rm-alloc-73 rm-control-73011e rm-free getDisplays "
                   "allocateMemory mapMemory view-surface createSurface view-gop map-gop close40 unmap-gop "
                   "apply-check apply-commit flipPending-1 allocateMemory mapMemory createSurface give-surface-direct thread "
                   "registerVblank-1 thread"));
    CHECK(interest == (1u << NVKMS_EVENT_TYPE_DPY_CHANGED | 1u << NVKMS_EVENT_TYPE_DYNAMIC_DPY_CONNECTED));
    CHECK(never("framebufferConsoleDisabled"));
    /* Memory: pitch-linear video memory for scanout, the pitch aligned as NVKMS asks. */
    CHECK(alloc_log[0].layout == NvKmsSurfaceMemoryLayoutPitch && alloc_log[0].type == NVKMS_KAPI_ALLOCATION_TYPE_SCANOUT);
    CHECK(alloc_log[0].useVideoMemory && alloc_log[0].noDisplayCaching && alloc_log[0].size == 7680ull * H);
    CHECK(surface_log[0].format == NvKmsSurfaceMemoryFormatX8R8G8B8 && surface_log[0].planes[0].pitch == 7680 &&
          surface_log[0].width == W && surface_log[0].height == H);
    /* The cursor: 32x32 A8R8G8B8, pitch 256 (NVKMS's least for that size), video memory, mapped
       for the pointer's pixels; made after the mode set, before the surface is given. */
    CHECK(in_order("apply-commit flipPending-1 allocateMemory mapMemory createSurface give-surface-direct"));
    CHECK(mem_used == 2 && surf_used == 2);
    CHECK(alloc_log[1].layout == NvKmsSurfaceMemoryLayoutPitch && alloc_log[1].useVideoMemory &&
          alloc_log[1].size == 8192 && alloc_log[1].type == NVKMS_KAPI_ALLOCATION_TYPE_SCANOUT);
    CHECK(surface_log[1].format == NvKmsSurfaceMemoryFormatA8R8G8B8 && surface_log[1].width == 32 &&
          surface_log[1].height == 32 && surface_log[1].planes[0].pitch == 256 &&
          surface_log[1].planes[0].memory == (struct NvKmsKapiMemory *)&mem_pool[1]);
    /* The views: the surface write-combined and writable, the boot screen write-combined, read. */
    CHECK(view_phys[0] == VRAM_BUS && view_bytes[0] == 7680ull * H && view_caching[0] == NV_MEMORY_WRITECOMBINED &&
          view_access[0] == N_MEMORY_READ_WRITE);
    CHECK(view_phys[1] == GOP_BUS && view_caching[1] == NV_MEMORY_WRITECOMBINED && view_access[1] == N_MEMORY_READ);
    /* The boot screen's pixels, row by row into the other pitch; the padding is not written. */
    CHECK(((uint32_t *)vram)[0] == 0 && ((uint32_t *)(vram + 7680u * 500))[700] == (500u << 16 | 700));
    CHECK(((uint32_t *)(vram + 7680u * (H - 1)))[W - 1] == ((H - 1) << 16 | (W - 1)));
    /* The mode: the boot screen's size on display 0x2, refresh nearest 144 Hz among the valid
       progressive ones, on its lowest head, the surface unscaled. */
    CHECK(last_request.headsMask == 0x2);
    const struct NvKmsKapiHeadRequestedConfig *h = head_cfg(1);
    CHECK(h->modeSetConfig.bActive && h->modeSetConfig.numDisplays == 1 && h->modeSetConfig.displays[0] == 0x2);
    CHECK(h->modeSetConfig.mode.timings.hVisible == W && h->modeSetConfig.mode.timings.refreshRate == 120000);
    CHECK(h->flags.activeChanged && h->flags.displaysChanged && h->flags.modeChanged);
    const struct NvKmsKapiLayerRequestedConfig *p = &h->layerRequestedConfig[0];
    CHECK(p->config.surface == (struct NvKmsKapiSurface *)&surf_pool[0] && p->flags.surfaceChanged);
    CHECK(p->config.srcWidth == W && p->config.srcHeight == H && p->config.dstWidth == W && p->config.dstHeight == H);
    CHECK(p->config.csc.m[0][0] == 0x10000 && p->config.minPresentInterval == 1);
    CHECK(h->layerRequestedConfig[1].config.surface == NULL && h->layerRequestedConfig[1].flags.surfaceChanged);
    CHECK(!h->layerRequestedConfig[3].flags.surfaceChanged);
    /* What the system was given. */
    CHECK(given_surface == SURFACE_VIEW && given_desc.width == W && given_desc.height == H &&
          given_desc.pitch == 7680 && given_desc.format == (N_SCANOUT_XRGB8888 | N_SCANOUT_DIRECT) &&
          given_desc.offset == 0);
    CHECK(scr.direct && scr.state_running && in_order("give-surface-direct thread registerVblank-1 thread"));
    CHECK(strstr(logs, "display 0x00000002: DP-2, heads 0x6, connected, DEL1234 TEST MON, lit by the firmware"));

    /* Vertical blanks go to the system, counted, with NVKMS's timestamp. */
    CHECK(vblank_proc != NULL);
    if (vblank_proc) { vblank_proc(0, 123456789); vblank_proc(0, 0); }
    CHECK(vblank_calls == 2 && vblank_count == 2 && vblank_ts == clock_ns);

    calls[0] = 0;
    nvrm_display_stop();
    /* Both threads joined; the cursor and the surface freed after ownership is released. */
    CHECK(in_order("join join give-boot unregisterVblank-1 close41 releaseOwnership destroySurface unmapMemory "
                   "freeMemory destroySurface unmapMemory freeMemory declareEventInterest-0 freeDevice"));
    CHECK(!scr.direct && !scr.state_running && mem_freed == 2 && surf_destroyed == 2);
    /* After the boot screen is back, a late blank is not passed on. */
    vblank_calls = 0;
    scr.vblank = NULL;
    vblank_arrived(0, 1);
    CHECK(vblank_calls == 0);
}

static void refresh_choice(void) {
    reset();
    displays[1].modes[4].reject = true; /* NVKMS refuses 120 Hz: the next nearest, 60 Hz */
    CHECK(nvrm_display_start());
    CHECK(head_cfg(1)->modeSetConfig.mode.timings.refreshRate == 60000);
    nvrm_display_stop();

    reset();
    displays[1].modes[0].preferred = false; /* no preferred mode: nearest 60 Hz */
    displays[1].modes[1].rr = 59940;
    CHECK(nvrm_display_start());
    CHECK(head_cfg(1)->modeSetConfig.mode.timings.refreshRate == 59940);
    nvrm_display_stop();
}

static void nothing_given_on_failure(void) {
    /* No mode of the boot screen's size: no memory, no mode set, nothing given. */
    reset();
    displays[1].mode_count = 1;
    rm_boot_mask = 0x2;
    CHECK(!nvrm_display_start());
    CHECK(never("allocateMemory") && never("apply-commit") && never("give-surface") && never("give-surface-direct") && never("give-boot"));
    CHECK(in_order("grabOwnership releaseOwnership declareEventInterest-0 freeDevice"));

    /* NVKMS rejects the mode set: the surface is freed, nothing given. */
    reset();
    apply_check_fails = true;
    CHECK(!nvrm_display_start());
    CHECK(never("apply-commit") && never("give-surface") && never("give-surface-direct"));
    CHECK(in_order("apply-check close41 releaseOwnership destroySurface unmapMemory freeMemory freeDevice"));

    /* The flip never completes: not given; NVKMS restores the boot screen on release. */
    reset();
    flip_pending_forever = true;
    uint64_t before = clock_ns;
    CHECK(!nvrm_display_start());
    CHECK(clock_ns - before >= (uint64_t)FLIP_TIMEOUT_MS * 1000000u);
    CHECK(never("give-surface") && never("give-surface-direct") && in_order("apply-commit flipPending-1 releaseOwnership freeDevice"));

    /* The system refuses the surface: nothing to give back, the console restored. */
    reset();
    give_result = -N_ERR_DENIED;
    CHECK(!nvrm_display_start());
    CHECK(never("give-boot") && never("registerVblank-1") && never("thread"));
    CHECK(in_order("give-surface-direct give-surface close41 releaseOwnership destroySurface freeDevice"));
    CHECK(mem_freed == mem_used && surf_destroyed == surf_used);

    /* RM's mapping is not a BAR's: no view, nothing given. */
    reset();
    mapping_not_bar = true;
    CHECK(!nvrm_display_start());
    CHECK(never("view-surface") && never("apply-commit") && in_order("mapMemory unmapMemory freeMemory freeDevice"));

    /* The boot screen is not on this GPU, or not 32-bit: NVKMS is not even asked. */
    reset();
    nvrm_dev.info.framebuffer_bar = N_PCI_NONE;
    CHECK(!nvrm_display_start() && never("allocateDevice"));
    reset();
    nvrm_dev.info.framebuffer_bpp = 16;
    CHECK(!nvrm_display_start() && never("allocateDevice"));

    /* No video memory allocation: nothing given. */
    reset();
    alloc_fails = true;
    CHECK(!nvrm_display_start() && never("mapMemory") && never("give-surface") && never("give-surface-direct"));
}

static void boot_displays_unknown(void) {
    /* RM cannot say which displays the firmware lit: the first connected one (0x2). */
    reset();
    rm_boot_fails = true;
    CHECK(nvrm_display_start());
    CHECK(head_cfg(1)->modeSetConfig.displays[0] == 0x2);
    CHECK(strstr(logs, "cannot read the boot displays"));
    nvrm_display_stop();

    /* The firmware lit 0x4 only (and it has no 1920x1080 mode): not taken over at start. */
    reset();
    rm_boot_mask = 0x4;
    CHECK(!nvrm_display_start() && never("give-surface") && never("give-surface-direct"));
}

static void monitors_come_and_go(void) {
    reset();
    CHECK(nvrm_display_start());

    /* An event wakes the display thread; the shown display still being there changes nothing. */
    struct NvKmsKapiEvent event = { .type = NVKMS_EVENT_TYPE_DPY_CHANGED };
    kapi_event(&event);
    CHECK(scr.rescan);
    calls[0] = 0;
    rescan();
    CHECK(never("apply-commit") && never("unregisterVblank-1"));

    /* 0x2 is unplugged: its head off; 0x4 is there, without the size: its preferred 4K mode, the
       desktop scaled to it, on head 0. The surface stays given. */
    displays[1].connected = false;
    calls[0] = 0;
    rescan();
    CHECK(in_order("unregisterVblank-1 apply-check apply-commit apply-check apply-commit flipPending-0 registerVblank-0"));
    CHECK(never("give-boot") && never("give-surface") && never("give-surface-direct") && never("destroySurface"));
    CHECK(last_request.headsMask == 0x1);
    const struct NvKmsKapiHeadRequestedConfig *h = head_cfg(0);
    CHECK(h->modeSetConfig.displays[0] == 0x4 && h->modeSetConfig.mode.timings.hVisible == 3840);
    CHECK(h->layerRequestedConfig[0].config.srcWidth == W && h->layerRequestedConfig[0].config.dstWidth == 3840 &&
          h->layerRequestedConfig[0].config.dstHeight == 2160);
    CHECK(scr.display == 0x4 && scr.head == 0 && scr.head_on);

    /* 0x4 goes too: dark, the surface still the system's. */
    displays[2].connected = false;
    calls[0] = 0;
    rescan();
    CHECK(in_order("unregisterVblank-0 apply-check apply-commit") && never("registerVblank-0"));
    CHECK(last_request.headsMask == 0x1 && head_cfg(0)->modeSetConfig.numDisplays == 0);
    CHECK(scr.display == 0 && !scr.head_on);

    /* The firmware's monitor comes back: its own mode again, on head 1. */
    displays[1].connected = true;
    calls[0] = 0;
    rescan();
    CHECK(in_order("apply-check apply-commit flipPending-1 registerVblank-1"));
    CHECK(last_request.headsMask == 0x2 && head_cfg(1)->modeSetConfig.mode.timings.refreshRate == 120000);
    CHECK(head_cfg(1)->layerRequestedConfig[0].config.dstWidth == W);

    calls[0] = 0;
    nvrm_display_stop();
    CHECK(in_order("give-boot unregisterVblank-1 close41 releaseOwnership destroySurface freeDevice"));
}

/* ---------------------------------------------------------------- direct scanout */
#define APP 77u
#define OTHER_APP 78u
#define SURF(i) ((struct NvKmsKapiSurface *)&surf_pool[i])
/* Memory i is mapped at extra_vram[i - 1]: 1 the cursor, 2... apps' images. */
#define PIXEL(i, offset) (((uint32_t *)extra_vram[(i) - 1])[(offset) / 4])

static uint32_t scanout(void *file, uint32_t app, struct nvrm_scanout_request *r) {
    CHECK(nvrm_display_scanout(file, app, r, sizeof *r) == 0);
    return r->status;
}
static uint32_t scanout_op(void *file, uint32_t app, uint32_t op, uint32_t image) {
    struct nvrm_scanout_request r = { .op = op, .image = image };
    return scanout(file, app, &r);
}
static uint32_t create_image(void *file, uint32_t app) {
    struct nvrm_scanout_request r = { .op = NVRM_SCANOUT_CREATE };
    CHECK(scanout(file, app, &r) == NV_OK);
    return r.image;
}
static unsigned image_count(void) {
    unsigned n = 0;
    for (struct image *i = scr.images; i; i = i->next) n++;
    return n;
}

static void direct_scanout(void) {
    reset();
    CHECK(nvrm_display_start());

    /* CREATE: video memory for scanout of the screen's size and pitch, X8B8G8R8, black; nothing
       on the screen changes. */
    struct nvrm_scanout_request r = { .op = NVRM_SCANOUT_CREATE };
    calls[0] = 0;
    CHECK(scanout(APP_FILE, APP, &r) == NV_OK);
    CHECK(r.image != 0 && r.width == W && r.height == H && r.pitch == 7680 && r.bytes == 7680ull * H);
    CHECK(in_order("allocateMemory createSurface mapMemory unmapMemory") && never("apply-commit"));
    CHECK(mem_used == 3 && alloc_log[2].type == NVKMS_KAPI_ALLOCATION_TYPE_SCANOUT &&
          alloc_log[2].layout == NvKmsSurfaceMemoryLayoutPitch && alloc_log[2].useVideoMemory &&
          alloc_log[2].size == 7680ull * H);
    CHECK(surface_log[2].format == NvKmsSurfaceMemoryFormatX8B8G8R8 && surface_log[2].planes[0].pitch == 7680 &&
          surface_log[2].width == W && surface_log[2].height == H &&
          surface_log[2].planes[0].memory == (struct NvKmsKapiMemory *)&mem_pool[2]);
    CHECK(PIXEL(2, 0) == 0 && PIXEL(2, 7680u * H - 4) == 0);
    uint32_t a = r.image;
    uint32_t b = create_image(APP_FILE, APP);
    CHECK(b && b != a && image_count() == 2);

    /* Bad requests. */
    CHECK(nvrm_display_scanout(APP_FILE, APP, &r, sizeof r - 1) == -N_ERR_INVALID);
    CHECK(scanout_op(APP_FILE, APP, 0, a) == NV_ERR_INVALID_ARGUMENT);
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_IMPORT + 1, a) == NV_ERR_INVALID_ARGUMENT);
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_PRESENT, 0x7777) == NV_ERR_INVALID_OBJECT_HANDLE);

    /* PRESENT while the system does not show the app directly: refused, nothing flipped. */
    calls[0] = 0;
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_PRESENT, a) == NV_ERR_INVALID_STATE);
    CHECK(never("apply-commit") && never("apply-check") && !scr.shown);
    /* Another app's images are not told apart from none, even while that app is shown. */
    screen_says(OTHER_APP, true, 100, 50);
    CHECK(scanout_op(OTHER_FILE, OTHER_APP, NVRM_SCANOUT_PRESENT, a) == NV_ERR_INVALID_OBJECT_HANDLE);
    CHECK(scanout_op(OTHER_FILE, OTHER_APP, NVRM_SCANOUT_DESTROY, a) == NV_ERR_INVALID_OBJECT_HANDLE);
    CHECK(never("apply-commit") && never("destroySurface") && image_count() == 2);

    /* The system shows the app directly: a flip, committed only, of the head as it is (display,
       mode), the image on the primary layer and the pointer as the cursor at its hot spot. */
    screen_pointer_image(0x80000000u);
    screen_says(APP, true, 100, 50);
    calls[0] = 0;
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_PRESENT, a) == NV_OK);
    CHECK(in_order("flipPending-1 apply-commit") && never("apply-check"));
    const struct NvKmsKapiHeadRequestedConfig *h = head_cfg(1);
    CHECK(last_request.headsMask == 0x2 && h->modeSetConfig.bActive && h->modeSetConfig.numDisplays == 1 &&
          h->modeSetConfig.displays[0] == 0x2 && h->modeSetConfig.mode.timings.refreshRate == 120000);
    CHECK(!h->flags.activeChanged && !h->flags.displaysChanged && !h->flags.modeChanged);
    const struct NvKmsKapiLayerRequestedConfig *p = &h->layerRequestedConfig[0];
    CHECK(p->flags.surfaceChanged && p->config.surface == SURF(2) && p->config.srcWidth == W &&
          p->config.srcHeight == H && p->config.dstWidth == W && p->config.dstHeight == H);
    CHECK(!h->layerRequestedConfig[1].flags.surfaceChanged);
    const struct NvKmsKapiCursorRequestedConfig *c = &h->cursorRequestedConfig;
    CHECK(c->flags.surfaceChanged && c->surface == SURF(1) &&
          c->compParams.compMode == NVKMS_COMPOSITION_BLENDING_MODE_NON_PREMULT_ALPHA && c->compParams.surfaceAlpha == 0xff);
    CHECK(c->flags.dstXYChanged && c->dstX == 92 && c->dstY == 42);
    /* The pointer's pixels, row by row into the cursor's pitch. */
    CHECK(PIXEL(1, 0) == 0x80000000u && PIXEL(1, 256 * 31 + 4 * 31) == 0x80000000u + 32 * 32 - 1 &&
          PIXEL(1, 256 * 1) == 0x80000000u + 32);
    CHECK(scr.shown && scr.shown->number == a && !scr.leaving && strstr(logs, "app 77's images are shown directly"));

    /* The next present waits until that flip has completed: one flip waiting at a time. The
       image it took off may be written again once the next PRESENT has returned. */
    pending_polls = 2;
    calls[0] = 0;
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_PRESENT, b) == NV_OK);
    CHECK(in_order("flipPending-1 flipPending-1 flipPending-1 apply-commit"));
    CHECK(head_cfg(1)->layerRequestedConfig[0].config.surface == SURF(3) &&
          !head_cfg(1)->cursorRequestedConfig.flags.surfaceChanged && !head_cfg(1)->cursorRequestedConfig.flags.dstXYChanged);
    CHECK(scr.shown->number == b && scr.leaving && scr.leaving->number == a);

    /* The pointer moves: the cursor alone; its image changes: the pixels, no flip. */
    screen_says(APP, true, 400, 300);
    calls[0] = 0;
    follow_screen();
    CHECK(in_order("apply-commit") && never("apply-check"));
    c = &head_cfg(1)->cursorRequestedConfig;
    CHECK(c->flags.dstXYChanged && !c->flags.surfaceChanged && c->dstX == 392 && c->dstY == 292);
    CHECK(!head_cfg(1)->layerRequestedConfig[0].flags.surfaceChanged && !scr.leaving);
    screen_pointer_image(0x40000000u);
    calls[0] = 0;
    follow_screen();
    CHECK(never("apply-commit") && PIXEL(1, 0) == 0x40000000u);
    /* The pointer hidden: the cursor off. */
    screen_says(APP, false, 400, 300);
    calls[0] = 0;
    follow_screen();
    CHECK(in_order("apply-commit"));
    c = &head_cfg(1)->cursorRequestedConfig;
    CHECK(c->flags.surfaceChanged && c->surface == NULL && !scr.cursor_on);
    screen_says(APP, true, 400, 300);
    follow_screen();
    CHECK(scr.cursor_on && head_cfg(1)->cursorRequestedConfig.surface == SURF(1));

    /* The system no longer shows the app directly: the desktop again, without the cursor. */
    screen_says(0, true, 400, 300);
    calls[0] = 0;
    follow_screen();
    CHECK(in_order("apply-commit"));
    h = head_cfg(1);
    CHECK(h->layerRequestedConfig[0].flags.surfaceChanged && h->layerRequestedConfig[0].config.surface == SURF(0));
    CHECK(h->cursorRequestedConfig.flags.surfaceChanged && h->cursorRequestedConfig.surface == NULL);
    CHECK(!scr.shown && scr.leaving && scr.leaving->number == b && strstr(logs, "the desktop is shown again"));
    calls[0] = 0;
    follow_screen();
    CHECK(never("apply-commit"));

    /* DESTROY of the image the last flip took off: its completion awaited, no flip. */
    pending_polls = 1;
    calls[0] = 0;
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_DESTROY, b) == NV_OK);
    CHECK(in_order("flipPending-1 flipPending-1 destroySurface freeMemory") && never("apply-commit"));
    CHECK(!scr.leaving && image_count() == 1 && scanout_op(APP_FILE, APP, NVRM_SCANOUT_PRESENT, b) ==
          NV_ERR_INVALID_OBJECT_HANDLE);

    /* DESTROY of the image shown: the desktop first, the flip completed, then freed. */
    screen_says(APP, true, 100, 50);
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_PRESENT, a) == NV_OK);
    calls[0] = 0;
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_DESTROY, a) == NV_OK);
    CHECK(in_order("flipPending-1 apply-commit flipPending-1 destroySurface freeMemory"));
    CHECK(head_cfg(1)->layerRequestedConfig[0].config.surface == SURF(0) && !scr.shown && !scr.leaving &&
          !scr.images);

    /* IMPORT: only the image's app; NVKMS's memory shared with the client named for DUP_OBJECT,
       duplicated by the app's own RM file, the share revoked at once. */
    uint32_t i4 = create_image(APP_FILE, APP); /* memory 4 */
    struct nvrm_scanout_request imp = { .op = NVRM_SCANOUT_IMPORT, .image = i4, .client = 0xc100, .parent = 0xc101 };
    calls[0] = 0;
    CHECK(scanout(OTHER_FILE, OTHER_APP, &imp) == NV_ERR_INVALID_OBJECT_HANDLE);
    CHECK(never("rm-share") && never("rm-dup") && imp.object == 0);
    CHECK(scanout(APP_FILE, APP, &imp) == NV_OK);
    CHECK(in_order("rm-share rm-dup rm-unshare") && shares == 2 && !shared_with);
    CHECK(last_share[0].hObject == NVKMS_MEMORY(4) && last_share[0].sharePolicy.target == 0xc100 &&
          !(last_share[0].sharePolicy.action & RS_SHARE_ACTION_FLAG_REVOKE));
    CHECK(last_share[1].hObject == NVKMS_MEMORY(4) && last_share[1].sharePolicy.target == 0xc100 &&
          (last_share[1].sharePolicy.action & RS_SHARE_ACTION_FLAG_REVOKE));
    CHECK(dup_file == APP_FILE && last_dup.hClient == 0xc100 && last_dup.hParent == 0xc101 && last_dup.hObject == 0 &&
          last_dup.hClientSrc == NVKMS_CLIENT && last_dup.hObjectSrc == NVKMS_MEMORY(4));
    CHECK(imp.object == 0xd0000001u);
    /* The handle the app chose is kept; RM's refusal is the reply, the share revoked all the same. */
    imp.object = 0x5555;
    shares = 0;
    CHECK(scanout(APP_FILE, APP, &imp) == NV_OK && imp.object == 0x5555 && last_dup.hObject == 0x5555);
    dup_result = NV_ERR_INSUFFICIENT_PERMISSIONS;
    shares = 0;
    calls[0] = 0;
    CHECK(scanout(APP_FILE, APP, &imp) == NV_ERR_INSUFFICIENT_PERMISSIONS);
    CHECK(in_order("rm-share rm-dup rm-unshare") && shares == 2 && !shared_with);
    dup_result = 0;

    /* The connection's end: its images go, the one shown off the screen first; another
       connection's stay. */
    uint32_t i5 = create_image(APP_FILE, APP);           /* memory 5 */
    uint32_t i6 = create_image(OTHER_FILE, OTHER_APP);   /* memory 6 */
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_PRESENT, i5) == NV_OK);
    unsigned destroyed = surf_destroyed;
    calls[0] = 0;
    nvrm_display_connection_end(APP_FILE);
    CHECK(in_order("apply-commit flipPending-1 destroySurface freeMemory destroySurface freeMemory"));
    CHECK(surf_destroyed == destroyed + 2 && image_count() == 1 && scr.images->number == i6 && !scr.shown);
    CHECK(head_cfg(1)->layerRequestedConfig[0].config.surface == SURF(0));
    (void)i4;

    /* stop: the threads first, the remaining images and the cursor after ownership is released. */
    calls[0] = 0;
    nvrm_display_stop();
    CHECK(in_order("join join give-boot unregisterVblank-1 close41 releaseOwnership destroySurface freeMemory "
                   "destroySurface unmapMemory freeMemory destroySurface unmapMemory freeMemory freeDevice"));
    CHECK(mem_freed == mem_used && surf_destroyed == surf_used && !scr.images);
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_CREATE, 0) == NV_ERR_NOT_SUPPORTED);
}

/* Displays coming and going under an app's image: the mode set shows the desktop; the cursor
   follows the pointer in the mode's pixels when the screen is scaled. */
static void direct_scanout_and_displays(void) {
    reset();
    CHECK(nvrm_display_start());
    uint32_t a = create_image(APP_FILE, APP);
    screen_pointer_image(1);
    screen_says(APP, true, 100, 50);
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_PRESENT, a) == NV_OK && scr.shown);

    displays[1].connected = false; /* 0x2 unplugged; 0x4, 3840x2160, takes the desktop */
    rescan();
    CHECK(!scr.shown && !scr.leaving && !scr.cursor_on && scr.display == 0x4);
    CHECK(head_cfg(0)->layerRequestedConfig[0].config.surface == SURF(0));
    calls[0] = 0;
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_PRESENT, a) == NV_OK);
    const struct NvKmsKapiHeadRequestedConfig *h = head_cfg(0);
    CHECK(last_request.headsMask == 0x1 && h->modeSetConfig.displays[0] == 0x4 &&
          h->modeSetConfig.mode.timings.hVisible == 3840 && !h->flags.modeChanged);
    CHECK(h->layerRequestedConfig[0].config.surface == SURF(2) && h->layerRequestedConfig[0].config.dstWidth == 3840);
    CHECK(h->cursorRequestedConfig.dstX == 184 && h->cursorRequestedConfig.dstY == 84);

    /* No display: nothing to present on. */
    displays[2].connected = false;
    rescan();
    calls[0] = 0;
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_PRESENT, a) == NV_ERR_INVALID_STATE && never("apply-commit"));
    nvrm_display_stop();
    CHECK(mem_freed == mem_used && surf_destroyed == surf_used);
}

static void no_direct_scanout(void) {
    /* No cursor the size of the system's pointer: the surface given without N_SCANOUT_DIRECT. */
    reset();
    cursor_caps_missing = true;
    CHECK(nvrm_display_start());
    CHECK(given_desc.format == N_SCANOUT_XRGB8888 && never("give-surface-direct") && mem_used == 1);
    CHECK(!scr.direct && !scr.state_running && strstr(logs, "cannot show apps' X8B8G8R8 images"));
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_CREATE, 0) == NV_ERR_NOT_SUPPORTED && mem_used == 1);
    nvrm_display_connection_end(APP_FILE);
    calls[0] = 0;
    nvrm_display_stop();
    CHECK(in_order("join give-boot") && never("join join") && mem_freed == 1);

    /* The system refuses N_SCANOUT_DIRECT: given again without it, the cursor freed. */
    reset();
    direct_refused = true;
    CHECK(nvrm_display_start());
    CHECK(in_order("give-surface-direct destroySurface unmapMemory freeMemory give-surface registerVblank-1 thread"));
    CHECK(given_desc.format == N_SCANOUT_XRGB8888 && !scr.direct && !scr.state_running && mem_freed == 1);
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_CREATE, 0) == NV_ERR_NOT_SUPPORTED);
    nvrm_display_stop();
    CHECK(mem_freed == mem_used && surf_destroyed == surf_used);

    /* The system does not say what the screen shows: logged once, nothing presented. */
    reset();
    CHECK(nvrm_display_start());
    uint32_t a = create_image(APP_FILE, APP);
    screen_result = -N_ERR_DENIED;
    follow_screen();
    CHECK(strstr(logs, "the system does not say what the screen shows"));
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_PRESENT, a) == NV_ERR_INVALID_STATE && !scr.shown);
    nvrm_display_stop();
}

int main(void) {
    bring_up_and_tear_down();
    refresh_choice();
    nothing_given_on_failure();
    boot_displays_unknown();
    monitors_come_and_go();
    direct_scanout();
    direct_scanout_and_displays();
    no_direct_scanout();
    if (failures) {
        printf("log:\n%s", logs);
        printf("nvrm_display_hosttest: %d failures\n", failures);
        return 1;
    }
    printf("nvrm_display_hosttest: ok\n");
    return 0;
}
