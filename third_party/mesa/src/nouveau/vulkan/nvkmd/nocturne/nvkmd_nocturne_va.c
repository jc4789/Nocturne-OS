/*
 * SPDX-License-Identifier: MIT
 */
#include "nvkmd_nocturne.h"

#include "util/u_math.h"
#include "util/u_memory.h"
#include "vk_log.h"

#include <inttypes.h>

static VkResult MUST_CHECK
alloc_heap_addr_locked(struct nvkmd_nocturne_dev *dev,
                       struct vk_object_base *log_obj,
                       enum nvkmd_va_flags flags,
                       uint64_t size_B, uint64_t align_B,
                       uint64_t fixed_addr, uint64_t *addr_out)
{
   if (flags & NVKMD_VA_ALLOC_FIXED) {
      assert(flags & NVKMD_VA_REPLAY);

      if (fixed_addr < NVKMD_NOCTURNE_REPLAY_HEAP_START ||
          fixed_addr >= NVKMD_NOCTURNE_REPLAY_HEAP_END) {
         return vk_errorf(log_obj, VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS,
                          "Capture address 0x%" PRIx64 " not in the replay "
                          "heap address range [0x%" PRIx64 ", 0x%" PRIx64 ")",
                          fixed_addr, NVKMD_NOCTURNE_REPLAY_HEAP_START,
                          NVKMD_NOCTURNE_REPLAY_HEAP_END);
      }

      if (fixed_addr & (align_B - 1)) {
         return vk_errorf(log_obj, VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS,
                          "Unaligned capture address: 0x%" PRIx64, fixed_addr);
      }

      if (!util_vma_heap_alloc_addr(&dev->replay_heap, fixed_addr, size_B)) {
         return vk_errorf(log_obj, VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS,
                          "Replay address collision: 0x%" PRIx64, fixed_addr);
      }

      *addr_out = fixed_addr;
   } else if (flags & NVKMD_VA_REPLAY) {
      *addr_out = util_vma_heap_alloc(&dev->replay_heap, size_B, align_B);
      if (*addr_out == 0)
         return vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                          "Failed to allocate virtual address range");
   } else {
      *addr_out = util_vma_heap_alloc(&dev->heap, size_B, align_B);
      if (*addr_out == 0)
         return vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                          "Failed to allocate virtual address range");
   }

   return VK_SUCCESS;
}

static VkResult MUST_CHECK
alloc_heap_addr(struct nvkmd_nocturne_dev *dev,
                struct vk_object_base *log_obj,
                enum nvkmd_va_flags flags,
                uint64_t size_B, uint64_t align_B,
                uint64_t fixed_addr, uint64_t *addr_out)
{
   simple_mtx_lock(&dev->heap_mutex);
   VkResult result = alloc_heap_addr_locked(dev, log_obj, flags,
                                            size_B, align_B,
                                            fixed_addr, addr_out);
   simple_mtx_unlock(&dev->heap_mutex);
   return result;
}

static void
free_heap_addr(struct nvkmd_nocturne_dev *dev,
               enum nvkmd_va_flags flags,
               uint64_t addr, uint64_t size_B)
{
   simple_mtx_lock(&dev->heap_mutex);
   if (flags & NVKMD_VA_REPLAY) {
      assert(addr >= NVKMD_NOCTURNE_REPLAY_HEAP_START);
      assert(addr + size_B <= NVKMD_NOCTURNE_REPLAY_HEAP_END);
      util_vma_heap_free(&dev->replay_heap, addr, size_B);
   } else {
      assert(addr >= NVKMD_NOCTURNE_HEAP_START);
      assert(addr + size_B <= NVKMD_NOCTURNE_HEAP_END);
      util_vma_heap_free(&dev->heap, addr, size_B);
   }
   simple_mtx_unlock(&dev->heap_mutex);
}

VkResult
nvkmd_nocturne_alloc_va(struct nvkmd_dev *_dev,
                        struct vk_object_base *log_obj,
                        enum nvkmd_va_flags flags, uint8_t pte_kind,
                        uint64_t size_B, uint64_t align_B,
                        uint64_t fixed_addr, struct nvkmd_va **va_out)
{
   struct nvkmd_nocturne_dev *dev = nvkmd_nocturne_dev(_dev);
   VkResult result;

   struct nvkmd_nocturne_va *va = CALLOC_STRUCT(nvkmd_nocturne_va);
   if (va == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   const uint32_t min_align_B = _dev->pdev->bind_align_B;
   assert(util_is_power_of_two_or_zero64(align_B));
   align_B = MAX2(align_B, min_align_B);
   size_B = align64(size_B, align_B);

   assert((fixed_addr == 0) == !(flags & NVKMD_VA_ALLOC_FIXED));
   result = alloc_heap_addr(dev, log_obj, flags, size_B, align_B,
                            fixed_addr, &va->base.addr);
   if (result != VK_SUCCESS)
      goto fail_alloc;

   /* Every range is an RM virtual object at that fixed address; a sparse one
    * reads zeros and drops writes where nothing is bound */
   uint32_t st = nnrm_virt_reserve(dev->rm, va->base.addr, size_B,
                                   flags & NVKMD_VA_SPARSE, &va->virt);
   if (st != 0) {
      result = vk_errorf(log_obj, nnrm_vk_result(st, true),
                         "Failed to reserve GPU VA [0x%" PRIx64 ", 0x%" PRIx64 "): %s",
                         va->base.addr, va->base.addr + size_B,
                         nnrm_status_name(st));
      goto fail_addr;
   }

   va->base.ops = &nvkmd_nocturne_va_ops;
   va->base.dev = &dev->base;
   va->base.flags = flags;
   va->base.pte_kind = pte_kind;
   va->base.size_B = size_B;

   *va_out = &va->base;
   return VK_SUCCESS;

fail_addr:
   free_heap_addr(dev, flags, va->base.addr, size_B);
fail_alloc:
   FREE(va);
   return result;
}

static void
nvkmd_nocturne_va_free(struct nvkmd_va *_va)
{
   struct nvkmd_nocturne_va *va = nvkmd_nocturne_va(_va);
   struct nvkmd_nocturne_dev *dev = nvkmd_nocturne_dev(va->base.dev);

   nnrm_virt_release(va->virt);
   free_heap_addr(dev, va->base.flags, va->base.addr, va->base.size_B);
   FREE(va);
}

static VkResult
nvkmd_nocturne_va_bind_mem(struct nvkmd_va *_va,
                           struct vk_object_base *log_obj,
                           uint64_t va_offset_B,
                           struct nvkmd_mem *_mem,
                           uint64_t mem_offset_B,
                           uint64_t range_B)
{
   struct nvkmd_nocturne_va *va = nvkmd_nocturne_va(_va);
   struct nvkmd_nocturne_mem *mem = nvkmd_nocturne_mem(_mem);

   uint32_t st = nnrm_virt_map(va->virt, va_offset_B, mem->rm_mem,
                               mem_offset_B, range_B, va->base.pte_kind);
   if (st != 0) {
      return vk_errorf(log_obj, nnrm_vk_result(st, true),
                       "Failed to bind memory at 0x%" PRIx64 ": %s",
                       va->base.addr + va_offset_B, nnrm_status_name(st));
   }

   return VK_SUCCESS;
}

static VkResult
nvkmd_nocturne_va_unbind(struct nvkmd_va *_va,
                         struct vk_object_base *log_obj,
                         uint64_t va_offset_B,
                         uint64_t range_B)
{
   struct nvkmd_nocturne_va *va = nvkmd_nocturne_va(_va);

   uint32_t st = nnrm_virt_unmap(va->virt, va_offset_B, range_B);
   if (st != 0) {
      return vk_errorf(log_obj, nnrm_vk_result(st, false),
                       "Failed to unbind memory at 0x%" PRIx64 ": %s",
                       va->base.addr + va_offset_B, nnrm_status_name(st));
   }

   return VK_SUCCESS;
}

const struct nvkmd_va_ops nvkmd_nocturne_va_ops = {
   .free = nvkmd_nocturne_va_free,
   .bind_mem = nvkmd_nocturne_va_bind_mem,
   .unbind = nvkmd_nocturne_va_unbind,
};
