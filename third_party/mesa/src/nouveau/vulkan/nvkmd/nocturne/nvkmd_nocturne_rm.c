/*
 * SPDX-License-Identifier: MIT
 *
 * Nocturne's NVKMD backend: NVIDIA's resource manager through libnvrm.
 *
 * This is the only file of the backend that includes RM headers (they define
 * their own NvU32/NV_STATUS world and clash with Mesa's nouveau headers), so it
 * includes nothing from Mesa but the Vulkan result codes.  See
 * nvkmd_nocturne_rm.h for the interface and ports/nvidia/README.md for libnvrm.
 *
 * How the pieces map onto RM objects (the same shape RM's own CE utilities
 * and UVM's gpu ops use):
 *
 *   NV01_ROOT_CLIENT -> NV01_DEVICE_0 -> NV20_SUBDEVICE_0   one nnrm
 *   NV01_MEMORY_SYSTEM / NV01_MEMORY_LOCAL_USER             one nnrm_mem
 *   NV50_MEMORY_VIRTUAL + NVOS46/NVOS47 (map/unmap DMA)     one nnrm_virt
 *   *_CHANNEL_GPFIFO_A (in a group of its own) -> engine objects  one nnrm_chan
 *   VOLTA_USERMODE_A                                        the doorbell page
 */
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>

#include "nvkmd_nocturne_rm.h"

#include "nvrm.h"
#include "nocturne.h"

/* nvstatus.h brings RM's own nvtypes.h; Mesa's nouveau headers have a file of
 * that name too, so it is never included by name here */
#include "nvstatus.h"
#include "nvos.h"
#include "nv_escape.h"
#include "nvmisc.h"

#include "class/cl0080.h"
#include "class/cl2080.h"
#include "class/cl003e.h"
#include "class/cl0040.h"
#include "class/cl50a0.h"
#include "class/cla06c.h"
#include "class/cla06f.h"
#include "class/clc361.h"
#include "class/clc661.h"
#include "class/clc36f.h"
#include "class/clb0b5sw.h"
#include "alloc/alloc_channel.h"

#include "ctrl/ctrl0000/ctrl0000gpu.h"
#include "ctrl/ctrl0080/ctrl0080gpu.h"
#include "ctrl/ctrl2080/ctrl2080bus.h"
#include "ctrl/ctrl2080/ctrl2080ce.h"
#include "ctrl/ctrl2080/ctrl2080fb.h"
#include "ctrl/ctrl2080/ctrl2080gpu.h"
#include "ctrl/ctrl2080/ctrl2080gr.h"
#include "ctrl/ctrl2080/ctrl2080mc.h"
#include "ctrl/ctrl2080/ctrl2080tmr.h"
#include "ctrl/ctrla06f/ctrla06fgpfifo.h"
#include "ctrl/ctrlc36f.h"

#include "vulkan/vulkan_core.h"

/* The GPU address space of one nnrm, all below 1 << 40 because host semaphore
 * addresses (SEM_ADDR_HI) and the GPFIFO base have 40 bits on Volta..Ada:
 *
 *   [NNRM_VA_START, NNRM_VA_END)          NVK's heaps (nnrm_va_range)
 *   [NNRM_INTERNAL_VA, NNRM_INTERNAL_END) the channels' own buffers
 *   [NNRM_RM_VA_START, NNRM_RM_VA_LIMIT]  RM's internal mappings (context
 *                                         buffers), kept there by the device's
 *                                         RESTRICT_RESERVED_VALIMITS
 *
 * Every client range is allocated at a fixed address, so RM never picks one. */
#define NNRM_VA_START      (1ull << 20)
#define NNRM_VA_END        (3ull << 37)
#define NNRM_INTERNAL_VA   NNRM_VA_END
#define NNRM_INTERNAL_END  (1ull << 39)
#define NNRM_INTERNAL_STEP (1ull << 20)
#define NNRM_RM_VA_START   (1ull << 39)
#define NNRM_RM_VA_LIMIT   ((1ull << 40) - 1)

#define NNRM_OWNER 0x4e4f4354u /* 'NOCT': a tag RM keeps with the allocation */

#define NNRM_RING_WAIT_NS 10000000000ull

/* ------------------------------------------------------------------ session */

/* Take an owned PCI snapshot; discovery errors are not an empty GPU list. */
static uint32_t
pci_snapshot(struct n_pciinfo **out, uint32_t *count)
{
   struct n_pciinfo *list = NULL;
   size_t capacity = 1;
   uint32_t status = NV_ERR_INSUFFICIENT_RESOURCES;
   for (;;) {
      if (capacity > INT_MAX || capacity > SIZE_MAX / sizeof(*list))
         break;
      struct n_pciinfo *next = realloc(list, capacity * sizeof(*list));
      if (next == NULL) {
         status = NV_ERR_NO_MEMORY;
         break;
      }
      list = next;
      n_result len = pcilist(list, (int)capacity);
      if (len < 0) {
         status = len == -N_ERR_NO_MEMORY ? NV_ERR_NO_MEMORY : NV_ERR_OPERATING_SYSTEM;
         break;
      }
      if ((size_t)len < capacity) {
         *out = list;
         *count = len;
         return NV_OK;
      }
      if (capacity > SIZE_MAX / 2)
         break;
      capacity *= 2;
   }
   free(list);
   return status;
}

struct nnrm {
   struct nvrm *rm;
   uint32_t client, device, subdevice;
   uint32_t gpu_id;
   struct nnrm_gpu_info info;

   /* classes the GPU offers (NV0080_CTRL_GPU_GET_CLASSLIST_V2) */
   uint32_t class_count;
   uint32_t classes[NV0080_CTRL_GPU_CLASSLIST_MAX_SIZE];

   /* NV2080_ENGINE_TYPE_* of the copy engine for copy-only channels, and of
    * the graphics engine's partner copy engine */
   uint32_t copy_engine_type, grce_engine_type;

   /* Channel internals take VAs from here, above NNRM_VA_END */
   atomic_uint_fast64_t internal_va;

   /* the doorbell page (VOLTA_USERMODE_A), made on first use */
   mtx_t usermode_lock;
   /* The owner and each channel retain this address space. The shared BAR1
    * read target is initialized under usermode_lock and lives until the last
    * reference, so channel churn does not allocate another VRAM page. */
   size_t refs;
   struct nnrm_mem *posted_mem;
   void *posted_map;
   uint32_t usermode_object;
   volatile uint8_t *usermode_map;

   /* /bin/nvrm has gone: nothing the GPU was doing can be waited for any more */
   atomic_bool daemon_lost;
};

#define OS32_ATTR(field, value) DRF_DEF(OS32, _ATTR, field, value)

static uint32_t
rm_alloc(struct nnrm *n, uint32_t parent, uint32_t *object, uint32_t cls,
         void *params, uint32_t size)
{
   *object = 0;
   uint32_t st = nvrm_alloc(n->rm, n->client, parent, object, cls, params, size);
   if (st != NV_OK && *object != 0) {
      /* An error reply can still carry a partially-created object. The
       * caller cannot use it, so release it before dropping its only handle. */
      nvrm_free(n->rm, n->client, parent, *object);
      *object = 0;
   }
   return st;
}

static void
rm_free(struct nnrm *n, uint32_t parent, uint32_t object)
{
   if (object != 0)
      nvrm_free(n->rm, n->client, parent, object);
}

static uint32_t
rm_control(struct nnrm *n, uint32_t object, uint32_t cmd, void *params, uint32_t size)
{
   return nvrm_control(n->rm, n->client, object, cmd, params, size);
}

/* ------------------------------------------------------------------ results */

int
nnrm_vk_result(uint32_t st, bool out_of_device)
{
   switch (st) {
   case NV_OK:
      return VK_SUCCESS;
   case NV_ERR_NO_MEMORY:
   case NV_ERR_INSUFFICIENT_RESOURCES:
      return out_of_device ? VK_ERROR_OUT_OF_DEVICE_MEMORY
                           : VK_ERROR_OUT_OF_HOST_MEMORY;
   case NV_ERR_OPERATING_SYSTEM:
   case NV_ERR_GPU_IS_LOST:
   case NV_ERR_RESET_REQUIRED:
      return VK_ERROR_DEVICE_LOST;
   case NV_ERR_NOT_SUPPORTED:
      return VK_ERROR_FEATURE_NOT_PRESENT;
   default:
      return VK_ERROR_UNKNOWN;
   }
}

const char *
nnrm_status_name(uint32_t st)
{
   switch (st) {
   case NV_OK: return "NV_OK";
   case NV_ERR_NO_MEMORY: return "NV_ERR_NO_MEMORY";
   case NV_ERR_INSUFFICIENT_RESOURCES: return "NV_ERR_INSUFFICIENT_RESOURCES";
   case NV_ERR_INVALID_ARGUMENT: return "NV_ERR_INVALID_ARGUMENT";
   case NV_ERR_INVALID_OBJECT_HANDLE: return "NV_ERR_INVALID_OBJECT_HANDLE";
   case NV_ERR_INVALID_CLASS: return "NV_ERR_INVALID_CLASS";
   case NV_ERR_NOT_SUPPORTED: return "NV_ERR_NOT_SUPPORTED";
   case NV_ERR_OPERATING_SYSTEM: return "NV_ERR_OPERATING_SYSTEM (nvrm not reachable)";
   case NV_ERR_GPU_IS_LOST: return "NV_ERR_GPU_IS_LOST";
   case NV_ERR_TIMEOUT: return "NV_ERR_TIMEOUT";
   case NV_ERR_INVALID_STATE: return "NV_ERR_INVALID_STATE";
   case NV_ERR_OBJECT_NOT_FOUND: return "NV_ERR_OBJECT_NOT_FOUND";
   default: return "NV_ERR_(other)";
   }
}

bool
nnrm_gpu_unavailable(uint32_t status)
{
   return status == NV_ERR_OBJECT_NOT_FOUND;
}

/* ------------------------------------------------------------------ open */

static uint32_t
class_with_low_byte(const struct nnrm *n, uint32_t low)
{
   /* the highest class of this kind, as Mesa's DRM winsys picks them */
   uint32_t best = 0;
   for (uint32_t i = 0; i < n->class_count; i++) {
      if ((n->classes[i] & 0xff) == low && n->classes[i] > best)
         best = n->classes[i];
   }
   return best;
}

static bool
has_class(const struct nnrm *n, uint32_t cls)
{
   for (uint32_t i = 0; i < n->class_count; i++) {
      if (n->classes[i] == cls)
         return true;
   }
   return false;
}

static uint32_t
popcount32(uint32_t v)
{
   return (uint32_t)__builtin_popcount(v);
}

/* The shader configuration NVK sizes its shader memory from: it has to be
 * complete, a partial count would under-size it. */
static uint32_t
query_gr(struct nnrm *n)
{
   NV2080_CTRL_GR_GET_GPC_MASK_PARAMS gpc = {0};
   uint32_t st = rm_control(n, n->subdevice, NV2080_CTRL_CMD_GR_GET_GPC_MASK,
                            &gpc, sizeof(gpc));
   if (st != NV_OK)
      return st;
   if (gpc.gpcMask == 0)
      return NV_ERR_INVALID_STATE;

   uint32_t tpcs = 0;
   for (uint32_t id = 0; id < 32; id++) {
      if (!(gpc.gpcMask & (1u << id)))
         continue;
      NV2080_CTRL_GR_GET_TPC_MASK_PARAMS tpc = {0};
      tpc.gpcId = id;
      st = rm_control(n, n->subdevice, NV2080_CTRL_CMD_GR_GET_TPC_MASK,
                      &tpc, sizeof(tpc));
      if (st != NV_OK)
         return st;
      tpcs += popcount32(tpc.tpcMask);
   }
   if (tpcs == 0)
      return NV_ERR_INVALID_STATE;

   n->info.gpc_count = (uint8_t)popcount32(gpc.gpcMask);
   n->info.tpc_count = (uint16_t)tpcs;
   return NV_OK;
}

static void
query_fb(struct nnrm *n)
{
   NV2080_CTRL_FB_GET_INFO_V2_PARAMS fb = {0};
   fb.fbInfoListSize = 2;
   fb.fbInfoList[0].index = NV2080_CTRL_FB_INFO_INDEX_RAM_SIZE;
   fb.fbInfoList[1].index = NV2080_CTRL_FB_INFO_INDEX_BAR1_SIZE;
   if (rm_control(n, n->subdevice, NV2080_CTRL_CMD_FB_GET_INFO_V2, &fb, sizeof(fb)) != NV_OK)
      return;

   n->info.vram_size_B = (uint64_t)fb.fbInfoList[0].data * 1024;
   n->info.bar_size_B = (uint64_t)fb.fbInfoList[1].data * 1024 * 1024;
}

static uint32_t
probed_gpu(struct nvrm *rm, uint32_t client, uint32_t target, uint32_t *gpu_id)
{
   NV0000_CTRL_GPU_GET_PROBED_IDS_PARAMS probed = {0};
   for (uint32_t i = 0; i < NV0000_CTRL_GPU_MAX_PROBED_GPUS; i++)
      probed.gpuIds[i] = NV0000_CTRL_GPU_INVALID_ID;
   uint32_t st = nvrm_control(rm, client, client, NV0000_CTRL_CMD_GPU_GET_PROBED_IDS,
                             &probed, sizeof(probed));
   if (st != NV_OK)
      return st;

   for (uint32_t i = 0; i < NV0000_CTRL_GPU_MAX_PROBED_GPUS; i++) {
      if (probed.gpuIds[i] == NV0000_CTRL_GPU_INVALID_ID)
         break;
      NV0000_CTRL_GPU_GET_PCI_INFO_PARAMS pci = {.gpuId = probed.gpuIds[i]};
      st = nvrm_control(rm, client, client, NV0000_CTRL_CMD_GPU_GET_PCI_INFO,
                        &pci, sizeof(pci));
      if (st != NV_OK)
         return st;
      /* Native PCI targets currently have no segment field. RM's command
       * supplies bus/slot but no function; the function remains the one
       * selected by the native endpoint, never a fabricated RM output. */
      if (pci.domain == 0 && pci.bus == (target >> 8) &&
          pci.slot == ((target >> 3) & 0x1f)) {
         *gpu_id = probed.gpuIds[i];
         return NV_OK;
      }
   }
   return NV_ERR_OBJECT_NOT_FOUND;
}

static uint32_t
query_gpu(struct nnrm *n)
{
   uint32_t id;
   uint32_t st = probed_gpu(n->rm, n->client, n->info.pci_target, &id);
   if (st != NV_OK)
      return st;
   n->gpu_id = id;

   /* A GPU has to be attached to the client before a device can use it */
   NV0000_CTRL_GPU_ATTACH_IDS_PARAMS attach;
   memset(&attach, 0, sizeof(attach));
   attach.gpuIds[0] = id;
   attach.gpuIds[1] = NV0000_CTRL_GPU_INVALID_ID;
   attach.failedId = NV0000_CTRL_GPU_INVALID_ID;
   st = rm_control(n, n->client, NV0000_CTRL_CMD_GPU_ATTACH_IDS, &attach, sizeof(attach));
   if (st != NV_OK)
      return st;

   NV0000_CTRL_GPU_GET_ID_INFO_V2_PARAMS idinfo;
   memset(&idinfo, 0, sizeof(idinfo));
   idinfo.gpuId = id;
   st = rm_control(n, n->client, NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2, &idinfo, sizeof(idinfo));
   if (st != NV_OK)
      return st;

   /* A GPU address space of our own ("share with myself"), with RM's internal
    * mappings fenced into their own range so NVK's fixed addresses never meet
    * them. */
   NV0080_ALLOC_PARAMETERS dev;
   memset(&dev, 0, sizeof(dev));
   dev.deviceId = idinfo.deviceInstance;
   dev.hClientShare = n->client;
   dev.flags = NV_DEVICE_ALLOCATION_FLAGS_RESTRICT_RESERVED_VALIMITS;
   dev.vaStartInternal = NNRM_RM_VA_START;
   dev.vaLimitInternal = NNRM_RM_VA_LIMIT;
   st = rm_alloc(n, n->client, &n->device, NV01_DEVICE_0, &dev, sizeof(dev));
   if (st != NV_OK)
      return st;

   NV2080_ALLOC_PARAMETERS sub;
   memset(&sub, 0, sizeof(sub));
   sub.subDeviceId = idinfo.subDeviceInstance;
   st = rm_alloc(n, n->device, &n->subdevice, NV20_SUBDEVICE_0, &sub, sizeof(sub));
   if (st != NV_OK)
      return st;

   NV2080_CTRL_BUS_GET_PCI_INFO_PARAMS bus;
   memset(&bus, 0, sizeof(bus));
   if (rm_control(n, n->subdevice, NV2080_CTRL_CMD_BUS_GET_PCI_INFO, &bus, sizeof(bus)) == NV_OK) {
      n->info.device_id = (uint16_t)(bus.pciDeviceId >> 16);
      n->info.subsystem_vendor_id = (uint16_t)(bus.pciSubSystemId & 0xffff);
      n->info.subsystem_id = (uint16_t)(bus.pciSubSystemId >> 16);
      n->info.revision_id = (uint8_t)bus.pciRevisionId;
   }

   NV2080_CTRL_MC_GET_ARCH_INFO_PARAMS arch;
   memset(&arch, 0, sizeof(arch));
   st = rm_control(n, n->subdevice, NV2080_CTRL_CMD_MC_GET_ARCH_INFO, &arch, sizeof(arch));
   if (st != NV_OK)
      return st;
   n->info.chipset = (uint16_t)(arch.architecture + arch.implementation);

   NV0080_CTRL_GPU_GET_CLASSLIST_V2_PARAMS cl;
   memset(&cl, 0, sizeof(cl));
   st = rm_control(n, n->device, NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2, &cl, sizeof(cl));
   if (st != NV_OK)
      return st;
   n->class_count = cl.numClasses < NV0080_CTRL_GPU_CLASSLIST_MAX_SIZE
                       ? cl.numClasses : NV0080_CTRL_GPU_CLASSLIST_MAX_SIZE;
   memcpy(n->classes, cl.classList, n->class_count * sizeof(uint32_t));

   n->info.cls_copy = (uint16_t)class_with_low_byte(n, 0xb5);
   n->info.cls_eng2d = (uint16_t)class_with_low_byte(n, 0x2d);
   n->info.cls_eng3d = (uint16_t)class_with_low_byte(n, 0x97);
   n->info.cls_m2mf = (uint16_t)(class_with_low_byte(n, 0x40) ? class_with_low_byte(n, 0x40)
                                                              : class_with_low_byte(n, 0x39));
   n->info.cls_compute = (uint16_t)class_with_low_byte(n, 0xc0);
   n->info.cls_gpfifo = (uint16_t)class_with_low_byte(n, 0x6f);
   /* Video decode would be a channel on the NVDEC engine; not wired up yet */
   n->info.cls_vdec = 0;

   /* Copy-only channels sit on an asynchronous copy engine, one that is not a
    * graphics channel's partner (GRCE); the first copy engine RM lists is the
    * fallback. */
   NV2080_CTRL_GPU_GET_ENGINES_V2_PARAMS eng;
   memset(&eng, 0, sizeof(eng));
   n->copy_engine_type = NV2080_ENGINE_TYPE_NULL;
   if (rm_control(n, n->subdevice, NV2080_CTRL_CMD_GPU_GET_ENGINES_V2, &eng, sizeof(eng)) == NV_OK) {
      for (uint32_t i = 0; i < eng.engineCount && i < NV2080_GPU_MAX_ENGINES_LIST_SIZE; i++) {
         if (NV2080_ENGINE_TYPE_IS_COPY(eng.engineList[i])) {
            n->copy_engine_type = eng.engineList[i];
            break;
         }
      }
   }
   NV2080_CTRL_CE_GET_ALL_CAPS_PARAMS ce;
   memset(&ce, 0, sizeof(ce));
   if (rm_control(n, n->subdevice, NV2080_CTRL_CMD_CE_GET_ALL_CAPS, &ce, sizeof(ce)) == NV_OK) {
      for (uint32_t i = 0; i < NV2080_CTRL_MAX_CES; i++) {
         if ((ce.present & (1ull << i)) &&
             !NV2080_CTRL_CE_GET_CAP(ce.capsTbl[i], NV2080_CTRL_CE_CAPS_CE_GRCE)) {
            n->copy_engine_type = NV2080_ENGINE_TYPE_COPY(i);
            break;
         }
      }
   }

   /* The copy engine that runs alongside graphics, for the copy object of a
    * graphics channel */
   NV2080_CTRL_GPU_GET_ENGINE_PARTNERLIST_PARAMS partner;
   memset(&partner, 0, sizeof(partner));
   partner.engineType = NV2080_ENGINE_TYPE_GRAPHICS;
   partner.partnershipClassId = n->info.cls_gpfifo;
   n->grce_engine_type = NV2080_ENGINE_TYPE_NULL;
   if (rm_control(n, n->subdevice, NV2080_CTRL_CMD_GPU_GET_ENGINE_PARTNERLIST,
                  &partner, sizeof(partner)) == NV_OK && partner.numPartners > 0)
      n->grce_engine_type = partner.partnerList[0];

   st = query_gr(n);
   if (st != NV_OK)
      return st;
   query_fb(n);
   return NV_OK;
}

static uint32_t
connect_device(uint32_t target, struct nvrm **out)
{
   int64_t status = nvrm_connect_device_status(target, out);
   switch (status) {
   case 0:
      /* RM registers probed IDs before adapter initialization finishes. A
       * diagnostic endpoint alone therefore cannot publish a usable GPU.
       * HELLO's explicit ready snapshot is fixed before the daemon listens. */
      if (nvrm_device_ready(*out))
         return NV_OK;
      nvrm_disconnect(*out);
      *out = NULL;
      return NV_ERR_OBJECT_NOT_FOUND;
   case -N_ERR_NOT_FOUND:
   case -N_ERR_NOT_READY: return NV_ERR_OBJECT_NOT_FOUND;
   case -N_ERR_NO_MEMORY: return NV_ERR_NO_MEMORY;
   case -N_ERR_CLOSED: return NV_ERR_GPU_IS_LOST;
   case -N_ERR_PROTOCOL: return NV_ERR_INVALID_STATE;
   default: return NV_ERR_OPERATING_SYSTEM;
   }
}

uint32_t
nnrm_enumerate_gpus(uint32_t **targets_out, uint32_t *count_out)
{
   *targets_out = NULL;
   *count_out = 0;
   struct n_pciinfo *list = NULL;
   uint32_t count;
   uint32_t st = pci_snapshot(&list, &count);
   if (st != NV_OK)
      return st;
   uint32_t *targets = count ? malloc((size_t)count * sizeof(*targets)) : NULL;
   if (count && targets == NULL) {
      free(list);
      return NV_ERR_NO_MEMORY;
   }
   uint32_t found = 0;
   for (uint32_t i = 0; i < count; i++) {
      if (!nvrm_pci_is_gpu(list[i].vendor, list[i].cls, list[i].subcls, list[i].progif))
         continue;
      uint32_t target = N_PCI_TARGET(list[i].bus, list[i].dev, list[i].func);
      struct nvrm *rm = NULL;
      st = connect_device(target, &rm);
      if (st == NV_ERR_OBJECT_NOT_FOUND)
         continue;
      if (st != NV_OK)
         goto fail;
      uint32_t client = 0, gpu_id;
      st = nvrm_alloc(rm, 0, 0, &client, NV01_ROOT_CLIENT, NULL, 0);
      if (st == NV_OK)
         st = probed_gpu(rm, client, target, &gpu_id);
      if (client != 0) {
         uint32_t cleanup = nvrm_free(rm, client, client, client);
         if (st == NV_OK)
            st = cleanup;
      }
      nvrm_disconnect(rm);
      if (st == NV_ERR_OBJECT_NOT_FOUND)
         continue;
      if (st != NV_OK)
         goto fail;
      targets[found++] = target;
   }
   free(list);
   *targets_out = targets;
   *count_out = found;
   return NV_OK;
fail:
   free(targets);
   free(list);
   return st;
}

uint32_t
nnrm_open_device(uint32_t target, struct nnrm **out, struct nnrm_gpu_info *info)
{
   *out = NULL;
   struct nvrm *rm = NULL;
   uint32_t st = connect_device(target, &rm);
   if (st != NV_OK)
      return st;

   struct nnrm *n = calloc(1, sizeof(*n));
   if (n == NULL) {
      nvrm_disconnect(rm);
      return NV_ERR_NO_MEMORY;
   }
   n->rm = rm;
   n->refs = 1;
   n->info.pci_target = target;
   atomic_init(&n->internal_va, NNRM_INTERNAL_VA);
   atomic_init(&n->daemon_lost, false);
   int lock_status = mtx_init(&n->usermode_lock, mtx_plain);
   if (lock_status != thrd_success) {
      free(n);
      nvrm_disconnect(rm);
      return lock_status == thrd_nomem ? NV_ERR_NO_MEMORY : NV_ERR_OPERATING_SYSTEM;
   }

   st = nvrm_alloc(rm, 0, 0, &n->client, NV01_ROOT_CLIENT, NULL, 0);
   if (st == NV_OK)
      st = query_gpu(n);

   if (st != NV_OK) {
      nnrm_close(n);
      return st;
   }

   n->info.pci_bus = target >> 8;
   n->info.pci_dev = (target >> 3) & 0x1f;
   n->info.pci_func = target & 0x7;
   if (info != NULL)
      *info = n->info;
   *out = n;
   return NV_OK;
}

void
nnrm_close(struct nnrm *n)
{
   if (n == NULL)
      return;

   mtx_lock(&n->usermode_lock);
   bool last = --n->refs == 0;
   mtx_unlock(&n->usermode_lock);
   if (!last)
      return;

   /* Every channel has been freed before its address-space reference. */
   if (n->posted_map != NULL)
      nnrm_mem_unmap(n->posted_mem, n->posted_map);
   nnrm_mem_free(n->posted_mem);
   if (n->usermode_map != NULL)
      nvrm_unmap(n->rm, n->client, n->subdevice, n->usermode_object,
                 (void *)n->usermode_map, 0);
   /* Freeing the client frees everything below it */
   if (n->client != 0)
      nvrm_free(n->rm, n->client, n->client, n->client);

   mtx_destroy(&n->usermode_lock);
   nvrm_disconnect(n->rm);
   free(n);
}

uint64_t
nnrm_vram_used(struct nnrm *n)
{
   NV2080_CTRL_FB_GET_INFO_V2_PARAMS fb = {0};
   fb.fbInfoListSize = 2;
   fb.fbInfoList[0].index = NV2080_CTRL_FB_INFO_INDEX_HEAP_SIZE;
   fb.fbInfoList[1].index = NV2080_CTRL_FB_INFO_INDEX_HEAP_FREE;
   if (rm_control(n, n->subdevice, NV2080_CTRL_CMD_FB_GET_INFO_V2, &fb, sizeof(fb)) != NV_OK)
      return 0;

   if (fb.fbInfoList[0].data < fb.fbInfoList[1].data)
      return 0;
   return (uint64_t)(fb.fbInfoList[0].data - fb.fbInfoList[1].data) * 1024;
}

uint64_t
nnrm_gpu_timestamp(struct nnrm *n)
{
   NV2080_CTRL_TIMER_GET_TIME_PARAMS t = {0};
   if (rm_control(n, n->subdevice, NV2080_CTRL_CMD_TIMER_GET_TIME, &t, sizeof(t)) != NV_OK)
      return 0;
   return t.time_nsec;
}

void
nnrm_va_range(struct nnrm *n, uint64_t *start, uint64_t *end)
{
   (void)n;
   *start = NNRM_VA_START;
   *end = NNRM_VA_END;
}

/* ------------------------------------------------------------------ memory */

struct nnrm_mem {
   struct nnrm *rm;
   uint32_t handle;
   enum nnrm_mem_kind kind;
   uint64_t size_B;
};

uint32_t
nnrm_mem_alloc(struct nnrm *n, enum nnrm_mem_kind kind,
               uint64_t size_B, uint64_t align_B, struct nnrm_mem **out)
{
   struct nnrm_mem *mem = calloc(1, sizeof(*mem));
   if (mem == NULL)
      return NV_ERR_NO_MEMORY;

   NV_MEMORY_ALLOCATION_PARAMS p;
   memset(&p, 0, sizeof(p));
   p.owner = NNRM_OWNER;
   p.type = NVOS32_TYPE_IMAGE;
   p.size = size_B;
   if (align_B > NNRM_PAGE_SIZE) {
      p.alignment = align_B;
      p.flags |= NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE;
   }

   if (kind == NNRM_MEM_SYSMEM) {
      /* Cached, snooped system memory: the GPU is coherent with x86 caches */
      p.attr = OS32_ATTR(_LOCATION, _PCI) | OS32_ATTR(_COHERENCY, _CACHED) |
               OS32_ATTR(_PAGE_SIZE, _4KB);
   } else {
      p.attr = OS32_ATTR(_LOCATION, _VIDMEM) | OS32_ATTR(_PAGE_SIZE, _4KB);
   }
   p.attr2 = DRF_DEF(OS32, _ATTR2, _ZBC, _PREFER_NO_ZBC);

   uint32_t st = rm_alloc(n, n->device, &mem->handle,
                          kind == NNRM_MEM_SYSMEM ? NV01_MEMORY_SYSTEM : NV01_MEMORY_LOCAL_USER,
                          &p, sizeof(p));
   if (st != NV_OK) {
      free(mem);
      return st;
   }

   mem->rm = n;
   mem->kind = kind;
   mem->size_B = size_B;
   *out = mem;
   return NV_OK;
}

void
nnrm_mem_free(struct nnrm_mem *mem)
{
   if (mem == NULL)
      return;
   rm_free(mem->rm, mem->rm->device, mem->handle);
   free(mem);
}

uint32_t
nnrm_mem_map(struct nnrm_mem *mem, void **map_out)
{
   struct nnrm *n = mem->rm;
   return nvrm_map(n->rm, n->client, n->device, mem->handle, 0, mem->size_B,
                   DRF_DEF(OS33, _FLAGS, _ACCESS, _READ_WRITE), map_out);
}

void
nnrm_mem_unmap(struct nnrm_mem *mem, void *map)
{
   struct nnrm *n = mem->rm;
   nvrm_unmap(n->rm, n->client, n->device, mem->handle, map, 0);
}

/* ------------------------------------------------------------------ GPU address space */

/* A piece of a virtual object that has memory mapped into it */
struct virt_piece {
   uint64_t off, len;
   uint32_t mem_handle;
};

struct nnrm_virt {
   struct nnrm *rm;
   uint32_t handle;
   uint64_t addr, size_B;
   bool sparse;

   /* Guards pieces/count/cap and makes "look up, change, tell RM" one step:
    * queues sharing a sparse buffer bind and unbind through here at once. */
   mtx_t lock;
   struct virt_piece *pieces; /* sorted by off, disjoint */
   uint32_t count, cap;
};

static uint32_t
virt_alloc_object(struct nnrm *n, uint64_t addr, uint64_t size_B, bool sparse,
                  uint32_t *handle)
{
   NV_MEMORY_ALLOCATION_PARAMS p;
   memset(&p, 0, sizeof(p));
   p.owner = NNRM_OWNER;
   p.type = NVOS32_TYPE_IMAGE;
   p.size = size_B;
   p.alignment = NNRM_PAGE_SIZE;
   p.offset = addr;
   p.hVASpace = 0; /* the device's default GPU address space */
   p.flags = NVOS32_ALLOC_FLAGS_VIRTUAL | NVOS32_ALLOC_FLAGS_FIXED_ADDRESS_ALLOCATE |
             NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE;
   if (sparse)
      p.flags |= NVOS32_ALLOC_FLAGS_SPARSE;
   else
      p.flags |= NVOS32_ALLOC_FLAGS_LAZY; /* page tables on map, not for the whole range */
   p.attr = OS32_ATTR(_PAGE_SIZE, _4KB);

   return rm_alloc(n, n->device, handle, NV50_MEMORY_VIRTUAL, &p, sizeof(p));
}

uint32_t
nnrm_virt_reserve(struct nnrm *n, uint64_t addr, uint64_t size_B,
                  bool sparse, struct nnrm_virt **out)
{
   struct nnrm_virt *v = calloc(1, sizeof(*v));
   if (v == NULL)
      return NV_ERR_NO_MEMORY;

   uint32_t st = virt_alloc_object(n, addr, size_B, sparse, &v->handle);
   if (st != NV_OK) {
      free(v);
      return st;
   }

   v->rm = n;
   v->addr = addr;
   v->size_B = size_B;
   v->sparse = sparse;
   mtx_init(&v->lock, mtx_plain);
   *out = v;
   return NV_OK;
}

uint32_t
nnrm_virt_release(struct nnrm_virt *v)
{
   if (v == NULL)
      return NV_OK;
   /* RM unmaps what is still mapped when the virtual object goes */
   rm_free(v->rm, v->rm->device, v->handle);
   mtx_destroy(&v->lock);
   free(v->pieces);
   free(v);
   return NV_OK;
}

/* Make room for `extra` more pieces.  Done before RM is told anything, so a
 * mapping that RM accepted can always be recorded. */
static uint32_t
piece_reserve(struct nnrm_virt *v, uint32_t extra)
{
   if (v->count + extra <= v->cap)
      return NV_OK;

   uint32_t cap = v->cap ? v->cap : 8;
   while (cap < v->count + extra)
      cap *= 2;
   struct virt_piece *grown = realloc(v->pieces, cap * sizeof(*grown));
   if (grown == NULL)
      return NV_ERR_NO_MEMORY;
   v->pieces = grown;
   v->cap = cap;
   return NV_OK;
}

/* Cannot fail: piece_reserve() has made the room */
static void
piece_insert(struct nnrm_virt *v, uint32_t at, struct virt_piece piece)
{
   memmove(&v->pieces[at + 1], &v->pieces[at], (v->count - at) * sizeof(*v->pieces));
   v->pieces[at] = piece;
   v->count++;
}

static uint32_t
rm_unmap_dma(struct nnrm_virt *v, uint64_t off, uint64_t len, uint32_t mem_handle)
{
   struct nnrm *n = v->rm;
   NVOS47_PARAMETERS p;
   memset(&p, 0, sizeof(p));
   p.hClient = n->client;
   p.hDevice = n->device;
   p.hDma = v->handle;
   p.hMemory = mem_handle;
   p.flags = 0;
   p.dmaOffset = v->addr + off;
   p.size = len;

   uint32_t st = nvrm_escape(n->rm, NV_ESC_RM_UNMAP_MEMORY_DMA, &p, sizeof(p));
   return st == NV_OK ? p.status : st;
}

/* v->lock held.  One contiguous cut splits at most one piece in two. */
static uint32_t
virt_unmap_locked(struct nnrm_virt *v, uint64_t off, uint64_t len)
{
   const uint64_t end = off + len;
   uint32_t result = piece_reserve(v, 1);
   if (result != NV_OK)
      return result; /* nothing was unmapped */

   for (uint32_t i = 0; i < v->count;) {
      struct virt_piece piece = v->pieces[i];
      const uint64_t piece_end = piece.off + piece.len;

      if (piece_end <= off) {
         i++;
         continue;
      }
      if (piece.off >= end)
         break;

      const uint64_t cut_lo = piece.off > off ? piece.off : off;
      const uint64_t cut_hi = piece_end < end ? piece_end : end;

      /* Memory freed while still mapped takes its mappings with it: RM no
       * longer knows the handle, and the piece is simply forgotten */
      uint32_t st = rm_unmap_dma(v, cut_lo, cut_hi - cut_lo, piece.mem_handle);
      if (st == NV_ERR_INVALID_OBJECT_HANDLE || st == NV_ERR_OBJECT_NOT_FOUND)
         st = NV_OK;
      if (st != NV_OK) {
         result = st;
         i++;
         continue;
      }

      /* Keep what is left on either side of the cut */
      const bool keep_lo = piece.off < cut_lo;
      const bool keep_hi = piece_end > cut_hi;
      if (keep_lo && keep_hi) {
         v->pieces[i].len = cut_lo - piece.off;
         struct virt_piece hi = {cut_hi, piece_end - cut_hi, piece.mem_handle};
         piece_insert(v, i + 1, hi);
         i += 2;
      } else if (keep_lo) {
         v->pieces[i].len = cut_lo - piece.off;
         i++;
      } else if (keep_hi) {
         v->pieces[i].off = cut_hi;
         v->pieces[i].len = piece_end - cut_hi;
         i++;
      } else {
         memmove(&v->pieces[i], &v->pieces[i + 1],
                 (v->count - i - 1) * sizeof(*v->pieces));
         v->count--;
      }
   }

   return result;
}

uint32_t
nnrm_virt_unmap(struct nnrm_virt *v, uint64_t off, uint64_t len)
{
   mtx_lock(&v->lock);
   uint32_t st = virt_unmap_locked(v, off, len);
   mtx_unlock(&v->lock);
   return st;
}

static uint32_t
virt_map_locked(struct nnrm_virt *v, uint64_t va_off, struct nnrm_mem *mem,
                uint64_t mem_off, uint64_t range, uint8_t pte_kind, uint32_t extra_flags)
{
   struct nnrm *n = v->rm;

   /* The unmap may split a piece and the new mapping adds one: room for both
    * up front, so no record can fail after RM has acted. */
   uint32_t st = piece_reserve(v, 2);
   if (st != NV_OK)
      return st;

   st = virt_unmap_locked(v, va_off, range);
   if (st != NV_OK)
      return st;

   NVOS46_PARAMETERS p;
   memset(&p, 0, sizeof(p));
   p.hClient = n->client;
   p.hDevice = n->device;
   p.hDma = v->handle;
   p.hMemory = mem->handle;
   p.offset = mem_off;
   p.length = range;
   p.flags = DRF_DEF(OS46, _FLAGS, _DMA_OFFSET_FIXED, _TRUE) |
             DRF_DEF(OS46, _FLAGS, _PAGE_SIZE, _4KB) |
             DRF_DEF(OS46, _FLAGS, _CACHE_SNOOP, _ENABLE) | extra_flags;
   if (pte_kind != 0) {
      p.flags |= DRF_DEF(OS46, _FLAGS, _PAGE_KIND_OVERRIDE, _YES);
      p.kindOverride = pte_kind;
   }
   p.dmaOffset = v->addr + va_off;

   st = nvrm_escape(n->rm, NV_ESC_RM_MAP_MEMORY_DMA, &p, sizeof(p));
   if (st == NV_OK)
      st = p.status;
   if (st != NV_OK)
      return st;

   uint32_t at = 0;
   while (at < v->count && v->pieces[at].off < va_off)
      at++;
   struct virt_piece piece = {va_off, range, mem->handle};
   piece_insert(v, at, piece);
   return NV_OK;
}

static uint32_t
virt_map(struct nnrm_virt *v, uint64_t va_off, struct nnrm_mem *mem,
         uint64_t mem_off, uint64_t range, uint8_t pte_kind, uint32_t extra_flags)
{
   mtx_lock(&v->lock);
   uint32_t st = virt_map_locked(v, va_off, mem, mem_off, range, pte_kind, extra_flags);
   mtx_unlock(&v->lock);
   return st;
}

uint32_t
nnrm_virt_map(struct nnrm_virt *v, uint64_t va_off, struct nnrm_mem *mem,
              uint64_t mem_off, uint64_t range, uint8_t pte_kind)
{
   return virt_map(v, va_off, mem, mem_off, range, pte_kind, 0);
}

/* ------------------------------------------------------------------ channels */

struct chan_buffer {
   struct nnrm_mem *mem;
   struct nnrm_virt *virt;
   void *map;
   uint64_t addr;
};

struct nnrm_chan {
   struct nnrm *rm;
   uint32_t channel; /* RM puts it in a channel group of its own */
   uint32_t objects[8];
   uint32_t object_count;

   /* GP FIFO ring (GPU-visible), error notifier and USERD (RM and CPU only) */
   struct chan_buffer gpfifo, notifier, userd;

   uint32_t entries; /* GP FIFO entries, a power of two */
   uint32_t put;     /* next entry to write, wraps at entries */
   uint32_t token;   /* work submit token, written to the doorbell */
};

static uint32_t
buffer_create(struct nnrm *n, struct chan_buffer *b, uint64_t size_B,
              bool gpu_visible, uint32_t gpu_map_flags)
{
   size_B = (size_B + NNRM_PAGE_SIZE - 1) & ~(uint64_t)(NNRM_PAGE_SIZE - 1);

   uint32_t st = nnrm_mem_alloc(n, NNRM_MEM_SYSMEM, size_B, NNRM_PAGE_SIZE, &b->mem);
   if (st != NV_OK)
      return st;

   if (gpu_visible) {
      if (size_B > NNRM_INTERNAL_STEP)
         return NV_ERR_INVALID_ARGUMENT;
      b->addr = atomic_fetch_add(&n->internal_va, NNRM_INTERNAL_STEP);
      if (b->addr + NNRM_INTERNAL_STEP > NNRM_INTERNAL_END)
         return NV_ERR_INSUFFICIENT_RESOURCES;
      st = nnrm_virt_reserve(n, b->addr, size_B, false, &b->virt);
      if (st == NV_OK)
         st = virt_map(b->virt, 0, b->mem, 0, size_B, 0, gpu_map_flags);
      if (st != NV_OK)
         return st;
   }

   st = nnrm_mem_map(b->mem, &b->map);
   if (st != NV_OK)
      return st;
   memset(b->map, 0, size_B);
   return NV_OK;
}

static void
buffer_destroy(struct chan_buffer *b)
{
   if (b->map != NULL)
      nnrm_mem_unmap(b->mem, b->map);
   nnrm_virt_release(b->virt);
   nnrm_mem_free(b->mem);
   memset(b, 0, sizeof(*b));
}

static uint32_t
usermode_map(struct nnrm *n)
{
   mtx_lock(&n->usermode_lock);
   uint32_t st = NV_OK;
   if (n->usermode_map == NULL) {
      /* Hopper and later ask for the BAR1 flavour; Volta..Ada take no parameters */
      void *params = NULL;
      uint32_t size = 0;
      uint32_t cls = VOLTA_USERMODE_A;
      NV_HOPPER_USERMODE_A_PARAMS hopper = {.bBar1Mapping = NV_TRUE, .bPriv = NV_FALSE};
      if (has_class(n, HOPPER_USERMODE_A)) {
         cls = HOPPER_USERMODE_A;
         params = &hopper;
         size = sizeof(hopper);
      }

      st = rm_alloc(n, n->subdevice, &n->usermode_object, cls, params, size);
      void *map = NULL;
      if (st == NV_OK)
         st = nvrm_map(n->rm, n->client, n->subdevice, n->usermode_object, 0,
                       NVC361_NV_USERMODE__SIZE,
                       DRF_DEF(OS33, _FLAGS, _ACCESS, _WRITE_ONLY), &map);
      if (st == NV_OK)
         n->usermode_map = map;
      else if (n->usermode_object != 0) {
         rm_free(n, n->subdevice, n->usermode_object);
         n->usermode_object = 0;
      }
   }
   mtx_unlock(&n->usermode_lock);
   return st;
}

static uint32_t
engine_object(struct nnrm *n, struct nnrm_chan *c, uint32_t cls, void *params, uint32_t size)
{
   uint32_t object = 0;
   uint32_t st = rm_alloc(n, c->channel, &object, cls, params, size);
   if (st == NV_OK)
      c->objects[c->object_count++] = object;
   return st;
}

static uint64_t
nnrm_now_ns(void)
{
   struct timespec ts;
   timespec_get(&ts, TIME_MONOTONIC);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Graphics engine objects take NV_GR_ALLOCATION_PARAMETERS (RM forwards them
 * to GSP as they are, so they are passed zeroed rather than left out) */
static uint32_t
gr_object(struct nnrm *n, struct nnrm_chan *c, uint32_t cls)
{
   NV_GR_ALLOCATION_PARAMETERS p;
   memset(&p, 0, sizeof(p));
   p.version = 2;
   p.size = sizeof(p);
   return engine_object(n, c, cls, &p, sizeof(p));
}

static uint32_t
posted_map(struct nnrm *n)
{
   mtx_lock(&n->usermode_lock);
   uint32_t st = NV_OK;
   if (n->posted_map == NULL) {
      struct nnrm_mem *mem = NULL;
      void *map = NULL;
      st = nnrm_mem_alloc(n, NNRM_MEM_VRAM, NNRM_PAGE_SIZE, NNRM_PAGE_SIZE, &mem);
      if (st == NV_OK)
         st = nnrm_mem_map(mem, &map);
      if (st == NV_OK) {
         n->posted_mem = mem;
         n->posted_map = map;
      } else {
         nnrm_mem_free(mem);
      }
   }
   mtx_unlock(&n->usermode_lock);
   return st;
}

static uint32_t
chan_setup(struct nnrm *n, struct nnrm_chan *c, uint32_t engines, bool gr,
           uint32_t engine_type)
{
   uint32_t st = usermode_map(n);
   if (st != NV_OK)
      return st;

   st = posted_map(n);
   if (st != NV_OK)
      return st;

   st = buffer_create(n, &c->gpfifo, (uint64_t)c->entries * NVC36F_GP_ENTRY__SIZE, true, 0);
   if (st != NV_OK)
      return st;
   st = buffer_create(n, &c->notifier,
                      sizeof(NvNotification) * NV_CHANNELGPFIFO_NOTIFICATION_TYPE__SIZE_1,
                      false, 0);
   if (st != NV_OK)
      return st;
   st = buffer_create(n, &c->userd, NNRM_PAGE_SIZE, false, 0);
   if (st != NV_OK)
      return st;

   /* No channel group and no VA space handle: RM makes a group of one and the
    * channel runs in the device's address space */
   NV_CHANNEL_ALLOC_PARAMS p;
   memset(&p, 0, sizeof(p));
   p.hObjectError = c->notifier.mem->handle;
   p.gpFifoOffset = c->gpfifo.addr;
   p.gpFifoEntries = c->entries;
   p.engineType = engine_type;
   p.hUserdMemory[0] = c->userd.mem->handle;
   p.userdOffset[0] = 0;
   st = rm_alloc(n, n->device, &c->channel, n->info.cls_gpfifo, &p, sizeof(p));
   if (st != NV_OK)
      return st;

   if (gr) {
      if ((engines & NNRM_CHAN_3D) && n->info.cls_eng3d != 0 &&
          (st = gr_object(n, c, n->info.cls_eng3d)) != NV_OK)
         return st;
      if ((engines & NNRM_CHAN_COMPUTE) && n->info.cls_compute != 0 &&
          (st = gr_object(n, c, n->info.cls_compute)) != NV_OK)
         return st;
      if ((engines & NNRM_CHAN_M2MF) && n->info.cls_m2mf != 0 &&
          (st = gr_object(n, c, n->info.cls_m2mf)) != NV_OK)
         return st;
      if ((engines & NNRM_CHAN_2D) && n->info.cls_eng2d != 0 &&
          (st = gr_object(n, c, n->info.cls_eng2d)) != NV_OK)
         return st;
   }

   if ((engines & NNRM_CHAN_COPY) && n->info.cls_copy != 0) {
      /* On a graphics channel the copy object is the graphics engine's partner */
      NVB0B5_ALLOCATION_PARAMETERS ce;
      memset(&ce, 0, sizeof(ce));
      ce.version = NVB0B5_ALLOCATION_PARAMETERS_VERSION_1;
      ce.engineType = gr ? n->grce_engine_type : engine_type;
      if (gr && ce.engineType == NV2080_ENGINE_TYPE_NULL)
         ce.engineType = n->copy_engine_type;
      st = engine_object(n, c, n->info.cls_copy, &ce, sizeof(ce));
      if (st != NV_OK)
         return st;
   }

   NVA06F_CTRL_GPFIFO_SCHEDULE_PARAMS sched;
   memset(&sched, 0, sizeof(sched));
   sched.bEnable = NV_TRUE;
   st = rm_control(n, c->channel, NVA06F_CTRL_CMD_GPFIFO_SCHEDULE, &sched, sizeof(sched));
   if (st != NV_OK)
      return st;

   NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN_PARAMS token;
   memset(&token, 0, sizeof(token));
   st = rm_control(n, c->channel, NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN,
                   &token, sizeof(token));
   if (st != NV_OK)
      return st;
   c->token = token.workSubmitToken;
   return NV_OK;
}

uint32_t
nnrm_chan_create(struct nnrm *n, uint32_t engines, struct nnrm_chan **out)
{
   const bool gr = engines & (NNRM_CHAN_3D | NNRM_CHAN_COMPUTE | NNRM_CHAN_2D | NNRM_CHAN_M2MF);
   const bool copy_only = (engines & NNRM_CHAN_COPY) && !gr;

   if (engines & NNRM_CHAN_VDEC)
      return NV_ERR_NOT_SUPPORTED;
   if (n->info.cls_gpfifo == 0)
      return NV_ERR_NOT_SUPPORTED;

   uint32_t engine_type = NV2080_ENGINE_TYPE_GRAPHICS;
   if (copy_only) {
      if (n->copy_engine_type == NV2080_ENGINE_TYPE_NULL)
         return NV_ERR_NOT_SUPPORTED;
      engine_type = n->copy_engine_type;
   }

   struct nnrm_chan *c = calloc(1, sizeof(*c));
   if (c == NULL)
      return NV_ERR_NO_MEMORY;
   mtx_lock(&n->usermode_lock);
   if (n->refs == SIZE_MAX) {
      mtx_unlock(&n->usermode_lock);
      free(c);
      return NV_ERR_INSUFFICIENT_RESOURCES;
   }
   n->refs++;
   mtx_unlock(&n->usermode_lock);
   c->rm = n;
   c->entries = NNRM_CHAN_GP_ENTRIES;

   uint32_t st = chan_setup(n, c, engines, gr, engine_type);
   if (st != NV_OK) {
      nnrm_chan_destroy(c);
      return st;
   }

   *out = c;
   return NV_OK;
}

void
nnrm_chan_destroy(struct nnrm_chan *c)
{
   if (c == NULL)
      return;

   /* Freeing the channel frees its engine objects and its channel group */
   if (c->channel != 0)
      rm_free(c->rm, c->rm->device, c->channel);

   buffer_destroy(&c->gpfifo);
   buffer_destroy(&c->notifier);
   buffer_destroy(&c->userd);
   struct nnrm *n = c->rm;
   free(c);
   nnrm_close(n);
}

static volatile Nvc36fControl *
chan_userd(struct nnrm_chan *c)
{
   return (volatile Nvc36fControl *)c->userd.map;
}

uint32_t
nnrm_chan_submit(struct nnrm_chan *c, const struct nnrm_gp *gps, uint32_t count)
{
   volatile uint32_t *ring = c->gpfifo.map;
   volatile Nvc36fControl *userd = chan_userd(c);
   volatile uint32_t *doorbell =
      (volatile uint32_t *)(c->rm->usermode_map + NVC361_NOTIFY_CHANNEL_PENDING);
   const uint32_t mask = c->entries - 1;

   uint32_t i = 0;
   while (i < count) {
      /* One entry stays free so that GPPut == GPGet always means "empty" */
      uint32_t get = userd->GPGet & mask;
      uint32_t space = (get - c->put - 1) & mask;

      if (space == 0) {
         if (nnrm_chan_has_error(c))
            return NV_ERR_RESET_REQUIRED;

         const uint64_t deadline = nnrm_now_ns() + NNRM_RING_WAIT_NS;
         while ((((userd->GPGet & mask) - c->put - 1) & mask) == 0) {
            if (nnrm_chan_has_error(c))
               return NV_ERR_RESET_REQUIRED;
            if (nnrm_now_ns() > deadline)
               return NV_ERR_TIMEOUT;
            thrd_yield();
         }
         continue;
      }

      for (; space > 0 && i < count; space--, i++) {
         const struct nnrm_gp *gp = &gps[i];
         ring[c->put * 2 + 0] = DRF_NUM(C36F, _GP_ENTRY0, _GET, (uint32_t)gp->addr >> 2);
         ring[c->put * 2 + 1] = DRF_NUM(C36F, _GP_ENTRY1, _GET_HI, (uint32_t)(gp->addr >> 32)) |
                                DRF_NUM(C36F, _GP_ENTRY1, _LENGTH, gp->size_B / 4) |
                                (gp->no_prefetch ? DRF_DEF(C36F, _GP_ENTRY1, _SYNC, _WAIT)
                                                 : DRF_DEF(C36F, _GP_ENTRY1, _SYNC, _PROCEED));
         c->put = (c->put + 1) & mask;
      }

      /* CPU ordering alone does not drain WC/UC PCI posted writes.  Order
       * the caller's VRAM stores, complete a read from this device's live
       * BAR1 allocation, then publish sysmem GPPut and ring the doorbell. */
      atomic_thread_fence(memory_order_seq_cst);
      (void)*(const volatile uint32_t *)c->rm->posted_map;
      atomic_thread_fence(memory_order_seq_cst);
      userd->GPPut = c->put;
      atomic_thread_fence(memory_order_seq_cst);
      *doorbell = c->token;
   }

   return NV_OK;
}

bool
nnrm_chan_has_error(struct nnrm_chan *c)
{
   const volatile NvNotification *note =
      (const volatile NvNotification *)c->notifier.map;
   const volatile NvNotification *err = &note[NV_CHANNELGPFIFO_NOTIFICATION_TYPE_ERROR];
   if (err->status != 0 || err->info32 != 0)
      return true;

   /* The daemon dying takes the RM client, and with it every channel, but
    * leaves no notifier entry behind: ask the connection too. */
   struct nnrm *n = c->rm;
   if (atomic_load(&n->daemon_lost))
      return true;
   if (!nvrm_alive(n->rm)) {
      atomic_store(&n->daemon_lost, true);
      return true;
   }
   return false;
}
