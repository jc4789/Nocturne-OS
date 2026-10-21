/* NVIDIA RM on Nocturne: the PCI function, its BARs and interrupts, and system memory for the GPU.
 *
 * Everything is reached through the N_KIND_PCI right init hands to nvrm (handle 3):
 *   config space      n_pci_config_read / n_pci_config_write
 *   register BAR      n_pci_bar + n_shared_map (BAR0, mapped whole at start)
 *   other BAR ranges  n_pci_bar + n_shared_view (uncached or write-combining) + n_shared_map
 *   system memory     n_dma_create / n_dma_addresses (physical addresses; where an IOMMU
 *                     isolates the GPU, its domain maps them at the same addresses)
 *   interrupts        n_pci_interrupts / n_pci_interrupt_take
 */
#include "nvrm_os.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include <os-interface.h>

#define PAGE 4096ull
#define ALIGN_UP(v, a) (((v) + ((a) - 1)) & ~((uint64_t)(a) - 1))
#define ALIGN_DOWN(v, a) ((v) & ~((uint64_t)(a) - 1))

enum { PCI_COMMAND = 0x04, PCI_COMMAND_MEMORY = 1u << 1, PCI_COMMAND_MASTER = 1u << 2 };

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
#define BAR_COUNT (sizeof nvrm_dev.info.bar / sizeof nvrm_dev.info.bar[0])

struct bar {
    n_handle handle;   /* n_pci_bar, opened on first use; -1 before */
    uint8_t *map;      /* whole-BAR mapping (BAR0 only) */
};

static struct bar bars[BAR_COUNT] = {
    { -1, NULL }, { -1, NULL }, { -1, NULL }, { -1, NULL }, { -1, NULL }, { -1, NULL },
};
/* RM aperture order differs from PCI configuration indices after a 64-bit BAR. */
static unsigned aperture_bar[NV_GPU_NUM_BARS];
static void pci_apertures(const struct n_pci_function *g, nv_state_t *nv) {
    for (unsigned a = 0; a < NV_GPU_NUM_BARS; a++) {
        aperture_bar[a] = N_PCI_NONE;
        nv->bars[a].cpu_address = nv->bars[a].size = 0;
    }
    unsigned aperture = 0;
    for (unsigned i = 0; i < BAR_COUNT && aperture < NV_GPU_NUM_BARS; i++) {
        const struct n_pci_bar *b = &g->bar[i];
        if ((b->flags & N_PCI_BAR_MEMORY) && b->size) {
            aperture_bar[aperture] = i;
            nv->bars[aperture].cpu_address = b->address;
            nv->bars[aperture].size = b->size;
            aperture++;
        }
        /* The upper DWORD of a 64-bit BAR never denotes a second aperture. */
        if (b->flags & N_PCI_BAR_64BIT) i++;
    }
}
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

static bool ranges_overlap(uint64_t a, uint64_t a_size, uint64_t b, uint64_t b_size) {
    return a_size && b_size && (a >= b ? a - b < b_size : b - a < a_size);
}

static uint32_t bar_cache(unsigned index, uint64_t phys, uint64_t bytes, uint32_t nv_caching) {
    /* Register/doorbell accesses remain UC. RM requests WC for both framebuffer
       and instance memory, subject to the native BAR's prefetchable capability.
       USERD is a special UC region inside BAR1, supplied by RM in nv->ud.
       Test the actual page-rounded span: even a partial USERD overlap must not
       create a WC alias for its page. */
    const nv_state_t *nv = &nvrm_dev.nv;
    if (index == aperture_bar[NV_GPU_BAR_INDEX_REGS] ||
        !(nvrm_dev.info.bar[index].flags & N_PCI_BAR_PREFETCH) ||
        (index == aperture_bar[NV_GPU_BAR_INDEX_FB] && nv->ud.cpu_address &&
         ranges_overlap(phys, bytes, nv->ud.cpu_address, nv->ud.size)) ||
        (nv_caching == NV_MEMORY_WRITECOMBINED && rm_disable_iomap_wc()))
        return N_CACHE_UNCACHED;
    return n_cache_for(nv_caching, true);
}

n_handle nvrm_bar_view(uint64_t phys, uint64_t bytes, uint32_t nv_caching, uint32_t access,
                       uint64_t *view_offset) {
    if (!bytes) return -N_ERR_INVALID;
    int i = bar_of(phys, bytes);
    if (i < 0) return -N_ERR_INVALID;
    n_handle bar = bar_handle((unsigned)i);
    if (bar < 0) return bar;
    uint64_t off = phys - nvrm_dev.info.bar[i].address;
    if (off > SIZE_MAX - bytes || off + bytes > SIZE_MAX - (PAGE - 1)) return -N_ERR_TOO_LARGE;
    uint64_t start = ALIGN_DOWN(off, PAGE);
    uint64_t end = ALIGN_UP(off + bytes, PAGE);
    if (view_offset) *view_offset = off - start;
    uint32_t cache = bar_cache((unsigned)i, nvrm_dev.info.bar[i].address + start, end - start, nv_caching);
    return n_shared_view_access(bar, (size_t)start, (size_t)(end - start), cache, access);
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
    unsigned registers = aperture_bar[NV_GPU_BAR_INDEX_REGS];
    if ((unsigned)i == registers && bars[i].map) return bars[i].map + off;

    n_handle bar = bar_handle((unsigned)i);
    if (bar < 0) return NULL;
    if (!size || off > SIZE_MAX - size || off + size > SIZE_MAX - (PAGE - 1)) return NULL;
    uint64_t astart = ALIGN_DOWN(off, PAGE);
    uint64_t length = ALIGN_UP(off + size, PAGE) - astart;
    uint32_t cache = bar_cache((unsigned)i, nvrm_dev.info.bar[i].address + astart, length, mode);
    n_handle view = n_shared_view(bar, (size_t)astart, (size_t)length, cache);
    if (view < 0) return NULL;
    uint8_t *base = n_shared_map(view, (size_t)length, N_MEMORY_READ_WRITE);
    n_handle_close(view);
    if (!base) return NULL;
    call_once(&bar_once, bar_setup);
    mtx_lock(&bar_lock);
    if (kmap_count == kmap_cap) {
        if (kmap_cap > SIZE_MAX / 2 / sizeof *kmaps) {
            mtx_unlock(&bar_lock);
            n_memory_release(base, (size_t)length);
            return NULL;
        }
        size_t cap = kmap_cap ? kmap_cap * 2 : 1;
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
    uintptr_t p = (uintptr_t)address;
    unsigned registers = aperture_bar[NV_GPU_BAR_INDEX_REGS];
    if (registers < BAR_COUNT && bars[registers].map && p >= (uintptr_t)bars[registers].map &&
        p - (uintptr_t)bars[registers].map < nvrm_dev.info.bar[registers].size) return;
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
/* RM writes back the whole CPU cache only for a GPU that cannot snoop (memdescFlushCpuCaches
   asks kbifIsSnoopDmaCapable). A PCIe GPU on x86 snoops, so RM never asks here; an app could not
   write back every cache of every CPU anyway, and reporting success would hide stale data. */
NV_STATUS NV_API_CALL os_flush_cpu_cache_all(void) {
    static atomic_flag said = ATOMIC_FLAG_INIT;
    if (!atomic_flag_test_and_set(&said))
        nvrm_log("NVRM: RM asked to write back all CPU caches (a GPU that does not snoop?); not supported");
    return NV_ERR_NOT_SUPPORTED;
}
/* A client's request (NV0000_CTRL_CMD_OS_UNIX_FLUSH_USER_CACHE) for its cached system memory:
   the GPU reaches cached memory snooped (GMMU SYS_COH), so on x86 there is nothing to write back.
   Uncached and write-combined memory never has cache lines (nv_alloc_pages types it). */
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

    pci_apertures(g, nv);
    nv->regs = &nv->bars[NV_GPU_BAR_INDEX_REGS];
    nv->fb = &nv->bars[NV_GPU_BAR_INDEX_FB];
    /* RM maps the boot screen through BAR1's virtual mode only when it starts at BAR1 offset 0
       (bPreserveBar1ConsoleEnabled); anywhere else the screen is lost once RM starts. */
    if (g->framebuffer_bar != N_PCI_NONE)
        nvrm_log("NVRM: boot screen %ux%u at 0x%llx (%llu KiB), BAR1 at 0x%llx: %s", (unsigned)g->framebuffer_width,
                 (unsigned)g->framebuffer_height, (unsigned long long)g->framebuffer_address,
                 (unsigned long long)(g->framebuffer_size >> 10), (unsigned long long)nv->fb->cpu_address,
                 g->framebuffer_address == nv->fb->cpu_address ? "RM keeps it mapped"
                                                               : "not at BAR1's start, RM will not keep it mapped");

    n_result command = n_pci_config_read(d->pci, N_PCI_SELF, PCI_COMMAND, sizeof(uint16_t));
    if (command < 0) return NV_ERR_OPERATING_SYSTEM;
    nvrm_log("NVRM: PCI Command=0x%04x at attach (memory=%u, bus-master=%u); DMA addresses are physical, %s",
             (unsigned)command, !!(command & PCI_COMMAND_MEMORY), !!(command & PCI_COMMAND_MASTER),
             g->flags & N_PCI_ISOLATED ? "the IOMMU confines the GPU to its DMA memory" : "no IOMMU isolates the GPU");

    for (unsigned dw = 0; dw < NVRM_PCICFG_NUM_DWORDS; dw++) {
        n_result v = n_pci_config_read(d->pci, N_PCI_SELF, dw * 4, 4);
        nv->pci_cfg_space[dw] = v < 0 ? 0xFFFFFFFFu : (NvU32)v;
    }

    unsigned registers = aperture_bar[NV_GPU_BAR_INDEX_REGS];
    if (registers >= BAR_COUNT) {
        nvrm_log("NVRM: no register memory BAR");
        return NV_ERR_INVALID_STATE;
    }
    const struct n_pci_bar *b0 = &g->bar[registers];
    if (b0->size > SIZE_MAX) return NV_ERR_INVALID_ARGUMENT;
    n_handle h0 = bar_handle(registers);
    if (h0 < 0) {
        nvrm_log("NVRM: n_pci_bar(%u) failed: %s", registers, n_result_text(h0));
        return NV_ERR_GENERIC;
    }
    /* nvrm keeps one mapping of the whole BAR0; os_map_kernel_space hands out pieces of it.
       nv->regs->map stays NULL: RM sets it itself (RmSetupRegisters) and asserts it was unset. */
    bars[registers].map = n_shared_map(h0, (size_t)b0->size, N_MEMORY_READ_WRITE);
    if (!bars[registers].map) {
        nvrm_log("NVRM: mapping BAR0 (%llu bytes) failed", (unsigned long long)b0->size);
        return NV_ERR_GENERIC;
    }

    d->sp = stack_ptr;
    return NV_OK;
}

/* PCI Local Bus, type-0 header: Command is a 16-bit register at 04h. MSE (bit 1) admits CPU
   memory accesses; BME (bit 2) admits the function's memory requests, DMA and MSI included. RM
   expects both on before rm_init_adapter (it sets them itself only when it recovers from a
   function that stopped answering, or restores config space after a reset): without bus
   mastering the SEC2 scrubber's first IMEM transfer never completes. The other bits stay as the
   system set them (the kernel refuses changes to them). The write is read back: a function that
   did not take it is not reported as mastering. */
bool nvrm_pci_bus_master(bool on) {
    struct nvrm_device *d = &nvrm_dev;
    n_result command = n_pci_config_read(d->pci, N_PCI_SELF, PCI_COMMAND, sizeof(uint16_t));
    if (command < 0) {
        nvrm_log("NVRM: cannot read PCI Command: %s", n_result_text(command));
        return false;
    }
    uint16_t want = on ? (uint16_t)command | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER
                       : (uint16_t)command & ~PCI_COMMAND_MASTER;
    n_result r = n_pci_config_write(d->pci, PCI_COMMAND, sizeof(uint16_t), want);
    n_result now = n_pci_config_read(d->pci, N_PCI_SELF, PCI_COMMAND, sizeof(uint16_t));
    bool mastering = now >= 0 && (now & PCI_COMMAND_MASTER);
    d->bus_master = on ? mastering : mastering && d->bus_master;
    if (r < 0 || now < 0 || mastering != on) {
        nvrm_log("NVRM: switching bus mastering %s failed: %s (Command 0x%04x)", on ? "on" : "off",
                 n_result_text(r < 0 ? r : now < 0 ? now : -N_ERR_IO), (unsigned)(now < 0 ? command : now));
        return false;
    }
    nvrm_log("NVRM: bus mastering %s (Command 0x%04x)", on ? "on" : "off", (unsigned)now);
    return true;
}

void nvrm_pci_detach(void) {
    for (size_t k = 0; k < kmap_count; k++) n_memory_release(kmaps[k].base, kmaps[k].length);
    free(kmaps); kmaps = NULL; kmap_count = kmap_cap = 0;
    for (unsigned i = 0; i < BAR_COUNT; i++) {
        if (bars[i].map) n_memory_release(bars[i].map, (size_t)nvrm_dev.info.bar[i].size);
        bars[i].map = NULL;
        if (bars[i].handle >= 0) n_handle_close(bars[i].handle);
        bars[i].handle = -1;
    }
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

/* RM brackets starting, disabling and shutting down the adapter (and power transitions) with
   these when nv_get_screen_info reports the boot screen in this GPU's BAR: physical RM (GSP)
   rebuilds BAR1 meanwhile, and a CPU write through BAR1 can disturb that. The system stops
   drawing there (n_pci_hold_screen) until the matching enable. RM keeps the boot screen mapped
   at BAR1 offset 0 through virtual mode when the screen starts there (logged at attach), so
   drawing then goes on where it was. Only the enable or the end of nvrm's claim (which stops
   the GPU first) gives the screen back; with boot option fbhold=<s> the system stops nvrm when
   a hold lasts that long. */
void NV_API_CALL os_disable_console_access(void) {
    n_result r = n_pci_hold_screen(nvrm_dev.pci, true);
    if (r < 0 && r != -N_ERR_NOT_FOUND) nvrm_log("NVRM: cannot hold the boot screen: %s", n_result_text(r));
    else if (r >= 0) nvrm_log("NVRM: the system stopped drawing to the boot screen");
}
void NV_API_CALL os_enable_console_access(void) {
    n_result r = n_pci_hold_screen(nvrm_dev.pci, false);
    if (r < 0 && r != -N_ERR_NOT_FOUND) nvrm_log("NVRM: cannot release the boot screen: %s", n_result_text(r));
    else if (r >= 0) nvrm_log("NVRM: the system draws to the boot screen again");
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

/* One owner for the backing allocation, its optional aligned view and address
   table. Every failed construction follows the same unwind as a normal free. */
static void alloc_destroy(struct nvrm_alloc *a) {
    if (a->kmap) n_memory_release(a->kmap, (size_t)(a->pages * PAGE));
    if (a->root != a->dma && a->root >= 0) n_handle_close(a->root);
    if (a->dma >= 0) n_handle_close(a->dma);
    free(a->addr);
    free(a);
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
        if (n <= 0 || (uint64_t)n > a->pages - done) {
            free(a->addr);
            a->addr = NULL;
            return NV_ERR_GENERIC;
        }
        done += (uint64_t)n;
    }
    return NV_OK;
}

static NV_STATUS alloc_check_addresses(const struct nvrm_alloc *a, uint64_t mask) {
    /* Scattered frames are not ordered by bus address. Checking only the last
       entry can publish an unreachable earlier frame to RM. Use subtraction
       so an overflowing physical end address cannot pass the mask check. */
    uint64_t count = a->contiguous ? 1 : a->pages;
    uint64_t span = a->contiguous ? a->pages * PAGE : PAGE;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t address = alloc_page_address(a, i);
        if ((address & (PAGE - 1)) || mask < span - 1 || address > mask - (span - 1)) {
            nvrm_log("NVRM: DMA range base=0x%llx bytes=0x%llx cannot use mask=0x%llx (page %llu)",
                     (unsigned long long)address, (unsigned long long)span, (unsigned long long)mask,
                     (unsigned long long)i);
            return NV_ERR_INVALID_ADDRESS;
        }
    }
    return NV_OK;
}

NV_STATUS NV_API_CALL nv_alloc_pages(nv_state_t *nv, NvU32 page_count, NvU64 page_size, NvBool contiguous,
                                     NvU32 cache_type, NvBool zeroed, NvBool unencrypted, NvS32 node_id,
                                     NvU64 *pte_array, void **priv_data) {
    (void)zeroed; (void)unencrypted; (void)node_id; /* n_dma_create returns zeroed RAM */
    call_once(&alloc_once, alloc_setup);
    if (!page_count || !pte_array || !priv_data) return NV_ERR_INVALID_ARGUMENT;
    *priv_data = NULL;
    if (page_size < PAGE) page_size = PAGE;
    uint64_t bytes = (uint64_t)page_count * PAGE;
    /* page_count is in OS pages (osGetPagesInfo/osAllocPagesInternal), not in
       page_size units. Alignment must be a power of two and any over-allocation
       must fit the native size_t; neither is an arbitrary allocation ceiling. */
    if ((page_size & (page_size - 1)) || page_size - PAGE > SIZE_MAX - bytes)
        return NV_ERR_INVALID_ARGUMENT;

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
            alloc_destroy(a);
            return NV_ERR_NO_MEMORY;
        }
        a->dma = a->root = big;
        uint64_t first;
        if (n_dma_addresses(big, 0, &first, 1) != 1) status = NV_ERR_GENERIC;
        else if ((first & (PAGE - 1)) || first > UINT64_MAX - (page_size - 1))
            status = NV_ERR_INVALID_ADDRESS;
        else {
            uint64_t skip = ALIGN_UP(first, page_size) - first;
            n_handle view = n_shared_view(big, (size_t)skip, (size_t)bytes, N_CACHE_DEFAULT);
            if (view < 0) status = NV_ERR_NO_MEMORY;
            else {
                a->root = view;
                a->base = first + skip;
            }
        }
    }
    uint64_t mask = nv ? nv->dma_mask : UINT64_MAX;
    if ((flags & N_DMA_32BIT) && mask > UINT32_MAX) mask = UINT32_MAX;
    if (status == NV_OK) status = alloc_check_addresses(a, mask);
    /* RM hands uncached and write-combined system memory to the GPU as non-snooped (GMMU
       SYS_NONCOH) and does nothing to the CPU's caches itself: the memory must have that type
       before RM learns its addresses. The first typed view makes it the object's type and
       writes back what the system's zeroing left in the caches (n_shared_view on DMA memory);
       every later mapping and view of the object uses that type. */
    uint32_t typed = n_cache_for(cache_type, false);
    if (status == NV_OK && typed != N_CACHE_WRITE_BACK) {
        n_handle view = n_shared_view(a->root, 0, (size_t)bytes, typed);
        if (view < 0) {
            nvrm_log("NVRM: cannot make %llu KiB of DMA memory %s: %s", (unsigned long long)(bytes >> 10),
                     typed == N_CACHE_UNCACHED ? "uncached" : "write-combined", n_result_text(view));
            status = NV_ERR_NO_MEMORY;
        } else {
            n_handle_close(view);
        }
    }
    if (status != NV_OK) {
        alloc_destroy(a);
        return status;
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
    alloc_destroy(a);
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

static bool alloc_range_valid(const struct nvrm_alloc *a, uint64_t page_index, uint64_t page_offset,
                               uint64_t size) {
    if (!a || page_index >= a->pages || page_offset >= PAGE || !size) return false;
    return size <= (a->pages - page_index) * PAGE - page_offset;
}

void *NV_API_CALL nv_alloc_kernel_mapping(nv_state_t *nv, void *priv, NvU64 page_index, NvU32 page_offset,
                                          NvU64 size, void **priv_data) {
    (void)nv;
    struct nvrm_alloc *a = priv;
    call_once(&alloc_once, alloc_setup);
    if (!priv_data) return NULL;
    *priv_data = NULL;
    if (!alloc_range_valid(a, page_index, page_offset, size)) return NULL;
    uint64_t span = ALIGN_UP((uint64_t)page_offset + size, PAGE);
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
 * RM's "user address" for system memory names the mapping (nvrm_user_mapping_new gives each
 * one addresses of its own, as many as the whole allocation has bytes) and says which page of
 * the allocation it starts at and where in that page. RM hands it back in
 * rm_create_mmap_context of the same request, and os_match_mmap_offset turns it into a page
 * index again. */
NV_STATUS NV_API_CALL nv_alloc_user_mapping(nv_state_t *nv, void *priv, NvU64 page_index, NvU32 page_offset,
                                            NvU64 size, NvU32 protect, NvU64 *user_address, void **priv_data) {
    (void)nv; (void)protect;
    struct nvrm_alloc *a = priv;
    if (!user_address || !priv_data || !alloc_range_valid(a, page_index, page_offset, size))
        return NV_ERR_INVALID_ARGUMENT;
    uint64_t first;
    *priv_data = nvrm_user_mapping_new(a->pages * PAGE, &first);
    if (!*priv_data) return NV_ERR_NO_MEMORY;
    *user_address = first + page_index * PAGE + page_offset;
    return NV_OK;
}
void NV_API_CALL nv_free_user_mapping(nv_state_t *nv, void *priv, NvU64 address, void *priv_data) {
    (void)nv; (void)priv; (void)address;
    nvrm_user_mapping_free(priv_data);
}

/* offset is the page-aligned address of the mapping this request just made of the allocation. */
NV_STATUS NV_API_CALL os_match_mmap_offset(void *priv, NvU64 offset, NvU64 *page_index) {
    struct nvrm_alloc *a = priv;
    if (!a || !page_index) return NV_ERR_INVALID_ARGUMENT;
    uint64_t first, bytes;
    if (!nvrm_user_mapping_span(nvrm_user_mapping_current(), &first, &bytes) || bytes != a->pages * PAGE ||
        offset < first || offset - first >= bytes)
        return NV_ERR_OBJECT_NOT_FOUND;
    *page_index = (offset - first) / PAGE;
    return NV_OK;
}

n_handle nvrm_alloc_view(struct nvrm_alloc *a, uint64_t page_index, uint64_t bytes, uint32_t nv_caching,
                         uint32_t access) {
    if (!alloc_range_valid(a, page_index, 0, bytes)) return -N_ERR_INVALID;
    uint64_t span = ALIGN_UP(bytes, PAGE);
    uint32_t cache = nv_caching == NV_MEMORY_DEFAULT ? n_cache_for(a->cache, false) : n_cache_for(nv_caching, false);
    return n_shared_view_access(a->root, (size_t)(page_index * PAGE), (size_t)span, cache, access);
}

/* ---- the mapping RM parked for the request being served ---- */
static thread_local struct nvrm_mapping parked;
static thread_local bool parked_valid;

NV_STATUS NV_API_CALL nv_add_mapping_context_to_file(nv_state_t *nv, nv_usermap_access_params_t *nvuap, NvU32 prot,
                                                     void *alloc_priv, NvU64 page_index, NvU32 fd) {
    (void)nv; (void)fd;
    nvrm_drop_mapping();
    if (!nvuap) return NV_ERR_INVALID_ARGUMENT;
    struct nvrm_mapping *m = &parked;
    m->owner = nvrm_user_mapping_current();
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
        uint64_t n = nvuap->memArea.numRanges;
        if (!n || !nvuap->memArea.pRanges || n > SIZE_MAX / sizeof *m->ranges)
            return NV_ERR_INVALID_ARGUMENT;
        uint64_t total = 0;
        for (uint64_t i = 0; i < n; i++) {
            uint64_t start = nvuap->memArea.pRanges[i].start;
            uint64_t size = nvuap->memArea.pRanges[i].size;
            /* Each disjoint extent represents whole OS pages. RM rounds the
               first/last extent before installing this context. */
            if (!size || start > UINT64_MAX - size || total > SIZE_MAX - size ||
                (n > 1 && ((start | size) & (PAGE - 1)))) return NV_ERR_INVALID_ARGUMENT;
            total += size;
        }
        m->ranges = malloc((size_t)n * sizeof *m->ranges);
        if (!m->ranges) return NV_ERR_NO_MEMORY;
        m->range_count = (size_t)n;
        for (size_t i = 0; i < m->range_count; i++) {
            m->ranges[i].start = nvuap->memArea.pRanges[i].start;
            m->ranges[i].size = nvuap->memArea.pRanges[i].size;
        }
    }
    parked_valid = true;
    return NV_OK;
}

bool nvrm_take_mapping(struct nvrm_mapping *out) {
    if (!out || !parked_valid) return false;
    *out = parked;
    memset(&parked, 0, sizeof parked);
    parked_valid = false;
    return true;
}
void nvrm_mapping_release(struct nvrm_mapping *m) {
    if (!m) return;
    free(m->ranges);
    memset(m, 0, sizeof *m);
}
void nvrm_drop_mapping(void) {
    nvrm_mapping_release(&parked);
    parked_valid = false;
}

n_handle nvrm_mapping_view(const struct nvrm_mapping *m, uint32_t access, uint64_t page_offset,
                           uint64_t *view_offset, uint64_t *bytes) {
    if (!m || !view_offset || !bytes || page_offset >= PAGE) return -N_ERR_INVALID;
    if (m->is_alloc) {
        uint64_t whole = nvrm_alloc_size(m->alloc);
        if (!m->size || m->page_index > UINT64_MAX / PAGE ||
            m->size > SIZE_MAX - page_offset - (PAGE - 1)) return -N_ERR_INVALID;
        uint64_t start = m->page_index * PAGE;
        uint64_t length = ALIGN_UP(page_offset + m->size, PAGE);
        if (start >= whole || length > whole - start) return -N_ERR_INVALID;
        n_handle h = nvrm_alloc_view(m->alloc, m->page_index, length, m->caching, access);
        if (h >= 0) { *view_offset = page_offset; *bytes = length; }
        return h;
    }
    if (!m->range_count || !m->ranges) return -N_ERR_INVALID;
    if (m->range_count == 1) {
        uint64_t off = 0, size = m->ranges[0].size;
        if (size > SIZE_MAX - (PAGE - 1) - (PAGE - 1)) return -N_ERR_TOO_LARGE;
        n_handle h = nvrm_bar_view(m->ranges[0].start, size, m->caching, access, &off);
        if (h >= 0) { *view_offset = page_offset + off; *bytes = ALIGN_UP(off + size, PAGE); }
        return h;
    }
    if (m->range_count > SIZE_MAX / sizeof(struct n_shared_range)) return -N_ERR_TOO_LARGE;
    struct n_shared_range *parts = calloc(m->range_count, sizeof *parts);
    if (!parts) return -N_ERR_NO_MEMORY;
    size_t made = 0;
    uint64_t total = 0;
    n_handle h = -N_ERR_INVALID;
    for (size_t i = 0; i < m->range_count; i++) {
        const struct nvrm_range *r = &m->ranges[i];
        if (!r->size || ((r->start | r->size) & (PAGE - 1)) || r->size > SIZE_MAX - total) goto done;
        uint64_t off;
        h = nvrm_bar_view(r->start, r->size, m->caching, access, &off);
        if (h < 0) goto done;
        parts[made++] = (struct n_shared_range){.shared = h, .bytes = r->size};
        total += r->size;
        h = -N_ERR_INVALID;
    }
    h = n_shared_compose(parts, made, access);
    if (h >= 0) { *view_offset = page_offset; *bytes = total; }
done:
    for (size_t i = 0; i < made; i++) n_handle_close(parts[i].shared);
    free(parts);
    return h;
}
