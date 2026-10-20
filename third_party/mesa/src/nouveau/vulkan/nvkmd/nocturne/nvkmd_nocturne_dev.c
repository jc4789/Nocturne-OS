/*
 * SPDX-License-Identifier: MIT
 */
#include "nvkmd_nocturne.h"

#include "util/u_memory.h"
#include "vk_log.h"

VkResult
nvkmd_nocturne_create_dev(struct nvkmd_pdev *_pdev,
                          struct vk_object_base *log_obj,
                          struct nvkmd_dev **dev_out)
{
   struct nvkmd_nocturne_pdev *pdev = nvkmd_nocturne_pdev(_pdev);

   struct nvkmd_nocturne_dev *dev = CALLOC_STRUCT(nvkmd_nocturne_dev);
   if (dev == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* A device has an RM client (and so a GPU address space) of its own */
   uint32_t st = nnrm_open_device(pdev->pci_target, &dev->rm, NULL);
   if (st != 0) {
      FREE(dev);
      return vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                       "Failed to open the GPU through nvrm: %s",
                       nnrm_status_name(st));
   }

   uint64_t va_start, va_end;
   nnrm_va_range(dev->rm, &va_start, &va_end);
   assert(va_start <= NVKMD_NOCTURNE_HEAP_START);
   assert(NVKMD_NOCTURNE_REPLAY_HEAP_END <= va_end);

   dev->base.ops = &nvkmd_nocturne_dev_ops;
   dev->base.pdev = &pdev->base;
   dev->base.va_start = 0;
   dev->base.va_end = va_end;

   list_inithead(&dev->base.mems);
   simple_mtx_init(&dev->base.mems_mutex, mtx_plain);

   simple_mtx_init(&dev->heap_mutex, mtx_plain);

   STATIC_ASSERT(NVKMD_NOCTURNE_HEAP_START < NVKMD_NOCTURNE_HEAP_END);
   util_vma_heap_init(&dev->heap, NVKMD_NOCTURNE_HEAP_START,
                      NVKMD_NOCTURNE_HEAP_END - NVKMD_NOCTURNE_HEAP_START);

   STATIC_ASSERT(NVKMD_NOCTURNE_REPLAY_HEAP_START <
                 NVKMD_NOCTURNE_REPLAY_HEAP_END);
   util_vma_heap_init(&dev->replay_heap, NVKMD_NOCTURNE_REPLAY_HEAP_START,
                      NVKMD_NOCTURNE_REPLAY_HEAP_END -
                      NVKMD_NOCTURNE_REPLAY_HEAP_START);

   *dev_out = &dev->base;
   return VK_SUCCESS;
}

static void
nvkmd_nocturne_dev_destroy(struct nvkmd_dev *_dev)
{
   struct nvkmd_nocturne_dev *dev = nvkmd_nocturne_dev(_dev);

   util_vma_heap_finish(&dev->heap);
   util_vma_heap_finish(&dev->replay_heap);
   simple_mtx_destroy(&dev->heap_mutex);
   simple_mtx_destroy(&dev->base.mems_mutex);

   /* Closing the client frees whatever RM objects are left */
   nnrm_close(dev->rm);
   FREE(dev);
}

static uint64_t
nvkmd_nocturne_dev_get_gpu_timestamp(struct nvkmd_dev *_dev)
{
   struct nvkmd_nocturne_dev *dev = nvkmd_nocturne_dev(_dev);

   return nnrm_gpu_timestamp(dev->rm);
}

const struct nvkmd_dev_ops nvkmd_nocturne_dev_ops = {
   .destroy = nvkmd_nocturne_dev_destroy,
   .get_gpu_timestamp = nvkmd_nocturne_dev_get_gpu_timestamp,
   .alloc_mem = nvkmd_nocturne_alloc_mem,
   .alloc_tiled_mem = nvkmd_nocturne_alloc_tiled_mem,
   .alloc_va = nvkmd_nocturne_alloc_va,
   .create_ctx = nvkmd_nocturne_create_ctx,
};
