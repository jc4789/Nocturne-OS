/* NVIDIA RM on Nocturne: what the original's nv.c answers itself of a client's request
 * (nvidia_ioctl, nv_validate_ioctl_data, nvidia_read_card_info), and the GPU open counts of
 * clients outside RM (nvidia_dev_get, nvidia_dev_put).
 *
 * The original has two kinds of file: the control device every client opens, and one device
 * file per GPU, opened for the requests RM takes only on a GPU (NV_ACTUAL_DEVICE_ONLY). A
 * client of nvrm is both at once: nvrm serves one GPU, and the connection is the client's
 * control file and that GPU's file (rm_api.c sends RM's GPU-only requests with the GPU's state).
 * So the "control device only" and "GPU only" checks of the original never refuse anything
 * here; every request is valid on the one file there is.
 *
 * Every request is checked first, as nvidia_ioctl checks it before anything else: its size
 * must be the size of the escape's argument (an array of them for the array escapes), and an
 * escape neither RM nor this file knows is refused before it reaches RM.
 *
 * Results are NV_STATUS values (the reply's rm_status), where the original returns an errno.
 */
#include "nvrm_os.h"

#include <stdlib.h>
#include <string.h>

#include <nv-ioctl.h>

/* ---------------------------------------------------------------- the size check */
static const struct {
    uint32_t escape;
    uint32_t size;
    bool array;      /* any non-zero number of arguments of that size */
} nv_escapes[] = {
    { NV_ESC_CHECK_VERSION_STR, sizeof(nv_ioctl_rm_api_version_t), false },
    { NV_ESC_IOCTL_XFER_CMD, sizeof(nv_ioctl_xfer_t), false },
    { NV_ESC_ATTACH_GPUS_TO_FD, sizeof(NvU32), true },
    { NV_ESC_CARD_INFO, sizeof(nv_ioctl_card_info_t), true },
    { NV_ESC_QUERY_DEVICE_INTR, sizeof(nv_ioctl_query_device_intr_t), false },
    { NV_ESC_SYS_PARAMS, sizeof(nv_ioctl_sys_params_t), false },
    { NV_ESC_EXPORT_TO_DMABUF_FD, sizeof(nv_ioctl_export_to_dma_buf_fd_t), false },
    { NV_ESC_WAIT_OPEN_COMPLETE, sizeof(nv_ioctl_wait_open_complete_t), false },
    { NV_ESC_NUMA_INFO, sizeof(nv_ioctl_numa_info_t), false },
    { NV_ESC_SET_NUMA_STATUS, sizeof(nv_ioctl_set_numa_status_t), false },
};

static NV_STATUS validate(uint32_t escape, uint32_t size) {
    /* The original's command is the 8-bit number of the ioctl; RM's checks mask it so. A larger
       one would pass as another escape's number. */
    if (escape > 0xFF) {
        nvrm_log("NVRM: unknown NVRM escape 0x%x", (unsigned)escape);
        return NV_ERR_INVALID_ARGUMENT;
    }
    NV_STATUS status = rm_validate_ioctls(escape, size);
    if (status != NV_ERR_INVALID_COMMAND) return status;
    for (size_t i = 0; i < sizeof nv_escapes / sizeof nv_escapes[0]; i++) {
        if (nv_escapes[i].escape != escape) continue;
        bool ok = nv_escapes[i].array ? size && size % nv_escapes[i].size == 0 : size == nv_escapes[i].size;
        if (ok) return NV_OK;
        nvrm_log("NVRM: escape %u: invalid argument size %u, expected %s%u", (unsigned)escape, (unsigned)size,
                 nv_escapes[i].array ? "a multiple of " : "", (unsigned)nv_escapes[i].size);
        return NV_ERR_INVALID_ARGUMENT;
    }
    nvrm_log("NVRM: unknown NVRM escape 0x%x", (unsigned)escape);
    return NV_ERR_INVALID_ARGUMENT;
}

/* ---------------------------------------------------------------- GPU open counts */
/* Clients outside RM (a client's NV_ESC_ATTACH_GPUS_TO_FD, NVKMS, UVM) keep the GPU open. The
   original's first open also starts the adapter; nvrm starts it once, before it serves anyone,
   and a GPU that failed to start is not started again, as the original refuses opens after a
   failed probe-time start. What remains is the count RM keeps of them, which holds off a GPU
   reset while any is attached (unless the client copes with resets itself). */
static mtx_t opens_lock;
static once_flag opens_once = ONCE_FLAG_INIT;
static size_t gpu_opens;
static void opens_setup(void) { mtx_init(&opens_lock, mtx_plain); }

NV_STATUS nvrm_gpu_get(NvU32 gpu_id, NvBool reset_aware) {
    nv_state_t *nv = &nvrm_dev.nv;
    if (!gpu_id || gpu_id != nv->gpu_id || NV_IS_DEVICE_IN_SURPRISE_REMOVAL(nv)) return NV_ERR_INVALID_DEVICE;
    call_once(&opens_once, opens_setup);
    mtx_lock(&opens_lock);
    NV_STATUS status = nvrm_dev.adapter_ready ? NV_OK : NV_ERR_INVALID_STATE;
    if (status == NV_OK) {
        gpu_opens++;
        if (!reset_aware && rm_set_external_kernel_client_count(nvrm_dev.sp, nv, NV_TRUE) != NV_OK)
            nvrm_log("NVRM: WARNING: counting an external client of GPU 0x%x failed", (unsigned)gpu_id);
    }
    mtx_unlock(&opens_lock);
    return status;
}

void nvrm_gpu_put(NvU32 gpu_id, NvBool reset_aware) {
    nv_state_t *nv = &nvrm_dev.nv;
    if (!gpu_id || gpu_id != nv->gpu_id) return;
    call_once(&opens_once, opens_setup);
    mtx_lock(&opens_lock);
    if (gpu_opens) {
        gpu_opens--;
        if (!reset_aware && rm_set_external_kernel_client_count(nvrm_dev.sp, nv, NV_FALSE) != NV_OK)
            nvrm_log("NVRM: WARNING: uncounting an external client of GPU 0x%x failed", (unsigned)gpu_id);
    }
    mtx_unlock(&opens_lock);
}

/* ---------------------------------------------------------------- the escapes */
/* nvidia_read_card_info: the GPUs there are, which is the one nvrm serves (unless RM excluded
   it). The array is cleared first, and too short an array is refused after that, as there. */
static NV_STATUS card_info(nv_ioctl_card_info_t *ci, size_t entries) {
    const nv_state_t *nv = &nvrm_dev.nv;
    memset(ci, 0, entries * sizeof *ci);
    if (entries < 1) return NV_ERR_INVALID_ARGUMENT;
    if (nv->flags & NV_FLAG_EXCLUDE) return NV_OK;
    ci[0].valid = NV_TRUE;
    ci[0].pci_info.domain = nv->pci_info.domain;
    ci[0].pci_info.bus = nv->pci_info.bus;
    ci[0].pci_info.slot = nv->pci_info.slot;
    ci[0].pci_info.vendor_id = nv->pci_info.vendor_id;
    ci[0].pci_info.device_id = nv->pci_info.device_id;
    ci[0].gpu_id = nv->gpu_id;
    ci[0].interrupt_line = (NvU16)nv->interrupt_line;
    ci[0].reg_address = nv->regs ? nv->regs->cpu_address : 0;
    ci[0].reg_size = nv->regs ? nv->regs->size : 0;
    ci[0].fb_address = nv->fb ? nv->fb->cpu_address : 0;
    ci[0].fb_size = nv->fb ? nv->fb->size : 0;
    ci[0].minor_number = 0;
    return NV_OK;
}

/* The GPUs a client attaches its file to stay open until it detaches by closing. Once per file;
   an id of 0 is a free slot. Requests of one client never run at once (one thread serves it),
   so the list needs no lock of its own. */
static NV_STATUS attach_gpus(struct nvrm_gpu_attachments *attached, const NvU32 *ids, size_t count) {
    if (!count || attached->count) return NV_ERR_INVALID_ARGUMENT;
    NvU32 *copy = malloc(count * sizeof *copy);
    if (!copy) return NV_ERR_NO_MEMORY;
    memcpy(copy, ids, count * sizeof *copy);
    for (size_t i = 0; i < count; i++) {
        if (!copy[i]) continue;
        if (nvrm_gpu_get(copy[i], NV_FALSE) != NV_OK) {
            while (i--)
                if (copy[i]) nvrm_gpu_put(copy[i], NV_FALSE);
            free(copy);
            return NV_ERR_INVALID_ARGUMENT;
        }
    }
    attached->ids = copy;
    attached->count = count;
    return NV_OK;
}

void nvrm_nv_file_close(struct nvrm_gpu_attachments *attached) {
    if (!attached) return;
    for (size_t i = 0; i < attached->count; i++)
        if (attached->ids[i]) nvrm_gpu_put(attached->ids[i], NV_FALSE);
    free(attached->ids);
    attached->ids = NULL;
    attached->count = 0;
}

/* The block size NUMA onlining uses, set once by the first client that tells it. */
static uint64_t numa_memblock_size;

bool nvrm_nv_ioctl(struct nvrm_gpu_attachments *attached, uint32_t escape, void *args, uint32_t size,
                   NV_STATUS *status) {
    nv_state_t *nv = &nvrm_dev.nv;
    *status = validate(escape, size);
    if (*status != NV_OK) return true;
    if (NV_IS_DEVICE_IN_SURPRISE_REMOVAL(nv)) {
        *status = NV_ERR_GPU_IS_LOST;
        return true;
    }
    switch (escape) {
    case NV_ESC_WAIT_OPEN_COMPLETE: {
        /* The original's GPU open may finish in the background; here the adapter started (or
           failed to) before nvrm served anyone. nvrm.c logs why a start failed and keeps only
           whether it succeeded. */
        nv_ioctl_wait_open_complete_t *p = args;
        p->rc = nvrm_dev.adapter_ready ? 0 : -1;
        p->adapterStatus = nvrm_dev.adapter_ready ? NV_OK : NV_ERR_INVALID_STATE;
        *status = NV_OK;
        return true;
    }
    case NV_ESC_IOCTL_XFER_CMD:
        /* The original's ioctl number holds at most 16 KiB of argument size, and XFER points at
           a larger argument elsewhere in the caller's memory. A request here carries its whole
           argument in the message (nvrm_protocol.h), whatever its size: there is nothing to
           point at, and no client of the protocol sends it. */
        *status = NV_ERR_NOT_SUPPORTED;
        return true;
    case NV_ESC_QUERY_DEVICE_INTR: {
        nv_ioctl_query_device_intr_t *p = args;
        if (!nv->regs || !nv->regs->map) {
            *status = NV_ERR_INVALID_STATE;
            return true;
        }
        p->intrStatus = *(volatile NvU32 *)(nv->regs->map + (NV_RM_DEVICE_INTR_ADDRESS >> 2));
        p->status = NV_OK;
        *status = NV_OK;
        return true;
    }
    case NV_ESC_CARD_INFO:
        *status = card_info(args, size / sizeof(nv_ioctl_card_info_t));
        return true;
    case NV_ESC_ATTACH_GPUS_TO_FD:
        *status = attach_gpus(attached, args, size / sizeof(NvU32));
        return true;
    case NV_ESC_CHECK_VERSION_STR:
        *status = rm_perform_version_check(nvrm_dev.sp, args, size) == NV_OK ? NV_OK : NV_ERR_INVALID_ARGUMENT;
        return true;
    case NV_ESC_SYS_PARAMS: {
        const nv_ioctl_sys_params_t *p = args;
        uint64_t unset = 0;
        if (__atomic_compare_exchange_n(&numa_memblock_size, &unset, p->memblock_size, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            *status = NV_OK;
        else
            *status = unset == p->memblock_size ? NV_OK : NV_ERR_IN_USE;
        return true;
    }
    case NV_ESC_NUMA_INFO: {
        nv_ioctl_numa_info_t *p = args;
        if (rm_get_gpu_numa_info(nvrm_dev.sp, nv, p) != NV_OK) {
            *status = NV_ERR_IN_USE;
            return true;
        }
        /* No NUMA node holds the GPU's memory: it is not coherent system memory on a PCIe GPU,
           and Nocturne gives memory no NUMA nodes. */
        p->status = NV_IOCTL_NUMA_STATUS_DISABLED;
        p->use_auto_online = 0;
        p->memblock_size = __atomic_load_n(&numa_memblock_size, __ATOMIC_ACQUIRE);
        *status = NV_OK;
        return true;
    }
    case NV_ESC_SET_NUMA_STATUS: {
        const nv_ioctl_set_numa_status_t *p = args;
        if (!nvrm_thread_admin) {
            *status = NV_ERR_INSUFFICIENT_PERMISSIONS;
            return true;
        }
        /* Without a NUMA node the status is DISABLED and stays so; the original fails setting
           any other (nv_set_numa_status), after offlining fails the same way for a GPU whose
           memory was never onlined. */
        *status = p->status == NV_IOCTL_NUMA_STATUS_DISABLED ? NV_OK : NV_ERR_INVALID_ARGUMENT;
        return true;
    }
    case NV_ESC_EXPORT_TO_DMABUF_FD: {
        /* nv-dmabuf: a reply carries a shared object only for NV_ESC_RM_MAP_MEMORY
           (nvrm_protocol.h), so an exported object could not reach the client. */
        nv_ioctl_export_to_dma_buf_fd_t *p = args;
        p->status = NV_ERR_NOT_SUPPORTED;
        *status = NV_OK;
        return true;
    }
    default:
        return false; /* RM's (rm_ioctl) */
    }
}
