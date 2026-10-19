#pragma once
#include <stdint.h>
/* The channel between /bin/nvrm, the NVIDIA resource manager running as an app, and its clients
   (libnvrm, include/nvrm.h). It is Nocturne's own transport for RM's escapes, not a device file.

   A client connects to NVRM_CHANNEL and sends NVRM_HELLO with its argument arena attached: a
   shared object the client has mapped at arena_address. nvrm maps it too. Every later request is
   one message: the header, then the escape's argument struct exactly as RM's rm_ioctl takes it
   (NVOS21_PARAMETERS, nv_ioctl_nvos33_parameters_with_fd, ...). Pointers inside those arguments
   are client addresses inside the arena; nvrm translates them and refuses any other address, so
   no app reaches into another's memory (docs/nvidia/LEDGER.md D4). The reply is the header and
   the argument struct as RM left it. A reply to NV_ESC_RM_MAP_MEMORY carries the mapped memory as
   an attached shared object: `offset` is where the mapping starts in it, `bytes` its length. */
#define NVRM_CHANNEL "nocturne.nvidia.rm"
#define NVRM_PROTOCOL 1
#define NVRM_HELLO 0x1000u   /* escapes below 0x1000 are RM's NV_ESC_* numbers */
#define NVRM_ARGS_MAX 1024u  /* the argument struct, not the arena's contents */

struct nvrm_message {
    uint32_t protocol;      /* NVRM_PROTOCOL */
    uint32_t escape;        /* NV_ESC_* or NVRM_HELLO */
    uint32_t size;          /* argument bytes after the header */
    int32_t status;         /* reply: 0, or a negative n_result when the request was not run */
    uint64_t arena_address; /* hello: where the client mapped its arena */
    uint64_t arena_bytes;
    uint64_t offset, bytes; /* map reply: the mapping inside the attached object */
};
