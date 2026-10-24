/*
 * SPDX-License-Identifier: MIT
 */
#include "nvkmd_nocturne.h"

#include "util/bitscan.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "vk_log.h"

#include <inttypes.h>

static uint32_t nvkmd_nocturne_next_log_handle = 1;

static VkResult
create_mem_or_free(struct nvkmd_nocturne_dev *dev,
                   struct vk_object_base *log_obj,
                   enum nvkmd_mem_flags flags,
                   struct nnrm_mem *rm_mem, uint64_t size_B,
                   enum nvkmd_va_flags va_flags, uint64_t va_align_B,
                   struct nvkmd_mem **mem_out)
{
   VkResult result;

   struct nvkmd_nocturne_mem *mem = CALLOC_STRUCT(nvkmd_nocturne_mem);
   if (mem == NULL) {
      result = vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_rm_mem;
   }

   va_align_B = MAX2(dev->base.pdev->bind_align_B, va_align_B);
   nvkmd_mem_init(&dev->base, &mem->base, &nvkmd_nocturne_mem_ops,
                  flags, size_B, va_align_B);
   mem->rm_mem = rm_mem;
   mem->log_handle = p_atomic_inc_return(&nvkmd_nocturne_next_log_handle);

   result = nvkmd_dev_alloc_va(&dev->base, log_obj, va_flags, 0 /* pte_kind */,
                               size_B, va_align_B, 0 /* fixed_addr */,
                               &mem->base.va);
   if (result != VK_SUCCESS)
      goto fail_mem;

   result = nvkmd_va_bind_mem(mem->base.va, log_obj, 0 /* va_offset_B */,
                              &mem->base, 0 /* mem_offset_B */, size_B);
   if (result != VK_SUCCESS)
      goto fail_va;

   *mem_out = &mem->base;
   return VK_SUCCESS;

fail_va:
   nvkmd_va_free(mem->base.va);
fail_mem:
   simple_mtx_destroy(&mem->base.map_mutex);
   FREE(mem);
fail_rm_mem:
   nnrm_mem_free(rm_mem);
   return result;
}

VkResult
nvkmd_nocturne_alloc_mem(struct nvkmd_dev *dev,
                         struct vk_object_base *log_obj,
                         uint64_t size_B, uint64_t align_B,
                         enum nvkmd_mem_flags flags,
                         struct nvkmd_mem **mem_out)
{
   return nvkmd_nocturne_alloc_tiled_mem(dev, log_obj, size_B, align_B,
                                         0 /* pte_kind */, 0 /* tile_mode */,
                                         flags, mem_out);
}

VkResult
nvkmd_nocturne_alloc_tiled_mem(struct nvkmd_dev *_dev,
                               struct vk_object_base *log_obj,
                               uint64_t size_B, uint64_t align_B,
                               uint8_t pte_kind, uint16_t tile_mode,
                               enum nvkmd_mem_flags flags,
                               struct nvkmd_mem **mem_out)
{
   struct nvkmd_nocturne_dev *dev = nvkmd_nocturne_dev(_dev);
   const struct nv_device_info *dev_info = &dev->base.pdev->dev_info;

   /* nvkmd_info::has_alloc_tiled is false: tiling comes from the VA's PTE
    * kind, never from the memory object */
   if (pte_kind != 0 || tile_mode != 0)
      return vk_error(log_obj, VK_ERROR_FEATURE_NOT_PRESENT);

   /* Only one placement flag may be specified */
   assert(util_bitcount(flags & NVKMD_MEM_PLACEMENT_FLAGS) == 1);

   assert(util_is_power_of_two_or_zero64(align_B));
   align_B = MAX2(align_B, dev->base.pdev->bind_align_B);
   size_B = align64(size_B, align_B);

   bool try_vram = false, try_sysmem = false;
   if (flags & NVKMD_MEM_LOCAL) {
      try_vram = dev_info->vram_size_B > 0;
      try_sysmem = true;
   } else if (flags & NVKMD_MEM_GART) {
      try_sysmem = true;
   } else if (flags & NVKMD_MEM_VRAM) {
      try_vram = true;
   }
   if (dev->base.pdev->debug_flags & NVK_DEBUG_FORCE_GART) {
      try_vram = false;
      try_sysmem = true;
   }

   /* Discrete GPU maps are coherent: snooped system memory or VRAM through
    * BAR1 */
   flags |= NVKMD_MEM_COHERENT;

   struct nnrm_mem *rm_mem = NULL;
   enum nvkmd_va_flags va_flags = 0;
   uint32_t st = 1;
   if (try_vram)
      st = nnrm_mem_alloc(dev->rm, NNRM_MEM_VRAM, size_B, align_B, &rm_mem);
   if (st != 0 && try_sysmem) {
      st = nnrm_mem_alloc(dev->rm, NNRM_MEM_SYSMEM, size_B, align_B, &rm_mem);
      if (st == 0 && !try_vram)
         va_flags |= NVKMD_VA_GART;
   }
   if (st != 0) {
      return vk_errorf(log_obj, nnrm_vk_result(st, true),
                       "Failed to allocate %" PRIu64 " B: %s",
                       size_B, nnrm_status_name(st));
   }

   return create_mem_or_free(dev, log_obj, flags, rm_mem, size_B,
                             va_flags, align_B, mem_out);
}

VkResult
nvkmd_nocturne_import_scanout(struct nvkmd_dev *_dev,
                              struct vk_object_base *log_obj,
                              uint32_t image, uint64_t size_B,
                              struct nvkmd_mem **mem_out)
{
   struct nvkmd_nocturne_dev *dev = nvkmd_nocturne_dev(_dev);

   size_B = align64(size_B, NNRM_PAGE_SIZE);

   struct nnrm_mem *rm_mem = NULL;
   uint32_t st = nnrm_mem_import_scanout(dev->rm, image, size_B, &rm_mem);
   if (st != 0) {
      return vk_errorf(log_obj, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                       "Failed to import scanout image %u: %s",
                       image, nnrm_status_name(st));
   }

   /* Video memory the display reads: never mapped by the CPU */
   return create_mem_or_free(dev, log_obj, NVKMD_MEM_VRAM | NVKMD_MEM_SHARED,
                             rm_mem, size_B, 0 /* va_flags */,
                             0 /* va_align_B */, mem_out);
}

static void
nvkmd_nocturne_mem_free(struct nvkmd_mem *_mem)
{
   struct nvkmd_nocturne_mem *mem = nvkmd_nocturne_mem(_mem);

   nvkmd_va_free(mem->base.va);
   nnrm_mem_free(mem->rm_mem);
   simple_mtx_destroy(&mem->base.map_mutex);
   FREE(mem);
}

static VkResult
nvkmd_nocturne_mem_map(struct nvkmd_mem *_mem,
                       struct vk_object_base *log_obj,
                       enum nvkmd_mem_map_flags map_flags,
                       void *fixed_addr,
                       void **map_out)
{
   struct nvkmd_nocturne_mem *mem = nvkmd_nocturne_mem(_mem);

   /* nvkmd_info::has_map_fixed is false */
   if (map_flags & NVKMD_MEM_MAP_FIXED)
      return vk_error(log_obj, VK_ERROR_MEMORY_MAP_FAILED);

   void *map = NULL;
   uint32_t st = nnrm_mem_map(mem->rm_mem, &map);
   if (st != 0) {
      return vk_errorf(log_obj, VK_ERROR_MEMORY_MAP_FAILED,
                       "Failed to map memory: %s", nnrm_status_name(st));
   }

   *map_out = map;
   return VK_SUCCESS;
}

static void
nvkmd_nocturne_mem_unmap(struct nvkmd_mem *_mem,
                         enum nvkmd_mem_map_flags flags,
                         void *map)
{
   struct nvkmd_nocturne_mem *mem = nvkmd_nocturne_mem(_mem);

   nnrm_mem_unmap(mem->rm_mem, map);
}

static uint32_t
nvkmd_nocturne_mem_log_handle(struct nvkmd_mem *_mem)
{
   return nvkmd_nocturne_mem(_mem)->log_handle;
}

const struct nvkmd_mem_ops nvkmd_nocturne_mem_ops = {
   .free = nvkmd_nocturne_mem_free,
   .map = nvkmd_nocturne_mem_map,
   .unmap = nvkmd_nocturne_mem_unmap,
   .log_handle = nvkmd_nocturne_mem_log_handle,
};
