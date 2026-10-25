#pragma once
#include "kernel.h"
struct regs;
struct task;
void syscall_dispatch(struct regs *r);
/* An app's syscall on an AP (sched_ap_interrupt), served there when it changes no BSP state:
   identity and clock queries, network copies of mapped memory, polls that find nothing to take
   or only read a seqcount, and a window's state (AP read sections, arch/smp.h). false: the BSP
   serves it. */
bool syscall_ap(struct regs *r);
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
/* Programs whose read-only segments every task running them shares (syscall.c). A start holds the
   program its READ opening doc reads: the cached one when that content was read before, else a new
   one (NULL: the content has no identity to share it by, or no memory). program_map maps the
   segment at va, filesz bytes of doc from offset, into t from the program's frames: 0 when it is
   mapped, its pages read from doc (which the program holds on to) when a task first touches them;
   < 0 when it is not shared (the start copies it). program_put ends the start's hold: a new
   program a start mapped completely joins the cache. */
struct handle;
struct program;
struct program *program_get(struct handle *doc);
int program_map(struct task *t, struct program *p, struct handle *doc, uint64_t offset, uint64_t va, uint64_t filesz,
                uint64_t memsz, bool exec);
void program_put(struct program *p, bool complete);
/* First touch of a reserved shared page; false (after saying why) when it gets none: no memory, or
   a program page its document could not give. It may wait for the disk. */
bool shared_demand_fault(uint64_t addr, bool write);
/* A PCI claim's last handle closed (dev/pciuser.c): every process loses the device pages of its
   BAR objects' mappings. The address ranges stay reserved; a touch there reads all-ones, as a
   function gone from the bus does, and writes go nowhere. */
struct pci_claim;
void shared_revoke_claim(const struct pci_claim *c);
/* A display driver's surface the screen is drawn into (n_pci_give_screen, dev/pciuser.c): bytes
   from offset of a writable shared object made through claim, a BAR's (which the system writes
   write-combined, or uncached without PAT) or a DMA object's (in its one type, which must not be
   write-back), views of them included. Open pins that type over the pages, as a mapping does, and
   holds the object; frames are the pages from the one offset lies in, type their PTE bits, and
   first_byte where offset is in the first. Close gives all of it back. BSP. */
struct object;
struct shared_scanout {
    struct object *object;
    uint64_t *frames;
    size_t pages, first_byte;
    uint64_t type;
};
int64_t shared_scanout_open(struct handle *surface, const struct pci_claim *claim, uint64_t offset, uint64_t bytes,
                            struct shared_scanout *out);
void shared_scanout_close(struct shared_scanout *s);
