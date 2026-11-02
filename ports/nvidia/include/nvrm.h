#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* libnvrm: an app's connection to nvrm, the NVIDIA resource manager (nvrm_protocol.h).
   Every call returns RM's NV_STATUS (NV_OK is 0; nvstatuscodes.h in the RM SDK), and
   NV_ERR_OPERATING_SYSTEM when the driver cannot be reached. Calls on one connection are
   serialised, so threads may share it.

   Parameter blocks are copied into the connection's argument arena for the call and back out
   after it. A pointer inside a parameter block (a deprecated control's list) must point into the
   arena: take such buffers from nvrm_scratch inside nvrm_lock/nvrm_unlock. */
struct nvrm;

struct nvrm *nvrm_connect(void); /* NULL when no driver runs */
/* First adapter explicitly reported ready; diagnostic-only daemons are skipped. */
struct nvrm *nvrm_connect_ready(void);
/* nvrm_connect_ready that says why there is none: 0 with *out set; -N_ERR_NOT_FOUND when nvrm
   serves no NVIDIA GPU; otherwise why the first GPU nvrm serves could not be used:
   -N_ERR_NOT_READY when it answered but is not ready, or the failure to reach it
   (nvrm_connect_device_status). */
int64_t nvrm_connect_ready_status(struct nvrm **out);
/* Select a domain-zero PCI function, encoded by N_PCI_TARGET(bus, dev, func). */
struct nvrm *nvrm_connect_device(uint32_t pci_target);
/* 0 with *out set; otherwise a negative native N_ERR_* result. In particular,
   endpoint absence is not confused with OOM or an invalid protocol reply. */
int64_t nvrm_connect_device_status(uint32_t pci_target, struct nvrm **out);
/* Adapter state confirmed by this connection's HELLO snapshot. The daemon
   starts serving only after adapter initialization finishes. False includes a
   diagnostic-only daemon and old servers that do not explicitly report state. */
bool nvrm_device_ready(const struct nvrm *rm);
/* NVIDIA's display functions accepted by this RM port, per PCI class encoding. */
static inline bool nvrm_pci_is_gpu(uint16_t vendor, uint8_t cls, uint8_t subcls, uint8_t progif) {
    return vendor == 0x10de && cls == 0x03 && progif == 0x00 && (subcls == 0x00 || subcls == 0x02);
}
void nvrm_disconnect(struct nvrm *rm);

/* NV_ESC_RM_ALLOC: *object 0 asks RM for a handle (as for a new client: root 0, parent 0). */
uint32_t nvrm_alloc(struct nvrm *rm, uint32_t root, uint32_t parent, uint32_t *object, uint32_t cls,
                    void *params, uint32_t size);
uint32_t nvrm_free(struct nvrm *rm, uint32_t root, uint32_t parent, uint32_t object);
uint32_t nvrm_control(struct nvrm *rm, uint32_t client, uint32_t object, uint32_t cmd, void *params,
                      uint32_t size);
/* CPU mapping of memory object `memory` (NV_ESC_RM_MAP_MEMORY): *address receives this app's
   address of byte `offset`. nvrm_unmap takes that address back. */
uint32_t nvrm_map(struct nvrm *rm, uint32_t client, uint32_t device, uint32_t memory, uint64_t offset,
                  uint64_t length, uint32_t flags, void **address);
uint32_t nvrm_unmap(struct nvrm *rm, uint32_t client, uint32_t device, uint32_t memory, void *address,
                    uint32_t flags);
/* Any other escape whose argument struct holds no pointers (NVOS46 map DMA, NVOS47, ...). Returns
   rm_ioctl's NV_STATUS; the individual API's status is the struct's status field. */
uint32_t nvrm_escape(struct nvrm *rm, uint32_t escape, void *args, uint32_t size);

/* false once nvrm has gone (its end of the connection is closed); polling waits use it to
   give up. Safe to call from any thread, locked or not. */
bool nvrm_alive(struct nvrm *rm);

void nvrm_lock(struct nvrm *rm);
void nvrm_unlock(struct nvrm *rm);
/* Arena memory that stays valid until nvrm_unlock (NULL when the arena is full). */
void *nvrm_scratch(struct nvrm *rm, size_t bytes);

/* NVIDIA UVM, CUDA's unified memory (nvrm_protocol.h, NVRM_UVM), on the same connection. A UVM
   file is numbered per connection (UVM's parameter blocks call it an fd) and closes with it:
   minor 0 is nvidia-uvm, 1 nvidia-uvm-tools. Calls return UVM's NV_STATUS, NV_ERR_NOT_SUPPORTED
   when nvrm runs no UVM, NV_ERR_OPERATING_SYSTEM when the driver cannot be reached. */
uint32_t nvrm_uvm_open(struct nvrm *rm, uint32_t minor, int32_t *file);
uint32_t nvrm_uvm_close(struct nvrm *rm, int32_t file);
/* UVM's ioctl cmd (UVM_*): params is copied through the arena and back, as for nvrm_control;
   a pointer inside it must point into the arena (nvrm_scratch). The ioctl's own result is the
   block's rmStatus. */
uint32_t nvrm_uvm_ioctl(struct nvrm *rm, int32_t file, uint32_t cmd, void *params, uint32_t size);
/* A read/write mapping of `bytes` (whole pages) of UVM memory through file: *address gets where
   it is, which is also its offset in the file as UVM sees it. Its pages appear as UVM supplies
   them on first touch. nvrm_uvm_unmap takes it back; the connection's end does too. */
uint32_t nvrm_uvm_map(struct nvrm *rm, int32_t file, uint64_t bytes, void **address);
uint32_t nvrm_uvm_unmap(struct nvrm *rm, void *address);
/* Waits up to timeout_ms (-1: until there are some) for file's events: *events gets UVM's POLL*
   bits (POLLIN 1, POLLPRI 2, POLLOUT 4, ...), 0 when the time ran out. */
uint32_t nvrm_uvm_poll(struct nvrm *rm, int32_t file, int64_t timeout_ms, uint32_t *events);

/* Direct scanout (nvrm_protocol.h, NVRM_SCANOUT): images of the screen's size an app's GPU work
   writes into and the display shows instead of the desktop while the system lets the app's
   fullscreen window be shown directly. NV_ERR_NOT_SUPPORTED when nvrm shows no screen. */
uint32_t nvrm_scanout_create(struct nvrm *rm, uint32_t *image, uint32_t *width, uint32_t *height,
                             uint32_t *pitch, uint64_t *bytes);
uint32_t nvrm_scanout_destroy(struct nvrm *rm, uint32_t image);
/* NV_ERR_INVALID_STATE: the app is not shown directly now. */
uint32_t nvrm_scanout_present(struct nvrm *rm, uint32_t image);
/* The image as memory object *object (0: RM picks it) of RM client `client` made on this
   connection, under `parent`. */
uint32_t nvrm_scanout_import(struct nvrm *rm, uint32_t image, uint32_t client, uint32_t parent, uint32_t *object);
