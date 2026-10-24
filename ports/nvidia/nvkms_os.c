/* NVKMS OS layer for Nocturne: what NVIDIA's mode-setting core (src/nvidia-modeset, linked
 * into nvrm as nv-modeset-kernel.o) expects from its operating system, and its connection to
 * RM in the same program.
 *
 * On Linux these are two pieces of glue in two kernel modules: nvidia-modeset-linux.c (the
 * nvkms_* functions of nvidia-modeset-os-interface.h, the global NVKMS lock, timers, the work
 * thread and the client entry points) in nvidia-modeset.ko, and nv-modeset-interface.c (the
 * nvidia_modeset_rm_ops_t table: RM stack, GPU list, open/close of a GPU, rm_kernel_rmapi_op,
 * suspend/resume/remove callbacks) in nvidia.ko. Both are here, written against Nocturne's
 * libc (<threads.h>, malloc) and nvrm's RM OS layer (os.c: log, clock, delays, semaphores,
 * reader/writer locks, the client-memory copies), which they share with RM instead of carrying
 * their own.
 *
 * Locking follows NVIDIA's rule: core NVKMS runs only under nvkms_lock (a semaphore, as
 * on Linux: it is not tied to the thread that took it), and the power-management lock is
 * taken for reading around every entry and for writing during suspend. Timers and KAPI event
 * notifications run on one NVKMS work thread, in order, under nvkms_lock.
 *
 * What has no counterpart on Nocturne returns "not there", as NVKMS's own Linux layer does on
 * a kernel without the feature (README "NVKMS"): procfs files, backlight class devices, Tegra
 * syncpoints, Android extcon, and NVKMS's character device for user-space clients (nvrm has
 * no NVKMS client channel yet; its clients use KAPI).
 */
#include "nvrm_os.h"
#include "nvkms_os.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>

#include <os-interface.h>           /* RM's OS layer (os.c): semaphores, rwlocks, delays, copies */
#include <nvUnixVersion.h>          /* NV_VERSION_STRING */
#include <nv-modeset-interface.h>   /* kernel-open/common/inc: the RM <-> NVKMS contract */
#include <nvidia-modeset-os-interface.h>
#include <nvkms.h>
#include <nvkms-ioctl.h>            /* nvKmsNvU64AddressIsSafe, nvKmsNvU64ToPointer */
#include <nvkms-kapi.h>

#define NVKMS_LOG_PREFIX "nvidia-modeset: "

/* ------------------------------------------------------------------------------------------
 * Settings: the module parameters of nvidia-modeset.ko, with their names and defaults, read
 * from the launch setting NVKMS_OPTIONS ("name=value;name=value"), as nvrm reads RM's from
 * NVRM_REGISTRY_DWORDS.
 * ---------------------------------------------------------------------------------------- */
static struct {
    bool output_rounding_fix;
    bool disable_hdmi_frl;
    bool disable_vrr_memclk_switch;
    bool hdmi_deepcolor;
    unsigned max_output_color_depth;
    bool opportunistic_display_sync;
    unsigned debug_force_color_space;
    bool enable_overlay_layers;
    int fail_malloc;
    bool malloc_verbose;
    int force_frl_rate;
    bool conceal_vrr_caps;
    bool enhanced_pcon_support;
    int fail_alloc_core_channel;
    int debug;
    char config_file[512];
} opt;

static void options_reset(void) {
    memset(&opt, 0, sizeof opt);
    opt.output_rounding_fix = true;
    opt.hdmi_deepcolor = true;
    opt.max_output_color_depth = 10;
    opt.opportunistic_display_sync = true;
    opt.debug_force_color_space = NVKMS_DEBUG_FORCE_COLOR_SPACE_NONE;
    opt.enable_overlay_layers = true;
    opt.fail_malloc = -1;
    opt.fail_alloc_core_channel = -1;
}

/* The spellings a Linux bool parameter accepts: 1/0, y/n, Y/N, on/off, true/false. */
static bool parse_bool(const char *v, size_t n, bool *out) {
    if ((n == 1 && (*v == '1' || *v == 'y' || *v == 'Y')) || (n == 2 && !strncmp(v, "on", 2)) ||
        (n == 4 && !strncmp(v, "true", 4))) {
        *out = true;
        return true;
    }
    if ((n == 1 && (*v == '0' || *v == 'n' || *v == 'N')) || (n == 3 && !strncmp(v, "off", 3)) ||
        (n == 5 && !strncmp(v, "false", 5))) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_int(const char *v, size_t n, long *out) {
    char text[24];
    if (!n || n >= sizeof text) return false;
    memcpy(text, v, n);
    text[n] = 0;
    char *end;
    long value = strtol(text, &end, 0);
    if (*end) return false;
    *out = value;
    return true;
}

static bool option_set(const char *name, size_t name_len, const char *v, size_t n) {
    static const struct { const char *name; bool *value; } bools[] = {
        { "output_rounding_fix", &opt.output_rounding_fix },
        { "disable_hdmi_frl", &opt.disable_hdmi_frl },
        { "disable_vrr_memclk_switch", &opt.disable_vrr_memclk_switch },
        { "hdmi_deepcolor", &opt.hdmi_deepcolor },
        { "opportunistic_display_sync", &opt.opportunistic_display_sync },
        { "enable_overlay_layers", &opt.enable_overlay_layers },
        { "malloc_verbose", &opt.malloc_verbose },
        { "conceal_vrr_caps", &opt.conceal_vrr_caps },
        { "enhanced_pcon_support", &opt.enhanced_pcon_support },
    };
    static const struct { const char *name; int *value; } ints[] = {
        { "fail_malloc", &opt.fail_malloc },
        { "force_frl_rate", &opt.force_frl_rate },
        { "fail_alloc_core_channel", &opt.fail_alloc_core_channel },
        { "debug", &opt.debug },
    };
    static const struct { const char *name; unsigned *value; } uints[] = {
        { "max_output_color_depth", &opt.max_output_color_depth },
        { "debug_force_color_space", &opt.debug_force_color_space },
    };
#define NAME_IS(s) (strlen(s) == name_len && !memcmp(name, s, name_len))
    for (size_t i = 0; i < sizeof bools / sizeof bools[0]; i++)
        if (NAME_IS(bools[i].name)) return parse_bool(v, n, bools[i].value);
    long value;
    for (size_t i = 0; i < sizeof ints / sizeof ints[0]; i++)
        if (NAME_IS(ints[i].name)) {
            if (!parse_int(v, n, &value) || value < INT32_MIN || value > INT32_MAX) return false;
            *ints[i].value = (int)value;
            return true;
        }
    for (size_t i = 0; i < sizeof uints / sizeof uints[0]; i++)
        if (NAME_IS(uints[i].name)) {
            if (!parse_int(v, n, &value) || value < 0 || value > UINT32_MAX) return false;
            *uints[i].value = (unsigned)value;
            return true;
        }
    if (NAME_IS("config_file")) {
        if (!n || n >= sizeof opt.config_file) return false;
        memcpy(opt.config_file, v, n);
        opt.config_file[n] = 0;
        return true;
    }
#undef NAME_IS
    return false;
}

static void options_read(void) {
    options_reset();
    const char *text = getenv("NVKMS_OPTIONS");
    if (!text || !*text) return;
    nvrm_log(NVKMS_LOG_PREFIX "options: %s", text);
    while (*text) {
        const char *end = strchr(text, ';');
        size_t len = end ? (size_t)(end - text) : strlen(text);
        const char *eq = memchr(text, '=', len);
        if (len) {
            if (!eq || !option_set(text, (size_t)(eq - text), eq + 1, len - (size_t)(eq - text) - 1))
                nvrm_log(NVKMS_LOG_PREFIX "WARNING: ignoring the option \"%.*s\"", (int)len, text);
        }
        text += len;
        if (*text == ';') text++;
    }
}

NvBool nvkms_test_fail_alloc_core_channel(enum NvKmsFailAllocCoreChannelMethod method) {
    if ((int)method != opt.fail_alloc_core_channel) return NV_FALSE;
    nvrm_log(NVKMS_LOG_PREFIX "Failing core channel allocation using method %d", opt.fail_alloc_core_channel);
    return NV_TRUE;
}

enum NvKmsFrlRateForce nvkms_force_frl_rate(void) {
    switch (opt.force_frl_rate) {
    case 2: return NVKMS_FRL_RATE_FORCE_MAX_DSC;
    case 1: return NVKMS_FRL_RATE_FORCE_MAX;
    }
    return NVKMS_FRL_RATE_FORCE_NONE;
}

NvBool nvkms_conceal_vrr_caps(void) { return opt.conceal_vrr_caps; }
NvBool nvkms_enhanced_pcon_support(void) { return opt.enhanced_pcon_support; }
NvBool nvkms_output_rounding_fix(void) { return opt.output_rounding_fix; }
NvBool nvkms_disable_hdmi_frl(void) { return opt.disable_hdmi_frl; }
NvBool nvkms_disable_vrr_memclk_switch(void) { return opt.disable_vrr_memclk_switch; }
NvBool nvkms_hdmi_deepcolor(void) { return opt.hdmi_deepcolor; }

NvU32 nvkms_max_output_color_bpc(void) {
    switch (opt.max_output_color_depth) {
    case 6: return 6;
    case 8: return 8;
    case 12: return 12;
    default: return 10;
    }
}

NvBool nvkms_opportunistic_display_sync(void) { return opt.opportunistic_display_sync; }

enum NvKmsDebugForceColorSpace nvkms_debug_force_color_space(void) {
    if (opt.debug_force_color_space >= NVKMS_DEBUG_FORCE_COLOR_SPACE_MAX) return NVKMS_DEBUG_FORCE_COLOR_SPACE_NONE;
    return (enum NvKmsDebugForceColorSpace)opt.debug_force_color_space;
}

NvBool nvkms_enable_overlay_layers(void) { return opt.enable_overlay_layers; }
NvBool nvkms_debug_logging(void) { return opt.debug != 0; }

/* ------------------------------------------------------------------------------------------
 * Features that belong to other platforms
 * ---------------------------------------------------------------------------------------- */

/* Tegra host1x syncpoints: only Tegra SoC display has them. */
NvBool nvkms_kernel_supports_syncpts(void) { return NV_FALSE; }
NvBool nvkms_syncpt_op(enum NvKmsSyncPtOp op, NvKmsSyncPtOpParams *params) {
    (void)op; (void)params;
    return NV_FALSE;
}

/* Android's extcon HDMI state reports (Tegra builds only). */
void nvkms_extcon_report_hdmi(NvBool state) { (void)state; }
void nvkms_extcon_report_hdmi_audio(NvBool state) { (void)state; }

/* Linux backlight class devices for laptop panels driven through NVKMS. Nocturne has no
 * backlight interface for apps yet; NVKMS treats NULL as "no backlight device", as on a Linux
 * kernel without CONFIG_BACKLIGHT_CLASS_DEVICE. */
struct nvkms_backlight_device *nvkms_register_backlight(NvU32 gpu_id, NvU32 display_id, void *drv_priv,
                                                        NvU32 current_brightness) {
    (void)gpu_id; (void)display_id; (void)drv_priv; (void)current_brightness;
    return NULL;
}
void nvkms_unregister_backlight(struct nvkms_backlight_device *nvkms_bd) { (void)nvkms_bd; }

/* NVKMS's own character device (/dev/nvidia-modeset) and its user-space clients: nvrm has no
 * channel for them, so no file descriptor is ever one of NVKMS's or of RM's. */
void *nvkms_get_per_open_data(int fd) {
    (void)fd;
    return NULL;
}
NvBool nvkms_fd_is_nvidia_chardev(int fd) {
    (void)fd;
    return NV_FALSE;
}

/* ------------------------------------------------------------------------------------------
 * Memory, strings, time
 * ---------------------------------------------------------------------------------------- */
static int alloc_count; /* atomic: nvkms_alloc calls, for fail_malloc / malloc_verbose */

void *nvkms_alloc(size_t size, NvBool zero) {
    if (opt.malloc_verbose || opt.fail_malloc >= 0) {
        int this_alloc = __atomic_fetch_add(&alloc_count, 1, __ATOMIC_RELAXED);
        if (opt.fail_malloc >= 0 && opt.fail_malloc == this_alloc) {
            nvrm_log(NVKMS_LOG_PREFIX "Failing alloc %d", opt.fail_malloc);
            return NULL;
        }
    }
    return zero ? calloc(1, size) : malloc(size);
}

void nvkms_free(void *ptr, size_t size) {
    (void)size;
    free(ptr);
}

void *nvkms_memset(void *ptr, NvU8 c, size_t size) { return memset(ptr, c, size); }
void *nvkms_memcpy(void *dest, const void *src, size_t n) { return memcpy(dest, src, n); }
void *nvkms_memmove(void *dest, const void *src, size_t n) { return memmove(dest, src, n); }
int nvkms_memcmp(const void *s1, const void *s2, size_t n) { return memcmp(s1, s2, n); }
size_t nvkms_strlen(const char *s) { return strlen(s); }
int nvkms_strcmp(const char *s1, const char *s2) { return strcmp(s1, s2); }

/* As on Linux: under a millisecond a busy wait, else a sleep of whole milliseconds clamped to
   0xFFF (about 4 s). os_delay / os_delay_us choose between spinning and sleeping the way RM's
   own waits do. */
void nvkms_usleep(NvU64 usec) {
    if (usec < 1000) {
        os_delay_us((NvU32)usec);
    } else {
        os_delay((NvU32)(((usec + 500) / 1000) & 0xFFF));
    }
}

NvU64 nvkms_get_usec(void) { return nvrm_now_ns() / 1000; }

/* "User" memory is a client's: the arena a client of nvrm shared for the request being served
   (os_memcpy_from_user / os_memcpy_to_user, nvrm_serve.c). The errors are Linux's -EINVAL and
   -EFAULT; NVKMS only tests for non-zero. */
int nvkms_copyin(void *kptr, NvU64 uaddr, size_t n) {
    if (!nvKmsNvU64AddressIsSafe(uaddr) || n > UINT32_MAX) return -22;
    if (os_memcpy_from_user(kptr, nvKmsNvU64ToPointer(uaddr), (NvU32)n) != NV_OK) return -14;
    return 0;
}

int nvkms_copyout(NvU64 uaddr, const void *kptr, size_t n) {
    if (!nvKmsNvU64AddressIsSafe(uaddr) || n > UINT32_MAX) return -22;
    if (os_memcpy_to_user(nvKmsNvU64ToPointer(uaddr), kptr, (NvU32)n) != NV_OK) return -14;
    return 0;
}

void nvkms_yield(void) { os_schedule(); }
void nvkms_dump_stack(void) { os_dump_stack(); }

int nvkms_snprintf(char *str, size_t size, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vsnprintf(str, size, format, ap);
    va_end(ap);
    return ret;
}

int nvkms_vsnprintf(char *str, size_t size, const char *format, va_list ap) {
    return vsnprintf(str, size, format, ap);
}

void nvkms_log(const int level, const char *gpuPrefix, const char *msg) {
    const char *levelPrefix;
    switch (level) {
    default:
    case NVKMS_LOG_LEVEL_INFO: levelPrefix = ""; break;
    case NVKMS_LOG_LEVEL_WARN: levelPrefix = "WARNING: "; break;
    case NVKMS_LOG_LEVEL_ERROR: levelPrefix = "ERROR: "; break;
    }
    nvrm_log(NVKMS_LOG_PREFIX "%s%s%s", levelPrefix, gpuPrefix ? gpuPrefix : "", msg ? msg : "");
}

/* ------------------------------------------------------------------------------------------
 * Locks. The handles NVKMS gets are RM's OS-layer objects (os.c), which have the semantics
 * Linux's struct semaphore and rw_semaphore give NVKMS there.
 * ---------------------------------------------------------------------------------------- */
static void *nvkms_lock;      /* os_alloc_semaphore(1): the global NVKMS lock */
static void *nvkms_pm_lock;   /* os_alloc_rwlock: clients read, suspend/resume write */

nvkms_sema_handle_t *nvkms_sema_alloc(void) { return (nvkms_sema_handle_t *)os_alloc_semaphore(1); }
void nvkms_sema_free(nvkms_sema_handle_t *sema) { os_free_semaphore(sema); }
void nvkms_sema_down(nvkms_sema_handle_t *sema) { os_acquire_semaphore(sema); }
void nvkms_sema_up(nvkms_sema_handle_t *sema) { os_release_semaphore(sema); }

static void lock_nvkms(void) { os_acquire_semaphore(nvkms_lock); }
static void unlock_nvkms(void) { os_release_semaphore(nvkms_lock); }
static void pm_read_lock(void) { os_acquire_rwlock_read(nvkms_pm_lock); }
static bool pm_read_trylock(void) { return os_cond_acquire_rwlock_read(nvkms_pm_lock) == NV_OK; }
static void pm_read_unlock(void) { os_release_rwlock_read(nvkms_pm_lock); }

/* ------------------------------------------------------------------------------------------
 * The NVKMS work thread: Linux's nvkms_kthread_q. One thread runs, in order, the items queued
 * for it (KAPI event notifications) and the timers whose time has come, so NVKMS's deferred
 * work never runs concurrently with itself, as NVIDIA's code assumes. RM's work queue (os.c) is
 * not used: its workers run RM work items (rm_execute_work_item) in parallel, and NVKMS's
 * items must neither enter RM's queue nor overtake each other.
 * ---------------------------------------------------------------------------------------- */
struct kms_item {
    struct kms_item *next;
    void (*run)(void *arg);
    void *arg;
    bool queued;          /* on the ready list (q.lock) */
};

static struct {
    mtx_t lock;
    cnd_t wake;           /* work arrived, a timer was armed, or stop */
    cnd_t done;           /* an item finished (flush waits on it) */
    struct kms_item *head, *tail;  /* ready, in order */
    struct nvkms_timer_t *timers;  /* armed, earliest first */
    bool stop;
    bool running;
    thrd_t thread;
} q;

static bool q_on_worker(void) { return q.running && thrd_equal(thrd_current(), q.thread); }

static void q_append_locked(struct kms_item *item) {
    item->next = NULL;
    item->queued = true;
    if (q.tail) q.tail->next = item;
    else q.head = item;
    q.tail = item;
}

/* nv_kthread_q_schedule_q_item: false if the item is already queued or the queue stopped. */
static bool q_schedule(struct kms_item *item) {
    bool ok = false;
    mtx_lock(&q.lock);
    if (!item->queued && !q.stop) {
        q_append_locked(item);
        cnd_signal(&q.wake);
        ok = true;
    }
    mtx_unlock(&q.lock);
    return ok;
}

/* Takes a queued item off the ready list again (it will not run). */
static void q_unqueue(struct kms_item *item) {
    mtx_lock(&q.lock);
    if (item->queued) {
        struct kms_item **link = &q.head, *prev = NULL;
        while (*link && *link != item) {
            prev = *link;
            link = &(*link)->next;
        }
        if (*link) {
            *link = item->next;
            if (q.tail == item) q.tail = prev;
        }
        item->queued = false;
    }
    mtx_unlock(&q.lock);
}

struct flush_mark {
    struct kms_item item;
    bool done;
};

static void flush_mark_run(void *arg) {
    struct flush_mark *mark = arg;
    mtx_lock(&q.lock);
    mark->done = true;
    cnd_broadcast(&q.done);
    mtx_unlock(&q.lock);
}

/* nv_kthread_q_flush: everything queued before the call has run when it returns. Twice, as
   NVIDIA does, so an item that queued another one while it ran is waited for as well. */
static void q_flush(void) {
    if (!q.running || q_on_worker()) return;
    for (int pass = 0; pass < 2; pass++) {
        struct flush_mark mark = { .item = { .run = flush_mark_run, .arg = &mark } };
        mtx_lock(&q.lock);
        q_append_locked(&mark.item);
        cnd_signal(&q.wake);
        while (!mark.done) cnd_wait(&q.done, &q.lock);
        mtx_unlock(&q.lock);
    }
}

/* ------------------------------------------------------------------------------------------
 * Timers (nvkms_alloc_timer & co.). An armed timer waits on q.timers until its time, then its
 * item runs on the work thread, which takes the power-management lock for reading and
 * nvkms_lock and calls the procedure unless the timer was cancelled. A timer on a ref_ptr is
 * cancelled when the object behind the ref_ptr is gone.
 * ---------------------------------------------------------------------------------------- */
struct nvkms_timer_t {
    struct kms_item item;
    struct nvkms_timer_t *next_armed;
    uint64_t due_ns;
    NvBool cancel;
    NvBool complete;
    NvBool isRefPtr;
    nvkms_timer_proc_t *proc;
    void *dataPtr;
    NvU32 dataU32;
};

struct nvkms_ref_ptr {
    uint32_t refcnt;      /* atomic */
    void *ptr;            /* guarded by nvkms_lock */
};

struct nvkms_ref_ptr *nvkms_alloc_ref_ptr(void *ptr) {
    struct nvkms_ref_ptr *ref_ptr = nvkms_alloc(sizeof(*ref_ptr), NV_FALSE);
    if (ref_ptr) {
        /* The ref_ptr owner counts as a reference on the ref_ptr itself. */
        ref_ptr->refcnt = 1;
        ref_ptr->ptr = ptr;
    }
    return ref_ptr;
}

void nvkms_free_ref_ptr(struct nvkms_ref_ptr *ref_ptr) {
    if (ref_ptr) {
        ref_ptr->ptr = NULL;
        nvkms_dec_ref(ref_ptr);
    }
}

void nvkms_inc_ref(struct nvkms_ref_ptr *ref_ptr) { __atomic_add_fetch(&ref_ptr->refcnt, 1, __ATOMIC_RELAXED); }

void *nvkms_dec_ref(struct nvkms_ref_ptr *ref_ptr) {
    void *ptr = ref_ptr->ptr;
    if (__atomic_sub_fetch(&ref_ptr->refcnt, 1, __ATOMIC_ACQ_REL) == 0) nvkms_free(ref_ptr, sizeof(*ref_ptr));
    return ptr;
}

static void timer_run(void *arg) {
    struct nvkms_timer_t *timer = arg;
    void *dataPtr;

    /* Block during suspend & resume, so that events such as DP IRQs and hotplugs are handled
       after resume. */
    pm_read_lock();
    lock_nvkms();

    if (timer->isRefPtr) {
        /* If the object this timer refers to was destroyed, treat the timer as cancelled. */
        dataPtr = nvkms_dec_ref(timer->dataPtr);
        if (!dataPtr) timer->cancel = NV_TRUE;
    } else {
        dataPtr = timer->dataPtr;
    }

    if (!timer->cancel) {
        timer->proc(dataPtr, timer->dataU32);
        timer->complete = NV_TRUE;
    }

    /* A ref_ptr timer belongs to nobody else; a plain one is freed here only when cancelled
       (otherwise nvkms_free_timer frees it once complete). */
    if (timer->isRefPtr || timer->cancel) free(timer);

    unlock_nvkms();
    pm_read_unlock();
}

static void timer_start(struct nvkms_timer_t *timer, nvkms_timer_proc_t *proc, void *dataPtr, NvU32 dataU32,
                        NvBool isRefPtr, NvU64 usec) {
    memset(timer, 0, sizeof(*timer));
    timer->isRefPtr = isRefPtr;
    timer->proc = proc;
    timer->dataPtr = dataPtr;
    timer->dataU32 = dataU32;
    timer->item.run = timer_run;
    timer->item.arg = timer;

    mtx_lock(&q.lock);
    if (usec == 0) {
        q_append_locked(&timer->item);
    } else {
        timer->due_ns = nvrm_now_ns() + (usec > UINT64_MAX / 1000 ? UINT64_MAX / 2 : usec * 1000);
        struct nvkms_timer_t **link = &q.timers;
        while (*link && (*link)->due_ns <= timer->due_ns) link = &(*link)->next_armed;
        timer->next_armed = *link;
        *link = timer;
    }
    cnd_signal(&q.wake);
    mtx_unlock(&q.lock);
}

nvkms_timer_handle_t *nvkms_alloc_timer(nvkms_timer_proc_t *proc, void *dataPtr, NvU32 dataU32, NvU64 usec) {
    /* nvkms_alloc, so fail_malloc can make it fail, as on Linux; the ref_ptr timers below are
       allocated past it (kmalloc there). nvkms_alloc is malloc, so both are released with free. */
    struct nvkms_timer_t *timer = nvkms_alloc(sizeof(*timer), NV_FALSE);
    if (timer) timer_start(timer, proc, dataPtr, dataU32, NV_FALSE, usec);
    return timer;
}

/* NVKMS calls this from RM's interrupt bottom half (rm_isr_bh on nvrm's interrupt thread),
   which may block here: it is an ordinary thread. */
NvBool nvkms_alloc_timer_with_ref_ptr(nvkms_timer_proc_t *proc, struct nvkms_ref_ptr *ref_ptr, NvU32 dataU32,
                                      NvU64 usec) {
    struct nvkms_timer_t *timer = malloc(sizeof(*timer));
    if (timer) {
        /* Reference the ref_ptr so that it is not freed before the timer fires. */
        nvkms_inc_ref(ref_ptr);
        timer_start(timer, proc, ref_ptr, dataU32, NV_TRUE, usec);
    }
    return timer != NULL;
}

void nvkms_free_timer(nvkms_timer_handle_t *handle) {
    struct nvkms_timer_t *timer = handle;
    if (timer == NULL) return;
    if (timer->complete) {
        free(timer);
        return;
    }
    timer->cancel = NV_TRUE;
}

static int q_thread(void *unused) {
    (void)unused;
    mtx_lock(&q.lock);
    for (;;) {
        uint64_t now = nvrm_now_ns();
        while (q.timers && q.timers->due_ns <= now) {
            struct nvkms_timer_t *timer = q.timers;
            q.timers = timer->next_armed;
            q_append_locked(&timer->item);
        }
        if (q.head) {
            struct kms_item *item = q.head;
            q.head = item->next;
            if (!q.head) q.tail = NULL;
            item->queued = false;
            mtx_unlock(&q.lock);
            item->run(item->arg);
            mtx_lock(&q.lock);
            cnd_broadcast(&q.done);
            continue;
        }
        if (q.stop) break;
        if (q.timers) {
            /* TIME_UTC is the clock cnd_timedwait takes (as os.c's timer thread does); a wall
               clock change only makes this wake early or late once, and the loop re-checks. */
            uint64_t wait_ns = q.timers->due_ns - now;
            struct timespec until;
            timespec_get(&until, TIME_UTC);
            uint64_t ns = (uint64_t)until.tv_nsec + wait_ns % 1000000000ull;
            until.tv_sec += (time_t)(wait_ns / 1000000000ull + ns / 1000000000ull);
            until.tv_nsec = (long)(ns % 1000000000ull);
            cnd_timedwait(&q.wake, &q.lock, &until);
        } else {
            cnd_wait(&q.wake, &q.lock);
        }
    }
    mtx_unlock(&q.lock);
    return 0;
}

static bool q_start(void) {
    memset(&q, 0, sizeof q);
    if (mtx_init(&q.lock, mtx_plain) != thrd_success) return false;
    if (cnd_init(&q.wake) != thrd_success || cnd_init(&q.done) != thrd_success) return false;
    if (thrd_create(&q.thread, q_thread, NULL) != thrd_success) return false;
    q.running = true;
    return true;
}

/* nvkms_exit: timers that have not come due are dropped (a ref_ptr timer gives its reference
   back), what is ready still runs (cancelled timers free themselves), then the thread ends. */
static void q_stop(void) {
    if (!q.running) return;
    mtx_lock(&q.lock);
    struct nvkms_timer_t *armed = q.timers;
    q.timers = NULL;
    mtx_unlock(&q.lock);
    while (armed) {
        struct nvkms_timer_t *timer = armed;
        armed = timer->next_armed;
        if (timer->isRefPtr) {
            lock_nvkms();
            nvkms_dec_ref(timer->dataPtr);
            unlock_nvkms();
        }
        free(timer);
    }
    mtx_lock(&q.lock);
    q.stop = true;
    cnd_signal(&q.wake);
    mtx_unlock(&q.lock);
    thrd_join(q.thread, NULL);
    q.running = false;
    cnd_destroy(&q.done);
    cnd_destroy(&q.wake);
    mtx_destroy(&q.lock);
}

/* ------------------------------------------------------------------------------------------
 * The RM side: nv-modeset-interface.c of nvidia.ko. nvrm drives one GPU (nvrm_dev); NVKMS sees
 * it once it is registered, which nvrm_nvkms_start does after RM started the adapter.
 * ---------------------------------------------------------------------------------------- */
static const nvidia_modeset_callbacks_t *modeset_callbacks;
static bool gpu_registered;   /* nvrm_dev is listed and can be opened */
static mtx_t gpu_lock;        /* nvl->ldata_lock: gpu_registered and gpu_opens */
static unsigned gpu_opens;

/* RM's altstack: nvrm has one shared dummy (no __use_altstack__, NV_ENTER_RM_RUNTIME does not
   switch stacks), which every thread passes, as RM's own threads do. */
static int rm_alloc_stack(nvidia_modeset_stack_ptr *sp) {
    *sp = nvrm_dev.sp;
    return nvrm_dev.sp ? 0 : -12; /* -ENOMEM */
}
static void rm_free_stack(nvidia_modeset_stack_ptr sp) { (void)sp; }

static void rm_gpu_info(nv_gpu_info_t *gpu_info) {
    const nv_state_t *nv = &nvrm_dev.nv;
    memset(gpu_info, 0, sizeof(*gpu_info));
    gpu_info->gpu_id = nv->gpu_id;
    gpu_info->pci_info.domain = nv->pci_info.domain;
    gpu_info->pci_info.bus = nv->pci_info.bus;
    gpu_info->pci_info.slot = nv->pci_info.slot;
    gpu_info->pci_info.function = nv->pci_info.function;
    /* NUMA onlining of GPU memory is for coherent (NVLink C2C) systems; a PCIe GPU has none. */
    gpu_info->needs_numa_setup = NV_FALSE;
    /* Linux's struct device, which only nvidia-drm uses (for its DRM device): none here. */
    gpu_info->os_device_ptr = NULL;
    gpu_info->is_soc_disp = NV_IS_SOC_DISPLAY_DEVICE(nv);
}

static NvU32 rm_enumerate_gpus(nv_gpu_info_t *gpu_info) {
    NvU32 count = 0;
    mtx_lock(&gpu_lock);
    if (gpu_registered) rm_gpu_info(&gpu_info[count++]);
    mtx_unlock(&gpu_lock);
    return count;
}

/* nvidia_dev_get / nvidia_dev_put: the GPU open counts nvrm keeps for every client outside RM
   (os_ioctl.c); gpu_opens counts NVKMS's among them. */
static int rm_open_gpu(NvU32 gpu_id, nvidia_modeset_stack_ptr sp, NvBool reset_aware) {
    (void)sp;
    int rc = -19; /* -ENODEV */
    mtx_lock(&gpu_lock);
    if (gpu_registered && nvrm_gpu_get(gpu_id, reset_aware) == NV_OK) {
        gpu_opens++;
        rc = 0;
    }
    mtx_unlock(&gpu_lock);
    return rc;
}

static void rm_close_gpu(NvU32 gpu_id, nvidia_modeset_stack_ptr sp, NvBool reset_aware) {
    (void)sp;
    mtx_lock(&gpu_lock);
    if (gpu_opens && gpu_id == nvrm_dev.nv.gpu_id) {
        gpu_opens--;
        nvrm_gpu_put(gpu_id, reset_aware);
    }
    mtx_unlock(&gpu_lock);
}

static int rm_set_callbacks(const nvidia_modeset_callbacks_t *cb) {
    if ((modeset_callbacks != NULL && cb != NULL) || (modeset_callbacks == NULL && cb == NULL))
        return -22; /* -EINVAL */
    modeset_callbacks = cb;
    return 0;
}

/* nvidia_get_rm_ops. Write combining for the framebuffer: Nocturne maps BAR memory
   write-combined (N_CACHE_WRITE_COMBINE), as Linux does on x86-64. */
static NV_STATUS rm_get_ops(nvidia_modeset_rm_ops_t *rm_ops) {
    const nvidia_modeset_rm_ops_t local_rm_ops = {
        .version_string = NV_VERSION_STRING,
        .system_info = { .allow_write_combining = NV_TRUE },
        .alloc_stack = rm_alloc_stack,
        .free_stack = rm_free_stack,
        .enumerate_gpus = rm_enumerate_gpus,
        .open_gpu = rm_open_gpu,
        .close_gpu = rm_close_gpu,
        .op = rm_kernel_rmapi_op,
        .set_callbacks = rm_set_callbacks,
    };
    if (strcmp(rm_ops->version_string, NV_VERSION_STRING) != 0) {
        rm_ops->version_string = NV_VERSION_STRING;
        return NV_ERR_GENERIC;
    }
    *rm_ops = local_rm_ops;
    return NV_OK;
}

/* ------------------------------------------------------------------------------------------
 * The NVKMS side of that interface (nvidia-modeset-linux.c)
 * ---------------------------------------------------------------------------------------- */
static nvidia_modeset_rm_ops_t rm_ops;

void nvkms_call_rm(void *ops) {
    nvidia_modeset_stack_ptr stack = NULL;
    if (rm_ops.alloc_stack(&stack) != 0) return;
    rm_ops.op(stack, ops);
    rm_ops.free_stack(stack);
}

NvBool nvkms_open_gpu(NvU32 gpuId, NvBool reset_aware) {
    nvidia_modeset_stack_ptr stack = NULL;
    if (rm_ops.alloc_stack(&stack) != 0) return NV_FALSE;
    NvBool ret = rm_ops.open_gpu(gpuId, stack, reset_aware) == 0;
    rm_ops.free_stack(stack);
    return ret;
}

void nvkms_close_gpu(NvU32 gpuId, NvBool reset_aware) {
    nvidia_modeset_stack_ptr stack = NULL;
    if (rm_ops.alloc_stack(&stack) != 0) return;
    rm_ops.close_gpu(gpuId, stack, reset_aware);
    rm_ops.free_stack(stack);
}

NvU32 nvkms_enumerate_gpus(nv_gpu_info_t *gpu_info) { return rm_ops.enumerate_gpus(gpu_info); }
NvBool nvkms_allow_write_combining(void) { return rm_ops.system_info.allow_write_combining; }

static void nvkms_suspend(NvU32 gpuId) {
    nvKmsKapiSuspendResume(NV_TRUE);
    if (gpuId == 0) os_acquire_rwlock_write(nvkms_pm_lock);
    lock_nvkms();
    nvKmsSuspend(gpuId);
    unlock_nvkms();
}

static void nvkms_resume(NvU32 gpuId) {
    lock_nvkms();
    nvKmsResume(gpuId);
    unlock_nvkms();
    if (gpuId == 0) os_release_rwlock_write(nvkms_pm_lock);
    nvKmsKapiSuspendResume(NV_FALSE);
}

static void nvkms_remove(NvU32 gpuId) {
    /* NVKMS frees its device state when its clients free their devices; all of them must be
       closed before a GPU is removed (as on Linux). */
    nvKmsKapiRemove(gpuId);
}

static void nvkms_probe(const nv_gpu_info_t *gpu_info) { nvKmsKapiProbe(gpu_info); }

static const nvidia_modeset_callbacks_t nvkms_rm_callbacks = {
    .suspend = nvkms_suspend,
    .resume = nvkms_resume,
    .remove = nvkms_remove,
    .probe = nvkms_probe,
};

/* ------------------------------------------------------------------------------------------
 * Clients. Each open of NVKMS is a nvkms_per_open; KAPI's are kernel clients, whose "events
 * available" notification runs nvKmsKapiHandleEventQueueChange on the work thread.
 * ---------------------------------------------------------------------------------------- */
struct nvkms_per_open {
    void *data;
    enum NvKmsClientType type;
    struct kms_item events;   /* NVKMS_CLIENT_KERNEL_SPACE */
};

static void kapi_event_run(void *arg) { nvKmsKapiHandleEventQueueChange(arg); }

void nvkms_event_queue_changed(nvkms_per_open_handle_t *pOpenKernel, NvBool eventsAvailable) {
    struct nvkms_per_open *popen = pOpenKernel;
    /* Only kernel clients exist (nvkms_get_per_open_data): they are told on the work thread.
       A notification already queued covers this one. */
    if (popen && popen->type == NVKMS_CLIENT_KERNEL_SPACE && eventsAvailable) q_schedule(&popen->events);
}

struct nvkms_per_open *nvkms_open_from_kapi(struct NvKmsKapiDevice *device) {
    struct nvkms_per_open *popen = nvkms_alloc(sizeof(*popen), NV_TRUE);
    if (!popen) return NULL;
    popen->type = NVKMS_CLIENT_KERNEL_SPACE;
    /* Ready before NVKMS knows the client, so an event that arrives at once finds it. */
    popen->events.run = kapi_event_run;
    popen->events.arg = device;

    pm_read_lock();
    lock_nvkms();
    popen->data = nvKmsOpen(os_get_current_process(), NVKMS_CLIENT_KERNEL_SPACE, popen);
    unlock_nvkms();
    pm_read_unlock();

    if (!popen->data) {
        nvkms_free(popen, sizeof(*popen));
        return NULL;
    }
    return popen;
}

void nvkms_close_from_kapi(struct nvkms_per_open *popen) {
    pm_read_lock();
    lock_nvkms();
    nvKmsClose(popen->data);
    popen->data = NULL;
    unlock_nvkms();
    pm_read_unlock();

    /* No notification is queued for this client any more after nvKmsClose; drop one already
       waiting, and wait for one that is running (unless that is the caller itself), before
       the memory it lives in goes. The PM lock is given back first: the work thread may be
       waiting for it behind a suspend, which in turn waits for this reader. */
    q_unqueue(&popen->events);
    q_flush();

    nvkms_free(popen, sizeof(*popen));
}

static NvBool ioctl_locked_pm(struct nvkms_per_open *popen, NvU32 cmd, void *params_address, size_t size) {
    NvBool ret = NV_FALSE;
    lock_nvkms();
    if (popen->data != NULL) ret = nvKmsIoctl(popen->data, cmd, (NvU64)(NvUPtr)params_address, size);
    unlock_nvkms();
    return ret;
}

NvBool nvkms_ioctl_from_kapi(struct nvkms_per_open *popen, NvU32 cmd, void *params_address, const size_t params_size) {
    pm_read_lock();
    NvBool ret = ioctl_locked_pm(popen, cmd, params_address, params_size);
    pm_read_unlock();
    return ret;
}

/* The PM lock may be unavailable (a suspend is in progress): then the request fails instead of
   waiting (NVIDIA bug 4432810). */
NvBool nvkms_ioctl_from_kapi_try_pmlock(struct nvkms_per_open *popen, NvU32 cmd, void *params_address,
                                        const size_t params_size) {
    if (!pm_read_trylock()) return NV_FALSE;
    NvBool ret = ioctl_locked_pm(popen, cmd, params_address, params_size);
    pm_read_unlock();
    return ret;
}

/* ------------------------------------------------------------------------------------------
 * KAPI, the interface nvidia-drm uses on Linux (EXPORT_SYMBOL there)
 * ---------------------------------------------------------------------------------------- */
NvBool nvKmsKapiGetFunctionsTable(struct NvKmsKapiFunctionsTable *funcsTable) {
    return nvKmsKapiGetFunctionsTableInternal(funcsTable);
}
NvU32 nvKmsKapiF16ToF32(NvU16 a) { return nvKmsKapiF16ToF32Internal(a); }
NvU16 nvKmsKapiF32ToF16(NvU32 a) { return nvKmsKapiF32ToF16Internal(a); }
NvU32 nvKmsKapiF32Mul(NvU32 a, NvU32 b) { return nvKmsKapiF32MulInternal(a, b); }
NvU32 nvKmsKapiF32Div(NvU32 a, NvU32 b) { return nvKmsKapiF32DivInternal(a, b); }
NvU32 nvKmsKapiF32Add(NvU32 a, NvU32 b) { return nvKmsKapiF32AddInternal(a, b); }
NvU32 nvKmsKapiF32ToUI32RMinMag(NvU32 a, NvBool exact) { return nvKmsKapiF32ToUI32RMinMagInternal(a, exact); }
NvU32 nvKmsKapiUI32ToF32(NvU32 a) { return nvKmsKapiUI32ToF32Internal(a); }

/* ------------------------------------------------------------------------------------------
 * The configuration file (config_file option): the same text format as on Linux. EDID
 * overrides in it name further files, which NVKMS reads through the same function.
 * ---------------------------------------------------------------------------------------- */
static size_t config_file_read(char *fname, char **const buff) {
    *buff = NULL;
    void *data = NULL;
    size_t size = 0;
    n_result r = n_document_load(fname, &data, &size);
    if (r < 0) {
        nvrm_log(NVKMS_LOG_PREFIX "WARNING: Failed to open %s (%s)", fname, n_result_text(r));
        return 0;
    }
    size_t file_size = 0;
    if (size > NVKMS_READ_FILE_MAX_SIZE) {
        nvrm_log(NVKMS_LOG_PREFIX "WARNING: File exceeds maximum size");
    } else if (size) {
        *buff = nvkms_alloc(size, NV_FALSE);
        if (*buff) {
            memcpy(*buff, data, size);
            file_size = size;
        } else {
            nvrm_log(NVKMS_LOG_PREFIX "WARNING: Out of memory");
        }
    }
    n_memory_release(data, size);
    return file_size;
}

/* Called with nvkms_lock held. */
static void config_file_apply(void) {
    if (!opt.config_file[0]) return;
    char *buffer = NULL;
    size_t buf_size = config_file_read(opt.config_file, &buffer);
    if (buf_size == 0) return;
    if (nvKmsReadConf(buffer, buf_size, config_file_read))
        nvrm_log(NVKMS_LOG_PREFIX "Successfully read %s", opt.config_file);
    nvkms_free(buffer, buf_size);
}

/* ------------------------------------------------------------------------------------------
 * Start and stop (module_init / module_exit, plus the GPU's probe and remove)
 * ---------------------------------------------------------------------------------------- */
static bool nvkms_started;

static void nvkms_release(void) {
    q_stop();
    if (modeset_callbacks) rm_ops.set_callbacks(NULL);
    if (nvkms_pm_lock) os_free_rwlock(nvkms_pm_lock);
    if (nvkms_lock) os_free_semaphore(nvkms_lock);
    nvkms_pm_lock = nvkms_lock = NULL;
    mtx_destroy(&gpu_lock);
}

bool nvrm_nvkms_start(void) {
    if (nvkms_started) return true;
    if (!nvrm_dev.adapter_ready) {
        nvrm_log(NVKMS_LOG_PREFIX "the GPU is not running; NVKMS is not loaded");
        return false;
    }
    options_read();
    __atomic_store_n(&alloc_count, 0, __ATOMIC_RELAXED);

    rm_ops.version_string = NV_VERSION_STRING;
    if (rm_get_ops(&rm_ops) != NV_OK) {
        nvrm_log(NVKMS_LOG_PREFIX "Version mismatch: nvidia(%s) nvidia-modeset(%s)", rm_ops.version_string,
                 NV_VERSION_STRING);
        return false;
    }
    if (mtx_init(&gpu_lock, mtx_plain) != thrd_success) return false;
    if (rm_ops.set_callbacks(&nvkms_rm_callbacks) < 0) {
        nvrm_log(NVKMS_LOG_PREFIX "Failed to register callbacks");
        mtx_destroy(&gpu_lock);
        return false;
    }

    nvkms_lock = os_alloc_semaphore(1);
    nvkms_pm_lock = os_alloc_rwlock();
    if (!nvkms_lock || !nvkms_pm_lock || !q_start()) {
        nvrm_log(NVKMS_LOG_PREFIX "cannot create NVKMS's locks or work thread");
        nvkms_release();
        return false;
    }

    lock_nvkms();
    bool loaded = nvKmsModuleLoad();
    if (loaded) config_file_apply();
    unlock_nvkms();
    if (!loaded) {
        nvrm_log(NVKMS_LOG_PREFIX "nvKmsModuleLoad failed");
        nvkms_release();
        return false;
    }

    /* The GPU appears to NVKMS (nvidia_modeset_probe after a device is probed). */
    mtx_lock(&gpu_lock);
    gpu_registered = true;
    mtx_unlock(&gpu_lock);
    nv_gpu_info_t info;
    rm_gpu_info(&info);
    if (modeset_callbacks && modeset_callbacks->probe) modeset_callbacks->probe(&info);

    nvkms_started = true;
    return true;
}

void nvrm_nvkms_stop(void) {
    if (!nvkms_started) return;

    /* nvidia_modeset_remove when the device goes away, then the GPU is no longer listed. */
    if (modeset_callbacks && modeset_callbacks->remove) modeset_callbacks->remove(nvrm_dev.nv.gpu_id);
    mtx_lock(&gpu_lock);
    gpu_registered = false;
    if (gpu_opens) nvrm_log(NVKMS_LOG_PREFIX "WARNING: the GPU is still open %u times while NVKMS stops", gpu_opens);
    mtx_unlock(&gpu_lock);

    lock_nvkms();
    nvKmsModuleUnload();
    unlock_nvkms();

    nvkms_release();
    if (opt.malloc_verbose)
        nvrm_log(NVKMS_LOG_PREFIX "Total allocations: %d", __atomic_load_n(&alloc_count, __ATOMIC_RELAXED));
    nvkms_started = false;
}

/* nv_set_system_power_state: the system-wide suspend (gpu id 0, which takes the PM lock for
   writing and so holds back every client) comes first, then the device's own; resume in the
   opposite order. */
void nvrm_nvkms_suspend(void) {
    if (!nvkms_started || !modeset_callbacks) return;
    modeset_callbacks->suspend(0);
    modeset_callbacks->suspend(nvrm_dev.nv.gpu_id);
}

void nvrm_nvkms_resume(void) {
    if (!nvkms_started || !modeset_callbacks) return;
    modeset_callbacks->resume(nvrm_dev.nv.gpu_id);
    modeset_callbacks->resume(0);
}
