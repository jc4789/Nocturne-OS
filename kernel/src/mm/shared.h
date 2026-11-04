/* Shared memory: objects of pages that apps map, hand to each other and revoke, the paged memory
   an app of theirs supplies, and the read-only segments of programs (mm/shared.c, mm/pager.c,
   mm/program.c; the design is at the top of mm/shared.c). The calls here take no lock from their
   callers and may sleep, shared_fault's reasons named there. Any CPU, thread context, no spinlock
   held. */
#pragma once
#include "kernel.h"
#include "sys/wait.h"

struct handle;
struct object;
struct process;
struct hal_claim;
struct n_pager_fault;
struct n_pager_supply;

/* ---- objects ----
   A new object or view comes back as a handle of the caller's, *out; an object whose handle could
   not be made is gone again. The handles passed in stay the caller's. */

/* bytes (rounded up to pages) of zeroed RAM; demand: its frames come at the first touch. */
int64_t shared_create(size_t bytes, bool demand, struct handle **out);
/* Part of an object as a new one with its own caching. Device memory only uncached or
   write-combining, and not after its claim ended; DMA RAM in its one type (the first other type
   asked while nothing maps it becomes that type); other objects only as they are. maximum: 0, or
   the access the view allows at most. */
int64_t shared_view(struct handle *h, size_t offset, size_t bytes, uint32_t cache, uint32_t maximum, struct handle **out);
/* Page-aligned slices of objects concatenated, in array order. */
struct shared_slice {
    struct handle *handle;
    size_t offset, bytes;
};
int64_t shared_compose(const struct shared_slice *slices, size_t count, uint32_t access, struct handle **out);
/* The view or composite behind h, its descendants and what was composed of them become unusable:
   every mapping of theirs is unmapped and shot down, and the pages supplied from them unbound,
   before this returns. Only the app that made it may. */
int64_t shared_revoke(struct handle *h);
/* bytes of h's object (its whole size, rounded up; not 0) into the calling app's reservation
   region, N_MEMORY_READ or N_MEMORY_READ_WRITE: the address. */
int64_t shared_map(struct handle *h, size_t bytes, int access);
/* A claimed device's memory resource slot (hal_claim_memory): device memory, uncached unless a view
   says otherwise, write-combining only where the resource allows it. */
int64_t shared_hal_memory(struct handle *claim, uint32_t slot, struct handle **out);
/* Zeroed RAM a claimed device may reach by DMA, within the device's and its bus's address width
   (-N_ERR_UNSUPPORTED for a bus without DMA). A contiguous run is allocated whole. */
int64_t shared_dma_create(struct handle *claim, size_t bytes, uint32_t flags, struct handle **out);
/* How many pages of a DMA object or device range from page first may be asked of
   shared_dma_frames (count at most), or an error. */
int64_t shared_dma_pages(struct handle *h, size_t first, size_t count);
void shared_dma_frames(struct handle *h, size_t first, uint64_t *frames, size_t count);

/* A shared object of RAM (N_KIND_SHARED) the kernel reads and writes too (a window's surface,
   gui/wm.c): bytes rounded up to pages, zeroed, mapped into the kernel at *view. The caller holds
   its one reference: shared_kernel_end unmaps the view and drops that reference (the pages go
   with the last one, the apps' mappings and handles included). shared_kernel_handle opens a
   writable handle on it in the calling app: its number. */
struct object *shared_kernel_new(size_t bytes, void **view);
void shared_kernel_end(struct object *o, void *view);
int64_t shared_kernel_handle(struct object *o);
/* The same for memory a driver's device reads: a contiguous run below 2^bits, zeroed, *base its first
   frame, which the kernel reaches through the direct map. The caller holds its one reference
   (object_unref). */
struct object *shared_kernel_contig(size_t pages, unsigned bits, uint64_t *base);

/* A display driver's surface the screen is drawn into (n_screen_give, dev/screen_driver.c): bytes
   from offset of a writable shared object made through claim, a BAR's (which the system writes
   write-combined, or uncached without PAT) or a DMA object's (in its one type, which must not be
   write-back), views of them included. Open pins that type over the pages, as a mapping does, and
   holds the object; frames are the pages from the one offset lies in, type their PTE bits, and
   first_byte where offset is in the first. Close gives all of it back. */
struct shared_scanout {
    struct object *object;
    uint64_t *frames;
    size_t pages, first_byte;
    uint64_t type;
};
int64_t shared_scanout_open(struct handle *surface, const struct hal_claim *claim, uint64_t offset, uint64_t bytes,
                            struct shared_scanout *out);
void shared_scanout_close(struct shared_scanout *s);

/* What a claim keeps for the device memory made through it (in struct hal_claim, reached by
   hal_claim_shared): its memory objects, which its end withdraws. Zeroed memory is an empty one. */
struct shared_link;
struct shared_claim {
    struct mutex lock;
    struct shared_link *ranges, *ended;
};
/* A claim ended (hal/claim.c, which marked it released first): every process
   loses the device pages of its BAR objects' mappings, shot down before this returns, and a
   mapping still being made fails. The address ranges stay reserved; a touch there reads all-ones,
   as a function gone from the bus does, and writes go nowhere. */
void shared_revoke_claim(struct hal_claim *c);

/* ---- touches ---- */

/* A first touch of a reserved page of the calling app, or a write to a page without write access,
   which only a pager's page can be given: true when the access may be made now. A kernel copy
   and a page fault come here, interrupts on. It may wait for the disk or for a pager, *slept
   (when not NULL) saying so: a sibling may have unmapped what the caller checked before. False
   (after saying why when it is not the app's fault): no such page, or no memory. */
bool shared_fault(uint64_t addr, bool write, bool *slept);

/* The process's end, or a start that failed (no thread of it runs): its shared mappings and its
   space's records go; nothing is reserved in the space after. */
void shared_process_end(struct process *owner);

/* ---- paged memory (docs/nocturne-native-abi.md §7, mm/pager.c) ----
   Each call but pager_create takes the pager's handle, which pager_allowed checks. */

/* The memory and its pager, new: the caller's handles. */
int64_t pager_create(size_t bytes, struct handle **memory, struct handle **pager);
/* 0 when h is a pager's handle and the calling app its creator, else the error. */
int64_t pager_allowed(struct handle *h);
/* Up to max (not 0) faults not taken yet into out, ids[] telling them apart: how many. It waits
   until the deadline. */
int64_t pager_take(struct handle *h, struct n_pager_fault *out, uint64_t *ids, size_t max, uint64_t deadline);
/* The pager did not get these after all: they wait for its next take. */
void pager_untake(struct handle *h, const uint64_t *ids, size_t count);
/* One run of n_pager_supply, its copy in the kernel. */
int64_t pager_supply(struct handle *h, const struct n_pager_supply *run);
/* access 0: the pages lose their frames; N_MEMORY_READ: their write access. */
int64_t pager_revoke(struct handle *h, uint64_t offset, uint64_t bytes, uint32_t access);
/* The faults of [offset, offset+bytes) fail: their threads see an access violation. */
int64_t pager_fail(struct handle *h, uint64_t offset, uint64_t bytes);

/* ---- programs (mm/program.c) ----
   Programs whose read-only segments every process running them shares. A start holds the program
   its READ opening doc reads: the cached one when that content was read before, else a new one
   (NULL: the content has no identity to share it by, or no memory). program_map maps the segment
   at base + va (base page-aligned: a driver image's place differs by process, the frames do not),
   filesz bytes of doc from offset, into p from the program's frames: 0 when it is mapped,
   its pages read from doc (which the program holds on to) when a process first touches them; < 0
   when it is not shared (the start copies it). program_put ends the start's hold: a new program a
   start mapped completely joins the cache. */
struct program;
struct program *program_get(struct handle *doc);
int program_map(struct process *p, struct program *prog, struct handle *doc, uint64_t offset, uint64_t base,
                uint64_t va, uint64_t filesz, uint64_t memsz, bool exec);
void program_put(struct program *p, bool complete);
/* The frames of all idle programs: free for the asking. */
uint64_t programs_idle(void);
/* Whether pages more frames are free, once idle programs gave theirs back if not. */
bool frames_free(uint64_t pages);
