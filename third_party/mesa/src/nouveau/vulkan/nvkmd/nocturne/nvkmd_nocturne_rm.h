/*
 * SPDX-License-Identifier: MIT
 *
 * Nocturne's NVKMD backend: the boundary to NVIDIA's resource manager (RM).
 *
 * Everything that needs an RM header (object classes, control commands, NVOS
 * parameter blocks) lives behind this interface in nvkmd_nocturne_rm.c, which
 * is the only file that includes them.  The rest of the backend speaks plain C:
 * sizes, addresses and opaque handles.  Every call below reaches RM through
 * libnvrm (ports/nvidia/include/nvrm.h), i.e. through the /bin/nvrm app; there
 * is no device file and no ioctl.
 *
 * Calls return 0 on success and an RM NV_STATUS otherwise; nnrm_vk_result()
 * maps it to a VkResult.
 */
#ifndef NVKMD_NOCTURNE_RM_H
#define NVKMD_NOCTURNE_RM_H 1

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct nnrm;       /* one RM client with a device, a subdevice and a GPU address space */
struct nnrm_mem;   /* physical memory (system or video) */
struct nnrm_virt;  /* a reserved range of the GPU address space */
struct nnrm_chan;  /* a GPFIFO channel with its engine objects and doorbell */

#define NNRM_PAGE_SIZE 4096u

/* What the GPU is, from NV01_ROOT/NV01_DEVICE_0/NV20_SUBDEVICE_0 controls */
struct nnrm_gpu_info {
   uint16_t device_id;
   uint16_t subsystem_id;
   uint16_t subsystem_vendor_id;
   uint8_t revision_id;

   uint16_t pci_domain;
   uint8_t pci_bus, pci_dev, pci_func;

   /* architecture | implementation, i.e. the chipset number NVK keys on
    * (0x192 for AD102). */
   uint16_t chipset;

   uint8_t gpc_count;
   uint16_t tpc_count;

   uint64_t vram_size_B;
   uint64_t bar_size_B;

   /* Highest class of each engine this GPU offers (0 when it has none) */
   uint16_t cls_copy, cls_eng2d, cls_eng3d, cls_m2mf, cls_compute;
   uint16_t cls_gpfifo, cls_vdec;
};

/* Number of GPUs RM has probed (0 when /bin/nvrm cannot be reached) */
uint32_t nnrm_gpu_count(void);

/* Open GPU `gpu_index`: a new RM client, the device and subdevice, the
 * default GPU address space.  `info` may be NULL. */
uint32_t nnrm_open(uint32_t gpu_index, struct nnrm **out, struct nnrm_gpu_info *info);
void nnrm_close(struct nnrm *rm);

/* VRAM in use (bytes), 0 when RM does not say */
uint64_t nnrm_vram_used(struct nnrm *rm);
/* The GPU's global timer in nanoseconds */
uint64_t nnrm_gpu_timestamp(struct nnrm *rm);

/* The usable GPU address range [*start, *end) for this address space */
void nnrm_va_range(struct nnrm *rm, uint64_t *start, uint64_t *end);

/* ---- physical memory ---- */

enum nnrm_mem_kind {
   NNRM_MEM_VRAM,
   NNRM_MEM_SYSMEM,
};

uint32_t nnrm_mem_alloc(struct nnrm *rm, enum nnrm_mem_kind kind,
                        uint64_t size_B, uint64_t align_B, struct nnrm_mem **out);
void nnrm_mem_free(struct nnrm_mem *mem);

/* CPU mapping of the whole object (unmapped by nnrm_mem_unmap) */
uint32_t nnrm_mem_map(struct nnrm_mem *mem, void **map_out);
void nnrm_mem_unmap(struct nnrm_mem *mem, void *map);

/* ---- GPU address space ---- */

/* Reserve [addr, addr + size_B) as one virtual object.  `sparse` makes the
 * unmapped parts read as zero and swallow writes instead of faulting. */
uint32_t nnrm_virt_reserve(struct nnrm *rm, uint64_t addr, uint64_t size_B,
                           bool sparse, struct nnrm_virt **out);
/* Unmaps everything still mapped and frees the reservation */
uint32_t nnrm_virt_release(struct nnrm_virt *virt);

/* Map [mem_offset_B, mem_offset_B + range_B) of `mem` at virt->addr + va_offset_B.
 * Anything mapped in that range is replaced. */
uint32_t nnrm_virt_map(struct nnrm_virt *virt, uint64_t va_offset_B,
                       struct nnrm_mem *mem, uint64_t mem_offset_B,
                       uint64_t range_B, uint8_t pte_kind);
uint32_t nnrm_virt_unmap(struct nnrm_virt *virt, uint64_t va_offset_B, uint64_t range_B);

/* ---- channels ---- */

/* Which engines a channel binds, one bit each (the order of enum nvkmd_engines) */
#define NNRM_CHAN_COPY    (1u << 0)
#define NNRM_CHAN_2D      (1u << 1)
#define NNRM_CHAN_3D      (1u << 2)
#define NNRM_CHAN_M2MF    (1u << 3)
#define NNRM_CHAN_COMPUTE (1u << 4)
#define NNRM_CHAN_VDEC    (1u << 6)

#define NNRM_CHAN_GP_ENTRIES 1024u

/* One GP entry: `size_B` bytes of push buffer at GPU address `addr` */
struct nnrm_gp {
   uint64_t addr;
   uint32_t size_B;
   bool no_prefetch;
};

uint32_t nnrm_chan_create(struct nnrm *rm, uint32_t engines, struct nnrm_chan **out);
void nnrm_chan_destroy(struct nnrm_chan *chan);

/* Queue GP entries and ring the doorbell.  Waits (bounded polling) for ring
 * space.  The caller serialises calls on one channel. */
uint32_t nnrm_chan_submit(struct nnrm_chan *chan, const struct nnrm_gp *gps, uint32_t count);

/* true once the channel has hit an error (RM reset it) or /bin/nvrm has gone */
bool nnrm_chan_has_error(struct nnrm_chan *chan);

/* ---- results ---- */

/* VkResult for an RM status; `out_of_device` picks the memory error flavour */
int nnrm_vk_result(uint32_t nv_status, bool out_of_device);
const char *nnrm_status_name(uint32_t nv_status);

#endif /* NVKMD_NOCTURNE_RM_H */
