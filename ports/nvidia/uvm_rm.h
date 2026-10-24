/* RM's side of nvrm's UVM OS layer: what NVIDIA's kernel-open/nvidia/nv_uvm_interface.c (RM's
 * half of the RM-UVM interface, nvidia.ko's on Linux) is compiled against in nvrm, in the place
 * of nv-linux.h. It is RM's OS layer (nvrm_os.h) plus the few kernel words that file uses;
 * uvm_rm.c implements the rest (nvidia_dev_*, on_each_cpu) and the bridges UVM's own OS layer
 * needs from RM (uvm_nvrm.h).
 */
#ifndef NVRM_UVM_RM_H
#define NVRM_UVM_RM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "nvrm_os.h"
#include "os-interface.h"
#include "uvm_shared.h"
#include "uvm_nvrm.h"

/* RM's altstack: nvrm has one shared dummy (no __use_altstack__, NV_ENTER_RM_RUNTIME does not
   switch stacks), which every thread passes, as RM's own threads do. */
static inline int nv_kmem_cache_alloc_stack(nvidia_stack_t **stack)
{
    *stack = nvrm_dev.sp;
    return nvrm_dev.sp ? 0 : -ENOMEM;
}
static inline void nv_kmem_cache_free_stack(nvidia_stack_t *stack) { (void)stack; }

/* nv-lock.h's mutex is a binary semaphore. */
#define NV_INIT_MUTEX(sem) sema_init((sem), 1)

/* Every thread of nvrm may sleep except the interrupt thread while it runs a top half. */
#define NV_MAY_SLEEP() (!nvrm_in_isr)

#define WARN_ON(cond) ({                                                                   \
        bool _warn = !!(cond);                                                             \
        if (__builtin_expect(_warn, 0))                                                    \
            nvrm_log("nvrm: WARNING at %s:%d: %s", __FILE__, __LINE__, #cond);             \
        _warn;                                                                             \
    })

/* nvidia.ko's exports are ordinary functions of nvrm. */
#define EXPORT_SYMBOL(sym)

/* Runs fn(info) once every top half that started on any CPU before the call has ended (how
   nvUvmInterfaceDeRegisterUvmEvents waits for top halves still using UVM's callbacks; the
   count is nvrm_uvm_interrupt's). wait is always honoured. */
void on_each_cpu(void (*fn)(void *info), void *info, int wait);

/* What nv-linux.h's nvidia.ko gives the interface about its GPUs (nv.c on Linux), for the GPU
   nvrm serves. 0 or a negative errno. */
int nvidia_dev_get_uuid(const NvU8 *uuid, nvidia_stack_t *sp);
void nvidia_dev_put_uuid(const NvU8 *uuid, nvidia_stack_t *sp);
int nvidia_dev_block_gc6(const NvU8 *uuid, nvidia_stack_t *sp);
int nvidia_dev_unblock_gc6(const NvU8 *uuid, nvidia_stack_t *sp);
int nvidia_dev_get_pci_info(const NvU8 *uuid, struct pci_dev **pci_dev_out, NvU64 *dma_start, NvU64 *dma_limit);

/* The system's ATS state (nv.c on Linux): no ATS here; set by nvrm_uvm_start from RM's view. */
extern NvBool nv_ats_supported;
extern NvBool nv_non_ats_device_present;

/* NVreg_EnableUserNUMAManagement (nv-reg.h, defined in os.c's registry table). */
extern NvU32 NVreg_EnableUserNUMAManagement;

/* RM's half of the interface (nv_uvm_interface.c), as nvidia.ko's module start calls it. */
int nv_uvm_init(void);
void nv_uvm_exit(void);
NV_STATUS nv_uvm_event_interrupt(const NvU8 *uuid);

#endif
