#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "nocturne.h"
/* The boundary between nvrm's request server (nvrm_serve.c: clients, argument arenas, replies)
   and its RM OS layer (RM's state, rm_ioctl, memory). Protocol: include/nvrm_protocol.h. */

/* From the OS layer. A file is one client's RM state (RM's per-open-file private data); every
   request runs on RM's control device, as RM expects of an API client. */
void *nvrm_rm_file_open(bool admin); /* admin: RM treats the client's requests as an administrator's */
void nvrm_rm_file_close(void *file); /* frees everything the client still holds in RM */
uint32_t nvrm_rm_ioctl(void *file, uint32_t escape, void *args, uint32_t size); /* NV_STATUS */
/* After a successful NV_ESC_RM_MAP_MEMORY: what RM recorded for the mapping, as a new shared
   object handle (a view of a BAR or of DMA memory) with the mapping's offset and length in it;
   -1 when RM recorded none. */
n_handle nvrm_rm_mapping_object(uint64_t *offset, uint64_t *bytes);

/* From the server, for os_memcpy_from_user / os_memcpy_to_user: nvrm's address of n bytes of the
   current client's arena at the client's address `user`, or NULL outside it. */
void *nvrm_user_translate(const void *user, size_t n);
/* Serve clients until the channel cannot be listened on; returns the reason. */
n_result nvrm_serve(void);
