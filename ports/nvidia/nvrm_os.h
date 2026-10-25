/* Nocturne OS layer for NVIDIA RM (nvrm): shared declarations.
 *
 * RM itself (third_party/nvidia-open) is OS neutral and calls the os_* / nv_* functions
 * implemented in os.c, os_pci.c, os_stubs.c, os_caps.c, os_acpi.c, nvlink_os.c and spdm_crypto.c
 * (libspdm's cryptography); os_ioctl.c and rm_api.c take the clients' requests. This header holds
 * what those files share, and the few entry points the request server (nvrm_serve) needs.
 *
 * Nothing here is POSIX; the implementation uses Nocturne's native ABI and ISO C
 * (malloc, <threads.h>, wait_on_address, timespec_get).
 */
#ifndef NVRM_OS_H
#define NVRM_OS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <threads.h>

#include "nocturne.h"
#include "nvrm_serve.h"

#include <nvtypes.h>
#include <nvstatus.h>
#include <nv.h>

#define NVRM_FIRMWARE_VERSION "615.78.08"

/* Handle numbers of an app launched by init: 0 input, 1 output, 2 log, 3 the PCI right. */
#define NVRM_PCI_HANDLE 3

/* ------------------------------------------------------------------------------------------
 * The device (one GPU per nvrm process)
 * ---------------------------------------------------------------------------------------- */

struct nvrm_device {
    n_handle pci;                    /* N_KIND_PCI right; never duplicated */
    struct n_pci_function info;      /* as n_pci_info returned it */
    nv_state_t nv;                   /* what RM sees */
    nvidia_stack_t *sp;              /* RM's (dummy) altstack; no __use_altstack__ here */
    bool adapter_ready;              /* rm_init_adapter succeeded */
    bool bus_master;                 /* nvrm_pci_bus_master(true) and no stop since */
    char name[128];                  /* rm_get_device_name text */
};

extern struct nvrm_device nvrm_dev;

/* The control "device" RM expects for clients without a GPU (nv_get_ctl_state). */
extern nv_state_t nvrm_ctl_state;

/* ------------------------------------------------------------------------------------------
 * Logging (os.c): system log (n_system_log_open) and nvrm.log in AppData\NVIDIA of the user's folder
 * ---------------------------------------------------------------------------------------- */

void nvrm_log_init(void);
void nvrm_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void nvrm_log_raw(const char *text, size_t length);

/* ------------------------------------------------------------------------------------------
 * Per-thread context
 * ---------------------------------------------------------------------------------------- */

/* Set by the interrupt thread (nvrm_isr_begin and nvrm_isr_end, os.c): os_is_isr() is true while
 * it runs RM's interrupt handler, and os_get_cpu_number() says the CPU that run began on. */
extern thread_local bool nvrm_in_isr;
void nvrm_isr_begin(void);
void nvrm_isr_end(void);

/* Whether RM sees the running request as an administrator's (os_is_administrator,
 * os_check_access). nvrm_rm_ioctl sets it to the client's own right for the length of one request
 * and back to true after; threads of the process itself (interrupts, work, timers) are
 * administrators. */
extern thread_local bool nvrm_thread_admin;
/* Kernel-authenticated native app ID during a request; zero for daemon work. */
extern thread_local uint32_t nvrm_thread_process;

/* ------------------------------------------------------------------------------------------
 * The request server (nvrm_serve.c, nvrm_serve.h) calls the nvrm_rm_* functions of rm_api.c
 * and provides nvrm_user_translate, which os_memcpy_from_user / os_memcpy_to_user use.
 * rm_api.c also holds the clients' RM event queues (nv_post_event, nv_get_event).
 * ---------------------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------------------------
 * What nv.c answers itself of a request (os_ioctl.c)
 *
 * rm_api.c hands every request to nvrm_nv_ioctl first: it checks the argument size of every
 * escape and answers the escapes that are not RM's (card info, GPU attachment, version check,
 * system parameters, NUMA, the device interrupt, the open status). It returns false for RM's
 * escapes, which go on to rm_ioctl; otherwise *status is the result.
 * ---------------------------------------------------------------------------------------- */

/* The GPUs a client attached its file to (NV_ESC_ATTACH_GPUS_TO_FD); part of the client. */
struct nvrm_gpu_attachments {
    NvU32 *ids;
    size_t count;
};

bool nvrm_nv_ioctl(struct nvrm_gpu_attachments *attached, uint32_t escape, void *args, uint32_t size,
                   NV_STATUS *status);
/* When the client closes, after RM freed what it held: its GPUs are put. */
void nvrm_nv_file_close(struct nvrm_gpu_attachments *attached);
/* nvidia_dev_get / nvidia_dev_put for clients outside RM (a client's attachment, NVKMS, UVM):
   the GPU must be the one nvrm serves, and started. Unless reset_aware, RM counts the client
   as one that holds off a GPU reset. */
NV_STATUS nvrm_gpu_get(NvU32 gpu_id, NvBool reset_aware);
void nvrm_gpu_put(NvU32 gpu_id, NvBool reset_aware);

/* ------------------------------------------------------------------------------------------
 * Client mappings (os_pci.c)
 *
 * RM calls nv_add_mapping_context_to_file while the server handles a "map this memory to the
 * client" request (rm_create_mmap_context). The context is parked in a per-thread slot; the
 * rm_api.c (nvrm_rm_mapping_object) takes it with nvrm_take_mapping and turns it into a
 * shared object with nvrm_mapping_view, which it sends to the client as a channel attachment.
 * BAR ranges may have disjoint backing; their native views retain their individual cache
 * policy and are composed into one contiguous virtual mapping.
 * ---------------------------------------------------------------------------------------- */

struct nvrm_alloc; /* a system-memory allocation made by nv_alloc_pages; opaque */

struct nvrm_range {
    uint64_t start, size;            /* physical addresses inside a BAR */
};

struct nvrm_mapping {
    void *owner;                    /* RM's per-user-map private lifetime token */
    bool is_alloc;
    uint32_t prot;                   /* NV_PROTECT_* */
    uint32_t caching;                /* NV_MEMORY_* (nvuap->caching) */
    /* is_alloc: system memory */
    struct nvrm_alloc *alloc;
    uint64_t page_index;             /* first 4 KiB page of the allocation */
    uint64_t size;                   /* nvuap->size, bytes */
    /* !is_alloc: BAR memory */
    uint64_t access_start, access_size;
    struct nvrm_range *ranges;       /* owned copy, in the order of the client's virtual pages */
    size_t range_count;
};

/* Moves the parked context of this thread into *out; false when there is none. */
bool nvrm_take_mapping(struct nvrm_mapping *out);
/* Releases the copied ranges, not the RM private lifetime token. */
void nvrm_mapping_release(struct nvrm_mapping *mapping);
/* Forgets a parked context nobody took (a failed request). */
void nvrm_drop_mapping(void);
/* Publish a context as one native object. page_offset is RM's returned address modulo
 * the OS page size. On success, return the final offset and full page-rounded length. */
n_handle nvrm_mapping_view(const struct nvrm_mapping *mapping, uint32_t access, uint64_t page_offset,
                           uint64_t *view_offset, uint64_t *bytes);
/* Private tokens are kept by RM until explicit or implicit user unmap. Each comes with `bytes`
 * addresses no other mapping is named by, from *address on: RM's address for the mapping is
 * one of them (never a bus address, which two mappings of the same memory share). */
void *nvrm_user_mapping_new(uint64_t bytes, uint64_t *address);
void *nvrm_user_mapping_current(void);
/* The addresses of a token from nvrm_user_mapping_new. */
bool nvrm_user_mapping_span(const void *owner, uint64_t *address, uint64_t *bytes);
void nvrm_user_mapping_free(void *owner);

/* A new shared handle over pages of a system-memory allocation (caller closes it or sends
 * it). caching is NV_MEMORY_*. N_CACHE_* is chosen from it. Returns a negative n_result on
 * failure. */
n_handle nvrm_alloc_view(struct nvrm_alloc *alloc, uint64_t page_index, uint64_t bytes,
                         uint32_t nv_caching, uint32_t access);
/* Bytes the allocation holds (page rounded). */
uint64_t nvrm_alloc_size(const struct nvrm_alloc *alloc);

/* A new shared handle over [phys, phys + bytes) of a BAR of the device. phys and bytes are
 * rounded out to pages; *view_offset gets the byte offset of phys inside the view. */
n_handle nvrm_bar_view(uint64_t phys, uint64_t bytes, uint32_t nv_caching, uint32_t access,
                       uint64_t *view_offset);

/* ------------------------------------------------------------------------------------------
 * Pieces shared between the OS layer files
 * ---------------------------------------------------------------------------------------- */

/* os_pci.c */
NV_STATUS nvrm_pci_attach(void);     /* n_pci_info + nv_state_t fields + BAR0 mapping */
void nvrm_pci_detach(void);
/* Bus mastering on before RM starts the adapter; off only once RM has stopped it again
   (rm_shutdown_adapter, or a failed start, which shuts down on its own). The system takes a
   function that ends its claim with bus mastering off as stopped; on, as left running by a
   crashed driver, and keeps the DMA memory it was given out of use. */
bool nvrm_pci_bus_master(bool on);
uint32_t nvrm_interrupt_count(void); /* vectors enabled (0 before nvrm_irq_enable) */
int nvrm_irq_enable(void);           /* n_pci_interrupts; returns the number of vectors */
/* nv-procfs's GPU "information" and "power" pages as log lines; called once the GPU started. */
void nvrm_log_gpu_information(void);

/* os.c */
uint64_t nvrm_now_ns(void);          /* monotonic, TSC based when the system offers it */
void nvrm_timers_init(void);
bool nvrm_timers_start(void);        /* the timer thread runs; false when it cannot be started */
void nvrm_timers_stop(void);         /* stops and joins it; armed timers never fire after */
void nvrm_work_init(void);
void nvrm_work_stop(void);           /* runs what is queued, stops and waits for the workers */
struct os_work_queue *nvrm_work_queue(void);
/* The registry's per-device lists (NVreg_RegistryDwordsPerDevice, NVreg_RegistryBinaryFilePerDevice)
   for this GPU; the original applies them once the GPU's RM state exists (after
   rm_init_private_state), before the adapter starts. */
NV_STATUS nvrm_registry_per_device(nvidia_stack_t *sp, nv_state_t *nv);
/* Whether NVreg_ExcludedGpus (or NVreg_GpuBlacklist) names this GPU UUID: the original then
   excludes the adapter (rm_exclude_adapter) once it knows the UUID. */
bool nvrm_gpu_excluded(const char *uuid);
/* NVreg_ExcludeAllGpus (nv-reg.h, defined in os.c): no GPU is started. */
extern NvU32 NVreg_ExcludeAllGpus;

/* The interrupt thread (nvrm.c): MSI/MSI-X delivery, or a polling loop when the system gives no
 * vector. start returns false when no thread runs; stop joins it. */
bool nvrm_irq_thread_start(void);
void nvrm_irq_thread_stop(void);

/* The GPU node's ACPI Notify values to rm_acpi_notify (os_acpi.c), as the original's notifier
 * (nv_acpi_register_notifier / nv_acpi_unregister_notifier): started once rm_init_adapter
 * succeeded, stopped before rm_disable_adapter, both outside RM's locks. stop joins its thread. */
void nvrm_acpi_notify_start(void);
void nvrm_acpi_notify_stop(void);

#endif
