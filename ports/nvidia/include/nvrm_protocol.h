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

/* NVIDIA UVM (CUDA's unified memory, nvidia-uvm) through the same connection: one request, its
   argument a struct nvrm_uvm_request. A UVM file is numbered per connection (what UVM's
   parameter blocks call an fd) and closes with it. Mapping UVM memory takes two steps, because
   only the system chooses where a mapping goes: NVRM_UVM_MAP replies with paged shared memory
   attached (docs/nocturne-native-abi.md §7), which the client maps read/write wherever the
   system puts it and releases the handle of, and NVRM_UVM_MAP_COMMIT names that address (UVM
   requires a mapping's file offset to be its address). NVRM_UVM_UNMAP takes the mapping out of
   UVM before the client releases it; releasing it without that does the same, later. */
#define NVRM_UVM (NVRM_HELLO + 1u)
#define NVRM_UVM_OPEN 1u        /* minor 0 nvidia-uvm, 1 nvidia-uvm-tools: reply file */
#define NVRM_UVM_CLOSE 2u       /* file */
#define NVRM_UVM_IOCTL 3u       /* file, cmd (UVM_*), arg: the parameter block's address in the arena */
#define NVRM_UVM_MAP 4u         /* file, bytes: reply token, memory attached */
#define NVRM_UVM_MAP_COMMIT 5u  /* token, address */
#define NVRM_UVM_UNMAP 6u       /* address; address 0: token, a MAP not committed */
/* file, timeout_ms (-1 waits): reply events (UVM's POLL* bits). One request waits at most a
   second, so the connection's thread is never held longer; libnvrm repeats it for longer waits. */
#define NVRM_UVM_POLL 7u
struct nvrm_uvm_request {
    uint32_t op;
    int32_t file;
    uint32_t minor;
    uint32_t cmd;
    uint64_t arg;
    uint64_t address;
    uint64_t bytes;
    uint64_t token;
    int64_t timeout_ms;
    uint32_t status;        /* reply: NV_STATUS */
    uint32_t events;        /* reply to POLL */
};

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
