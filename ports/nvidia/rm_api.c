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

#define PAGE 4096ull

/* pLinearAddress of the last NV_ESC_RM_MAP_MEMORY this thread ran: its low 12 bits are the
   position of the mapping inside its first page (RM does not keep that in the parked context). */
static thread_local uint64_t last_linear;

/* Events RM posts for a client wait here until the client asks for them with
 * NV_ESC_RM_GET_EVENT_DATA (rm_ioctl calls nv_get_event). The protocol is request and reply, so
 * a client polls; nothing is pushed to its channel. */
#define EVENT_QUEUE_MAX 4096u

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
    bool admin;                 /* RM treats the client as an administrator */
    mtx_t lock;                 /* the queue: RM posts from its interrupt and work threads */
    struct queued_event *head, *tail;
    uint32_t queued;
    bool overflow_logged;
};

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
    return f;
}

void nvrm_rm_file_close(void *file) {
    struct nvrm_file *f = file;
    if (!f) return;
    rm_cleanup_file_private(nvrm_dev.sp, nv_get_ctl_state(), &f->nvfp);
    /* RM stops posting to the file when its events are freed; wait for posts in flight. */
    while (nv_event_posting_refcount_read(&f->nvfp) > 0) thrd_yield();
    while (f->head) {
        struct queued_event *e = f->head;
        f->head = e->next;
        free(e);
    }
    mtx_destroy(&f->lock);
    free(f);
}

uint32_t nvrm_rm_ioctl(void *file, uint32_t escape, void *args, uint32_t size) {
    struct nvrm_file *f = file;
    nvrm_drop_mapping();
    last_linear = 0;
    if (escape == NV_ESC_RM_MAP_MEMORY && size == sizeof(nv_ioctl_nvos33_parameters_with_fd) &&
        __atomic_load_n(&nvrm_dev.mappings_revoked, __ATOMIC_ACQUIRE)) {
        /* RM revoked the GPU mappings (nv_revoke_gpu_mappings): no new ones until it allows them. */
        ((nv_ioctl_nvos33_parameters_with_fd *)args)->params.status = NV_ERR_INVALID_STATE;
        return NV_ERR_INVALID_STATE;
    }
    /* Only this request runs with the client's rights; the thread is nvrm's own again after. */
    nvrm_thread_admin = f->admin;
    NV_STATUS status = rm_ioctl(nvrm_dev.sp, nv_get_ctl_state(), &f->nvfp, escape, args, size);
    nvrm_thread_admin = true;
    if (escape == NV_ESC_RM_MAP_MEMORY && size == sizeof(nv_ioctl_nvos33_parameters_with_fd))
        last_linear = (uint64_t)((nv_ioctl_nvos33_parameters_with_fd *)args)->params.pLinearAddress;
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
    if (f->queued >= EVENT_QUEUE_MAX) {
        keep = false;
        if (!f->overflow_logged) {
            f->overflow_logged = true;
            nvrm_log("NVRM: a client does not collect its RM events; later ones are dropped");
        }
    } else if (!data_valid) {
        for (struct queued_event *q = f->head; q; q = q->next)
            if (!q->data_valid && q->object == handle && q->index == index) { keep = false; break; }
    }
    if (keep) {
        if (f->tail) f->tail->next = e; else f->head = e;
        f->tail = e;
        f->queued++;
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
        f->queued--;
        f->overflow_logged = false;
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

n_handle nvrm_rm_mapping_object(uint64_t *offset, uint64_t *bytes) {
    struct nvrm_mapping m;
    if (!nvrm_take_mapping(&m)) return -1;
    uint64_t in_page = last_linear & (PAGE - 1);
    n_handle h;
    uint64_t length;
    if (m.is_alloc) {
        uint64_t whole = nvrm_alloc_size(m.alloc);
        uint64_t start = m.page_index * PAGE;
        length = (in_page + m.size + PAGE - 1) & ~(PAGE - 1);
        if (start >= whole) return -1;
        if (length > whole - start) length = whole - start;
        h = nvrm_alloc_view(m.alloc, m.page_index, length, m.caching);
    } else {
        /* A BAR mapping is one run of pages; the ranges are already page aligned. */
        if (m.range_count != 1) {
            nvrm_log("NVRM: a mapping of %u separate ranges is not supported", (unsigned)m.range_count);
            return -1;
        }
        uint64_t view_offset = 0, size = m.ranges[0].size;
        h = nvrm_bar_view(m.ranges[0].start, size, m.caching, &view_offset);
        /* The view is whole pages; the client maps all of it, as for system memory. */
        length = (view_offset + size + PAGE - 1) & ~(PAGE - 1);
        in_page += view_offset;
    }
    if (h < 0) {
        nvrm_log("NVRM: no shared object for the mapping: %s", n_result_text(h));
        return -1;
    }
    *offset = in_page;
    *bytes = length;
    return h;
}
