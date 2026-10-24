/*
 * SPDX-License-Identifier: MIT
 *
 * Nocturne's NVKMD backend: NVK on NVIDIA's resource manager (/bin/nvrm).
 *
 * The shape follows Mesa's nouveau backend (pdev, dev with VA heaps, mem, va,
 * exec and bind contexts) but every kernel call is an RM call through
 * nvkmd_nocturne_rm.h, and synchronisation is a vk_sync type of our own:
 * each exec context owns a fence (a 64-bit semaphore in system memory that its
 * channel releases after each flush) and a sync object is a list of
 * (fence, seqno, value) points.  GPU waits become semaphore acquires in the
 * waiting channel; CPU waits poll the fences.
 */
#ifndef NVKMD_NOCTURNE_H
#define NVKMD_NOCTURNE_H 1

#include "nvkmd/nvkmd.h"
#include "nvkmd_nocturne_rm.h"

#include "util/simple_mtx.h"
#include "util/u_dynarray.h"
#include "util/vma.h"
#include "vk_sync.h"

VkResult nvkmd_nocturne_enumerate_gpus(struct vk_object_base *log_obj,
                                      uint32_t **gpu_ids, uint32_t *count);

VkResult nvkmd_nocturne_try_create_pdev(uint32_t pci_target,
                                        struct vk_object_base *log_obj,
                                        enum nvk_debug debug_flags,
                                        struct nvkmd_pdev **pdev_out);

/* ---- sync ---- */

/* A channel's completion counter: the GPU writes seqno after each flush */
struct nvkmd_nocturne_fence {
   uint32_t refcnt;

   struct nvkmd_mem *mem;
   volatile uint64_t *map;
   uint64_t addr;

   /* The channel, for error checks; NULL once its context is gone (under
    * the sync lock) */
   struct nnrm_chan *chan;
};

struct nvkmd_nocturne_fence *
nvkmd_nocturne_fence_ref(struct nvkmd_nocturne_fence *fence);
void nvkmd_nocturne_fence_unref(struct nvkmd_nocturne_fence *fence);
void nvkmd_nocturne_fence_detach(struct nvkmd_nocturne_fence *fence);

/* Wait until the fence reaches seqno; VK_TIMEOUT or VK_ERROR_DEVICE_LOST
 * otherwise.  abs_timeout_ns is os_time_get_absolute_timeout()'s clock. */
VkResult nvkmd_nocturne_fence_wait(struct nvkmd_nocturne_fence *fence,
                                   uint64_t seqno, uint64_t abs_timeout_ns);

extern const struct vk_sync_type nvkmd_nocturne_sync_type;

/* One GPU signal the sync is still waiting for */
struct nvkmd_nocturne_sync_point {
   struct nvkmd_nocturne_fence *fence;
   uint64_t seqno;
   uint64_t value;
};

struct nvkmd_nocturne_sync {
   struct vk_sync base;

   /* Value reached (binary: 0 or 1) */
   uint64_t value;
   /* GPU signals in flight, in submission order */
   struct util_dynarray points;
};

/* What a GPU wait for (sync, value) has to acquire: *fence_out is NULL when
 * the value is already reached.  Fails with VK_NOT_READY when the value is
 * neither reached nor pending on a GPU (the caller waits on the CPU). */
VkResult nvkmd_nocturne_sync_gpu_wait_point(struct vk_sync *sync, uint64_t value,
                                            struct nvkmd_nocturne_fence **fence_out,
                                            uint64_t *seqno_out);

/* Record that `fence` reaching `seqno` signals (sync, value) */
VkResult nvkmd_nocturne_sync_add_gpu_signal(struct vk_sync *sync, uint64_t value,
                                            struct nvkmd_nocturne_fence *fence,
                                            uint64_t seqno);

/* ---- pdev ---- */

struct nvkmd_nocturne_pdev {
   struct nvkmd_pdev base;

   uint32_t pci_target;
   /* An RM session for get_vram_used() */
   struct nnrm *rm;

   const struct vk_sync_type *sync_types[2];
};

NVKMD_DECL_SUBCLASS(pdev, nocturne);

/* ---- dev ---- */

/* NVK's part of the GPU address space (see nvkmd_nocturne_rm.c for the rest):
 * normal allocations below 1 << 38, capture/replay above */
#define NVKMD_NOCTURNE_HEAP_START ((uint64_t)1 << 20)
#define NVKMD_NOCTURNE_HEAP_END ((uint64_t)1 << 38)
#define NVKMD_NOCTURNE_REPLAY_HEAP_START NVKMD_NOCTURNE_HEAP_END
#define NVKMD_NOCTURNE_REPLAY_HEAP_END ((uint64_t)3 << 37)

struct nvkmd_nocturne_dev {
   struct nvkmd_dev base;

   struct nnrm *rm;

   simple_mtx_t heap_mutex;
   struct util_vma_heap heap;
   struct util_vma_heap replay_heap;
};

NVKMD_DECL_SUBCLASS(dev, nocturne);

VkResult nvkmd_nocturne_create_dev(struct nvkmd_pdev *pdev,
                                   struct vk_object_base *log_obj,
                                   struct nvkmd_dev **dev_out);

/* ---- mem ---- */

struct nvkmd_nocturne_mem {
   struct nvkmd_mem base;

   struct nnrm_mem *rm_mem;
   uint32_t log_handle;
};

NVKMD_DECL_SUBCLASS(mem, nocturne);

VkResult nvkmd_nocturne_alloc_mem(struct nvkmd_dev *dev,
                                  struct vk_object_base *log_obj,
                                  uint64_t size_B, uint64_t align_B,
                                  enum nvkmd_mem_flags flags,
                                  struct nvkmd_mem **mem_out);

VkResult nvkmd_nocturne_alloc_tiled_mem(struct nvkmd_dev *dev,
                                        struct vk_object_base *log_obj,
                                        uint64_t size_B, uint64_t align_B,
                                        uint8_t pte_kind, uint16_t tile_mode,
                                        enum nvkmd_mem_flags flags,
                                        struct nvkmd_mem **mem_out);

/* Import nvrm's direct-scanout image `image` (nvrm_scanout_create); size_B
 * must not exceed the image's bytes. */
VkResult nvkmd_nocturne_import_scanout(struct nvkmd_dev *dev,
                                       struct vk_object_base *log_obj,
                                       uint32_t image, uint64_t size_B,
                                       struct nvkmd_mem **mem_out);

/* ---- va ---- */

struct nvkmd_nocturne_va {
   struct nvkmd_va base;

   struct nnrm_virt *virt;
};

NVKMD_DECL_SUBCLASS(va, nocturne);

VkResult nvkmd_nocturne_alloc_va(struct nvkmd_dev *dev,
                                 struct vk_object_base *log_obj,
                                 enum nvkmd_va_flags flags, uint8_t pte_kind,
                                 uint64_t size_B, uint64_t align_B,
                                 uint64_t fixed_addr, struct nvkmd_va **va_out);

/* ---- contexts ---- */

/* The host commands a flush adds around the client's pushes (semaphore
 * acquires, the fence release, the first flush's SET_OBJECTs) go into slots
 * of a ring; a slot is reused once the fence passes the flush that used it. */
#define NVKMD_NOCTURNE_HOST_SLOT_B 4096
#define NVKMD_NOCTURNE_HOST_SLOTS 64

struct nvkmd_nocturne_wait {
   struct nvkmd_nocturne_fence *fence;
   uint64_t seqno;
};

struct nvkmd_nocturne_exec_ctx {
   struct nvkmd_ctx base;

   struct nnrm_chan *chan;
   enum nvkmd_engines engines;
   /* subchannel the host methods go to */
   uint8_t host_subc;
   bool objects_bound;

   struct nvkmd_nocturne_fence *fence;
   uint64_t seqno; /* last seqno submitted */

   struct nvkmd_mem *host_mem;
   uint32_t host_slot;
   uint64_t host_slot_seqno[NVKMD_NOCTURNE_HOST_SLOTS];

   /* the next flush: GPU waits, pushes, signals */
   struct util_dynarray waits;   /* struct nvkmd_nocturne_wait */
   struct util_dynarray gps;     /* struct nnrm_gp */
   struct util_dynarray signals; /* struct vk_sync_signal */
};

NVKMD_DECL_SUBCLASS(ctx, nocturne_exec);

struct nvkmd_nocturne_bind_ctx {
   struct nvkmd_ctx base;
};

NVKMD_DECL_SUBCLASS(ctx, nocturne_bind);

VkResult nvkmd_nocturne_create_ctx(struct nvkmd_dev *dev,
                                   struct vk_object_base *log_obj,
                                   enum nvkmd_engines engines,
                                   struct nvkmd_ctx **ctx_out);

#endif /* NVKMD_NOCTURNE_H */
