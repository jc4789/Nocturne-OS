#pragma once
#include <stddef.h>
/* Nocturne has no virtual memory API: all user memory is readable, writable and executable, so
   mprotect succeeds without doing anything and mmap only offers anonymous memory. */
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
