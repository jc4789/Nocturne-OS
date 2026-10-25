/*
 * SPDX-License-Identifier: MIT
 *
 * Exec contexts are RM GPFIFO channels.  A flush puts, in order, on the
 * channel's ring:
 *
 *   host push: SET_OBJECTs (first flush only), semaphore acquires for the
 *              waits on other channels
 *   the client's pushes
 *   host push: wait for idle, system membar, release fence = seqno
 *
 * and the signals of the flush become (fence, seqno) points on their sync
 * objects.  Bind contexts work on the CPU: RM updates page tables
 * synchronously, so a bind waits for its waits, binds and signals.
 */
#include "nvkmd_nocturne.h"

#include "nv_push.h"
#include "util/os_time.h"
#include "util/u_memory.h"
#include "vk_log.h"
#include "vk_object.h"

#include "clc36f.h"

#include <inttypes.h>

/* Host-push dwords: one acquire, the release tail, one SET_OBJECT */
#define ACQUIRE_DW 6
#define RELEASE_DW (2 + 5 + 6)
#define SET_OBJECT_DW 2
#define MAX_SUBCHANNELS 5
#define MAX_ACQUIRES \
   ((NVKMD_NOCTURNE_HOST_SLOT_B / 4 - RELEASE_DW - MAX_SUBCHANNELS * SET_OBJECT_DW) / \
    ACQUIRE_DW)

/* GP entries the ring holds unconsumed (one stays free so that GPPut ==
 * GPGet means "empty"), and the client's share of one flush (its two host
 * pushes take the rest) */
#define GP_RING_ENTRIES (NNRM_CHAN_GP_ENTRIES - 1)
#define MAX_PUSHES (GP_RING_ENTRIES - 2)

static_assert(NVKMD_ENGINE_COPY == NNRM_CHAN_COPY, "nvkmd and nnrm engine bits differ");
static_assert(NVKMD_ENGINE_2D == NNRM_CHAN_2D, "nvkmd and nnrm engine bits differ");
static_assert(NVKMD_ENGINE_3D == NNRM_CHAN_3D, "nvkmd and nnrm engine bits differ");
static_assert(NVKMD_ENGINE_M2MF == NNRM_CHAN_M2MF, "nvkmd and nnrm engine bits differ");
static_assert(NVKMD_ENGINE_COMPUTE == NNRM_CHAN_COMPUTE, "nvkmd and nnrm engine bits differ");
static_assert(NVKMD_ENGINE_VDEC == NNRM_CHAN_VDEC, "nvkmd and nnrm engine bits differ");

static struct vk_device *
log_device(struct vk_object_base *log_obj)
{
   return log_obj != NULL ? log_obj->device : NULL;
}

/* ---------------------------------------------------------------- exec */

static void
clear_waits(struct nvkmd_nocturne_exec_ctx *ctx)
{
   util_dynarray_foreach(&ctx->waits, struct nvkmd_nocturne_wait, w)
      nvkmd_nocturne_fence_unref(w->fence);
   util_dynarray_clear(&ctx->waits);
}

static VkResult
create_fence(struct nvkmd_nocturne_dev *dev, struct vk_object_base *log_obj,
             struct nnrm_chan *chan, struct nvkmd_nocturne_fence **fence_out)
{
   struct nvkmd_nocturne_fence *fence = CALLOC_STRUCT(nvkmd_nocturne_fence);
   if (fence == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result =
      nvkmd_dev_alloc_mapped_mem(&dev->base, log_obj, 4096, 0, NVKMD_MEM_GART,
                                 NVKMD_MEM_MAP_RDWR, &fence->mem);
   if (result != VK_SUCCESS) {
      FREE(fence);
      return result;
   }

   fence->refcnt = 1;
   fence->map = fence->mem->map;
   fence->addr = fence->mem->va->addr;
   fence->chan = chan;
   *fence->map = 0;

   *fence_out = fence;
   return VK_SUCCESS;
}

static VkResult
nvkmd_nocturne_create_exec_ctx(struct nvkmd_nocturne_dev *dev,
                               struct vk_object_base *log_obj,
                               enum nvkmd_engines engines,
                               struct nvkmd_ctx **ctx_out)
{
   VkResult result;

   struct nvkmd_nocturne_exec_ctx *ctx = CALLOC_STRUCT(nvkmd_nocturne_exec_ctx);
   if (ctx == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   ctx->base.ops = &nvkmd_nocturne_exec_ctx_ops;
   ctx->base.dev = &dev->base;
   ctx->engines = engines;
   ctx->host_subc =
      (engines & (NVKMD_ENGINE_3D | NVKMD_ENGINE_COMPUTE |
                  NVKMD_ENGINE_2D | NVKMD_ENGINE_M2MF)) ? 0 : 4;

   const char *step;
   uint32_t st = nnrm_chan_create(dev->rm, engines, &ctx->chan, &step);
   if (st != 0) {
      result = vk_errorf(log_obj, nnrm_vk_result(st, false),
                         "Failed to create a GPU channel for engines 0x%x: %s failed: %s (0x%x)",
                         engines, step, nnrm_status_name(st), st);
      goto fail_ctx;
   }

   result = create_fence(dev, log_obj, ctx->chan, &ctx->fence);
   if (result != VK_SUCCESS)
      goto fail_chan;

   result = nvkmd_dev_alloc_mapped_mem(&dev->base, log_obj,
                                       NVKMD_NOCTURNE_HOST_SLOT_B *
                                       NVKMD_NOCTURNE_HOST_SLOTS, 0,
                                       NVKMD_MEM_GART, NVKMD_MEM_MAP_RDWR,
                                       &ctx->host_mem);
   if (result != VK_SUCCESS)
      goto fail_fence;

   util_dynarray_init(&ctx->waits, NULL);
   util_dynarray_init(&ctx->gps, NULL);
   util_dynarray_init(&ctx->signals, NULL);

   *ctx_out = &ctx->base;
   return VK_SUCCESS;

fail_fence:
   nvkmd_nocturne_fence_detach(ctx->fence);
   nvkmd_nocturne_fence_unref(ctx->fence);
fail_chan:
   nnrm_chan_destroy(ctx->chan);
fail_ctx:
   FREE(ctx);
   return result;
}

static void
nvkmd_nocturne_exec_ctx_destroy(struct nvkmd_ctx *_ctx)
{
   struct nvkmd_nocturne_exec_ctx *ctx = nvkmd_nocturne_exec_ctx(_ctx);

   /* Let the channel finish before it goes; a lost channel is gone anyway */
   nvkmd_nocturne_fence_wait(ctx->fence, ctx->seqno,
                             os_time_get_absolute_timeout(5000000000ull));

   nvkmd_nocturne_fence_detach(ctx->fence);
   nnrm_chan_destroy(ctx->chan);

   clear_waits(ctx);
   util_dynarray_fini(&ctx->waits);
   util_dynarray_fini(&ctx->gps);
   util_dynarray_fini(&ctx->signals);

   nvkmd_mem_unref(ctx->host_mem);
   nvkmd_nocturne_fence_unref(ctx->fence);
   FREE(ctx);
}

static void
add_gpu_wait(struct nvkmd_nocturne_exec_ctx *ctx,
             struct nvkmd_nocturne_fence *fence, uint64_t seqno)
{
   /* The channel runs its own work in order */
   if (fence == ctx->fence) {
      nvkmd_nocturne_fence_unref(fence);
      return;
   }

   util_dynarray_foreach(&ctx->waits, struct nvkmd_nocturne_wait, w) {
      if (w->fence == fence) {
         w->seqno = MAX2(w->seqno, seqno);
         nvkmd_nocturne_fence_unref(fence);
         return;
      }
   }

   const struct nvkmd_nocturne_wait wait = { .fence = fence, .seqno = seqno };
   util_dynarray_append(&ctx->waits, wait);
}

static VkResult
nvkmd_nocturne_exec_ctx_wait(struct nvkmd_ctx *_ctx,
                             struct vk_object_base *log_obj,
                             uint32_t wait_count,
                             const struct vk_sync_wait *waits)
{
   struct nvkmd_nocturne_exec_ctx *ctx = nvkmd_nocturne_exec_ctx(_ctx);

   for (uint32_t i = 0; i < wait_count; i++) {
      struct vk_sync *sync = waits[i].sync;
      VkResult result = VK_NOT_READY;

      if (sync->type == &nvkmd_nocturne_sync_type) {
         struct nvkmd_nocturne_fence *fence;
         uint64_t seqno = 0;
         result = nvkmd_nocturne_sync_gpu_wait_point(sync, waits[i].wait_value,
                                                     &fence, &seqno);
         if (result == VK_SUCCESS && fence != NULL)
            add_gpu_wait(ctx, fence, seqno);
      }

      /* Not on a GPU yet (or not ours): wait on the CPU */
      if (result == VK_NOT_READY) {
         result = vk_sync_wait(log_device(log_obj), sync, waits[i].wait_value,
                               VK_SYNC_WAIT_COMPLETE, UINT64_MAX);
      }
      if (result != VK_SUCCESS)
         return result;
   }

   return VK_SUCCESS;
}

static VkResult
nvkmd_nocturne_exec_ctx_flush(struct nvkmd_ctx *_ctx,
                              struct vk_object_base *log_obj);

static VkResult
nvkmd_nocturne_exec_ctx_exec(struct nvkmd_ctx *_ctx,
                             struct vk_object_base *log_obj,
                             uint32_t exec_count,
                             const struct nvkmd_ctx_exec *execs)
{
   struct nvkmd_nocturne_exec_ctx *ctx = nvkmd_nocturne_exec_ctx(_ctx);

   for (uint32_t i = 0; i < exec_count; i++) {
      /* A push ending in an incomplete method goes in one flush with the
       * pushes that complete it */
      uint32_t incomplete_count = 0;
      for (uint32_t j = i; j < exec_count && execs[j].incomplete; j++)
         incomplete_count++;
      assert(incomplete_count < exec_count - i && incomplete_count < MAX_PUSHES);

      /* A flush has to fit on the ring */
      if (util_dynarray_num_elements(&ctx->gps, struct nnrm_gp) +
          incomplete_count >= MAX_PUSHES) {
         VkResult result = nvkmd_nocturne_exec_ctx_flush(&ctx->base, log_obj);
         if (result != VK_SUCCESS)
            return result;
      }

      /* This is the hardware limit on all current GPUs */
      assert((execs[i].addr % 4) == 0 && (execs[i].size_B % 4) == 0);
      assert(execs[i].size_B < (1u << 23));

      const struct nnrm_gp gp = {
         .addr = execs[i].addr,
         .size_B = execs[i].size_B,
         .no_prefetch = execs[i].no_prefetch,
      };
      util_dynarray_append(&ctx->gps, gp);
   }

   return VK_SUCCESS;
}

static void
push_set_objects(struct nvkmd_nocturne_exec_ctx *ctx, struct nv_push *p)
{
   const struct nv_device_info *info = &ctx->base.dev->pdev->dev_info;
   const struct {
      enum nvkmd_engines engine;
      uint8_t subc;
      uint16_t cls;
   } objects[] = {
      { NVKMD_ENGINE_3D,      0, info->cls_eng3d },
      { NVKMD_ENGINE_COMPUTE, 1, info->cls_compute },
      { NVKMD_ENGINE_M2MF,    2, info->cls_m2mf },
      { NVKMD_ENGINE_2D,      3, info->cls_eng2d },
      { NVKMD_ENGINE_COPY,    4, info->cls_copy },
   };

   /* RM creates the engine objects but does not put them on subchannels */
   for (uint32_t i = 0; i < ARRAY_SIZE(objects); i++) {
      if (!(ctx->engines & objects[i].engine) || objects[i].cls == 0)
         continue;
      __push_mthd(p, objects[i].subc, NVC36F_SET_OBJECT);
      P_INLINE_DATA(p, NVVAL(NVC36F, SET_OBJECT, NVCLASS, objects[i].cls));
   }
}

static void
push_acquire(struct nv_push *p, uint8_t subc, uint64_t addr, uint64_t value)
{
   __push_mthd(p, subc, NVC36F_SEM_ADDR_LO);
   P_INLINE_DATA(p, (uint32_t)addr);
   P_INLINE_DATA(p, (uint32_t)(addr >> 32));
   P_INLINE_DATA(p, (uint32_t)value);
   P_INLINE_DATA(p, (uint32_t)(value >> 32));
   P_INLINE_DATA(p, NVDEF(NVC36F, SEM_EXECUTE, OPERATION, ACQ_STRICT_GEQ) |
                    NVDEF(NVC36F, SEM_EXECUTE, ACQUIRE_SWITCH_TSG, EN) |
                    NVDEF(NVC36F, SEM_EXECUTE, PAYLOAD_SIZE, 64BIT));
}

static void
push_release(struct nv_push *p, uint8_t subc, uint64_t addr, uint64_t value)
{
   __push_immd(p, subc, NVC36F_WFI, NVC36F_WFI_SCOPE_ALL);

   /* Make the work's writes visible to the CPU before the fence moves */
   __push_mthd(p, subc, NVC36F_MEM_OP_A);
   P_INLINE_DATA(p, 0);
   P_INLINE_DATA(p, 0);
   P_INLINE_DATA(p, NVDEF(NVC36F, MEM_OP_C, MEMBAR_TYPE, SYS_MEMBAR));
   P_INLINE_DATA(p, NVDEF(NVC36F, MEM_OP_D, OPERATION, MEMBAR));

   __push_mthd(p, subc, NVC36F_SEM_ADDR_LO);
   P_INLINE_DATA(p, (uint32_t)addr);
   P_INLINE_DATA(p, (uint32_t)(addr >> 32));
   P_INLINE_DATA(p, (uint32_t)value);
   P_INLINE_DATA(p, (uint32_t)(value >> 32));
   P_INLINE_DATA(p, NVDEF(NVC36F, SEM_EXECUTE, OPERATION, RELEASE) |
                    NVDEF(NVC36F, SEM_EXECUTE, RELEASE_WFI, EN) |
                    NVDEF(NVC36F, SEM_EXECUTE, PAYLOAD_SIZE, 64BIT));
}

/* Room on the ring for `count` more GP entries.  The GPU is done with a
 * flush's entries once the fence passes it.  Host's GP_GET in USERD is no
 * measure: Host writes USERD back only when RM chose so for the channel, and
 * dev_ram.ref recommends following a channel by its semaphores. */
static VkResult
wait_for_gp_room(struct nvkmd_nocturne_exec_ctx *ctx, uint32_t count)
{
   assert(count <= GP_RING_ENTRIES);
   if (ctx->gp_put + count <= GP_RING_ENTRIES)
      return VK_SUCCESS;

   /* The oldest flush still in a slot whose entries end late enough; the
    * newest one's end at ctx->gp_put always is */
   const uint64_t need = ctx->gp_put + count - GP_RING_ENTRIES;
   uint64_t seqno = ctx->seqno >= NVKMD_NOCTURNE_HOST_SLOTS ?
                    ctx->seqno - NVKMD_NOCTURNE_HOST_SLOTS + 1 : 1;
   while (ctx->gp_end[seqno % NVKMD_NOCTURNE_HOST_SLOTS] < need)
      seqno++;
   assert(seqno <= ctx->seqno);

   return nvkmd_nocturne_fence_wait(ctx->fence, seqno, UINT64_MAX);
}

static VkResult
nvkmd_nocturne_exec_ctx_flush(struct nvkmd_ctx *_ctx,
                              struct vk_object_base *log_obj)
{
   struct nvkmd_nocturne_exec_ctx *ctx = nvkmd_nocturne_exec_ctx(_ctx);
   VkResult result;

   const uint32_t gp_count =
      util_dynarray_num_elements(&ctx->gps, struct nnrm_gp);
   const uint32_t signal_count =
      util_dynarray_num_elements(&ctx->signals, struct vk_sync_signal);
   uint32_t wait_count =
      util_dynarray_num_elements(&ctx->waits, struct nvkmd_nocturne_wait);
   if (gp_count == 0 && signal_count == 0 && wait_count == 0)
      return VK_SUCCESS;

   /* More waits than one host push holds: the rest wait here */
   struct nvkmd_nocturne_wait *waits = util_dynarray_begin(&ctx->waits);
   while (wait_count > MAX_ACQUIRES) {
      wait_count--;
      result = nvkmd_nocturne_fence_wait(waits[wait_count].fence,
                                         waits[wait_count].seqno, UINT64_MAX);
      if (result != VK_SUCCESS)
         return vk_error(log_obj, result);
   }

   /* The slot's previous flush has to be done with it */
   const uint64_t seqno = ctx->seqno + 1;
   const uint32_t slot = seqno % NVKMD_NOCTURNE_HOST_SLOTS;
   if (seqno > NVKMD_NOCTURNE_HOST_SLOTS) {
      result = nvkmd_nocturne_fence_wait(ctx->fence,
                                         seqno - NVKMD_NOCTURNE_HOST_SLOTS,
                                         UINT64_MAX);
      if (result != VK_SUCCESS)
         return vk_error(log_obj, result);
   }

   const uint32_t total = gp_count + 2;
   result = wait_for_gp_room(ctx, total);
   if (result != VK_SUCCESS)
      return vk_error(log_obj, result);

   uint32_t *slot_map = (uint32_t *)((char *)ctx->host_mem->map +
                                     slot * NVKMD_NOCTURNE_HOST_SLOT_B);
   const uint64_t slot_addr = ctx->host_mem->va->addr +
                              (uint64_t)slot * NVKMD_NOCTURNE_HOST_SLOT_B;

   struct nv_push push;
   nv_push_init(&push, slot_map, NVKMD_NOCTURNE_HOST_SLOT_B / 4, SUBC_MASK_ALL);
   struct nv_push *p = &push;

   if (!ctx->objects_bound)
      push_set_objects(ctx, p);
   for (uint32_t i = 0; i < wait_count; i++)
      push_acquire(p, ctx->host_subc, waits[i].fence->addr, waits[i].seqno);
   const uint32_t pre_dw = nv_push_dw_count(p);

   push_release(p, ctx->host_subc, ctx->fence->addr, seqno);
   const uint32_t post_dw = nv_push_dw_count(p) - pre_dw;

   struct nnrm_gp *gps = malloc(total * sizeof(*gps));
   if (gps == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   uint32_t n = 0;
   if (pre_dw > 0)
      gps[n++] = (struct nnrm_gp) { .addr = slot_addr, .size_B = pre_dw * 4 };
   memcpy(&gps[n], util_dynarray_begin(&ctx->gps), gp_count * sizeof(*gps));
   n += gp_count;
   gps[n++] = (struct nnrm_gp) {
      .addr = slot_addr + pre_dw * 4,
      .size_B = post_dw * 4,
   };

   uint32_t st = nnrm_chan_submit(ctx->chan, gps, n);
   free(gps);
   if (st != 0) {
      return vk_errorf(log_obj, VK_ERROR_DEVICE_LOST,
                       "GPU channel submit failed: %s", nnrm_status_name(st));
   }

   ctx->seqno = seqno;
   ctx->objects_bound = true;
   ctx->gp_put += n;
   ctx->gp_end[slot] = ctx->gp_put;

   result = VK_SUCCESS;
   util_dynarray_foreach(&ctx->signals, struct vk_sync_signal, s) {
      VkResult r;
      if (s->sync->type == &nvkmd_nocturne_sync_type) {
         r = nvkmd_nocturne_sync_add_gpu_signal(s->sync, s->signal_value,
                                                ctx->fence, seqno);
      } else {
         /* A sync of another kind is signalled once the work is done */
         r = nvkmd_nocturne_fence_wait(ctx->fence, seqno, UINT64_MAX);
         if (r == VK_SUCCESS)
            r = vk_sync_signal(log_device(log_obj), s->sync, s->signal_value);
      }
      if (r != VK_SUCCESS && result == VK_SUCCESS)
         result = r;
   }

   clear_waits(ctx);
   util_dynarray_clear(&ctx->gps);
   util_dynarray_clear(&ctx->signals);

   return result;
}

static VkResult
nvkmd_nocturne_exec_ctx_signal(struct nvkmd_ctx *_ctx,
                               struct vk_object_base *log_obj,
                               uint32_t signal_count,
                               const struct vk_sync_signal *signals)
{
   struct nvkmd_nocturne_exec_ctx *ctx = nvkmd_nocturne_exec_ctx(_ctx);

   for (uint32_t i = 0; i < signal_count; i++)
      util_dynarray_append(&ctx->signals, signals[i]);

   return nvkmd_nocturne_exec_ctx_flush(&ctx->base, log_obj);
}

static VkResult
nvkmd_nocturne_exec_ctx_sync(struct nvkmd_ctx *_ctx,
                             struct vk_object_base *log_obj)
{
   struct nvkmd_nocturne_exec_ctx *ctx = nvkmd_nocturne_exec_ctx(_ctx);

   VkResult result = nvkmd_nocturne_exec_ctx_flush(&ctx->base, log_obj);
   if (result != VK_SUCCESS)
      return result;

   result = nvkmd_nocturne_fence_wait(ctx->fence, ctx->seqno, UINT64_MAX);
   if (result != VK_SUCCESS)
      return vk_error(log_obj, result);

   return VK_SUCCESS;
}

const struct nvkmd_ctx_ops nvkmd_nocturne_exec_ctx_ops = {
   .destroy = nvkmd_nocturne_exec_ctx_destroy,
   .wait = nvkmd_nocturne_exec_ctx_wait,
   .exec = nvkmd_nocturne_exec_ctx_exec,
   .signal = nvkmd_nocturne_exec_ctx_signal,
   .flush = nvkmd_nocturne_exec_ctx_flush,
   .sync = nvkmd_nocturne_exec_ctx_sync,
};

/* ---------------------------------------------------------------- bind */

static VkResult
nvkmd_nocturne_create_bind_ctx(struct nvkmd_nocturne_dev *dev,
                               struct vk_object_base *log_obj,
                               struct nvkmd_ctx **ctx_out)
{
   struct nvkmd_nocturne_bind_ctx *ctx = CALLOC_STRUCT(nvkmd_nocturne_bind_ctx);
   if (ctx == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   ctx->base.ops = &nvkmd_nocturne_bind_ctx_ops;
   ctx->base.dev = &dev->base;

   *ctx_out = &ctx->base;
   return VK_SUCCESS;
}

static void
nvkmd_nocturne_bind_ctx_destroy(struct nvkmd_ctx *_ctx)
{
   struct nvkmd_nocturne_bind_ctx *ctx = nvkmd_nocturne_bind_ctx(_ctx);

   FREE(ctx);
}

static VkResult
nvkmd_nocturne_bind_ctx_wait(struct nvkmd_ctx *_ctx,
                             struct vk_object_base *log_obj,
                             uint32_t wait_count,
                             const struct vk_sync_wait *waits)
{
   /* Page tables change as soon as RM is asked, so binds wait here */
   for (uint32_t i = 0; i < wait_count; i++) {
      VkResult result = vk_sync_wait(log_device(log_obj), waits[i].sync,
                                     waits[i].wait_value,
                                     VK_SYNC_WAIT_COMPLETE, UINT64_MAX);
      if (result != VK_SUCCESS)
         return result;
   }

   return VK_SUCCESS;
}

static VkResult
nvkmd_nocturne_bind_ctx_bind(struct nvkmd_ctx *_ctx,
                             struct vk_object_base *log_obj,
                             uint32_t bind_count,
                             const struct nvkmd_ctx_bind *binds)
{
   for (uint32_t i = 0; i < bind_count; i++) {
      struct nvkmd_va *va = binds[i].va;
      VkResult result;
      if (binds[i].op == NVKMD_BIND_OP_BIND) {
         result = va->ops->bind_mem(va, log_obj, binds[i].va_offset_B,
                                    binds[i].mem, binds[i].mem_offset_B,
                                    binds[i].range_B);
      } else {
         result = va->ops->unbind(va, log_obj, binds[i].va_offset_B,
                                  binds[i].range_B);
      }
      if (result != VK_SUCCESS)
         return result;
   }

   return VK_SUCCESS;
}

static VkResult
nvkmd_nocturne_bind_ctx_signal(struct nvkmd_ctx *_ctx,
                               struct vk_object_base *log_obj,
                               uint32_t signal_count,
                               const struct vk_sync_signal *signals)
{
   for (uint32_t i = 0; i < signal_count; i++) {
      VkResult result = vk_sync_signal(log_device(log_obj), signals[i].sync,
                                       signals[i].signal_value);
      if (result != VK_SUCCESS)
         return result;
   }

   return VK_SUCCESS;
}

static VkResult
nvkmd_nocturne_bind_ctx_flush(struct nvkmd_ctx *_ctx,
                              struct vk_object_base *log_obj)
{
   return VK_SUCCESS;
}

static VkResult
nvkmd_nocturne_bind_ctx_sync(struct nvkmd_ctx *_ctx,
                             struct vk_object_base *log_obj)
{
   return VK_SUCCESS;
}

const struct nvkmd_ctx_ops nvkmd_nocturne_bind_ctx_ops = {
   .destroy = nvkmd_nocturne_bind_ctx_destroy,
   .wait = nvkmd_nocturne_bind_ctx_wait,
   .bind = nvkmd_nocturne_bind_ctx_bind,
   .signal = nvkmd_nocturne_bind_ctx_signal,
   .flush = nvkmd_nocturne_bind_ctx_flush,
   .sync = nvkmd_nocturne_bind_ctx_sync,
};

VkResult
nvkmd_nocturne_create_ctx(struct nvkmd_dev *_dev,
                          struct vk_object_base *log_obj,
                          enum nvkmd_engines engines,
                          struct nvkmd_ctx **ctx_out)
{
   struct nvkmd_nocturne_dev *dev = nvkmd_nocturne_dev(_dev);

   if (engines == NVKMD_ENGINE_BIND)
      return nvkmd_nocturne_create_bind_ctx(dev, log_obj, ctx_out);

   assert(!(engines & NVKMD_ENGINE_BIND));
   return nvkmd_nocturne_create_exec_ctx(dev, log_obj, engines, ctx_out);
}
