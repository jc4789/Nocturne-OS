/*
 * SPDX-License-Identifier: MIT
 *
 * The backend's vk_sync type.  A sync object is the value it has reached plus
 * the GPU signals still in flight, each one "this value once that channel's
 * fence passes that seqno".  Everything is in this process, so one lock covers
 * all sync objects; waits poll the fences (a short spin, then 1 ms sleeps).
 */
#include "nvkmd_nocturne.h"

#include "util/os_time.h"
#include "util/u_memory.h"
#include "vk_log.h"

#include <threads.h>

/* Poll without sleeping for this long before falling back to 1 ms sleeps */
#define NVKMD_NOCTURNE_SPIN_NS 2000000ll

static simple_mtx_t sync_lock = SIMPLE_MTX_INITIALIZER;

/* ---------------------------------------------------------------- fences */

struct nvkmd_nocturne_fence *
nvkmd_nocturne_fence_ref(struct nvkmd_nocturne_fence *fence)
{
   p_atomic_inc(&fence->refcnt);
   return fence;
}

void
nvkmd_nocturne_fence_unref(struct nvkmd_nocturne_fence *fence)
{
   if (!p_atomic_dec_zero(&fence->refcnt))
      return;

   nvkmd_mem_unref(fence->mem);
   FREE(fence);
}

void
nvkmd_nocturne_fence_detach(struct nvkmd_nocturne_fence *fence)
{
   simple_mtx_lock(&sync_lock);
   fence->chan = NULL;
   simple_mtx_unlock(&sync_lock);
}

static inline bool
fence_passed(const struct nvkmd_nocturne_fence *fence, uint64_t seqno)
{
   return *fence->map >= seqno;
}

static bool
fence_lost_locked(const struct nvkmd_nocturne_fence *fence)
{
   return fence->chan != NULL && nnrm_chan_has_error(fence->chan);
}

/* One step of a polling wait that started at `start`: false once the
 * deadline has passed */
static bool
poll_pause(int64_t start, uint64_t abs_timeout_ns)
{
   const int64_t now = os_time_get_nano();
   if ((uint64_t)now >= abs_timeout_ns)
      return false;

   if (now - start < NVKMD_NOCTURNE_SPIN_NS)
      thrd_yield();
   else
      os_time_sleep(1000);
   return true;
}

VkResult
nvkmd_nocturne_fence_wait(struct nvkmd_nocturne_fence *fence,
                          uint64_t seqno, uint64_t abs_timeout_ns)
{
   const int64_t start = os_time_get_nano();
   for (;;) {
      if (fence_passed(fence, seqno))
         return VK_SUCCESS;

      simple_mtx_lock(&sync_lock);
      const bool lost = fence_lost_locked(fence);
      simple_mtx_unlock(&sync_lock);
      if (lost)
         return VK_ERROR_DEVICE_LOST;

      if (!poll_pause(start, abs_timeout_ns))
         return VK_TIMEOUT;
   }
}

/* ---------------------------------------------------------------- sync objects */

static struct nvkmd_nocturne_sync *
to_sync(struct vk_sync *sync)
{
   assert(sync->type == &nvkmd_nocturne_sync_type);
   return container_of(sync, struct nvkmd_nocturne_sync, base);
}

static uint64_t
normalize_value(const struct vk_sync *sync, uint64_t value)
{
   /* A binary payload is signalled (1) or not (0) */
   return (sync->flags & VK_SYNC_IS_TIMELINE) ? value : 1;
}

static void
clear_points_locked(struct nvkmd_nocturne_sync *s)
{
   util_dynarray_foreach(&s->points, struct nvkmd_nocturne_sync_point, p)
      nvkmd_nocturne_fence_unref(p->fence);
   util_dynarray_clear(&s->points);
}

/* Fold the GPU signals that have landed into s->value */
static void
update_locked(struct nvkmd_nocturne_sync *s)
{
   const unsigned count =
      util_dynarray_num_elements(&s->points, struct nvkmd_nocturne_sync_point);
   struct nvkmd_nocturne_sync_point *points =
      util_dynarray_begin(&s->points);

   unsigned kept = 0;
   for (unsigned i = 0; i < count; i++) {
      if (fence_passed(points[i].fence, points[i].seqno)) {
         s->value = MAX2(s->value, points[i].value);
         nvkmd_nocturne_fence_unref(points[i].fence);
      } else {
         points[kept++] = points[i];
      }
   }
   util_dynarray_resize(&s->points, struct nvkmd_nocturne_sync_point, kept);
}

static uint64_t
pending_value_locked(const struct nvkmd_nocturne_sync *s)
{
   uint64_t value = s->value;
   util_dynarray_foreach(&s->points, struct nvkmd_nocturne_sync_point, p)
      value = MAX2(value, p->value);
   return value;
}

static bool
lost_locked(const struct nvkmd_nocturne_sync *s)
{
   util_dynarray_foreach(&s->points, struct nvkmd_nocturne_sync_point, p) {
      if (fence_lost_locked(p->fence))
         return true;
   }
   return false;
}

static VkResult
nvkmd_nocturne_sync_init(struct vk_device *device,
                         struct vk_sync *sync,
                         uint64_t initial_value)
{
   struct nvkmd_nocturne_sync *s = to_sync(sync);

   s->value = (sync->flags & VK_SYNC_IS_TIMELINE) ? initial_value
                                                  : (initial_value != 0);
   util_dynarray_init(&s->points, NULL);
   return VK_SUCCESS;
}

static void
nvkmd_nocturne_sync_finish(struct vk_device *device,
                           struct vk_sync *sync)
{
   struct nvkmd_nocturne_sync *s = to_sync(sync);

   simple_mtx_lock(&sync_lock);
   clear_points_locked(s);
   simple_mtx_unlock(&sync_lock);
   util_dynarray_fini(&s->points);
}

static VkResult
nvkmd_nocturne_sync_signal(struct vk_device *device,
                           struct vk_sync *sync,
                           uint64_t value)
{
   struct nvkmd_nocturne_sync *s = to_sync(sync);

   simple_mtx_lock(&sync_lock);
   if (sync->flags & VK_SYNC_IS_TIMELINE) {
      update_locked(s);
      s->value = MAX2(s->value, value);
   } else {
      clear_points_locked(s);
      s->value = 1;
   }
   simple_mtx_unlock(&sync_lock);
   return VK_SUCCESS;
}

static VkResult
nvkmd_nocturne_sync_get_value(struct vk_device *device,
                              struct vk_sync *sync,
                              uint64_t *value)
{
   struct nvkmd_nocturne_sync *s = to_sync(sync);

   simple_mtx_lock(&sync_lock);
   update_locked(s);
   *value = s->value;
   simple_mtx_unlock(&sync_lock);
   return VK_SUCCESS;
}

static VkResult
nvkmd_nocturne_sync_reset(struct vk_device *device,
                          struct vk_sync *sync)
{
   struct nvkmd_nocturne_sync *s = to_sync(sync);

   simple_mtx_lock(&sync_lock);
   clear_points_locked(s);
   s->value = 0;
   simple_mtx_unlock(&sync_lock);
   return VK_SUCCESS;
}

static VkResult
nvkmd_nocturne_sync_move(struct vk_device *device,
                         struct vk_sync *dst_sync,
                         struct vk_sync *src_sync)
{
   struct nvkmd_nocturne_sync *dst = to_sync(dst_sync);
   struct nvkmd_nocturne_sync *src = to_sync(src_sync);

   simple_mtx_lock(&sync_lock);
   clear_points_locked(dst);
   util_dynarray_fini(&dst->points);

   dst->value = src->value;
   dst->points = src->points;

   src->value = 0;
   util_dynarray_init(&src->points, NULL);
   simple_mtx_unlock(&sync_lock);
   return VK_SUCCESS;
}

static VkResult
nvkmd_nocturne_sync_wait_many(struct vk_device *device,
                              uint32_t wait_count,
                              const struct vk_sync_wait *waits,
                              enum vk_sync_wait_flags wait_flags,
                              uint64_t abs_timeout_ns)
{
   const bool any = wait_flags & VK_SYNC_WAIT_ANY;
   const bool pending = wait_flags & VK_SYNC_WAIT_PENDING;
   const int64_t start = os_time_get_nano();

   for (;;) {
      uint32_t done = 0;
      bool lost = false;

      simple_mtx_lock(&sync_lock);
      for (uint32_t i = 0; i < wait_count; i++) {
         struct nvkmd_nocturne_sync *s = to_sync(waits[i].sync);
         const uint64_t want = normalize_value(waits[i].sync, waits[i].wait_value);

         update_locked(s);
         const uint64_t have = pending ? pending_value_locked(s) : s->value;
         if (have >= want)
            done++;
         else if (lost_locked(s))
            lost = true;
      }
      simple_mtx_unlock(&sync_lock);

      if (any ? done > 0 : done == wait_count)
         return VK_SUCCESS;
      if (lost)
         return VK_ERROR_DEVICE_LOST;
      if (!poll_pause(start, abs_timeout_ns))
         return VK_TIMEOUT;
   }
}

static VkResult
nvkmd_nocturne_sync_wait(struct vk_device *device,
                         struct vk_sync *sync,
                         uint64_t wait_value,
                         enum vk_sync_wait_flags wait_flags,
                         uint64_t abs_timeout_ns)
{
   const struct vk_sync_wait wait = {
      .sync = sync,
      .stage_mask = ~(VkPipelineStageFlags2)0,
      .wait_value = wait_value,
   };
   return nvkmd_nocturne_sync_wait_many(device, 1, &wait, wait_flags,
                                        abs_timeout_ns);
}

VkResult
nvkmd_nocturne_sync_gpu_wait_point(struct vk_sync *sync, uint64_t value,
                                   struct nvkmd_nocturne_fence **fence_out,
                                   uint64_t *seqno_out)
{
   struct nvkmd_nocturne_sync *s = to_sync(sync);
   const uint64_t want = normalize_value(sync, value);
   VkResult result = VK_NOT_READY;

   simple_mtx_lock(&sync_lock);
   update_locked(s);
   if (s->value >= want) {
      *fence_out = NULL;
      result = VK_SUCCESS;
   } else {
      /* Values rise in submission order: the first signal that reaches the
       * value is the one to wait for */
      util_dynarray_foreach(&s->points, struct nvkmd_nocturne_sync_point, p) {
         if (p->value >= want) {
            *fence_out = nvkmd_nocturne_fence_ref(p->fence);
            *seqno_out = p->seqno;
            result = VK_SUCCESS;
            break;
         }
      }
   }
   simple_mtx_unlock(&sync_lock);
   return result;
}

VkResult
nvkmd_nocturne_sync_add_gpu_signal(struct vk_sync *sync, uint64_t value,
                                   struct nvkmd_nocturne_fence *fence,
                                   uint64_t seqno)
{
   struct nvkmd_nocturne_sync *s = to_sync(sync);
   const struct nvkmd_nocturne_sync_point point = {
      .fence = nvkmd_nocturne_fence_ref(fence),
      .seqno = seqno,
      .value = normalize_value(sync, value),
   };

   simple_mtx_lock(&sync_lock);
   if (!(sync->flags & VK_SYNC_IS_TIMELINE)) {
      /* A new binary payload replaces the old one */
      clear_points_locked(s);
      s->value = 0;
   }
   util_dynarray_append(&s->points, point);
   simple_mtx_unlock(&sync_lock);
   return VK_SUCCESS;
}

const struct vk_sync_type nvkmd_nocturne_sync_type = {
   .size = sizeof(struct nvkmd_nocturne_sync),
   .features = VK_SYNC_FEATURE_BINARY |
               VK_SYNC_FEATURE_TIMELINE |
               VK_SYNC_FEATURE_GPU_WAIT |
               VK_SYNC_FEATURE_GPU_MULTI_WAIT |
               VK_SYNC_FEATURE_CPU_WAIT |
               VK_SYNC_FEATURE_CPU_RESET |
               VK_SYNC_FEATURE_CPU_SIGNAL |
               VK_SYNC_FEATURE_WAIT_ANY |
               VK_SYNC_FEATURE_WAIT_PENDING,
   .init = nvkmd_nocturne_sync_init,
   .finish = nvkmd_nocturne_sync_finish,
   .signal = nvkmd_nocturne_sync_signal,
   .get_value = nvkmd_nocturne_sync_get_value,
   .reset = nvkmd_nocturne_sync_reset,
   .move = nvkmd_nocturne_sync_move,
   .wait = nvkmd_nocturne_sync_wait,
   .wait_many = nvkmd_nocturne_sync_wait_many,
};
