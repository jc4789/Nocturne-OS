/* In-process round trips through the actual server and client framing code.
   Native channel delivery and RM work are fixtures, not real PCI/GPU work. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define request server_request
#include "../ports/nvidia/nvrm_serve.c"
#undef request
#define request client_request
#include "../ports/nvidia/libnvrm.c"
#undef request

static int failures, map_access, map_calls, rm_calls, unmap_calls;
static uint32_t rm_result, individual_status;
static bool fail_export, corrupt_protocol;
static n_handle response_attachment;
static struct nvrm_message *incoming, *outgoing;
static size_t incoming_bytes, outgoing_bytes;
static struct client server;
static uint8_t shared_page[N_PAGE_SIZE];
static char connected_name[NVRM_CHANNEL_NAME_BYTES];
static char listened_name[NVRM_CHANNEL_NAME_BYTES];
static int listener_calls, watches, listener_closes;
static bool legacy_busy;
FILE *stdout; /* The native FILE global is not MinGW's __acrt_iob_func(). */
int fflush(FILE *stream) { (void)stream; return 0; }

/* The original statically linked clients have this exact wire layout. */
struct legacy_header {
    uint32_t protocol, escape, size;
    int32_t status;
    uint64_t arena_address, arena_bytes, offset, bytes;
};
_Static_assert(sizeof(struct legacy_header) == sizeof(struct nvrm_message), "Protocol1 binary size");
_Static_assert(offsetof(struct legacy_header, offset) == offsetof(struct nvrm_message, offset), "Protocol1 map offset");

#define CHECK(expr) do { if (!(expr)) { \
    printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; \
} } while (0)

int mtx_init(mtx_t *m, int type) { (void)m; (void)type; return thrd_success; }
int mtx_lock(mtx_t *m) { (void)m; return thrd_success; }
int mtx_unlock(mtx_t *m) { (void)m; return thrd_success; }
void mtx_destroy(mtx_t *m) { (void)m; }
n_result n_handle_close(n_handle h) { if (h == 10 || h == 20 || h == 30) listener_closes++; return 0; }
n_result n_events_unwatch(n_handle q, uint64_t token) { (void)q; (void)token; return 0; }
int n_memory_release(void *p, size_t bytes) { (void)p; (void)bytes; return 0; }
n_handle n_channel_connect(const char *name) { snprintf(connected_name, sizeof connected_name, "%s", name); return 12; }
n_handle n_channel_listen(const char *name) {
    listener_calls++;
    if (!strcmp(name, NVRM_CHANNEL)) return legacy_busy ? -N_ERR_EXISTS : 30;
    snprintf(listened_name, sizeof listened_name, "%s", name);
    return 10;
}
n_handle n_channel_accept(n_handle listener) { (void)listener; return -N_ERR_NOT_READY; }
n_handle n_events_create(void) { return 20; }
n_result n_events_watch(n_handle q, n_handle h, uint32_t interest, uint64_t token) {
    (void)q; (void)interest;
    CHECK((h == 10 && token == LISTENER_TOKEN) || (h == 30 && token == LEGACY_LISTENER_TOKEN));
    watches++;
    return 0;
}
n_result n_events_wait(n_handle q, struct n_event *out, size_t max, int64_t timeout_ms) {
    (void)q; (void)out; (void)max; (void)timeout_ms;
    return -N_ERR_CANCELLED;
}
n_handle n_shared_reserve(size_t bytes) { (void)bytes; return 43; }
void *n_shared_map(n_handle h, size_t bytes, int access) {
    (void)bytes;
    if (h == 42) {
        map_access = access; map_calls++;
        if (access != N_MEMORY_READ) return NULL; /* native maximum RO fixture */
    }
    return shared_page;
}
int pcilist(struct n_pciinfo *list, int capacity) {
    /* The ready GPU follows several non-display entries, so truncation misses it. */
    struct n_pciinfo entries[] = {
        {.vendor = 0x1234}, {.vendor = 0x10de, .cls = 0x04},
        {.bus = 3, .dev = 4, .func = 1, .vendor = 0x10de, .cls = 0x03},
    };
    int count = sizeof entries / sizeof entries[0];
    if (count > capacity) count = capacity;
    memcpy(list, entries, count * sizeof *list);
    return count;
}
void *nvrm_rm_file_open(bool admin) { CHECK(!admin); return &rm_calls; }
void nvrm_rm_file_close(void *file) { (void)file; }
uint32_t nvrm_rm_ioctl(void *file, uint32_t escape, void *args, uint32_t size) {
    (void)file;
    rm_calls++;
    if (escape == NV_ESC_RM_MAP_MEMORY && size == sizeof(nv_ioctl_nvos33_parameters_with_fd)) {
        nv_ioctl_nvos33_parameters_with_fd *m = args;
        m->params.status = individual_status;
        m->params.pLinearAddress = NV_PTR_TO_NvP64((void *)(uintptr_t)0x1000);
    }
    if (escape == NV_ESC_RM_UNMAP_MEMORY) {
        unmap_calls++;
        ((NVOS34_PARAMETERS *)args)->status = individual_status;
    }
    return rm_result;
}
n_handle nvrm_rm_mapping_object(void *file, uint64_t *offset, uint64_t *bytes, uint32_t *access) {
    (void)file;
    if (fail_export) return -N_ERR_NO_MEMORY;
    *offset = 0; *bytes = N_PAGE_SIZE; *access = N_MEMORY_READ;
    return 42;
}

n_result n_channel_send(n_handle channel, const void *data, size_t bytes, n_handle attached) {
    struct nvrm_message **destination = channel == 12 ? &incoming : &outgoing;
    size_t *length = channel == 12 ? &incoming_bytes : &outgoing_bytes;
    CHECK(channel == 11 || channel == 12);
    free(*destination);
    *destination = malloc(bytes);
    CHECK(*destination);
    memcpy(*destination, data, bytes);
    *length = bytes;
    if (channel == 11) {
        response_attachment = attached;
        if (corrupt_protocol) (*destination)->protocol++;
    }
    return bytes;
}
n_result n_channel_receive(n_handle channel, void *data, size_t capacity, int flags, n_handle *attached) {
    if (attached) *attached = -1;
    if (channel == 11) {
        if (flags & N_CHANNEL_PEEK) return incoming_bytes;
        CHECK(incoming && capacity >= incoming_bytes);
        memcpy(data, incoming, incoming_bytes);
        if (attached && incoming->escape == NVRM_HELLO) *attached = 43;
        size_t size = incoming_bytes;
        free(incoming); incoming = NULL; incoming_bytes = 0;
        return size;
    }
    CHECK(channel == 12 && !(flags & N_CHANNEL_PEEK));
    server_request(-1, &server);
    CHECK(outgoing && capacity >= outgoing_bytes);
    memcpy(data, outgoing, outgoing_bytes);
    if (attached) *attached = response_attachment;
    size_t size = outgoing_bytes;
    free(outgoing); outgoing = NULL; outgoing_bytes = 0;
    return size;
}

int main(void) {
    server.channel = 11;
    server.arena_handle = -1;
    server.file = &rm_calls;
    struct nvrm rm = {.channel = 12};
    uint32_t untouched = 0;
    rm_result = NV_ERR_INVALID_ARGUMENT;
    CHECK(nvrm_escape(&rm, 0, &untouched, sizeof untouched) == NV_ERR_INVALID_ARGUMENT && untouched == 0);
    rm_result = NV_ERR_NOT_SUPPORTED;
    CHECK(nvrm_escape(&rm, 0x777, &untouched, 0) == NV_ERR_NOT_SUPPORTED);
    /* No driver-specific 1024-byte argument truncation. */
    uint8_t args[2 * N_PAGE_SIZE] = {0};
    rm_result = NV_OK;
    CHECK(nvrm_escape(&rm, 0, args, sizeof args) == NV_OK);
    corrupt_protocol = true;
    CHECK(nvrm_escape(&rm, 0, &untouched, sizeof untouched) == NV_ERR_OPERATING_SYSTEM);
    corrupt_protocol = false;
    void *address = NULL;
    CHECK(nvrm_map(&rm, 1, 2, 3, 0, N_PAGE_SIZE, 0, &address) == NV_OK);
    CHECK(address == shared_page && map_access == N_MEMORY_READ && map_calls == 1);
    CHECK(nvrm_unmap(&rm, 1, 2, 3, address, 0) == NV_OK && !rm.mappings);
    fail_export = true;
    int before = unmap_calls;
    CHECK(nvrm_map(&rm, 1, 2, 3, 0, N_PAGE_SIZE, 0, &address) == NV_ERR_OPERATING_SYSTEM);
    CHECK(unmap_calls == before + 1 && map_calls == 1);
    fail_export = false;
    individual_status = NV_ERR_PROTECTION_FAULT;
    CHECK(nvrm_map(&rm, 1, 2, 3, 0, N_PAGE_SIZE, 0, &address) == NV_ERR_PROTECTION_FAULT);
    CHECK(map_calls == 1);
    individual_status = NV_OK;

    /* Public default API still finds a single ready GPU; device opens use its
       BDF, not the first daemon that wins a shared service-name race. */
    server.file = NULL;
    struct nvrm *connection = nvrm_connect();
    CHECK(connection && !strcmp(connected_name, "nocturne.nvidia.rm.0321"));
    nvrm_disconnect(connection);
    server.file = NULL; server.arena = NULL; server.arena_handle = -1;
    connection = nvrm_connect_device(N_PCI_TARGET(7, 0, 0));
    CHECK(connection && !strcmp(connected_name, "nocturne.nvidia.rm.0700"));
    nvrm_disconnect(connection);
    CHECK(!nvrm_connect_device((uint32_t)UINT16_MAX + 1));

    /* A legacy reader ignores the newly-used union slots. It must still fail
       on dispatcher failure instead of reading zeroed argument.status as OK. */
    server.file = &rm_calls;
    struct legacy_header old = {.protocol = 1, .escape = 0x777};
    rm_result = NV_ERR_NOT_SUPPORTED;
    n_channel_send(12, &old, sizeof old, -1);
    CHECK(n_channel_receive(12, &old, sizeof old, N_CHANNEL_WAIT, NULL) == sizeof old);
    CHECK(old.protocol == 1 && old.status < 0 && old.size == 0);
    rm_result = NV_OK;
    /* Old binaries always requested RW mapping. The new server's immutable RO
       view must reject that, not relax protection for compatibility. */
    CHECK(!n_shared_map(42, N_PAGE_SIZE, N_MEMORY_READ_WRITE));

    legacy_busy = true;
    CHECK(nvrm_serve(N_PCI_TARGET(2, 1, 0)) == -N_ERR_CANCELLED);
    CHECK(listener_calls == 2 && watches == 1 && listener_closes == 2);
    CHECK(!strcmp(listened_name, "nocturne.nvidia.rm.0208"));
    legacy_busy = false;
    listener_calls = watches = listener_closes = 0;
    CHECK(nvrm_serve(N_PCI_TARGET(3, 1, 0)) == -N_ERR_CANCELLED);
    CHECK(listener_calls == 2 && watches == 2 && listener_closes == 3);
    printf("nvrm_transport_hosttest: %d failed (channel/RM fixtures; no GPU execution)\n", failures);
    return failures != 0;
}
