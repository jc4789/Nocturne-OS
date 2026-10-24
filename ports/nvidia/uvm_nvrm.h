/* The boundary between NVIDIA UVM's OS layer in nvrm and the rest of nvrm.
 *
 * UVM's side (uvm_os.c, uvm_mm.c, uvm_serve.c) is compiled against uvm_os.h, the vocabulary
 * UVM's sources are written in; RM's side (uvm_rm.c, nv_uvm_interface.c) against nvrm_os.h and
 * RM's headers. Neither header can be included with the other, so what crosses between them is
 * declared here in plain types, and uvm_rm.c, which includes both this header and nvrm_os.h,
 * makes the compiler check the declarations repeated from nvrm_os.h against the originals.
 *
 * ports/nvidia/README.md ("UVM") describes the design.
 */
#ifndef NVRM_UVM_NVRM_H
#define NVRM_UVM_NVRM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nocturne.h"
#include "nvtypes.h"
#include "nvstatus.h"

/* ------------------------------------------------------------------------------------------
 * For nvrm.c and the RM OS layer (uvm_rm.c)
 * ---------------------------------------------------------------------------------------- */

/* UVM's start, once RM started the GPU (nvrm_dev.adapter_ready) and before clients are served:
   RM's half of the interface (nv_uvm_init), then UVM itself (its module init, which registers
   its interrupt and power callbacks with RM). false when UVM is not running: the reason is in
   the log, and RM and the screen are served all the same. */
bool nvrm_uvm_start(void);
/* UVM's stop, after the request server ended (every client's UVM state is gone then) and while
   RM still runs the GPU and its interrupt thread: before rm_disable_adapter. */
void nvrm_uvm_stop(void);
/* Whether UVM runs (between a successful nvrm_uvm_start and nvrm_uvm_stop). */
bool nvrm_uvm_running(void);
/* A device interrupt's UVM top half (nv_uvm_event_interrupt for the GPU nvrm serves): called
   by the interrupt thread before rm_isr, after rm_gpu_handle_mmu_faults, and by
   nv_schedule_uvm_isr. true when the interrupt was UVM's. Callers must use this function rather
   than nv_uvm_event_interrupt: it counts the top halves running, which is how UVM's
   unregistration (on_each_cpu, uvm_rm.c) knows that none of them still uses its callbacks. */
bool nvrm_uvm_interrupt(void);
/* RM's power path calls nv_uvm_suspend / nv_uvm_resume and its P2P path nv_uvm_drain_P2P /
   nv_uvm_resume_P2P (kernel-open/nvidia/nv_uvm_interface.c) directly. */
NV_STATUS nv_uvm_suspend(void);
NV_STATUS nv_uvm_resume(void);
NV_STATUS nv_uvm_drain_P2P(const NvU8 *uuid);
NV_STATUS nv_uvm_resume_P2P(const NvU8 *uuid);

/* ------------------------------------------------------------------------------------------
 * For the request server (nvrm_serve.c): the NVRM_UVM escape (include/nvrm_protocol.h)
 * ---------------------------------------------------------------------------------------- */

/* One client connection's UVM state: its files, its mappings of UVM memory and the thread
   that serves their faults. Made by its first NVRM_UVM request. */
struct nvrm_uvm_client;
/* Serves one NVRM_UVM request of the connection whose state is *client (NULL until its first),
   from the app `principal`. args/size are the request's struct nvrm_uvm_request; pointers in
   UVM's parameter blocks are translated with nvrm_user_translate on this thread, as for RM's
   escapes. *attach gets a handle to send with the reply (-1: none); it stays UVM's, so the
   server sends it without closing it. Returns 0 or a negative n_result for a malformed request;
   UVM's own result is the request's status. */
n_result nvrm_uvm_request(struct nvrm_uvm_client **client, uint32_t principal, void *args, uint32_t size,
                          n_handle *attach);
/* The connection ended: its mappings are taken down as at an app's exit, its files closed.
   Called before RM frees the client's RM state, which UVM's state refers to. */
void nvrm_uvm_client_end(struct nvrm_uvm_client *client);

/* ------------------------------------------------------------------------------------------
 * From the RM side for UVM's OS layer (uvm_rm.c)
 * ---------------------------------------------------------------------------------------- */

/* System memory for UVM's pages: one physically contiguous run of `pages` 4 KiB pages of DMA
   memory of the GPU's claim (RM's nv_alloc_pages), zeroed and write-back cached. */
struct nvrm_uvm_memory {
    void *rm;                 /* RM's allocation */
    void *kmap;               /* RM's kernel-mapping token */
    void *cpu;                /* nvrm's mapping of the whole run */
    uint64_t bus;             /* the bus address of its first page */
    n_handle view;            /* a read/write write-back view of the whole run (pager source) */
};
/* dma32: every page below 4 GiB. false when there is not that much memory. */
bool nvrm_uvm_memory_alloc(uint64_t pages, bool dma32, struct nvrm_uvm_memory *out);
void nvrm_uvm_memory_free(struct nvrm_uvm_memory *memory);

/* The PCI function nvrm serves, as UVM's struct pci_dev describes it. */
struct nvrm_uvm_pci {
    uint16_t vendor, device, subsystem_vendor, subsystem_device;
    uint8_t bus, dev, func;
    uint64_t dma_mask;
    struct { uint64_t address, size; bool is_64bit; } bar[6];
};
void nvrm_uvm_pci_info(struct nvrm_uvm_pci *out);

/* nvrm's mapping of [phys, phys + bytes) of a BAR (write-back where the system allows it,
   uncached otherwise); NULL outside the BARs. */
void *nv_ioremap_cache(NvU64 phys, NvU64 bytes);
void nv_iounmap(void *address, NvU64 bytes);

/* ------------------------------------------------------------------------------------------
 * From UVM's OS layer for the RM side
 * ---------------------------------------------------------------------------------------- */

struct pci_dev;
/* UVM's description of the GPU (uvm_mm.c), filled from nvrm_uvm_pci_info. */
struct pci_dev *uvm_os_pci_dev(void);
/* The module entry points of nvidia-uvm.o (module_init / module_exit in uvm.c). */
int nvrm_uvm_module_init(void);
void nvrm_uvm_module_exit(void);
/* UVM's module parameters from nvrm's launch setting UVM_OPTIONS ("name=value;..."), before
   the module starts (uvm_os.c). */
void uvm_os_params_init(void);
/* UVM's own threads (deferred work) are stopped: after nvrm_uvm_module_exit (uvm_os.c). */
void uvm_os_stop(void);

/* ------------------------------------------------------------------------------------------
 * nvrm's own, repeated from nvrm_os.h (uvm_rm.c checks them against it)
 * ---------------------------------------------------------------------------------------- */

void nvrm_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
uint64_t nvrm_now_ns(void);
extern _Thread_local bool nvrm_in_isr;
extern _Thread_local uint32_t nvrm_thread_process;

#endif
