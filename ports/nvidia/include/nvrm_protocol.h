#pragma once
#include <stdint.h>
/* The channel between nvrm, the NVIDIA resource manager running as an app, and its clients
   (libnvrm, include/nvrm.h). It is Nocturne's own transport for RM's escapes, not a device file.

   A client connects to the PCI-function-specific channel and sends NVRM_HELLO with its argument arena attached: a
   shared object the client has mapped at arena_address. nvrm maps it too. Every later request is
   one message: the header, then the escape's argument struct exactly as RM's rm_ioctl takes it
   (NVOS21_PARAMETERS, nv_ioctl_nvos33_parameters_with_fd, ...). Pointers inside those arguments
   are client addresses inside the arena; nvrm translates them and refuses any other address, so
   no app reaches into another's memory (docs/nvidia/LEDGER.md D4). The reply is the header and
   the argument struct as RM left it. A reply to NV_ESC_RM_MAP_MEMORY carries the mapped memory as
   an attached shared object: `offset` is where the mapping starts in it, `bytes` its length. */
#define NVRM_CHANNEL "nocturne.nvidia.rm"
#define NVRM_CHANNEL_FORMAT NVRM_CHANNEL ".%04x"
/* Domain zero PCI BDF is a 16-bit N_PCI_TARGET. Include the dot, hex digits and NUL. */
#define NVRM_CHANNEL_NAME_BYTES (sizeof(NVRM_CHANNEL) + 1 + 2 * sizeof(uint16_t))
#define NVRM_PROTOCOL 1
#define NVRM_HELLO 0x1000u   /* escapes below 0x1000 are RM's NV_ESC_* numbers */
/* HELLO reply capabilities in access (arena_bytes on the request). Old arena
   sizes are page aligned, so these low bits cannot be mistaken for an echo. */
#define NVRM_HELLO_STATUS_PRESENT (UINT64_C(1) << 0)
#define NVRM_HELLO_ADAPTER_READY (UINT64_C(1) << 1)

struct nvrm_message {
    uint32_t protocol;      /* NVRM_PROTOCOL */
    uint32_t escape;        /* NV_ESC_* or NVRM_HELLO */
    uint32_t size;          /* argument bytes after the header */
    int32_t status;         /* reply: 0, or negative n_result; old clients fail closed on RM errors */
    /* Preserve Protocol1's size/offsets for statically linked apps. Hello uses
       arena fields; later replies use those previously-unused slots for status
       and maximum mapping access. RM errors also set status<0 for old clients,
       but keep the updated argument block, including partial-success handles. */
    union { uint64_t arena_address; uint64_t rm_status; };
    union { uint64_t arena_bytes; uint64_t access; };
    uint64_t offset, bytes; /* map reply: the mapping inside the attached object */
};
