#pragma once
#include "kernel.h"
#include "sys/clock.h"
struct regs;
struct process;
/* Any CPU: the app's call in r, on the stack of the calling thread. */
void syscall_dispatch(struct regs *r);
/* An app's timeout (the ABI's milliseconds: negative for no limit, 0 for no wait) as the deadline
   the kernel's waits take (sys/wait.h), made once as the call comes in: CLOCK_NEVER for no limit,
   now for no wait. */
static inline uint64_t syscall_deadline(int64_t timeout_ms) {
    if (timeout_ms < 0 || (uint64_t)timeout_ms >= CLOCK_NEVER / NS_PER_MS) return CLOCK_NEVER;
    return clock_after((uint64_t)timeout_ms * NS_PER_MS);
}
/* The address waits' timeout: unsigned milliseconds, UINT32_MAX for no limit (the ABI's §7). */
static inline uint64_t address_deadline(unsigned timeout_ms) {
    return syscall_deadline(timeout_ms == UINT32_MAX ? -1 : (int64_t)timeout_ms);
}
/* The calling app's memory. A sibling thread on another CPU may unmap or protect any page of it at
   any time, so the kernel reads and writes it only through these copies, never through the app's
   pointer itself: a page that went meanwhile ends the copy with -N_ERR_BAD_ADDRESS instead of a
   fault. A page not there yet is made present first (a growing stack, a reserved page, a pager's
   page), which may sleep: never with a spinlock held. */
int copy_from_user(void *dst, const void *usrc, size_t n);
int copy_to_user(void *udst, const void *src, size_t n);
/* Whether [p, p+n) is memory the app may read (or, with _w, also write) now, its pages made
   present: a check before long work, which the copies after it still make again. */
bool user_ok(const void *p, size_t n);
bool user_ok_w(void *p, size_t n);
/* The NUL-terminated string at src into dst (at most max bytes with the NUL): its length, or an
   error (-N_ERR_NAME_TOO_LONG when it does not fit). */
int user_str(char *dst, const char *src, size_t max);
/* An app's string of any length (a path, an argument), copied into the kernel; kfree it after use. */
int user_string(const char *u, char **out);
/* A process's end, or a start that failed (no thread of it runs): its shared mappings and its
   space's records go; nothing is reserved in the space after. */
void sys_shared_cleanup(struct process *owner);
/* Any CPU, no lock held (they take owner's space lock, sys/process.h). Return [va, va+len) of the
   mmap region (with its guard page) for reuse; after the process's end nothing is taken back. */
void mmap_hole_add(struct process *owner, uint64_t va, uint64_t len);
/* len bytes of the mmap region followed by an unmapped guard page; 0 if full or after the end. */
uint64_t mmap_reserve(struct process *owner, uint64_t len);
/* Programs whose read-only segments every process running them shares (syscall.c). A start holds
   the program its READ opening doc reads: the cached one when that content was read before, else a
   new one (NULL: the content has no identity to share it by, or no memory). program_map maps the
   segment at va, filesz bytes of doc from offset, into p from the program's frames: 0 when it is
   mapped, its pages read from doc (which the program holds on to) when a process first touches
   them; < 0 when it is not shared (the start copies it). program_put ends the start's hold: a new
   program a start mapped completely joins the cache. */
struct handle;
struct program;
struct program *program_get(struct handle *doc);
int program_map(struct process *p, struct program *prog, struct handle *doc, uint64_t offset, uint64_t va,
                uint64_t filesz, uint64_t memsz, bool exec);
void program_put(struct program *p, bool complete);
/* First touch of a reserved shared page; false (after saying why) when it gets none: no memory, or
   a program page its document could not give. It may wait for the disk. */
bool shared_demand_fault(uint64_t addr, bool write);
/* A PCI claim's last handle closed (dev/pciuser.c, which marked it released first): every process
   loses the device pages of its BAR objects' mappings, shot down before this returns, and a
   mapping still being made fails. The address ranges stay reserved; a touch there reads all-ones,
   as a function gone from the bus does, and writes go nowhere. Any CPU, no spinlock held. */
struct pci_claim;
void shared_revoke_claim(const struct pci_claim *c);
/* A display driver's surface the screen is drawn into (n_pci_give_screen, dev/pciuser.c): bytes
   from offset of a writable shared object made through claim, a BAR's (which the system writes
   write-combined, or uncached without PAT) or a DMA object's (in its one type, which must not be
   write-back), views of them included. Open pins that type over the pages, as a mapping does, and
   holds the object; frames are the pages from the one offset lies in, type their PTE bits, and
   first_byte where offset is in the first. Close gives all of it back. Any CPU, no spinlock held. */
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
