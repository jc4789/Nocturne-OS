/* Networking placeholder: no NIC driver yet. */
#include "kernel.h"
#include "abi.h"

void net_init(void) {}

int64_t net_syscall(int num, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) { return -ENOSYS; }
