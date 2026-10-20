/* NVIDIA RM on Nocturne: the entry points the request server uses (nvrm_serve.h).
 *
 * A client of nvrm is what RM calls an open file of its control device: it gets an
 * nv_file_private_t, every escape goes to rm_ioctl on the control device's state, and closing
 * the client runs rm_cleanup_file_private, which frees every RM object the client still holds.
 */
#include "nvrm_os.h"

#include <stdlib.h>
#include <string.h>

#include <nv-ioctl.h>
#include <nv_escape.h>
#include <nv-unix-nvos-params-wrappers.h>
#include <nvos.h>
#include <os/nv_memory_type.h>

#define PAGE 4096ull

/* pLinearAddress of the last NV_ESC_RM_MAP_MEMORY this thread ran: its low 12 bits are the
   position of the mapping inside its first page (RM does not keep that in the parked context). */
static thread_local uint64_t last_linear;

/* Mapping publication, revocation and file-list membership share one lock. It is
   recursive because RM may call its mmap-lock callbacks while holding it. Never
   hold it across rm_ioctl or rm_cleanup_file_private (RM owns their GPU locks). */
static mtx_t mmap_lock;
static once_flag mmap_once = ONCE_FLAG_INIT;
static void mmap_setup(void) { mtx_init(&mmap_lock, mtx_recursive); }
void NV_API_CALL nv_acquire_mmap_lock(nv_state_t *nv) {
    (void)nv; call_once(&mmap_once, mmap_setup); mtx_lock(&mmap_lock);
}
void NV_API_CALL nv_release_mmap_lock(nv_state_t *nv) { (void)nv; mtx_unlock(&mmap_lock); }

struct exported_mapping {
    struct exported_mapping *next;
    struct nvrm_file *file;
    n_handle view;
};

/* Events RM posts for a client wait here until the client asks for them with
 * NV_ESC_RM_GET_EVENT_DATA (rm_ioctl calls nv_get_event). The protocol is request and reply, so
 * a client polls; nothing is pushed to its channel. */
struct queued_event {
    struct queued_event *next;
    NvHandle object;
    NvU32 index, info32;
    NvU16 info16;
    bool data_valid;
};

/* One client: RM's per-open-file state first (RM hands out pointers to it), then what only
 * nvrm keeps. */
struct nvrm_file {
    nv_file_private_t nvfp;
    struct nvrm_file *next;
    uint32_t principal;          /* connection identity, never reused during this daemon */
    bool admin;                 /* RM treats the client as an administrator */
    mtx_t lock;                 /* the queue: RM posts from its interrupt and work threads */
    struct queued_event *head, *tail;
    struct exported_mapping *mappings;
};

static struct nvrm_file *files;
static uint32_t next_principal = 1; /* zero means the actual uint32_t RM identity space is exhausted */
static bool all_mappings_revoked;
static thread_local struct nvrm_file *request_file;
static thread_local struct exported_mapping *pending_user_mapping;

static void mapping_close(struct exported_mapping *m) {
    /* A client retaining a derived view must not retain access after RM unmap. */
    if (m->view >= 0) {
        n_shared_revoke(m->view);
        n_handle_close(m->view);
    }
    free(m);
}

void *nvrm_user_mapping_new(void) {
    if (!request_file) return NULL;
    struct exported_mapping *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->view = -1;
    m->file = request_file;
    nv_acquire_mmap_lock(&nvrm_dev.nv);
    if (__atomic_load_n(&nvrm_dev.mappings_revoked, __ATOMIC_ACQUIRE)) {
        nv_release_mmap_lock(&nvrm_dev.nv);
        free(m);
        return NULL;
    }
    m->next = request_file->mappings;
    request_file->mappings = m;
    pending_user_mapping = m;
    nv_release_mmap_lock(&nvrm_dev.nv);
    return m;
}

void *nvrm_user_mapping_current(void) { return pending_user_mapping; }

void nvrm_user_mapping_free(void *owner) {
    struct exported_mapping *m = owner;
    if (!m) return;
    nv_acquire_mmap_lock(&nvrm_dev.nv);
    struct exported_mapping **at = &m->file->mappings;
    while (*at && *at != m) at = &(*at)->next;
    if (*at) {
        *at = m->next;
        if (pending_user_mapping == m) pending_user_mapping = NULL;
        mapping_close(m);
    }
    nv_release_mmap_lock(&nvrm_dev.nv);
}

NV_STATUS NV_API_CALL nv_revoke_gpu_mappings(nv_state_t *nv) {
    nv_acquire_mmap_lock(nv);
    __atomic_store_n(&nvrm_dev.mappings_revoked, true, __ATOMIC_RELEASE);
    bool complete = true;
    for (struct nvrm_file *f = files; f; f = f->next)
        for (struct exported_mapping *m = f->mappings; m; m = m->next)
            if (m->view >= 0 && n_shared_revoke(m->view) < 0) complete = false;
    all_mappings_revoked = complete;
    nv_release_mmap_lock(nv);
    return complete ? NV_OK : NV_ERR_OPERATING_SYSTEM;
}
NvBool NV_API_CALL nv_get_all_mappings_revoked_locked(nv_state_t *nv) {
    (void)nv;
    return all_mappings_revoked ? NV_TRUE : NV_FALSE;
}
void NV_API_CALL nv_set_safe_to_mmap_locked(nv_state_t *nv, NvBool safe) {
    (void)nv;
    __atomic_store_n(&nvrm_dev.mappings_revoked, !safe, __ATOMIC_RELEASE);
    if (safe) all_mappings_revoked = false;
}

void *nvrm_rm_file_open(bool admin) {
    struct nvrm_file *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    if (mtx_init(&f->lock, mtx_plain) != thrd_success) {
        free(f);
        return NULL;
    }
    f->nvfp.deviceInstance = NV_MAX_DEVICES;
    f->nvfp.gpuInstanceId = NV_U32_MAX;
    f->admin = admin;
    /* No authenticated channel-peer identity exists in the native API. Give
       each open connection a distinct RM process principal instead of accepting
       an app-supplied PID or conflating all callers with the daemon. Do not reuse
       identities: an explicit sharing policy may outlive its original file. */
    n_result daemon = n_app_self();
    nv_acquire_mmap_lock(&nvrm_dev.nv);
    if (daemon > 0 && (uint64_t)daemon <= UINT32_MAX && next_principal) {
        if (next_principal == (uint32_t)daemon) next_principal++;
        f->principal = next_principal;
        if (next_principal) next_principal++;
    }
    if (f->principal) { f->next = files; files = f; }
    nv_release_mmap_lock(&nvrm_dev.nv);
    if (!f->principal) { mtx_destroy(&f->lock); free(f); return NULL; }
    return f;
}

void nvrm_rm_file_close(void *file) {
    struct nvrm_file *f = file;
    if (!f) return;
    bool admin = nvrm_thread_admin;
    uint32_t principal = nvrm_thread_process;
    struct nvrm_file *saved_file = request_file;
    /* Drop CPU access before RM can release/reuse the underlying VRAM. RM's
       per-map destructor below then closes the exact view, including duplicate
       maps of one physical address, without invalidating an unrelated map. */
    nv_acquire_mmap_lock(&nvrm_dev.nv);
    for (struct exported_mapping *m = f->mappings; m; m = m->next)
        if (m->view >= 0) n_shared_revoke(m->view);
    nv_release_mmap_lock(&nvrm_dev.nv);
    request_file = f;
    nvrm_thread_admin = f->admin;
    nvrm_thread_process = f->principal;
    rm_cleanup_file_private(nvrm_dev.sp, nv_get_ctl_state(), &f->nvfp);
    nvrm_thread_admin = admin;
    nvrm_thread_process = principal;
    request_file = saved_file;
    /* RM stops posting to the file when its events are freed; wait for posts in flight. */
    while (nv_event_posting_refcount_read(&f->nvfp) > 0) thrd_yield();
    while (f->head) {
        struct queued_event *e = f->head;
        f->head = e->next;
        free(e);
    }
    nv_acquire_mmap_lock(&nvrm_dev.nv);
    for (struct nvrm_file **at = &files; *at; at = &(*at)->next)
        if (*at == f) { *at = f->next; break; }
    while (f->mappings) {
        struct exported_mapping *m = f->mappings;
        f->mappings = m->next;
        if (pending_user_mapping == m) pending_user_mapping = NULL;
        mapping_close(m);
    }
    nv_release_mmap_lock(&nvrm_dev.nv);
    mtx_destroy(&f->lock);
    free(f);
}

uint32_t nvrm_rm_ioctl(void *file, uint32_t escape, void *args, uint32_t size) {
    struct nvrm_file *f = file;
    nvrm_drop_mapping();
    last_linear = 0;
    pending_user_mapping = NULL;
    if (escape == NV_ESC_RM_MAP_MEMORY && size == sizeof(nv_ioctl_nvos33_parameters_with_fd) &&
        __atomic_load_n(&nvrm_dev.mappings_revoked, __ATOMIC_ACQUIRE)) {
        /* RM revoked the GPU mappings (nv_revoke_gpu_mappings): no new ones until it allows them. */
        ((nv_ioctl_nvos33_parameters_with_fd *)args)->params.status = NV_ERR_INVALID_STATE;
        return NV_ERR_INVALID_STATE;
    }
    /* Only this request runs with the client's rights; the thread is nvrm's own again after. */
    bool admin = nvrm_thread_admin;
    uint32_t principal = nvrm_thread_process;
    struct nvrm_file *saved_file = request_file;
    request_file = f;
    nvrm_thread_admin = f->admin;
    nvrm_thread_process = f->principal;
    NV_STATUS status = rm_ioctl(nvrm_dev.sp, nv_get_ctl_state(), &f->nvfp, escape, args, size);
    nvrm_thread_admin = admin;
    nvrm_thread_process = principal;
    request_file = saved_file;
    if (status == NV_OK && escape == NV_ESC_RM_MAP_MEMORY && size == sizeof(nv_ioctl_nvos33_parameters_with_fd)) {
        last_linear = (uint64_t)((nv_ioctl_nvos33_parameters_with_fd *)args)->params.pLinearAddress;
    }
    return status;
}

/* RM posts an event for the client whose file the event belongs to (nv_event_t::nvfp). A post
 * that carries no data only says "this object fired": one is kept per object and index until
 * the client collects it. */
void NV_API_CALL nv_post_event(nv_event_t *event, NvHandle handle, NvU32 index, NvU32 info32, NvU16 info16,
                               NvBool data_valid) {
    struct nvrm_file *f = (struct nvrm_file *)event->nvfp;
    struct queued_event *e = calloc(1, sizeof *e);
    if (!e) {
        nvrm_log("NVRM: no memory for an RM event (object 0x%x, index %u)", (unsigned)handle, (unsigned)index);
        return;
    }
    e->object = handle;
    e->index = index;
    e->info32 = data_valid ? info32 : 0;
    e->info16 = data_valid ? info16 : 0;
    e->data_valid = data_valid;
    mtx_lock(&f->lock);
    bool keep = true;
    if (!data_valid) {
        for (struct queued_event *q = f->head; q; q = q->next)
            if (!q->data_valid && q->object == handle && q->index == index) { keep = false; break; }
    }
    if (keep) {
        if (f->tail) f->tail->next = e; else f->head = e;
        f->tail = e;
    }
    mtx_unlock(&f->lock);
    if (!keep) free(e);
}

/* The oldest queued event of the file; *more says whether others wait. -1 when none does. */
NvS32 NV_API_CALL nv_get_event(nv_file_private_t *nvfp, nv_event_t *event, NvU32 *more) {
    struct nvrm_file *f = (struct nvrm_file *)nvfp;
    mtx_lock(&f->lock);
    struct queued_event *e = f->head;
    if (e) {
        f->head = e->next;
        if (!f->head) f->tail = NULL;
    }
    if (more) *more = f->head ? 1 : 0;
    mtx_unlock(&f->lock);
    if (!e) return -1;
    event->hObject = e->object;
    event->index = e->index;
    event->info32 = e->info32;
    event->info16 = e->info16;
    free(e);
    return 0;
}

n_handle nvrm_rm_mapping_object(void *file, uint64_t *offset, uint64_t *bytes, uint32_t *access) {
    struct nvrm_file *f = file;
    struct nvrm_mapping m;
    if (!nvrm_take_mapping(&m)) return -1;
    if (m.prot != NV_PROTECT_READABLE && m.prot != NV_PROTECT_WRITEABLE && m.prot != NV_PROTECT_READ_WRITE)
        return -N_ERR_DENIED;
    uint32_t native_access = m.prot == NV_PROTECT_READABLE ? N_MEMORY_READ : N_MEMORY_READ_WRITE;
    /* Native memory has RO/RW, not write-only. Never silently add read access. */
    if (m.prot == NV_PROTECT_WRITEABLE) return -N_ERR_UNSUPPORTED;
    nv_acquire_mmap_lock(&nvrm_dev.nv);
    n_handle h = -N_ERR_INVALID;
    if (__atomic_load_n(&nvrm_dev.mappings_revoked, __ATOMIC_ACQUIRE)) goto done;
    struct exported_mapping *exported = m.owner;
    bool known = false;
    for (struct exported_mapping *k = f->mappings; k; k = k->next) known |= k == exported;
    if (!known || exported->file != f || exported->view >= 0) goto done;
    uint64_t in_page = last_linear & (PAGE - 1);
    uint64_t length;
    if (m.is_alloc) {
        uint64_t whole = nvrm_alloc_size(m.alloc);
        if (m.page_index > UINT64_MAX / PAGE || m.size > UINT64_MAX - in_page - (PAGE - 1)) goto done;
        uint64_t start = m.page_index * PAGE;
        length = (in_page + m.size + PAGE - 1) & ~(PAGE - 1);
        if (!m.size || start >= whole || length > whole - start) goto done;
        h = nvrm_alloc_view(m.alloc, m.page_index, length, m.caching, native_access);
    } else {
        /* A BAR mapping is one run of pages; the ranges are already page aligned. */
        uint64_t view_offset = 0, size = m.range.size;
        if (!size || size > UINT64_MAX - (PAGE - 1) - (PAGE - 1)) goto done;
        h = nvrm_bar_view(m.range.start, size, m.caching, native_access, &view_offset);
        /* The view is whole pages; the client maps all of it, as for system memory. */
        length = (view_offset + size + PAGE - 1) & ~(PAGE - 1);
        in_page += view_offset;
    }
    if (h < 0) {
        nvrm_log("NVRM: no shared object for the mapping: %s", n_result_text(h));
        goto done;
    }
    *offset = in_page;
    *bytes = length;
    *access = native_access;
    exported->view = h;
    all_mappings_revoked = false;
done:
    nv_release_mmap_lock(&nvrm_dev.nv);
    return h;
}
