/* NVIDIA RM on Nocturne: RM's capabilities (the port of kernel-open/nvidia/nv-caps.c and the
 * fabric-management part of nvlink_caps.c).
 *
 * RM guards a few features with capabilities rather than with "is the caller an administrator":
 * MIG configuration and monitoring, the GPU-instance and compute-instance access of MIG, the
 * fabric manager (NVLink/NVSwitch), IMEX, the profilers, the trace device and WPPS. It registers
 * them as a tree (osRmCapRegisterSys, osRmCapRegisterGpu, osRmCapRegisterSmc*Partition), and an
 * RM client that wants one names it by a descriptor in its allocation parameters
 * (capDescriptor); RM asks the OS whether the descriptor really grants that capability
 * (os_nv_cap_validate_and_dup_fd) and keeps what the OS returns until the object is freed
 * (os_nv_cap_close_fd).
 *
 * The original makes every capability a device file with a minor number of its own, gives the
 * file the capability's mode, and accepts an open file descriptor of that device as proof. The
 * proof here is the requesting client's own connection, which the system authenticated
 * (nvrm_serve.c): a descriptor names the capability by the same number the original gives its
 * minor (the same table order: NVLink, MIG, the MIG instances of 32 GPUs, the system ones), and
 * the connection's right decides whether the capability's mode lets it hold it. OS_RUSR (the
 * owner of the original device file, root) is an administrator's; the "others" read bit
 * (OS_RUGO) is everyone's. What RM gets back is a hold of its own, which os_nv_cap_close_fd
 * ends; the original's duplicated descriptor does the same job.
 *
 * Diagnostics: the original lists the minors in /proc/driver/nvidia-caps; each file
 * capability is logged with its descriptor instead when RM creates it.
 */
#include "nvrm_os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <os-interface.h>
#include <nv-caps.h>
/* os_custom.h (the OS_R* mode bits) names two RM types in its declarations; this file only
   needs the bits, so the types are declared as RM's own headers declare them. */
typedef struct OBJGPU OBJGPU;
typedef struct PACKED_REGISTRY_TABLE PACKED_REGISTRY_TABLE;
#include <os_custom.h>

struct nv_cap {
    char *path;            /* "/driver/nvidia/capabilities/mig/config", the original's names */
    const char *name;      /* the last element of path */
    int mode;              /* OS_R* bits RM gave it */
    int minor;             /* the descriptor that names it; -1 for a directory */
    bool fixed;            /* one of the roots below: never freed */
};

/* The root the original's module init creates (nv_cap_init("/driver/nvidia")), and the one
   capability of the NVLink library's own root (nvlink_cap_init("/driver/nvidia-nvlink")), which
   RM never extends. */
static nv_cap_t nvidia_root = {
    .path = "/driver/nvidia/capabilities", .name = "capabilities", .mode = OS_RUGO | OS_XUGO, .minor = -1,
    .fixed = true,
};
static nv_cap_t nvlink_fabric_mgmt = {
    .path = "/driver/nvidia-nvlink/capabilities/fabric-mgmt", .name = "fabric-mgmt", .mode = OS_RUSR,
    .minor = 0, .fixed = true,
};

nv_cap_t *nvidia_caps_root = &nvidia_root;

/* ---------------------------------------------------------------- descriptor numbers */
/* The original's table (nv-caps.c): minors in this order, from 0. */
#define CAP_MIG_FIRST 1u                         /* mig/config, mig/monitor */
#define CAP_GPUS 32u
#define CAP_GIS 15u                              /* gi0..gi14 */
#define CAP_CIS 8u                               /* ci0..ci7 */
#define CAP_PER_GI (1u + CAP_CIS)                /* gi%u/access, then its ci%u/access */
#define CAP_PER_GPU (CAP_GIS * CAP_PER_GI)
#define CAP_MIG_GPU_FIRST (CAP_MIG_FIRST + 2u)
#define CAP_SYS_FIRST (CAP_MIG_GPU_FIRST + CAP_GPUS * CAP_PER_GPU)

static const char *const sys_caps[] = {
    "fabric-imex-mgmt", "profiler-device", "profiler-context", "trace-device", "wpps-access",
};

/* A decimal number below limit, without sign or leading zero; advances *at past it. */
static bool take_number(const char **at, unsigned limit, unsigned *value) {
    const char *p = *at;
    if (*p < '0' || *p > '9' || (*p == '0' && p[1] >= '0' && p[1] <= '9')) return false;
    unsigned v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (unsigned)(*p++ - '0');
        if (v >= limit) return false;
    }
    *at = p;
    *value = v;
    return true;
}

static bool take(const char **at, const char *text) {
    size_t n = strlen(text);
    if (strncmp(*at, text, n)) return false;
    *at += n;
    return true;
}

/* The descriptor of a file capability, -1 for a path the original's table does not have. */
static int cap_minor(const char *path) {
    if (!strcmp(path, nvlink_fabric_mgmt.path)) return 0;
    const char *p = path;
    if (!take(&p, "/driver/nvidia/capabilities/")) return -1;
    if (!strcmp(p, "mig/config")) return (int)CAP_MIG_FIRST;
    if (!strcmp(p, "mig/monitor")) return (int)CAP_MIG_FIRST + 1;
    for (unsigned i = 0; i < sizeof sys_caps / sizeof sys_caps[0]; i++)
        if (!strcmp(p, sys_caps[i])) return (int)(CAP_SYS_FIRST + i);
    unsigned gpu, gi, ci;
    if (!take(&p, "gpu") || !take_number(&p, CAP_GPUS, &gpu) || !take(&p, "/mig/gi") ||
        !take_number(&p, CAP_GIS, &gi))
        return -1;
    unsigned at = CAP_MIG_GPU_FIRST + gpu * CAP_PER_GPU + gi * CAP_PER_GI;
    if (!strcmp(p, "/access")) return (int)at;
    if (!take(&p, "/ci") || !take_number(&p, CAP_CIS, &ci) || strcmp(p, "/access")) return -1;
    return (int)(at + 1 + ci);
}

/* ---------------------------------------------------------------- the tree */
static nv_cap_t *cap_new(nv_cap_t *parent, const char *name, int mode) {
    if (!parent || !name || !*name || strchr(name, '/')) return NULL;
    nv_cap_t *cap = calloc(1, sizeof *cap);
    if (!cap) return NULL;
    size_t length = strlen(parent->path) + 1 + strlen(name) + 1;
    cap->path = malloc(length);
    if (!cap->path) {
        free(cap);
        return NULL;
    }
    snprintf(cap->path, length, "%s/%s", parent->path, name);
    cap->name = cap->path + strlen(parent->path) + 1;
    cap->mode = mode;
    cap->minor = -1;
    return cap;
}

nv_cap_t *NV_API_CALL os_nv_cap_create_dir_entry(nv_cap_t *parent, const char *name, int mode) {
    return cap_new(parent, name, mode);
}

nv_cap_t *NV_API_CALL os_nv_cap_create_file_entry(nv_cap_t *parent, const char *name, int mode) {
    nv_cap_t *cap = cap_new(parent, name, mode);
    if (!cap) return NULL;
    cap->minor = cap_minor(cap->path);
    if (cap->minor < 0) {
        /* as the original: a capability it has no minor for is not created */
        nvrm_log("NVRM: capability %s is not one RM's table has", cap->path);
        free(cap->path);
        free(cap);
        return NULL;
    }
    nvrm_log("NVRM: capability %s: descriptor %d, %s", cap->path, cap->minor,
             mode & OS_ROTH ? "every client" : mode & OS_RUSR ? "administrators" : "nobody");
    return cap;
}

void NV_API_CALL os_nv_cap_destroy_entry(nv_cap_t *cap) {
    if (!cap || cap->fixed) return;
    free(cap->path);
    free(cap);
}

/* ---------------------------------------------------------------- holds */
/* What RM keeps while an object uses a capability. Numbers are not reused while held. */
struct cap_hold {
    struct cap_hold *next;
    int id;
    int minor;
    uint32_t principal;    /* the app that showed the capability, 0 for nvrm itself */
};

static mtx_t holds_lock;
static once_flag holds_once = ONCE_FLAG_INIT;
static struct cap_hold *holds;
static int next_hold_id;

static void holds_setup(void) { mtx_init(&holds_lock, mtx_plain); }

/* Whether the requesting connection may hold a capability of this mode: the original's device
   file belongs to root and has the capability's mode. */
static bool cap_allowed(const nv_cap_t *cap) {
    if (cap->mode & OS_ROTH) return true;
    return nvrm_thread_admin && (cap->mode & OS_RUSR);
}

static bool hold_taken(int id) {
    for (struct cap_hold *h = holds; h; h = h->next)
        if (h->id == id) return true;
    return false;
}

int NV_API_CALL os_nv_cap_validate_and_dup_fd(const nv_cap_t *cap, int fd) {
    if (!cap || cap->minor < 0 || fd != cap->minor || !cap_allowed(cap)) return -1;
    struct cap_hold *h = calloc(1, sizeof *h);
    if (!h) return -1;
    h->minor = cap->minor;
    h->principal = nvrm_thread_process;
    call_once(&holds_once, holds_setup);
    mtx_lock(&holds_lock);
    /* ids are non-negative ints (RM keeps them as an fd); skip any still held after a wrap */
    do {
        h->id = next_hold_id;
        next_hold_id = next_hold_id == INT32_MAX ? 0 : next_hold_id + 1;
    } while (hold_taken(h->id));
    h->next = holds;
    holds = h;
    mtx_unlock(&holds_lock);
    return h->id;
}

void NV_API_CALL os_nv_cap_close_fd(int fd) {
    if (fd < 0) return;
    call_once(&holds_once, holds_setup);
    mtx_lock(&holds_lock);
    struct cap_hold *found = NULL;
    for (struct cap_hold **at = &holds; *at; at = &(*at)->next)
        if ((*at)->id == fd) {
            found = *at;
            *at = found->next;
            break;
        }
    mtx_unlock(&holds_lock);
    free(found);
}

/* RM's fabric manager session (FM_SESSION_API): the NVLink library's fabric-mgmt capability
   (nv.c, nvlink_caps.c in the original). */
NV_STATUS NV_API_CALL nv_acquire_fabric_mgmt_cap(int fd, int *duped) {
    if (!duped) return NV_ERR_INVALID_ARGUMENT;
    *duped = os_nv_cap_validate_and_dup_fd(&nvlink_fabric_mgmt, fd);
    return *duped < 0 ? NV_ERR_INSUFFICIENT_PERMISSIONS : NV_OK;
}
