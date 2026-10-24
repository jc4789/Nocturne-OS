/* RM's side of nvrm's UVM OS layer (uvm_rm.h): what nvidia.ko's nv.c gives NVIDIA's RM-UVM
 * interface (kernel-open/nvidia/nv_uvm_interface.c) about the GPU, and the bridges UVM's own
 * OS layer (uvm_os.c, uvm_mm.c, uvm_serve.c) needs from RM (uvm_nvrm.h): system memory for its
 * pages, the PCI function, BAR mappings, the interrupt top half and UVM's start and stop.
 *
 * This file includes both nvrm_os.h and uvm_nvrm.h, so the compiler checks the declarations
 * uvm_nvrm.h repeats from nvrm_os.h against the originals.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uvm_rm.h"
#include "nv_uvm_interface.h"

/* The pages of RM's allocations and of the BARs. */
#define PAGE 4096u

NvBool nv_ats_supported;
NvBool nv_non_ats_device_present;

/* nvrm_uvm_start succeeded and no stop since: written by nvrm.c's thread before clients are
   served and after they ended, read by the client threads. */
static bool running;

/* ------------------------------------------------------------------------------------------
 * The GPU nvrm serves, as nv.c's nvidia_dev_* describe a GPU to the interface
 * ---------------------------------------------------------------------------------------- */

static bool is_our_gpu(const NvU8 *uuid)
{
    const NvU8 *ours = nv_get_cached_uuid(&nvrm_dev.nv);
    return uuid && ours && memcmp(uuid, ours, NV_UUID_LEN) == 0;
}

int nvidia_dev_get_uuid(const NvU8 *uuid, nvidia_stack_t *sp)
{
    (void)sp;
    if (!is_our_gpu(uuid))
        return -ENODEV;
    /* UVM is a client like NVKMS that holds off a GPU reset while it uses the GPU. */
    NV_STATUS status = nvrm_gpu_get(nvrm_dev.nv.gpu_id, NV_FALSE);
    if (status == NV_OK)
        return 0;
    return status == NV_ERR_NO_MEMORY ? -ENOMEM : -ENODEV;
}

void nvidia_dev_put_uuid(const NvU8 *uuid, nvidia_stack_t *sp)
{
    (void)sp;
    if (is_our_gpu(uuid))
        nvrm_gpu_put(nvrm_dev.nv.gpu_id, NV_FALSE);
}

/* GC6 (the GPU powered off while idle) stays off while UVM uses the GPU: a fine-grained
   dynamic power reference, as nv.c takes one. */
int nvidia_dev_block_gc6(const NvU8 *uuid, nvidia_stack_t *sp)
{
    if (!is_our_gpu(uuid))
        return -ENODEV;
    return rm_ref_dynamic_power(sp, &nvrm_dev.nv, NV_DYNAMIC_PM_FINE) == NV_OK ? 0 : -EINVAL;
}

int nvidia_dev_unblock_gc6(const NvU8 *uuid, nvidia_stack_t *sp)
{
    if (!is_our_gpu(uuid))
        return -ENODEV;
    rm_unref_dynamic_power(sp, &nvrm_dev.nv, NV_DYNAMIC_PM_FINE);
    return 0;
}

int nvidia_dev_get_pci_info(const NvU8 *uuid, struct pci_dev **pci_dev_out, NvU64 *dma_start, NvU64 *dma_limit)
{
    if (!is_our_gpu(uuid))
        return -ENODEV;
    *pci_dev_out = uvm_os_pci_dev();
    /* No IOMMU between the GPU and memory: its DMA addresses are bus addresses up to its mask. */
    *dma_start = 0;
    *dma_limit = nvrm_dev.nv.dma_mask;
    return 0;
}

void nvrm_uvm_pci_info(struct nvrm_uvm_pci *out)
{
    const struct n_pci_function *f = &nvrm_dev.info;
    memset(out, 0, sizeof *out);
    out->vendor = f->vendor;
    out->device = f->device;
    out->subsystem_vendor = f->subsystem_vendor;
    out->subsystem_device = f->subsystem_id;
    out->bus = f->bus;
    out->dev = f->dev;
    out->func = f->func;
    out->dma_mask = nvrm_dev.nv.dma_mask;
    for (unsigned i = 0; i < 6; i++) {
        if (!(f->bar[i].flags & N_PCI_BAR_MEMORY))
            continue;
        out->bar[i].address = f->bar[i].address;
        out->bar[i].size = f->bar[i].size;
        out->bar[i].is_64bit = (f->bar[i].flags & N_PCI_BAR_64BIT) != 0;
    }
}

/* ------------------------------------------------------------------------------------------
 * System memory for UVM's pages: RM's own allocator for the GPU (os_pci.c)
 * ---------------------------------------------------------------------------------------- */

bool nvrm_uvm_memory_alloc(uint64_t pages, bool dma32, struct nvrm_uvm_memory *out)
{
    memset(out, 0, sizeof *out);
    out->view = -1;
    if (!pages || pages > UINT32_MAX)
        return false;
    NvU64 first;
    void *rm = NULL;
    /* One contiguous run, so that a struct page array describes it as UVM expects of
       alloc_pages; write-back cached, as the kernel's pages are. */
    NV_STATUS status = nv_alloc_pages(&nvrm_dev.nv, (NvU32)pages, PAGE, NV_TRUE, NV_MEMORY_CACHED,
                                      NV_TRUE, NV_FALSE, -1, &first, &rm);
    if (status != NV_OK)
        return false;
    uint64_t bytes = pages * PAGE;
    if (dma32 && (first > UINT32_MAX || bytes > (uint64_t)UINT32_MAX + 1 - first)) {
        /* RM allocates below 4 GiB only for a GPU that needs it (nv->dma_mask); UVM asks for it
           itself only for such a GPU too, so this does not happen with the GPU nvrm serves. */
        nv_free_pages(&nvrm_dev.nv, (NvU32)pages, NV_TRUE, NV_MEMORY_CACHED, rm);
        return false;
    }
    void *kmap = NULL;
    void *cpu = nv_alloc_kernel_mapping(&nvrm_dev.nv, rm, 0, 0, bytes, &kmap);
    if (!cpu) {
        nv_free_pages(&nvrm_dev.nv, (NvU32)pages, NV_TRUE, NV_MEMORY_CACHED, rm);
        return false;
    }
    n_handle view = nvrm_alloc_view(rm, 0, bytes, NV_MEMORY_CACHED, N_MEMORY_READ_WRITE);
    if (view < 0) {
        nv_free_kernel_mapping(&nvrm_dev.nv, rm, cpu, kmap);
        nv_free_pages(&nvrm_dev.nv, (NvU32)pages, NV_TRUE, NV_MEMORY_CACHED, rm);
        return false;
    }
    out->rm = rm;
    out->kmap = kmap;
    out->cpu = cpu;
    out->bus = first;
    out->view = view;
    return true;
}

void nvrm_uvm_memory_free(struct nvrm_uvm_memory *memory)
{
    if (!memory->rm)
        return;
    uint64_t pages = nvrm_alloc_size(memory->rm) / PAGE;
    n_handle_close(memory->view);
    nv_free_kernel_mapping(&nvrm_dev.nv, memory->rm, memory->cpu, memory->kmap);
    nv_free_pages(&nvrm_dev.nv, (NvU32)pages, NV_TRUE, NV_MEMORY_CACHED, memory->rm);
    memset(memory, 0, sizeof *memory);
    memory->view = -1;
}

/* ------------------------------------------------------------------------------------------
 * BAR mappings in nvrm (UVM maps GPU memory that the CPU reaches coherently this way)
 * ---------------------------------------------------------------------------------------- */

void *nv_ioremap_cache(NvU64 phys, NvU64 bytes)
{
    uint64_t offset;
    n_handle view = nvrm_bar_view(phys, bytes, NV_MEMORY_CACHED, N_MEMORY_READ_WRITE, &offset);
    if (view < 0)
        return NULL;
    uint64_t span = (offset + bytes + PAGE - 1) & ~(uint64_t)(PAGE - 1);
    void *base = NULL;
    n_result mapped = n_shared_map(view, (size_t)span, N_MEMORY_READ_WRITE, &base);
    n_handle_close(view);
    return mapped < 0 ? NULL : (char *)base + offset;
}

void nv_iounmap(void *address, NvU64 bytes)
{
    uintptr_t start = (uintptr_t)address & ~(uintptr_t)(PAGE - 1);
    uint64_t span = ((uintptr_t)address - start + bytes + PAGE - 1) & ~(uint64_t)(PAGE - 1);
    if (address)
        n_memory_release((void *)start, (size_t)span);
}

/* ------------------------------------------------------------------------------------------
 * The interrupt top half
 * ---------------------------------------------------------------------------------------- */

/* Top halves running nv_uvm_event_interrupt; woken by address when it drops to zero. */
static volatile uint32_t top_halves;

bool nvrm_uvm_interrupt(void)
{
    __atomic_add_fetch(&top_halves, 1, __ATOMIC_SEQ_CST);
    const NvU8 *uuid = nv_get_cached_uuid(&nvrm_dev.nv);
    NV_STATUS status = uuid ? nv_uvm_event_interrupt(uuid) : NV_ERR_NO_INTR_PENDING;
    if (__atomic_sub_fetch(&top_halves, 1, __ATOMIC_SEQ_CST) == 0)
        wake_address(&top_halves, 0);
    return status == NV_OK;
}

void on_each_cpu(void (*fn)(void *info), void *info, int wait)
{
    (void)wait;
    for (;;) {
        uint32_t running_now = __atomic_load_n(&top_halves, __ATOMIC_SEQ_CST);
        if (!running_now)
            break;
        wait_on_address(&top_halves, running_now, UINT32_MAX);
    }
    fn(info);
}

/* ------------------------------------------------------------------------------------------
 * UVM's start and stop (nvidia.ko's nv_uvm_init, then nvidia-uvm.ko's module init)
 * ---------------------------------------------------------------------------------------- */

bool nvrm_uvm_running(void)
{
    return __atomic_load_n(&running, __ATOMIC_ACQUIRE);
}

bool nvrm_uvm_start(void)
{
    if (running)
        return true;
    if (!nvrm_dev.adapter_ready) {
        nvrm_log("nvrm: UVM not started: the GPU did not start");
        return false;
    }
    nv_ats_supported = nvrm_dev.nv.ats_support;
    nv_non_ats_device_present = !nvrm_dev.nv.ats_support;
    uvm_os_params_init();
    int rc = nv_uvm_init();
    if (rc != 0) {
        nvrm_log("nvrm: UVM not started: RM's interface (%d)", rc);
        return false;
    }
    rc = nvrm_uvm_module_init();
    if (rc != 0) {
        uvm_os_stop();
        nv_uvm_exit();
        nvrm_log("nvrm: UVM not started: nvidia-uvm's start failed (%d)", rc);
        return false;
    }
    __atomic_store_n(&running, true, __ATOMIC_RELEASE);
    nvrm_log("nvrm: UVM started (nvidia-uvm " NVRM_FIRMWARE_VERSION ")");
    return true;
}

void nvrm_uvm_stop(void)
{
    if (!running)
        return;
    nvrm_uvm_module_exit();
    uvm_os_stop();
    nv_uvm_exit();
    __atomic_store_n(&running, false, __ATOMIC_RELEASE);
    nvrm_log("nvrm: UVM stopped");
}
