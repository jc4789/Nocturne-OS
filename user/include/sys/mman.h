#pragma once
#include <stddef.h>
/* Anonymous mappings only (no files). Memory is always readable, so PROT_NONE means read-only.
   Pages are never writable and executable unless asked for: code is read-only, data and the
   stack are non-executable (W^X). mprotect works on any page of the process. */
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_FAILED ((void *)-1)
void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off);
int munmap(void *addr, size_t len);
int mprotect(void *addr, size_t len, int prot);
