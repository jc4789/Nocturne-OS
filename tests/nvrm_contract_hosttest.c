/* Exercise the actual RM adaptation boundary, not a simulated GPU. Native
   handles, RM's dispatcher and condition primitives are controlled fixtures. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
static bool fail_next_allocation;
static void *contract_calloc(size_t count, size_t size);
#define calloc contract_calloc
#include "../ports/nvidia/os.c"
#include "../ports/nvidia/rm_api.c"
#undef calloc

static void *contract_calloc(size_t count, size_t size) {
    if (fail_next_allocation) { fail_next_allocation = false; return NULL; }
    return calloc(count, size);
}

struct nvrm_device nvrm_dev;
static int failures, condition_waits, broadcasts, revokes, closed, cleanup_calls, releases, drops;
static bool fail_revoke, have_mapping;
static uint32_t seen_principal, return_status, native_access;
static bool seen_admin;
static bool park_nonmap;
static uint32_t map_argument_status;
static struct nvrm_mapping pending_mapping;
static void *rm_mapping_private;

#define CHECK(expr) do { if (!(expr)) { \
    printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; \
} } while (0)

int mtx_init(mtx_t *m, int type) { (void)m; (void)type; return thrd_success; }
int mtx_lock(mtx_t *m) { (void)m; return thrd_success; }
int mtx_unlock(mtx_t *m) { (void)m; return thrd_success; }
void mtx_destroy(mtx_t *m) { (void)m; }
int cnd_init(cnd_t *c) { (void)c; return thrd_success; }
void cnd_destroy(cnd_t *c) { (void)c; }
int cnd_broadcast(cnd_t *c) { (void)c; broadcasts++; return thrd_success; }
int cnd_wait(cnd_t *c, mtx_t *m) {
    (void)c; (void)m;
    /* There must be no wait after complete-all, even for a later caller. */
    condition_waits++;
    CHECK(false);
    exit(1);
}
void call_once(once_flag *flag, void (*init)(void)) { (void)flag; init(); }
void thrd_yield(void) {}
n_result n_app_self(void) { return 2; }
/* The system log, so a case can count what the driver reports; there is no disk C: to keep a file. */
static unsigned dropped_reports;
n_handle n_system_log_open(void) { return 50; }
n_result n_channel_send(n_handle h, const void *data, size_t n, n_handle attach) {
    CHECK(h == 50 && attach == -1);
    char line[1024];
    size_t keep = n < sizeof line - 1 ? n : sizeof line - 1;
    memcpy(line, data, keep);
    line[keep] = 0;
    if (strstr(line, "does not collect")) dropped_reports++;
    return (n_result)n;
}
n_result n_app_data_folder(const char *name, char *out, size_t cap) {
    (void)name; (void)out; (void)cap;
    return -N_ERR_NOT_FOUND;
}
n_result n_path_join(const char *folder, const char *name, char *out, size_t cap) {
    (void)folder; (void)name; (void)out; (void)cap;
    CHECK(false);
    return -N_ERR_DENIED;
}
n_result n_document_append(const char *path, const void *data, size_t size) {
    (void)path; (void)data; (void)size;
    CHECK(false);
    return -N_ERR_DENIED;
}
uint64_t n_clock_uptime_ms(void) { return 0; }
nv_state_t *NV_API_CALL nv_get_ctl_state(void) { return &nvrm_dev.nv; }

NV_STATUS NV_API_CALL rm_ioctl(nvidia_stack_t *s, nv_state_t *nv, nv_file_private_t *f,
                               NvU32 escape, void *args, NvU32 size) {
    (void)s; (void)nv;
    seen_principal = os_get_current_process();
    seen_admin = os_is_administrator();
    if (escape == NV_ESC_RM_MAP_MEMORY && size == sizeof(nv_ioctl_nvos33_parameters_with_fd)) {
        nv_ioctl_nvos33_parameters_with_fd *m = args;
        m->params.status = map_argument_status;
        uint64_t address = 0;
        rm_mapping_private = nvrm_user_mapping_new(PAGE, &address);
        m->params.pLinearAddress = NV_PTR_TO_NvP64((void *)(uintptr_t)address);
        pending_mapping.owner = rm_mapping_private;
        have_mapping = true;
    }
    if (park_nonmap) have_mapping = true;
    if (escape == NV_ESC_RM_UNMAP_MEMORY && size == sizeof(NVOS34_PARAMETERS)) {
        /* As RM does: the mapping is the one its address names (refFindCpuMapping). */
        NVOS34_PARAMETERS *u = args;
        uint64_t at = (uint64_t)(uintptr_t)NvP64_VALUE(u->pLinearAddress);
        struct exported_mapping *named = NULL;
        for (struct exported_mapping *k = ((struct nvrm_file *)f)->mappings; k && !named; k = k->next)
            if (at >= k->address && at - k->address < k->bytes) named = k;
        if (named == rm_mapping_private) rm_mapping_private = NULL;
        nvrm_user_mapping_free(named);
        u->status = named ? NV_OK : NV_ERR_OBJECT_NOT_FOUND;
    }
    return return_status;
}
/* nv.c's part of a request (os_ioctl.c, its own test): every escape here is RM's. */
bool nvrm_nv_ioctl(struct nvrm_gpu_attachments *attached, uint32_t escape, void *args, uint32_t size,
                   NV_STATUS *status) {
    (void)attached; (void)escape; (void)args; (void)size; (void)status;
    return false;
}
void nvrm_nv_file_close(struct nvrm_gpu_attachments *attached) { CHECK(attached && !attached->count); }
void NV_API_CALL rm_cleanup_file_private(nvidia_stack_t *s, nv_state_t *nv, nv_file_private_t *f) {
    (void)s; (void)nv;
    CHECK(os_get_current_process() == ((struct nvrm_file *)f)->principal);
    CHECK(!os_is_administrator());
    cleanup_calls++;
}
void nvrm_drop_mapping(void) { have_mapping = false; drops++; }
bool nvrm_take_mapping(struct nvrm_mapping *m) {
    if (!have_mapping) return false;
    have_mapping = false;
    *m = pending_mapping;
    return true;
}
n_handle nvrm_mapping_view(const struct nvrm_mapping *m, uint32_t access, uint64_t page_offset,
                           uint64_t *offset, uint64_t *bytes) {
    native_access = access;
    *offset = page_offset;
    *bytes = (page_offset + m->size + PAGE - 1) & ~(PAGE - 1);
    return 40;
}
void nvrm_mapping_release(struct nvrm_mapping *m) {
    free(m->ranges); m->ranges = NULL; m->range_count = 0;
    releases++;
}
n_result n_shared_revoke(n_handle h) { CHECK(h == 40 || h == 41); revokes++; return fail_revoke ? -N_ERR_IO : 0; }
n_result n_handle_close(n_handle h) { CHECK(h == 40 || h == 41); closed++; return 0; }

static void principal_cases(void) {
    struct nvrm_file *a = nvrm_rm_file_open(false, 10), *b = nvrm_rm_file_open(false, 10);
    struct nvrm_file *c = nvrm_rm_file_open(false, 11);
    CHECK(a && b && c && a->principal == b->principal && a->principal != c->principal);
    CHECK(a->principal == 10 && c->principal == 11);
    uint32_t first = a->principal, second = b->principal;
    CHECK(nvrm_rm_ioctl(a, 0, NULL, 0) == NV_OK);
    CHECK(seen_principal == first && !seen_admin);
    CHECK(os_get_current_process() == 2 && os_is_administrator());
    CHECK(nvrm_rm_ioctl(b, 0, NULL, 0) == NV_OK);
    CHECK(seen_principal == second && !seen_admin);
    return_status = NV_ERR_INVALID_ARGUMENT;
    CHECK(nvrm_rm_ioctl(a, 0, NULL, 0) == NV_ERR_INVALID_ARGUMENT);
    CHECK(os_get_current_process() == 2 && os_is_administrator());
    return_status = NV_OK;
    nvrm_rm_file_close(a);
    CHECK(nvrm_rm_ioctl(c, 0, NULL, 0) == NV_OK && seen_principal == 11);
    nvrm_rm_file_close(b);
    nvrm_rm_file_close(c);
    CHECK(cleanup_calls == 3 && !files);
    CHECK(!nvrm_rm_file_open(false, 0));
}

static void mapping_cases(void) {
    struct nvrm_file *f = nvrm_rm_file_open(false, 10);
    nv_ioctl_nvos33_parameters_with_fd map = {.params = {.hClient = 1, .hDevice = 2, .hMemory = 3}};
    pending_mapping = (struct nvrm_mapping){.is_alloc = true, .size = PAGE, .prot = NV_PROTECT_READABLE};
    uint64_t offset = 0, bytes = 0;
    uint32_t access = 0;
    CHECK(nvrm_rm_ioctl(f, NV_ESC_RM_MAP_MEMORY, &map, sizeof map) == NV_OK);
    NvP64 first_linear = map.params.pLinearAddress;
    CHECK(nvrm_rm_mapping_object(f, &offset, &bytes, &access) == 40);
    CHECK(access == N_MEMORY_READ && native_access == N_MEMORY_READ && bytes == PAGE && offset == 0);
    CHECK(f->mappings && closed == 0);
    /* Temporary idle revocation is not permanent capability destruction. */
    int before_revoke = revokes;
    CHECK(nv_revoke_gpu_mappings(&nvrm_dev.nv) == NV_ERR_NOT_SUPPORTED);
    CHECK(safe_to_mmap && revokes == before_revoke && f->mappings->view == 40);
    CHECK(!nv_get_all_mappings_revoked_locked(&nvrm_dev.nv));
    nv_acquire_mmap_lock(&nvrm_dev.nv);
    nv_set_safe_to_mmap_locked(&nvrm_dev.nv, NV_FALSE);
    CHECK(!nv_get_all_mappings_revoked_locked(&nvrm_dev.nv));
    nv_release_mmap_lock(&nvrm_dev.nv);
    fail_revoke = true;
    CHECK(nv_revoke_gpu_mappings(&nvrm_dev.nv) == NV_ERR_NOT_SUPPORTED);
    CHECK(!nv_get_all_mappings_revoked_locked(&nvrm_dev.nv));
    fail_revoke = false;
    CHECK(!safe_to_mmap && revokes == before_revoke);
    /* RM-owned lifetimes outside an IPC request are legal even while native
       publication is unsafe. Only their actual allocation can report OOM. */
    uint64_t orphan_address = 0;
    void *orphan = nvrm_user_mapping_new(PAGE, &orphan_address);
    CHECK(orphan && orphan_mappings == orphan && !((struct exported_mapping *)orphan)->file);
    CHECK(orphan_address >= USER_ADDRESSES_FIRST && orphan_address != (uint64_t)(uintptr_t)NvP64_VALUE(first_linear));
    nvrm_user_mapping_free(orphan);
    CHECK(!orphan_mappings && revokes == before_revoke);
    fail_next_allocation = true;
    CHECK(!nvrm_user_mapping_new(PAGE, &orphan_address) && !orphan_mappings);
    CHECK(!nvrm_user_mapping_new(0, &orphan_address) && !orphan_mappings);
    CHECK(nvrm_rm_ioctl(f, NV_ESC_RM_MAP_MEMORY, &map, sizeof map) == NV_ERR_INVALID_STATE);
    nv_acquire_mmap_lock(&nvrm_dev.nv);
    nv_set_safe_to_mmap_locked(&nvrm_dev.nv, NV_TRUE);
    nv_release_mmap_lock(&nvrm_dev.nv);
    NVOS34_PARAMETERS unmap = {.hClient = 1, .hDevice = 2, .hMemory = 3, .pLinearAddress = first_linear};
    CHECK(nvrm_rm_ioctl(f, NV_ESC_RM_UNMAP_MEMORY, &unmap, sizeof unmap) == NV_OK);
    CHECK(!f->mappings && closed == 1);
    pending_mapping.prot = NV_PROTECT_WRITEABLE;
    CHECK(nvrm_rm_ioctl(f, NV_ESC_RM_MAP_MEMORY, &map, sizeof map) == NV_OK);
    CHECK(nvrm_rm_mapping_object(f, &offset, &bytes, &access) == 40 && access == N_MEMORY_READ_WRITE);
    nvrm_user_mapping_free(rm_mapping_private);
    CHECK(!f->mappings && closed == 2);
    pending_mapping.prot = NV_PROTECT_READ_WRITE;
    CHECK(nvrm_rm_ioctl(f, NV_ESC_RM_MAP_MEMORY, &map, sizeof map) == NV_OK);
    CHECK(nvrm_rm_mapping_object(f, &offset, &bytes, &access) == 40 && access == N_MEMORY_READ_WRITE);
    /* Same physical address, two distinct RM lifetime tokens. Releasing either
       (as FREE of a parent may do) must retain the unrelated mapping. */
    void *first_owner = rm_mapping_private;
    CHECK(nvrm_rm_ioctl(f, NV_ESC_RM_MAP_MEMORY, &map, sizeof map) == NV_OK);
    CHECK(nvrm_rm_mapping_object(f, &offset, &bytes, &access) == 40);
    CHECK(rm_mapping_private != first_owner && f->mappings->next);
    nvrm_user_mapping_free(first_owner);
    CHECK(f->mappings && !f->mappings->next && f->mappings == rm_mapping_private && closed == 3);
    /* A second map of the same memory, then the client's unmap of it by the address RM returned:
       RM finds the mapping by that address alone, so the older one stays live with its view. */
    struct exported_mapping *older = rm_mapping_private;
    NvP64 older_linear = map.params.pLinearAddress;
    CHECK(nvrm_rm_ioctl(f, NV_ESC_RM_MAP_MEMORY, &map, sizeof map) == NV_OK);
    CHECK(nvrm_rm_mapping_object(f, &offset, &bytes, &access) == 40);
    CHECK(rm_mapping_private != older && map.params.pLinearAddress != older_linear);
    unmap.pLinearAddress = map.params.pLinearAddress;
    CHECK(nvrm_rm_ioctl(f, NV_ESC_RM_UNMAP_MEMORY, &unmap, sizeof unmap) == NV_OK && unmap.status == NV_OK);
    CHECK(f->mappings == older && !older->next && older->view == 40 && closed == 4);
    rm_mapping_private = older;
    /* An implicit RM user-unmap callback, without an UNMAP escape, revokes the
       exact remaining view before the backing resource can be reused. */
    nvrm_user_mapping_free(rm_mapping_private);
    rm_mapping_private = NULL;
    CHECK(!f->mappings && closed == 5);
    /* An invalid protection still releases the taken dynamic mapping context. */
    pending_mapping.prot = 0;
    CHECK(nvrm_rm_ioctl(f, NV_ESC_RM_MAP_MEMORY, &map, sizeof map) == NV_OK);
    int before_release = releases;
    CHECK(nvrm_rm_mapping_object(f, &offset, &bytes, &access) == -N_ERR_DENIED);
    CHECK(releases == before_release + 1);
    nvrm_user_mapping_free(rm_mapping_private);
    return_status = NV_ERR_INVALID_ADDRESS;
    before_release = drops;
    CHECK(nvrm_rm_ioctl(f, NV_ESC_RM_MAP_MEMORY, &map, sizeof map) == NV_ERR_INVALID_ADDRESS);
    CHECK(!have_mapping && drops == before_release + 2);
    nvrm_user_mapping_free(rm_mapping_private);
    return_status = NV_OK;
    map_argument_status = NV_ERR_PROTECTION_FAULT;
    CHECK(nvrm_rm_ioctl(f, NV_ESC_RM_MAP_MEMORY, &map, sizeof map) == NV_OK && !have_mapping);
    nvrm_user_mapping_free(rm_mapping_private);
    map_argument_status = NV_OK;
    park_nonmap = true;
    CHECK(nvrm_rm_ioctl(f, NV_ESC_RM_ALLOC_MEMORY, &map, sizeof map) == NV_OK && !have_mapping);
    park_nonmap = false;
    orphan = nvrm_user_mapping_new(PAGE, &orphan_address);
    CHECK(orphan && !((struct exported_mapping *)orphan)->file);
    nvrm_rm_file_close(f);
    CHECK(closed == 5 && !files);
    CHECK(orphan_mappings == orphan); /* a different lifetime, not this file's */
    nvrm_user_mapping_free(orphan);
    CHECK(!orphan_mappings);
}

static void wait_queue_cases(void) {
    os_wait_queue *q = NULL;
    CHECK(os_alloc_wait_queue(&q) == NV_OK && q);
    os_wake_up(q);
    os_wait_interruptible(q);
    os_wait_uninterruptible(q);
    CHECK(!condition_waits && broadcasts == 1);
    os_free_wait_queue(q);
}

/* Events with data queue up to the bound; past it the newest are dropped until the client
   collects. Data-less events stay one per object and index. */
static void event_cases(void) {
    struct nvrm_file *f = nvrm_rm_file_open(false, 12);
    CHECK(f);
    if (!f) return;
    nv_event_t post = {.nvfp = &f->nvfp}, got;
    NvU32 more = 0;
    for (unsigned i = 0; i < EVENT_DATA_QUEUED + 10; i++) nv_post_event(&post, 0x100, 1, i, 0, NV_TRUE);
    CHECK(f->data_events == EVENT_DATA_QUEUED && f->overflowed && dropped_reports == 1);
    nv_post_event(&post, 0x200, 2, 0, 0, NV_FALSE);
    nv_post_event(&post, 0x200, 2, 0, 0, NV_FALSE);
    unsigned collected = 0, data_less = 0;
    while (nv_get_event(&f->nvfp, &got, &more) == 0) {
        if (got.hObject == 0x100) CHECK(got.info32 == collected++); /* oldest first, none skipped */
        else data_less++;
    }
    CHECK(collected == EVENT_DATA_QUEUED && data_less == 1 && !more);
    CHECK(f->data_events == 0 && !f->overflowed);
    nv_post_event(&post, 0x100, 1, 7, 0, NV_TRUE);
    CHECK(nv_get_event(&f->nvfp, &got, &more) == 0 && got.info32 == 7);
    nvrm_rm_file_close(f);
}

int main(void) {
    principal_cases();
    mapping_cases();
    wait_queue_cases();
    event_cases();
    printf("nvrm_contract_hosttest: %d failed (RM/native services mocked; no GPU execution)\n", failures);
    return failures != 0;
}
