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
static bool partial_alloc, partial_control;
static n_result connect_result = 12, reserve_result = 43;
static bool fail_arena_map;
static uint64_t sender_app = 10;
static uint64_t queued_sender;
static bool fixture_adapter_ready = true, legacy_hello;
static bool multi_gpu_ready, override_hello;
static uint64_t hello_flags;
static size_t last_reply_size;
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
/* The server here accepts no client (n_channel_accept is never ready), so no client thread runs;
   requests are driven directly on this thread through server_request. */
static int threads_started, stop_wakes;
int cnd_init(cnd_t *c) { (void)c; return thrd_success; }
int cnd_signal(cnd_t *c) { (void)c; return thrd_success; }
int cnd_wait(cnd_t *c, mtx_t *m) { (void)c; (void)m; CHECK(!"no client thread to wait for"); return thrd_error; }
void cnd_destroy(cnd_t *c) { (void)c; }
int thrd_create(thrd_t *t, thrd_start_t f, void *arg) { (void)t; (void)f; (void)arg; threads_started++; return thrd_error; }
int thrd_detach(thrd_t t) { (void)t; return thrd_success; }
n_result wake_address(volatile uint32_t *address, unsigned count) {
    CHECK(*address == 1 && count == 0);
    stop_wakes++;
    return 0;
}
n_result n_events_watch_address(n_handle q, volatile uint32_t *address, uint32_t expected, uint64_t token) {
    (void)q; (void)address; (void)expected; (void)token;
    return -N_ERR_NOT_READY;
}
const char *n_result_text(n_result r) { (void)r; return "fixture"; }
n_result n_handle_close(n_handle h) {
    if (h == 10 || h == 20 || h == 30) listener_closes++;
    if (h == 12) { server.file = NULL; server.arena = NULL; server.arena_handle = -1; }
    return 0;
}
n_result n_events_unwatch(n_handle q, uint64_t token) { (void)q; (void)token; return 0; }
n_result n_memory_release(void *p, size_t bytes) { (void)p; (void)bytes; return 0; }
n_handle n_channel_connect(const char *name) { snprintf(connected_name, sizeof connected_name, "%s", name); return connect_result; }
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
n_handle n_shared_reserve(size_t bytes) { (void)bytes; return reserve_result; }
n_result n_shared_map(n_handle h, size_t bytes, int access, void **mapping) {
    (void)bytes;
    *mapping = NULL;
    if (h == 43 && fail_arena_map) return -N_ERR_NO_MEMORY;
    if (h == 42) {
        map_access = access; map_calls++;
        if (access != N_MEMORY_READ) return -N_ERR_DENIED; /* native maximum RO fixture */
    }
    *mapping = shared_page;
    return 0;
}
n_result pcilist(struct n_pciinfo *list, int capacity) {
    /* The ready GPU follows several non-display entries, so truncation misses it. */
    struct n_pciinfo entries[] = {
        {.vendor = 0x1234}, {.vendor = 0x10de, .cls = 0x04},
        {.vendor = 0x10de, .cls = 0x03, .subcls = 0x01},
        {.vendor = 0x10de, .cls = 0x03, .subcls = 0x00, .progif = 1},
        {.bus = 3, .dev = 4, .func = 1, .vendor = 0x10de, .cls = 0x03},
        {.bus = 4, .vendor = 0x10de, .cls = 0x03, .subcls = 0x02},
    };
    int count = sizeof entries / sizeof entries[0];
    if (count > capacity) count = capacity;
    memcpy(list, entries, count * sizeof *list);
    return count;
}
void *nvrm_rm_file_open(bool admin, uint32_t principal) { CHECK(!admin && principal == sender_app); return &rm_calls; }
bool nvrm_rm_adapter_ready(void) {
    return fixture_adapter_ready && (!multi_gpu_ready || strcmp(connected_name, "nocturne.nvidia.rm.0321"));
}
void nvrm_rm_file_close(void *file) { (void)file; }
/* UVM (uvm_nvrm.h), as the server sees it when nvrm runs no UVM. */
static int uvm_requests;
n_result nvrm_uvm_request(struct nvrm_uvm_client **client, uint32_t principal, void *args, uint32_t size,
                          n_handle *attach) {
    (void)client;
    CHECK(principal == sender_app);
    *attach = -1;
    uvm_requests++;
    if (size != sizeof(struct nvrm_uvm_request)) return -N_ERR_INVALID;
    ((struct nvrm_uvm_request *)args)->status = NV_ERR_NOT_SUPPORTED;
    return 0;
}
void nvrm_uvm_client_end(struct nvrm_uvm_client *client) { (void)client; }
/* The screen (display.h), as the server sees it when nvrm shows none it can show images on
   (tests/nvrm_servetest.c checks what reaches it). */
n_result nvrm_display_scanout(void *file, uint32_t principal, void *args, uint32_t size) {
    CHECK(file == &rm_calls && principal == sender_app);
    if (size != sizeof(struct nvrm_scanout_request)) return -N_ERR_INVALID;
    ((struct nvrm_scanout_request *)args)->status = NV_ERR_NOT_SUPPORTED;
    return 0;
}
void nvrm_display_connection_end(void *file) { CHECK(file == &rm_calls); }
uint32_t nvrm_rm_ioctl(void *file, uint32_t escape, void *args, uint32_t size) {
    (void)file;
    rm_calls++;
    if (partial_alloc && escape == NV_ESC_RM_ALLOC && size == sizeof(NVOS21_PARAMETERS)) {
        NVOS21_PARAMETERS *a = args;
        a->hObjectNew = 0x1234;
        a->status = NV_OK;
        if (a->paramsSize == sizeof(uint32_t)) *(uint32_t *)NvP64_VALUE(a->pAllocParms) = 0xcafe;
    }
    if (partial_control && escape == NV_ESC_RM_CONTROL && size == sizeof(NVOS54_PARAMETERS)) {
        NVOS54_PARAMETERS *a = args;
        a->status = NV_OK;
        if (a->paramsSize == sizeof(uint32_t)) *(uint32_t *)NvP64_VALUE(a->params) = 0xbeef;
    }
    if (escape == 0x777 && size == sizeof(uint32_t)) *(uint32_t *)args = 0xabcdef;
    if (escape == NV_ESC_RM_MAP_MEMORY && size == sizeof(nv_ioctl_nvos33_parameters_with_fd)) {
        nv_ioctl_nvos33_parameters_with_fd *m = args;
        m->params.status = individual_status;
        m->params.pLinearAddress = NV_PTR_TO_NvP64((void *)(uintptr_t)0x1000);
    }
    if (escape == NV_ESC_RM_UNMAP_MEMORY) {
        unmap_calls++;
        ((NVOS34_PARAMETERS *)args)->status = individual_status;
    }
    if (escape == NV_ESC_RM_FREE && size == sizeof(NVOS00_PARAMETERS))
        ((NVOS00_PARAMETERS *)args)->status = individual_status;
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
    if (channel == 12) queued_sender = sender_app;
    if (channel == 11) {
        last_reply_size = (*destination)->size;
        response_attachment = attached;
        if (corrupt_protocol) (*destination)->protocol++;
        if (legacy_hello && (*destination)->escape == NVRM_HELLO) (*destination)->access = ARENA_BYTES;
        if (override_hello && (*destination)->escape == NVRM_HELLO) (*destination)->access = hello_flags;
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
    CHECK(server_request(&server)); /* the fixture's client stays connected */
    CHECK(outgoing && capacity >= outgoing_bytes);
    memcpy(data, outgoing, outgoing_bytes);
    if (attached) *attached = response_attachment;
    size_t size = outgoing_bytes;
    free(outgoing); outgoing = NULL; outgoing_bytes = 0;
    return size;
}
n_result n_channel_receive_from(n_handle channel, void *data, size_t capacity, int flags,
                                 struct n_channel_message *message) {
    message->reserved = 0;
    message->sender_app = queued_sender;
    return n_channel_receive(channel, data, capacity, flags, &message->attached);
}

int main(void) {
    server.channel = 11;
    server.arena_handle = -1;
    server.file = &rm_calls;
    server.principal = sender_app;
    struct nvrm rm = {.channel = 12, .arena = shared_page};
    uint32_t untouched = 0;
    rm_result = NV_ERR_INVALID_ARGUMENT;
    CHECK(nvrm_escape(&rm, 0, &untouched, sizeof untouched) == NV_ERR_INVALID_ARGUMENT && untouched == 0);
    rm_result = NV_ERR_NOT_SUPPORTED;
    CHECK(nvrm_escape(&rm, 0x777, &untouched, 0) == NV_ERR_NOT_SUPPORTED);
    CHECK(nvrm_escape(&rm, 0x777, &untouched, sizeof untouched) == NV_ERR_NOT_SUPPORTED);
    CHECK(untouched == 0xabcdef && last_reply_size == sizeof untouched);
    partial_alloc = true;
    uint32_t object = 0, parameter = 0;
    CHECK(nvrm_alloc(&rm, 0, 0, &object, 1, &parameter, sizeof parameter) == NV_ERR_NOT_SUPPORTED);
    CHECK(object == 0x1234 && parameter == 0xcafe && last_reply_size == sizeof(NVOS21_PARAMETERS));
    corrupt_protocol = true;
    object = parameter = 0;
    CHECK(nvrm_alloc(&rm, 0, 0, &object, 1, &parameter, sizeof parameter) == NV_ERR_OPERATING_SYSTEM);
    CHECK(object == 0 && parameter == 0); /* no trusted reply, no invented output */
    corrupt_protocol = false;
    partial_alloc = false;
    partial_control = true;
    parameter = 0;
    CHECK(nvrm_control(&rm, 1, 2, 3, &parameter, sizeof parameter) == NV_ERR_NOT_SUPPORTED);
    CHECK(parameter == 0xbeef);
    partial_control = false;
    individual_status = NV_ERR_PROTECTION_FAULT;
    CHECK(nvrm_free(&rm, 1, 2, 3) == NV_ERR_NOT_SUPPORTED); /* dispatcher error has precedence */
    rm_result = NV_OK;
    CHECK(nvrm_free(&rm, 1, 2, 3) == NV_ERR_PROTECTION_FAULT);
    individual_status = NV_OK;
    /* No driver-specific 1024-byte argument truncation. */
    uint8_t args[2 * N_PAGE_SIZE] = {0};
    rm_result = NV_OK;
    CHECK(nvrm_escape(&rm, 0, args, sizeof args) == NV_OK);
    corrupt_protocol = true;
    untouched = 7;
    CHECK(nvrm_escape(&rm, 0x777, &untouched, sizeof untouched) == NV_ERR_OPERATING_SYSTEM && untouched == 7);
    CHECK(nvrm_escape(&rm, 0, &untouched, sizeof untouched) == NV_ERR_OPERATING_SYSTEM);
    corrupt_protocol = false;
    /* NVRM_UVM goes to UVM, never to RM; its status is UVM's */
    int32_t uvm_file = 5;
    int rm_before_uvm = rm_calls;
    CHECK(nvrm_uvm_open(&rm, 0, &uvm_file) == NV_ERR_NOT_SUPPORTED && uvm_file == 5);
    CHECK(uvm_requests == 1 && rm_calls == rm_before_uvm);
    CHECK(nvrm_escape(&rm, NVRM_UVM, &untouched, sizeof untouched) == NV_ERR_OPERATING_SYSTEM);
    CHECK(uvm_requests == 2 && rm_calls == rm_before_uvm);
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
    rm_result = NV_ERR_INVALID_ADDRESS;
    before = unmap_calls;
    CHECK(nvrm_map(&rm, 1, 2, 3, 0, N_PAGE_SIZE, 0, &address) == NV_ERR_INVALID_ADDRESS);
    CHECK(unmap_calls == before + 1 && map_calls == 1); /* partial map is returned to RM */
    rm_result = NV_OK;

    /* Transferring a connected endpoint does not transfer its bound identity. */
    int before_calls = rm_calls;
    sender_app = 11;
    CHECK(nvrm_escape(&rm, 0, &untouched, sizeof untouched) == NV_ERR_OPERATING_SYSTEM);
    CHECK(rm_calls == before_calls && last_reply_size == 0);
    sender_app = 10;

    /* Public default API still finds a single ready GPU; device opens use its
       BDF, not the first daemon that wins a shared service-name race. */
    server.file = NULL;
    struct nvrm *connection = nvrm_connect();
    CHECK(connection && !strcmp(connected_name, "nocturne.nvidia.rm.0321"));
    CHECK(nvrm_device_ready(connection));
    nvrm_disconnect(connection);
    server.file = NULL; server.arena = NULL; server.arena_handle = -1;
    connection = nvrm_connect_device(N_PCI_TARGET(7, 0, 0));
    CHECK(connection && !strcmp(connected_name, "nocturne.nvidia.rm.0700"));
    CHECK(nvrm_device_ready(connection));
    nvrm_disconnect(connection);
    server.file = NULL; server.arena = NULL; server.arena_handle = -1;
    fixture_adapter_ready = false;
    connection = nvrm_connect_device(0);
    CHECK(connection && !nvrm_device_ready(connection)); /* diagnostic access survives */
    nvrm_disconnect(connection);
    fixture_adapter_ready = true;
    server.file = NULL; server.arena = NULL; server.arena_handle = -1;
    legacy_hello = true;
    connection = nvrm_connect_device(0);
    CHECK(connection && !nvrm_device_ready(connection)); /* old state echo is not proof */
    nvrm_disconnect(connection);
    legacy_hello = false;
    override_hello = true;
    uint64_t flags[] = {
        0, NVRM_HELLO_ADAPTER_READY, NVRM_HELLO_STATUS_PRESENT,
        UINT64_C(1) << 63, NVRM_HELLO_STATUS_PRESENT | (UINT64_C(1) << 63),
        NVRM_HELLO_STATUS_PRESENT | NVRM_HELLO_ADAPTER_READY | (UINT64_C(1) << 63),
    };
    for (size_t i = 0; i < sizeof flags / sizeof flags[0]; i++) {
        hello_flags = flags[i];
        connection = nvrm_connect_device(0);
        CHECK(connection && nvrm_device_ready(connection) == (i == sizeof flags / sizeof flags[0] - 1));
        nvrm_disconnect(connection);
    }
    override_hello = false;
    multi_gpu_ready = true;
    connection = nvrm_connect();
    CHECK(connection && !nvrm_device_ready(connection) && !strcmp(connected_name, "nocturne.nvidia.rm.0321"));
    nvrm_disconnect(connection);
    connection = nvrm_connect_ready();
    CHECK(connection && nvrm_device_ready(connection) && !strcmp(connected_name, "nocturne.nvidia.rm.0400"));
    nvrm_disconnect(connection);
    connection = NULL;
    CHECK(nvrm_connect_ready_status(&connection) == 0 && connection && !strcmp(connected_name, "nocturne.nvidia.rm.0400"));
    nvrm_disconnect(connection);
    /* Why there is none: the first GPU's reason, not the last one's. */
    fixture_adapter_ready = false;
    CHECK(nvrm_connect_ready_status(&connection) == -N_ERR_NOT_READY && !connection);
    fixture_adapter_ready = true;
    connect_result = -N_ERR_NOT_FOUND;
    CHECK(nvrm_connect_ready_status(&connection) == -N_ERR_NOT_FOUND && !connection && !nvrm_connect_ready());
    connect_result = -N_ERR_DENIED;
    CHECK(nvrm_connect_ready_status(&connection) == -N_ERR_DENIED && !connection);
    connect_result = 12;
    CHECK(nvrm_connect_ready_status(NULL) == -N_ERR_INVALID);
    multi_gpu_ready = false;
    CHECK(!nvrm_connect_device((uint32_t)UINT16_MAX + 1));
    CHECK(nvrm_pci_is_gpu(0x10de, 0x03, 0x02, 0x00));
    CHECK(!nvrm_pci_is_gpu(0x10de, 0x03, 0x01, 0x00));
    CHECK(!nvrm_pci_is_gpu(0x10de, 0x03, 0x00, 0x01));

    struct nvrm *status_connection = NULL;
    connect_result = -N_ERR_NOT_FOUND;
    CHECK(nvrm_connect_device_status(0, &status_connection) == -N_ERR_NOT_FOUND && !status_connection);
    connect_result = 12;
    reserve_result = -N_ERR_NO_MEMORY;
    CHECK(nvrm_connect_device_status(0, &status_connection) == -N_ERR_NO_MEMORY && !status_connection);
    reserve_result = 43;
    fail_arena_map = true;
    CHECK(nvrm_connect_device_status(0, &status_connection) == -N_ERR_NO_MEMORY && !status_connection);
    fail_arena_map = false;
    server.file = NULL; server.arena = NULL; server.arena_handle = -1;
    corrupt_protocol = true;
    CHECK(nvrm_connect_device_status(0, &status_connection) == -N_ERR_PROTOCOL && !status_connection);
    corrupt_protocol = false;

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
    void *refused;
    CHECK(n_shared_map(42, N_PAGE_SIZE, N_MEMORY_READ_WRITE, &refused) == -N_ERR_DENIED && !refused);

    legacy_busy = true;
    CHECK(nvrm_serve(N_PCI_TARGET(2, 1, 0)) == -N_ERR_CANCELLED);
    CHECK(listener_calls == 2 && watches == 1 && listener_closes == 2);
    CHECK(!strcmp(listened_name, "nocturne.nvidia.rm.0208"));
    legacy_busy = false;
    listener_calls = watches = listener_closes = 0;
    CHECK(nvrm_serve(N_PCI_TARGET(3, 1, 0)) == -N_ERR_CANCELLED);
    CHECK(listener_calls == 2 && watches == 2 && listener_closes == 3);
    CHECK(stop_wakes == 2 && threads_started == 0); /* the end is announced to client threads */
    printf("nvrm_transport_hosttest: %d failed (channel/RM fixtures; no GPU execution)\n", failures);
    return failures != 0;
}
