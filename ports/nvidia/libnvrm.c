/* libnvrm: an app's connection to /bin/nvrm (include/nvrm.h, include/nvrm_protocol.h). */
#include <stdlib.h>
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
};

struct nvrm *nvrm_connect(void) {
    struct nvrm *rm = calloc(1, sizeof *rm);
    if (!rm) return NULL;
    rm->channel = n_channel_connect(NVRM_CHANNEL);
    rm->arena_handle = rm->channel >= 0 ? n_shared_reserve(ARENA_BYTES) : -1;
    rm->arena = rm->arena_handle >= 0 ? n_shared_map(rm->arena_handle, ARENA_BYTES, N_MEMORY_READ_WRITE) : NULL;
    if (!rm->arena || mtx_init(&rm->lock, mtx_recursive) != thrd_success) goto fail;
    struct nvrm_message hello = {.protocol = NVRM_PROTOCOL, .escape = NVRM_HELLO,
                                 .arena_address = (uint64_t)(uintptr_t)rm->arena, .arena_bytes = ARENA_BYTES};
    struct nvrm_message reply;
    if (n_channel_send(rm->channel, &hello, sizeof hello, rm->arena_handle) < 0 ||
        n_channel_receive(rm->channel, &reply, sizeof reply, N_CHANNEL_WAIT, NULL) != (n_result)sizeof reply ||
        reply.status < 0) {
        mtx_destroy(&rm->lock);
        goto fail;
    }
    return rm;
fail:
    if (rm->arena) n_memory_release(rm->arena, ARENA_BYTES);
    if (rm->arena_handle >= 0) n_handle_close(rm->arena_handle);
    if (rm->channel >= 0) n_handle_close(rm->channel);
    free(rm);
    return NULL;
}

void nvrm_disconnect(struct nvrm *rm) {
    if (!rm) return;
    while (rm->mappings) {
        struct mapping *m = rm->mappings;
        rm->mappings = m->next;
        n_memory_release(m->base, m->bytes);
        free(m);
    }
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
                        struct nvrm_message *head) {
    struct {
        struct nvrm_message head;
        uint8_t args[NVRM_ARGS_MAX];
    } msg;
    if (size > NVRM_ARGS_MAX) return NV_ERR_INVALID_ARGUMENT;
    msg.head = (struct nvrm_message){.protocol = NVRM_PROTOCOL, .escape = escape, .size = size};
    memcpy(msg.args, args, size);
    if (attached) *attached = -1;
    if (n_channel_send(rm->channel, &msg, sizeof msg.head + size, -1) < 0) return NV_ERR_OPERATING_SYSTEM;
    n_result got = n_channel_receive(rm->channel, &msg, sizeof msg, N_CHANNEL_WAIT, attached);
    if (got < (n_result)sizeof msg.head || msg.head.status < 0 || msg.head.size != size ||
        (size_t)got != sizeof msg.head + size) {
        if (attached && *attached >= 0) n_handle_close(*attached);
        return NV_ERR_OPERATING_SYSTEM;
    }
    memcpy(args, msg.args, size);
    if (head) *head = msg.head;
    return NV_OK;
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
    uint32_t r = params && size && !in ? NV_ERR_INSUFFICIENT_RESOURCES : request(rm, NV_ESC_RM_ALLOC, &a, sizeof a, NULL, NULL);
    if (r == NV_OK) {
        r = a.status;
        *object = a.hObjectNew;
        if (in) memcpy(params, in, size);
    }
    nvrm_unlock(rm);
    return r;
}

uint32_t nvrm_free(struct nvrm *rm, uint32_t root, uint32_t parent, uint32_t object) {
    NVOS00_PARAMETERS a = {.hRoot = root, .hObjectParent = parent, .hObjectOld = object};
    nvrm_lock(rm);
    uint32_t r = request(rm, NV_ESC_RM_FREE, &a, sizeof a, NULL, NULL);
    nvrm_unlock(rm);
    return r == NV_OK ? a.status : r;
}

uint32_t nvrm_control(struct nvrm *rm, uint32_t client, uint32_t object, uint32_t cmd, void *params,
                      uint32_t size) {
    nvrm_lock(rm);
    void *in = arena_copy(rm, params, size);
    NVOS54_PARAMETERS a = {.hClient = client, .hObject = object, .cmd = cmd, .params = NV_PTR_TO_NvP64(in),
                           .paramsSize = size};
    uint32_t r = params && size && !in ? NV_ERR_INSUFFICIENT_RESOURCES : request(rm, NV_ESC_RM_CONTROL, &a, sizeof a, NULL, NULL);
    if (r == NV_OK) {
        r = a.status;
        if (in) memcpy(params, in, size);
    }
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
    uint32_t r = request(rm, NV_ESC_RM_MAP_MEMORY, &a, sizeof a, &shared, &head);
    if (r == NV_OK) r = a.params.status;
    if (r == NV_OK && (shared < 0 || head.offset > head.bytes)) r = NV_ERR_OPERATING_SYSTEM;
    if (r == NV_OK) {
        m->bytes = (size_t)head.bytes;
        m->base = n_shared_map(shared, m->bytes, N_MEMORY_READ_WRITE);
        if (!m->base) r = NV_ERR_NO_MEMORY;
    }
    if (shared >= 0) n_handle_close(shared); /* the mapping keeps the object */
    if (r == NV_OK) {
        m->address = (uint8_t *)m->base + head.offset;
        m->linear = a.params.pLinearAddress;
        m->next = rm->mappings;
        rm->mappings = m;
        *address = m->address;
    } else if (a.params.status == NV_OK && a.params.pLinearAddress) {
        /* RM mapped it but this app cannot: give RM's mapping back */
        NVOS34_PARAMETERS u = {.hClient = client, .hDevice = device, .hMemory = memory,
                               .pLinearAddress = a.params.pLinearAddress, .flags = flags};
        request(rm, NV_ESC_RM_UNMAP_MEMORY, &u, sizeof u, NULL, NULL);
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
        r = request(rm, NV_ESC_RM_UNMAP_MEMORY, &u, sizeof u, NULL, NULL);
        if (r == NV_OK) r = u.status;
        free(m);
    }
    nvrm_unlock(rm);
    return r;
}

uint32_t nvrm_escape(struct nvrm *rm, uint32_t escape, void *args, uint32_t size) {
    nvrm_lock(rm);
    uint32_t r = request(rm, escape, args, size, NULL, NULL);
    nvrm_unlock(rm);
    return r;
}
