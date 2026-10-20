/* Exercise the actual RM adaptation boundary, not a simulated GPU. Native
   handles, RM's dispatcher and condition primitives are controlled fixtures. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ports/nvidia/os.c"
#include "../ports/nvidia/rm_api.c"

struct nvrm_device nvrm_dev;
static int failures, condition_waits, broadcasts, revokes, closed, cleanup_calls;
static bool fail_revoke, have_mapping;
static uint32_t seen_principal, return_status, native_access;
static bool seen_admin;
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
nv_state_t *NV_API_CALL nv_get_ctl_state(void) { return &nvrm_dev.nv; }

NV_STATUS NV_API_CALL rm_ioctl(nvidia_stack_t *s, nv_state_t *nv, nv_file_private_t *f,
                               NvU32 escape, void *args, NvU32 size) {
    (void)s; (void)nv; (void)f;
    seen_principal = os_get_current_process();
    seen_admin = os_is_administrator();
    if (escape == NV_ESC_RM_MAP_MEMORY && size == sizeof(nv_ioctl_nvos33_parameters_with_fd)) {
        nv_ioctl_nvos33_parameters_with_fd *m = args;
        m->params.status = NV_OK;
        m->params.pLinearAddress = NV_PTR_TO_NvP64((void *)(uintptr_t)0x1000);
        rm_mapping_private = nvrm_user_mapping_new();
        pending_mapping.owner = rm_mapping_private;
        have_mapping = true;
    }
    if (escape == NV_ESC_RM_UNMAP_MEMORY && size == sizeof(NVOS34_PARAMETERS)) {
        nvrm_user_mapping_free(rm_mapping_private);
        rm_mapping_private = NULL;
        ((NVOS34_PARAMETERS *)args)->status = NV_OK;
    }
    return return_status;
}
void NV_API_CALL rm_cleanup_file_private(nvidia_stack_t *s, nv_state_t *nv, nv_file_private_t *f) {
    (void)s; (void)nv;
    CHECK(os_get_current_process() == ((struct nvrm_file *)f)->principal);
    CHECK(!os_is_administrator());
    cleanup_calls++;
}
void nvrm_drop_mapping(void) { have_mapping = false; }
bool nvrm_take_mapping(struct nvrm_mapping *m) {
    if (!have_mapping) return false;
    have_mapping = false;
    *m = pending_mapping;
    return true;
}
uint64_t nvrm_alloc_size(const struct nvrm_alloc *a) { (void)a; return 2 * PAGE; }
n_handle nvrm_alloc_view(struct nvrm_alloc *a, uint64_t page, uint64_t bytes, uint32_t cache, uint32_t access) {
    (void)a; (void)page; (void)bytes; (void)cache;
    native_access = access;
    return 40;
}
n_handle nvrm_bar_view(uint64_t phys, uint64_t bytes, uint32_t cache, uint32_t access, uint64_t *offset) {
    (void)phys; (void)bytes; (void)cache;
    native_access = access;
    *offset = 0;
    return 41;
}
n_result n_shared_revoke(n_handle h) { CHECK(h == 40 || h == 41); revokes++; return fail_revoke ? -N_ERR_IO : 0; }
n_result n_handle_close(n_handle h) { CHECK(h == 40 || h == 41); closed++; return 0; }

static void principal_cases(void) {
    struct nvrm_file *a = nvrm_rm_file_open(false), *b = nvrm_rm_file_open(false);
    CHECK(a && b && a->principal != b->principal);
    CHECK(a->principal != 2 && b->principal != 2);
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
    struct nvrm_file *c = nvrm_rm_file_open(false);
    CHECK(c && c->principal != first && c->principal != second);
    nvrm_rm_file_close(b);
    nvrm_rm_file_close(c);
    CHECK(cleanup_calls == 3 && !files);
}

static void mapping_cases(void) {
    struct nvrm_file *f = nvrm_rm_file_open(false);
    nv_ioctl_nvos33_parameters_with_fd map = {.params = {.hClient = 1, .hDevice = 2, .hMemory = 3}};
    pending_mapping = (struct nvrm_mapping){.is_alloc = true, .size = PAGE, .prot = NV_PROTECT_READABLE};
    uint64_t offset = 0, bytes = 0;
    uint32_t access = 0;
    CHECK(nvrm_rm_ioctl(f, NV_ESC_RM_MAP_MEMORY, &map, sizeof map) == NV_OK);
    CHECK(nvrm_rm_mapping_object(f, &offset, &bytes, &access) == 40);
    CHECK(access == N_MEMORY_READ && native_access == N_MEMORY_READ && bytes == PAGE && offset == 0);
    CHECK(f->mappings && closed == 0);
    /* Blocking new maps is not proof that already-issued maps were revoked. */
    nv_acquire_mmap_lock(&nvrm_dev.nv);
    nv_set_safe_to_mmap_locked(&nvrm_dev.nv, NV_FALSE);
    CHECK(!nv_get_all_mappings_revoked_locked(&nvrm_dev.nv));
    nv_release_mmap_lock(&nvrm_dev.nv);
    fail_revoke = true;
    CHECK(nv_revoke_gpu_mappings(&nvrm_dev.nv) == NV_ERR_OPERATING_SYSTEM);
    CHECK(!nv_get_all_mappings_revoked_locked(&nvrm_dev.nv));
    fail_revoke = false;
    CHECK(nv_revoke_gpu_mappings(&nvrm_dev.nv) == NV_OK);
    CHECK(nv_get_all_mappings_revoked_locked(&nvrm_dev.nv) && revokes == 2);
    CHECK(nvrm_rm_ioctl(f, NV_ESC_RM_MAP_MEMORY, &map, sizeof map) == NV_ERR_INVALID_STATE);
    nv_acquire_mmap_lock(&nvrm_dev.nv);
    nv_set_safe_to_mmap_locked(&nvrm_dev.nv, NV_TRUE);
    nv_release_mmap_lock(&nvrm_dev.nv);
    NVOS34_PARAMETERS unmap = {.hClient = 1, .hDevice = 2, .hMemory = 3,
        .pLinearAddress = NV_PTR_TO_NvP64((void *)(uintptr_t)0x1000)};
    CHECK(nvrm_rm_ioctl(f, NV_ESC_RM_UNMAP_MEMORY, &unmap, sizeof unmap) == NV_OK);
    CHECK(!f->mappings && closed == 1);
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
    CHECK(f->mappings && !f->mappings->next && f->mappings == rm_mapping_private && closed == 2);
    /* An implicit RM user-unmap callback, without an UNMAP escape, revokes the
       exact remaining view before the backing resource can be reused. */
    nvrm_user_mapping_free(rm_mapping_private);
    rm_mapping_private = NULL;
    CHECK(!f->mappings && closed == 3);
    nvrm_rm_file_close(f);
    CHECK(closed == 3 && !files);

    /* Real representational exhaustion rejects allocation; closing a file does
       not recycle a principal that may survive in an RM sharing policy. */
    next_principal = UINT32_MAX;
    f = nvrm_rm_file_open(false);
    CHECK(f && f->principal == UINT32_MAX && !next_principal);
    CHECK(!nvrm_rm_file_open(false));
    nvrm_rm_file_close(f);
    CHECK(!nvrm_rm_file_open(false));
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

int main(void) {
    principal_cases();
    mapping_cases();
    wait_queue_cases();
    printf("nvrm_contract_hosttest: %d failed (RM/native services mocked; no GPU execution)\n", failures);
    return failures != 0;
}
