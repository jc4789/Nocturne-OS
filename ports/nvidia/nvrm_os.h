/* Nocturne OS layer for NVIDIA RM (/bin/nvrm): shared declarations.
 *
 * RM itself (third_party/nvidia-open) is OS neutral and calls the os_* / nv_* functions
 * implemented in os.c, os_pci.c, os_stubs.c, nvlink_os.c and spdm_stubs.c. This header holds
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
    char name[128];                  /* rm_get_device_name text */
};

extern struct nvrm_device nvrm_dev;

/* The control "device" RM expects for clients without a GPU (nv_get_ctl_state). */
extern nv_state_t nvrm_ctl_state;

/* ------------------------------------------------------------------------------------------
 * Logging (os.c): system log (n_system_log_open) and /data/logs/nvrm.log when /data exists
 * ---------------------------------------------------------------------------------------- */

void nvrm_log_init(void);
void nvrm_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void nvrm_log_raw(const char *text, size_t length);

/* ------------------------------------------------------------------------------------------
 * Per-thread context
 * ---------------------------------------------------------------------------------------- */

/* Set by the interrupt thread: os_is_isr() is true while it runs RM. */
extern thread_local bool nvrm_in_isr;

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
/* Private tokens are kept by RM until explicit or implicit user unmap. */
void *nvrm_user_mapping_new(void);
void *nvrm_user_mapping_current(void);
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
uint32_t nvrm_interrupt_count(void); /* vectors enabled (0 before nvrm_irq_enable) */
int nvrm_irq_enable(void);           /* n_pci_interrupts; returns the number of vectors */

/* os.c */
uint64_t nvrm_now_ns(void);          /* monotonic, TSC based when the system offers it */
void nvrm_timers_init(void);
bool nvrm_timers_start(void);        /* the timer thread runs; false when it cannot be started */
void nvrm_timers_stop(void);         /* stops and joins it; armed timers never fire after */
void nvrm_work_init(void);
void nvrm_work_stop(void);           /* runs what is queued, stops and waits for the workers */
struct os_work_queue *nvrm_work_queue(void);

/* The interrupt thread (nvrm.c): MSI/MSI-X delivery, or a polling loop when the system gives no
 * vector. start returns false when no thread runs; stop joins it. */
bool nvrm_irq_thread_start(void);
void nvrm_irq_thread_stop(void);

#endif
