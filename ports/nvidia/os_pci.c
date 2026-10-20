/* NVIDIA RM on Nocturne: the PCI function, its BARs and interrupts, and system memory for the GPU.
 *
 * Everything is reached through the N_KIND_PCI right init hands to nvrm (handle 3):
 *   config space      n_pci_config_read / n_pci_config_write
 *   register BAR      n_pci_bar + n_shared_map (BAR0, mapped whole at start)
 *   other BAR ranges  n_pci_bar + n_shared_view (uncached or write-combining) + n_shared_map
 *   system memory     n_dma_create / n_dma_addresses (the bus address is the physical address;
 *                     Nocturne programs no IOMMU, so RM's "DMA remap" is never needed)
 *   interrupts        n_pci_interrupts / n_pci_interrupt_take
 */
#include "nvrm_os.h"

#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include <os-interface.h>

#define PAGE 4096ull
#define ALIGN_UP(v, a) (((v) + ((a) - 1)) & ~((uint64_t)(a) - 1))
#define ALIGN_DOWN(v, a) ((v) & ~((uint64_t)(a) - 1))

struct nvrm_device nvrm_dev = { .pci = -1 };
nv_state_t nvrm_ctl_state = { .flags = NV_FLAG_CONTROL };

static nvidia_stack_t dummy_stack;
static nvidia_stack_t *const stack_ptr = &dummy_stack;

nv_state_t *NV_API_CALL nv_get_ctl_state(void) { return &nvrm_ctl_state; }
nv_state_t *NV_API_CALL nv_get_adapter_state(NvU32 domain, NvU8 bus, NvU8 slot) {
    nv_state_t *nv = &nvrm_dev.nv;
    if (!nvrm_dev.adapter_ready && !nv->priv) return NULL;
    if (domain == 0 && nv->pci_info.bus == bus && nv->pci_info.slot == slot) return nv;
    return NULL;
}
NvU32 NV_API_CALL nv_get_dev_minor(nv_state_t *nv) { (void)nv; return 0; }

/* ------------------------------------------------------------------------------------------
 * PCI configuration space
 *
 * os_pci_init_handle returns a token for a bus/device/function. The GPU itself reads and writes;
 * other functions (RM looks at the bridges above the GPU) can only be read.
 * ---------------------------------------------------------------------------------------- */
struct pci_token {
    uint32_t target;   /* N_PCI_SELF or N_PCI_TARGET(...) */
    bool self;
    struct pci_token *next;
};

static struct pci_token self_token = { .target = N_PCI_SELF, .self = true };

/* The tokens of other functions. RM never hands one back (os_pci_remove_supported is false) and
   asks for the same bridges again and again, so each function gets one token for the life of
   nvrm. */
static struct pci_token *other_tokens;
static mtx_t token_lock;
static once_flag token_once = ONCE_FLAG_INIT;
static void token_setup(void) { mtx_init(&token_lock, mtx_plain); }

static n_result pci_read(struct pci_token *t, uint32_t offset, uint32_t width) {
    if (!t || nvrm_dev.pci < 0) return -N_ERR_NO_HANDLE;
    return n_pci_config_read(nvrm_dev.pci, t->target, offset, width);
}

static bool function_present(n_result id) { return id >= 0 && (id & 0xFFFF) != 0xFFFF && (id & 0xFFFF) != 0; }

/* Only a function that answers gets a token, so probing empty slots keeps nothing. */
static struct pci_token *other_token(uint32_t target, n_result *id) {
    call_once(&token_once, token_setup);
    mtx_lock(&token_lock);
    struct pci_token *t = other_tokens;
    while (t && t->target != target) t = t->next;
    if (t) {
        mtx_unlock(&token_lock);
        *id = pci_read(t, 0, 4);
        return t;
    }
    struct pci_token probe = { .target = target };
    *id = pci_read(&probe, 0, 4);
    if (function_present(*id) && (t = malloc(sizeof *t))) {
        *t = (struct pci_token){ .target = target, .next = other_tokens };
        other_tokens = t;
    }
    mtx_unlock(&token_lock);
    return t;
}

void *NV_API_CALL os_pci_init_handle(NvU32 domain, NvU8 bus, NvU8 slot, NvU8 function, NvU16 *vendor,
                                     NvU16 *device) {
    if (domain != 0 || slot > 31 || function > 7 || nvrm_dev.pci < 0) return NULL;
    struct pci_token *t;
    n_result id;
    const struct n_pci_function *g = &nvrm_dev.info;
    if (bus == g->bus && slot == g->dev && function == g->func) {
        t = &self_token;
        id = pci_read(t, 0, 4);
    } else {
        t = other_token(N_PCI_TARGET(bus, slot, function), &id);
    }
    if (!t || !function_present(id)) return NULL;
    if (vendor) *vendor = (NvU16)(id & 0xFFFF);
    if (device) *device = (NvU16)((id >> 16) & 0xFFFF);
    return t;
}

NV_STATUS NV_API_CALL os_pci_read_byte(void *handle, NvU32 offset, NvU8 *value) {
    n_result r = pci_read(handle, offset, 1);
    if (r < 0) return NV_ERR_GENERIC;
    *value = (NvU8)r;
    return NV_OK;
}
NV_STATUS NV_API_CALL os_pci_read_word(void *handle, NvU32 offset, NvU16 *value) {
    n_result r = pci_read(handle, offset, 2);
    if (r < 0) return NV_ERR_GENERIC;
    *value = (NvU16)r;
    return NV_OK;
}
NV_STATUS NV_API_CALL os_pci_read_dword(void *handle, NvU32 offset, NvU32 *value) {
    n_result r = pci_read(handle, offset, 4);
    if (r < 0) return NV_ERR_GENERIC;
    *value = (NvU32)r;
    return NV_OK;
}

static NV_STATUS pci_write(void *handle, NvU32 offset, uint32_t width, NvU32 value) {
    struct pci_token *t = handle;
    if (!t || !t->self || nvrm_dev.pci < 0) return NV_ERR_NOT_SUPPORTED;
    return n_pci_config_write(nvrm_dev.pci, offset, width, value) >= 0 ? NV_OK : NV_ERR_GENERIC;
}
NV_STATUS NV_API_CALL os_pci_write_byte(void *handle, NvU32 offset, NvU8 value) { return pci_write(handle, offset, 1, value); }
NV_STATUS NV_API_CALL os_pci_write_word(void *handle, NvU32 offset, NvU16 value) { return pci_write(handle, offset, 2, value); }
NV_STATUS NV_API_CALL os_pci_write_dword(void *handle, NvU32 offset, NvU32 value) { return pci_write(handle, offset, 4, value); }

NvBool NV_API_CALL os_pci_remove_supported(void) { return NV_FALSE; }
void NV_API_CALL os_pci_remove(void *handle) { (void)handle; }
NvBool NV_API_CALL os_is_bif_reset_supported(void *handle) { (void)handle; return NV_FALSE; }
NV_STATUS NV_API_CALL os_enable_pci_req_atomics(void *handle, enum os_pci_req_atomics_type type) {
    (void)handle; (void)type;
    return NV_ERR_NOT_SUPPORTED;
}

/* x86 I/O ports (the legacy VGA window RM pokes at) are not reachable from an app. */
NvU8 NV_API_CALL os_io_read_byte(NvU32 address) { (void)address; return 0xFF; }
NvU16 NV_API_CALL os_io_read_word(NvU32 address) { (void)address; return 0xFFFF; }
NvU32 NV_API_CALL os_io_read_dword(NvU32 address) { (void)address; return 0xFFFFFFFFu; }
void NV_API_CALL os_io_write_byte(NvU32 address, NvU8 value) { (void)address; (void)value; }
void NV_API_CALL os_io_write_word(NvU32 address, NvU16 value) { (void)address; (void)value; }
void NV_API_CALL os_io_write_dword(NvU32 address, NvU32 value) { (void)address; (void)value; }

/* ------------------------------------------------------------------------------------------
 * BARs
 * ---------------------------------------------------------------------------------------- */
#define BAR_COUNT 6

struct bar {
    n_handle handle;   /* n_pci_bar, opened on first use; -1 before */
    uint8_t *map;      /* whole-BAR mapping (BAR0 only) */
};

static struct bar bars[BAR_COUNT] = {
    { -1, NULL }, { -1, NULL }, { -1, NULL }, { -1, NULL }, { -1, NULL }, { -1, NULL },
};
static mtx_t bar_lock;
static once_flag bar_once = ONCE_FLAG_INIT;
static void bar_setup(void) { mtx_init(&bar_lock, mtx_plain); }

static n_handle bar_handle(unsigned index) {
    call_once(&bar_once, bar_setup);
    mtx_lock(&bar_lock);
    if (bars[index].handle < 0) bars[index].handle = n_pci_bar(nvrm_dev.pci, index);
    n_handle h = bars[index].handle;
    mtx_unlock(&bar_lock);
    return h;
}

/* The BAR holding [phys, phys + bytes), or -1. */
static int bar_of(uint64_t phys, uint64_t bytes) {
    for (int i = 0; i < BAR_COUNT; i++) {
        const struct n_pci_bar *b = &nvrm_dev.info.bar[i];
        if (!(b->flags & N_PCI_BAR_MEMORY) || !b->size) continue;
        if (phys >= b->address && bytes <= b->size && phys - b->address <= b->size - bytes) return i;
    }
    return -1;
}

static uint32_t n_cache_for(uint32_t nv_caching, bool device) {
    switch (nv_caching) {
    case NV_MEMORY_WRITECOMBINED: return N_CACHE_WRITE_COMBINE;
    case NV_MEMORY_UNCACHED:
    case NV_MEMORY_UNCACHED_WEAK: return N_CACHE_UNCACHED;
    default: return device ? N_CACHE_UNCACHED : N_CACHE_WRITE_BACK;
    }
}

n_handle nvrm_bar_view(uint64_t phys, uint64_t bytes, uint32_t nv_caching, uint64_t *view_offset) {
    int i = bar_of(phys, bytes ? bytes : 1);
    if (i < 0) return -N_ERR_INVALID;
    n_handle bar = bar_handle((unsigned)i);
    if (bar < 0) return bar;
    uint64_t off = phys - nvrm_dev.info.bar[i].address;
    uint64_t start = ALIGN_DOWN(off, PAGE);
    uint64_t end = ALIGN_UP(off + bytes, PAGE);
    if (view_offset) *view_offset = off - start;
    return n_shared_view(bar, (size_t)start, (size_t)(end - start), n_cache_for(nv_caching, true));
}

/* Kernel mappings of BAR ranges (os_map_kernel_space). BAR0 registers come out of the whole
   mapping; every other range gets a view and a mapping of its own. */
struct kmap {
    void *address;   /* what os_map_kernel_space returned */
    uint8_t *base;   /* start of the n_shared_map mapping */
    size_t length;
};
static struct kmap *kmaps;
static size_t kmap_count, kmap_cap;

void *NV_API_CALL os_map_kernel_space(NvU64 start, NvU64 size, NvU32 mode) {
    int i = bar_of(start, size ? size : 1);
    if (i < 0) {
        nvrm_log("NVRM: os_map_kernel_space(0x%llx, 0x%llx) is outside every BAR", (unsigned long long)start,
                 (unsigned long long)size);
        return NULL;
    }
    uint64_t off = start - nvrm_dev.info.bar[i].address;
    if (i == 0 && bars[0].map && (mode == NV_MEMORY_UNCACHED || mode == NV_MEMORY_UNCACHED_WEAK))
        return bars[0].map + off;

    n_handle bar = bar_handle((unsigned)i);
    if (bar < 0) return NULL;
    uint64_t astart = ALIGN_DOWN(off, PAGE);
    uint64_t length = ALIGN_UP(off + size, PAGE) - astart;
    n_handle view = n_shared_view(bar, (size_t)astart, (size_t)length, n_cache_for(mode, true));
    if (view < 0) return NULL;
    uint8_t *base = n_shared_map(view, (size_t)length, N_MEMORY_READ_WRITE);
    n_handle_close(view);
    if (!base) return NULL;
    call_once(&bar_once, bar_setup);
    mtx_lock(&bar_lock);
    if (kmap_count == kmap_cap) {
        size_t cap = kmap_cap ? kmap_cap * 2 : 16;
        struct kmap *grown = realloc(kmaps, cap * sizeof *grown);
        if (!grown) {
            mtx_unlock(&bar_lock);
            n_memory_release(base, (size_t)length);
            return NULL;
        }
        kmaps = grown;
        kmap_cap = cap;
    }
    kmaps[kmap_count++] = (struct kmap){ base + (off - astart), base, (size_t)length };
    mtx_unlock(&bar_lock);
    return base + (off - astart);
}

void NV_API_CALL os_unmap_kernel_space(void *address, NvU64 size) {
    (void)size;
    if (!address) return;
    uint8_t *p = address;
    if (bars[0].map && p >= bars[0].map && p < bars[0].map + nvrm_dev.info.bar[0].size) return;
    call_once(&bar_once, bar_setup);
    mtx_lock(&bar_lock);
    for (size_t k = 0; k < kmap_count; k++) {
        if (kmaps[k].address != address) continue;
        struct kmap done = kmaps[k];
        kmaps[k] = kmaps[--kmap_count];
        mtx_unlock(&bar_lock);
        n_memory_release(done.base, done.length);
        return;
    }
    mtx_unlock(&bar_lock);
}

/* x86 keeps DMA coherent; write-combining buffers are drained by the fences below. */
NV_STATUS NV_API_CALL os_flush_cpu_cache_all(void) { return NV_OK; }
NV_STATUS NV_API_CALL os_flush_user_cache(void) { return NV_OK; }
void NV_API_CALL os_flush_cpu_write_combine_buffer(void) { __asm__ volatile("sfence" ::: "memory"); }

/* ------------------------------------------------------------------------------------------
 * Attaching to the function
 * ---------------------------------------------------------------------------------------- */
NV_STATUS nvrm_pci_attach(void) {
    struct nvrm_device *d = &nvrm_dev;
    d->pci = NVRM_PCI_HANDLE;
    if (n_handle_kind(d->pci) != N_KIND_PCI) {
        nvrm_log("NVRM: handle %d is not a PCI right (kind %lld)", d->pci, (long long)n_handle_kind(d->pci));
        return NV_ERR_INVALID_STATE;
    }
    n_result r = n_pci_info(d->pci, &d->info);
    if (r < 0) {
        nvrm_log("NVRM: n_pci_info failed: %s", n_result_text(r));
        return NV_ERR_GENERIC;
    }
    const struct n_pci_function *g = &d->info;
    nv_state_t *nv = &d->nv;
    memset(nv, 0, sizeof *nv);
    nv->os_state = d;
    nv->pci_info.domain = 0;
    nv->pci_info.bus = g->bus;
    nv->pci_info.slot = g->dev;
    nv->pci_info.function = g->func;
    nv->pci_info.vendor_id = g->vendor;
    nv->pci_info.device_id = g->device;
    nv->subsystem_id = g->subsystem_id;
    nv->subsystem_vendor = g->subsystem_vendor;
    nv->handle = &self_token;
    nv->queue = nvrm_work_queue();
    nv->dma_mask = ~0ull;
    nv->primary_vga = g->cls == 0x03 && g->subcls == 0x00 && g->framebuffer_bar != N_PCI_NONE;
    nv->console_device = g->framebuffer_bar != N_PCI_NONE;

    /* RM's three apertures: registers (config BAR 0), framebuffer (BAR 1, 64 bit) and the
       instance-memory BAR (config BAR 3). */
    static const unsigned index[NV_GPU_NUM_BARS] = { 0, 1, 3 };
    for (int a = 0; a < NV_GPU_NUM_BARS; a++) {
        const struct n_pci_bar *b = &g->bar[index[a]];
        if (!(b->flags & N_PCI_BAR_MEMORY)) continue;
        nv->bars[a].cpu_address = b->address;
        nv->bars[a].size = b->size;
    }
    nv->regs = &nv->bars[NV_GPU_BAR_INDEX_REGS];
    nv->fb = &nv->bars[NV_GPU_BAR_INDEX_FB];

    for (unsigned dw = 0; dw < NVRM_PCICFG_NUM_DWORDS; dw++) {
        n_result v = n_pci_config_read(d->pci, N_PCI_SELF, dw * 4, 4);
        nv->pci_cfg_space[dw] = v < 0 ? 0xFFFFFFFFu : (NvU32)v;
    }

    const struct n_pci_bar *b0 = &g->bar[0];
    if (!(b0->flags & N_PCI_BAR_MEMORY) || !b0->size) {
        nvrm_log("NVRM: BAR0 is not a memory BAR");
        return NV_ERR_INVALID_STATE;
    }
    n_handle h0 = bar_handle(0);
    if (h0 < 0) {
        nvrm_log("NVRM: n_pci_bar(0) failed: %s", n_result_text(h0));
        return NV_ERR_GENERIC;
    }
    /* nvrm keeps one mapping of the whole BAR0; os_map_kernel_space hands out pieces of it.
       nv->regs->map stays NULL: RM sets it itself (RmSetupRegisters) and asserts it was unset. */
    bars[0].map = n_shared_map(h0, (size_t)b0->size, N_MEMORY_READ_WRITE);
    if (!bars[0].map) {
        nvrm_log("NVRM: mapping BAR0 (%llu bytes) failed", (unsigned long long)b0->size);
        return NV_ERR_GENERIC;
    }

    d->sp = stack_ptr;
    return NV_OK;
}

void nvrm_pci_detach(void) {
    if (bars[0].map) n_memory_release(bars[0].map, (size_t)nvrm_dev.info.bar[0].size);
    bars[0].map = NULL;
}

/* MSI or MSI-X: the system picks (MSI-X when the function offers it) and reports one vector
   for the one it enabled; RM's own flags only record which kind that was. This is where
   NV_FLAG_USES_MSI / NV_FLAG_USES_MSIX are set (RM sources never read them; they are kept
   for RM's bookkeeping, as in NVIDIA's kernel interface). */
static uint32_t irq_vectors;
int nvrm_irq_enable(void) {
    n_result n = n_pci_interrupts(nvrm_dev.pci, 1);
    if (n < 0) {
        nvrm_log("NVRM: n_pci_interrupts failed: %s", n_result_text(n));
        return (int)n;
    }
    irq_vectors = (uint32_t)n;
    nv_state_t *nv = &nvrm_dev.nv;
    if (nvrm_dev.info.msix_vectors) nv->flags |= NV_FLAG_USES_MSIX;
    else nv->flags |= NV_FLAG_USES_MSI;
    return (int)n;
}
uint32_t nvrm_interrupt_count(void) { return irq_vectors; }

NV_STATUS NV_API_CALL nv_get_current_irq_priv_data(nv_state_t *nv, NvU32 *data) {
    (void)nv; (void)data;
    return NV_ERR_NOT_SUPPORTED;
}
nv_soc_irq_type_t NV_API_CALL nv_get_current_irq_type(nv_state_t *nv) { (void)nv; return NV_SOC_IRQ_INVALID_TYPE; }
void NV_API_CALL nv_schedule_uvm_isr(nv_state_t *nv) { (void)nv; }

/* The boot screen, when the firmware put it into this GPU's BAR: RM then keeps that part of
   the framebuffer untouched (it is the console region). */
void NV_API_CALL nv_get_screen_info(nv_state_t *nv, NvU64 *base, NvU32 *width, NvU32 *height, NvU32 *depth,
                                    NvU32 *pitch, NvU64 *size) {
    (void)nv;
    const struct n_pci_function *g = &nvrm_dev.info;
    *base = 0; *width = *height = *depth = *pitch = 0; *size = 0;
    if (g->framebuffer_bar == N_PCI_NONE || !g->framebuffer_size) return;
    *base = g->framebuffer_address;
    *width = g->framebuffer_width;
    *height = g->framebuffer_height;
    *depth = g->framebuffer_bpp;
    *pitch = g->framebuffer_pitch;
    *size = g->framebuffer_size;
}

void NV_API_CALL nv_set_dma_address_size(nv_state_t *nv, NvU32 bits) {
    nv->dma_mask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
}
NvBool NV_API_CALL nv_requires_dma_remap(nv_state_t *nv) { (void)nv; return NV_FALSE; }

/* ------------------------------------------------------------------------------------------
 * System memory for the GPU (nv_alloc_pages and friends)
 *
 * One allocation is one n_dma_create object. RM sees it through the opaque pointer
 * `struct nvrm_alloc *` and the physical (bus) address of each 4 KiB page.
 * ---------------------------------------------------------------------------------------- */
struct nvrm_alloc {
    n_handle dma;           /* the n_dma_create object */
    n_handle root;          /* what views are taken from: dma, or a page-aligned view of it */
    uint64_t pages;         /* 4 KiB pages */
    bool contiguous;
    uint32_t cache;         /* NV_MEMORY_* */
    uint64_t base;          /* contiguous: bus address of page 0 */
    uint64_t *addr;         /* !contiguous: bus address of each page */
    uint8_t *kmap;          /* shared write-back mapping of the whole object, while in use */
    uint32_t kmap_refs;
};

static mtx_t alloc_lock;
static once_flag alloc_once = ONCE_FLAG_INIT;
static void alloc_setup(void) { mtx_init(&alloc_lock, mtx_plain); }

uint64_t nvrm_alloc_size(const struct nvrm_alloc *a) { return a->pages * PAGE; }

static uint64_t alloc_page_address(const struct nvrm_alloc *a, uint64_t index) {
    return a->contiguous ? a->base + index * PAGE : a->addr[index];
}

static NV_STATUS alloc_fetch_addresses(struct nvrm_alloc *a, n_handle object) {
    if (a->contiguous) {
        uint64_t first;
        if (n_dma_addresses(object, 0, &first, 1) != 1) return NV_ERR_GENERIC;
        a->base = first;
        return NV_OK;
    }
    a->addr = malloc((size_t)a->pages * sizeof *a->addr);
    if (!a->addr) return NV_ERR_NO_MEMORY;
    for (uint64_t done = 0; done < a->pages;) {
        n_result n = n_dma_addresses(object, (size_t)done, a->addr + done, (size_t)(a->pages - done));
        if (n <= 0) {
            free(a->addr);
            a->addr = NULL;
            return NV_ERR_GENERIC;
        }
        done += (uint64_t)n;
    }
    return NV_OK;
}

NV_STATUS NV_API_CALL nv_alloc_pages(nv_state_t *nv, NvU32 page_count, NvU64 page_size, NvBool contiguous,
                                     NvU32 cache_type, NvBool zeroed, NvBool unencrypted, NvS32 node_id,
                                     NvU64 *pte_array, void **priv_data) {
    (void)zeroed; (void)unencrypted; (void)node_id; /* n_dma_create returns zeroed RAM */
    call_once(&alloc_once, alloc_setup);
    if (!page_count || !pte_array || !priv_data) return NV_ERR_INVALID_ARGUMENT;
    if (page_size < PAGE) page_size = PAGE;

    struct nvrm_alloc *a = calloc(1, sizeof *a);
    if (!a) return NV_ERR_NO_MEMORY;
    a->dma = a->root = -1;
    a->pages = page_count;
    a->cache = cache_type;

    /* RM may rely on every page_size chunk being aligned and whole; the system gives 4 KiB
       pages, so a request for bigger pages is served from one aligned contiguous run. */
    bool want_contig = contiguous || page_size > PAGE;
    uint32_t flags = 0;
    if (want_contig) flags |= N_DMA_CONTIGUOUS;
    if (nv && (nv->force_dma32_alloc || nv->dma_mask <= 0xFFFFFFFFull)) flags |= N_DMA_32BIT;

    uint64_t bytes = (uint64_t)page_count * PAGE;
    /* No scattered retry for big pages: page_size is the GPU's page granularity, and RM promises
       its callers whole aligned chunks of it. When the run cannot be had, the failure goes back
       to RM, which retries the allocation with 4 KiB pages itself (memdescAlloc). */
    n_handle dma = n_dma_create(nvrm_dev.pci, (size_t)bytes, flags);
    if (dma < 0) {
        free(a);
        return NV_ERR_NO_MEMORY;
    }
    a->dma = a->root = dma;
    a->contiguous = want_contig;

    NV_STATUS status = alloc_fetch_addresses(a, dma);
    if (status == NV_OK && a->contiguous && page_size > PAGE && (a->base & (page_size - 1))) {
        /* Misaligned run: take a larger one and use an aligned part of it. */
        n_handle_close(dma);
        a->dma = a->root = -1;
        n_handle big = n_dma_create(nvrm_dev.pci, (size_t)(bytes + page_size - PAGE), flags);
        if (big < 0) {
            free(a);
            return NV_ERR_NO_MEMORY;
        }
        uint64_t first;
        if (n_dma_addresses(big, 0, &first, 1) != 1) status = NV_ERR_GENERIC;
        else {
            uint64_t skip = ALIGN_UP(first, page_size) - first;
            n_handle view = n_shared_view(big, (size_t)skip, (size_t)bytes, N_CACHE_DEFAULT);
            if (view < 0) status = NV_ERR_NO_MEMORY;
            else {
                a->dma = big;
                a->root = view;
                a->base = first + skip;
            }
        }
        if (status != NV_OK) n_handle_close(big);
    }
    if (status != NV_OK) {
        if (a->dma >= 0) n_handle_close(a->dma);
        free(a->addr);
        free(a);
        return status;
    }
    if (a->dma >= 0 && a->root != a->dma && a->root < 0) a->root = a->dma;

    /* x86 DMA reaches every address the mask allows; check the mask RM set. */
    if (nv && nv->dma_mask != ~0ull) {
        uint64_t last = alloc_page_address(a, a->pages - 1) + PAGE - 1;
        if (last > nv->dma_mask) {
            nvrm_log("NVRM: allocation reaches 0x%llx, above the GPU's DMA mask 0x%llx", (unsigned long long)last,
                     (unsigned long long)nv->dma_mask);
            if (a->root != a->dma) n_handle_close(a->root);
            n_handle_close(a->dma);
            free(a->addr);
            free(a);
            return NV_ERR_NO_MEMORY;
        }
    }

    if (a->contiguous) {
        pte_array[0] = a->base;
        if (!contiguous) for (uint64_t i = 1; i < a->pages; i++) pte_array[i] = a->base + i * PAGE;
    } else {
        memcpy(pte_array, a->addr, (size_t)a->pages * sizeof *a->addr);
    }
    *priv_data = a;
    return NV_OK;
}

NV_STATUS NV_API_CALL nv_free_pages(nv_state_t *nv, NvU32 page_count, NvBool contiguous, NvU32 cache_type,
                                    void *priv_data) {
    (void)nv; (void)page_count; (void)contiguous; (void)cache_type;
    struct nvrm_alloc *a = priv_data;
    if (!a) return NV_ERR_INVALID_ARGUMENT;
    if (a->kmap) n_memory_release(a->kmap, (size_t)(a->pages * PAGE));
    if (a->root != a->dma && a->root >= 0) n_handle_close(a->root);
    if (a->dma >= 0) n_handle_close(a->dma);
    free(a->addr);
    free(a);
    return NV_OK;
}

NV_STATUS NV_API_CALL nv_alias_pages(nv_state_t *nv, NvU32 count, NvU64 page_size, NvU32 contiguous, NvU32 cache,
                                     NvU64 guest_id, NvU64 *pte_array, NvBool carveout, void **priv_data) {
    (void)nv; (void)count; (void)page_size; (void)contiguous; (void)cache; (void)guest_id; (void)pte_array;
    (void)carveout; (void)priv_data;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_get_num_phys_pages(void *priv, NvU32 *pages) {
    struct nvrm_alloc *a = priv;
    if (!a || !pages) return NV_ERR_INVALID_ARGUMENT;
    *pages = (NvU32)a->pages;
    return NV_OK;
}
/* The pages as host page structures: no such thing here. */
NV_STATUS NV_API_CALL nv_get_phys_pages(void *priv, void *pages, NvU32 *count) {
    (void)priv; (void)pages; (void)count;
    return NV_ERR_NOT_SUPPORTED;
}

/* ---- CPU mappings of an allocation, inside nvrm ---- */
struct kern_mapping {
    uint8_t *base;
    size_t length;
};

void *NV_API_CALL nv_alloc_kernel_mapping(nv_state_t *nv, void *priv, NvU64 page_index, NvU32 page_offset,
                                          NvU64 size, void **priv_data) {
    (void)nv;
    struct nvrm_alloc *a = priv;
    call_once(&alloc_once, alloc_setup);
    if (!a || page_index >= a->pages) return NULL;
    uint64_t span = ALIGN_UP((uint64_t)page_offset + size, PAGE);
    if (page_index + span / PAGE > a->pages) return NULL;
    uint32_t cache = n_cache_for(a->cache, false);
    if (cache == N_CACHE_WRITE_BACK) {
        mtx_lock(&alloc_lock);
        if (!a->kmap) a->kmap = n_shared_map(a->root, (size_t)(a->pages * PAGE), N_MEMORY_READ_WRITE);
        uint8_t *base = a->kmap;
        if (base) a->kmap_refs++;
        mtx_unlock(&alloc_lock);
        *priv_data = NULL;
        return base ? base + page_index * PAGE + page_offset : NULL;
    }
    n_handle view = n_shared_view(a->root, (size_t)(page_index * PAGE), (size_t)span, cache);
    if (view < 0) return NULL;
    uint8_t *base = n_shared_map(view, (size_t)span, N_MEMORY_READ_WRITE);
    n_handle_close(view);
    if (!base) return NULL;
    struct kern_mapping *m = malloc(sizeof *m);
    if (!m) {
        n_memory_release(base, (size_t)span);
        return NULL;
    }
    m->base = base;
    m->length = (size_t)span;
    *priv_data = m;
    return base + page_offset;
}

void NV_API_CALL nv_free_kernel_mapping(nv_state_t *nv, void *priv, void *address, void *priv_data) {
    (void)nv; (void)address;
    struct nvrm_alloc *a = priv;
    if (priv_data) {
        struct kern_mapping *m = priv_data;
        n_memory_release(m->base, m->length);
        free(m);
        return;
    }
    call_once(&alloc_once, alloc_setup);
    mtx_lock(&alloc_lock);
    if (a && a->kmap && a->kmap_refs && --a->kmap_refs == 0) {
        n_memory_release(a->kmap, (size_t)(a->pages * PAGE));
        a->kmap = NULL;
    }
    mtx_unlock(&alloc_lock);
}

/* ---- mappings for a client ----
 *
 * RM's "user address" for system memory is the bus address of the page plus the offset in
 * it (the value an mmap offset carried on the systems RM was written for). RM hands it back
 * in rm_create_mmap_context, and os_match_mmap_offset turns it into a page index again. */
NV_STATUS NV_API_CALL nv_alloc_user_mapping(nv_state_t *nv, void *priv, NvU64 page_index, NvU32 page_offset,
                                            NvU64 size, NvU32 protect, NvU64 *user_address, void **priv_data) {
    (void)nv; (void)size; (void)protect;
    struct nvrm_alloc *a = priv;
    if (!a || page_index >= a->pages || !user_address) return NV_ERR_INVALID_ARGUMENT;
    *user_address = alloc_page_address(a, page_index) + page_offset;
    if (priv_data) *priv_data = NULL;
    return NV_OK;
}
void NV_API_CALL nv_free_user_mapping(nv_state_t *nv, void *priv, NvU64 address, void *priv_data) {
    (void)nv; (void)priv; (void)address; (void)priv_data;
}

NV_STATUS NV_API_CALL os_match_mmap_offset(void *priv, NvU64 offset, NvU64 *page_index) {
    struct nvrm_alloc *a = priv;
    if (!a || !page_index) return NV_ERR_INVALID_ARGUMENT;
    if (a->contiguous) {
        if (offset < a->base || offset >= a->base + a->pages * PAGE) return NV_ERR_OBJECT_NOT_FOUND;
        *page_index = (offset - a->base) / PAGE;
        return NV_OK;
    }
    for (uint64_t i = 0; i < a->pages; i++) {
        if (a->addr[i] == offset) {
            *page_index = i;
            return NV_OK;
        }
    }
    return NV_ERR_OBJECT_NOT_FOUND;
}

n_handle nvrm_alloc_view(struct nvrm_alloc *a, uint64_t page_index, uint64_t bytes, uint32_t nv_caching) {
    if (!a || page_index >= a->pages || !bytes) return -N_ERR_INVALID;
    uint64_t span = ALIGN_UP(bytes, PAGE);
    if (page_index + span / PAGE > a->pages) return -N_ERR_INVALID;
    uint32_t cache = nv_caching == NV_MEMORY_DEFAULT ? n_cache_for(a->cache, false) : n_cache_for(nv_caching, false);
    return n_shared_view(a->root, (size_t)(page_index * PAGE), (size_t)span, cache);
}

/* ---- the mapping RM parked for the request being served ---- */
static thread_local struct nvrm_mapping parked;
static thread_local bool parked_valid;

NV_STATUS NV_API_CALL nv_add_mapping_context_to_file(nv_state_t *nv, nv_usermap_access_params_t *nvuap, NvU32 prot,
                                                     void *alloc_priv, NvU64 page_index, NvU32 fd) {
    (void)nv; (void)fd;
    struct nvrm_mapping *m = &parked;
    memset(m, 0, sizeof *m);
    m->prot = prot;
    m->caching = nvuap->caching;
    if (alloc_priv) {
        m->is_alloc = true;
        m->alloc = alloc_priv;
        m->page_index = page_index;
        m->size = nvuap->size;
    } else {
        m->access_start = nvuap->access_start;
        m->access_size = nvuap->access_size;
        uint32_t n = nvuap->memArea.numRanges;
        if (n > NVRM_MAPPING_MAX_RANGES) return NV_ERR_NOT_SUPPORTED;
        for (uint32_t i = 0; i < n; i++) {
            m->ranges[i].start = nvuap->memArea.pRanges[i].start;
            m->ranges[i].size = nvuap->memArea.pRanges[i].size;
        }
        m->range_count = n;
    }
    parked_valid = true;
    return NV_OK;
}

bool nvrm_take_mapping(struct nvrm_mapping *out) {
    if (!parked_valid) return false;
    *out = parked;
    parked_valid = false;
    return true;
}
void nvrm_drop_mapping(void) { parked_valid = false; }
