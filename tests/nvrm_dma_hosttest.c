/* Production nv_alloc_pages/mapping callbacks, with only the native handle
   services replaced. This does not emulate a GPU or establish DMA acceptance. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ports/nvidia/os_pci.c"

static int failures, creates, closes, maps, views, live;
static uint64_t first_address, replacement_address, scattered[3];
static size_t allocated_bytes;
static uint32_t allocation_flags, view_cache, view_access;
static bool fail_addresses, fail_view;
static uint8_t mapped[3 * NV_RM_PAGE_SIZE];

#define CHECK(expr) do { if (!(expr)) { \
    printf("FAIL line %d: %s\n", __LINE__, #expr); failures++; \
} } while (0)

/* Single-threaded fixture; no RM entry point or background worker is run. */
int mtx_init(mtx_t *m, int type) { (void)m; (void)type; return thrd_success; }
int mtx_lock(mtx_t *m) { (void)m; return thrd_success; }
int mtx_unlock(mtx_t *m) { (void)m; return thrd_success; }
void call_once(once_flag *flag, void (*init)(void)) { (void)flag; init(); }

void nvrm_log(const char *fmt, ...) { (void)fmt; }
void *nvrm_user_mapping_new(void) { return NULL; }
void *nvrm_user_mapping_current(void) { return NULL; }
void nvrm_user_mapping_free(void *owner) { (void)owner; }
n_handle n_dma_create(n_handle pci, size_t bytes, uint32_t flags) {
    (void)pci;
    allocated_bytes = bytes;
    allocation_flags = flags;
    creates++; live++;
    return creates;
}
n_result n_dma_addresses(n_handle h, size_t first, uint64_t *out, size_t count) {
    if (fail_addresses) return -N_ERR_IO;
    if (count == 1) {
        *out = (h == 1 ? first_address : replacement_address) + first * PAGE;
        return 1;
    }
    CHECK(first == 0 && count == 3);
    memcpy(out, scattered, sizeof scattered);
    return 3;
}
n_handle n_shared_view(n_handle h, size_t offset, size_t bytes, uint32_t cache) {
    (void)h;
    view_cache = cache;
    views++;
    CHECK(offset <= allocated_bytes && bytes <= allocated_bytes - offset);
    if (fail_view) return -N_ERR_NO_MEMORY;
    live++;
    return 100;
}
n_handle n_shared_view_access(n_handle h, size_t offset, size_t bytes, uint32_t cache, uint32_t access) {
    view_access = access;
    return n_shared_view(h, offset, bytes, cache);
}
n_handle n_pci_bar(n_handle pci, uint32_t index) {
    (void)pci;
    CHECK(index < BAR_COUNT);
    allocated_bytes = nvrm_dev.info.bar[index].size;
    live++;
    return 200 + index;
}
void *n_shared_map(n_handle h, size_t bytes, int access) {
    (void)h; (void)access;
    CHECK(bytes <= sizeof mapped);
    maps++;
    return mapped;
}
int n_memory_release(void *p, size_t bytes) { (void)p; (void)bytes; maps--; return 0; }
n_result n_handle_close(n_handle h) { CHECK(h >= 0); live--; closes++; return 0; }

static void reset_fixture(void) {
    CHECK(live == 0 && maps == 0);
    creates = closes = views = 0;
    allocation_flags = view_cache = view_access = 0;
    fail_addresses = fail_view = false;
    first_address = replacement_address = PAGE;
}

static void allocation_cases(void) {
    nv_state_t nv = {.dma_mask = UINT32_MAX};
    uint64_t ptes[3] = {0};
    void *priv = NULL;
    reset_fixture();
    /* The high frame is NOT last: the original last-entry-only check missed it. */
    scattered[0] = PAGE;
    scattered[1] = (uint64_t)UINT32_MAX + 1;
    scattered[2] = 2 * PAGE;
    CHECK(nv_alloc_pages(&nv, 3, PAGE, NV_FALSE, NV_MEMORY_CACHED,
                        NV_TRUE, NV_FALSE, 0, ptes, &priv) != NV_OK);
    CHECK(priv == NULL && live == 0 && closes == 1 && ptes[0] == 0);
    CHECK(allocation_flags == N_DMA_32BIT);

    reset_fixture();
    /* The last byte at the inclusive DMA mask is valid. */
    first_address = (uint64_t)UINT32_MAX + 1 - PAGE;
    CHECK(nv_alloc_pages(&nv, 1, PAGE, NV_TRUE, NV_MEMORY_CACHED,
                        NV_TRUE, NV_FALSE, 0, ptes, &priv) == NV_OK);
    CHECK(ptes[0] == first_address);
    CHECK(nv_free_pages(&nv, 1, NV_TRUE, NV_MEMORY_CACHED, priv) == NV_OK);
    priv = NULL;

    reset_fixture();
    nv.dma_mask = UINT64_MAX;
    first_address = UINT64_MAX - PAGE + 1;
    CHECK(nv_alloc_pages(&nv, 2, PAGE, NV_TRUE, NV_MEMORY_CACHED,
                        NV_TRUE, NV_FALSE, 0, ptes, &priv) != NV_OK);
    CHECK(priv == NULL && live == 0 && closes == 1);

    reset_fixture();
    first_address = PAGE + 1;
    CHECK(nv_alloc_pages(&nv, 1, PAGE, NV_TRUE, NV_MEMORY_CACHED,
                        NV_TRUE, NV_FALSE, 0, ptes, &priv) != NV_OK);
    CHECK(priv == NULL && live == 0);
    CHECK(allocation_flags == N_DMA_CONTIGUOUS);

    reset_fixture();
    CHECK(nv_alloc_pages(&nv, 1, 3 * PAGE, NV_TRUE, NV_MEMORY_CACHED,
                        NV_TRUE, NV_FALSE, 0, ptes, &priv) == NV_ERR_INVALID_ARGUMENT);
    CHECK(creates == 0 && priv == NULL);

    reset_fixture();
    fail_addresses = true;
    CHECK(nv_alloc_pages(&nv, 1, PAGE, NV_TRUE, NV_MEMORY_CACHED,
                        NV_TRUE, NV_FALSE, 0, ptes, &priv) != NV_OK);
    CHECK(live == 0 && closes == 1);

    reset_fixture();
    fail_view = true;
    CHECK(nv_alloc_pages(&nv, 2, 2 * PAGE, NV_TRUE, NV_MEMORY_CACHED,
                        NV_TRUE, NV_FALSE, 0, ptes, &priv) != NV_OK);
    CHECK(creates == 2 && closes == 2 && live == 0 && priv == NULL);

    reset_fixture();
    /* Alignment over-allocation: both the backing object and view stay alive
       until free; page_count is still in 4 KiB OS pages. */
    CHECK(nv_alloc_pages(&nv, 2, 2 * PAGE, NV_TRUE, NV_MEMORY_CACHED,
                        NV_TRUE, NV_FALSE, 0, ptes, &priv) == NV_OK);
    CHECK(creates == 2 && closes == 1 && live == 2);
    CHECK(ptes[0] == 2 * PAGE && nvrm_alloc_size(priv) == 2 * PAGE);
    CHECK(nv_free_pages(&nv, 2, NV_TRUE, NV_MEMORY_CACHED, priv) == NV_OK);
    CHECK(live == 0 && closes == 3);
    priv = NULL;

    reset_fixture();
    /* Physical addresses above 4 GiB are legitimate with a wide DMA mask. */
    first_address = (uint64_t)UINT32_MAX + 1;
    CHECK(nv_alloc_pages(&nv, 2, PAGE, NV_TRUE, NV_MEMORY_CACHED,
                        NV_TRUE, NV_FALSE, 0, ptes, &priv) == NV_OK);
    CHECK(ptes[0] == first_address);
    CHECK(nv_free_pages(&nv, 2, NV_TRUE, NV_MEMORY_CACHED, priv) == NV_OK);
    priv = NULL;

    reset_fixture();
    nv.force_dma32_alloc = NV_TRUE;
    first_address = (uint64_t)UINT32_MAX + 1;
    CHECK(nv_alloc_pages(&nv, 1, PAGE, NV_TRUE, NV_MEMORY_CACHED,
                        NV_TRUE, NV_FALSE, 0, ptes, &priv) != NV_OK);
    CHECK(priv == NULL && live == 0);
    CHECK(allocation_flags == (N_DMA_CONTIGUOUS | N_DMA_32BIT));
}

static void mapping_cases(void) {
    reset_fixture();
    nv_state_t nv = {.dma_mask = UINT64_MAX};
    uint64_t pte = 0, user = 0;
    void *priv = NULL, *mapping = NULL;
    CHECK(nv_alloc_pages(&nv, 2, PAGE, NV_TRUE, NV_MEMORY_CACHED,
                        NV_TRUE, NV_FALSE, 0, &pte, &priv) == NV_OK);
    CHECK(nv_alloc_kernel_mapping(&nv, priv, 1, 0, UINT64_MAX, &mapping) == NULL);
    CHECK(nv_alloc_kernel_mapping(&nv, priv, UINT64_MAX, 0, 1, &mapping) == NULL);
    CHECK(nv_alloc_kernel_mapping(&nv, priv, 0, PAGE, 1, &mapping) == NULL);
    CHECK(nv_alloc_kernel_mapping(&nv, priv, 0, 0, 0, &mapping) == NULL);
    CHECK(nv_alloc_kernel_mapping(&nv, priv, 0, 0, PAGE, NULL) == NULL);
    CHECK(nvrm_alloc_view(priv, 1, UINT64_MAX, NV_MEMORY_DEFAULT, N_MEMORY_READ) == -N_ERR_INVALID);
    CHECK(nv_alloc_user_mapping(&nv, priv, 1, PAGE - 1, 2, 0, &user, NULL) == NV_ERR_INVALID_ARGUMENT);
    CHECK(maps == 0 && views == 0);

    void *address = nv_alloc_kernel_mapping(&nv, priv, 1, PAGE - 1, 1, &mapping);
    CHECK(address == mapped + 2 * PAGE - 1 && maps == 1);
    CHECK(nv_alloc_user_mapping(&nv, priv, 1, PAGE - 1, 1, 0, &user, NULL) == NV_OK);
    CHECK(user == pte + 2 * PAGE - 1);
    nv_free_kernel_mapping(&nv, priv, address, mapping);
    CHECK(nv_free_pages(&nv, 2, NV_TRUE, NV_MEMORY_CACHED, priv) == NV_OK);
    CHECK(live == 0 && maps == 0);

    reset_fixture();
    priv = mapping = NULL;
    /* The scrubber's allocation uses contiguous, uncached system memory.
       Verify the actual OS callback's view/mapping ownership for that path;
       this does not exercise Falcon, cache hardware or PCI DMA. */
    CHECK(nv_alloc_pages(&nv, 2, PAGE, NV_TRUE, NV_MEMORY_UNCACHED,
                        NV_TRUE, NV_FALSE, 0, &pte, &priv) == NV_OK);
    address = nv_alloc_kernel_mapping(&nv, priv, 0, 0, 2 * PAGE, &mapping);
    CHECK(address == mapped && mapping != NULL);
    CHECK(view_cache == N_CACHE_UNCACHED && views == 1 && maps == 1 && live == 1);
    nv_free_kernel_mapping(&nv, priv, address, mapping);
    CHECK(maps == 0 && live == 1);
    CHECK(nv_free_pages(&nv, 2, NV_TRUE, NV_MEMORY_UNCACHED, priv) == NV_OK);
    CHECK(live == 0);

    reset_fixture();
    CHECK(nv_alloc_pages(&nv, 1, PAGE, NV_TRUE, NV_MEMORY_CACHED,
                        NV_TRUE, NV_FALSE, 0, &pte, &priv) == NV_OK);
    n_handle ro = nvrm_alloc_view(priv, 0, PAGE, NV_MEMORY_DEFAULT, N_MEMORY_READ);
    CHECK(ro >= 0 && view_access == N_MEMORY_READ);
    n_handle_close(ro);
    CHECK(nv_free_pages(&nv, 1, NV_TRUE, NV_MEMORY_CACHED, priv) == NV_OK);
}

static void bar_cases(void) {
    reset_fixture();
    struct n_pci_function *g = &nvrm_dev.info;
    nv_state_t *nv = &nvrm_dev.nv;
    memset(g, 0, sizeof *g);
    /* Pre-Hopper: 32-bit registers followed by two 64-bit apertures. */
    unsigned old_layout[] = {0, 1, 3};
    for (unsigned a = 0; a < sizeof old_layout / sizeof old_layout[0]; a++) {
        unsigned i = old_layout[a];
        g->bar[i] = (struct n_pci_bar){.address = (i + 1) * 16 * PAGE, .size = 8 * PAGE,
            .flags = N_PCI_BAR_MEMORY | (a ? N_PCI_BAR_64BIT : 0)};
    }
    pci_apertures(g, nv);
    for (unsigned a = 0; a < NV_GPU_NUM_BARS; a++)
        CHECK(nv->bars[a].cpu_address == g->bar[old_layout[a]].address && aperture_bar[a] == old_layout[a]);

    /* All 64-bit: the PCI indices are 0,2,4, not the RM indices 0,1,2. */
    memset(g, 0, sizeof *g);
    for (unsigned i = 0; i < BAR_COUNT; i += 2)
        g->bar[i] = (struct n_pci_bar){.address = (i + 1) * 16 * PAGE, .size = 8 * PAGE,
            .flags = N_PCI_BAR_MEMORY | N_PCI_BAR_64BIT | N_PCI_BAR_PREFETCH};
    pci_apertures(g, nv);
    for (unsigned a = 0; a < NV_GPU_NUM_BARS; a++) {
        CHECK(aperture_bar[a] == 2 * a && nv->bars[a].size == 8 * PAGE);
        uint64_t offset;
        n_handle h = nvrm_bar_view(g->bar[2 * a].address + 1, PAGE - 1,
                                   NV_MEMORY_WRITECOMBINED, N_MEMORY_READ, &offset);
        CHECK(h >= 0 && offset == 1 && view_access == N_MEMORY_READ);
        CHECK(view_cache == (a == NV_GPU_BAR_INDEX_FB ? N_CACHE_WRITE_COMBINE : N_CACHE_UNCACHED));
        n_handle_close(h);
    }
    g->bar[2].flags &= ~N_PCI_BAR_PREFETCH;
    uint64_t offset;
    n_handle h = nvrm_bar_view(g->bar[2].address, PAGE, NV_MEMORY_WRITECOMBINED, N_MEMORY_READ_WRITE, &offset);
    CHECK(h >= 0 && view_cache == N_CACHE_UNCACHED && view_access == N_MEMORY_READ_WRITE);
    n_handle_close(h);
    nvrm_pci_detach();
    CHECK(live == 0);

    MemoryRange ranges[20]; /* Crosses the removed historical 16-range limit. */
    for (unsigned i = 0; i < sizeof ranges / sizeof ranges[0]; i++)
        ranges[i] = (MemoryRange){.start = (i + 1) * PAGE, .size = PAGE};
    nv_usermap_access_params_t u = {0};
    u.memArea.numRanges = sizeof ranges / sizeof ranges[0];
    u.memArea.pRanges = ranges;
    CHECK(nv_add_mapping_context_to_file(nv, &u, NV_PROTECT_READABLE, NULL, 0, 0) == NV_OK);
    struct nvrm_mapping m;
    CHECK(nvrm_take_mapping(&m) && m.range.start == PAGE && m.range.size == sizeof ranges / sizeof ranges[0] * PAGE);
    ranges[1].start += PAGE;
    CHECK(nv_add_mapping_context_to_file(nv, &u, NV_PROTECT_READABLE, NULL, 0, 0) == NV_ERR_NOT_SUPPORTED);
    CHECK(!nvrm_take_mapping(&m));
}

int main(void) {
    allocation_cases();
    mapping_cases();
    bar_cases();
    printf("nvrm_dma_hosttest: %d failed (native services mocked; no GPU execution)\n", failures);
    return failures != 0;
}
