#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* libnvrm: an app's connection to /bin/nvrm, the NVIDIA resource manager (nvrm_protocol.h).
   Every call returns RM's NV_STATUS (NV_OK is 0; nvstatuscodes.h in the RM SDK), and
   NV_ERR_OPERATING_SYSTEM when the driver cannot be reached. Calls on one connection are
   serialised, so threads may share it.

   Parameter blocks are copied into the connection's argument arena for the call and back out
   after it. A pointer inside a parameter block (a deprecated control's list) must point into the
   arena: take such buffers from nvrm_scratch inside nvrm_lock/nvrm_unlock. */
struct nvrm;

struct nvrm *nvrm_connect(void); /* NULL when no driver runs */
/* Select a domain-zero PCI function, encoded by N_PCI_TARGET(bus, dev, func). */
struct nvrm *nvrm_connect_device(uint32_t pci_target);
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

/* false once /bin/nvrm has gone (its end of the connection is closed); polling waits use it to
   give up. Safe to call from any thread, locked or not. */
bool nvrm_alive(struct nvrm *rm);

void nvrm_lock(struct nvrm *rm);
void nvrm_unlock(struct nvrm *rm);
/* Arena memory that stays valid until nvrm_unlock (NULL when the arena is full). */
void *nvrm_scratch(struct nvrm *rm, size_t bytes);
