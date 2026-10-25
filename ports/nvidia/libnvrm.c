/* libnvrm: an app's connection to nvrm (include/nvrm.h, include/nvrm_protocol.h). */
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <threads.h>
#include "nocturne.h"
#include "nvrm.h"
#include "nvrm_protocol.h"
#include "nvtypes.h"
#include "nvstatus.h"
#include "nvos.h"
#include "nv_escape.h"
#include "nv-unix-nvos-params-wrappers.h"

#define ARENA_BYTES (4u << 20) /* reserved: pages appear when a call first uses them */

/* A CPU mapping handed out by nvrm_map: RM's own address for it (what unmap names) and the
   shared mapping it lives in. */
struct mapping {
    struct mapping *next;
    uint8_t *address;      /* what the caller got */
    void *base;            /* the whole shared mapping */
    size_t bytes;
    NvP64 linear;          /* RM's address of the mapping */
};

struct nvrm {
    n_handle channel, arena_handle;
    uint8_t *arena;
    size_t used;           /* arena bytes in use by the locked caller */
    unsigned depth;        /* nvrm_lock nesting */
    mtx_t lock;
    struct mapping *mappings;
    struct mapping *uvm_mappings;  /* nvrm_uvm_map's (linear unused) */
    uint64_t hello_capabilities;
};

int64_t nvrm_connect_device_status(uint32_t pci_target, struct nvrm **out) {
    if (!out) return -N_ERR_INVALID;
    *out = NULL;
    if (pci_target > UINT16_MAX) return -N_ERR_INVALID;
    char channel[NVRM_CHANNEL_NAME_BYTES];
    snprintf(channel, sizeof channel, NVRM_CHANNEL_FORMAT, (unsigned)pci_target);
    struct nvrm *rm = calloc(1, sizeof *rm);
    if (!rm) return -N_ERR_NO_MEMORY;
    n_result result;
    bool lock_ready = false;
    rm->channel = n_channel_connect(channel);
    rm->arena_handle = -1;
    if (rm->channel < 0) { result = rm->channel; goto fail; }
    rm->arena_handle = n_shared_reserve(ARENA_BYTES);
    if (rm->arena_handle < 0) { result = rm->arena_handle; goto fail; }
    void *arena;
    result = n_shared_map(rm->arena_handle, ARENA_BYTES, N_MEMORY_READ_WRITE, &arena);
    rm->arena = arena;
    if (result < 0) goto fail;
    int initialized = mtx_init(&rm->lock, mtx_recursive);
    if (initialized != thrd_success) { result = initialized == thrd_nomem ? -N_ERR_NO_MEMORY : -N_ERR_IO; goto fail; }
    lock_ready = true;
    struct nvrm_message hello = {.protocol = NVRM_PROTOCOL, .escape = NVRM_HELLO,
                                 .arena_address = (uint64_t)(uintptr_t)rm->arena, .arena_bytes = ARENA_BYTES};
    struct nvrm_message reply;
    result = n_channel_send(rm->channel, &hello, sizeof hello, rm->arena_handle);
    if (result < 0) goto fail;
    if (result != sizeof hello) { result = -N_ERR_PROTOCOL; goto fail; }
    result = n_channel_receive(rm->channel, &reply, sizeof reply, N_CHANNEL_WAIT, NULL);
    if (result < 0) goto fail;
    if (result != sizeof reply || reply.protocol != NVRM_PROTOCOL || reply.escape != NVRM_HELLO ||
        reply.size || reply.status > 0) { result = -N_ERR_PROTOCOL; goto fail; }
    if (reply.status < 0) { result = reply.status; goto fail; }
    rm->hello_capabilities = reply.access;
    *out = rm;
    return 0;
fail:
    if (lock_ready) mtx_destroy(&rm->lock);
    if (rm->arena) n_memory_release(rm->arena, ARENA_BYTES);
    if (rm->arena_handle >= 0) n_handle_close(rm->arena_handle);
    if (rm->channel >= 0) n_handle_close(rm->channel);
    free(rm);
    return result;
}

struct nvrm *nvrm_connect_device(uint32_t pci_target) {
    struct nvrm *rm = NULL;
    nvrm_connect_device_status(pci_target, &rm);
    return rm;
}

bool nvrm_device_ready(const struct nvrm *rm) {
    const uint64_t ready = NVRM_HELLO_STATUS_PRESENT | NVRM_HELLO_ADAPTER_READY;
    return rm && (rm->hello_capabilities & ready) == ready;
}

/* The first NVIDIA GPU's connection, the first ready one's when ready_only: 0 with *out set, or why
   there is none (nvrm.h, nvrm_connect_ready_status). */
static int64_t connect_any(bool ready_only, struct nvrm **out) {
    *out = NULL;
    /* pcilist fills the supplied capacity. Grow until the complete snapshot fits,
       rather than truncating the set of GPUs to a driver-specific constant. */
    struct n_pciinfo *list = NULL;
    size_t capacity = 1;
    n_result count;
    for (;;) {
        if (capacity > INT_MAX || capacity > SIZE_MAX / sizeof *list) { free(list); return -N_ERR_NO_MEMORY; }
        struct n_pciinfo *grown = realloc(list, capacity * sizeof *list);
        if (!grown) { free(list); return -N_ERR_NO_MEMORY; }
        list = grown;
        count = pcilist(list, (int)capacity);
        if (count < 0) { free(list); return count; }
        if ((size_t)count < capacity) break;
        if (capacity > INT_MAX / 2) { free(list); return -N_ERR_NO_MEMORY; }
        capacity *= 2;
    }
    int64_t why = -N_ERR_NOT_FOUND;
    for (n_result i = 0; i < count && !*out; i++) {
        if (!nvrm_pci_is_gpu(list[i].vendor, list[i].cls, list[i].subcls, list[i].progif)) continue;
        struct nvrm *rm;
        int64_t result = nvrm_connect_device_status(N_PCI_TARGET(list[i].bus, list[i].dev, list[i].func), &rm);
        if (result >= 0 && ready_only && !nvrm_device_ready(rm)) {
            nvrm_disconnect(rm);
            result = -N_ERR_NOT_READY;
        }
        if (result >= 0) *out = rm;
        else if (why == -N_ERR_NOT_FOUND) why = result;
    }
    free(list);
    return *out ? 0 : why;
}

struct nvrm *nvrm_connect(void) {
    struct nvrm *rm;
    connect_any(false, &rm);
    return rm;
}

struct nvrm *nvrm_connect_ready(void) {
    struct nvrm *rm;
    connect_any(true, &rm);
    return rm;
}

int64_t nvrm_connect_ready_status(struct nvrm **out) {
    if (!out) return -N_ERR_INVALID;
    return connect_any(true, out);
}

static void mappings_release(struct mapping **list) {
    while (*list) {
        struct mapping *m = *list;
        *list = m->next;
        n_memory_release(m->base, m->bytes);
        free(m);
    }
}

void nvrm_disconnect(struct nvrm *rm) {
    if (!rm) return;
    mappings_release(&rm->mappings);
    mappings_release(&rm->uvm_mappings); /* nvrm takes UVM's side down with the connection */
    n_memory_release(rm->arena, ARENA_BYTES);
    n_handle_close(rm->arena_handle);
    n_handle_close(rm->channel); /* nvrm frees what this client still holds */
    mtx_destroy(&rm->lock);
    free(rm);
}

void nvrm_lock(struct nvrm *rm) {
    mtx_lock(&rm->lock);
    rm->depth++;
}

void nvrm_unlock(struct nvrm *rm) {
    if (!--rm->depth) rm->used = 0;
    mtx_unlock(&rm->lock);
}

void *nvrm_scratch(struct nvrm *rm, size_t bytes) {
    size_t at = (rm->used + 15) & ~(size_t)15;
    if (bytes > ARENA_BYTES || at > ARENA_BYTES - bytes) return NULL;
    rm->used = at + bytes;
    return rm->arena + at;
}

/* One request and its reply; the reply's arguments overwrite args. Called locked. */
static uint32_t request(struct nvrm *rm, uint32_t escape, void *args, uint32_t size, n_handle *attached,
                        struct nvrm_message *head, bool *returned) {
    if (returned) *returned = false;
    size_t bytes = sizeof(struct nvrm_message) + size;
    if (bytes < size) return NV_ERR_INVALID_ARGUMENT;
    struct nvrm_message *msg = malloc(bytes);
    if (!msg) return NV_ERR_NO_MEMORY;
    *msg = (struct nvrm_message){.protocol = NVRM_PROTOCOL, .escape = escape, .size = size};
    if (size) memcpy(msg + 1, args, size);
    if (attached) *attached = -1;
    uint32_t result = NV_ERR_OPERATING_SYSTEM;
    if (n_channel_send(rm->channel, msg, bytes, -1) < 0) goto done;
    n_result got = n_channel_receive(rm->channel, msg, bytes, N_CHANNEL_WAIT, attached);
    if (got < (n_result)sizeof *msg || msg->protocol != NVRM_PROTOCOL || msg->escape != escape) goto invalid;
    if (msg->rm_status > UINT32_MAX || msg->status > 0) goto invalid;
    if (msg->status < 0 && !msg->rm_status) goto invalid; /* native rejection, not RM output */
    if (msg->status < 0 && !msg->size && (size_t)got == sizeof *msg && size) {
        /* Older server: retain its exact error, but do not invent absent args. */
        result = (uint32_t)msg->rm_status;
        goto invalid;
    }
    if (msg->size != size || (size_t)got != bytes) goto invalid;
    if (size) memcpy(args, msg + 1, size);
    if (head) *head = *msg;
    if (returned) *returned = true;
    result = (uint32_t)msg->rm_status;
    goto done;
invalid:
    if (attached && *attached >= 0) { n_handle_close(*attached); *attached = -1; }
done:
    free(msg);
    return result;
}

/* params copied into the arena around a request whose argument struct points at them. */
static void *arena_copy(struct nvrm *rm, const void *params, uint32_t size) {
    if (!params || !size) return NULL;
    void *in = nvrm_scratch(rm, size);
    if (in) memcpy(in, params, size);
    return in;
}

uint32_t nvrm_alloc(struct nvrm *rm, uint32_t root, uint32_t parent, uint32_t *object, uint32_t cls,
                    void *params, uint32_t size) {
    nvrm_lock(rm);
    void *in = arena_copy(rm, params, size);
    NVOS21_PARAMETERS a = {.hRoot = root, .hObjectParent = parent, .hObjectNew = *object, .hClass = cls,
                           .pAllocParms = NV_PTR_TO_NvP64(in), .paramsSize = size};
    bool returned = false;
    uint32_t r = params && size && !in ? NV_ERR_INSUFFICIENT_RESOURCES : request(rm, NV_ESC_RM_ALLOC, &a, sizeof a, NULL, NULL, &returned);
    if (returned) {
        *object = a.hObjectNew;
        if (in) memcpy(params, in, size);
    }
    if (r == NV_OK) r = a.status;
    nvrm_unlock(rm);
    return r;
}

uint32_t nvrm_free(struct nvrm *rm, uint32_t root, uint32_t parent, uint32_t object) {
    NVOS00_PARAMETERS a = {.hRoot = root, .hObjectParent = parent, .hObjectOld = object};
    nvrm_lock(rm);
    uint32_t r = request(rm, NV_ESC_RM_FREE, &a, sizeof a, NULL, NULL, NULL);
    nvrm_unlock(rm);
    return r == NV_OK ? a.status : r;
}

uint32_t nvrm_control(struct nvrm *rm, uint32_t client, uint32_t object, uint32_t cmd, void *params,
                      uint32_t size) {
    nvrm_lock(rm);
    void *in = arena_copy(rm, params, size);
    NVOS54_PARAMETERS a = {.hClient = client, .hObject = object, .cmd = cmd, .params = NV_PTR_TO_NvP64(in),
                           .paramsSize = size};
    bool returned = false;
    uint32_t r = params && size && !in ? NV_ERR_INSUFFICIENT_RESOURCES : request(rm, NV_ESC_RM_CONTROL, &a, sizeof a, NULL, NULL, &returned);
    if (returned && in) memcpy(params, in, size);
    if (r == NV_OK) r = a.status;
    nvrm_unlock(rm);
    return r;
}

uint32_t nvrm_map(struct nvrm *rm, uint32_t client, uint32_t device, uint32_t memory, uint64_t offset,
                  uint64_t length, uint32_t flags, void **address) {
    struct mapping *m = calloc(1, sizeof *m);
    if (!m) return NV_ERR_NO_MEMORY;
    nv_ioctl_nvos33_parameters_with_fd a = {
        .params = {.hClient = client, .hDevice = device, .hMemory = memory, .offset = offset, .length = length,
                   .flags = flags},
        .fd = -1};
    struct nvrm_message head;
    n_handle shared = -1;
    nvrm_lock(rm);
    bool returned = false;
    uint32_t r = request(rm, NV_ESC_RM_MAP_MEMORY, &a, sizeof a, &shared, &head, &returned);
    if (r == NV_OK) r = a.params.status;
    if (r == NV_OK && (shared < 0 || !head.bytes || head.bytes > SIZE_MAX || head.offset >= head.bytes ||
                      length > head.bytes - head.offset ||
                      (head.access != N_MEMORY_READ && head.access != N_MEMORY_READ_WRITE)))
        r = NV_ERR_OPERATING_SYSTEM;
    if (r == NV_OK) {
        m->bytes = (size_t)head.bytes;
        if (n_shared_map(shared, m->bytes, head.access, &m->base) < 0) r = NV_ERR_NO_MEMORY;
    }
    if (shared >= 0) n_handle_close(shared); /* the mapping keeps the object */
    if (r == NV_OK) {
        m->address = (uint8_t *)m->base + head.offset;
        m->linear = a.params.pLinearAddress;
        m->next = rm->mappings;
        rm->mappings = m;
        *address = m->address;
    } else if (returned && a.params.status == NV_OK && a.params.pLinearAddress) {
        /* RM mapped it but this app cannot: give RM's mapping back */
        NVOS34_PARAMETERS u = {.hClient = client, .hDevice = device, .hMemory = memory,
                               .pLinearAddress = a.params.pLinearAddress, .flags = flags};
        request(rm, NV_ESC_RM_UNMAP_MEMORY, &u, sizeof u, NULL, NULL, NULL);
    }
    nvrm_unlock(rm);
    if (r != NV_OK) free(m);
    return r;
}

uint32_t nvrm_unmap(struct nvrm *rm, uint32_t client, uint32_t device, uint32_t memory, void *address,
                    uint32_t flags) {
    nvrm_lock(rm);
    struct mapping **link = &rm->mappings;
    while (*link && (*link)->address != address) link = &(*link)->next;
    struct mapping *m = *link;
    uint32_t r = NV_ERR_INVALID_ADDRESS;
    if (m) {
        *link = m->next;
        n_memory_release(m->base, m->bytes);
        NVOS34_PARAMETERS u = {.hClient = client, .hDevice = device, .hMemory = memory,
                               .pLinearAddress = m->linear, .flags = flags};
        r = request(rm, NV_ESC_RM_UNMAP_MEMORY, &u, sizeof u, NULL, NULL, NULL);
        if (r == NV_OK) r = u.status;
        free(m);
    }
    nvrm_unlock(rm);
    return r;
}

uint32_t nvrm_escape(struct nvrm *rm, uint32_t escape, void *args, uint32_t size) {
    nvrm_lock(rm);
    uint32_t r = request(rm, escape, args, size, NULL, NULL, NULL);
    nvrm_unlock(rm);
    return r;
}

/* A message already queued from nvrm (a reply some thread is about to take) counts as alive; only
   a gone peer with nothing queued reports N_ERR_CLOSED. Takes no lock and takes no message. */
bool nvrm_alive(struct nvrm *rm) {
    n_result r = n_channel_receive(rm->channel, NULL, 0, N_CHANNEL_PEEK, NULL);
    return r != -N_ERR_CLOSED && r != -N_ERR_NO_HANDLE;
}

/* ---- UVM (NVRM_UVM) ---- */

/* One NVRM_UVM request; UVM's status, or the transport's failure. Called locked. */
static uint32_t uvm_request(struct nvrm *rm, struct nvrm_uvm_request *u, n_handle *attached) {
    uint32_t r = request(rm, NVRM_UVM, u, sizeof *u, attached, NULL, NULL);
    return r == NV_OK ? u->status : r;
}

uint32_t nvrm_uvm_open(struct nvrm *rm, uint32_t minor, int32_t *file) {
    struct nvrm_uvm_request u = {.op = NVRM_UVM_OPEN, .file = -1, .minor = minor};
    nvrm_lock(rm);
    uint32_t r = uvm_request(rm, &u, NULL);
    nvrm_unlock(rm);
    if (r == NV_OK) *file = u.file;
    return r;
}

uint32_t nvrm_uvm_close(struct nvrm *rm, int32_t file) {
    struct nvrm_uvm_request u = {.op = NVRM_UVM_CLOSE, .file = file};
    nvrm_lock(rm);
    uint32_t r = uvm_request(rm, &u, NULL);
    nvrm_unlock(rm);
    return r;
}

uint32_t nvrm_uvm_ioctl(struct nvrm *rm, int32_t file, uint32_t cmd, void *params, uint32_t size) {
    nvrm_lock(rm);
    void *in = arena_copy(rm, params, size);
    struct nvrm_uvm_request u = {.op = NVRM_UVM_IOCTL, .file = file, .cmd = cmd,
                                 .arg = (uint64_t)(uintptr_t)in};
    uint32_t r = params && size && !in ? NV_ERR_INSUFFICIENT_RESOURCES : uvm_request(rm, &u, NULL);
    /* UVM writes its results (and its status field) into the block whatever the outcome. */
    if (in && r != NV_ERR_OPERATING_SYSTEM) memcpy(params, in, size);
    nvrm_unlock(rm);
    return r;
}

uint32_t nvrm_uvm_map(struct nvrm *rm, int32_t file, uint64_t bytes, void **address) {
    if (!bytes || bytes > SIZE_MAX || bytes % N_PAGE_SIZE) return NV_ERR_INVALID_ARGUMENT;
    struct mapping *m = calloc(1, sizeof *m);
    if (!m) return NV_ERR_NO_MEMORY;
    struct nvrm_uvm_request u = {.op = NVRM_UVM_MAP, .file = file, .bytes = bytes};
    n_handle memory = -1;
    nvrm_lock(rm);
    uint32_t r = uvm_request(rm, &u, &memory);
    if (r == NV_OK && memory < 0) r = NV_ERR_OPERATING_SYSTEM;
    if (r == NV_OK) {
        uint64_t token = u.token;
        m->bytes = (size_t)bytes;
        if (n_shared_map(memory, m->bytes, N_MEMORY_READ_WRITE, &m->base) < 0) {
            /* give the uncommitted map back */
            u = (struct nvrm_uvm_request){.op = NVRM_UVM_UNMAP, .file = file, .token = token};
            uvm_request(rm, &u, NULL);
            r = NV_ERR_NO_MEMORY;
        } else {
            u = (struct nvrm_uvm_request){.op = NVRM_UVM_MAP_COMMIT, .file = file, .token = token,
                                          .address = (uint64_t)(uintptr_t)m->base};
            r = uvm_request(rm, &u, NULL);
            if (r != NV_OK) n_memory_release(m->base, m->bytes); /* nvrm took it back already */
        }
    }
    if (memory >= 0) n_handle_close(memory); /* the mapping keeps the memory */
    if (r == NV_OK) {
        m->address = m->base;
        m->next = rm->uvm_mappings;
        rm->uvm_mappings = m;
        *address = m->address;
    }
    nvrm_unlock(rm);
    if (r != NV_OK) free(m);
    return r;
}

uint32_t nvrm_uvm_unmap(struct nvrm *rm, void *address) {
    nvrm_lock(rm);
    struct mapping **link = &rm->uvm_mappings;
    while (*link && (*link)->address != address) link = &(*link)->next;
    struct mapping *m = *link;
    uint32_t r = NV_ERR_INVALID_ADDRESS;
    if (m) {
        *link = m->next;
        /* UVM lets go of it first, so nothing it frees stays reachable here meanwhile */
        struct nvrm_uvm_request u = {.op = NVRM_UVM_UNMAP, .address = (uint64_t)(uintptr_t)m->base};
        r = uvm_request(rm, &u, NULL);
        n_memory_release(m->base, m->bytes);
        free(m);
    }
    nvrm_unlock(rm);
    return r;
}

uint32_t nvrm_uvm_poll(struct nvrm *rm, int32_t file, int64_t timeout_ms, uint32_t *events) {
    uint64_t deadline = timeout_ms < 0 ? UINT64_MAX : n_clock_uptime_ms() + (uint64_t)timeout_ms;
    for (;;) {
        uint64_t now = n_clock_uptime_ms();
        int64_t left = timeout_ms < 0 ? -1 : now >= deadline ? 0 : (int64_t)(deadline - now);
        struct nvrm_uvm_request u = {.op = NVRM_UVM_POLL, .file = file, .timeout_ms = left};
        /* one request at a time, so other threads' calls go between the waits */
        nvrm_lock(rm);
        uint32_t r = uvm_request(rm, &u, NULL);
        nvrm_unlock(rm);
        if (r != NV_OK || u.events || left == 0) {
            *events = r == NV_OK ? u.events : 0;
            return r;
        }
    }
}

/* ---- Direct scanout (NVRM_SCANOUT) ---- */

/* One NVRM_SCANOUT request; nvrm's status, or the transport's failure. */
static uint32_t scanout_request(struct nvrm *rm, struct nvrm_scanout_request *s) {
    nvrm_lock(rm);
    uint32_t r = request(rm, NVRM_SCANOUT, s, sizeof *s, NULL, NULL, NULL);
    nvrm_unlock(rm);
    return r == NV_OK ? s->status : r;
}

uint32_t nvrm_scanout_create(struct nvrm *rm, uint32_t *image, uint32_t *width, uint32_t *height,
                             uint32_t *pitch, uint64_t *bytes) {
    struct nvrm_scanout_request s = {.op = NVRM_SCANOUT_CREATE};
    uint32_t r = scanout_request(rm, &s);
    if (r == NV_OK) {
        *image = s.image;
        *width = s.width;
        *height = s.height;
        *pitch = s.pitch;
        *bytes = s.bytes;
    }
    return r;
}

uint32_t nvrm_scanout_destroy(struct nvrm *rm, uint32_t image) {
    struct nvrm_scanout_request s = {.op = NVRM_SCANOUT_DESTROY, .image = image};
    return scanout_request(rm, &s);
}

uint32_t nvrm_scanout_present(struct nvrm *rm, uint32_t image) {
    struct nvrm_scanout_request s = {.op = NVRM_SCANOUT_PRESENT, .image = image};
    return scanout_request(rm, &s);
}

uint32_t nvrm_scanout_import(struct nvrm *rm, uint32_t image, uint32_t client, uint32_t parent, uint32_t *object) {
    struct nvrm_scanout_request s = {.op = NVRM_SCANOUT_IMPORT, .image = image, .client = client, .parent = parent,
                                     .object = *object};
    uint32_t r = scanout_request(rm, &s);
    if (r == NV_OK) *object = s.object;
    return r;
}
