/* nvrm's screen (display.h): NVKMS's kernel API (KAPI) used for what nvidia-drm does with it on
 * Linux when it takes the console over, without DRM's objects around it.
 *
 *   allocateDevice -> grabOwnership -> declareEventInterest -> getDeviceResourcesInfo
 *   -> RM's boot displays (the ones the firmware lit) -> getDisplays, each display's connector,
 *      static and dynamic information (connection, EDID) -> the display the firmware lit
 *   -> getDisplayMode: the firmware's width and height, refresh nearest the monitor's preferred
 *      mode -> validateDisplayMode
 *   -> allocateMemory (video memory, pitch linear, for scanout) -> mapMemory (RM's kernel
 *      mapping, through BAR1) -> createSurface (X8R8G8B8)
 *   -> the boot screen's pixels copied into it, so the switch shows no black or stale frame
 *   -> applyModeSetConfig (checked, then committed) -> the flip has completed
 *   -> n_pci_give_screen: the system draws the desktop into the surface from then on
 *   -> registerVblankIntrCallback: each vertical blank goes to n_pci_screen_vblank
 *
 * The surface is given to the system as a view of BAR1 over the same pages RM mapped
 * (os_pci.c records where its kernel mappings lie on the bus). It stays given while monitors
 * come and go: a display that goes away has its head turned off, and the first display that
 * is connected (the firmware's first, then any) gets the same procedure again.
 *
 * nvrm_display_stop undoes it in the opposite order: the boot screen back to the system, then
 * releaseOwnership, with which NVKMS shows the firmware's framebuffer console again
 * (nvkms-console-restore.c), then the surface and the device.
 *
 * Threads: KAPI's event callback runs on NVKMS's work thread and only wakes this file's
 * display thread, which does the KAPI calls of a reconnection (they take NVKMS's lock). The
 * vertical-blank callback runs under NVKMS's lock on its timer thread and only counts and
 * tells the system. Before the display thread starts and after it is joined, the calling
 * thread (nvrm's main thread) is the only one that touches the state below.
 */
#include "display.h"
#include "nvkms_os.h"
#include "nvrm_os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "nvkms-kapi.h"
#include <nvUnixVersion.h>
#include <nv-kernel-rmapi-ops.h>
#include <os/nv_memory_type.h>
#include <class/cl0000.h>
#include <class/cl0073.h>
#include <class/cl0080.h>
#include <ctrl/ctrl0000/ctrl0000gpu.h>
#include <ctrl/ctrl0073/ctrl0073system.h>

#define LOG "NVRM display: "
#define FLIP_TIMEOUT_MS 3000         /* a committed mode set whose flip has not completed by then failed */
#define DEFAULT_REFRESH 60000u       /* RRx1k, when the monitor names no preferred mode */
#define PAGE_BYTES 4096u

static struct NvKmsKapiFunctionsTable kapi = { .versionString = NV_VERSION_STRING };

static struct {
    struct NvKmsKapiDevice *device;
    bool owner;                      /* grabOwnership succeeded */
    bool events;                     /* declareEventInterest succeeded */
    struct NvKmsKapiDeviceResourcesInfo res;
    uint32_t width, height;          /* the boot screen's, and so the surface's */
    uint32_t boot_mask;              /* RM's boot displays, as display handles (dpy id bits) */

    struct NvKmsKapiMemory *memory;
    void *pixels;                    /* RM's kernel mapping of memory (KAPI mapMemory) */
    struct NvKmsKapiSurface *surface;
    uint32_t pitch;
    uint64_t bytes;
    n_handle view;                   /* the surface as a view of BAR1, given to the system */
    uint64_t view_offset;            /* where the surface starts in the view */
    bool given;                      /* atomic: the system draws into the surface */

    NvKmsKapiDisplay display;        /* the display nvrm lit, 0 none */
    uint32_t head;                   /* its head, while head_on */
    bool head_on;
    struct NvKmsKapiVblankIntrCallback *vblank;
    uint32_t vblank_head;
    uint64_t vblanks;                /* atomic */
    bool vblank_refused;             /* atomic: the system refused one (logged once) */

    bool sync_ready;                 /* lock and wake are initialised */
    mtx_t lock;
    cnd_t wake;
    bool rescan, stop;               /* under lock */
    thrd_t thread;
    bool thread_running;
} scr = { .view = -1 };

/* Large KAPI structures, used by one thread at a time (see the top of the file). */
static struct NvKmsKapiDynamicDisplayParams dyn;
static struct NvKmsKapiRequestedModeSetConfig request;
static struct NvKmsKapiModeSetReplyConfig reply;

static void mode_text(const struct NvKmsKapiDisplayMode *m, char *out, size_t cap) {
    snprintf(out, cap, "%ux%u at %u.%03u Hz", (unsigned)m->timings.hVisible, (unsigned)m->timings.vVisible,
             (unsigned)(m->timings.refreshRate / 1000), (unsigned)(m->timings.refreshRate % 1000));
}

/* ------------------------------------------------------------------------------------------
 * The displays the firmware lit: RM's boot displays, which NVKMS reads for its console
 * restore (ProbeBootDisplays) but KAPI does not pass on. A short-lived RM client of nvrm's
 * own (root -> device -> display common) asks for them. The mask's bits are dpy ids, the
 * values KAPI's display handles have.
 * ---------------------------------------------------------------------------------------- */
#define RM_DEVICE_HANDLE 0x00d10080u
#define RM_DISPLAY_COMMON_HANDLE 0x00d10073u

static NvU32 rm_alloc(NvHandle client, NvHandle parent, NvHandle object, NvU32 cls, void *params) {
    nvidia_kernel_rmapi_ops_t ops = { .op = NV04_ALLOC };
    ops.params.alloc.hRoot = client;
    ops.params.alloc.hObjectParent = parent;
    ops.params.alloc.hObjectNew = object;
    ops.params.alloc.hClass = cls;
    ops.params.alloc.pAllocParms = NV_PTR_TO_NvP64(params);
    rm_kernel_rmapi_op(nvrm_dev.sp, &ops);
    return ops.params.alloc.status;
}

static NvU32 rm_control(NvHandle client, NvHandle object, NvU32 cmd, void *params, NvU32 size) {
    nvidia_kernel_rmapi_ops_t ops = { .op = NV04_CONTROL };
    ops.params.control.hClient = client;
    ops.params.control.hObject = object;
    ops.params.control.cmd = cmd;
    ops.params.control.params = NV_PTR_TO_NvP64(params);
    ops.params.control.paramsSize = size;
    rm_kernel_rmapi_op(nvrm_dev.sp, &ops);
    return ops.params.control.status;
}

static void rm_free_client(NvHandle client) {
    nvidia_kernel_rmapi_ops_t ops = { .op = NV01_FREE };
    ops.params.free.hRoot = client;
    ops.params.free.hObjectParent = client;
    ops.params.free.hObjectOld = client;
    rm_kernel_rmapi_op(nvrm_dev.sp, &ops);
}

static uint32_t boot_displays(void) {
    NvHandle client = 0;
    NvU32 st = rm_alloc(0, 0, 0, NV01_ROOT, &client);
    if (st != NVOS_STATUS_SUCCESS || !client) {
        nvrm_log(LOG "cannot open an RM client to read the boot displays (0x%x)", (unsigned)st);
        return 0;
    }
    const char *step = "GPU_GET_ID_INFO_V2";
    NV0000_CTRL_GPU_GET_ID_INFO_V2_PARAMS id = { .gpuId = nvrm_dev.nv.gpu_id };
    NV0073_CTRL_SYSTEM_GET_BOOT_DISPLAYS_PARAMS boot = { 0 };
    st = rm_control(client, client, NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2, &id, sizeof id);
    if (st == NVOS_STATUS_SUCCESS) {
        step = "NV01_DEVICE_0";
        NV0080_ALLOC_PARAMETERS device = { .deviceId = id.deviceInstance, .hClientShare = client };
        st = rm_alloc(client, client, RM_DEVICE_HANDLE, NV01_DEVICE_0, &device);
    }
    if (st == NVOS_STATUS_SUCCESS) {
        step = "NV04_DISPLAY_COMMON";
        st = rm_alloc(client, RM_DEVICE_HANDLE, RM_DISPLAY_COMMON_HANDLE, NV04_DISPLAY_COMMON, NULL);
    }
    if (st == NVOS_STATUS_SUCCESS) {
        step = "SYSTEM_GET_BOOT_DISPLAYS";
        st = rm_control(client, RM_DISPLAY_COMMON_HANDLE, NV0073_CTRL_CMD_SYSTEM_GET_BOOT_DISPLAYS, &boot,
                        sizeof boot);
    }
    rm_free_client(client);
    if (st != NVOS_STATUS_SUCCESS) {
        nvrm_log(LOG "cannot read the boot displays from RM: %s returned 0x%x", step, (unsigned)st);
        return 0;
    }
    return boot.bootDisplayMask;
}

/* ------------------------------------------------------------------------------------------
 * Displays and modes
 * ---------------------------------------------------------------------------------------- */

/* Manufacturer, product code and the monitor name descriptor of an EDID's base block. */
static void edid_text(const uint8_t *e, size_t n, char *out, size_t cap) {
    out[0] = 0;
    if (n < 128 || cap < 16) return;
    unsigned id = (unsigned)e[8] << 8 | e[9];
    char name[14] = "";
    for (unsigned d = 54; d + 18 <= 126; d += 18) {
        const uint8_t *b = e + d;
        if (b[0] || b[1] || b[2] || b[3] != 0xfc) continue;
        size_t k = 0;
        for (unsigned j = 5; j < 18 && b[j] != 0x0a; j++) name[k++] = b[j] >= 0x20 && b[j] < 0x7f ? (char)b[j] : '?';
        while (k && name[k - 1] == ' ') k--;
        name[k] = 0;
        break;
    }
    snprintf(out, cap, "%c%c%c%04x%s%s", '@' + (id >> 10 & 31), '@' + (id >> 5 & 31), '@' + (id & 31),
             (unsigned)e[10] | (unsigned)e[11] << 8, name[0] ? " " : "", name);
}

/* The display to light and its head (the lowest of those it can use): a connected one the
   firmware lit, else the first connected one. 0 when none is connected. verbose logs every
   display. */
static NvKmsKapiDisplay choose_display(uint32_t *head, bool verbose) {
    NvU32 count = 0;
    if (!kapi.getDisplays(scr.device, &count, NULL)) {
        nvrm_log(LOG "NVKMS cannot list the displays");
        return 0;
    }
    NvKmsKapiDisplay *list = count ? calloc(count, sizeof *list) : NULL;
    if (count && (!list || !kapi.getDisplays(scr.device, &count, list))) {
        nvrm_log(LOG "NVKMS cannot list the displays");
        free(list);
        return 0;
    }
    NvKmsKapiDisplay chosen = 0, first = 0;
    uint32_t chosen_head = 0, first_head = 0;
    unsigned boot_connected = 0;
    for (NvU32 i = 0; i < count; i++) {
        struct NvKmsKapiStaticDisplayInfo info = { 0 };
        struct NvKmsKapiConnectorInfo connector = { 0 };
        if (!kapi.getStaticDisplayInfo(scr.device, list[i], &info)) continue;
        bool named = kapi.getConnectorInfo(scr.device, info.connectorHandle, &connector);
        memset(&dyn, 0, sizeof dyn);
        dyn.handle = list[i];
        bool connected = kapi.getDynamicDisplayInfo(scr.device, &dyn) && dyn.connected;
        bool boot = (list[i] & scr.boot_mask) != 0;
        if (verbose) {
            char monitor[40];
            edid_text(dyn.edid.buffer, connected ? dyn.edid.bufferSize : 0, monitor, sizeof monitor);
            nvrm_log(LOG "display 0x%08x: %s-%u%s%s, heads 0x%x, %s%s%s%s", (unsigned)list[i],
                     named ? NvKmsConnectorTypeString(connector.type) : "?",
                     named ? (unsigned)connector.physicalIndex : 0u, info.isDpMST ? " (DP MST)" : "",
                     info.internal ? " (internal)" : "", (unsigned)info.headMask,
                     connected ? "connected" : "not connected", monitor[0] ? ", " : "", monitor,
                     boot ? ", lit by the firmware" : "");
        }
        if (!connected || !info.headMask) continue;
        uint32_t h = (uint32_t)__builtin_ctz(info.headMask);
        if (!first) { first = list[i]; first_head = h; }
        if (boot) {
            if (!chosen) { chosen = list[i]; chosen_head = h; }
            boot_connected++;
        }
    }
    free(list);
    if (!chosen && first) {
        if (verbose && scr.boot_mask)
            nvrm_log(LOG "no display the firmware lit is connected; using display 0x%08x", (unsigned)first);
        chosen = first;
        chosen_head = first_head;
    }
    if (verbose && boot_connected > 1)
        nvrm_log(LOG "the firmware lit %u connected displays; nvrm shows the desktop on display 0x%08x only",
                 boot_connected, (unsigned)chosen);
    if (verbose && !chosen) nvrm_log(LOG "no display is connected");
    *head = chosen_head;
    return chosen;
}

static uint32_t refresh_distance(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }

/* The mode for the surface on display: the surface's width and height, the refresh nearest
   the monitor's preferred mode (DEFAULT_REFRESH when it names none; the higher of two equally
   near), the first of those NVKMS validates. With allow_scaled, a display without such a mode
   gets its preferred mode, the surface scaled to it (*scaled). */
static bool pick_mode(NvKmsKapiDisplay display, bool allow_scaled, struct NvKmsKapiDisplayMode *out, bool *scaled) {
    struct NvKmsKapiDisplayMode mode, preferred;
    struct NvKmsKapiDisplayMode *same = NULL;
    size_t count = 0, cap = 0;
    bool have_preferred = false;
    unsigned listed = 0;
    for (NvU32 index = 0;; index++) {
        NvBool valid = NV_FALSE, is_preferred = NV_FALSE;
        memset(&mode, 0, sizeof mode);
        int more = kapi.getDisplayMode(scr.device, display, index, &mode, &valid, &is_preferred);
        if (more < 0) {
            nvrm_log(LOG "NVKMS cannot list the modes of display 0x%08x", (unsigned)display);
            free(same);
            return false;
        }
        if (more == 0) break;
        if (!valid || mode.timings.flags.interlaced || mode.timings.flags.doubleScan) continue;
        listed++;
        if (is_preferred && !have_preferred) { preferred = mode; have_preferred = true; }
        if (mode.timings.hVisible != scr.width || mode.timings.vVisible != scr.height) continue;
        if (count == cap) {
            size_t grown_cap = cap ? cap * 2 : 16;
            struct NvKmsKapiDisplayMode *grown = realloc(same, grown_cap * sizeof *grown);
            if (!grown) break;
            same = grown;
            cap = grown_cap;
        }
        same[count++] = mode;
    }
    uint32_t want = have_preferred ? preferred.timings.refreshRate : DEFAULT_REFRESH;
    /* Nearest first (insertion sort; a monitor lists a few dozen modes). */
    for (size_t i = 1; i < count; i++) {
        struct NvKmsKapiDisplayMode m = same[i];
        uint32_t d = refresh_distance(m.timings.refreshRate, want);
        size_t j = i;
        while (j > 0) {
            uint32_t dj = refresh_distance(same[j - 1].timings.refreshRate, want);
            if (dj < d || (dj == d && same[j - 1].timings.refreshRate >= m.timings.refreshRate)) break;
            same[j] = same[j - 1];
            j--;
        }
        same[j] = m;
    }
    char text[48], pref_text[48] = "none";
    if (have_preferred) mode_text(&preferred, pref_text, sizeof pref_text);
    bool found = false;
    for (size_t i = 0; i < count && !found; i++) {
        if (!kapi.validateDisplayMode(scr.device, display, &same[i])) continue;
        *out = same[i];
        *scaled = false;
        found = true;
    }
    if (!found && allow_scaled && have_preferred && kapi.validateDisplayMode(scr.device, display, &preferred)) {
        *out = preferred;
        *scaled = true;
        found = true;
    }
    if (found) {
        mode_text(out, text, sizeof text);
        nvrm_log(LOG "display 0x%08x: mode %s%s (%u modes, %zu of %ux%u; preferred %s)", (unsigned)display, text,
                 *scaled ? ", the desktop scaled to it" : "", listed, count, (unsigned)scr.width,
                 (unsigned)scr.height, pref_text);
    } else {
        nvrm_log(LOG "display 0x%08x: no valid %ux%u mode (%u modes, %zu of that size; preferred %s)",
                 (unsigned)display, (unsigned)scr.width, (unsigned)scr.height, listed, count, pref_text);
    }
    free(same);
    return found;
}

/* ------------------------------------------------------------------------------------------
 * The surface
 * ---------------------------------------------------------------------------------------- */
static int bar_index_of(uint64_t phys) {
    for (int i = 0; i < (int)(sizeof nvrm_dev.info.bar / sizeof nvrm_dev.info.bar[0]); i++) {
        const struct n_pci_bar *b = &nvrm_dev.info.bar[i];
        if ((b->flags & N_PCI_BAR_MEMORY) && b->size && phys >= b->address && phys - b->address < b->size) return i;
    }
    return -1;
}

static bool surface_create(void) {
    if (!scr.res.caps.hasVideoMemory) {
        nvrm_log(LOG "the GPU has no video memory for a surface the system can draw into");
        return false;
    }
    uint64_t align = scr.res.caps.pitchAlignment ? scr.res.caps.pitchAlignment : 256;
    uint64_t pitch = ((uint64_t)scr.width * 4 + align - 1) / align * align;
    uint64_t bytes = (pitch * scr.height + PAGE_BYTES - 1) / PAGE_BYTES * PAGE_BYTES;
    if (pitch > UINT32_MAX) return false;
    scr.pitch = (uint32_t)pitch;
    scr.bytes = bytes;

    NvU8 compressible = 0;
    struct NvKmsKapiAllocateMemoryParams params = {
        .layout = NvKmsSurfaceMemoryLayoutPitch,
        .type = NVKMS_KAPI_ALLOCATION_TYPE_SCANOUT,
        .size = bytes,
        .noDisplayCaching = NV_TRUE,
        .useVideoMemory = NV_TRUE,
        .compressible = &compressible,
    };
    scr.memory = kapi.allocateMemory(scr.device, &params);
    if (!scr.memory) {
        nvrm_log(LOG "cannot allocate %llu KiB of video memory for the surface", (unsigned long long)(bytes >> 10));
        return false;
    }
    if (!kapi.mapMemory(scr.device, scr.memory, NVKMS_KAPI_MAPPING_TYPE_KERNEL, &scr.pixels)) {
        scr.pixels = NULL;
        nvrm_log(LOG "RM cannot map the surface");
        return false;
    }
    /* The system gets the pages RM's mapping shows; they must be one stretch of a BAR. */
    uint64_t phys = 0, last = 0;
    if (!nvrm_kernel_mapping_phys(scr.pixels, &phys) ||
        !nvrm_kernel_mapping_phys((const uint8_t *)scr.pixels + bytes - 1, &last) || last - phys != bytes - 1) {
        nvrm_log(LOG "RM's mapping of the surface is not one stretch of a BAR");
        return false;
    }
    int bar = bar_index_of(phys);
    scr.view = nvrm_bar_view(phys, bytes, NV_MEMORY_WRITECOMBINED, N_MEMORY_READ_WRITE, &scr.view_offset);
    if (scr.view < 0) {
        nvrm_log(LOG "cannot make a view of BAR%d at 0x%llx for the system: %s", bar, (unsigned long long)phys,
                 n_result_text(scr.view));
        scr.view = -1;
        return false;
    }

    struct NvKmsKapiCreateSurfaceParams surface = { 0 };
    surface.planes[0].memory = scr.memory;
    surface.planes[0].offset = 0;
    surface.planes[0].pitch = scr.pitch;
    surface.width = scr.width;
    surface.height = scr.height;
    surface.format = NvKmsSurfaceMemoryFormatX8R8G8B8;
    scr.surface = kapi.createSurface(scr.device, &surface);
    if (!scr.surface) {
        nvrm_log(LOG "NVKMS cannot make a %ux%u X8R8G8B8 surface", (unsigned)scr.width, (unsigned)scr.height);
        return false;
    }
    nvrm_log(LOG "surface %ux%u, pitch %u, %llu KiB of video memory at BAR%d offset 0x%llx (bus 0x%llx)",
             (unsigned)scr.width, (unsigned)scr.height, (unsigned)scr.pitch, (unsigned long long)(bytes >> 10), bar,
             (unsigned long long)(bar >= 0 ? phys - nvrm_dev.info.bar[bar].address : 0), (unsigned long long)phys);
    return true;
}

/* What the boot screen shows, into the surface (black when it cannot be read). */
static void copy_boot_screen(void) {
    const struct n_pci_function *g = &nvrm_dev.info;
    uint8_t *dst = scr.pixels;
    uint64_t row = (uint64_t)scr.width * 4;
    uint64_t src_bytes = (uint64_t)g->framebuffer_pitch * (scr.height - 1) + row;
    uint64_t off = 0;
    /* The system draws the boot screen write-combined; a view of it must be too (C42). */
    n_handle view = nvrm_bar_view(g->framebuffer_address, src_bytes, NV_MEMORY_WRITECOMBINED, N_MEMORY_READ, &off);
    void *map = NULL;
    size_t map_bytes = (size_t)((off + src_bytes + PAGE_BYTES - 1) / PAGE_BYTES * PAGE_BYTES);
    n_result r = view < 0 ? view : n_shared_map(view, map_bytes, N_MEMORY_READ, &map);
    if (view >= 0) n_handle_close(view);
    if (r < 0) {
        nvrm_log(LOG "cannot read the boot screen (%s); the surface starts black", n_result_text(r));
        for (uint32_t y = 0; y < scr.height; y++) memset(dst + (uint64_t)y * scr.pitch, 0, (size_t)row);
    } else {
        const uint8_t *src = (const uint8_t *)map + off;
        for (uint32_t y = 0; y < scr.height; y++)
            memcpy(dst + (uint64_t)y * scr.pitch, src + (uint64_t)y * g->framebuffer_pitch, (size_t)row);
        n_memory_release(map, map_bytes);
    }
    /* The surface is mapped write-combined: drain it before the display engine reads it. */
    __asm__ volatile("sfence" ::: "memory");
}

/* ------------------------------------------------------------------------------------------
 * Mode setting
 * ---------------------------------------------------------------------------------------- */

/* Adds head to the request: display shown in mode with the surface on its primary layer
   (scaled to the mode), its other layers empty; display 0 turns the head off. */
static void head_request(uint32_t head, NvKmsKapiDisplay display, const struct NvKmsKapiDisplayMode *mode) {
    struct NvKmsKapiHeadRequestedConfig *h = &request.headRequestedConfig[head];
    uint32_t layers = scr.res.numLayers[head] < NVKMS_KAPI_LAYER_MAX ? scr.res.numLayers[head] : NVKMS_KAPI_LAYER_MAX;
    request.headsMask |= 1u << head;
    memset(h, 0, sizeof *h);
    h->flags.activeChanged = NV_TRUE;
    h->flags.displaysChanged = NV_TRUE;
    h->flags.modeChanged = NV_TRUE;
    for (uint32_t l = 0; l < NVKMS_KAPI_LAYER_MAX; l++) {
        h->layerRequestedConfig[l].config.csc = NVKMS_IDENTITY_CSC_MATRIX;
        if (l < layers) h->layerRequestedConfig[l].flags.surfaceChanged = NV_TRUE; /* no surface */
    }
    if (!display) return;

    h->modeSetConfig.bActive = NV_TRUE;
    h->modeSetConfig.numDisplays = 1;
    h->modeSetConfig.displays[0] = display;
    h->modeSetConfig.mode = *mode;
    h->modeSetConfig.colorimetry = NVKMS_OUTPUT_COLORIMETRY_DEFAULT;

    struct NvKmsKapiLayerRequestedConfig *primary = &h->layerRequestedConfig[NVKMS_KAPI_LAYER_PRIMARY_IDX];
    struct NvKmsKapiLayerConfig *c = &primary->config;
    c->surface = scr.surface;
    c->compParams.compMode = NVKMS_COMPOSITION_BLENDING_MODE_OPAQUE;
    c->minPresentInterval = 1;
    c->tearing = NV_FALSE;
    c->srcX = c->srcY = 0;
    c->srcWidth = (NvU16)scr.width;
    c->srcHeight = (NvU16)scr.height;
    c->dstX = c->dstY = 0;
    c->dstWidth = (NvU16)mode->timings.hVisible;
    c->dstHeight = (NvU16)mode->timings.vVisible;
    c->inputColorSpace = NVKMS_INPUT_COLOR_SPACE_NONE;
    primary->flags.surfaceChanged = NV_TRUE;
    primary->flags.srcXYChanged = NV_TRUE;
    primary->flags.srcWHChanged = NV_TRUE;
    primary->flags.dstXYChanged = NV_TRUE;
    primary->flags.dstWHChanged = NV_TRUE;
}

/* The request, checked by NVKMS first and then committed. */
static bool apply(const char *what) {
    memset(&reply, 0, sizeof reply);
    if (!kapi.applyModeSetConfig(scr.device, &request, &reply, NV_FALSE)) {
        nvrm_log(LOG "NVKMS rejects %s", what);
        return false;
    }
    memset(&reply, 0, sizeof reply);
    if (!kapi.applyModeSetConfig(scr.device, &request, &reply, NV_TRUE) ||
        reply.flipResult != NV_KMS_FLIP_RESULT_SUCCESS) {
        nvrm_log(LOG "%s failed (flip result %d)", what, (int)reply.flipResult);
        return false;
    }
    return true;
}

static bool flip_completed(uint32_t head) {
    uint64_t until = nvrm_now_ns() + (uint64_t)FLIP_TIMEOUT_MS * 1000000u;
    for (;;) {
        NvBool pending = NV_FALSE;
        if (!kapi.getFlipPendingStatus(scr.device, head, NVKMS_KAPI_LAYER_PRIMARY_IDX, &pending)) {
            nvrm_log(LOG "NVKMS cannot say whether the flip on head %u completed", (unsigned)head);
            return false;
        }
        if (!pending) return true;
        if (nvrm_now_ns() >= until) {
            nvrm_log(LOG "the flip on head %u has not completed after %d ms", (unsigned)head, FLIP_TIMEOUT_MS);
            return false;
        }
        n_sleep_ms(1);
    }
}

/* display shows the surface in mode on head; the head nvrm used before is turned off when it
   is another one. */
static bool show_on(NvKmsKapiDisplay display, uint32_t head, const struct NvKmsKapiDisplayMode *mode) {
    memset(&request, 0, sizeof request);
    if (scr.head_on && scr.head != head) head_request(scr.head, 0, NULL);
    head_request(head, display, mode);
    if (!apply("the mode set")) return false;
    scr.head_on = true;
    scr.head = head;
    scr.display = display;
    return flip_completed(head);
}

static bool head_off(void) {
    if (!scr.head_on) return true;
    memset(&request, 0, sizeof request);
    head_request(scr.head, 0, NULL);
    if (!apply("turning the head off")) return false;
    scr.head_on = false;
    return true;
}

/* ------------------------------------------------------------------------------------------
 * Vertical blanks (under NVKMS's lock on its timer thread: nothing here may call KAPI)
 * ---------------------------------------------------------------------------------------- */
static void vblank_arrived(NvU64 data, NvU64 timestamp) {
    (void)data;
    if (!__atomic_load_n(&scr.given, __ATOMIC_ACQUIRE)) return;
    uint64_t count = __atomic_add_fetch(&scr.vblanks, 1, __ATOMIC_RELAXED);
    n_result r = n_pci_screen_vblank(nvrm_dev.pci, count, timestamp ? timestamp : nvrm_now_ns());
    if (r < 0 && !__atomic_exchange_n(&scr.vblank_refused, true, __ATOMIC_RELAXED))
        nvrm_log(LOG "the system refuses vertical blank %llu: %s", (unsigned long long)count, n_result_text(r));
}

static void vblank_on(uint32_t head) {
    scr.vblank = kapi.registerVblankIntrCallback(scr.device, head, vblank_arrived, 0);
    if (!scr.vblank) {
        nvrm_log(LOG "no vertical-blank interrupt on head %u; the desktop keeps its own frame period", (unsigned)head);
        return;
    }
    scr.vblank_head = head;
}

static void vblank_off(void) {
    if (!scr.vblank) return;
    kapi.unregisterVblankIntrCallback(scr.device, scr.vblank_head, scr.vblank);
    scr.vblank = NULL;
}

/* ------------------------------------------------------------------------------------------
 * Monitors coming and going (the display thread)
 * ---------------------------------------------------------------------------------------- */
static void kapi_event(const struct NvKmsKapiEvent *event) {
    if (event->type != NVKMS_EVENT_TYPE_DPY_CHANGED && event->type != NVKMS_EVENT_TYPE_DYNAMIC_DPY_CONNECTED) return;
    mtx_lock(&scr.lock);
    scr.rescan = true;
    cnd_signal(&scr.wake);
    mtx_unlock(&scr.lock);
}

/* The surface stays the system's throughout; only what shows it changes. */
static void rescan(void) {
    if (scr.display) {
        memset(&dyn, 0, sizeof dyn);
        dyn.handle = scr.display;
        if (kapi.getDynamicDisplayInfo(scr.device, &dyn) && dyn.connected) return;
        nvrm_log(LOG "display 0x%08x is disconnected; its head is turned off, the desktop is still drawn",
                 (unsigned)scr.display);
        vblank_off();
        head_off();
        scr.display = 0;
    }
    uint32_t head = 0;
    NvKmsKapiDisplay display = choose_display(&head, true);
    if (!display) return;
    struct NvKmsKapiDisplayMode mode;
    bool scaled = false;
    if (!pick_mode(display, true, &mode, &scaled)) return;
    if (!show_on(display, head, &mode)) {
        nvrm_log(LOG "display 0x%08x stays dark", (unsigned)display);
        return;
    }
    vblank_on(head);
    nvrm_log(LOG "the desktop is shown on display 0x%08x (head %u)", (unsigned)display, (unsigned)head);
}

static int display_thread(void *unused) {
    (void)unused;
    mtx_lock(&scr.lock);
    while (!scr.stop) {
        if (!scr.rescan) {
            cnd_wait(&scr.wake, &scr.lock);
            continue;
        }
        scr.rescan = false;
        mtx_unlock(&scr.lock);
        rescan();
        mtx_lock(&scr.lock);
    }
    mtx_unlock(&scr.lock);
    return 0;
}

/* ------------------------------------------------------------------------------------------
 * Start and stop
 * ---------------------------------------------------------------------------------------- */
bool nvrm_display_start(void) {
    const struct n_pci_function *g = &nvrm_dev.info;
    if (g->framebuffer_bar == N_PCI_NONE) {
        nvrm_log(LOG "the boot screen is not on this GPU; the screen is left as it is");
        return false;
    }
    if (g->framebuffer_bpp != 32 || !g->framebuffer_width || !g->framebuffer_height ||
        g->framebuffer_pitch < (uint64_t)g->framebuffer_width * 4 || g->framebuffer_width > UINT16_MAX ||
        g->framebuffer_height > UINT16_MAX) {
        nvrm_log(LOG "the boot screen (%ux%u, %u bits, pitch %u) is not a 32-bit screen nvrm can take over",
                 (unsigned)g->framebuffer_width, (unsigned)g->framebuffer_height, (unsigned)g->framebuffer_bpp,
                 (unsigned)g->framebuffer_pitch);
        return false;
    }
    scr.width = g->framebuffer_width;
    scr.height = g->framebuffer_height;

    kapi.versionString = NV_VERSION_STRING;
    if (!nvKmsKapiGetFunctionsTable(&kapi)) {
        nvrm_log(LOG "NVKMS's KAPI is version %s, nvrm's %s", kapi.versionString, NV_VERSION_STRING);
        return false;
    }
    if (mtx_init(&scr.lock, mtx_plain) != thrd_success) return false;
    if (cnd_init(&scr.wake) != thrd_success) {
        mtx_destroy(&scr.lock);
        return false;
    }
    scr.sync_ready = true;
    scr.rescan = scr.stop = false;

    const char *why = NULL;
    struct NvKmsKapiAllocateDeviceParams params = {
        .gpuId = nvrm_dev.nv.gpu_id,
        .migDevice = NO_MIG_DEVICE,
        .privateData = &scr,
        .eventCallback = kapi_event,
    };
    struct NvKmsKapiDisplayMode mode;
    bool scaled = false;
    uint32_t head = 0;
    NvKmsKapiDisplay display = 0;
    n_result given;

    scr.device = kapi.allocateDevice(&params);
    if (!scr.device) { why = "NVKMS cannot open the GPU"; goto fail; }
    scr.owner = kapi.grabOwnership(scr.device);
    if (!scr.owner) { why = "another NVKMS client owns the display"; goto fail; }
    scr.events = kapi.declareEventInterest(scr.device, 1u << NVKMS_EVENT_TYPE_DPY_CHANGED |
                                                       1u << NVKMS_EVENT_TYPE_DYNAMIC_DPY_CONNECTED);
    if (!scr.events) nvrm_log(LOG "NVKMS does not report displays coming and going; only the first one is set up");
    if (!kapi.getDeviceResourcesInfo(scr.device, &scr.res)) { why = "NVKMS cannot describe the display engine"; goto fail; }
    nvrm_log(LOG "%u heads, %u connectors, pitch alignment %u; boot screen %ux%u at 0x%llx, NVKMS's console "
             "surface at 0x%llx (%llu KiB)", (unsigned)scr.res.numHeads, (unsigned)scr.res.numConnectors,
             (unsigned)scr.res.caps.pitchAlignment, (unsigned)scr.width, (unsigned)scr.height,
             (unsigned long long)g->framebuffer_address, (unsigned long long)scr.res.vtFbBaseAddress,
             (unsigned long long)(scr.res.vtFbSize >> 10));
    if (!scr.res.vtFbSize)
        nvrm_log(LOG "NVKMS has no console surface: once nvrm stops, the boot screen is not shown again until restart");

    scr.boot_mask = boot_displays();
    nvrm_log(LOG "displays lit by the firmware: 0x%08x", (unsigned)scr.boot_mask);
    display = choose_display(&head, true);
    if (!display) { why = "no display is connected"; goto fail; }
    if (!pick_mode(display, false, &mode, &scaled)) { why = "the display has no mode of the boot screen's size"; goto fail; }
    if (!surface_create()) { why = "there is no surface for the system"; goto fail; }
    copy_boot_screen();
    if (!show_on(display, head, &mode)) { why = "the mode set did not complete"; goto fail; }

    struct n_scanout desc = {
        .width = scr.width,
        .height = scr.height,
        .pitch = scr.pitch,
        .format = N_SCANOUT_XRGB8888,
        .offset = scr.view_offset,
    };
    given = n_pci_give_screen(nvrm_dev.pci, scr.view, &desc);
    if (given < 0) {
        nvrm_log(LOG "the system does not take the surface: %s", n_result_text(given));
        why = "the system keeps drawing into the boot screen";
        goto fail;
    }
    __atomic_store_n(&scr.given, true, __ATOMIC_RELEASE);
    vblank_on(head);

    if (thrd_create(&scr.thread, display_thread, NULL) == thrd_success) scr.thread_running = true;
    else nvrm_log(LOG "cannot start the display thread; displays coming and going are not followed");
    nvrm_log(LOG "the desktop is drawn into nvrm's surface, shown on display 0x%08x (head %u)", (unsigned)display,
             (unsigned)head);
    return true;

fail:
    nvrm_log(LOG "%s; the screen stays with the firmware's framebuffer", why);
    nvrm_display_stop();
    return false;
}

void nvrm_display_stop(void) {
    if (scr.thread_running) {
        mtx_lock(&scr.lock);
        scr.stop = true;
        cnd_signal(&scr.wake);
        mtx_unlock(&scr.lock);
        thrd_join(scr.thread, NULL);
        scr.thread_running = false;
    }
    if (__atomic_load_n(&scr.given, __ATOMIC_ACQUIRE)) {
        n_result r = n_pci_give_screen(nvrm_dev.pci, N_SCANOUT_BOOT, NULL);
        __atomic_store_n(&scr.given, false, __ATOMIC_RELEASE);
        if (r < 0) nvrm_log(LOG "the system does not take the boot screen back: %s", n_result_text(r));
    }
    vblank_off();
    if (scr.view >= 0) n_handle_close(scr.view);
    scr.view = -1;
    if (scr.owner) {
        /* NVKMS shows its console surface (the firmware's framebuffer) on the boot displays
           again (nvkms.c RestoreConsole); without one it hands the display back to RM. */
        kapi.releaseOwnership(scr.device);
        nvrm_log(LOG "display ownership released; NVKMS restores the boot screen");
        scr.owner = false;
    }
    scr.head_on = false;
    scr.display = 0;
    if (scr.surface) kapi.destroySurface(scr.device, scr.surface);
    scr.surface = NULL;
    if (scr.pixels) kapi.unmapMemory(scr.device, scr.memory, NVKMS_KAPI_MAPPING_TYPE_KERNEL, scr.pixels);
    scr.pixels = NULL;
    if (scr.memory) kapi.freeMemory(scr.device, scr.memory);
    scr.memory = NULL;
    if (scr.events) kapi.declareEventInterest(scr.device, 0);
    scr.events = false;
    if (scr.device) kapi.freeDevice(scr.device);
    scr.device = NULL;
    if (scr.sync_ready) {
        cnd_destroy(&scr.wake);
        mtx_destroy(&scr.lock);
        scr.sync_ready = false;
    }
}
