#pragma once
#include "kernel.h"
struct regs;
struct task;
void syscall_dispatch(struct regs *r);
bool user_ok(const void *p, size_t n);   /* the process may read it */
bool user_ok_w(void *p, size_t n);       /* the process may write it */
/* Copies between the kernel and the calling app's memory: -N_ERR_BAD_ADDRESS where the app may not. */
int copy_from_user(void *dst, const void *usrc, size_t n);
int copy_to_user(void *udst, const void *src, size_t n);
int user_str(char *dst, const char *src, size_t max);
/* An app's string of any length (a path, an argument), copied into the kernel; kfree it after use. */
int user_string(const char *u, char **out);
void sys_shared_cleanup(struct task *owner);
/* Return [va, va+len) of the mmap region (with its guard page) for reuse. */
void mmap_hole_add(struct task *owner, uint64_t va, uint64_t len);
void mmap_holes_free(struct task *owner);
/* len bytes of the mmap region followed by an unmapped guard page; 0 if full. */
uint64_t mmap_reserve(struct task *owner, uint64_t len);
/* Maps a read-only program segment from frames shared by tasks running the same bytes. */
int shared_program_segment(struct task *t, const char *path, uint64_t va, const uint8_t *bytes,
                           uint64_t filesz, uint64_t memsz, bool exec);
bool shared_demand_fault(uint64_t addr, bool write); /* first touch of a reserved shared page */
/* A PCI claim's last handle closed (dev/pciuser.c): every process loses the device pages of its
   BAR objects' mappings. The address ranges stay reserved; a touch there reads all-ones, as a
   function gone from the bus does, and writes go nowhere. */
struct pci_claim;
void shared_revoke_claim(const struct pci_claim *c);
