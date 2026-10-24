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
 *   -> applyModeSetConfig (checked, then committed) -> the flip has completed (the completion
 *      notifier KAPI names in the commit, which nvkms_kapi_rm.c clears before it and reads)
 *   -> the pointer's cursor surface (32x32 A8R8G8B8, mapped): with it the surface is offered for
 *      direct scanout (N_SCANOUT_DIRECT)
 *   -> n_pci_give_screen: the system draws the desktop into the surface from then on (without
 *      N_SCANOUT_DIRECT when the system or the display engine cannot show windows directly)
 *   -> registerVblankIntrCallback: each vertical blank goes to n_pci_screen_vblank
 *
 * The surface is given to the system as a view of BAR1 over the same pages RM mapped
 * (os_pci.c records where its kernel mappings lie on the bus). It stays given while monitors
 * come and go: a display that goes away has its head turned off, and the first display that
 * is connected (the firmware's first, then any) gets the same procedure again.
 *
 * Direct scanout (NVRM_SCANOUT, include/nvrm_protocol.h): an app's images are video memory of
 * the screen's size (allocateMemory, createSurface X8B8G8R8) that its GPU work writes into
 * (IMPORT duplicates the memory into the app's RM client). While n_pci_screen_state names the
 * app, PRESENT flips the head's primary layer to an image (a flip, not a mode set: only the
 * surface changes), and the display engine draws the pointer as its cursor. When the state
 * names another app or none, the state thread flips back to the desktop's surface, which the
 * system has kept drawing into, and the cursor goes. An image is off the screen, and its last
 * flip away from it complete, before it is freed.
 *
 * nvrm_display_stop undoes it in the opposite order: the boot screen back to the system, then
 * releaseOwnership, with which NVKMS shows the firmware's framebuffer console again
 * (nvkms-console-restore.c), then the images, the cursor, the surface and the device.
 *
 * Threads: KAPI's event callback runs on NVKMS's work thread and only wakes this file's
 * display thread, which does the KAPI calls of a reconnection (they take NVKMS's lock). The
 * state thread follows n_pci_screen_state. Requests come on nvrm's client threads
 * (nvrm_display_scanout), between nvrm_display_start and nvrm_display_stop. All of them change
 * the head, the images and the KAPI structures below under head_lock, one at a time. The
 * vertical-blank callback runs under NVKMS's lock on its timer thread and only counts and
 * tells the system. Before the threads start and after they are joined, the calling thread
 * (nvrm's main thread) is the only one that touches the state below.
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
#include <nv_escape.h>
#include "nvrm_protocol.h"

#define LOG "NVRM display: "
#define FLIP_TIMEOUT_MS 3000         /* a committed mode set whose flip has not completed by then failed */
#define DEFAULT_REFRESH 60000u       /* RRx1k, when the monitor names no preferred mode */
#define PAGE_BYTES 4096u
#define STATE_WAIT_MS 1000           /* the state thread looks at its stop flag at least this often */
#define CURSOR_MIN_PITCH 256u        /* NVKMS's for a 32x32 cursor (nvEvoGetHeadSetControlCursorValueC3) */

static struct NvKmsKapiFunctionsTable kapi = { .versionString = NV_VERSION_STRING };

/* An app's image for direct scanout (NVRM_SCANOUT): the screen's size, scr.pitch apart. */
struct image {
    struct image *next;
    uint32_t number;                 /* nvrm-wide, never 0 */
    uint32_t app;                    /* the app that made it (n_app_self's number) */
    const void *connection;          /* the connection that made it (its RM file) */
    struct NvKmsKapiMemory *memory;
    struct NvKmsKapiSurface *surface;
    void *pixels;                    /* RM's kernel mapping of memory, which the system's view is of */
    n_result panic;                  /* n_pci_screen_image's number: the panic screen is drawn here too */
};

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
    struct NvKmsKapiDisplayMode mode;/* the head's mode, while head_on */
    struct NvKmsKapiVblankIntrCallback *vblank;
    uint32_t vblank_head;
    uint64_t vblanks;                /* atomic */
    bool vblank_refused;             /* atomic: the system refused one (logged once) */

    /* Direct scanout. The cursor shows the pointer over an image; its pixels stay mapped. */
    bool direct;                     /* atomic: the system took the surface with N_SCANOUT_DIRECT */
    struct NvKmsKapiMemory *cursor_memory;
    void *cursor_pixels;
    struct NvKmsKapiSurface *cursor_surface;
    uint32_t cursor_pitch;
    bool cursor_on;                  /* the head shows the cursor (head_lock) */
    bool cursor_drawn;               /* cursor_pixels hold pointer number cursor_serial */
    uint32_t cursor_serial;
    int32_t cursor_x, cursor_y;      /* where the head shows it */
    struct image *images;            /* head_lock */
    uint32_t last_image;             /* the number given last */
    struct image *shown;             /* the image the last flip put on the head; NULL the desktop */
    struct image *leaving;           /* the image shown before it, read until that flip completes */
    bool flipping;                   /* the last commit that put a surface on a primary layer (flip_head's) has
                                        not been seen to complete */
    uint32_t flip_head;
    bool lost;                       /* a flip back to the desktop failed: the head may show no surface (NVKMS
                                        took a freed image off it) until a flip puts one on it again */

    bool sync_ready;                 /* lock, wake and head_lock are initialised */
    mtx_t lock;
    cnd_t wake;
    bool rescan, stop;               /* under lock */
    thrd_t thread;
    bool thread_running;
    mtx_t head_lock;                 /* the head, the images, the KAPI structures below */
    thrd_t state_thread;
    bool state_running;
    bool state_stop;                 /* atomic */
} scr = { .view = -1 };

/* Large KAPI structures, used under head_lock or by the main thread alone (see the top of the
   file). */
static struct NvKmsKapiDynamicDisplayParams dyn;
static struct NvKmsKapiRequestedModeSetConfig request;
static struct NvKmsKapiModeSetReplyConfig reply;
/* What the screen shows (n_pci_screen_state): the state thread's, and PRESENT's (head_lock). */
static struct n_screen_state state_followed, state_now;

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

/* Bytes from one row of a pitch-linear surface width pixels wide (32 bits each) to the next. */
static uint32_t pitch_of(uint32_t width) {
    uint32_t align = scr.res.caps.pitchAlignment ? scr.res.caps.pitchAlignment : 256;
    return ((uint32_t)width * 4 + align - 1) / align * align;   /* width <= UINT16_MAX */
}

/* Pitch-linear video memory the display engine can scan out, whole pages. */
static struct NvKmsKapiMemory *scanout_memory(uint64_t bytes) {
    NvU8 compressible = 0;
    struct NvKmsKapiAllocateMemoryParams params = {
        .layout = NvKmsSurfaceMemoryLayoutPitch,
        .type = NVKMS_KAPI_ALLOCATION_TYPE_SCANOUT,
        .size = (bytes + PAGE_BYTES - 1) / PAGE_BYTES * PAGE_BYTES,
        .noDisplayCaching = NV_TRUE,
        .useVideoMemory = NV_TRUE,
        .compressible = &compressible,
    };
    return kapi.allocateMemory(scr.device, &params);
}

static struct NvKmsKapiSurface *surface_of(struct NvKmsKapiMemory *memory, uint32_t width, uint32_t height,
                                           uint32_t pitch, enum NvKmsSurfaceMemoryFormat format) {
    struct NvKmsKapiCreateSurfaceParams surface = { 0 };
    surface.planes[0].memory = memory;
    surface.planes[0].offset = 0;
    surface.planes[0].pitch = pitch;
    surface.width = width;
    surface.height = height;
    surface.format = format;
    return kapi.createSurface(scr.device, &surface);
}

/* A view for the system of the BAR pages RM's kernel mapping pixels shows (bytes of them), which
   must be one stretch of a BAR: -N_ERR_INVALID with *phys 0 when they are not. */
static n_handle mapping_view(const void *pixels, uint64_t bytes, uint64_t *phys, uint64_t *offset) {
    uint64_t first = 0, last = 0;
    *phys = 0;
    if (!nvrm_kernel_mapping_phys(pixels, &first) ||
        !nvrm_kernel_mapping_phys((const uint8_t *)pixels + bytes - 1, &last) || last - first != bytes - 1)
        return -N_ERR_INVALID;
    *phys = first;
    return nvrm_bar_view(first, bytes, NV_MEMORY_WRITECOMBINED, N_MEMORY_READ_WRITE, offset);
}

static bool surface_create(void) {
    if (!scr.res.caps.hasVideoMemory) {
        nvrm_log(LOG "the GPU has no video memory for a surface the system can draw into");
        return false;
    }
    scr.pitch = pitch_of(scr.width);
    scr.bytes = ((uint64_t)scr.pitch * scr.height + PAGE_BYTES - 1) / PAGE_BYTES * PAGE_BYTES;
    uint64_t bytes = scr.bytes;
    scr.memory = scanout_memory(bytes);
    if (!scr.memory) {
        nvrm_log(LOG "cannot allocate %llu KiB of video memory for the surface", (unsigned long long)(bytes >> 10));
        return false;
    }
    if (!kapi.mapMemory(scr.device, scr.memory, NVKMS_KAPI_MAPPING_TYPE_KERNEL, &scr.pixels)) {
        scr.pixels = NULL;
        nvrm_log(LOG "RM cannot map the surface");
        return false;
    }
    uint64_t phys = 0;
    scr.view = mapping_view(scr.pixels, bytes, &phys, &scr.view_offset);
    int bar = bar_index_of(phys);
    if (scr.view < 0) {
        if (!phys) nvrm_log(LOG "RM's mapping of the surface is not one stretch of a BAR");
        else
            nvrm_log(LOG "cannot make a view of BAR%d at 0x%llx for the system: %s", bar, (unsigned long long)phys,
                     n_result_text(scr.view));
        scr.view = -1;
        return false;
    }

    scr.surface = surface_of(scr.memory, scr.width, scr.height, scr.pitch, NvKmsSurfaceMemoryFormatX8R8G8B8);
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

/* The cursor that shows the pointer over an app's image: N_SCREEN_POINTER_SIZE square,
   A8R8G8B8 (the system's 0xAARRGGBB pixels as they are), blended without premultiplied alpha.
   False, with the reason logged, when the display engine has no such cursor. */
static bool cursor_create(void) {
    const uint32_t size = N_SCREEN_POINTER_SIZE;
    const struct NvKmsKapiDeviceResourcesInfo *r = &scr.res;
    if (!(r->caps.supportedCursorSurfaceMemoryFormats & (1ull << NvKmsSurfaceMemoryFormatA8R8G8B8)) ||
        r->caps.maxCursorSizeInPixels < size ||
        !(r->caps.validCursorCompositionModes & (1u << NVKMS_COMPOSITION_BLENDING_MODE_NON_PREMULT_ALPHA)) ||
        !(r->supportedSurfaceMemoryFormats[NVKMS_KAPI_LAYER_PRIMARY_IDX] & (1ull << NvKmsSurfaceMemoryFormatX8B8G8R8))) {
        nvrm_log(LOG "the display engine cannot show apps' X8B8G8R8 images with a %ux%u A8R8G8B8 cursor (cursor "
                 "formats 0x%llx, up to %u pixels, blending 0x%x; primary formats 0x%llx)", (unsigned)size,
                 (unsigned)size, (unsigned long long)r->caps.supportedCursorSurfaceMemoryFormats,
                 (unsigned)r->caps.maxCursorSizeInPixels, (unsigned)r->caps.validCursorCompositionModes,
                 (unsigned long long)r->supportedSurfaceMemoryFormats[NVKMS_KAPI_LAYER_PRIMARY_IDX]);
        return false;
    }
    scr.cursor_pitch = pitch_of(size);
    if (scr.cursor_pitch < CURSOR_MIN_PITCH) scr.cursor_pitch = CURSOR_MIN_PITCH;
    scr.cursor_memory = scanout_memory((uint64_t)scr.cursor_pitch * size);
    if (!scr.cursor_memory) {
        nvrm_log(LOG "cannot allocate video memory for the cursor");
        return false;
    }
    if (!kapi.mapMemory(scr.device, scr.cursor_memory, NVKMS_KAPI_MAPPING_TYPE_KERNEL, &scr.cursor_pixels)) {
        scr.cursor_pixels = NULL;
        nvrm_log(LOG "RM cannot map the cursor");
        return false;
    }
    scr.cursor_surface = surface_of(scr.cursor_memory, size, size, scr.cursor_pitch, NvKmsSurfaceMemoryFormatA8R8G8B8);
    if (!scr.cursor_surface) {
        nvrm_log(LOG "NVKMS cannot make a %ux%u A8R8G8B8 cursor", (unsigned)size, (unsigned)size);
        return false;
    }
    return true;
}

static void cursor_destroy(void) {
    if (scr.cursor_surface) kapi.destroySurface(scr.device, scr.cursor_surface);
    scr.cursor_surface = NULL;
    if (scr.cursor_pixels) kapi.unmapMemory(scr.device, scr.cursor_memory, NVKMS_KAPI_MAPPING_TYPE_KERNEL, scr.cursor_pixels);
    scr.cursor_pixels = NULL;
    if (scr.cursor_memory) kapi.freeMemory(scr.device, scr.cursor_memory);
    scr.cursor_memory = NULL;
    scr.cursor_drawn = false;
}

/* ------------------------------------------------------------------------------------------
 * Mode setting
 * ---------------------------------------------------------------------------------------- */

/* The head's primary layer shows surface (the screen's size) scaled to mode. */
static void primary_request(struct NvKmsKapiHeadRequestedConfig *h, struct NvKmsKapiSurface *surface,
                            const struct NvKmsKapiDisplayMode *mode) {
    struct NvKmsKapiLayerRequestedConfig *primary = &h->layerRequestedConfig[NVKMS_KAPI_LAYER_PRIMARY_IDX];
    struct NvKmsKapiLayerConfig *c = &primary->config;
    c->surface = surface;
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
}

/* The head's display and mode as they are, for a flip (KAPI flips only an active head). */
static void active_request(struct NvKmsKapiHeadRequestedConfig *h, NvKmsKapiDisplay display,
                           const struct NvKmsKapiDisplayMode *mode) {
    h->modeSetConfig.bActive = NV_TRUE;
    h->modeSetConfig.numDisplays = 1;
    h->modeSetConfig.displays[0] = display;
    h->modeSetConfig.mode = *mode;
    h->modeSetConfig.colorimetry = NVKMS_OUTPUT_COLORIMETRY_DEFAULT;
}

/* Adds head to the request: display shown in mode with the desktop's surface on its primary
   layer (scaled to the mode), its other layers and the cursor empty; display 0 turns the head
   off. */
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

    active_request(h, display, mode);
    primary_request(h, scr.surface, mode);
    struct NvKmsKapiLayerRequestedConfig *primary = &h->layerRequestedConfig[NVKMS_KAPI_LAYER_PRIMARY_IDX];
    primary->flags.srcXYChanged = NV_TRUE;
    primary->flags.srcWHChanged = NV_TRUE;
    primary->flags.dstXYChanged = NV_TRUE;
    primary->flags.dstWHChanged = NV_TRUE;
}

/* Commits the request. flip is the head whose primary layer it puts a surface on, -1 none: that
   flip's completion is awaited by flip_completed (nvkms_kapi_rm.c reads the completion notifier
   KAPI names in the commit, since KAPI's table does not say). */
static bool commit(int flip) {
    if (flip >= 0) nvrm_nvkms_flip_arm(scr.device, (uint32_t)flip, NVKMS_KAPI_LAYER_PRIMARY_IDX);
    memset(&reply, 0, sizeof reply);
    if (!kapi.applyModeSetConfig(scr.device, &request, &reply, NV_TRUE) ||
        reply.flipResult != NV_KMS_FLIP_RESULT_SUCCESS)
        return false; /* nothing flipped: no notifier is written */
    if (flip >= 0) {
        scr.flipping = true;
        scr.flip_head = (uint32_t)flip;
    }
    return true;
}

/* The request, checked by NVKMS first and then committed (commit). */
static bool apply(const char *what, int flip) {
    memset(&reply, 0, sizeof reply);
    if (!kapi.applyModeSetConfig(scr.device, &request, &reply, NV_FALSE)) {
        nvrm_log(LOG "NVKMS rejects %s", what);
        return false;
    }
    if (!commit(flip)) {
        nvrm_log(LOG "%s failed (flip result %d)", what, (int)reply.flipResult);
        return false;
    }
    return true;
}

/* The last commit that put a surface on a primary layer has completed. One whose flip does not
   complete in FLIP_TIMEOUT_MS stays awaited. */
static bool flip_completed(void) {
    if (!scr.flipping) return true;
    uint64_t until = nvrm_now_ns() + (uint64_t)FLIP_TIMEOUT_MS * 1000000u;
    while (!nvrm_nvkms_flip_done(scr.device, scr.flip_head, NVKMS_KAPI_LAYER_PRIMARY_IDX)) {
        if (nvrm_now_ns() >= until) {
            nvrm_log(LOG "the flip on head %u has not completed after %d ms", (unsigned)scr.flip_head, FLIP_TIMEOUT_MS);
            return false;
        }
        n_sleep_ms(1);
    }
    scr.flipping = false;
    return true;
}

/* display shows the desktop's surface in mode on head, without the cursor; the head nvrm used
   before is turned off when it is another one. False when the mode set failed (scr.head_on
   tells whether the head is lit) or its flip has not completed. */
static bool show_on(NvKmsKapiDisplay display, uint32_t head, const struct NvKmsKapiDisplayMode *mode) {
    memset(&request, 0, sizeof request);
    if (scr.head_on && scr.head != head) head_request(scr.head, 0, NULL);
    head_request(head, display, mode);
    if (!apply("the mode set", (int)head)) return false;
    scr.head_on = true;
    scr.head = head;
    scr.display = display;
    scr.mode = *mode;
    scr.leaving = scr.shown;
    scr.shown = NULL;
    scr.lost = false;
    scr.cursor_on = false;
    if (!flip_completed()) return false;
    scr.leaving = NULL;
    return true;
}

/* A mode set: NVKMS returns once the head is off, and nothing of nvrm's is read any more. */
static bool head_off(void) {
    if (!scr.head_on) return true;
    memset(&request, 0, sizeof request);
    head_request(scr.head, 0, NULL);
    if (!apply("turning the head off", -1)) return false;
    scr.head_on = false;
    scr.shown = scr.leaving = NULL;
    scr.flipping = scr.lost = false;
    scr.cursor_on = false;
    return true;
}

/* ------------------------------------------------------------------------------------------
 * Flips: an app's image or the desktop's surface on the lit head, and the cursor over an image
 * (head_lock)
 * ---------------------------------------------------------------------------------------- */

/* The last flip has completed: what it took off the head is no longer read. */
static bool settle(void) {
    if (!flip_completed()) return false;
    scr.leaving = NULL;
    return true;
}

/* Where the cursor's top left goes for the pointer of st, in the mode's pixels (the screen is
   scaled to the mode). */
static void cursor_place(const struct n_screen_state *st, int32_t *x, int32_t *y) {
    int64_t px = ((int64_t)st->pointer_x - st->hot_x) * scr.mode.timings.hVisible / scr.width;
    int64_t py = ((int64_t)st->pointer_y - st->hot_y) * scr.mode.timings.vVisible / scr.height;
    *x = (int32_t)(px < INT16_MIN ? INT16_MIN : px > INT16_MAX ? INT16_MAX : px);
    *y = (int32_t)(py < INT16_MIN ? INT16_MIN : py > INT16_MAX ? INT16_MAX : py);
}

/* The head shows image (NULL: the desktop's surface) from its next vertical blank, with the
   pointer of st drawn by the cursor over an image; the desktop's surface has the pointer in it.
   Waits for the flip before to complete first, not for this one. A flip, not a mode set: NVKMS
   checks it as it commits it. */
static bool show(struct image *image, const struct n_screen_state *st) {
    if (!scr.head_on || !settle()) return false;
    bool cursor = image && st && st->pointer_shown;
    int32_t x = 0, y = 0;
    if (cursor) cursor_place(st, &x, &y);
    bool surface_changed = scr.lost || image != scr.shown;
    bool cursor_changed = cursor != scr.cursor_on;
    bool moved = cursor && (cursor_changed || x != scr.cursor_x || y != scr.cursor_y);
    if (cursor && !(scr.cursor_drawn && scr.cursor_serial == st->pointer_serial)) {
        for (uint32_t row = 0; row < N_SCREEN_POINTER_SIZE; row++)
            memcpy((uint8_t *)scr.cursor_pixels + (uint64_t)row * scr.cursor_pitch,
                   st->pointer + row * N_SCREEN_POINTER_SIZE, N_SCREEN_POINTER_SIZE * 4);
        __asm__ volatile("sfence" ::: "memory");
        scr.cursor_drawn = true;
        scr.cursor_serial = st->pointer_serial;
    }
    if (!surface_changed && !cursor_changed && !moved) return true;

    memset(&request, 0, sizeof request);
    request.headsMask = 1u << scr.head;
    struct NvKmsKapiHeadRequestedConfig *h = &request.headRequestedConfig[scr.head];
    active_request(h, scr.display, &scr.mode);
    if (surface_changed) primary_request(h, image ? image->surface : scr.surface, &scr.mode);
    struct NvKmsKapiCursorRequestedConfig *c = &h->cursorRequestedConfig;
    if (cursor_changed) {
        c->surface = cursor ? scr.cursor_surface : NULL;
        c->compParams.compMode = NVKMS_COMPOSITION_BLENDING_MODE_NON_PREMULT_ALPHA;
        c->compParams.surfaceAlpha = 0xff;
        c->flags.surfaceChanged = NV_TRUE;
    }
    if (moved) {
        c->dstX = (NvS16)x;
        c->dstY = (NvS16)y;
        c->flags.dstXYChanged = NV_TRUE;
    }
    if (!commit(surface_changed ? (int)scr.head : -1)) {
        nvrm_log(LOG "the flip to %s on head %u failed (flip result %d)", image ? "an app's image" : "the desktop",
                 (unsigned)scr.head, (int)reply.flipResult);
        return false;
    }
    if (surface_changed) {
        if (image && !scr.shown) nvrm_log(LOG "app %u's images are shown directly", (unsigned)image->app);
        else if (!image) nvrm_log(LOG "the desktop is shown again");
        scr.leaving = scr.shown;
        scr.shown = image;
        scr.lost = false;
    }
    scr.cursor_on = cursor;
    if (moved) {
        scr.cursor_x = x;
        scr.cursor_y = y;
    }
    return true;
}

/* image is no longer read by the display engine: the desktop is shown instead of it, and the
   flip away from it has completed. When that fails, NVKMS takes the image off the head as its
   surface is destroyed, which leaves the head without a surface: scr.lost, and the desktop is
   flipped back once the image is gone (recover). */
static void take_off(struct image *image) {
    if (image == scr.shown && !show(NULL, NULL)) {
        nvrm_log(LOG "image %u is still shown; NVKMS takes it off the head as it is freed", (unsigned)image->number);
        scr.lost = true;
    }
    if (image == scr.leaving) settle();
    /* Whether or not that worked, the image is gone for nvrm. */
    if (image == scr.shown) scr.shown = NULL;
    if (image == scr.leaving) scr.leaving = NULL;
}

/* The desktop again on a head a failed flip left without a surface. Under head_lock. */
static void recover(void) {
    if (scr.lost && scr.head_on && !show(NULL, NULL))
        nvrm_log(LOG "the desktop is still not shown on head %u; tried again when the screen changes", (unsigned)scr.head);
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

/* The surface stays the system's throughout; only what shows it changes. A display lit again
   shows the desktop until the app shown directly presents its next image. Under head_lock. */
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
        if (!scr.head_on) {
            nvrm_log(LOG "display 0x%08x stays dark", (unsigned)display);
            return;
        }
        /* Lit: the next flip waits for this one again (settle). */
        nvrm_log(LOG "display 0x%08x is lit, though its first flip has not been seen to complete", (unsigned)display);
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
        mtx_lock(&scr.head_lock);
        rescan();
        mtx_unlock(&scr.head_lock);
        mtx_lock(&scr.lock);
    }
    mtx_unlock(&scr.lock);
    return 0;
}

/* ------------------------------------------------------------------------------------------
 * Direct scanout: what the system lets the screen show (the state thread) and apps' images
 * (nvrm's client threads)
 * ---------------------------------------------------------------------------------------- */

/* The screen's state changed: an image of an app the system no longer shows directly goes, the
   cursor follows the pointer over one that stays; a head left without a surface gets the
   desktop again. Under head_lock. */
static void follow(const struct n_screen_state *st) {
    if (scr.shown && st->app == scr.shown->app) show(scr.shown, st);
    else if (scr.shown) show(NULL, NULL);
    else recover();
}

static int state_thread(void *unused) {
    (void)unused;
    uint64_t seen = 0;
    bool refused = false;
    while (!__atomic_load_n(&scr.state_stop, __ATOMIC_ACQUIRE)) {
        n_result r = n_pci_screen_state(nvrm_dev.pci, seen, &state_followed, STATE_WAIT_MS);
        if (r == -N_ERR_TIMEOUT) continue;
        if (r < 0) {
            /* Not while nvrm's surface is given with N_SCANOUT_DIRECT; looked at again later. */
            if (!refused) nvrm_log(LOG "the system does not say what the screen shows: %s", n_result_text(r));
            refused = true;
            n_sleep_ms(STATE_WAIT_MS);
            continue;
        }
        refused = false;
        seen = state_followed.serial;
        mtx_lock(&scr.head_lock);
        follow(&state_followed);
        mtx_unlock(&scr.head_lock);
    }
    return 0;
}

static struct image *image_find(uint32_t number) {
    for (struct image *i = scr.images; i; i = i->next)
        if (i->number == number) return i;
    return NULL;
}

/* What image_create made, given back; none of it is shown. The system's hold on the image ends
   first. Ending it fails only when the system ended it already, as it does with every image once
   nvrm's surface is no longer the screen's (nvrm_display_stop gives the boot screen back first). */
static void image_release(struct image *image) {
    if (image->panic > 0) n_pci_screen_image_end(nvrm_dev.pci, image->panic);
    if (image->pixels) kapi.unmapMemory(scr.device, image->memory, NVKMS_KAPI_MAPPING_TYPE_KERNEL, image->pixels);
    if (image->surface) kapi.destroySurface(scr.device, image->surface);
    if (image->memory) kapi.freeMemory(scr.device, image->memory);
    free(image);
}

/* Off the screen, out of the list and freed. Under head_lock. */
static void image_free(struct image *image) {
    take_off(image);
    for (struct image **at = &scr.images; *at; at = &(*at)->next)
        if (*at == image) {
            *at = image->next;
            break;
        }
    image_release(image);
    recover();
}

/* The system draws its panic screen into the image too (n_pci_screen_image): when the kernel
   stops, the display engine goes on showing the image flipped last, and nvrm no longer runs to
   show the desktop's surface again. A view of the BAR pages RM's mapping shows; the system holds
   what it draws into, so the view's handle closes at once. Returns the image's number there. */
static n_result image_for_panic(const struct image *image) {
    uint64_t phys = 0, offset = 0;
    n_handle view = mapping_view(image->pixels, scr.bytes, &phys, &offset);
    if (view < 0) return view;
    struct n_scanout desc = {
        .width = scr.width,
        .height = scr.height,
        .pitch = scr.pitch,
        .format = N_SCANOUT_XBGR8888,
        .offset = offset,
    };
    n_result r = n_pci_screen_image(nvrm_dev.pci, view, &desc);
    n_handle_close(view);
    return r ? r : -N_ERR_INVALID;
}

static uint32_t image_create(uint32_t app, const void *connection, struct nvrm_scanout_request *r) {
    struct image *image = calloc(1, sizeof *image);
    if (!image) return NV_ERR_NO_MEMORY;
    image->memory = scanout_memory(scr.bytes);
    if (image->memory)
        image->surface = surface_of(image->memory, scr.width, scr.height, scr.pitch, NvKmsSurfaceMemoryFormatX8B8G8R8);
    /* Video memory comes as the last owner left it: black before the app may read it. RM's
       mapping stays for the system's panic screen. */
    if (!image->surface ||
        !kapi.mapMemory(scr.device, image->memory, NVKMS_KAPI_MAPPING_TYPE_KERNEL, &image->pixels)) {
        image->pixels = NULL;
        nvrm_log(LOG "cannot make a %ux%u image for app %u", (unsigned)scr.width, (unsigned)scr.height, (unsigned)app);
        image_release(image);
        return NV_ERR_NO_MEMORY;
    }
    memset(image->pixels, 0, (size_t)scr.bytes);
    __asm__ volatile("sfence" ::: "memory");
    image->panic = image_for_panic(image);
    if (image->panic < 0) {
        /* Not shown where a stopped kernel could not be seen over it: the app's window stays composited. */
        nvrm_log(LOG "the system cannot draw its panic screen into a %ux%u image (%s); app %u gets none",
                 (unsigned)scr.width, (unsigned)scr.height, n_result_text(image->panic), (unsigned)app);
        image_release(image);
        return NV_ERR_INSUFFICIENT_RESOURCES;
    }
    do image->number = ++scr.last_image;
    while (!image->number || image_find(image->number));
    image->app = app;
    image->connection = connection;
    image->next = scr.images;
    scr.images = image;
    r->image = image->number;
    r->width = scr.width;
    r->height = scr.height;
    r->pitch = scr.pitch;
    r->bytes = scr.bytes;
    return NV_OK;
}

static uint32_t image_present(uint32_t app, struct image *image) {
    /* The latest state, not the one the state thread follows: that one may be older. */
    n_result r = n_pci_screen_state(nvrm_dev.pci, 0, &state_now, 0);
    if (r < 0 || state_now.app != app || !scr.head_on) return NV_ERR_INVALID_STATE;
    return show(image, &state_now) ? NV_OK : NV_ERR_INVALID_STATE;
}

/* The image as memory of the app's RM client: NVKMS's RM client shares it with that client
   alone (RS_SHARE_TYPE_CLIENT) while the app's own RM file duplicates it, so RM checks the
   duplicate as it checks the app's other requests (the client must be the file's), and the
   share is revoked again at once. */
static uint32_t image_import(void *file, struct image *image, struct nvrm_scanout_request *r) {
    uint32_t nvkms_client = 0, memory = 0;
    if (!nvrm_nvkms_memory_handle(scr.device, image->memory, &nvkms_client, &memory)) return NV_ERR_INVALID_STATE;
    nvidia_kernel_rmapi_ops_t share = { .op = NV04_SHARE };
    share.params.share.hClient = nvkms_client;
    share.params.share.hObject = memory;
    share.params.share.sharePolicy.target = r->client;
    share.params.share.sharePolicy.type = RS_SHARE_TYPE_CLIENT;
    share.params.share.sharePolicy.action = RS_SHARE_ACTION_FLAG_COMPOSE;
    RS_ACCESS_MASK_ADD(&share.params.share.sharePolicy.accessMask, RS_ACCESS_DUP_OBJECT);
    rm_kernel_rmapi_op(nvrm_dev.sp, &share);
    if (share.params.share.status != NV_OK) return share.params.share.status;

    NVOS55_PARAMETERS dup = { .hClient = r->client, .hParent = r->parent, .hObject = r->object,
                              .hClientSrc = nvkms_client, .hObjectSrc = memory };
    uint32_t st = nvrm_rm_ioctl(file, NV_ESC_RM_DUP_OBJECT, &dup, sizeof dup);
    if (st == NV_OK) st = dup.status;

    share.params.share.sharePolicy.action = RS_SHARE_ACTION_FLAG_REVOKE | RS_SHARE_ACTION_FLAG_COMPOSE;
    share.params.share.status = NV_OK;
    rm_kernel_rmapi_op(nvrm_dev.sp, &share);
    if (share.params.share.status != NV_OK)
        nvrm_log(LOG "RM keeps image %u shared with client 0x%x: 0x%x", (unsigned)image->number, (unsigned)r->client,
                 (unsigned)share.params.share.status);
    if (st == NV_OK) r->object = dup.hObject;
    return st;
}

n_result nvrm_display_scanout(void *file, uint32_t principal, void *args, uint32_t size) {
    if (size != sizeof(struct nvrm_scanout_request)) return -N_ERR_INVALID;
    struct nvrm_scanout_request *r = args;
    /* Requests come only between start and stop; without direct scanout head_lock may not exist. */
    if (!__atomic_load_n(&scr.direct, __ATOMIC_ACQUIRE)) {
        r->status = NV_ERR_NOT_SUPPORTED;
        return 0;
    }
    if (r->op < NVRM_SCANOUT_CREATE || r->op > NVRM_SCANOUT_IMPORT) {
        r->status = NV_ERR_INVALID_ARGUMENT;
        return 0;
    }
    mtx_lock(&scr.head_lock);
    struct image *image = r->op == NVRM_SCANOUT_CREATE ? NULL : image_find(r->image);
    if (r->op != NVRM_SCANOUT_CREATE && (!image || image->app != principal))
        r->status = NV_ERR_INVALID_OBJECT_HANDLE; /* another app's image is not told apart from none */
    else if (r->op == NVRM_SCANOUT_CREATE)
        r->status = image_create(principal, file, r);
    else if (r->op == NVRM_SCANOUT_DESTROY) {
        image_free(image);
        r->status = NV_OK;
    } else if (r->op == NVRM_SCANOUT_PRESENT)
        r->status = image_present(principal, image);
    else
        r->status = image_import(file, image, r);
    mtx_unlock(&scr.head_lock);
    return 0;
}

void nvrm_display_connection_end(void *file) {
    if (!__atomic_load_n(&scr.direct, __ATOMIC_ACQUIRE)) return;
    mtx_lock(&scr.head_lock);
    for (struct image *i = scr.images, *next; i; i = next) {
        next = i->next;
        if (i->connection == file) image_free(i);
    }
    mtx_unlock(&scr.head_lock);
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
    if (mtx_init(&scr.head_lock, mtx_plain) != thrd_success) {
        cnd_destroy(&scr.wake);
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

    /* Direct scanout needs the cursor, for the pointer over an app's image. */
    bool direct = cursor_create();
    if (!direct) cursor_destroy();
    struct n_scanout desc = {
        .width = scr.width,
        .height = scr.height,
        .pitch = scr.pitch,
        .format = N_SCANOUT_XRGB8888 | (direct ? N_SCANOUT_DIRECT : 0),
        .offset = scr.view_offset,
    };
    given = n_pci_give_screen(nvrm_dev.pci, scr.view, &desc);
    if (given < 0 && direct) {
        nvrm_log(LOG "the system does not let nvrm show windows directly (%s); it composites them",
                 n_result_text(given));
        direct = false;
        cursor_destroy();
        desc.format = N_SCANOUT_XRGB8888;
        given = n_pci_give_screen(nvrm_dev.pci, scr.view, &desc);
    }
    if (given < 0) {
        nvrm_log(LOG "the system does not take the surface: %s", n_result_text(given));
        why = "the system keeps drawing into the boot screen";
        goto fail;
    }
    __atomic_store_n(&scr.given, true, __ATOMIC_RELEASE);
    if (direct) {
        __atomic_store_n(&scr.state_stop, false, __ATOMIC_RELEASE);
        if (thrd_create(&scr.state_thread, state_thread, NULL) == thrd_success) {
            scr.state_running = true;
            __atomic_store_n(&scr.direct, true, __ATOMIC_RELEASE);
        } else {
            nvrm_log(LOG "cannot start the state thread; windows are not shown directly");
            direct = false;
            desc.format = N_SCANOUT_XRGB8888;
            n_result r = n_pci_give_screen(nvrm_dev.pci, scr.view, &desc);
            cursor_destroy();
            if (r < 0) {
                /* The system would still hand fullscreen windows to nvrm, which shows none. */
                nvrm_log(LOG "the system does not take the surface without direct scanout: %s", n_result_text(r));
                why = "the system would show windows directly that nvrm does not show";
                goto fail;
            }
        }
    }
    vblank_on(head);

    if (thrd_create(&scr.thread, display_thread, NULL) == thrd_success) scr.thread_running = true;
    else nvrm_log(LOG "cannot start the display thread; displays coming and going are not followed");
    nvrm_log(LOG "the desktop is drawn into nvrm's surface, shown on display 0x%08x (head %u)%s", (unsigned)display,
             (unsigned)head, direct ? "; apps' fullscreen windows can be shown directly" : "");
    return true;

fail:
    nvrm_log(LOG "%s; the screen stays with the firmware's framebuffer", why);
    nvrm_display_stop();
    return false;
}

void nvrm_display_stop(void) {
    __atomic_store_n(&scr.direct, false, __ATOMIC_RELEASE);
    if (scr.state_running) {
        __atomic_store_n(&scr.state_stop, true, __ATOMIC_RELEASE);
        thrd_join(scr.state_thread, NULL); /* within STATE_WAIT_MS */
        scr.state_running = false;
    }
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
    scr.shown = scr.leaving = NULL;  /* nothing of nvrm's is shown any more */
    scr.flipping = scr.lost = false;
    scr.cursor_on = false;
    while (scr.images) image_free(scr.images);
    cursor_destroy();
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
        mtx_destroy(&scr.head_lock);
        cnd_destroy(&scr.wake);
        mtx_destroy(&scr.lock);
        scr.sync_ready = false;
    }
}
