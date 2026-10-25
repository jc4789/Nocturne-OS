/* nvrm's screen (display.h): the display taken over from the firmware's framebuffer through
 * NVKMS's kernel API (KAPI), and the system's desktop shown on it.
 *
 *   allocateDevice -> grabOwnership -> declareEventInterest -> getDeviceResourcesInfo
 *   -> RM's boot displays (the ones the firmware lit) -> getDisplays, each display's connector,
 *      static and dynamic information (connection, EDID) -> the display the firmware lit
 *   -> the pointer's cursor surface (32x32 A8R8G8B8, mapped): with it the desktop's surface is
 *      offered for direct scanout (N_SCANOUT_DIRECT)
 *   -> getDisplayMode: the display's valid modes, offered to the system with its monitor's name
 *      (n_pci_screen_modes). Which of them the screen shows is the system's to say, and the
 *      user's (n_pci_screen_state's mode: the monitor's preferred mode until the user chooses
 *      another); nvrm says back what the head shows, so a mode it could not show is refused and
 *      the system's next best one comes
 *   -> for the mode the system asks for: validateDisplayMode -> allocateMemory (video memory,
 *      pitch linear, for scanout, the mode's size) -> mapMemory (RM's kernel mapping, through
 *      BAR1) -> createSurface (X8R8G8B8) -> the boot screen's pixels copied into the first one
 *      when it has the boot screen's size, so the switch shows no black or stale frame
 *   -> applyModeSetConfig checked -> n_pci_give_screen: the system draws the desktop into the
 *      surface, at its size, from then on (without N_SCANOUT_DIRECT when the system or the
 *      display engine cannot show windows directly) -> committed -> the flip has completed (the
 *      completion notifier KAPI names in the commit, which nvkms_kapi_rm.c clears before it and
 *      reads)
 *   -> registerVblankIntrCallback: each vertical blank goes to n_pci_screen_vblank
 *
 * The surface is given to the system as a view of BAR1 over the same pages RM mapped
 * (os_pci.c records where its kernel mappings lie on the bus). A mode of another size gets a
 * surface of its own, given to the system between NVKMS's check of the mode set and its commit,
 * so the desktop is drawn at the new size by the time the head shows it; the one before is
 * freed after. A mode of the same size is a mode set alone. The surface stays given while
 * monitors come and go: a display that goes away has its head turned off, and the first display
 * that is connected (the firmware's first, then any) has its modes offered in turn.
 *
 * Direct scanout (NVRM_SCANOUT, include/nvrm_protocol.h): an app's images are video memory of
 * the desktop's size (allocateMemory, createSurface X8B8G8R8) that its GPU work writes into
 * (IMPORT duplicates the memory into the app's RM client). While n_pci_screen_state names the
 * app, PRESENT flips the head's primary layer to an image (a flip, not a mode set: only the
 * surface changes), and the display engine draws the pointer as its cursor. When the state
 * names another app or none, the state thread flips back to the desktop's surface, which the
 * system has kept drawing into, and the cursor goes. An image is off the screen, and its last
 * flip away from it complete, before it is freed.
 *
 * hand_back undoes the display's part in the opposite order: the modes withdrawn and the boot
 * screen back to the system, then releaseOwnership, with which NVKMS shows the firmware's
 * framebuffer console again (nvkms-console-restore.c). nvrm_display_stop does it, and so does a
 * running nvrm whose flip is not seen to complete (the display engine may not show what nvrm
 * gives it) or whose user asks for the boot screen back (Ctrl+Alt+Esc, n_screen_state's
 * take_back): the screen is then the firmware's again, not a dark display. nvrm_display_stop
 * frees the images, the cursor, the surface and the device after it.
 *
 * Threads: KAPI's event callback runs on NVKMS's work thread and only wakes this file's
 * display thread, which does the KAPI calls of a reconnection (they take NVKMS's lock). The
 * state thread follows n_pci_screen_state: the mode the system asks for, and what the screen
 * shows directly. Requests come on nvrm's client threads (nvrm_display_scanout), between
 * nvrm_display_start and nvrm_display_stop. All of them change the head, the surfaces, the
 * images and the KAPI structures below under head_lock, one at a time. The vertical-blank
 * callback runs under NVKMS's lock on its timer thread and only counts and tells the system.
 * Before the threads start and after they are joined, the calling thread (nvrm's main thread)
 * is the only one that touches the state below.
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
#define FLIP_TIMEOUT_MS 3000         /* a committed flip that has not completed by then failed */
#define PAGE_BYTES 4096u
#define STATE_WAIT_MS 1000           /* the state thread looks at its stop flag at least this often */
#define CURSOR_MIN_PITCH 256u        /* NVKMS's for a 32x32 cursor (nvEvoGetHeadSetControlCursorValueC3) */
#define MODE_TRIES 4                 /* the system's modes tried at once after modes are offered: its next
                                        best after each one the display could not show; the state thread
                                        follows the rest */

static struct NvKmsKapiFunctionsTable kapi = { .versionString = NV_VERSION_STRING };

/* The desktop's surface: video memory of the mode's size the system draws the desktop into. */
struct desk {
    struct NvKmsKapiMemory *memory;
    void *pixels;                    /* RM's kernel mapping of memory (KAPI mapMemory) */
    struct NvKmsKapiSurface *surface;
    uint32_t width, height, pitch;
    uint64_t bytes;
    n_handle view;                   /* the surface as a view of BAR1, for the system; -1 none */
    uint64_t view_offset;            /* where the surface starts in the view */
};

/* An app's image for direct scanout (NVRM_SCANOUT): the desktop's size when it was made. */
struct image {
    struct image *next;
    uint32_t number;                 /* nvrm-wide, never 0 */
    uint32_t app;                    /* the app that made it (n_app_self's number) */
    const void *connection;          /* the connection that made it (its RM file) */
    struct NvKmsKapiMemory *memory;
    struct NvKmsKapiSurface *surface;
    void *pixels;                    /* RM's kernel mapping of memory, which the system's view is of */
    uint32_t width, height, pitch;
    uint64_t bytes;
    n_result panic;                  /* n_pci_screen_image's number: the panic screen is drawn here too */
};

static struct {
    struct NvKmsKapiDevice *device;
    bool owner;                      /* grabOwnership succeeded */
    bool events;                     /* declareEventInterest succeeded */
    struct NvKmsKapiDeviceResourcesInfo res;
    uint32_t boot_mask;              /* RM's boot displays, as display handles (dpy id bits) */

    struct desk desk;                /* what the system draws the desktop into (none before the first mode) */
    bool given;                      /* atomic: the system draws into desk */

    NvKmsKapiDisplay target;         /* the display whose modes the system is offered, 0 none */
    uint32_t target_head;            /* the head it is shown on (the lowest it can use) */
    uint64_t mode_serial;            /* the system's last wish followed (n_screen_state mode_serial) */
    NvKmsKapiDisplay display;        /* the display nvrm lit, 0 none */
    uint32_t head;                   /* its head, while head_on */
    bool head_on;
    struct NvKmsKapiDisplayMode mode;/* the head's mode, while head_on */
    struct NvKmsKapiVblankIntrCallback *vblank;
    uint32_t vblank_head;
    uint64_t vblanks;                /* atomic */
    bool vblank_refused;             /* atomic: the system refused one (logged once) */

    /* Direct scanout. The cursor shows the pointer over an image; its pixels stay mapped. */
    bool serving;                    /* atomic: between start and stop, nvrm's clients' requests are taken */
    bool direct;                     /* apps' images are made and shown: nvrm has the cursor, the system took
                                        the desktop's surface with N_SCANOUT_DIRECT and the state thread
                                        follows what it shows (head_lock) */
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
    uint64_t flip_commit_ns;         /* how long NVKMS took over that commit */
    bool lost;                    /* a flip back to the desktop failed: the head may show no surface (NVKMS
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
} scr = { .desk.view = -1 };

/* Large KAPI structures, used under head_lock or by the main thread alone (see the top of the
   file). */
static struct NvKmsKapiDynamicDisplayParams dyn;
static struct NvKmsKapiRequestedModeSetConfig request;
static struct NvKmsKapiModeSetReplyConfig reply;
/* The offered display's modes: for the system (offer; offered, what it took last) and their
   timings (kmodes[i] is offer.modes[i]). Under head_lock. */
static struct n_screen_modes offer, offered;
static struct NvKmsKapiDisplayMode kmodes[N_SCREEN_MODES_MAX];
/* What the screen shows (n_pci_screen_state): the state thread's, and the one read at once
   (head_lock). */
static struct n_screen_state state_followed, state_now;

static void mode_text(const struct NvKmsKapiDisplayMode *m, char *out, size_t cap) {
    snprintf(out, cap, "%ux%u at %u.%03u Hz", (unsigned)m->timings.hVisible, (unsigned)m->timings.vVisible,
             (unsigned)(m->timings.refreshRate / 1000), (unsigned)(m->timings.refreshRate % 1000));
}

static bool same_mode(const struct NvKmsKapiDisplayMode *a, const struct NvKmsKapiDisplayMode *b) {
    const struct NvKmsKapiDisplayModeTimings *x = &a->timings, *y = &b->timings;
    return x->refreshRate == y->refreshRate && x->pixelClockHz == y->pixelClockHz && x->hVisible == y->hVisible &&
           x->hSyncStart == y->hSyncStart && x->hSyncEnd == y->hSyncEnd && x->hTotal == y->hTotal &&
           x->vVisible == y->vVisible && x->vSyncStart == y->vSyncStart && x->vSyncEnd == y->vSyncEnd &&
           x->vTotal == y->vTotal && x->flags.interlaced == y->flags.interlaced &&
           x->flags.doubleScan == y->flags.doubleScan;
}

/* A mode as the system names it. */
static struct n_screen_mode mode_of(const struct NvKmsKapiDisplayMode *m) {
    return (struct n_screen_mode){ m->timings.hVisible, m->timings.vVisible, m->timings.refreshRate, 0 };
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

/* The ids of an EDID's base block: manufacturer, product code, serial number and the monitor
   name descriptor's text ("" without one). */
struct edid_ids {
    char maker[4];
    unsigned product;
    uint32_t serial;
    char name[14];
};

static bool edid_ids(const uint8_t *e, size_t n, struct edid_ids *id) {
    if (n < 128) return false;
    unsigned m = (unsigned)e[8] << 8 | e[9];
    id->maker[0] = (char)('@' + (m >> 10 & 31));
    id->maker[1] = (char)('@' + (m >> 5 & 31));
    id->maker[2] = (char)('@' + (m & 31));
    id->maker[3] = 0;
    id->product = (unsigned)e[10] | (unsigned)e[11] << 8;
    id->serial = (uint32_t)e[12] | (uint32_t)e[13] << 8 | (uint32_t)e[14] << 16 | (uint32_t)e[15] << 24;
    id->name[0] = 0;
    for (unsigned d = 54; d + 18 <= 126; d += 18) {
        const uint8_t *b = e + d;
        if (b[0] || b[1] || b[2] || b[3] != 0xfc) continue;
        size_t k = 0;
        for (unsigned j = 5; j < 18 && b[j] != 0x0a; j++) id->name[k++] = b[j] >= 0x20 && b[j] < 0x7f ? (char)b[j] : '?';
        while (k && id->name[k - 1] == ' ') k--;
        id->name[k] = 0;
        break;
    }
    return true;
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
            char monitor[40] = "";
            struct edid_ids id;
            if (connected && edid_ids(dyn.edid.buffer, dyn.edid.bufferSize, &id))
                snprintf(monitor, sizeof monitor, "%s%04x%s%s", id.maker, id.product, id.name[0] ? " " : "", id.name);
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

/* What the system knows display's monitor by (struct n_screen_modes): its EDID's manufacturer,
   product code and serial number, which tell it from other monitors (the user's choice of mode
   is per monitor), and the EDID's monitor name; without an EDID, the connector's name. */
static void monitor_of(NvKmsKapiDisplay display, struct n_screen_modes *m) {
    struct NvKmsKapiStaticDisplayInfo info = { 0 };
    struct NvKmsKapiConnectorInfo connector = { 0 };
    char port[32] = "?";
    if (kapi.getStaticDisplayInfo(scr.device, display, &info) &&
        kapi.getConnectorInfo(scr.device, info.connectorHandle, &connector))
        snprintf(port, sizeof port, "%s-%u", NvKmsConnectorTypeString(connector.type), (unsigned)connector.physicalIndex);
    memset(&dyn, 0, sizeof dyn);
    dyn.handle = display;
    struct edid_ids id;
    if (kapi.getDynamicDisplayInfo(scr.device, &dyn) && dyn.connected &&
        edid_ids(dyn.edid.buffer, dyn.edid.bufferSize, &id)) {
        snprintf(m->monitor, sizeof m->monitor, "%s%04x-%08x", id.maker, id.product, (unsigned)id.serial);
        snprintf(m->name, sizeof m->name, "%s", id.name[0] ? id.name : port);
    } else {
        snprintf(m->monitor, sizeof m->monitor, "%s", port);
        snprintf(m->name, sizeof m->name, "%s", port);
    }
}

/* Where m is in offer, -1 nowhere. */
static int mode_index(const struct n_screen_mode *m) {
    for (uint32_t i = 0; i < offer.count; i++)
        if (offer.modes[i].width == m->width && offer.modes[i].height == m->height &&
            offer.modes[i].refresh_mhz == m->refresh_mhz)
            return (int)i;
    return -1;
}

/* display's modes into offer and kmodes: those NVKMS calls valid, progressive and not doubled,
   that the display engine's surfaces can have, each size and refresh once (the EDID's preferred
   timing for it when it has one), N_SCREEN_MODES_MAX at most. The first the EDID prefers is
   marked so. None when display is 0 or its modes cannot be listed. */
static void modes_list(NvKmsKapiDisplay display) {
    memset(&offer, 0, sizeof offer);
    if (!display) return;
    monitor_of(display, &offer);
    const struct NvKmsKapiDeviceResourcesInfo *r = &scr.res;
    bool have_preferred = false;
    struct NvKmsKapiDisplayMode mode;
    for (NvU32 index = 0;; index++) {
        NvBool valid = NV_FALSE, preferred = NV_FALSE;
        memset(&mode, 0, sizeof mode);
        int more = kapi.getDisplayMode(scr.device, display, index, &mode, &valid, &preferred);
        if (more < 0) {
            nvrm_log(LOG "NVKMS cannot list the modes of display 0x%08x", (unsigned)display);
            offer.count = 0;
            return;
        }
        if (more == 0) break;
        const struct NvKmsKapiDisplayModeTimings *t = &mode.timings;
        if (!valid || t->flags.interlaced || t->flags.doubleScan || !t->hVisible || !t->vVisible ||
            t->hVisible > UINT16_MAX || t->vVisible > UINT16_MAX || !t->refreshRate ||
            (r->caps.maxWidthInPixels && t->hVisible > r->caps.maxWidthInPixels) ||
            (r->caps.maxHeightInPixels && t->vVisible > r->caps.maxHeightInPixels))
            continue;
        struct n_screen_mode m = mode_of(&mode);
        int at = mode_index(&m);
        bool first_preferred = preferred && !have_preferred;
        if (at >= 0) {
            if (first_preferred) {
                kmodes[at] = mode;
                offer.modes[at].flags |= N_SCREEN_MODE_PREFERRED;
                have_preferred = true;
            }
            continue;
        }
        if (offer.count == N_SCREEN_MODES_MAX) continue;
        if (first_preferred) {
            m.flags = N_SCREEN_MODE_PREFERRED;
            have_preferred = true;
        }
        kmodes[offer.count] = mode;
        offer.modes[offer.count++] = m;
    }
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

/* A surface of width x height for the system to draw the desktop into: video memory, RM's
   mapping of it and a view for the system of the BAR pages that mapping shows. False, with the
   reason logged, when one of them cannot be made; desk_free frees what was. */
static bool desk_create(struct desk *d, uint32_t width, uint32_t height) {
    if (!scr.res.caps.hasVideoMemory) {
        nvrm_log(LOG "the GPU has no video memory for a surface the system can draw into");
        return false;
    }
    d->width = width;
    d->height = height;
    d->pitch = pitch_of(width);
    d->bytes = ((uint64_t)d->pitch * height + PAGE_BYTES - 1) / PAGE_BYTES * PAGE_BYTES;
    d->memory = scanout_memory(d->bytes);
    if (!d->memory) {
        nvrm_log(LOG "cannot allocate %llu KiB of video memory for a %ux%u surface", (unsigned long long)(d->bytes >> 10),
                 (unsigned)width, (unsigned)height);
        return false;
    }
    if (!kapi.mapMemory(scr.device, d->memory, NVKMS_KAPI_MAPPING_TYPE_KERNEL, &d->pixels)) {
        d->pixels = NULL;
        nvrm_log(LOG "RM cannot map the surface");
        return false;
    }
    uint64_t phys = 0;
    d->view = mapping_view(d->pixels, d->bytes, &phys, &d->view_offset);
    int bar = bar_index_of(phys);
    if (d->view < 0) {
        if (!phys) nvrm_log(LOG "RM's mapping of the surface is not one stretch of a BAR");
        else
            nvrm_log(LOG "cannot make a view of BAR%d at 0x%llx for the system: %s", bar, (unsigned long long)phys,
                     n_result_text(d->view));
        d->view = -1;
        return false;
    }

    d->surface = surface_of(d->memory, width, height, d->pitch, NvKmsSurfaceMemoryFormatX8R8G8B8);
    if (!d->surface) {
        nvrm_log(LOG "NVKMS cannot make a %ux%u X8R8G8B8 surface", (unsigned)width, (unsigned)height);
        return false;
    }
    nvrm_log(LOG "surface %ux%u, pitch %u, %llu KiB of video memory at BAR%d offset 0x%llx (bus 0x%llx)",
             (unsigned)width, (unsigned)height, (unsigned)d->pitch, (unsigned long long)(d->bytes >> 10), bar,
             (unsigned long long)(bar >= 0 ? phys - nvrm_dev.info.bar[bar].address : 0), (unsigned long long)phys);
    return true;
}

/* What desk_create made, freed: neither the system nor the head shows it any more (NVKMS takes
   a surface off the heads before it destroys it). */
static void desk_free(struct desk *d) {
    if (d->view >= 0) n_handle_close(d->view);
    if (d->surface) kapi.destroySurface(scr.device, d->surface);
    if (d->pixels) kapi.unmapMemory(scr.device, d->memory, NVKMS_KAPI_MAPPING_TYPE_KERNEL, d->pixels);
    if (d->memory) kapi.freeMemory(scr.device, d->memory);
    *d = (struct desk){ .view = -1 };
}

/* A new surface's first pixels: what the boot screen shows, when it is the first surface and has
   the boot screen's size (the switch from the boot screen then shows no black or stale frame);
   black otherwise, or when the boot screen cannot be read. The system draws the desktop over it
   once it is given. */
static void desk_fill(const struct desk *d, bool first) {
    const struct n_pci_function *g = &nvrm_dev.info;
    uint8_t *dst = d->pixels;
    uint64_t row = (uint64_t)d->width * 4;
    n_result r = -N_ERR_INVALID;
    void *map = NULL;
    size_t map_bytes = 0;
    uint64_t off = 0;
    if (first && g->framebuffer_width == d->width && g->framebuffer_height == d->height && g->framebuffer_bpp == 32 &&
        g->framebuffer_pitch >= row) {
        uint64_t src_bytes = (uint64_t)g->framebuffer_pitch * (d->height - 1) + row;
        /* The system draws the boot screen write-combined; a view of it must be too (C42). */
        n_handle view = nvrm_bar_view(g->framebuffer_address, src_bytes, NV_MEMORY_WRITECOMBINED, N_MEMORY_READ, &off);
        map_bytes = (size_t)((off + src_bytes + PAGE_BYTES - 1) / PAGE_BYTES * PAGE_BYTES);
        r = view < 0 ? view : n_shared_map(view, map_bytes, N_MEMORY_READ, &map);
        if (view >= 0) n_handle_close(view);
        if (r < 0) nvrm_log(LOG "cannot read the boot screen (%s); the surface starts black", n_result_text(r));
    }
    if (r < 0) {
        for (uint32_t y = 0; y < d->height; y++) memset(dst + (uint64_t)y * d->pitch, 0, (size_t)row);
    } else {
        const uint8_t *src = (const uint8_t *)map + off;
        for (uint32_t y = 0; y < d->height; y++)
            memcpy(dst + (uint64_t)y * d->pitch, src + (uint64_t)y * g->framebuffer_pitch, (size_t)row);
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

/* Apps' images are not shown any more: the system does not take the desktop's surface with
   N_SCANOUT_DIRECT. The cursor goes (NVKMS takes it off the head first); an image on the screen
   leaves it once the system's state no longer names its app (follow). Under head_lock. */
static void direct_off(void) {
    scr.direct = false;
    cursor_destroy();
}

/* ------------------------------------------------------------------------------------------
 * Mode setting
 * ---------------------------------------------------------------------------------------- */

/* The head's primary layer shows surface (width x height) scaled to mode. */
static void primary_request(struct NvKmsKapiHeadRequestedConfig *h, struct NvKmsKapiSurface *surface, uint32_t width,
                            uint32_t height, const struct NvKmsKapiDisplayMode *mode) {
    struct NvKmsKapiLayerRequestedConfig *primary = &h->layerRequestedConfig[NVKMS_KAPI_LAYER_PRIMARY_IDX];
    struct NvKmsKapiLayerConfig *c = &primary->config;
    c->surface = surface;
    c->compParams.compMode = NVKMS_COMPOSITION_BLENDING_MODE_OPAQUE;
    c->minPresentInterval = 1;
    c->tearing = NV_FALSE;
    c->srcX = c->srcY = 0;
    c->srcWidth = (NvU16)width;
    c->srcHeight = (NvU16)height;
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

/* Adds head to the request: display shown in mode with d on its primary layer (scaled to the
   mode), its other layers and the cursor empty; display 0 turns the head off. */
static void head_request(uint32_t head, NvKmsKapiDisplay display, const struct NvKmsKapiDisplayMode *mode,
                         const struct desk *d) {
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
    primary_request(h, d->surface, d->width, d->height, mode);
    struct NvKmsKapiLayerRequestedConfig *primary = &h->layerRequestedConfig[NVKMS_KAPI_LAYER_PRIMARY_IDX];
    primary->flags.srcXYChanged = NV_TRUE;
    primary->flags.srcWHChanged = NV_TRUE;
    primary->flags.dstXYChanged = NV_TRUE;
    primary->flags.dstWHChanged = NV_TRUE;
}

/* Commits the request. flip is the head whose primary layer it puts a surface on, -1 none: that
   flip's completion is awaited by flip_completed (nvkms_kapi_rm.c reads the completion notifier
   KAPI names in the commit, since KAPI's table does not say). KAPI names one in every commit that
   puts a surface on a layer, a mode set's too (nvkms-kapi.c, as it builds the flip request), and
   NVKMS has the display engine write it (nvkms-hw-flip.c, nvAssignCompletionNotifierEvoHwState),
   whether or not the layer showed a surface before. */
static bool commit(int flip) {
    if (flip >= 0) nvrm_nvkms_flip_arm(scr.device, (uint32_t)flip, NVKMS_KAPI_LAYER_PRIMARY_IDX);
    memset(&reply, 0, sizeof reply);
    uint64_t start = nvrm_now_ns();
    if (!kapi.applyModeSetConfig(scr.device, &request, &reply, NV_TRUE) ||
        reply.flipResult != NV_KMS_FLIP_RESULT_SUCCESS)
        return false; /* nothing flipped: no notifier is written */
    if (flip >= 0) {
        scr.flipping = true;
        scr.flip_head = (uint32_t)flip;
        scr.flip_commit_ns = nvrm_now_ns() - start;
    }
    return true;
}

/* The request as NVKMS checks it, nothing committed. */
static bool checked(const char *what) {
    memset(&reply, 0, sizeof reply);
    if (kapi.applyModeSetConfig(scr.device, &request, &reply, NV_FALSE)) return true;
    nvrm_log(LOG "NVKMS rejects %s", what);
    return false;
}

/* The request, checked by NVKMS first and then committed (commit). */
static bool apply(const char *what, int flip) {
    if (!checked(what)) return false;
    if (!commit(flip)) {
        nvrm_log(LOG "%s failed (flip result %d)", what, (int)reply.flipResult);
        return false;
    }
    return true;
}

static void hand_back(const char *why);

/* The last commit that put a surface on a primary layer has completed. One whose flip is not
   seen to complete in FLIP_TIMEOUT_MS may have left the display showing nothing of nvrm's, or
   nothing at all: the screen is handed back (hand_back), so the firmware's framebuffer console
   NVKMS restores shows the boot screen the system draws again. The notifier's words are logged
   first, to tell a display engine that did not write it from one whose write is not seen. What a
   flip takes off the head is not freed while it is scanned out, as NVKMS takes a surface off the
   heads before it destroys it. */
static bool flip_completed(void) {
    if (!scr.flipping) return true;
    uint64_t until = nvrm_now_ns() + (uint64_t)FLIP_TIMEOUT_MS * 1000000u;
    while (!nvrm_nvkms_flip_done(scr.device, scr.flip_head, NVKMS_KAPI_LAYER_PRIMARY_IDX)) {
        if (nvrm_now_ns() >= until) {
            uint32_t slot = 0, words[2][4] = { { 0 } };
            nvrm_nvkms_flip_words(scr.device, scr.flip_head, NVKMS_KAPI_LAYER_PRIMARY_IDX, &slot, words);
            nvrm_log(LOG "the flip on head %u has not completed %d ms after its commit, which took NVKMS %llu ms; "
                     "the primary layer's completion notifier %u, of words %08x %08x %08x %08x and %08x %08x %08x "
                     "%08x", (unsigned)scr.flip_head, FLIP_TIMEOUT_MS,
                     (unsigned long long)(scr.flip_commit_ns / 1000000u), (unsigned)slot, words[0][0], words[0][1],
                     words[0][2], words[0][3], words[1][0], words[1][1], words[1][2], words[1][3]);
            scr.flipping = false;
            hand_back("nvrm's surface is not seen on the display");
            return false;
        }
        n_sleep_ms(1);
    }
    scr.flipping = false;
    return true;
}

/* The request of a mode set: display shown in mode on head with d on its primary layer, without
   the cursor; the head nvrm used before turned off when it is another one. */
static void mode_request(NvKmsKapiDisplay display, uint32_t head, const struct NvKmsKapiDisplayMode *mode,
                         const struct desk *d) {
    memset(&request, 0, sizeof request);
    if (scr.head_on && scr.head != head) head_request(scr.head, 0, NULL, NULL);
    head_request(head, display, mode, d);
}

/* The mode set mode_request made and NVKMS checked, committed, with its flip of the desktop's
   surface onto the head's primary layer awaited. *lit tells whether the commit went through;
   false is returned too when its flip has not completed (the screen is handed back then). */
static bool mode_set(NvKmsKapiDisplay display, uint32_t head, const struct NvKmsKapiDisplayMode *mode, bool *lit) {
    *lit = commit((int)head);
    if (!*lit) {
        nvrm_log(LOG "the mode set failed (flip result %d)", (int)reply.flipResult);
        return false;
    }
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
    head_request(scr.head, 0, NULL, NULL);
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

/* Where the cursor's top left goes for the pointer of st, in the mode's pixels (the desktop, and
   an image over it, are scaled to the mode). */
static void cursor_place(const struct n_screen_state *st, int32_t *x, int32_t *y) {
    int64_t px = ((int64_t)st->pointer_x - st->hot_x) * scr.mode.timings.hVisible / scr.desk.width;
    int64_t py = ((int64_t)st->pointer_y - st->hot_y) * scr.mode.timings.vVisible / scr.desk.height;
    *x = (int32_t)(px < INT16_MIN ? INT16_MIN : px > INT16_MAX ? INT16_MAX : px);
    *y = (int32_t)(py < INT16_MIN ? INT16_MIN : py > INT16_MAX ? INT16_MAX : py);
}

/* The head shows image (NULL: the desktop's surface) from its next vertical blank, with the
   pointer of st drawn by the cursor over an image; the desktop's surface has the pointer in it.
   Waits for the flip before to complete first, not for this one. A flip, not a mode set: NVKMS
   checks it as it commits it. */
static bool show(struct image *image, const struct n_screen_state *st) {
    if (!scr.head_on || !settle()) return false;
    bool cursor = image && st && st->pointer_shown && scr.cursor_surface;
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
    if (surface_changed) {
        if (image) primary_request(h, image->surface, image->width, image->height, &scr.mode);
        else primary_request(h, scr.desk.surface, scr.desk.width, scr.desk.height, &scr.mode);
    }
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
    if (image == scr.shown && !show(NULL, NULL) && scr.head_on) { /* not after hand_back: nothing is shown */
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
 * The system's screen: the modes offered, the one it asks for shown, the surface given
 * (head_lock, or the main thread before the threads start)
 * ---------------------------------------------------------------------------------------- */

/* d to the system: it draws the desktop into d, at d's size, from then on (n_pci_give_screen),
   with N_SCANOUT_DIRECT while nvrm shows apps' images. When the system refuses that and takes d
   without it, apps' images are not shown any more (direct_off). */
static n_result desk_give(const struct desk *d) {
    struct n_scanout desc = {
        .width = d->width,
        .height = d->height,
        .pitch = d->pitch,
        .format = N_SCANOUT_XRGB8888 | (scr.direct ? N_SCANOUT_DIRECT : 0),
        .offset = d->view_offset,
    };
    n_result r = n_pci_give_screen(nvrm_dev.pci, d->view, &desc);
    if (r < 0 && scr.direct) {
        desc.format = N_SCANOUT_XRGB8888;
        n_result plain = n_pci_give_screen(nvrm_dev.pci, d->view, &desc);
        if (plain >= 0) {
            nvrm_log(LOG "the system does not let nvrm show windows directly (%s); it composites them",
                     n_result_text(r));
            direct_off();
        }
        r = plain;
    }
    if (r < 0)
        nvrm_log(LOG "the system does not take the %ux%u surface: %s", (unsigned)d->width, (unsigned)d->height,
                 n_result_text(r));
    else __atomic_store_n(&scr.given, true, __ATOMIC_RELEASE);
    return r;
}

/* The boot screen back to the system: nothing of nvrm's is drawn into any more. */
static void give_boot(void) {
    if (!__atomic_load_n(&scr.given, __ATOMIC_ACQUIRE)) return;
    n_result r = n_pci_give_screen(nvrm_dev.pci, N_SCANOUT_BOOT, NULL);
    __atomic_store_n(&scr.given, false, __ATOMIC_RELEASE);
    if (r < 0) nvrm_log(LOG "the system does not take the boot screen back: %s", n_result_text(r));
}

/* The screen back to the system and the display to NVKMS's console, for good: the modes
   withdrawn (the system chooses none for a display nvrm no longer shows), the boot screen drawn
   again, then releaseOwnership, with which NVKMS shows its console surface (the firmware's
   framebuffer) on the boot displays again (nvkms.c RestoreConsole), or hands the display back to
   RM without one. Apps' images are not shown any more, and the desktop's surface is freed; the
   state thread ends, and nothing is followed until nvrm starts again. why, when not NULL, is
   logged first. Under head_lock, or by the main thread while no other thread runs. */
static void hand_back(const char *why) {
    if (why) nvrm_log(LOG "%s; the boot screen goes back to the system and the display to NVKMS's console", why);
    if (offered.count) {
        memset(&offer, 0, sizeof offer);
        n_result r = n_pci_screen_modes(nvrm_dev.pci, &offer);
        if (r < 0) nvrm_log(LOG "the system keeps the display's modes: %s", n_result_text(r));
    }
    memset(&offered, 0, sizeof offered);
    give_boot();
    vblank_off();
    if (scr.owner) {
        kapi.releaseOwnership(scr.device);
        nvrm_log(LOG "display ownership released; NVKMS restores the boot screen");
        scr.owner = false;
    }
    scr.head_on = false;
    scr.display = scr.target = 0;
    scr.shown = scr.leaving = NULL; /* nothing of nvrm's is shown any more */
    scr.flipping = scr.lost = false;
    scr.cursor_on = false;
    direct_off();
    desk_free(&scr.desk);
    __atomic_store_n(&scr.state_stop, true, __ATOMIC_RELEASE);
}

/* km, one of kmodes, shown on the offered display with a surface of its size for the desktop: a
   new one when the size changes, given to the system between NVKMS's check of the mode set and
   its commit, so the desktop is drawn at that size by the time the head shows it. True when the
   head shows km. When the commit fails, the head shows what it showed before, and the system
   draws into what it drew into before (the boot screen, when nvrm had no surface yet); when its
   flip is not seen to complete, the screen has been handed back (flip_completed). */
static bool mode_show(const struct NvKmsKapiDisplayMode *km) {
    NvKmsKapiDisplay display = scr.target;
    uint32_t head = scr.target_head, width = km->timings.hVisible, height = km->timings.vVisible;
    bool resize = !scr.desk.surface || scr.desk.width != width || scr.desk.height != height;
    bool given = __atomic_load_n(&scr.given, __ATOMIC_ACQUIRE);
    if (!resize && given && scr.head_on && !scr.lost && scr.display == display && scr.head == head &&
        same_mode(&scr.mode, km))
        return true;
    char text[48];
    mode_text(km, text, sizeof text);
    if (!kapi.validateDisplayMode(scr.device, display, km)) {
        nvrm_log(LOG "display 0x%08x cannot show %s", (unsigned)display, text);
        return false;
    }
    struct desk next = { .view = -1 };
    if (resize) {
        if (!desk_create(&next, width, height)) {
            desk_free(&next);
            return false;
        }
        desk_fill(&next, !scr.desk.surface);
    }
    struct desk *shown = resize ? &next : &scr.desk;
    mode_request(display, head, km, shown);
    if (!checked("the mode set") || ((resize || !given) && desk_give(shown) < 0)) {
        desk_free(&next);
        return false;
    }
    struct desk was = scr.desk;
    if (resize) scr.desk = next;
    bool lit = false, flipped = mode_set(display, head, km, &lit);
    if (!lit) {
        if (resize) {
            scr.desk = was;
            if (!was.surface || desk_give(&was) < 0) give_boot(); /* never into memory that is freed */
            desk_free(&next);
        }
        nvrm_log(LOG "display 0x%08x %s", (unsigned)display, scr.head_on ? "shows what it showed before" : "stays dark");
        return false;
    }
    if (resize) desk_free(&was); /* hand_back frees the new one */
    if (!flipped) return false;
    if (!scr.vblank || scr.vblank_head != head) {
        vblank_off();
        vblank_on(head);
    }
    nvrm_log(LOG "the desktop is shown on display 0x%08x (head %u) at %s", (unsigned)display, (unsigned)head, text);
    return true;
}

/* The offered display's modes to the system (none when there is no display), when they are not
   what it took last or anew is asked for (a display offered anew): the system then asks for a
   mode again (mode_follow_now). True when the system took them. */
static bool modes_offer(bool anew) {
    modes_list(scr.target);
    if (!anew && !memcmp(&offer, &offered, sizeof offer)) return false;
    n_result r = n_pci_screen_modes(nvrm_dev.pci, &offer);
    if (r < 0) {
        nvrm_log(LOG "the system does not take the modes of display 0x%08x: %s", (unsigned)scr.target, n_result_text(r));
        return false;
    }
    offered = offer;
    if (offer.count) {
        char preferred[48] = "none";
        for (uint32_t i = 0; i < offer.count; i++)
            if (offer.modes[i].flags & N_SCREEN_MODE_PREFERRED) mode_text(&kmodes[i], preferred, sizeof preferred);
        nvrm_log(LOG "display 0x%08x, monitor %s (%s): %u modes offered to the system, preferred %s",
                 (unsigned)scr.target, offer.monitor, offer.name, (unsigned)offer.count, preferred);
    }
    return true;
}

/* The mode the system asks for (st's, numbered mode_serial), each once: shown on the offered
   display, and what the head shows then said back (done), so a mode it could not show is
   refused and the system's next best one comes. The system numbers its wishes upwards; a state
   read before the last wish followed (the state thread's, while mode_follow_now followed a
   newer one) is not followed back, nor anything after hand_back (no modes are offered then). */
static void mode_follow(const struct n_screen_state *st) {
    if (st->mode_serial <= scr.mode_serial) return;
    scr.mode_serial = st->mode_serial;
    if (!st->mode.width || !offered.count) return;
    int i = mode_index(&st->mode);
    if (i < 0)
        nvrm_log(LOG "display 0x%08x has no mode %ux%u at %u.%03u Hz", (unsigned)scr.target, (unsigned)st->mode.width,
                 (unsigned)st->mode.height, (unsigned)(st->mode.refresh_mhz / 1000),
                 (unsigned)(st->mode.refresh_mhz % 1000));
    else if (!mode_show(&kmodes[i]) && !scr.owner)
        return; /* handed back: the modes are withdrawn */
    offer.done = st->mode_serial;
    if (scr.head_on && scr.display == scr.target && __atomic_load_n(&scr.given, __ATOMIC_ACQUIRE))
        offer.shown = mode_of(&scr.mode);
    n_result r = n_pci_screen_modes(nvrm_dev.pci, &offer);
    offer.done = 0;
    offer.shown = (struct n_screen_mode){ 0 };
    if (r < 0) nvrm_log(LOG "the system does not hear what display 0x%08x shows: %s", (unsigned)scr.target, n_result_text(r));
}

/* The mode the system asks for, read at once and followed until it no longer changes
   (MODE_TRIES at most): after modes are offered anew, without waiting for the state thread. */
static void mode_follow_now(void) {
    for (int i = 0; i < MODE_TRIES && scr.owner; i++) {
        if (n_pci_screen_state(nvrm_dev.pci, 0, &state_now, 0) < 0 || state_now.mode_serial <= scr.mode_serial) return;
        mode_follow(&state_now);
    }
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

/* The offered display still there: its modes again, offered when they changed (another EDID).
   Gone: its head turned off, and the next display's modes offered (none when no display is
   connected). The surface stays the system's throughout; a display lit again shows the desktop
   until the app shown directly presents its next image. Nothing after hand_back. Under head_lock. */
static void rescan(void) {
    if (!scr.owner) return;
    if (scr.target) {
        memset(&dyn, 0, sizeof dyn);
        dyn.handle = scr.target;
        if (kapi.getDynamicDisplayInfo(scr.device, &dyn) && dyn.connected) {
            if (modes_offer(false)) mode_follow_now();
            return;
        }
        nvrm_log(LOG "display 0x%08x is disconnected; its head is turned off, the desktop is still drawn",
                 (unsigned)scr.target);
        vblank_off();
        head_off();
        scr.display = 0;
    }
    scr.target = choose_display(&scr.target_head, true);
    if (modes_offer(true)) mode_follow_now();
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
 * What the system says the screen shows (the state thread) and apps' images (nvrm's client
 * threads)
 * ---------------------------------------------------------------------------------------- */

/* The screen's state changed: an image of an app the system no longer shows directly goes, the
   cursor follows the pointer over one that stays; a head left without a surface gets the
   desktop again. Under head_lock. */
static void follow(const struct n_screen_state *st) {
    if (scr.shown && scr.direct && st->app == scr.shown->app) show(scr.shown, st);
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
            /* Not while nvrm neither shows its surface nor offers modes; looked at again later. */
            if (__atomic_load_n(&scr.state_stop, __ATOMIC_ACQUIRE)) break; /* handed back */
            if (!refused) nvrm_log(LOG "the system does not say what the screen shows: %s", n_result_text(r));
            refused = true;
            n_sleep_ms(STATE_WAIT_MS);
            continue;
        }
        refused = false;
        seen = state_followed.serial;
        mtx_lock(&scr.head_lock);
        if (state_followed.take_back) {
            if (scr.owner) hand_back("the user asks for the boot screen back (Ctrl+Alt+Esc)");
        } else {
            mode_follow(&state_followed);
            follow(&state_followed);
        }
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
    n_handle view = mapping_view(image->pixels, image->bytes, &phys, &offset);
    if (view < 0) return view;
    struct n_scanout desc = {
        .width = image->width,
        .height = image->height,
        .pitch = image->pitch,
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
    image->width = scr.desk.width;
    image->height = scr.desk.height;
    image->pitch = scr.desk.pitch;
    image->bytes = scr.desk.bytes;
    image->memory = scanout_memory(image->bytes);
    if (image->memory)
        image->surface = surface_of(image->memory, image->width, image->height, image->pitch,
                                    NvKmsSurfaceMemoryFormatX8B8G8R8);
    /* Video memory comes as the last owner left it: black before the app may read it. RM's
       mapping stays for the system's panic screen. */
    if (!image->surface ||
        !kapi.mapMemory(scr.device, image->memory, NVKMS_KAPI_MAPPING_TYPE_KERNEL, &image->pixels)) {
        image->pixels = NULL;
        nvrm_log(LOG "cannot make a %ux%u image for app %u", (unsigned)image->width, (unsigned)image->height,
                 (unsigned)app);
        image_release(image);
        return NV_ERR_NO_MEMORY;
    }
    memset(image->pixels, 0, (size_t)image->bytes);
    __asm__ volatile("sfence" ::: "memory");
    image->panic = image_for_panic(image);
    if (image->panic < 0) {
        /* Not shown where a stopped kernel could not be seen over it: the app's window stays composited. */
        nvrm_log(LOG "the system cannot draw its panic screen into a %ux%u image (%s); app %u gets none",
                 (unsigned)image->width, (unsigned)image->height, n_result_text(image->panic), (unsigned)app);
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
    r->width = image->width;
    r->height = image->height;
    r->pitch = image->pitch;
    r->bytes = image->bytes;
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
    /* Requests are taken only between start and stop; outside them head_lock may not exist. */
    if (!__atomic_load_n(&scr.serving, __ATOMIC_ACQUIRE)) {
        r->status = NV_ERR_NOT_SUPPORTED;
        return 0;
    }
    if (r->op < NVRM_SCANOUT_CREATE || r->op > NVRM_SCANOUT_IMPORT) {
        r->status = NV_ERR_INVALID_ARGUMENT;
        return 0;
    }
    mtx_lock(&scr.head_lock);
    struct image *image = r->op == NVRM_SCANOUT_CREATE ? NULL : image_find(r->image);
    if (!scr.direct && r->op != NVRM_SCANOUT_DESTROY)
        r->status = NV_ERR_NOT_SUPPORTED; /* images made before direct scanout ended can still be freed */
    else if (r->op != NVRM_SCANOUT_CREATE && (!image || image->app != principal))
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
    if (!__atomic_load_n(&scr.serving, __ATOMIC_ACQUIRE)) return;
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

    scr.device = kapi.allocateDevice(&params);
    if (!scr.device) { why = "NVKMS cannot open the GPU"; goto fail; }
    scr.owner = kapi.grabOwnership(scr.device);
    if (!scr.owner) { why = "another NVKMS client owns the display"; goto fail; }
    scr.events = kapi.declareEventInterest(scr.device, 1u << NVKMS_EVENT_TYPE_DPY_CHANGED |
                                                       1u << NVKMS_EVENT_TYPE_DYNAMIC_DPY_CONNECTED);
    if (!scr.events) nvrm_log(LOG "NVKMS does not report displays coming and going; only the first one is set up");
    if (!kapi.getDeviceResourcesInfo(scr.device, &scr.res)) { why = "NVKMS cannot describe the display engine"; goto fail; }
    nvrm_log(LOG "%u heads, %u connectors, pitch alignment %u, surfaces up to %ux%u; boot screen %ux%u at 0x%llx, "
             "NVKMS's console surface at 0x%llx (%llu KiB)", (unsigned)scr.res.numHeads,
             (unsigned)scr.res.numConnectors, (unsigned)scr.res.caps.pitchAlignment,
             (unsigned)scr.res.caps.maxWidthInPixels, (unsigned)scr.res.caps.maxHeightInPixels,
             (unsigned)g->framebuffer_width, (unsigned)g->framebuffer_height,
             (unsigned long long)g->framebuffer_address, (unsigned long long)scr.res.vtFbBaseAddress,
             (unsigned long long)(scr.res.vtFbSize >> 10));
    if (!scr.res.vtFbSize)
        nvrm_log(LOG "NVKMS has no console surface: once nvrm stops, the boot screen is not shown again until restart");

    scr.boot_mask = boot_displays();
    nvrm_log(LOG "displays lit by the firmware: 0x%08x", (unsigned)scr.boot_mask);
    scr.target = choose_display(&scr.target_head, true);
    if (!scr.target) { why = "no display is connected"; goto fail; }

    /* Direct scanout needs the cursor, for the pointer over an app's image. */
    scr.direct = cursor_create();
    if (!scr.direct) cursor_destroy();
    if (!modes_offer(true)) { why = "the system does not take the display's modes"; goto fail; }
    if (!offer.count) { why = "the display has no mode NVKMS can show"; goto fail; }
    /* The system asks for its first mode at once: the monitor's preferred one, or the user's. */
    mode_follow_now();
    if (!scr.owner) {
        why = "the display was handed back";
        goto fail;
    }
    if (!__atomic_load_n(&scr.given, __ATOMIC_ACQUIRE) || !scr.head_on) {
        why = "no mode the system asked for could be shown";
        goto fail;
    }

    __atomic_store_n(&scr.state_stop, false, __ATOMIC_RELEASE);
    if (thrd_create(&scr.state_thread, state_thread, NULL) == thrd_success) scr.state_running = true;
    else {
        nvrm_log(LOG "cannot start the state thread; the screen keeps this mode and windows are not shown directly");
        if (scr.direct) {
            direct_off();
            /* The system would still hand fullscreen windows to nvrm, which shows none. */
            if (desk_give(&scr.desk) < 0) {
                why = "the system would show windows directly that nvrm does not show";
                goto fail;
            }
        }
    }
    __atomic_store_n(&scr.serving, true, __ATOMIC_RELEASE);

    if (thrd_create(&scr.thread, display_thread, NULL) == thrd_success) scr.thread_running = true;
    else nvrm_log(LOG "cannot start the display thread; displays coming and going are not followed");
    nvrm_log(LOG "the desktop is drawn into nvrm's surface, shown on display 0x%08x (head %u)%s", (unsigned)scr.display,
             (unsigned)scr.head, scr.direct ? "; apps' fullscreen windows can be shown directly" : "");
    return true;

fail:
    nvrm_log(LOG "%s; the screen stays with the firmware's framebuffer", why);
    nvrm_display_stop();
    return false;
}

void nvrm_display_stop(void) {
    __atomic_store_n(&scr.serving, false, __ATOMIC_RELEASE);
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
    hand_back(NULL);
    scr.mode_serial = 0;
    while (scr.images) image_free(scr.images);
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
