/*
 * SPDX-License-Identifier: MIT
 */
#include "nvkmd_nocturne.h"

#include "nvkmd/nvkmd_device_info.h"
#include "util/cache_ops.h"
#include "util/log.h"
#include "util/u_memory.h"
#include "vk_log.h"

#include <stdio.h>
#include <string.h>

/* RM drives Turing and later (the GSP generations) */
#define NVKMD_NOCTURNE_MIN_CHIPSET 0x160

/* Why a GPU nvrm serves is not one NVK uses goes to Mesa's log (the system
 * log): vk_error*() say nothing unless Vulkan debug logging is on, and Zink
 * says only that it found no GPU. */

VkResult
nvkmd_nocturne_enumerate_gpus(struct vk_object_base *log_obj,
                             uint32_t **gpu_ids, uint32_t *count)
{
   uint32_t st = nnrm_enumerate_gpus(gpu_ids, count);
   if (st == 0)
      return VK_SUCCESS;
   mesa_logw("NVK: cannot list the GPUs nvrm serves: %s (0x%x)", nnrm_status_name(st), st);
   return vk_errorf(log_obj, nnrm_vk_result(st, false),
      "Failed to enumerate RM-probed GPUs: %s", nnrm_status_name(st));
}

static const char *
chipset_family(uint16_t chipset)
{
   switch (chipset & 0x1f0) {
   case 0x160: return "TU10x";
   case 0x170: return "GA10x";
   case 0x180: return "GH10x";
   case 0x190: return "AD10x";
   case 0x1a0: return "GB10x";
   case 0x1b0: return "GB20x";
   default: return NULL;
   }
}

static void
fill_dev_info(struct nv_device_info *info, const struct nnrm_gpu_info *gpu)
{
   memset(info, 0, sizeof(*info));

   info->type = NV_DEVICE_TYPE_DIS;
   info->device_id = gpu->device_id;
   info->chipset = gpu->chipset;

   const char *family = chipset_family(gpu->chipset);
   if (family != NULL)
      snprintf(info->chipset_name, sizeof(info->chipset_name), "%s", family);
   else
      snprintf(info->chipset_name, sizeof(info->chipset_name), "NV%X", gpu->chipset);

   const char *name = nvkmd_name_for_chip(gpu->device_id, gpu->subsystem_id,
                                          gpu->subsystem_vendor_id);
   if (name != NULL)
      snprintf(info->device_name, sizeof(info->device_name), "%s", name);
   else
      snprintf(info->device_name, sizeof(info->device_name), "NVIDIA %s",
               info->chipset_name);

   info->pci.domain = gpu->pci_domain;
   info->pci.bus = gpu->pci_bus;
   info->pci.dev = gpu->pci_dev;
   info->pci.func = gpu->pci_func;
   info->pci.revision_id = gpu->revision_id;

   info->gpc_count = gpu->gpc_count;
   info->tpc_count = gpu->tpc_count;

   info->cls_copy = gpu->cls_copy;
   info->cls_eng2d = gpu->cls_eng2d;
   info->cls_eng3d = gpu->cls_eng3d;
   info->cls_m2mf = gpu->cls_m2mf;
   info->cls_compute = gpu->cls_compute;
   info->cls_gpfifo = gpu->cls_gpfifo;
   info->cls_vdec = gpu->cls_vdec;

   info->has_transfer_queue = gpu->cls_copy != 0;
   info->has_video = false;

   info->vram_size_B = gpu->vram_size_B;
   info->bar_size_B = gpu->bar_size_B;

   info->has_zcull_info = false;

   nvkmd_device_info_finish(info);
}

VkResult
nvkmd_nocturne_try_create_pdev(uint32_t pci_target,
                               struct vk_object_base *log_obj,
                               enum nvk_debug debug_flags,
                               struct nvkmd_pdev **pdev_out)
{
   struct nnrm *rm;
   struct nnrm_gpu_info gpu;
   const char *step;
   uint32_t st = nnrm_open_device(pci_target, &rm, &gpu, &step);
   if (st != 0) {
      mesa_logw("NVK: GPU %02x:%02x.%x: %s failed: %s (0x%x)", pci_target >> 8,
                (pci_target >> 3) & 0x1f, pci_target & 0x7, step, nnrm_status_name(st), st);
      /* A snapshot member can disappear before it is opened. Do not turn
       * host OOM or a failing RM command into an absent/unsupported GPU. */
      if (nnrm_gpu_unavailable(st))
         return VK_ERROR_INCOMPATIBLE_DRIVER;
      return vk_error(log_obj, nnrm_vk_result(st, false));
   }

   if (gpu.chipset < NVKMD_NOCTURNE_MIN_CHIPSET || gpu.cls_gpfifo == 0 ||
       gpu.cls_eng3d == 0 || gpu.cls_compute == 0) {
      mesa_logw("NVK: GPU %02x:%02x.%x is not used: chipset 0x%x, GPFIFO class 0x%x, 3D class 0x%x, "
                "compute class 0x%x (Turing, 0x%x, or later with all three is needed)", pci_target >> 8,
                (pci_target >> 3) & 0x1f, pci_target & 0x7, gpu.chipset, gpu.cls_gpfifo, gpu.cls_eng3d,
                gpu.cls_compute, NVKMD_NOCTURNE_MIN_CHIPSET);
      nnrm_close(rm);
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   }

   struct nvkmd_nocturne_pdev *pdev = CALLOC_STRUCT(nvkmd_nocturne_pdev);
   if (pdev == NULL) {
      nnrm_close(rm);
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   pdev->base.ops = &nvkmd_nocturne_pdev_ops;
   pdev->base.debug_flags = debug_flags;
   fill_dev_info(&pdev->base.dev_info, &gpu);
   pdev->base.dev_info.nc_atom_size_B = util_cache_granularity();

   pdev->base.kmd_info = (struct nvkmd_info) {
      .has_dma_buf = false,
      .has_get_vram_used = nnrm_vram_used(rm) != 0,
      .has_alloc_tiled = false,
      .has_map_fixed = false,
      .has_overmap = false,
      .has_compression = false,
   };

   /* Every allocation and mapping is in 4 KiB pages for now */
   pdev->base.bind_align_B = NNRM_PAGE_SIZE;

   pdev->pci_target = gpu.pci_target;
   pdev->rm = rm;

   pdev->sync_types[0] = &nvkmd_nocturne_sync_type;
   pdev->sync_types[1] = NULL;
   pdev->base.sync_types = pdev->sync_types;

   *pdev_out = &pdev->base;
   return VK_SUCCESS;
}

static void
nvkmd_nocturne_pdev_destroy(struct nvkmd_pdev *_pdev)
{
   struct nvkmd_nocturne_pdev *pdev = nvkmd_nocturne_pdev(_pdev);

   nnrm_close(pdev->rm);
   FREE(pdev);
}

static uint64_t
nvkmd_nocturne_pdev_get_vram_used(struct nvkmd_pdev *_pdev)
{
   struct nvkmd_nocturne_pdev *pdev = nvkmd_nocturne_pdev(_pdev);

   return nnrm_vram_used(pdev->rm);
}

const struct nvkmd_pdev_ops nvkmd_nocturne_pdev_ops = {
   .destroy = nvkmd_nocturne_pdev_destroy,
   .get_vram_used = nvkmd_nocturne_pdev_get_vram_used,
   .create_dev = nvkmd_nocturne_create_dev,
};
