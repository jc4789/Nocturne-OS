/* nvrm's screen (ports/nvidia/display.c), the production file, against a fake KAPI, a fake RM
   kernel client, a fake RM file, fake native handle services and a fake of the system's choice of
   mode (kernel/src/gui/screenmode.c as the driver sees it). It checks the order of the bring-up
   and of the tear-down, the choice of display, the modes offered to the system (valid,
   progressive, each once, the EDID's preferred marked, the monitor told apart by its EDID), that
   the mode shown is the one the system asks for, at its size (a surface of that size given
   between NVKMS's check and its commit, the one before freed after; a refresh alone a mode set on
   the same surface), that a mode the display cannot show is said back and refused, that the
   system never draws into freed memory when a step fails, the reconnection of displays, and
   direct scanout: apps' images made, presented only while the system lets the app be shown
   directly (one flip waiting at a time), the pointer as the cursor over them, the desktop shown
   again when the system says so, images freed off the screen (DESTROY, the connection's end,
   stop), the system's hold on each image for its panic screen (none made without it) and
   IMPORT's ownership and RM sharing; and the screen handed back (a flip not performed) with the
   modes still offered, taken again for the system's next wish, a wish not
   shown said back once and never tried again by nvrm itself, the modes withdrawn only as nvrm
   stops. No GPU, NVKMS, QEMU or real hardware runs.
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
/* How many times word appears in calls. */
static unsigned times(const char *word) {
    char needle[80];
    snprintf(needle, sizeof needle, "%s ", word);
    unsigned n = 0;
    for (const char *f = strstr(calls, needle); f; f = strstr(f + 1, needle))
        if (f == calls || f[-1] == ' ') n++;
    return n;
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
/* How many times text was logged. */
static unsigned logged(const char *text) {
    unsigned n = 0;
    for (const char *f = strstr(logs, text); f; f = strstr(f + 1, text)) n++;
    return n;
}

/* ---------------------------------------------------------------- the machine */
#define W 1920u
#define H 1080u
#define GOP_PITCH 8192u
#define BAR1_BUS 0xd0000000ull
#define GOP_BUS (BAR1_BUS + 0x100000ull)
#define VRAM_BUS (BAR1_BUS + 0x4000000ull)
#define SLOT (64ull << 20)          /* memory i lies at VRAM_BUS + i * SLOT in BAR1 */
#define GOP_VIEW 40
#define IMAGE_VIEW 50
#define DESK_VIEW(i) (100 + (i))    /* the view of memory i, a desktop's surface */

struct nvrm_device nvrm_dev;
static uint8_t gop[GOP_PITCH * H];
static uint64_t clock_ns;

uint64_t nvrm_now_ns(void) { return clock_ns; }
void n_sleep_ms(uint64_t ms) { clock_ns += ms * 1000000u; }
const char *n_result_text(n_result r) { (void)r; return "error"; }

/* KAPI's memory and surfaces, in the order allocated: the cursor first, then the desktop's
   surface, then apps' images (and the desktop's next surfaces). Each memory's pixels are its
   own. */
#define POOL 16
static int mem_pool[POOL], surf_pool[POOL];
static uint8_t *mem_pixels[POOL];
static uint64_t mem_size[POOL];
static unsigned mem_used, surf_used, mem_freed, surf_destroyed;
static struct NvKmsKapiAllocateMemoryParams alloc_log[POOL];
static struct NvKmsKapiCreateSurfaceParams surface_log[POOL];
#define PIXEL(i, offset) (((uint32_t *)mem_pixels[i])[(offset) / 4])

/* Memory i is an app's image: it has an X8B8G8R8 surface. */
static bool image_memory(unsigned i) {
    for (unsigned s = 0; s < surf_used; s++)
        if (surface_log[s].planes[0].memory == (struct NvKmsKapiMemory *)&mem_pool[i] &&
            surface_log[s].format == NvKmsSurfaceMemoryFormatX8B8G8R8)
            return true;
    return false;
}

static uint64_t view_phys[4], view_bytes[4];
static uint32_t view_caching[4], view_access[4];
static int views;
n_handle nvrm_bar_view(uint64_t phys, uint64_t bytes, uint32_t nv_caching, uint32_t access, uint64_t *view_offset) {
    if (views < 4) { view_phys[views] = phys; view_bytes[views] = bytes; view_caching[views] = nv_caching; view_access[views] = access; }
    views++;
    *view_offset = 0;
    if (phys == GOP_BUS) { called("view-gop"); return GOP_VIEW; }
    unsigned i = phys >= VRAM_BUS ? (unsigned)((phys - VRAM_BUS) / SLOT) : POOL;
    if (i >= mem_used || (phys - VRAM_BUS) % SLOT || bytes != mem_size[i] || nv_caching != NV_MEMORY_WRITECOMBINED ||
        access != N_MEMORY_READ_WRITE)
        return -N_ERR_INVALID;
    if (image_memory(i)) {
        called("view-image");
        return IMAGE_VIEW;
    }
    called("view-desk");
    return DESK_VIEW(i);
}
static bool mapping_not_bar;
bool nvrm_kernel_mapping_phys(const void *address, uint64_t *phys) {
    const uint8_t *p = address;
    if (mapping_not_bar) return false;
    for (unsigned i = 0; i < mem_used; i++)
        if (mem_pixels[i] && p >= mem_pixels[i] && p < mem_pixels[i] + mem_size[i]) {
            *phys = VRAM_BUS + i * SLOT + (uint64_t)(p - mem_pixels[i]);
            return true;
        }
    return false;
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
static bool plain_refused;       /* the system refuses a surface without N_SCANOUT_DIRECT */
static struct n_scanout given_desc;
static n_handle given_surface;
/* What the system draws its panic screen into besides the scanout (n_pci_screen_image): the
   numbers it holds. It lets go of all of them when the scanout is no longer N_SCANOUT_DIRECT. */
#define PANIC_IMAGES 64
static bool panic_held[PANIC_IMAGES];
static n_result panic_last, panic_refused;
static struct n_scanout panic_desc;
static unsigned panic_images(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < PANIC_IMAGES; i++) n += panic_held[i];
    return n;
}
static struct n_screen_state screen;
/* The boot screen back: the system lets go of the images (pciuser.c screen_give_back). */
n_result n_pci_give_screen(n_handle pci, n_handle surface, const struct n_scanout *desc) {
    (void)pci;
    if (surface == N_SCANOUT_BOOT) {
        called("give-boot");
        given_surface = N_SCANOUT_BOOT;
        memset(panic_held, 0, sizeof panic_held);
        return 0;
    }
    called((desc->format & N_SCANOUT_DIRECT) ? "give-surface-direct" : "give-surface");
    if (direct_refused && (desc->format & N_SCANOUT_DIRECT)) return -N_ERR_INVALID;
    if (plain_refused && !(desc->format & N_SCANOUT_DIRECT)) return -N_ERR_INVALID;
    if (give_result) return give_result;
    given_surface = surface;
    given_desc = *desc;
    if (!(desc->format & N_SCANOUT_DIRECT)) memset(panic_held, 0, sizeof panic_held);
    return 0;
}
n_result n_pci_screen_image(n_handle pci, n_handle surface, const struct n_scanout *desc) {
    CHECK(pci == nvrm_dev.pci && surface == IMAGE_VIEW);
    called("panic-image");
    if (panic_refused) return panic_refused;
    panic_desc = *desc;
    CHECK(panic_last + 1 < PANIC_IMAGES);
    panic_held[++panic_last] = true;
    return panic_last;
}
n_result n_pci_screen_image_end(n_handle pci, n_result image) {
    CHECK(pci == nvrm_dev.pci);
    called("panic-image-end");
    if (image <= 0 || image >= PANIC_IMAGES || !panic_held[image]) return -N_ERR_INVALID;
    panic_held[image] = false;
    return 0;
}

/* What the system says the screen shows (n_pci_screen_state). A wait on a state already seen
   stops the state thread, so the test runs state_thread() once per change. A refusal stops it the
   second time in a row: a refusal seen once the thread is stopping is the end of a hand_back and
   not logged, so the first one must not stop it. */
static n_result screen_result;
static unsigned screen_refusals;
n_result n_pci_screen_state(n_handle pci, uint64_t seen, struct n_screen_state *out, int64_t timeout_ms) {
    CHECK(pci == nvrm_dev.pci);
    if (screen_result) {
        if (timeout_ms > 0 && ++screen_refusals >= 2) __atomic_store_n(&scr.state_stop, true, __ATOMIC_RELEASE);
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

/* The system's choice of mode, as screenmode.c makes it (the desktop running): the user's choice
   for the monitor, else its preferred mode, else its preferred size at the nearest refresh, else
   its largest size nearest 60 Hz; never a mode the driver could not show. A monitor offered
   again keeps the mode it was asked for. Each wish is numbered anew. */
static struct n_screen_modes modes_seen; /* what n_pci_screen_modes gave last */
static struct n_screen_mode user_mode;   /* the user's choice for the monitor (0: none) */
static struct n_screen_mode refused_modes[N_SCREEN_MODES_MAX];
static unsigned refused_count;
static n_result modes_result;
static bool same_mode_as(const struct n_screen_mode *a, const struct n_screen_mode *b) {
    return a->width == b->width && a->height == b->height && a->refresh_mhz == b->refresh_mhz;
}
static bool system_refused(const struct n_screen_mode *m) {
    for (unsigned i = 0; i < refused_count; i++)
        if (same_mode_as(&refused_modes[i], m)) return true;
    return false;
}
static const struct n_screen_mode *system_offered(const struct n_screen_mode *m) {
    for (uint32_t i = 0; i < modes_seen.count; i++)
        if (same_mode_as(&modes_seen.modes[i], m)) return &modes_seen.modes[i];
    return NULL;
}
static const struct n_screen_mode *system_nearest(uint32_t width, uint32_t height, uint32_t mhz) {
    const struct n_screen_mode *best = NULL;
    for (uint32_t i = 0; i < modes_seen.count; i++) {
        const struct n_screen_mode *m = &modes_seen.modes[i];
        if ((width && (m->width != width || m->height != height)) || system_refused(m)) continue;
        if (best) {
            uint64_t area = (uint64_t)m->width * m->height, best_area = (uint64_t)best->width * best->height;
            if (!width && area != best_area) {
                if (area < best_area) continue;
            } else {
                uint32_t d = m->refresh_mhz > mhz ? m->refresh_mhz - mhz : mhz - m->refresh_mhz;
                uint32_t bd = best->refresh_mhz > mhz ? best->refresh_mhz - mhz : mhz - best->refresh_mhz;
                if (d > bd || (d == bd && m->refresh_mhz <= best->refresh_mhz)) continue;
            }
        }
        best = m;
    }
    return best;
}
static struct n_screen_mode system_pick(void) {
    const struct n_screen_mode *m = user_mode.width ? system_offered(&user_mode) : NULL, *preferred = NULL;
    if (m && !system_refused(m)) return *m;
    for (uint32_t i = 0; i < modes_seen.count && !preferred; i++)
        if (modes_seen.modes[i].flags & N_SCREEN_MODE_PREFERRED) preferred = &modes_seen.modes[i];
    m = preferred ? system_nearest(preferred->width, preferred->height, preferred->refresh_mhz) : NULL;
    if (!m) m = system_nearest(0, 0, 60000);
    return m ? *m : (struct n_screen_mode){ 0 };
}
static void system_wishes(struct n_screen_mode m) {
    screen.mode = m;
    screen.mode_serial++;
    screen.serial++;
}
/* The user chooses (or tries) a mode in Settings. */
static void system_wants(uint32_t width, uint32_t height, uint32_t mhz) {
    system_wishes((struct n_screen_mode){ width, height, mhz, 0 });
}
n_result n_pci_screen_modes(n_handle pci, const struct n_screen_modes *m) {
    CHECK(pci == nvrm_dev.pci);
    if (modes_result) {
        called("modes-refused");
        return modes_result;
    }
    bool new_monitor = !modes_seen.count || strcmp(modes_seen.monitor, m->monitor);
    modes_seen = *m;
    if (!m->count) {
        called("modes-withdrawn");
        refused_count = 0;
        if (screen.mode.width) system_wishes((struct n_screen_mode){ 0 });
        return 0;
    }
    if (!m->done) {
        called("modes-offered");
        if (new_monitor) refused_count = 0;
        bool keep = !new_monitor && screen.mode.width && system_offered(&screen.mode) && !system_refused(&screen.mode);
        system_wishes(keep ? screen.mode : system_pick());
        return 0;
    }
    called("modes-shown");
    if (m->done == screen.mode_serial && screen.mode.width && !same_mode_as(&m->shown, &screen.mode)) {
        if (refused_count < N_SCREEN_MODES_MAX) refused_modes[refused_count++] = screen.mode;
        struct n_screen_mode p = system_pick();
        system_wishes(p.width ? p : m->shown);
    }
    return 0;
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

/* Single-threaded: the threads are created but never run; the test calls rescan() and
   state_thread(). */
static bool thread_fails;
int thrd_create(thrd_t *thr, thrd_start_t func, void *arg) {
    (void)thr; (void)func; (void)arg;
    called(thread_fails ? "thread-fails" : "thread");
    return thread_fails ? thrd_error : thrd_success;
}
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
static bool cursor_caps_missing;
static unsigned pending_polls;   /* polls before the flip in progress completes */
static unsigned commit_failures; /* commits NVKMS refuses next */
/* The flip whose completion notifier display.c cleared for the next commit. KAPI names a notifier
   in every commit that puts a surface on a layer, a mode set's that lights the head too
   (nvkms-kapi.c), and the display engine writes it "begun" with its time stamp when it performs
   the flip (nvkms-hw-flip.c; a window's notifier never reads "finished", nvkms-headsurface.c);
   flip_pending_forever is a display engine whose write is never seen. (0a1d812a's fake wrote it
   only for a layer that showed a surface before the commit, which NVKMS's source does not say.
   Until the 8th run on real hardware the fake only said "done", and display.c waited for
   "finished", which a window's notifier never reads: every mode set handed the screen back.) */
static int armed_head = -1;
static bool armed_notified;      /* the armed flip's notifier is written */
static bool armed_polled;        /* its completion has been asked about */

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
static bool grab_fails; /* another NVKMS client owns the display */
static NvBool k_grab(struct NvKmsKapiDevice *d) { (void)d; called("grabOwnership"); return !grab_fails; }
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
    info->caps.maxWidthInPixels = 8192;
    info->caps.maxHeightInPixels = 8192;
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
    /* Turing and later: the heads have an output LUT (nvkms-evo3.c, FillLUTCaps), so KAPI sends the
       mode set's olutFpNormScale to NVKMS. */
    info->lutCaps.olut.supported = NV_TRUE;
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
/* Each monitor's EDID: Dell, product 0x12nn (nn the display's handle), serial 0x00c0ffee, named
   "TEST MON". */
static NvBool k_dynamic(struct NvKmsKapiDevice *d, struct NvKmsKapiDynamicDisplayParams *p) {
    (void)d;
    struct fake_display *f = find_display(p->handle);
    if (!f) return NV_FALSE;
    p->connected = f->connected;
    if (f->connected) {
        uint8_t edid[128] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0x10, 0xac, (uint8_t)p->handle, 0x12,
            0xee, 0xff, 0xc0, 0x00,
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
    mode->timings.pixelClockHz = (NvU64)m->w * m->h * m->rr / 1000 + index; /* each timing its own */
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
        if (m->w == mode->timings.hVisible && m->h == mode->timings.vVisible && m->rr == mode->timings.refreshRate &&
            m->valid && !m->interlaced)
            return !m->reject;
    }
    return NV_FALSE;
}
static struct NvKmsKapiMemory *k_alloc(struct NvKmsKapiDevice *d, struct NvKmsKapiAllocateMemoryParams *p) {
    (void)d;
    called("allocateMemory");
    if (alloc_fails || mem_used == POOL) return NULL;
    CHECK(p->size && p->size <= SLOT && p->size % 4096 == 0);
    alloc_log[mem_used] = *p;
    mem_size[mem_used] = p->size;
    mem_pixels[mem_used] = malloc((size_t)p->size);
    if (!mem_pixels[mem_used]) abort();
    memset(mem_pixels[mem_used], 0xee, (size_t)p->size); /* as the last owner left it */
    return (struct NvKmsKapiMemory *)&mem_pool[mem_used++];
}
static void k_free_memory(struct NvKmsKapiDevice *d, struct NvKmsKapiMemory *m) {
    (void)d; (void)m;
    called("freeMemory");
    mem_freed++;
}
static uint8_t *memory_pixels(const struct NvKmsKapiMemory *m) {
    unsigned i = (unsigned)((const int *)m - mem_pool);
    return i < mem_used ? mem_pixels[i] : NULL;
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
    if (!commit) return NV_TRUE;
    /* A commit that fails names no notifier, nor does one that puts no surface on the primary layer:
       the last flip's notifier stays as it is. */
    int head = armed_head;
    armed_head = -1;
    if (commit_failures) {
        commit_failures--;
        reply_config->flipResult = NV_KMS_FLIP_RESULT_INVALID_PARAMS;
        return NV_FALSE;
    }
    last_request = *req;
    if (head >= 0) {
        const struct NvKmsKapiHeadRequestedConfig *c = &req->headRequestedConfig[head];
        const struct NvKmsKapiLayerRequestedConfig *p = &c->layerRequestedConfig[NVKMS_KAPI_LAYER_PRIMARY_IDX];
        armed_notified = (req->headsMask & 1u << head) && c->modeSetConfig.bActive && p->flags.surfaceChanged &&
                         p->config.surface;
    }
    reply_config->flipResult = NV_KMS_FLIP_RESULT_SUCCESS;
    return NV_TRUE;
}
/* The completion notifier of the primary layer's last flip (nvkms_kapi_rm.c reads KAPI's),
   cleared for the commit that follows. */
void nvrm_nvkms_flip_arm(struct NvKmsKapiDevice *d, uint32_t head, uint32_t layer) {
    CHECK(d == (struct NvKmsKapiDevice *)&dev_obj && layer == NVKMS_KAPI_LAYER_PRIMARY_IDX);
    called("flipArm-%u", (unsigned)head);
    armed_head = (int)head;
    armed_polled = false;
}
/* The words of the layer's two notifiers, for the log of a flip not seen: as nvKmsResetNotifier
   leaves them, "not begun" with the invalid time stamp 1 (nvkms-sync.c), as the 8th run on real
   hardware showed for the slot no commit named. */
void nvrm_nvkms_flip_words(const struct NvKmsKapiDevice *d, uint32_t head, uint32_t layer, uint32_t *slot,
                           uint32_t words[2][4]) {
    CHECK(d == (const struct NvKmsKapiDevice *)&dev_obj && layer == NVKMS_KAPI_LAYER_PRIMARY_IDX);
    called("flipWords-%u", (unsigned)head);
    *slot = 1;
    for (unsigned i = 0; i < 2; i++) {
        words[i][0] = words[i][1] = words[i][3] = 0;
        words[i][2] = 1;
    }
}
/* The display engine's time stamp of a flip: the GPU's timer, which RM set to the system's time. */
#define FLIP_STAMP_NS 0x18e1aa5b68bd73a0ull /* the 8th run's: 2026-10-25 04:10:36.246 UTC */
bool nvrm_nvkms_flip_done(const struct NvKmsKapiDevice *d, uint32_t head, uint32_t layer, struct nvrm_flip_seen *seen) {
    CHECK(d == (const struct NvKmsKapiDevice *)&dev_obj && layer == NVKMS_KAPI_LAYER_PRIMARY_IDX);
    /* A flip that never completes is polled every millisecond: record its first poll only. */
    bool never_done = flip_pending_forever || !armed_notified;
    if (!never_done || !armed_polled) called("flipPending-%u", (unsigned)head);
    armed_polled = true;
    bool pending = never_done || pending_polls;
    if (pending_polls) pending_polls--;
    *seen = (struct nvrm_flip_seen){ .slot = 1 };
    if (!pending) {
        seen->status = 1; /* NVKMS_NOTIFIER_STATUS_BEGUN (nvkms_os.h) */
        seen->time_ns = FLIP_STAMP_NS;
        seen->time_valid = true;
    }
    return !pending;
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
/* The CRC32s of what the head shows: NVKMS reads them only for an active head it owns. */
static NvBool k_crc(struct NvKmsKapiDevice *d, NvU32 head, struct NvKmsKapiCrcs *crc) {
    CHECK(d == (struct NvKmsKapiDevice *)&dev_obj);
    called("getCRC32-%u", (unsigned)head);
    crc->compositorCrc32 = (struct NvKmsKapiDpyCRC32){ .value = 0x12345678u, .supported = NV_TRUE };
    crc->rasterGeneratorCrc32 = (struct NvKmsKapiDpyCRC32){ .value = 0x9abcdef0u, .supported = NV_TRUE };
    crc->outputCrc32 = (struct NvKmsKapiDpyCRC32){ .supported = NV_FALSE };
    return NV_TRUE;
}

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
    t->registerVblankIntrCallback = k_vblank_on;
    t->unregisterVblankIntrCallback = k_vblank_off;
    t->framebufferConsoleDisabled = k_console_disabled;
    t->getCRC32 = k_crc;
    return NV_TRUE;
}

/* ---------------------------------------------------------------- scenarios */
static void reset(void) {
    memset(&scr, 0, sizeof scr);
    scr.desk.view = -1;
    memset(&offer, 0, sizeof offer);
    memset(&offered, 0, sizeof offered);
    calls[0] = logs[0] = 0;
    views = 0;
    give_result = 0;
    direct_refused = cursor_caps_missing = false;
    memset(&screen, 0, sizeof screen);
    screen.serial = 1;
    screen.hot_x = screen.hot_y = 8;
    screen_result = 0;
    screen_refusals = 0;
    memset(&modes_seen, 0, sizeof modes_seen);
    memset(&user_mode, 0, sizeof user_mode);
    refused_count = 0;
    modes_result = 0;
    shared_with = shared_object = 0;
    shares = 0;
    memset(last_share, 0, sizeof last_share);
    memset(&last_dup, 0, sizeof last_dup);
    dup_file = NULL;
    dup_result = 0;
    for (unsigned i = 0; i < POOL; i++) {
        free(mem_pixels[i]);
        mem_pixels[i] = NULL;
        mem_size[i] = 0;
    }
    mem_used = surf_used = mem_freed = surf_destroyed = 0;
    pending_polls = commit_failures = 0;
    armed_head = -1;
    armed_notified = armed_polled = false;
    thread_fails = plain_refused = false;
    memset(panic_held, 0, sizeof panic_held);
    panic_last = panic_refused = 0;
    memset(&panic_desc, 0, sizeof panic_desc);
    given_surface = -1;
    memset(&given_desc, 0, sizeof given_desc);
    flip_pending_forever = apply_check_fails = alloc_fails = mapping_not_bar = grab_fails = false;
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
    nvrm_dev.info.bar[1].size = 2ull << 30;
    nvrm_dev.info.bar[1].flags = N_PCI_BAR_MEMORY | N_PCI_BAR_PREFETCH | N_PCI_BAR_64BIT;
    /* display 0x1: not connected; 0x2: lit by the firmware, heads 1 and 2; 0x4: another monitor. */
    display_count = 3;
    displays[0] = (struct fake_display){ .handle = 0x1, .head_mask = 0xf, .connected = false };
    displays[1] = (struct fake_display){
        .handle = 0x2, .head_mask = 0x6, .connected = true, .mode_count = 8,
        .modes = {
            { 2560, 1440, 144000, true, false, false, false },
            { W, H, 60000, true, false, false, false },
            { W, H, 143998, false, false, false, false },      /* not valid: not offered */
            { W, H, 144000, true, false, true, false },        /* interlaced: not offered */
            { W, H, 120000, true, true, false, false },        /* the EDID's preferred */
            { 1280, 720, 144000, true, false, false, false },
            { W, H, 60000, true, false, false, false },        /* the same again: offered once */
            { 10240, 4320, 60000, true, false, false, false }, /* wider than the engine's surfaces */
        } };
    displays[2] = (struct fake_display){
        .handle = 0x4, .head_mask = 0x1, .connected = true, .mode_count = 1,
        .modes = { { 3840, 2160, 60000, true, true, false, false } } };
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) ((uint32_t *)(gop + (size_t)y * GOP_PITCH))[x] = y << 16 | x;
}

static const struct NvKmsKapiHeadRequestedConfig *head_cfg(unsigned head) {
    return &last_request.headRequestedConfig[head];
}

#define SURF(i) ((struct NvKmsKapiSurface *)&surf_pool[i])

static void bring_up_and_tear_down(void) {
    reset();
    CHECK(nvrm_display_start());
    CHECK(in_order("allocateDevice grabOwnership declareEventInterest-3 getDeviceResourcesInfo "
                   "rm-alloc-0 rm-control-205 rm-alloc-80 rm-alloc-73 rm-control-73011e rm-free getDisplays "
                   "allocateMemory mapMemory createSurface modes-offered "
                   "allocateMemory mapMemory view-desk createSurface view-gop map-gop close40 unmap-gop "
                   "apply-check give-surface-direct flipArm-1 apply-commit flipPending-1 registerVblank-1 modes-shown "
                   "thread thread"));
    /* The mode set that lights the head puts the desktop's surface on its primary layer: KAPI
       names a completion notifier for that flip too, and it is awaited. */
    CHECK(!scr.flipping);
    CHECK(interest == (1u << NVKMS_EVENT_TYPE_DPY_CHANGED | 1u << NVKMS_EVENT_TYPE_DYNAMIC_DPY_CONNECTED));
    CHECK(never("framebufferConsoleDisabled"));
    /* The cursor: 32x32 A8R8G8B8, pitch 256 (NVKMS's least for that size), video memory, mapped
       for the pointer's pixels; made before the surface is given with N_SCANOUT_DIRECT. */
    CHECK(mem_used == 2 && surf_used == 2);
    CHECK(alloc_log[0].layout == NvKmsSurfaceMemoryLayoutPitch && alloc_log[0].useVideoMemory &&
          alloc_log[0].size == 8192 && alloc_log[0].type == NVKMS_KAPI_ALLOCATION_TYPE_SCANOUT);
    CHECK(surface_log[0].format == NvKmsSurfaceMemoryFormatA8R8G8B8 && surface_log[0].width == 32 &&
          surface_log[0].height == 32 && surface_log[0].planes[0].pitch == 256 &&
          surface_log[0].planes[0].memory == (struct NvKmsKapiMemory *)&mem_pool[0]);
    /* The desktop's surface: pitch-linear video memory for scanout of the mode's size, the pitch
       aligned as NVKMS asks. */
    CHECK(alloc_log[1].layout == NvKmsSurfaceMemoryLayoutPitch && alloc_log[1].type == NVKMS_KAPI_ALLOCATION_TYPE_SCANOUT);
    CHECK(alloc_log[1].useVideoMemory && alloc_log[1].noDisplayCaching && alloc_log[1].size == 7680ull * H);
    CHECK(surface_log[1].format == NvKmsSurfaceMemoryFormatX8R8G8B8 && surface_log[1].planes[0].pitch == 7680 &&
          surface_log[1].width == W && surface_log[1].height == H);
    /* The views: the surface write-combined and writable, the boot screen write-combined, read. */
    CHECK(view_phys[0] == VRAM_BUS + SLOT && view_bytes[0] == 7680ull * H && view_caching[0] == NV_MEMORY_WRITECOMBINED &&
          view_access[0] == N_MEMORY_READ_WRITE);
    CHECK(view_phys[1] == GOP_BUS && view_caching[1] == NV_MEMORY_WRITECOMBINED && view_access[1] == N_MEMORY_READ);
    /* The boot screen's pixels, row by row into the other pitch (the mode has its size). */
    CHECK(PIXEL(1, 0) == 0 && PIXEL(1, 7680u * 500 + 700 * 4) == (500u << 16 | 700));
    CHECK(PIXEL(1, 7680u * (H - 1) + (W - 1) * 4) == ((H - 1) << 16 | (W - 1)));
    /* The modes offered: the valid progressive ones the engine's surfaces can have, each once,
       the EDID's preferred marked; the monitor told apart by its EDID's maker, product and
       serial number, and named by its monitor name descriptor. */
    CHECK(modes_seen.count == 4 && !strcmp(modes_seen.monitor, "DEL1202-00c0ffee") && !strcmp(modes_seen.name, "TEST MON"));
    CHECK(modes_seen.modes[0].width == 2560 && modes_seen.modes[0].height == 1440 &&
          modes_seen.modes[0].refresh_mhz == 144000 && !modes_seen.modes[0].flags);
    CHECK(modes_seen.modes[1].width == W && modes_seen.modes[1].refresh_mhz == 60000 && !modes_seen.modes[1].flags);
    CHECK(modes_seen.modes[2].width == W && modes_seen.modes[2].refresh_mhz == 120000 &&
          modes_seen.modes[2].flags == N_SCREEN_MODE_PREFERRED);
    CHECK(modes_seen.modes[3].width == 1280 && modes_seen.modes[3].refresh_mhz == 144000);
    /* The mode the system asked for (the monitor's preferred one) on display 0x2, on its lowest
       head, the surface unscaled; said back as shown. */
    CHECK(last_request.headsMask == 0x2);
    const struct NvKmsKapiHeadRequestedConfig *h = head_cfg(1);
    CHECK(h->modeSetConfig.bActive && h->modeSetConfig.numDisplays == 1 && h->modeSetConfig.displays[0] == 0x2);
    CHECK(h->modeSetConfig.mode.timings.hVisible == W && h->modeSetConfig.mode.timings.refreshRate == 120000);
    CHECK(h->flags.activeChanged && h->flags.displaysChanged && h->flags.modeChanged);
    /* The output LUT's normalisation scale is 1.0: 0 makes the head send black (6th and 7th runs
       on real hardware, lit and dark). */
    CHECK(h->modeSetConfig.olutFpNormScale == NVKMS_OLUT_FP_NORM_SCALE_DEFAULT);
    const struct NvKmsKapiLayerRequestedConfig *p = &h->layerRequestedConfig[0];
    CHECK(p->config.surface == SURF(1) && p->flags.surfaceChanged);
    CHECK(p->config.srcWidth == W && p->config.srcHeight == H && p->config.dstWidth == W && p->config.dstHeight == H);
    CHECK(p->config.csc.m[0][0] == 0x10000 && p->config.minPresentInterval == 1);
    CHECK(h->layerRequestedConfig[1].config.surface == NULL && h->layerRequestedConfig[1].flags.surfaceChanged);
    CHECK(!h->layerRequestedConfig[3].flags.surfaceChanged);
    CHECK(modes_seen.done == screen.mode_serial && modes_seen.shown.width == W && modes_seen.shown.height == H &&
          modes_seen.shown.refresh_mhz == 120000);
    /* What the system was given. */
    CHECK(given_surface == DESK_VIEW(1) && given_desc.width == W && given_desc.height == H &&
          given_desc.pitch == 7680 && given_desc.format == (N_SCANOUT_XRGB8888 | N_SCANOUT_DIRECT) &&
          given_desc.offset == 0);
    CHECK(scr.direct && scr.state_running && scr.thread_running);
    CHECK(strstr(logs, "display 0x00000002: DP-2, heads 0x6, connected, DEL1202 TEST MON, lit by the firmware"));
    CHECK(strstr(logs, "monitor DEL1202-00c0ffee (TEST MON): 4 modes offered to the system, preferred 1920x1080 at 120.000 Hz"));
    CHECK(strstr(logs, "the desktop is shown on display 0x00000002 (head 1) at 1920x1080 at 120.000 Hz"));
    /* The mode set's flip, performed when its notifier reads "begun" (the display engine never
       writes a window's "finished"), logged with the GPU's time stamp; the head read back after. */
    CHECK(strstr(logs, "the flip on head 1 was performed: the primary layer's completion notifier 1 reads begun, "
                       "written at the GPU's time 1792901436.246160288 s, "));
    CHECK(in_order("flipPending-1 registerVblank-1 getCRC32-1 modes-shown"));
    CHECK(strstr(logs, "head 1 after the mode set: display 0x00000002 at 1920x1080 at 120.000 Hz, output LUT scale "
                       "0xffffffff in each mode set, 0 vertical blanks; CRC32 compositor 12345678, raster generator "
                       "9abcdef0, output none"));

    /* Vertical blanks go to the system, counted, with NVKMS's timestamp. */
    CHECK(vblank_proc != NULL);
    if (vblank_proc) { vblank_proc(0, 123456789); vblank_proc(0, 0); }
    CHECK(vblank_calls == 2 && vblank_count == 2 && vblank_ts == clock_ns && nvrm_display_vblanks() == 2);

    calls[0] = 0;
    nvrm_display_stop();
    /* Both threads joined; the modes withdrawn and the boot screen back before ownership is
       released; the cursor and the surface (its view first) freed after. */
    CHECK(in_order("join join modes-withdrawn give-boot unregisterVblank-1 releaseOwnership destroySurface unmapMemory "
                   "freeMemory close101 destroySurface unmapMemory freeMemory declareEventInterest-0 freeDevice"));
    CHECK(never("getCRC32-1"));
    CHECK(!scr.direct && !scr.state_running && mem_freed == 2 && surf_destroyed == 2 && !scr.desk.surface);
    CHECK(modes_seen.count == 0 && screen.mode.width == 0 && !offered.count);
    /* After the boot screen is back, a late blank is not passed on. */
    vblank_calls = 0;
    scr.vblank = NULL;
    vblank_arrived(0, 1);
    CHECK(vblank_calls == 0);
}

/* The mode the system asks for is the one shown: another size gets a surface of its own, given
   between NVKMS's check and its commit, the one before freed once the head no longer shows it; a
   refresh rate alone is a mode set on the same surface; a mode the display cannot show is said
   back, refused, and the system's next one comes. */
static void modes_from_the_system(void) {
    reset();
    CHECK(nvrm_display_start());

    /* 60 Hz at the same size: a mode set, no new surface, nothing given again; its flip's
       completion awaited, as every mode set's. */
    system_wants(W, H, 60000);
    calls[0] = 0;
    follow_screen();
    CHECK(in_order("apply-check flipArm-1 apply-commit flipPending-1 modes-shown"));
    CHECK(never("allocateMemory") && never("give-surface-direct") && never("destroySurface"));
    CHECK(head_cfg(1)->modeSetConfig.mode.timings.refreshRate == 60000 && head_cfg(1)->flags.modeChanged &&
          head_cfg(1)->layerRequestedConfig[0].config.surface == SURF(1));
    CHECK(modes_seen.done == screen.mode_serial && modes_seen.shown.refresh_mhz == 60000);
    /* The same wish again (a newer number): said back, nothing set. */
    system_wants(W, H, 60000);
    calls[0] = 0;
    follow_screen();
    CHECK(in_order("modes-shown") && never("apply-check") && never("apply-commit") && modes_seen.done == screen.mode_serial);

    /* 2560x1440: a surface of that size, black, given before the commit; the one before freed
       after its flip completed. */
    system_wants(2560, 1440, 144000);
    calls[0] = 0;
    follow_screen();
    CHECK(in_order("allocateMemory mapMemory view-desk createSurface apply-check give-surface-direct flipArm-1 "
                   "apply-commit flipPending-1 close101 destroySurface unmapMemory freeMemory modes-shown"));
    CHECK(never("view-gop") && PIXEL(2, 0) == 0 && PIXEL(2, 10240u * 1439 + 2559 * 4) == 0);
    CHECK(given_surface == DESK_VIEW(2) && given_desc.width == 2560 && given_desc.height == 1440 &&
          given_desc.pitch == 10240 && given_desc.format == (N_SCANOUT_XRGB8888 | N_SCANOUT_DIRECT));
    const struct NvKmsKapiHeadRequestedConfig *h = head_cfg(1);
    CHECK(h->modeSetConfig.mode.timings.hVisible == 2560 && h->modeSetConfig.mode.timings.refreshRate == 144000);
    CHECK(h->layerRequestedConfig[0].config.surface == SURF(2) && h->layerRequestedConfig[0].config.srcWidth == 2560 &&
          h->layerRequestedConfig[0].config.dstWidth == 2560 && h->layerRequestedConfig[0].config.srcHeight == 1440);
    CHECK(scr.desk.width == 2560 && scr.desk.height == 1440 && scr.desk.pitch == 10240 && mem_freed == 1);
    CHECK(modes_seen.shown.width == 2560 && modes_seen.shown.refresh_mhz == 144000 && modes_seen.done == screen.mode_serial);
    CHECK(strstr(logs, "the desktop is shown on display 0x00000002 (head 1) at 2560x1440 at 144.000 Hz"));

    /* A mode NVKMS says the display cannot show: nothing made or set, the mode shown said back,
       the system refuses the mode and asks for its next best (the preferred one). */
    displays[1].modes[5].reject = true;
    system_wants(1280, 720, 144000);
    calls[0] = 0;
    follow_screen();
    CHECK(strstr(logs, "display 0x00000002 cannot show 1280x720 at 144.000 Hz"));
    CHECK(in_order("modes-shown allocateMemory apply-check give-surface-direct apply-commit modes-shown"));
    CHECK(screen.mode.width == W && screen.mode.refresh_mhz == 120000 && scr.desk.width == W &&
          modes_seen.shown.width == W && modes_seen.shown.refresh_mhz == 120000 && modes_seen.done == screen.mode_serial);
    CHECK(refused_count == 1 && refused_modes[0].width == 1280);
    /* A mode the display does not have at all: the same. */
    system_wants(800, 600, 60000);
    calls[0] = 0;
    follow_screen();
    CHECK(strstr(logs, "display 0x00000002 has no mode 800x600 at 60.000 Hz") && never("apply-commit"));
    CHECK(screen.mode.width == W && modes_seen.done == screen.mode_serial);

    /* The commit of a new size fails: the head shows what it showed, and the system draws into
       the surface it drew into before (never into the new one, which is freed); the mode is
       refused. */
    uint32_t desk_memory = mem_used - 1;
    system_wants(2560, 1440, 144000);
    commit_failures = 1;
    calls[0] = 0;
    follow_screen();
    char seq[160];
    snprintf(seq, sizeof seq, "give-surface-direct apply-commit give-surface-direct close%d destroySurface unmapMemory "
             "freeMemory modes-shown", DESK_VIEW(mem_used - 1));
    CHECK(in_order(seq));
    CHECK(given_surface == DESK_VIEW(desk_memory) && given_desc.width == W && scr.desk.width == W && scr.head_on);
    CHECK(strstr(logs, "display 0x00000002 shows what it showed before") && screen.mode.width == W &&
          modes_seen.shown.width == W);

    /* Monitor events with the same modes offer nothing again; changed modes are offered again,
       and the mode shown, still among them, stays. */
    calls[0] = 0;
    rescan();
    CHECK(never("modes-offered") && never("apply-commit"));
    displays[1].modes[0].valid = false;
    calls[0] = 0;
    rescan();
    CHECK(in_order("modes-offered modes-shown") && never("apply-commit") && modes_seen.count == 3);
    CHECK(screen.mode.width == W && modes_seen.shown.width == W);

    calls[0] = 0;
    nvrm_display_stop();
    CHECK(in_order("modes-withdrawn give-boot releaseOwnership") && mem_freed == mem_used && surf_destroyed == surf_used);
}

/* The user's choice for the monitor, from the start: shown at once, not the boot screen's size,
   so the first surface starts black. */
static void users_mode_at_start(void) {
    reset();
    user_mode = (struct n_screen_mode){ 2560, 1440, 144000, 0 };
    CHECK(nvrm_display_start());
    CHECK(never("view-gop") && head_cfg(1)->modeSetConfig.mode.timings.hVisible == 2560);
    CHECK(given_desc.width == 2560 && given_desc.pitch == 10240 && PIXEL(1, 0) == 0 && mem_used == 2);
    nvrm_display_stop();

    /* NVKMS refuses the preferred mode: the system's next best, the preferred size at 60 Hz. */
    reset();
    displays[1].modes[4].reject = true;
    CHECK(nvrm_display_start());
    CHECK(in_order("modes-offered modes-shown allocateMemory apply-check give-surface-direct apply-commit modes-shown"));
    CHECK(head_cfg(1)->modeSetConfig.mode.timings.refreshRate == 60000 && modes_seen.shown.refresh_mhz == 60000);
    CHECK(strstr(logs, "cannot show 1920x1080 at 120.000 Hz"));
    nvrm_display_stop();

    /* The first mode set's commit fails: the boot screen back to the system before the surface
       is freed, then the next mode. */
    reset();
    commit_failures = 1;
    CHECK(nvrm_display_start());
    CHECK(in_order("give-surface-direct apply-commit give-boot close101 destroySurface unmapMemory freeMemory modes-shown "
                   "allocateMemory give-surface-direct apply-commit modes-shown"));
    CHECK(strstr(logs, "display 0x00000002 stays dark") && scr.head_on && scr.given &&
          given_surface == DESK_VIEW(2) && modes_seen.shown.refresh_mhz == 60000);
    nvrm_display_stop();
}

static void nothing_given_on_failure(void) {
    /* No mode NVKMS can show: none offered, nothing made or given. */
    reset();
    displays[1].mode_count = 0;
    CHECK(!nvrm_display_start());
    CHECK(never("apply-commit") && never("give-surface") && never("give-surface-direct") && never("give-boot"));
    CHECK(in_order("grabOwnership modes-withdrawn releaseOwnership declareEventInterest-0 freeDevice"));
    CHECK(strstr(logs, "the display has no mode NVKMS can show") && mem_freed == mem_used);

    /* The system does not take the modes: nothing made or given, nothing to withdraw. */
    reset();
    modes_result = -N_ERR_DENIED;
    CHECK(!nvrm_display_start());
    CHECK(never("view-desk") && never("apply-commit") && never("give-surface-direct") && never("modes-withdrawn"));
    CHECK(mem_freed == mem_used && surf_destroyed == surf_used);

    /* NVKMS rejects every mode set: each surface freed, nothing given. */
    reset();
    apply_check_fails = true;
    CHECK(!nvrm_display_start());
    CHECK(never("apply-commit") && never("give-surface") && never("give-surface-direct") && never("give-boot"));
    CHECK(in_order("apply-check close101 destroySurface unmapMemory freeMemory modes-shown releaseOwnership freeDevice"));
    CHECK(mem_freed == mem_used && surf_destroyed == surf_used && strstr(logs, "no mode the system asked for could be shown"));

    /* Flips never seen to complete at the start: screen_handed_back_at_start. */

    /* The system refuses the surface: nothing to give back, the console restored. */
    reset();
    give_result = -N_ERR_DENIED;
    CHECK(!nvrm_display_start());
    CHECK(never("give-boot") && never("registerVblank-1") && never("thread") && never("apply-commit"));
    CHECK(in_order("give-surface-direct give-surface close101 destroySurface releaseOwnership freeDevice"));
    CHECK(mem_freed == mem_used && surf_destroyed == surf_used);

    /* RM's mapping is not a BAR's: no view, nothing given. */
    reset();
    mapping_not_bar = true;
    CHECK(!nvrm_display_start());
    CHECK(never("view-desk") && never("apply-commit") && in_order("mapMemory unmapMemory freeMemory freeDevice"));
    CHECK(mem_freed == mem_used);

    /* The boot screen is not on this GPU: NVKMS is not even asked. */
    reset();
    nvrm_dev.info.framebuffer_bar = N_PCI_NONE;
    CHECK(!nvrm_display_start() && never("allocateDevice"));
    /* A 16-bit boot screen is not copied: the first surface starts black. */
    reset();
    nvrm_dev.info.framebuffer_bpp = 16;
    CHECK(nvrm_display_start() && never("view-gop") && PIXEL(1, 7680u * 500 + 700 * 4) == 0);
    nvrm_display_stop();

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

    /* The firmware lit 0x4 only: its own preferred mode, 3840x2160, not the boot screen's size. */
    reset();
    rm_boot_mask = 0x4;
    CHECK(nvrm_display_start());
    CHECK(head_cfg(0)->modeSetConfig.displays[0] == 0x4 && head_cfg(0)->modeSetConfig.mode.timings.hVisible == 3840);
    CHECK(given_desc.width == 3840 && given_desc.height == 2160 && given_desc.pitch == 15360 && never("view-gop"));
    CHECK(!strcmp(modes_seen.monitor, "DEL1204-00c0ffee") && modes_seen.count == 1);
    nvrm_display_stop();
}

static void monitors_come_and_go(void) {
    reset();
    CHECK(nvrm_display_start());

    /* An event wakes the display thread; the offered display still being there, with the same
       modes, changes nothing. */
    struct NvKmsKapiEvent event = { .type = NVKMS_EVENT_TYPE_DPY_CHANGED };
    kapi_event(&event);
    CHECK(scr.rescan);
    calls[0] = 0;
    rescan();
    CHECK(never("apply-commit") && never("unregisterVblank-1") && never("modes-offered"));

    /* 0x2 is unplugged: its head off; 0x4 is there: its modes offered, and the system's wish (its
       preferred 4K mode) shown with a surface of that size on head 0; the one before freed. */
    displays[1].connected = false;
    calls[0] = 0;
    rescan();
    CHECK(in_order("unregisterVblank-1 apply-check apply-commit modes-offered allocateMemory mapMemory view-desk "
                   "createSurface apply-check give-surface-direct flipArm-0 apply-commit flipPending-0 close101 "
                   "destroySurface unmapMemory freeMemory registerVblank-0 modes-shown"));
    CHECK(never("give-boot") && last_request.headsMask == 0x1 && !scr.flipping);
    const struct NvKmsKapiHeadRequestedConfig *h = head_cfg(0);
    CHECK(h->modeSetConfig.displays[0] == 0x4 && h->modeSetConfig.mode.timings.hVisible == 3840);
    CHECK(h->layerRequestedConfig[0].config.srcWidth == 3840 && h->layerRequestedConfig[0].config.dstWidth == 3840 &&
          h->layerRequestedConfig[0].config.dstHeight == 2160);
    CHECK(given_surface == DESK_VIEW(2) && given_desc.width == 3840 && given_desc.pitch == 15360);
    CHECK(scr.display == 0x4 && scr.head == 0 && scr.head_on && !strcmp(modes_seen.monitor, "DEL1204-00c0ffee"));

    /* 0x4 goes too: dark, the modes withdrawn, the surface still the system's. */
    displays[2].connected = false;
    calls[0] = 0;
    rescan();
    CHECK(in_order("unregisterVblank-0 apply-check apply-commit modes-withdrawn") && never("registerVblank-0"));
    CHECK(never("give-boot") && never("destroySurface") && never("modes-shown"));
    CHECK(last_request.headsMask == 0x1 && head_cfg(0)->modeSetConfig.numDisplays == 0);
    /* KAPI sends the output LUT's scale for a head turned off too: 1.0 there as well. */
    CHECK(!head_cfg(0)->modeSetConfig.bActive &&
          head_cfg(0)->modeSetConfig.olutFpNormScale == NVKMS_OLUT_FP_NORM_SCALE_DEFAULT);
    CHECK(scr.display == 0 && !scr.head_on && !scr.target && scr.given && screen.mode.width == 0);

    /* The firmware's monitor comes back: its preferred mode again, on head 1, at its size. */
    displays[1].connected = true;
    calls[0] = 0;
    rescan();
    CHECK(in_order("modes-offered allocateMemory apply-check give-surface-direct flipArm-1 apply-commit flipPending-1 "
                   "close102 registerVblank-1 modes-shown"));
    CHECK(last_request.headsMask == 0x2 && head_cfg(1)->modeSetConfig.mode.timings.refreshRate == 120000);
    CHECK(head_cfg(1)->layerRequestedConfig[0].config.dstWidth == W && given_desc.width == W);

    /* 0x4 instead: the mode set on head 0, its flip awaited. */
    displays[1].connected = false;
    displays[2].connected = true;
    calls[0] = 0;
    rescan();
    CHECK(in_order("unregisterVblank-1 apply-commit modes-offered apply-check give-surface-direct flipArm-0 apply-commit "
                   "flipPending-0 registerVblank-0"));
    CHECK(scr.head_on && scr.display == 0x4 && !scr.flipping &&
          strstr(logs, "the desktop is shown on display 0x00000004 (head 0) at 3840x2160 at 60.000 Hz"));

    /* 0x4 goes but its head stays lit (turning it off fails), and 0x2 comes back on that same
       head: the mode set replaces the surface the layer shows, its flip awaited. */
    displays[1].head_mask = 0x1;
    displays[1].connected = true;
    displays[2].connected = false;
    commit_failures = 1;
    pending_polls = 1;
    calls[0] = 0;
    rescan();
    CHECK(in_order("unregisterVblank-0 apply-check apply-commit modes-offered apply-check give-surface-direct flipArm-0 "
                   "apply-commit flipPending-0 flipPending-0 registerVblank-0"));
    CHECK(scr.head_on && scr.head == 0 && scr.display == 0x2 && !scr.flipping && strstr(logs, "turning the head off failed"));

    /* 0x4 again while flips are never seen to complete: the mode set's flip is awaited
       FLIP_TIMEOUT_MS, then the screen is handed back: the boot screen the system's, the display
       NVKMS's console's, every surface freed. 0x4's modes stay offered; the wish is said back as
       not shown, once, and the system, with no other mode of 0x4's left, asks for none. */
    displays[1].connected = false;
    displays[2].connected = true;
    flip_pending_forever = true;
    calls[0] = 0;
    uint64_t before = clock_ns;
    rescan();
    CHECK(in_order("unregisterVblank-0 apply-commit modes-offered apply-check give-surface-direct flipArm-0 apply-commit "
                   "flipPending-0 flipWords-0 give-boot releaseOwnership modes-shown"));
    CHECK(never("registerVblank-0") && never("modes-withdrawn") && times("modes-shown") == 1 && never("grabOwnership") &&
          clock_ns - before >= (uint64_t)FLIP_TIMEOUT_MS * 1000000u);
    CHECK(!scr.owner && !scr.head_on && !scr.display && !scr.flipping && !scr.direct && !scr.desk.surface &&
          given_surface == N_SCANOUT_BOOT && mem_freed == mem_used && surf_destroyed == surf_used);
    CHECK(modes_seen.count == 1 && offered.count == 1 && scr.target == 0x4 && refused_count == 1 &&
          refused_modes[0].width == 3840 && screen.mode.width == 0);
    CHECK(strstr(logs, "the display engine has not performed the flip on head 0 3000 ms after its commit"));
    flip_pending_forever = false;
    /* Monitors coming and going are still followed: 0x4 goes, 0x2 comes back on head 0, and its
       modes offered anew bring the system's wish, which takes the display again. */
    displays[1].connected = true;
    displays[2].connected = false;
    calls[0] = 0;
    rescan();
    CHECK(in_order("modes-offered grabOwnership allocateMemory mapMemory createSurface allocateMemory mapMemory "
                   "view-desk createSurface apply-check give-surface-direct flipArm-0 apply-commit flipPending-0 "
                   "registerVblank-0 modes-shown"));
    CHECK(scr.owner && scr.head_on && scr.head == 0 && scr.display == 0x2 && scr.direct && given_desc.width == W &&
          modes_seen.shown.refresh_mhz == 120000 && modes_seen.done == screen.mode_serial);

    /* Only stop withdraws the modes. */
    calls[0] = 0;
    nvrm_display_stop();
    CHECK(in_order("join join modes-withdrawn give-boot unregisterVblank-0 releaseOwnership declareEventInterest-0 "
                   "freeDevice"));
    CHECK(mem_freed == mem_used && surf_destroyed == surf_used && modes_seen.count == 0 && !offered.count);
}

/* ---------------------------------------------------------------- direct scanout */
#define APP 77u
#define OTHER_APP 78u
/* Memory 0 is the cursor, 1 the desktop's surface, 2... apps' images. */

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

    /* CREATE: video memory for scanout of the desktop's size and pitch, X8B8G8R8, black;
       nothing on the screen changes. */
    struct nvrm_scanout_request r = { .op = NVRM_SCANOUT_CREATE };
    calls[0] = 0;
    CHECK(scanout(APP_FILE, APP, &r) == NV_OK);
    CHECK(r.image != 0 && r.width == W && r.height == H && r.pitch == 7680 && r.bytes == 7680ull * H);
    /* RM's mapping stays, and the system holds a view of it for its panic screen (X8B8G8R8). */
    CHECK(in_order("allocateMemory createSurface mapMemory view-image panic-image close50") &&
          never("unmapMemory") && never("apply-commit"));
    CHECK(panic_images() == 1 && panic_desc.width == W && panic_desc.height == H && panic_desc.pitch == 7680 &&
          panic_desc.format == N_SCANOUT_XBGR8888 && panic_desc.offset == 0);
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
    /* Nothing awaited (the mode set's flip completed in start); this flip's notifier cleared
       first. */
    CHECK(in_order("flipArm-1 apply-commit") && never("flipPending-1") && never("apply-check"));
    const struct NvKmsKapiHeadRequestedConfig *h = head_cfg(1);
    CHECK(last_request.headsMask == 0x2 && h->modeSetConfig.bActive && h->modeSetConfig.numDisplays == 1 &&
          h->modeSetConfig.displays[0] == 0x2 && h->modeSetConfig.mode.timings.refreshRate == 120000);
    CHECK(!h->flags.activeChanged && !h->flags.displaysChanged && !h->flags.modeChanged);
    const struct NvKmsKapiLayerRequestedConfig *p = &h->layerRequestedConfig[0];
    CHECK(p->flags.surfaceChanged && p->config.surface == SURF(2) && p->config.srcWidth == W &&
          p->config.srcHeight == H && p->config.dstWidth == W && p->config.dstHeight == H);
    CHECK(!h->layerRequestedConfig[1].flags.surfaceChanged);
    const struct NvKmsKapiCursorRequestedConfig *c = &h->cursorRequestedConfig;
    CHECK(c->flags.surfaceChanged && c->surface == SURF(0) &&
          c->compParams.compMode == NVKMS_COMPOSITION_BLENDING_MODE_NON_PREMULT_ALPHA && c->compParams.surfaceAlpha == 0xff);
    CHECK(c->flags.dstXYChanged && c->dstX == 92 && c->dstY == 42);
    /* The pointer's pixels, row by row into the cursor's pitch. */
    CHECK(PIXEL(0, 0) == 0x80000000u && PIXEL(0, 256 * 31 + 4 * 31) == 0x80000000u + 32 * 32 - 1 &&
          PIXEL(0, 256 * 1) == 0x80000000u + 32);
    CHECK(scr.shown && scr.shown->number == a && !scr.leaving && strstr(logs, "app 77's images are shown directly"));

    /* The next present waits until that flip has completed: one flip waiting at a time. The
       image it took off may be written again once the next PRESENT has returned. */
    pending_polls = 2;
    calls[0] = 0;
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_PRESENT, b) == NV_OK);
    CHECK(in_order("flipPending-1 flipPending-1 flipPending-1 flipArm-1 apply-commit"));
    CHECK(head_cfg(1)->layerRequestedConfig[0].config.surface == SURF(3) &&
          !head_cfg(1)->cursorRequestedConfig.flags.surfaceChanged && !head_cfg(1)->cursorRequestedConfig.flags.dstXYChanged);
    CHECK(scr.shown->number == b && scr.leaving && scr.leaving->number == a);

    /* The pointer moves: the cursor alone; its image changes: the pixels, no flip. */
    screen_says(APP, true, 400, 300);
    calls[0] = 0;
    follow_screen();
    CHECK(in_order("apply-commit") && never("apply-check") && never("flipArm-1"));
    c = &head_cfg(1)->cursorRequestedConfig;
    CHECK(c->flags.dstXYChanged && !c->flags.surfaceChanged && c->dstX == 392 && c->dstY == 292);
    CHECK(!head_cfg(1)->layerRequestedConfig[0].flags.surfaceChanged && !scr.leaving);
    screen_pointer_image(0x40000000u);
    calls[0] = 0;
    follow_screen();
    CHECK(never("apply-commit") && PIXEL(0, 0) == 0x40000000u);
    /* The pointer hidden: the cursor off. */
    screen_says(APP, false, 400, 300);
    calls[0] = 0;
    follow_screen();
    CHECK(in_order("apply-commit"));
    c = &head_cfg(1)->cursorRequestedConfig;
    CHECK(c->flags.surfaceChanged && c->surface == NULL && !scr.cursor_on);
    screen_says(APP, true, 400, 300);
    follow_screen();
    CHECK(scr.cursor_on && head_cfg(1)->cursorRequestedConfig.surface == SURF(0));

    /* The system no longer shows the app directly: the desktop again, without the cursor. */
    screen_says(0, true, 400, 300);
    calls[0] = 0;
    follow_screen();
    CHECK(in_order("apply-commit"));
    h = head_cfg(1);
    CHECK(h->layerRequestedConfig[0].flags.surfaceChanged && h->layerRequestedConfig[0].config.surface == SURF(1));
    CHECK(h->cursorRequestedConfig.flags.surfaceChanged && h->cursorRequestedConfig.surface == NULL);
    CHECK(!scr.shown && scr.leaving && scr.leaving->number == b && strstr(logs, "the desktop is shown again"));
    calls[0] = 0;
    follow_screen();
    CHECK(never("apply-commit"));

    /* DESTROY of the image the last flip took off: its completion awaited, no flip. */
    pending_polls = 1;
    calls[0] = 0;
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_DESTROY, b) == NV_OK);
    CHECK(in_order("flipPending-1 flipPending-1 panic-image-end unmapMemory destroySurface freeMemory") &&
          never("apply-commit"));
    CHECK(!scr.leaving && image_count() == 1 && panic_images() == 1 &&
          scanout_op(APP_FILE, APP, NVRM_SCANOUT_PRESENT, b) == NV_ERR_INVALID_OBJECT_HANDLE);

    /* DESTROY of the image shown: the desktop first, the flip completed, then freed. */
    screen_says(APP, true, 100, 50);
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_PRESENT, a) == NV_OK);
    calls[0] = 0;
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_DESTROY, a) == NV_OK);
    CHECK(in_order("flipPending-1 flipArm-1 apply-commit flipPending-1 panic-image-end unmapMemory destroySurface "
                   "freeMemory"));
    CHECK(head_cfg(1)->layerRequestedConfig[0].config.surface == SURF(1) && !scr.shown && !scr.leaving &&
          !scr.images && panic_images() == 0);
    /* Flips of apps' images are not logged frame by frame: only the mode set's, whose notifier
       status ("begun") the later ones repeat. */
    CHECK(logged("was performed") == 1);

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
    CHECK(head_cfg(1)->layerRequestedConfig[0].config.surface == SURF(1));
    (void)i4;

    /* A commit NVKMS refuses flips nothing: no completion is awaited after it. */
    uint32_t i7 = create_image(APP_FILE, APP); /* memory 7 */
    commit_failures = 1;
    calls[0] = 0;
    uint64_t t0 = clock_ns;
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_PRESENT, i7) == NV_ERR_INVALID_STATE && !scr.shown);
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_PRESENT, i7) == NV_OK && scr.shown && scr.shown->number == i7);
    CHECK(in_order("flipArm-1 apply-commit flipArm-1 apply-commit") && never("flipPending-1") && clock_ns == t0);

    /* DESTROY of the image shown when the flip back to the desktop fails: NVKMS takes the image
       off the head as its surface goes, and the desktop is flipped back right after. */
    commit_failures = 1;
    calls[0] = 0;
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_DESTROY, i7) == NV_OK);
    /* That flip puts a surface on the layer again, so KAPI names a notifier for it too. */
    CHECK(in_order("flipArm-1 apply-commit destroySurface freeMemory flipArm-1 apply-commit") &&
          strstr(calls, " freeMemory flipArm-1 apply-commit "));
    CHECK(head_cfg(1)->layerRequestedConfig[0].config.surface == SURF(1) && !scr.lost && !scr.shown &&
          strstr(logs, "is still shown"));
    /* When that fails too, the next change of the screen's state shows the desktop. */
    uint32_t i8 = create_image(APP_FILE, APP); /* memory 8 */
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_PRESENT, i8) == NV_OK);
    commit_failures = 2;
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_DESTROY, i8) == NV_OK);
    CHECK(scr.lost && !scr.shown && strstr(logs, "the desktop is still not shown"));
    screen_says(0, true, 100, 50);
    calls[0] = 0;
    follow_screen();
    CHECK(in_order("flipArm-1 apply-commit") && !scr.lost);
    CHECK(head_cfg(1)->layerRequestedConfig[0].flags.surfaceChanged &&
          head_cfg(1)->layerRequestedConfig[0].config.surface == SURF(1));
    calls[0] = 0;
    follow_screen();
    CHECK(never("apply-commit"));
    screen_says(APP, true, 100, 50);

    /* An image the system cannot draw its panic screen into is not made: the app's window stays
       composited rather than shown where a stopped kernel could not be seen. */
    unsigned images = image_count();
    panic_refused = -N_ERR_DENIED;
    calls[0] = 0;
    r = (struct nvrm_scanout_request){ .op = NVRM_SCANOUT_CREATE };
    CHECK(scanout(APP_FILE, APP, &r) == NV_ERR_INSUFFICIENT_RESOURCES && !r.image); /* memory 9 */
    CHECK(in_order("mapMemory view-image panic-image close50 unmapMemory destroySurface freeMemory"));
    CHECK(image_count() == images && strstr(logs, "cannot draw its panic screen"));
    panic_refused = 0;
    mapping_not_bar = true; /* RM's mapping is not one stretch of a BAR */
    calls[0] = 0;
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_CREATE, 0) == NV_ERR_INSUFFICIENT_RESOURCES); /* memory 10 */
    CHECK(never("view-image") && never("panic-image") && in_order("mapMemory unmapMemory destroySurface freeMemory"));
    CHECK(image_count() == images);
    mapping_not_bar = false;

    /* stop: the threads first, the remaining images, the cursor and the desktop's surface after
       ownership is released. Giving the boot screen back let go of the images' views already, so
       the system is not asked to again. */
    calls[0] = 0;
    nvrm_display_stop();
    CHECK(in_order("join join modes-withdrawn give-boot unregisterVblank-1 releaseOwnership destroySurface unmapMemory "
                   "freeMemory close101 destroySurface unmapMemory freeMemory freeDevice"));
    CHECK(in_order("give-boot unmapMemory destroySurface freeMemory") && never("panic-image-end") && panic_images() == 0);
    CHECK(mem_freed == mem_used && surf_destroyed == surf_used && !scr.images);
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_CREATE, 0) == NV_ERR_NOT_SUPPORTED);
}

/* Displays coming and going under an app's image: the mode set shows the desktop, at the new
   display's size; an image made before is shown scaled to the mode, and the cursor follows the
   pointer in the desktop's pixels. */
static void direct_scanout_and_displays(void) {
    reset();
    CHECK(nvrm_display_start());
    uint32_t a = create_image(APP_FILE, APP); /* memory and surface 2 */
    screen_pointer_image(1);
    screen_says(APP, true, 100, 50);
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_PRESENT, a) == NV_OK && scr.shown);

    displays[1].connected = false; /* 0x2 unplugged; 0x4, 3840x2160, takes the desktop */
    rescan();
    CHECK(!scr.shown && !scr.leaving && !scr.cursor_on && scr.display == 0x4 && scr.desk.width == 3840);
    CHECK(head_cfg(0)->layerRequestedConfig[0].config.surface == SURF(3));
    calls[0] = 0;
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_PRESENT, a) == NV_OK);
    const struct NvKmsKapiHeadRequestedConfig *h = head_cfg(0);
    CHECK(last_request.headsMask == 0x1 && h->modeSetConfig.displays[0] == 0x4 &&
          h->modeSetConfig.mode.timings.hVisible == 3840 && !h->flags.modeChanged);
    CHECK(h->layerRequestedConfig[0].config.surface == SURF(2) && h->layerRequestedConfig[0].config.srcWidth == W &&
          h->layerRequestedConfig[0].config.dstWidth == 3840 && h->layerRequestedConfig[0].config.dstHeight == 2160);
    CHECK(h->cursorRequestedConfig.dstX == 92 && h->cursorRequestedConfig.dstY == 42);
    /* An image made now has the desktop's new size. */
    struct nvrm_scanout_request r = { .op = NVRM_SCANOUT_CREATE };
    CHECK(scanout(APP_FILE, APP, &r) == NV_OK && r.width == 3840 && r.height == 2160 && r.pitch == 15360);

    /* No display: nothing to present on. */
    displays[2].connected = false;
    rescan();
    calls[0] = 0;
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_PRESENT, a) == NV_ERR_INVALID_STATE && never("apply-commit"));
    nvrm_display_stop();
    CHECK(mem_freed == mem_used && surf_destroyed == surf_used);
}

/* A flip whose completion is never seen: the next flip waits FLIP_TIMEOUT_MS for it, then the
   screen is handed back: the boot screen the system's again, the display NVKMS's console's, apps'
   images not shown any more, the modes still offered. Images made before can still be freed. The
   system's next wish takes the display again, and apps' images are shown again.
   (0a1d812a stopped awaiting flips instead and kept the display, which stayed dark.) */
static void flips_not_seen(void) {
    reset();
    CHECK(nvrm_display_start());
    uint32_t a = create_image(APP_FILE, APP), b = create_image(APP_FILE, APP);
    screen_pointer_image(1);
    screen_says(APP, true, 100, 50);
    flip_pending_forever = true;
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_PRESENT, a) == NV_OK && scr.flipping);
    uint64_t before = clock_ns;
    calls[0] = 0;
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_PRESENT, b) == NV_ERR_INVALID_STATE);
    CHECK(clock_ns - before >= (uint64_t)FLIP_TIMEOUT_MS * 1000000u);
    CHECK(in_order("flipPending-1 flipWords-1 give-boot unregisterVblank-1 releaseOwnership "
                   "destroySurface unmapMemory freeMemory close101 destroySurface unmapMemory freeMemory"));
    CHECK(never("flipArm-1") && never("apply-commit") && never("modes-withdrawn"));
    CHECK(!scr.owner && !scr.head_on && !scr.flipping && !scr.shown && !scr.leaving && !scr.direct && !scr.desk.surface);
    CHECK(given_surface == N_SCANOUT_BOOT && panic_images() == 0 && modes_seen.count == 4 && offered.count == 4);
    CHECK(strstr(logs, "the display engine has not performed the flip on head 1 3000 ms after its commit") &&
          strstr(logs, "the primary layer's completion notifier 1, of words 00000000 00000000 00000001 00000000 and "
                       "00000000 00000000 00000001 00000000, does not read begun"));
    /* What the head showed, read back before it was handed back. */
    CHECK(in_order("flipWords-1 getCRC32-1 give-boot") &&
          strstr(logs, "head 1 as the screen is handed back: display 0x00000002 at 1920x1080 at 120.000 Hz"));
    CHECK(strstr(logs, "nvrm's surface is not seen on the display; the boot screen goes back to the system and the "
                       "display to NVKMS's console"));
    /* An app's flip is no wish of the system's: nothing is said back. */
    CHECK(never("modes-shown") && refused_count == 0);
    flip_pending_forever = false;

    /* Apps' requests are refused then, but DESTROY: nothing reaches NVKMS but the frees, and the
       system, which let go of the images as the boot screen came back, is not asked again. */
    calls[0] = 0;
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_PRESENT, b) == NV_ERR_NOT_SUPPORTED);
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_CREATE, 0) == NV_ERR_NOT_SUPPORTED);
    struct nvrm_scanout_request imp = { .op = NVRM_SCANOUT_IMPORT, .image = a, .client = 0xc100, .parent = 0xc101 };
    CHECK(scanout(APP_FILE, APP, &imp) == NV_ERR_NOT_SUPPORTED && never("rm-share"));
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_DESTROY, a) == NV_OK && image_count() == 1);
    CHECK(in_order("unmapMemory destroySurface freeMemory") && never("panic-image-end") && never("apply-commit") &&
          never("flipPending-1"));
    /* Monitors coming and going with the same modes ask for nothing. */
    calls[0] = 0;
    rescan();
    CHECK(!calls[0]);

    /* The system's next wish (the user's mode in Settings) takes the display again: ownership, the
       cursor, a surface of the mode's size given with N_SCANOUT_DIRECT, the mode set, its flip;
       the image left held again for the panic screen, then the mode said back as shown. */
    system_wants(2560, 1440, 144000);
    screen_says(0, true, 100, 50);
    calls[0] = 0;
    follow_screen();
    CHECK(in_order("grabOwnership allocateMemory mapMemory createSurface allocateMemory mapMemory view-desk createSurface "
                   "apply-check give-surface-direct flipArm-1 apply-commit flipPending-1 registerVblank-1 getCRC32-1 "
                   "view-image panic-image close50 modes-shown"));
    CHECK(scr.owner && scr.head_on && scr.direct && scr.desk.width == 2560 && given_desc.width == 2560 &&
          given_desc.format == (N_SCANOUT_XRGB8888 | N_SCANOUT_DIRECT) && panic_images() == 1);
    CHECK(modes_seen.done == screen.mode_serial && modes_seen.shown.width == 2560 && refused_count == 0);
    CHECK(strstr(logs, "display ownership taken again for the system's wish"));
    /* Apps' images are shown again, and made again at the desktop's new size. */
    screen_says(APP, true, 100, 50);
    calls[0] = 0;
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_PRESENT, b) == NV_OK && scr.shown && scr.shown->number == b);
    CHECK(in_order("flipArm-1 apply-commit"));
    struct nvrm_scanout_request r = { .op = NVRM_SCANOUT_CREATE };
    CHECK(scanout(APP_FILE, APP, &r) == NV_OK && r.width == 2560 && panic_images() == 2);

    /* Only stop withdraws the modes. */
    calls[0] = 0;
    nvrm_display_stop();
    CHECK(in_order("join join modes-withdrawn give-boot unregisterVblank-1 releaseOwnership declareEventInterest-0 "
                   "freeDevice"));
    CHECK(mem_freed == mem_used && surf_destroyed == surf_used && !scr.images && modes_seen.count == 0);
}

/* Flips never seen to complete from the start: the first mode set's flip is awaited
   FLIP_TIMEOUT_MS, then the screen is handed back (the boot screen to the system, the display to
   NVKMS's console), the notifier's words logged; the wish is said back as not shown, and the
   system's next best is tried once (the display taken again). Its flip not seen either, the
   screen stays handed back and the system is not told, or it would ask for each of its modes in
   turn, each dark for FLIP_TIMEOUT_MS. nvrm runs on with the modes offered. 0a1d812a expected
   the surface to be given at once instead, as if a mode set that lights the head named no
   notifier; NVKMS's source names one, and on real hardware the screen then stayed dark with
   nothing handed back. */
static void screen_handed_back_at_start(void) {
    reset();
    flip_pending_forever = true;
    uint64_t before = clock_ns;
    CHECK(nvrm_display_start());
    CHECK(clock_ns - before >= 2ull * FLIP_TIMEOUT_MS * 1000000u);
    CHECK(in_order("give-surface-direct flipArm-1 apply-commit flipPending-1 flipWords-1 give-boot releaseOwnership "
                   "destroySurface unmapMemory freeMemory close101 destroySurface unmapMemory freeMemory modes-shown "
                   "grabOwnership give-surface-direct flipArm-1 apply-commit flipWords-1 give-boot releaseOwnership "
                   "thread thread"));
    CHECK(never("registerVblank-1") && never("modes-withdrawn") && times("modes-shown") == 1 &&
          times("grabOwnership") == 2 && times("flipWords-1") == 2);
    CHECK(refused_count == 1 && refused_modes[0].refresh_mhz == 120000 && screen.mode.width == W &&
          screen.mode.refresh_mhz == 60000 && modes_seen.count == 4 && offered.count == 4);
    CHECK(strstr(logs, "the display engine has not performed the flip on head 1 3000 ms after its commit") &&
          strstr(logs, "nvrm's surface is not seen on the display; the boot screen goes back to the system") &&
          logged("the screen stays handed back: the system is not told that 1920x1080 at 60.000 Hz was not shown") == 1 &&
          strstr(logs, "the screen stays with the firmware's framebuffer for now; display 0x00000002's modes stay offered"));
    CHECK(given_surface == N_SCANOUT_BOOT && !scr.owner && !scr.direct && scr.state_running && scr.thread_running &&
          mem_freed == mem_used && surf_destroyed == surf_used);
    /* Nothing is tried again by nvrm itself: not on a state that changes nothing, nor on monitor
       events with the same modes. */
    screen.serial++;
    calls[0] = 0;
    follow_screen();
    rescan();
    CHECK(!calls[0]);
    /* The user's next mode, with flips seen again: shown, said back. */
    flip_pending_forever = false;
    system_wants(2560, 1440, 144000);
    calls[0] = 0;
    follow_screen();
    CHECK(in_order("grabOwnership apply-check give-surface-direct flipArm-1 apply-commit flipPending-1 registerVblank-1 "
                   "modes-shown"));
    CHECK(scr.owner && scr.head_on && modes_seen.shown.width == 2560 && modes_seen.done == screen.mode_serial);
    calls[0] = 0;
    nvrm_display_stop();
    CHECK(in_order("join join modes-withdrawn give-boot unregisterVblank-1 releaseOwnership freeDevice") &&
          mem_freed == mem_used && surf_destroyed == surf_used);

    /* Handed back with no state thread: nothing would take the display again, so start fails. */
    reset();
    flip_pending_forever = true;
    thread_fails = true;
    CHECK(!nvrm_display_start());
    CHECK(in_order("give-boot releaseOwnership modes-shown grabOwnership give-boot releaseOwnership thread-fails "
                   "modes-withdrawn declareEventInterest-0 freeDevice"));
    CHECK(strstr(logs, "without the state thread no wish of the system's would take it again") &&
          mem_freed == mem_used && surf_destroyed == surf_used);
}

/* After the screen is handed back: a wish whose flip is not seen either is said back once, the
   system's next best is tried once, and then nothing more until a mode set's flip is performed;
   the same wish is never tried again by nvrm itself; another NVKMS client that owns the display
   keeps it. */
static void screen_taken_again(void) {
    reset();
    CHECK(nvrm_display_start());
    mtx_lock(&scr.head_lock);
    hand_back(NULL); /* as for a flip not performed, with nothing said back */
    mtx_unlock(&scr.head_lock);
    CHECK(!scr.owner && modes_seen.count == 4);

    /* The user tries 2560x1440 while the display engine performs no flip: taken again, handed
       back after FLIP_TIMEOUT_MS, said back once; the system refuses it and asks for its next
       best (the preferred mode), which is taken again, handed back too, and not said back. */
    flip_pending_forever = true;
    system_wants(2560, 1440, 144000);
    calls[0] = 0;
    uint64_t before = clock_ns;
    follow_screen();
    CHECK(clock_ns - before >= 2ull * FLIP_TIMEOUT_MS * 1000000u && clock_ns - before < 3ull * FLIP_TIMEOUT_MS * 1000000u);
    CHECK(in_order("grabOwnership apply-commit flipWords-1 give-boot releaseOwnership modes-shown grabOwnership "
                   "apply-commit flipWords-1 give-boot releaseOwnership"));
    CHECK(times("grabOwnership") == 2 && times("modes-shown") == 1 && times("flipWords-1") == 2 && never("modes-withdrawn"));
    CHECK(refused_count == 1 && refused_modes[0].width == 2560 && screen.mode.width == W &&
          screen.mode.refresh_mhz == 120000 && !scr.owner && !scr.desk.surface && modes_seen.count == 4);
    CHECK(logged("display ownership taken again for the system's wish") == 2 &&
          logged("the screen stays handed back: the system is not told that 1920x1080 at 120.000 Hz was not shown") == 1);
    /* Not tried again by nvrm itself. */
    screen.serial++;
    calls[0] = 0;
    follow_screen();
    rescan();
    CHECK(!calls[0]);

    /* The user's next mode, with flips seen: shown, said back. */
    flip_pending_forever = false;
    system_wants(W, H, 60000);
    calls[0] = 0;
    follow_screen();
    CHECK(in_order("grabOwnership apply-commit flipPending-1 registerVblank-1 modes-shown") && times("modes-shown") == 1);
    CHECK(scr.owner && scr.head_on && modes_seen.shown.refresh_mhz == 60000 && !scr.unshown_said);

    /* A performed flip since: a mode the display engine does not flip to is said back once again,
       and the system's next best (the preferred mode, not refused before) tried once. */
    flip_pending_forever = true;
    system_wants(1280, 720, 144000);
    calls[0] = 0;
    follow_screen();
    CHECK(in_order("apply-commit flipWords-1 give-boot releaseOwnership modes-shown grabOwnership apply-commit "
                   "flipWords-1 give-boot releaseOwnership"));
    CHECK(times("modes-shown") == 1 && times("grabOwnership") == 1 && refused_count == 2 &&
          refused_modes[1].width == 1280 && screen.mode.refresh_mhz == 120000 && !scr.owner);
    flip_pending_forever = false;

    /* Another NVKMS client owns the display: the wish is not followed, nothing is said back, and
       it is not tried again by itself. */
    grab_fails = true;
    system_wants(W, H, 60000);
    calls[0] = 0;
    follow_screen();
    CHECK(times("grabOwnership") == 1 && never("apply-check") && never("modes-shown") && !scr.owner &&
          strstr(logs, "another NVKMS client owns the display; the system's wish is not followed"));
    grab_fails = false;
    screen.serial++;
    calls[0] = 0;
    follow_screen();
    CHECK(!calls[0]);

    calls[0] = 0;
    nvrm_display_stop();
    CHECK(in_order("join join modes-withdrawn declareEventInterest-0 freeDevice") && never("give-boot") &&
          never("releaseOwnership"));
    CHECK(mem_freed == mem_used && surf_destroyed == surf_used && modes_seen.count == 0);
}

static void no_direct_scanout(void) {
    /* No cursor the size of the system's pointer: the surface given without N_SCANOUT_DIRECT
       (memory 0 is the desktop's surface then). The state thread still follows the system's
       mode. */
    reset();
    cursor_caps_missing = true;
    CHECK(nvrm_display_start());
    CHECK(given_desc.format == N_SCANOUT_XRGB8888 && given_surface == DESK_VIEW(0) && never("give-surface-direct") &&
          mem_used == 1);
    CHECK(!scr.direct && scr.state_running && strstr(logs, "cannot show apps' X8B8G8R8 images"));
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_CREATE, 0) == NV_ERR_NOT_SUPPORTED && mem_used == 1);
    nvrm_display_connection_end(APP_FILE);
    system_wants(2560, 1440, 144000);
    calls[0] = 0;
    follow_screen();
    CHECK(in_order("allocateMemory apply-check give-surface flipArm-1 apply-commit close100 modes-shown") &&
          never("give-surface-direct"));
    CHECK(given_desc.width == 2560 && given_desc.format == N_SCANOUT_XRGB8888);
    calls[0] = 0;
    nvrm_display_stop();
    CHECK(in_order("join join modes-withdrawn give-boot") && mem_freed == mem_used);

    /* The system refuses N_SCANOUT_DIRECT: given again without it, the cursor freed. */
    reset();
    direct_refused = true;
    CHECK(nvrm_display_start());
    CHECK(in_order("give-surface-direct give-surface destroySurface unmapMemory freeMemory apply-commit registerVblank-1 "
                   "modes-shown thread thread"));
    CHECK(given_desc.format == N_SCANOUT_XRGB8888 && !scr.direct && scr.state_running && mem_freed == 1);
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_CREATE, 0) == NV_ERR_NOT_SUPPORTED);
    nvrm_display_stop();
    CHECK(mem_freed == mem_used && surf_destroyed == surf_used);

    /* No state thread: the screen keeps its mode, and windows are not shown directly. */
    reset();
    thread_fails = true;
    CHECK(nvrm_display_start());
    CHECK(in_order("give-surface-direct apply-commit modes-shown thread-fails give-surface thread-fails"));
    CHECK(!scr.direct && !scr.state_running && given_desc.format == N_SCANOUT_XRGB8888 && !scr.cursor_surface);
    CHECK(scanout_op(APP_FILE, APP, NVRM_SCANOUT_CREATE, 0) == NV_ERR_NOT_SUPPORTED);
    nvrm_display_stop();
    CHECK(mem_freed == mem_used && surf_destroyed == surf_used);
    /* ...and the system keeps the surface with N_SCANOUT_DIRECT: it would hand fullscreen
       windows to nvrm, which shows none. The boot screen back, nothing kept. */
    reset();
    thread_fails = plain_refused = true;
    CHECK(!nvrm_display_start());
    CHECK(in_order("give-surface-direct thread-fails give-surface modes-withdrawn give-boot releaseOwnership freeDevice"));
    CHECK(!scr.direct && !scr.given && mem_freed == mem_used && surf_destroyed == surf_used);

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
    modes_from_the_system();
    users_mode_at_start();
    nothing_given_on_failure();
    boot_displays_unknown();
    monitors_come_and_go();
    direct_scanout();
    direct_scanout_and_displays();
    flips_not_seen();
    screen_handed_back_at_start();
    screen_taken_again();
    no_direct_scanout();
    reset();
    if (failures) {
        printf("log:\n%s", logs);
        printf("nvrm_display_hosttest: %d failures\n", failures);
        return 1;
    }
    printf("nvrm_display_hosttest: ok\n");
    return 0;
}
