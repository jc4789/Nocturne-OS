/* System call dispatch (int 0x80). */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/smp.h"
#include "vm/vm.h"
#include "sys/sched.h"
#include "sys/syscall.h"
#include "sys/proc.h"
#include "sys/events.h"
#include "sys/object.h"
#include "sys/channel.h"
#include "sys/stream.h"
#include "sys/usersettings.h"
#include "sys/watch.h"
#include "fs/store.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "dev/timer.h"
#include "dev/fb.h"
#include "dev/gpu.h"
#include "dev/audio.h"
#include "dev/entropy.h"
#include "dev/pciuser.h"
#include "arch/pat.h"
#include "gui/wm.h"
#include "net/net.h"
#include "dev/pm.h"
#include "abi.h"
_Static_assert(PAGE_SIZE == N_PAGE_SIZE, "native mapping granule ABI");

_Static_assert(sizeof(struct n_gpu_blit) == 32, "GPU blit ABI");
_Static_assert(SYS_WATCH_TAKE == 207 && SYS_AUDIO_OUTPUT_WATCH < SYS_MAX, "every system call number is below SYS_MAX");
_Static_assert(sizeof(struct n_pager_fault) == 16 && sizeof(struct n_pager_supply) == 32, "pager ABI");

extern char cpu_brand[64];
int pci_list(struct n_pciinfo *out, int max);
void vmm_user_pages_split(uint64_t pml4, uint64_t out[6]);
int64_t net_syscall(int num, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e);

/* ---- user memory access ---- */

static bool shared_fault(uint64_t addr, bool write, bool *slept);

/* Is [p, p+n) user memory the process may read (or, with write, also write)? Missing stack
   pages are faulted in. The kernel must not write to a read-only user page itself: CR0.WP is on,
   so that would be a kernel page fault. A program page read from its document lets go of the
   caller's guard while the disk works: a sibling may unmap what was checked before it, so the
   check starts over, and the caller's copy follows a pass that did not sleep. */
static bool user_access(const void *p, size_t n, bool write) {
    uint64_t a = (uint64_t)p;
    if (n == 0) return true;
    if (a >= USER_TOP || a + n > USER_TOP || a + n < a) return false;
    struct task *t = current_task;
    uint64_t pg = ALIGN_DOWN(a, PAGE_SIZE);
    while (pg < a + n) {
        uint64_t pte = vmm_get_pte(t->pml4, pg);
        if (!(pte & PTE_P)) {
            if (pg >= USER_STACK_TOP - USER_STACK_MAX && pg < USER_STACK_TOP) {
                if (vmm_user_alloc(t->pml4, pg, PAGE_SIZE, VM_W) < 0) return false;
                pg += PAGE_SIZE;
                continue;
            }
            bool slept = false;
            if (!shared_fault(pg, write, &slept)) return false;
            if (slept) {
                pg = ALIGN_DOWN(a, PAGE_SIZE);
                continue;
            }
            pte = vmm_get_pte(t->pml4, pg);
        }
        if (!(pte & PTE_U)) return false;
        if (write && !(pte & PTE_W)) {
            /* a pager's page may be given write access; it is looked at again after */
            bool slept = false;
            if (!(pte & PTE_SHARED) || !shared_fault(pg, true, &slept)) return false;
            if (slept) pg = ALIGN_DOWN(a, PAGE_SIZE);
            continue;
        }
        pg += PAGE_SIZE;
    }
    return true;
}

bool user_ok(const void *p, size_t n) { return user_access(p, n, false); }
bool user_ok_w(void *p, size_t n) { return user_access(p, n, true); }

int user_str(char *dst, const char *src, size_t max) {
    if (!max) return -N_ERR_NAME_TOO_LONG;
    size_t i = 0;
    while (i < max) {
        uint64_t address = (uint64_t)src + i;
        if (address < (uint64_t)src || !user_ok((const void *)address, 1)) return -N_ERR_BAD_ADDRESS;
        size_t end = i + MIN(max - i, PAGE_SIZE - (address & (PAGE_SIZE - 1)));
        /* Permission is page-granular. Do not touch the next page until its
           PTE has been checked, even if the string terminates at this edge. */
        while (i < end) {
            char c = *(const char *)((uint64_t)src + i);
            dst[i++] = c;
            if (!c) return (int)(i - 1);
        }
    }
    dst[max - 1] = 0;
    return -N_ERR_NAME_TOO_LONG;
}

int user_string(const char *u, char **out) {
    /* its length first, a page at a time, then a copy of that many bytes */
    size_t n = 0;
    for (;;) {
        uint64_t address = (uint64_t)u + n;
        if (address < (uint64_t)u) return -N_ERR_BAD_ADDRESS;
        size_t end = PAGE_SIZE - (address & (PAGE_SIZE - 1)), i = 0;
        uint64_t flags = irq_save();
        bool ok = user_ok((const void *)address, 1);
        if (ok) while (i < end && ((const char *)address)[i]) i++;
        irq_restore(flags);
        if (!ok) return -N_ERR_BAD_ADDRESS;
        n += i;
        if (i < end) break;
        if (need_resched) schedule();
        if (current_task->killed) return -N_ERR_STOPPED;
    }
    char *s = kmalloc(n + 1);
    if (!s) return -N_ERR_NO_MEMORY;
    /* a sibling may change or unmap it meanwhile: the copy is checked again */
    int r = copy_from_user(s, u, n);
    if (r < 0) { kfree(s); return r; }
    s[n] = 0;
    *out = s;
    return 0;
}


/* ---- handlers ---- */

/* Physical memory ran out under a memory request: name the largest user
   processes once per interval, so the owner of the memory is visible. */
static void report_memory_exhaustion(struct task *t, int64_t inc) {
    static uint64_t last_report;
    uint64_t now = uptime_ms();
    if (last_report && now - last_report < 10000) return;
    last_report = now;
    struct { int pid; char name[32]; uint64_t kb; } top[8];
    int n = 0;
    uint64_t flags = irq_save();
    for (struct task *p = task_list; p; p = p->all_next) {
        if (!p->is_user || p->is_thread) continue;
        uint64_t kb = proc_user_pages(p) * 4;
        int at = n < 8 ? n++ : 8;
        while (at > 0 && top[at - 1].kb < kb) { if (at < 8) top[at] = top[at - 1]; at--; }
        if (at < 8) { top[at].pid = p->pid; strlcpy(top[at].name, p->name, sizeof top[at].name); top[at].kb = kb; }
    }
    irq_restore(flags);
    kprintf("[memory] pid %d (%s): no pages for %ld more bytes, %lu pages free; largest:",
            t->pid, t->name, (long)inc, (unsigned long)pmm_free_pages());
    for (int i = 0; i < n; i++) kprintf(" %d/%s %luK", top[i].pid, top[i].name, (unsigned long)top[i].kb);
    kprintf("\n");
    /* Where the private pages sit, for every process: many middling ones can
       outweigh the largest. */
    flags = irq_save();
    for (struct task *p = task_list; p; p = p->all_next) {
        if (!p->is_user || p->is_thread) continue;
        uint64_t s[6];
        unsigned threads = 0;
        vmm_user_pages_split(p->pml4, s);
        for (struct task *q = task_list; q; q = q->all_next)
            if (q->is_thread && q->process == p && q->state != TASK_ZOMBIE) threads++;
        kprintf("[memory]  %d/%s image %luK low %luK reserved %luK stack %luK shared %luK tables %luK threads %u\n",
                p->pid, p->name, (unsigned long)s[0] * 4, (unsigned long)s[1] * 4, (unsigned long)s[2] * 4,
                (unsigned long)s[3] * 4, (unsigned long)s[4] * 4, (unsigned long)s[5] * 4, threads);
    }
    irq_restore(flags);
}

/* Snapshot the explicit native key length once; never rescan mutable user
 * strings in the registry, and never constrain identity to a guessed path cap. */
static int64_t sys_channel_named(const char *ukey,size_t length,bool listen) {
    if(!length)return -N_ERR_INVALID;
    char *key=kmalloc(length);if(!key)return -N_ERR_NO_MEMORY;
    struct handle *h=NULL;uint64_t flags=irq_save();
    if(!user_ok(ukey,length)){irq_restore(flags);kfree(key);return -N_ERR_BAD_ADDRESS;}
    memcpy(key,ukey,length);
    int slot=handle_table_find(task_process());
    int result=slot>=0?(listen?channel_listen(key,length,&h):channel_connect(key,length,&h)):slot;
    if(!result)result=handle_install_locked(h);
    irq_restore(flags);kfree(key);
    /* The slot was checked and published in one BSP IRQ guard. If that
     * invariant changes, closing the unpublished reference remains safe. */
    if(result<0 && h)handle_close(h);
    return result;
}
static int64_t sys_channel_accept(int index) {
    struct handle *listener=handle_get(index);if(!listener)return -N_ERR_NO_HANDLE;
    struct handle *server=NULL;uint64_t flags=irq_save();int slot=handle_table_find(task_process());
    int result=slot>=0?channel_accept(listener,&server):slot;
    if(!result)result=handle_install_locked(server);
    irq_restore(flags);handle_close(listener);
    if(result<0 && server)handle_close(server);
    return result;
}

static int64_t sys_proclist(struct n_procinfo *out, int max) {
    if (max < 0 || !user_ok_w(out, (size_t)max * sizeof *out)) return -N_ERR_BAD_ADDRESS;
    uint64_t flags = irq_save();
    int n = 0;
    for (struct task *t = task_list; t && n < max; t = t->all_next) {
        struct n_procinfo *p = &out[n++];
        memset(p, 0, sizeof *p);
        p->pid = t->pid;
        p->ppid = t->parent ? t->parent->pid : 0;
        p->state = t->state;
        p->is_user = t->is_user;
        strlcpy(p->name, t->name, sizeof p->name);
        p->cpu_ms = t->cpu_ms;
        p->mem_kb = proc_user_pages(t) * 4;
        p->start_ms = t->start_ms;
        p->wait_phys = t->wait_phys;
    }
    irq_restore(flags);
    return n;
}

static uint64_t programs_idle(void);

static int64_t sys_sysinfo(struct n_sysinfo *si) {
    if (!user_ok_w(si, sizeof *si)) return -N_ERR_BAD_ADDRESS;
    memset(si, 0, sizeof *si);
    si->total_mem = pmm_total_pages() * PAGE_SIZE;
    /* What an app can have: idle programs give their frames to one that asks (frames_free), and
       the frames set aside for the APs' first touches are no one's yet. */
    si->free_mem = (pmm_free_pages() + programs_idle() + vmm_ap_stock_pages()) * PAGE_SIZE;
    si->heap_used = heap_used_bytes();
    si->uptime_ms = uptime_ms();
    uint64_t flags = irq_save();
    int n = 0;
    for (struct task *t = task_list; t; t = t->all_next) n++;
    irq_restore(flags);
    si->ntasks = n;
    si->fb_w = fb.width;
    si->fb_h = fb.height;
    if (wm_running()) wm_screen_size(&si->fb_w, &si->fb_h); /* the desktop may be a remote viewer's size */
    strlcpy(si->cpu, cpu_brand, sizeof si->cpu);
    strlcpy(si->os, OS_NAME " " OS_VERSION, sizeof si->os);
    return 0;
}

static int64_t stop_tree(int pid, int include_self) {
    /* kill every descendant of pid (used for Ctrl+C) */
    /* BSP task-list ownership must span ancestry traversal, not only each
       task_kill call: preemption may otherwise let another waiter reap t/a. */
    uint64_t flags = irq_save();
    int killed = 0;
    bool again = true;
    while (again) {
        again = false;
        for (struct task *t = task_list; t; t = t->all_next) {
            if (t->state == TASK_ZOMBIE || t->killed) continue;
            for (struct task *a = t->parent; a; a = a->parent) {
                if (a->pid == pid) {
                    task_kill(t->pid);
                    killed++;
                    again = true;
                    break;
                }
            }
            if (again) break;
        }
    }
    if (include_self && task_kill(pid) == 0) killed++;
    irq_restore(flags);
    return killed;
}

/* FD-backed native shared pages. A descriptor and every mapped address space
 * hold independent file references: closing/killing either peer cannot free
 * pages which the other still reads. No global names or untrusted pointers. */
/* Eager objects own one frame per page from creation. Demand objects reserve
 * address space (a growable buffer's or shared wasm memory's maximum): a
 * two-level table of leaf frames receives zeroed pages at the first touch by
 * any mapping process, and every later mapper sees the same frame. */
/* Drivers in apps (dev/pciuser.c) add objects over memory that is not handed out page by page:
 * a range is a run of physical pages, a device's BAR or a contiguous DMA run it allocated whole;
 * a view maps part of another object, which it holds open, with its own caching. DMA objects
 * are RAM (or an owned range) whose bus addresses n_dma_addresses reports.
 * BAR and DMA objects hold their PCI claim. A BAR object dies with the claim's last handle
 * (shared_revoke_claim). Where an IOMMU isolates the function, a DMA object's pages are in its
 * domain from creation until release, which returns them once the IOMMU confirmed the device
 * can no longer reach them. After a nonisolated claim ends, remaining DMA RAM stays quarantined:
 * BME clear/Transactions Pending alone do not prove all posted DMA drained. While the claim is
 * live, caller-driven free still requires the driver's own device synchronization.
 * DMA RAM has one memory type for all its mappings and the
 * direct map: write-back until the first uncached or write-combining view while nothing maps
 * it, then that type for good (x86 forbids one page with two types). */
/* Paged memory (n_pager_create) has no pages of its own: the app that made it, its pager, binds
 * each page to a frame of RAM it holds (n_pager_supply), with read or read/write access, and takes
 * frames or write access away again (n_pager_revoke). A touch of a page without the access it
 * needs waits while the pager is asked (n_pager_take). Its table has two leaves per 512 pages: the
 * bound frames with their access bits, and the objects they were supplied from (a root of RAM or a
 * view of one), each bound page holding a reference of its own (a DMA root also a mapping
 * reference: its type stays). Revoking a view a page was supplied from (or one it descends from)
 * unbinds that page everywhere, as it removes the view's own mappings (paged_revoke). */
#define SHARED_LEAF (PAGE_SIZE/sizeof(uint64_t))
enum {SHARED_RAM,SHARED_DEMAND,SHARED_RANGE,SHARED_VIEW,SHARED_COMPOSITE,SHARED_PAGED};
struct pager;
/* Physical views remain flattened. Revocation ancestry is independent of the
   backing, so creating a child never escapes an issuer's cancellation scope.
   References are released iteratively, not through an unbounded C call stack.
   bound: a paged page was supplied from an object within it (marked with every ancestor, never
   cleared), so its revocation looks through the paged memories too. */
struct shared_scope {struct shared_scope *parent;size_t refs;int issuer;bool revoked,bound;};
/* Composite segments point only at backing roots, never at another composite.
   Cancellation references are separate, so flattening cannot discard a view's
   issuer or a nested composite's lifetime. end is the exclusive logical page. */
struct shared_part {struct object *object;size_t first,end;uint8_t cache;};
struct shared_pages {
    size_t count,bytes;uint8_t kind,cache;bool dma,owned,wc_allowed;
    struct shared_scope *scope;
    size_t maps;              /* a DMA root: mapping references, including composite segments */
    struct pci_claim *claim;  /* a BAR or DMA root: the claim it was made through */
    uint64_t base;struct object *parent;size_t first; /* a range's first frame; a view's object and first page */
    struct shared_part *parts;size_t part_count;
    struct shared_scope **dependencies;size_t dependency_count;
    size_t resident; /* a demand object's frames */
    /* A program segment's demand object: its pages hold source's bytes from source_offset on,
       source_bytes of them starting source_skip bytes into the first page, zeros elsewhere, read
       on first touch. NULL while its program is idle (program_map gives a start's again). */
    struct handle *source;uint64_t source_offset,source_bytes;size_t source_skip;
    struct pager *pager; /* paged memory: the pager that supplies it (holds a reference) */
    struct object object;
    uint64_t pages[];
};
/* access: NULL, or the leaves (frames, 0 for none yet) of one byte per page holding the access
   n_memory_protect set there (0: the mapping's own); every PTE installed later keeps to it. */
struct shared_mapping {
    struct shared_mapping *next;struct object *object;uint64_t va,flags;size_t bytes;
    struct task *building_owner;bool cancelled,revoking;struct wait_queue build_wait;
    uint64_t *access;
};
static struct shared_mapping *shared_building; /* BSP-owned, before returning a VA */
static size_t shared_slots(const struct shared_pages *p) {
    return p->kind==SHARED_DEMAND?(p->count+SHARED_LEAF-1)/SHARED_LEAF:p->kind==SHARED_RAM?p->count:0;
}
static struct shared_pages *shared_root(struct shared_pages *p) {
    while(p->kind==SHARED_VIEW)p=p->parent->priv;
    return p;
}
static bool scope_revoked(const struct shared_scope *s) {
    for(;s;s=s->parent)if(s->revoked)return true;
    return false;
}
static bool scope_within(const struct shared_scope *s,const struct shared_scope *ancestor) {
    for(;s;s=s->parent)if(s==ancestor)return true;
    return false;
}
static void scope_drop(struct shared_scope *s) {
    while(s && !--s->refs){struct shared_scope *parent=s->parent;kfree(s);s=parent;}
}
static bool shared_cancelled(struct shared_pages *p) {
    if(scope_revoked(p->scope))return true;
    struct shared_pages *root=shared_root(p);
    for(size_t i=0;i<root->dependency_count;i++)if(scope_revoked(root->dependencies[i]))return true;
    return false;
}
static bool shared_depends(struct shared_pages *p,const struct shared_scope *scope) {
    if(scope_within(p->scope,scope))return true;
    struct shared_pages *root=shared_root(p);
    for(size_t i=0;i<root->dependency_count;i++)if(scope_within(root->dependencies[i],scope))return true;
    return false;
}
static struct shared_part *shared_part_at(struct shared_pages *p,size_t page) {
    size_t lo=0,hi=p->part_count;
    while(lo<hi){size_t mid=lo+(hi-lo)/2;if(page<p->parts[mid].end)hi=mid;else lo=mid+1;}
    ASSERT(lo<p->part_count);return &p->parts[lo];
}
/* Resolve one logical page without recursion. Optional backing/span are used
   when composing or counting DMA mapping references, not once per byte. */
static struct shared_pages *shared_leaf(struct shared_pages *p,size_t *index,uint8_t *cache,
                                        struct object **backing,size_t *span) {
    *cache=p->cache;
    while(p->kind==SHARED_VIEW){
        *index+=p->first;if(backing)*backing=p->parent;p=p->parent->priv;
    }
    if(p->kind==SHARED_COMPOSITE){
        struct shared_part *part=shared_part_at(p,*index);
        size_t start=part==p->parts?0:part[-1].end;
        if(span)*span=part->end-*index;
        *index=part->first+*index-start;*cache=part->cache;
        if(backing)*backing=part->object;p=part->object->priv;
    }else if(span)*span=p->count-*index;
    if(p->dma)*cache=p->cache;
    return p;
}
static bool shared_device_gone(struct shared_pages *p,const struct pci_claim *claim) {
    struct shared_pages *root=shared_root(p);
    if(root->kind!=SHARED_COMPOSITE)
        return root->kind==SHARED_RANGE && !root->owned &&
               (claim?root->claim==claim:pci_user_revoked(root->claim));
    for(size_t i=0;i<root->part_count;i++){
        struct shared_pages *leaf=root->parts[i].object->priv;
        if(leaf->kind==SHARED_RANGE && !leaf->owned &&
           (claim?leaf->claim==claim:pci_user_revoked(leaf->claim)))return true;
    }
    return false;
}
static void shared_build_remove(struct shared_mapping *m) {
    struct shared_mapping **link=&shared_building;
    while(*link && *link!=m)link=&(*link)->next;
    ASSERT(*link==m);*link=m->next;
}
static bool shared_build_overlap(struct task *owner,uint64_t va,size_t bytes) {
    uint64_t irq=irq_save();bool overlap=false;
    for(struct shared_mapping *m=shared_building;m;m=m->next)
        if(m->building_owner==owner && va<m->va+m->bytes && m->va<va+bytes){overlap=true;break;}
    irq_restore(irq);return overlap;
}
/* Mark before removing PTEs: each install tests cancelled in its own bounded
   critical section. The constructor joins a running revoker before it can
   drop this record or exit, keeping the owner address space alive as well.
   Large removals retain vmm_user_free's existing bounded TLB batches. */
static void shared_cancel_builds(const struct shared_scope *scope,const struct pci_claim *claim) {
    for(;;){
        uint64_t irq=irq_save();struct shared_mapping *found=NULL;
        for(struct shared_mapping *m=shared_building;m;m=m->next){
            bool match=scope?shared_depends(m->object->priv,scope):shared_device_gone(m->object->priv,claim);
            if(match && (m->revoking || !m->cancelled)){found=m;break;}
        }
        if(!found){irq_restore(irq);return;}
        if(found->revoking){wq_wait(&found->build_wait);irq_restore(irq);continue;}
        found->cancelled=found->revoking=true;
        irq_restore(irq);
        vmm_user_free(found->building_owner->pml4,found->va,found->bytes);
        irq=irq_save();found->revoking=false;wq_wake_all(&found->build_wait);irq_restore(irq);
    }
}
static void shared_dma_maps(struct shared_pages *p,bool add) {
    for(size_t at=0;at<p->count;){
        size_t page=at,span;uint8_t cache;
        struct shared_pages *leaf=shared_leaf(p,&page,&cache,NULL,&span);
        if(leaf->dma){if(add)leaf->maps++;else {ASSERT(leaf->maps);leaf->maps--;}}
        at+=MIN(span,p->count-at);
    }
}
/* A demand object's slot for page index, its table made when create; NULL when there is none.
   BSP IRQ guard held. */
static uint64_t *demand_slot(struct shared_pages *p,size_t index,bool create) {
    uint64_t *leaf=&p->pages[index/SHARED_LEAF];
    if(!*leaf && (!create || !(*leaf=pmm_alloc_zeroed())))return NULL;
    return (uint64_t *)phys_to_virt(*leaf)+index%SHARED_LEAF;
}
/* Paged memory: the entry of page index (frame | PAGED_BOUND | PAGED_WRITE, 0 when unbound) and,
   in *source, the slot of the object its frame was supplied from; NULL when the leaves are not
   there (made when create). BSP IRQ guard held. */
#define PAGED_BOUND 1ull
#define PAGED_WRITE 2ull
static uint64_t *paged_slot(struct shared_pages *p,size_t index,bool create,struct object ***source) {
    size_t group=2*(index/SHARED_LEAF);
    for(size_t k=0;k<2;k++)
        if(!p->pages[group+k] && (!create || !(p->pages[group+k]=pmm_alloc_zeroed())))return NULL;
    *source=(struct object **)phys_to_virt(p->pages[group+1])+index%SHARED_LEAF;
    return (uint64_t *)phys_to_virt(p->pages[group])+index%SHARED_LEAF;
}
static uint64_t paged_entry(struct shared_pages *p,size_t index) {
    struct object **source;uint64_t *entry=paged_slot(p,index,false,&source);
    return entry?*entry:0;
}
/* The PTE flags a mapping with `flags` gives page index of p: a paged page bound for reading only
   is mapped without write access, as a read-only shared page n_memory_protect cannot change. */
static uint64_t shared_page_flags(struct shared_pages *p,size_t index,uint64_t flags) {
    if(p->kind!=SHARED_PAGED || (paged_entry(p,index)&PAGED_WRITE))return flags;
    return (flags&~PTE_W)|PTE_SHARED_RO;
}
/* BSP-owned, called with interrupts disabled or from the fault handler. */
static uint64_t shared_frame(struct shared_pages *p,size_t index,bool create) {
    uint8_t cache;p=shared_leaf(p,&index,&cache,NULL,NULL);
    if(p->kind==SHARED_RAM)return p->pages[index];
    if(p->kind==SHARED_PAGED){uint64_t entry=paged_entry(p,index);return entry&PAGED_BOUND?entry&PTE_ADDR:0;}
    if(p->kind==SHARED_RANGE)return p->base+index*PAGE_SIZE;
    uint64_t *slot=demand_slot(p,index,create);
    if(slot && !*slot && create && (*slot=pmm_alloc_zeroed()))p->resident++;
    return slot?*slot:0;
}
static void paged_release(struct shared_pages *p);
static void paged_revoke(const struct shared_scope *scope);
static void shared_release(struct object *o) {
    struct shared_pages *p=o->priv;
    if(p->kind==SHARED_PAGED)paged_release(p); /* its bindings, leaves and pager */
    {
        bool keep=p->dma && (!pci_user_dma_unmap(p->claim,p->kind==SHARED_RAM?p->pages:NULL,p->base,p->count) ||
                             !pci_user_dma_reusable(p->claim)); /* the device may still write there */
        if(keep)kprintf("[dma] %lu pages stay out of use: their device may still reach them\n",(unsigned long)p->count);
        else if(p->dma && p->cache!=N_CACHE_WRITE_BACK)
            vmm_direct_type(p->kind==SHARED_RAM?p->pages:NULL,p->base,p->count,0);
        for(size_t i=0;!keep && i<shared_slots(p);i++){
            if(!p->pages[i])continue;
            if(p->kind==SHARED_DEMAND){uint64_t *leaf=phys_to_virt(p->pages[i]);
                for(size_t j=0;j<SHARED_LEAF;j++)if(leaf[j])pmm_free(leaf[j]);}
            pmm_free(p->pages[i]);
        }
        if(p->kind==SHARED_RANGE && p->owned && !keep)for(size_t i=0;i<p->count;i++)pmm_free(p->base+i*PAGE_SIZE);
        object_unref(p->parent);
        for(size_t i=0;i<p->part_count;i++)object_unref(p->parts[i].object);
        for(size_t i=0;i<p->dependency_count;i++)scope_drop(p->dependencies[i]);
        kfree(p->parts);kfree(p->dependencies);
        scope_drop(p->scope);
        pci_user_drop(p->claim);
        handle_close(p->source);
        kfree(p);
    }
}
static const struct object_ops shared_ops={.kind=N_KIND_SHARED,.release=shared_release};
/* An empty object record of `bytes` with `slots` page slots, write-back until told otherwise.
   Nothing references it yet: the first handle or mapping takes a reference, and before that
   shared_release frees it. */
static struct shared_pages *shared_new(size_t bytes,uint8_t kind,size_t slots) {
    if(slots>(SIZE_MAX-sizeof(struct shared_pages))/sizeof(uint64_t))return NULL;
    struct shared_pages *p=kzalloc(sizeof *p+slots*sizeof(uint64_t));
    if(!p)return NULL;
    p->kind=kind;p->cache=N_CACHE_WRITE_BACK;p->bytes=bytes;
    object_init(&p->object,&shared_ops,p);
    return p;
}
/* A handle on a new object; without memory for it the object is freed. */
static int64_t shared_open(struct shared_pages *p,bool writable,struct handle **out) {
    struct handle *h=handle_open(&p->object,writable);
    if(!h){shared_release(&p->object);return -N_ERR_NO_MEMORY;}
    *out=h;return 0;
}
/* The shared pages a handle reaches (NULL: it reaches something else). */
static struct shared_pages *shared_of(struct handle *h) {
    return handle_is(h,&shared_ops)?h->object->priv:NULL;
}
/* A new shared object of `bytes` (page aligned). */
static int64_t shared_object(size_t bytes,bool demand,struct shared_pages **out) {
    if(!bytes || bytes>SIZE_MAX-(PAGE_SIZE-1))return -N_ERR_INVALID;
    bytes=ALIGN_UP(bytes,PAGE_SIZE);size_t count=bytes/PAGE_SIZE;
    size_t slots=demand?(count+SHARED_LEAF-1)/SHARED_LEAF:count;
    if(!demand && count>pmm_free_pages())return -N_ERR_NO_MEMORY;
    struct shared_pages *p=shared_new(bytes,demand?SHARED_DEMAND:SHARED_RAM,slots);
    if(!p)return -N_ERR_NO_MEMORY;
    if(demand)p->count=count;
    else for(;p->count<count;p->count++){
        p->pages[p->count]=pmm_alloc_zeroed();
        if(!p->pages[p->count]){shared_release(&p->object);return -N_ERR_NO_MEMORY;}
    }
    *out=p;return 0;
}
/* The caller's handle number for a new object opened by shared_open (r: its result). */
static int64_t new_handle(int64_t r,struct handle *h) {
    return r<0?r:handle_install(h);
}
static int64_t sys_shared_create(size_t bytes,bool demand) {
    struct shared_pages *p;struct handle *h=NULL;
    int64_t r=shared_object(bytes,demand,&p);
    if(r>=0)r=shared_open(p,true,&h);
    return new_handle(r,h);
}
static uint64_t cache_pte(unsigned cache) {
    if(cache==N_CACHE_WRITE_COMBINE && pat_supported())return PTE_PWT|PTE_PAT4K; /* PAT index 5 */
    if(cache==N_CACHE_WRITE_COMBINE || cache==N_CACHE_UNCACHED)return PTE_PCD|PTE_PWT; /* index 3 */
    return 0;
}

/* Device memory has one memory type at a time. x86 leaves a physical page reached through
 * linear addresses of different types undefined (Intel SDM vol. 3A, PAT: aliasing is not
 * supported), so every mapping of a device range pins the type of its pages and a mapping asking
 * another type of a pinned page is refused while the first lives. The boot screen stays
 * write-combined in the direct map for as long as the system runs. A pin belongs to the claim
 * its range came through and goes with that claim (shared_revoke_claim): no mapping reaches
 * those pages after. BSP-owned, changed with interrupts disabled. */
struct device_pin {struct device_pin *next;const struct pci_claim *claim;uint64_t start,end;size_t refs;uint8_t cache;};
static struct device_pin *device_pins;
static uint8_t device_cache(uint8_t cache) { /* the type a mapping really gets */
    return cache==N_CACHE_WRITE_COMBINE && !pat_supported()?N_CACHE_UNCACHED:cache;
}
static const char *cache_name(uint8_t cache) {
    return cache==N_CACHE_WRITE_COMBINE?"write-combined":cache==N_CACHE_UNCACHED?"uncached":"write-back";
}
static bool device_pin(const struct pci_claim *claim,uint64_t start,uint64_t end,uint8_t cache,int64_t *error) {
    uint64_t screen=ALIGN_DOWN(fb.phys,PAGE_SIZE),screen_end=ALIGN_UP(fb.phys+(uint64_t)fb.pitch*fb.height,PAGE_SIZE);
    uint8_t other=fb.addr && start<screen_end && screen<end && cache!=device_cache(N_CACHE_WRITE_COMBINE)?
                  device_cache(N_CACHE_WRITE_COMBINE):N_CACHE_DEFAULT;
    struct device_pin *same=NULL;
    for(struct device_pin *d=device_pins;d && other==N_CACHE_DEFAULT;d=d->next){
        if(start<d->end && d->start<end && d->cache!=cache)other=d->cache;
        if(d->claim==claim && d->start==start && d->end==end && d->cache==cache)same=d;
    }
    if(other!=N_CACHE_DEFAULT){
        struct task *t=task_process();
        kprintf("[shared] pid %d (%s): device memory in %#lx-%#lx is mapped %s; a %s mapping is refused\n",
                t?t->pid:0,t?t->name:"kernel",(unsigned long)start,(unsigned long)end-1,cache_name(other),cache_name(cache));
        *error=-N_ERR_DENIED;return false;
    }
    if(same){same->refs++;return true;}
    struct device_pin *d=kmalloc(sizeof *d);
    if(!d){*error=-N_ERR_NO_MEMORY;return false;}
    *d=(struct device_pin){.next=device_pins,.claim=claim,.start=start,.end=end,.refs=1,.cache=cache};
    device_pins=d;return true;
}
static void device_unpin(const struct pci_claim *claim,uint64_t start,uint64_t end,uint8_t cache) {
    for(struct device_pin **link=&device_pins;*link;link=&(*link)->next){
        struct device_pin *d=*link;
        if(d->claim!=claim || d->start!=start || d->end!=end || d->cache!=cache)continue;
        if(!--d->refs){*link=d->next;kfree(d);}
        return;
    }
    ASSERT(pci_user_revoked(claim)); /* its pins went with the claim */
}
/* Pins (add) or unpins the type of the first `limit` device segments p maps. Adding stops at a
   segment whose pages have another type, or without memory for the record (*error); the return
   is the number of segments done. */
static size_t device_pins_walk(struct shared_pages *p,bool add,size_t limit,int64_t *error) {
    size_t done=0;
    for(size_t at=0;at<p->count && done<limit;){
        size_t page=at,span;uint8_t cache;
        struct shared_pages *leaf=shared_leaf(p,&page,&cache,NULL,&span);
        span=MIN(span,p->count-at);at+=span;
        if(leaf->kind!=SHARED_RANGE || leaf->owned)continue;
        uint64_t start=leaf->base+page*PAGE_SIZE,end=start+span*PAGE_SIZE;
        cache=device_cache(cache);
        if(add && !device_pin(leaf->claim,start,end,cache,error))break;
        if(!add)device_unpin(leaf->claim,start,end,cache);
        done++;
    }
    return done;
}
static int64_t shared_device_pin(struct shared_pages *p) {
    int64_t error=0;size_t done=device_pins_walk(p,true,SIZE_MAX,&error);
    if(error)device_pins_walk(p,false,done,NULL);
    return error;
}
static void shared_device_unpin(struct shared_pages *p) { device_pins_walk(p,false,SIZE_MAX,NULL); }
static void device_pins_drop(const struct pci_claim *claim) {
    for(struct device_pin **link=&device_pins;*link;){
        struct device_pin *d=*link;
        if(d->claim==claim){*link=d->next;kfree(d);}else link=&d->next;
    }
}

/* The screen drawn into a driver's surface (syscall.h). It counts as a mapping of the surface: its
   pages keep their type while the screen is there, and a DMA object's type stays fixed. */
int64_t shared_scanout_open(struct handle *surface,const struct pci_claim *claim,uint64_t offset,uint64_t bytes,
                            struct shared_scanout *out) {
    struct shared_pages *p=shared_of(surface);
    if(!p)return -N_ERR_WRONG_KIND;
    if(!surface->writable)return -N_ERR_DENIED;
    if(shared_cancelled(p) || shared_device_gone(p,NULL))return -N_ERR_CLOSED;
    struct shared_pages *root=shared_root(p);
    bool bar=root->kind==SHARED_RANGE && !root->owned && !root->dma;
    if(root->claim!=claim || (!bar && !root->dma))return -N_ERR_DENIED; /* another claim's, or no device's */
    if(!bytes || offset>p->bytes || bytes>p->bytes-offset)return -N_ERR_INVALID;
    uint8_t cache=bar?device_cache(N_CACHE_WRITE_COMBINE):root->cache;
    if(bar?!root->wc_allowed:cache==N_CACHE_WRITE_BACK)return -N_ERR_DENIED;
    size_t first=offset/PAGE_SIZE,pages=(offset+bytes-1)/PAGE_SIZE-first+1;
    uint64_t *frames=kmalloc(pages*sizeof *frames);
    if(!frames)return -N_ERR_NO_MEMORY;
    int64_t error=0;
    uint64_t irq=irq_save();
    for(size_t i=0;i<pages;i++)frames[i]=shared_frame(p,first+i,false);
    uint64_t start=frames[0],end=frames[pages-1]+PAGE_SIZE; /* a BAR's pages are one run */
    if(!bar || device_pin(root->claim,start,end,cache,&error)){
        if(root->dma)shared_dma_maps(p,true);
        object_ref(surface->object);
    }
    irq_restore(irq);
    if(error){kfree(frames);return error;}
    *out=(struct shared_scanout){.object=surface->object,.frames=frames,.pages=pages,
                                 .first_byte=offset%PAGE_SIZE,.type=cache_pte(cache)};
    return 0;
}
void shared_scanout_close(struct shared_scanout *s) {
    if(!s->object)return;
    struct shared_pages *p=s->object->priv,*root=shared_root(p);
    uint64_t irq=irq_save();
    if(root->dma)shared_dma_maps(p,false);
    else device_unpin(root->claim,s->frames[0],s->frames[s->pages-1]+PAGE_SIZE,device_cache(N_CACHE_WRITE_COMBINE));
    irq_restore(irq);
    kfree(s->frames);
    object_unref(s->object);
    *s=(struct shared_scanout){0};
}

/* The screen drawn into a display driver's surface, or the boot screen again (dev/pciuser.c). */
static int64_t sys_pci_give_screen(int h,int surface,const struct n_scanout *udesc) {
    struct n_scanout desc={0};
    if(surface!=N_SCANOUT_BOOT && copy_from_user(&desc,udesc,sizeof desc))return -N_ERR_BAD_ADDRESS;
    struct handle *pci=handle_get(h);
    if(!pci)return -N_ERR_NO_HANDLE;
    struct handle *s=surface==N_SCANOUT_BOOT?NULL:handle_get(surface);
    int64_t r=surface!=N_SCANOUT_BOOT && !s?-N_ERR_NO_HANDLE:pci_user_give_screen(pci,s,&desc);
    handle_close(s);handle_close(pci);
    return r;
}
/* A surface a display driver may show in its scanout's place, for the panic screen (dev/pciuser.c). */
static int64_t sys_pci_screen_image(int h,int surface,const struct n_scanout *udesc) {
    struct n_scanout desc;
    if(copy_from_user(&desc,udesc,sizeof desc))return -N_ERR_BAD_ADDRESS;
    struct handle *pci=handle_get(h);
    if(!pci)return -N_ERR_NO_HANDLE;
    struct handle *s=handle_get(surface);
    int64_t r=s?pci_user_screen_image(pci,s,&desc):-N_ERR_NO_HANDLE;
    handle_close(s);handle_close(pci);
    return r;
}
/* What the screen should show, for a driver whose scanout shows windows directly (dev/pciuser.c,
   gui/wm.c). The state is copied out even when the time ran out. */
static int64_t sys_pci_screen_state(int h,uint64_t seen,struct n_screen_state *uout,int64_t timeout_ms) {
    if(!user_ok_w(uout,sizeof *uout))return -N_ERR_BAD_ADDRESS;
    struct handle *pci=handle_get(h);
    if(!pci)return -N_ERR_NO_HANDLE;
    struct n_screen_state *st=kmalloc(sizeof *st);
    int64_t r=st?pci_user_screen_state(pci,seen,st,timeout_ms):-N_ERR_NO_MEMORY;
    handle_close(pci);
    if((r==0||r==-N_ERR_TIMEOUT)&&copy_to_user(uout,st,sizeof *st))r=-N_ERR_BAD_ADDRESS;
    kfree(st);
    return r;
}
/* A display driver's monitor and the modes it can show there (dev/pciuser.c, gui/screenmode.c). */
static int64_t sys_pci_screen_modes(int h,const struct n_screen_modes *um) {
    struct n_screen_modes *m=kmalloc(sizeof *m);
    if(!m)return -N_ERR_NO_MEMORY;
    int64_t r=copy_from_user(m,um,sizeof *m)?-N_ERR_BAD_ADDRESS:0;
    struct handle *pci=r?NULL:handle_get(h);
    if(!r)r=pci?pci_user_screen_modes(pci,m):-N_ERR_NO_HANDLE;
    handle_close(pci);
    kfree(m);
    return r;
}
/* An ACPI object of the claimed function's part of the namespace (dev/pciuser.c). The arguments'
   data and the result go through kernel copies of at most ACPI_DATA_MAX and ACPI_OUT_MAX bytes. */
#define ACPI_ARGS_MAX 7
#define ACPI_DATA_MAX (64u*1024)
#define ACPI_OUT_MAX (1024u*1024)
static int64_t sys_pci_acpi_eval(int h,const struct n_acpi_eval *ueval,void *uout,uint64_t out_size) {
    struct n_acpi_eval e;
    struct n_acpi_value args[ACPI_ARGS_MAX];
    char name[5];
    if(copy_from_user(&e,ueval,sizeof e))return -N_ERR_BAD_ADDRESS;
    if(e.nargs>ACPI_ARGS_MAX || e.data_size>ACPI_DATA_MAX)return -N_ERR_INVALID;
    int n=user_str(name,e.name,sizeof name);
    if(n==-N_ERR_BAD_ADDRESS)return n;
    if(n!=4)return -N_ERR_INVALID;
    if(e.nargs && copy_from_user(args,e.args,e.nargs*sizeof *args))return -N_ERR_BAD_ADDRESS;
    size_t room=(size_t)MIN(out_size,(uint64_t)ACPI_OUT_MAX);
    if(room && !user_ok_w(uout,room))return -N_ERR_BAD_ADDRESS;
    struct handle *pci=handle_get(h);
    if(!pci)return -N_ERR_NO_HANDLE;
    uint8_t *data=e.data_size?kmalloc(e.data_size):NULL,*out=room?kmalloc(room):NULL;
    int64_t r=(e.data_size && !data) || (room && !out)?-N_ERR_NO_MEMORY:0;
    if(!r && e.data_size && copy_from_user(data,e.data,e.data_size))r=-N_ERR_BAD_ADDRESS;
    if(!r)r=pci_user_acpi_eval(pci,e.node,name,args,e.nargs,data,e.data_size,out,room);
    if(r>0 && copy_to_user(uout,out,(size_t)r))r=-N_ERR_BAD_ADDRESS;
    kfree(data);kfree(out);handle_close(pci);
    return r;
}
/* A claimed function's memory BAR: device memory, uncached unless a view says otherwise. */
static int64_t sys_pci_bar(int h,uint32_t bar) {
    struct handle *pci=handle_get(h);
    if(!pci)return -N_ERR_NO_HANDLE;
    uint64_t address=0,size=0;int64_t r=pci_user_bar(pci,bar,&address,&size);
    bool wc_allowed=pci_user_bar_prefetchable(pci,bar);
    if(r>=0 && size>SIZE_MAX)r=-N_ERR_TOO_LARGE;
    struct shared_pages *p=r<0?NULL:shared_new(size,SHARED_RANGE,0);
    if(p)p->claim=pci_user_hold(pci);
    handle_close(pci);
    if(r<0)return r;
    if(!p)return -N_ERR_NO_MEMORY;
    p->base=address;p->count=size/PAGE_SIZE;p->cache=N_CACHE_UNCACHED;p->wc_allowed=wc_allowed;
    struct handle *range=NULL;
    r=shared_open(p,true,&range);
    return new_handle(r,range);
}
/* Zeroed RAM a claimed function may reach by DMA. A contiguous run is allocated whole;
   independent pages use the allocator's hint even with a device/IOMMU address ceiling. */
static int64_t sys_dma_create(int h,size_t bytes,uint32_t flags) {
    struct handle *pci=handle_get(h);
    if(!pci)return -N_ERR_NO_HANDLE;
    struct pci_claim *claim=pci_user_hold(pci);handle_close(pci);
    if(!claim)return -N_ERR_WRONG_KIND;
    int64_t r=0;
    if(flags&~(N_DMA_CONTIGUOUS|N_DMA_32BIT) || !bytes || bytes>SIZE_MAX-(PAGE_SIZE-1))r=-N_ERR_INVALID;
    else if(ALIGN_UP(bytes,PAGE_SIZE)/PAGE_SIZE>pmm_free_pages())r=-N_ERR_NO_MEMORY;
    if(r<0){pci_user_drop(claim);return r;}
    bytes=ALIGN_UP(bytes,PAGE_SIZE);size_t count=bytes/PAGE_SIZE;
    uint64_t limit=flags&N_DMA_32BIT?0x100000000ull:UINT64_MAX;
    uint64_t iommu_limit=pci_user_dma_limit(claim);
    if(iommu_limit<limit)limit=iommu_limit;
    struct shared_pages *p;
    if(flags&N_DMA_CONTIGUOUS){
        uint64_t base=pmm_alloc_contig_below(count,limit);
        if(!base){pci_user_drop(claim);return -N_ERR_NO_MEMORY;}
        if(!(p=shared_new(bytes,SHARED_RANGE,0))){
            for(size_t i=0;i<count;i++)pmm_free(base+i*PAGE_SIZE);
            pci_user_drop(claim);return -N_ERR_NO_MEMORY;
        }
        p->claim=claim;
        memset(phys_to_virt(base),0,bytes);
        p->base=base;p->count=count;p->owned=true;
    }else{
        if(!(p=shared_new(bytes,SHARED_RAM,count))){pci_user_drop(claim);return -N_ERR_NO_MEMORY;}
        p->claim=claim;
        for(;p->count<count;p->count++){
            uint64_t frame=pmm_alloc_below(limit);
            if(!frame){shared_release(&p->object);return -N_ERR_NO_MEMORY;}
            memset(phys_to_virt(frame),0,PAGE_SIZE);
            p->pages[p->count]=frame;
        }
    }
    p->dma=true;
    if(!pci_user_dma_map(claim,p->kind==SHARED_RAM?p->pages:NULL,p->base,p->count)){shared_release(&p->object);return -N_ERR_NO_MEMORY;}
    struct handle *dma=NULL;
    r=shared_open(p,true,&dma);
    return new_handle(r,dma);
}
/* Bus addresses of a DMA object's (or a device range's) pages, also through views. */
static int64_t sys_dma_addresses(int h,size_t first,uint64_t *uout,size_t count) {
    struct handle *object=handle_get(h);
    if(!object)return -N_ERR_NO_HANDLE;
    int64_t r=0;struct shared_pages *p=shared_of(object);
    if(!p)r=-N_ERR_WRONG_KIND;
    else if(shared_cancelled(p) || shared_device_gone(p,NULL))r=-N_ERR_CLOSED;
    else if(first>=p->count)r=-N_ERR_INVALID;
    else for(size_t at=first,n=MIN(count,p->count-first);at-first<n;){
        size_t page=at,span;uint8_t cache;
        struct shared_pages *leaf=shared_leaf(p,&page,&cache,NULL,&span);
        if(!leaf->dma && leaf->kind!=SHARED_RANGE){r=-N_ERR_WRONG_KIND;break;}
        at+=MIN(span,n-(at-first));
    }
    for(size_t done=0,n=r<0?0:MIN(count,p->count-first);done<n;){
        uint64_t piece[64];size_t k=MIN(n-done,ARRAY_SIZE(piece));
        uint64_t irq=irq_save();
        for(size_t i=0;i<k;i++)piece[i]=shared_frame(p,first+done+i,false);
        irq_restore(irq);
        if(copy_to_user(uout+done,piece,k*sizeof *piece)<0){r=-N_ERR_BAD_ADDRESS;break;}
        done+=k;r=(int64_t)done;
    }
    handle_close(object);return r;
}
/* Part of a shared object as a new one with its own caching. Device memory only uncached or
   write-combining, and not after its claim ended; DMA RAM in its one type (the first other
   type asked while nothing maps it becomes that type); other objects only as they are. */
static int64_t sys_shared_view(int h,size_t offset,size_t bytes,uint32_t cache,uint32_t maximum) {
    struct handle *f=handle_get(h);
    if(!f)return -N_ERR_NO_HANDLE;
    int64_t r=0;struct shared_pages *src=shared_of(f);
    if(!src)r=-N_ERR_WRONG_KIND;
    else if(!bytes || (offset|bytes)&(PAGE_SIZE-1) || offset>src->bytes || bytes>src->bytes-offset ||
            cache>N_CACHE_UNCACHED || (maximum && maximum!=N_MEMORY_READ && maximum!=N_MEMORY_READ_WRITE))r=-N_ERR_INVALID;
    if(r<0){handle_close(f);return r;}
    struct shared_pages *root=shared_root(src);
    /* paged memory is its pager's to hand out page by page: no views of it */
    if(root->kind==SHARED_PAGED){handle_close(f);return -N_ERR_WRONG_KIND;}
    if(shared_cancelled(src) || shared_device_gone(src,NULL)){handle_close(f);return -N_ERR_CLOSED;}
    bool writable=f->writable;
    if(maximum==N_MEMORY_READ_WRITE && !writable){handle_close(f);return -N_ERR_DENIED;}
    if(maximum==N_MEMORY_READ)writable=false;
    bool device=root->kind==SHARED_RANGE && !root->owned;
    if(root->kind==SHARED_COMPOSITE){
        /* A default view preserves each segment's type. An explicit single
           type must describe all selected pages; it cannot silently collapse
           heterogeneous BAR/RAM attributes into a different cache alias. */
        if(cache!=N_CACHE_DEFAULT)for(size_t at=offset/PAGE_SIZE,left=bytes/PAGE_SIZE;left;){
            size_t page=at,span;uint8_t type;
            shared_leaf(src,&page,&type,NULL,&span);
            if(cache!=type){handle_close(f);return -N_ERR_DENIED;}
            span=MIN(span,left);at+=span;left-=span;
        }
    }else if(root->dma){
        if(cache==N_CACHE_DEFAULT)cache=root->cache;
        else if(cache!=root->cache){
            if(root->cache!=N_CACHE_WRITE_BACK || root->maps){handle_close(f);return -N_ERR_DENIED;}
            if(!vmm_direct_type(root->kind==SHARED_RAM?root->pages:NULL,root->base,root->count,cache_pte(cache))){
                handle_close(f);return -N_ERR_NO_MEMORY;
            }
            root->cache=(uint8_t)cache;
        }
    }else if(cache==N_CACHE_DEFAULT)cache=src->cache;
    else if(device?(cache==N_CACHE_WRITE_BACK || (cache==N_CACHE_WRITE_COMBINE && !root->wc_allowed)):cache!=src->cache){handle_close(f);return -N_ERR_DENIED;}
    struct shared_scope *scope=kzalloc(sizeof *scope);
    if(!scope){handle_close(f);return -N_ERR_NO_MEMORY;}
    scope->refs=1;scope->issuer=task_process()->pid;scope->parent=src->scope;
    if(scope->parent)scope->parent->refs++;
    struct shared_pages *p=shared_new(bytes,SHARED_VIEW,0);
    if(!p){scope_drop(scope);handle_close(f);return -N_ERR_NO_MEMORY;}
    p->scope=scope;
    p->count=bytes/PAGE_SIZE;p->first=offset/PAGE_SIZE;p->cache=(uint8_t)cache;p->parent=f->object;
    if(src->kind==SHARED_VIEW){
        /* A view of a view points at the root object, so a chain of views (which any app can
           build, closing each parent) never grows the recursion in shared_frame and release. */
        p->parent=src->parent;p->first+=src->first;
    }
    object_ref(p->parent);handle_close(f);
    struct handle *view=NULL;
    r=shared_open(p,writable,&view);
    return new_handle(r,view);
}
static int64_t sys_shared_compose(const struct n_shared_range *uranges,size_t count,uint32_t access) {
    if(!count || (access!=N_MEMORY_READ && access!=N_MEMORY_READ_WRITE))return -N_ERR_INVALID;
    struct compose_input {struct handle *handle;size_t first,count;};
    if(count>SIZE_MAX/sizeof(struct compose_input) || count>SIZE_MAX/sizeof(*uranges))return -N_ERR_TOO_LARGE;
    if(count>(UINTPTR_MAX-(uintptr_t)uranges)/sizeof(*uranges))return -N_ERR_BAD_ADDRESS;
    struct compose_input *inputs=kzalloc(count*sizeof *inputs);
    if(!inputs)return -N_ERR_NO_MEMORY;
    int64_t r=0;size_t bytes=0,parts=0,dependencies=0;
    struct shared_pages *p=NULL;
    for(size_t i=0;i<count;i++){
        struct n_shared_range range;
        if((r=copy_from_user(&range,&uranges[i],sizeof range))<0)goto out;
        if(range.reserved || !range.bytes || (range.offset|range.bytes)&(PAGE_SIZE-1)){
            r=-N_ERR_INVALID;goto out;
        }
        struct handle *f=inputs[i].handle=handle_get(range.shared);
        if(!f){r=-N_ERR_NO_HANDLE;goto out;}
        struct shared_pages *src=shared_of(f);
        if(!src){r=-N_ERR_WRONG_KIND;goto out;}
        if(range.offset>src->bytes || range.bytes>src->bytes-range.offset){r=-N_ERR_INVALID;goto out;}
        if(access==N_MEMORY_READ_WRITE && !f->writable){r=-N_ERR_DENIED;goto out;}
        struct shared_pages *root=shared_root(src);
        if(root->kind==SHARED_PAGED){r=-N_ERR_WRONG_KIND;goto out;}
        if(shared_cancelled(src) || shared_device_gone(src,NULL)){r=-N_ERR_CLOSED;goto out;}
        if(range.bytes>SIZE_MAX-bytes || root->dependency_count>SIZE_MAX-dependencies ||
           (src->scope && root->dependency_count==SIZE_MAX-dependencies)){r=-N_ERR_TOO_LARGE;goto out;}
        bytes+=range.bytes;dependencies+=root->dependency_count+(src->scope!=NULL);
        inputs[i].first=range.offset/PAGE_SIZE;inputs[i].count=range.bytes/PAGE_SIZE;
        for(size_t at=inputs[i].first,left=inputs[i].count;left;){
            size_t page=at,span;uint8_t cache;
            shared_leaf(src,&page,&cache,NULL,&span);
            if(parts==SIZE_MAX){r=-N_ERR_TOO_LARGE;goto out;}parts++;
            span=MIN(span,left);at+=span;left-=span;
        }
    }
    if(parts>SIZE_MAX/sizeof(struct shared_part) || dependencies>SIZE_MAX/sizeof(struct shared_scope *)){
        r=-N_ERR_TOO_LARGE;goto out;
    }
    p=shared_new(bytes,SHARED_COMPOSITE,0);
    if(!p){r=-N_ERR_NO_MEMORY;goto out;}
    p->count=bytes/PAGE_SIZE;
    p->parts=kmalloc(parts*sizeof *p->parts);
    if(dependencies)p->dependencies=kmalloc(dependencies*sizeof *p->dependencies);
    p->scope=kzalloc(sizeof *p->scope);
    if(p->scope){p->scope->refs=1;p->scope->issuer=task_process()->pid;}
    if(!p->parts || (dependencies && !p->dependencies) || !p->scope){r=-N_ERR_NO_MEMORY;goto out;}
    size_t end=0;
    for(size_t i=0;i<count;i++){
        struct shared_pages *src=shared_of(inputs[i].handle),*root=shared_root(src);
        if(src->scope){src->scope->refs++;p->dependencies[p->dependency_count++]=src->scope;}
        for(size_t j=0;j<root->dependency_count;j++){
            root->dependencies[j]->refs++;p->dependencies[p->dependency_count++]=root->dependencies[j];
        }
        for(size_t at=inputs[i].first,left=inputs[i].count;left;){
            size_t page=at,span;uint8_t cache;struct object *backing=inputs[i].handle->object;
            shared_leaf(src,&page,&cache,&backing,&span);
            span=MIN(span,left);end+=span;object_ref(backing);
            p->parts[p->part_count++]=(struct shared_part){.object=backing,.first=page,.end=end,.cache=cache};
            at+=span;left-=span;
        }
    }
    if(shared_cancelled(p) || shared_device_gone(p,NULL)){r=-N_ERR_CLOSED;goto out;}
    {
        struct handle *composite=NULL;
        r=shared_open(p,access==N_MEMORY_READ_WRITE,&composite);
        p=NULL; /* shared_open consumes the object even on failure */
        r=new_handle(r,composite);
    }
out:
    if(p)shared_release(&p->object);
    for(size_t i=0;i<count;i++)handle_close(inputs[i].handle);
    kfree(inputs);return r;
}
static int64_t sys_shared_revoke(int h) {
    struct handle *f=handle_get(h);
    if(!f)return -N_ERR_NO_HANDLE;
    struct shared_pages *p=shared_of(f);
    if(!p){handle_close(f);return -N_ERR_WRONG_KIND;}
    struct shared_scope *scope=p->scope;
    if(!scope || scope->issuer!=task_process()->pid){handle_close(f);return -N_ERR_DENIED;}
    /* Revoked is published before any PTE removal, including SMP shootdown.
       A concurrent fault cannot restore a page while cancellation runs. */
    scope->revoked=true;
    shared_cancel_builds(scope,NULL);
    for(struct task *t=task_list;t;t=t->all_next){
        if(t->is_thread || !t->pml4 || t->pml4==kernel_pml4)continue;
        for(struct shared_mapping *m=t->shared_mappings;m;m=m->next)
            if(shared_depends(m->object->priv,scope))
                vmm_user_free(t->pml4,m->va,m->bytes);
    }
    if(scope->bound)paged_revoke(scope); /* pages a pager supplied from within it */
    handle_close(f);return 0;
}
/* ---- pagers (docs/nocturne-native-abi.md §7) ----
 * A pager is the app that made paged memory: through its pager handle, which only it can use
 * (no other app may hold or receive one), it answers the faults of every app mapping the memory.
 * A thread touching a page that is unbound, or bound for reading when it writes, files a fault
 * (one per page and access, which later touches join) and sleeps until the pager supplies the
 * page, takes the fault as failed, or ends; it is woken at once when it is stopped. The pager
 * takes faults in batches; a supply covers whole runs of pages and may bind pages nobody asked
 * for yet. Bound pages need no pager at all: a later touch by any mapper maps the frame itself.
 * Revocation unmaps a run everywhere (or takes its write access away) and shoots it down before
 * the frames go; so does n_shared_revoke of a view the pages were supplied from, which unbinds
 * them (the pager is asked again on the next touch). When the pager ends, bound pages stay and
 * unbound ones are access violations; when the memory is gone (no handle, no mapping), the pager
 * handle reports N_EVENT_CLOSED. */
enum {PAGER_PENDING,PAGER_SUPPLIED,PAGER_FAILED};
struct pager_fault {
    struct pager_fault *next;
    uint64_t id;            /* a number of its own, to find it again after a copy to the pager */
    size_t page;
    uint32_t access, app;   /* N_MEMORY_READ or _READ_WRITE; the app that touched it first */
    size_t waiters;
    uint8_t state;
    bool queued;            /* not taken by the pager yet */
};
struct pager {
    struct object object;
    struct shared_pages *memory; /* NULL once it is released */
    int creator;
    bool ended;                  /* the pager handle closed */
    struct pager_fault *faults;  /* in the order they came */
    size_t queued;
    uint64_t next_id;
    struct wait_queue waiters, taker;
    struct pager *next, *prev;   /* in pagers while its memory lives */
};
static struct pager *pagers; /* BSP IRQ guard */
#define PAGER_BATCH 128
static void pager_unlink(struct pager *g,struct pager_fault *f) {
    struct pager_fault **link=&g->faults;
    while(*link!=f)link=&(*link)->next;
    *link=f->next;
    if(f->queued)g->queued--;
    kfree(f);
}
/* Faults of [first, first+count) that the memory now answers (supplied: their access is bound;
   failed: all of them) leave the queue and wake their threads. BSP IRQ guard held. */
static void pager_settle(struct pager *g,size_t first,size_t count,uint8_t state) {
    bool any=false;
    for(struct pager_fault *f=g->faults,*next;f;f=next){
        next=f->next;
        if(f->state!=PAGER_PENDING || f->page<first || f->page-first>=count)continue;
        if(state==PAGER_SUPPLIED){
            uint64_t entry=g->memory?paged_entry(g->memory,f->page):0;
            if(!(entry&PAGED_BOUND) || (f->access==N_MEMORY_READ_WRITE && !(entry&PAGED_WRITE)))continue;
        }
        f->state=state;any=true;
        if(f->queued){f->queued=false;g->queued--;}
        if(!f->waiters)pager_unlink(g,f);
    }
    if(any){wq_wake_all(&g->waiters);object_notify(&g->object);}
}
/* The calling thread waits for the pager to answer page `page` of its memory with the access it
   needs: true once it supplied something there (the caller looks again), false when the fault
   failed, the pager ended or the thread is being stopped. BSP IRQ guard held; it is let go while
   the thread sleeps. */
static bool pager_wait(struct pager *g,size_t page,bool write) {
    uint32_t access=write?N_MEMORY_READ_WRITE:N_MEMORY_READ;
    if(g->ended || current_task->killed)return false;
    struct pager_fault *f=g->faults,**tail=&g->faults;
    for(;f;tail=&f->next,f=f->next) /* one per page and access: a binding for reading answers a read while a write waits */
        if(f->page==page && f->state==PAGER_PENDING && f->access==access)break;
    if(!f){
        if(!(f=kzalloc(sizeof *f)))return false;
        *f=(struct pager_fault){.id=++g->next_id,.page=page,.access=access,.app=(uint32_t)task_process()->pid,
                               .state=PAGER_PENDING,.queued=true};
        *tail=f;g->queued++;
        wq_wake_all(&g->taker);object_notify(&g->object);
    }
    f->waiters++;object_ref(&g->object);
    while(f->state==PAGER_PENDING && !g->ended && !current_task->killed)wq_wait(&g->waiters);
    bool supplied=f->state==PAGER_SUPPLIED;
    if(!--f->waiters && (f->state!=PAGER_PENDING || g->ended || current_task->killed))pager_unlink(g,f);
    object_unref(&g->object);
    return supplied;
}
/* Every mapping of paged memory p, also one being made: its pages [first, first+count) lose their
   frames (write_only: their write access), shot down before this returns. */
static void paged_unmap(struct shared_pages *p,size_t first,size_t count,bool write_only) {
    uint64_t offset=(uint64_t)first*PAGE_SIZE,bytes=(uint64_t)count*PAGE_SIZE;
    for(struct task *t=task_list;t;t=t->all_next){
        if(t->is_thread || !t->pml4 || t->pml4==kernel_pml4)continue;
        for(struct shared_mapping *m=t->shared_mappings;m;m=m->next){
            if(m->object!=&p->object)continue;
            if(write_only)vmm_user_write_protect(t->pml4,m->va+offset,bytes);
            else vmm_user_free(t->pml4,m->va+offset,bytes);
        }
    }
    for(struct shared_mapping *m=shared_building;m;m=m->next){
        if(m->object!=&p->object)continue;
        if(write_only)vmm_user_write_protect(m->building_owner->pml4,m->va+offset,bytes);
        else vmm_user_free(m->building_owner->pml4,m->va+offset,bytes);
    }
}
/* Drops the references bound pages held on their sources, once nothing reaches the frames. */
static void paged_drop(struct object **sources,size_t count) {
    uint64_t irq=irq_save();
    for(size_t i=0;i<count;i++){struct shared_pages *s=shared_root(sources[i]->priv);if(s->dma){ASSERT(s->maps);s->maps--;}}
    irq_restore(irq);
    for(size_t i=0;i<count;i++)object_unref(sources[i]);
}
/* Unbinds [first, first+count) of p (with scope: only the pages supplied from an object within it):
   unmapped everywhere first, then the frames are let go. */
static void paged_unbind(struct shared_pages *p,size_t first,size_t count,const struct shared_scope *scope) {
    while(count){
        size_t n=MIN(count,(size_t)PAGER_BATCH),dropped=0;
        struct object *sources[PAGER_BATCH];
        uint64_t irq=irq_save();
        for(size_t i=0;i<n;i++){
            struct object **source;uint64_t *entry=paged_slot(p,first+i,false,&source);
            if(!entry){i+=SHARED_LEAF-1-(first+i)%SHARED_LEAF;continue;} /* no leaves: nothing bound there */
            if(!*entry || (scope && !shared_depends((*source)->priv,scope)))continue;
            sources[dropped++]=*source;*entry=0;*source=NULL;
        }
        irq_restore(irq);
        if(dropped){paged_unmap(p,first,n,false);paged_drop(sources,dropped);}
        first+=n;count-=n;
    }
}
/* The first pager from g on whose memory still has a reference, its memory held: one whose last
   reference went is on its way out of the list (paged_release runs after the count reached 0,
   preemptible), and must not be taken up again. BSP IRQ guard held. */
static struct pager *pager_hold_from(struct pager *g) {
    while(g && !g->memory->object.refs)g=g->next;
    if(g)object_ref(&g->memory->object);
    return g;
}
/* n_shared_revoke of a scope pages were supplied from within: they are unbound in every paged
   memory, unmapped everywhere before their frames go. Nothing is bound from the scope once it is
   revoked (pager_supply_run), so none comes back meanwhile. Each memory is held while its pages
   go, the next one taken before it is let go: a memory that goes meanwhile has left the list. */
static void paged_revoke(const struct shared_scope *scope) {
    uint64_t irq=irq_save();
    struct pager *g=pager_hold_from(pagers);
    while(g){
        struct shared_pages *p=g->memory;
        irq_restore(irq);
        paged_unbind(p,0,p->count,scope);
        irq=irq_save();
        g=pager_hold_from(g->next);
        irq_restore(irq);
        object_unref(&p->object);
        irq=irq_save();
    }
    irq_restore(irq);
}
/* The memory's last reference went (shared_release): no mapping is left to unmap. */
static void paged_release(struct shared_pages *p) {
    struct pager *g=p->pager;
    uint64_t irq=irq_save();
    if(g->prev)g->prev->next=g->next;
    else if(pagers==g)pagers=g->next;
    if(g->next)g->next->prev=g->prev;
    g->next=g->prev=NULL;
    irq_restore(irq);
    paged_unbind(p,0,p->count,NULL);
    for(size_t i=0;i<2*((p->count+SHARED_LEAF-1)/SHARED_LEAF);i++)if(p->pages[i])pmm_free(p->pages[i]);
    irq=irq_save();
    g->memory=NULL;
    for(struct pager_fault *f=g->faults;f;f=f->next)f->state=PAGER_FAILED;
    while(g->faults && !g->faults->waiters)pager_unlink(g,g->faults);
    wq_wake_all(&g->waiters);wq_wake_all(&g->taker);object_notify(&g->object);
    irq_restore(irq);
    object_unref(&g->object);
}
static uint32_t pager_ready(struct handle *h) {
    struct pager *g=h->object->priv;
    return (g->queued?N_EVENT_RECEIVABLE:0)|(g->memory?0:N_EVENT_CLOSED);
}
/* The pager's handle closed: what waits fails, and what comes later fails at once. */
static void pager_close(struct handle *h) {
    struct pager *g=h->object->priv;
    uint64_t irq=irq_save();
    g->ended=true;
    for(struct pager_fault *f=g->faults,*next;f;f=next){
        next=f->next;f->state=PAGER_FAILED;
        if(!f->waiters)pager_unlink(g,f);
    }
    wq_wake_all(&g->waiters);
    irq_restore(irq);
}
static void pager_release(struct object *o) {
    struct pager *g=o->priv;
    while(g->faults)pager_unlink(g,g->faults);
    kfree(g);
}
static const struct object_ops pager_ops={.kind=N_KIND_PAGER,.ready=pager_ready,.close=pager_close,
                                          .release=pager_release};
/* The pager behind handle h, for its creator only (*error otherwise); *out holds the handle. */
static struct pager *pager_get(int h,struct handle **out,int64_t *error) {
    struct handle *f=handle_get(h);
    if(!f){*error=-N_ERR_NO_HANDLE;return NULL;}
    if(!handle_is(f,&pager_ops)){handle_close(f);*error=-N_ERR_WRONG_KIND;return NULL;}
    struct pager *g=f->object->priv;
    if(g->creator!=task_process()->pid){handle_close(f);*error=-N_ERR_DENIED;return NULL;}
    *out=f;return g;
}
static int64_t sys_pager_create(size_t bytes,int32_t *umemory) {
    if(!bytes || bytes>SIZE_MAX-(PAGE_SIZE-1))return -N_ERR_INVALID;
    bytes=ALIGN_UP(bytes,PAGE_SIZE);
    size_t count=bytes/PAGE_SIZE,groups=(count+SHARED_LEAF-1)/SHARED_LEAF;
    struct pager *g=kzalloc(sizeof *g);
    if(!g)return -N_ERR_NO_MEMORY;
    object_init(&g->object,&pager_ops,g);
    g->creator=task_process()->pid;
    struct shared_pages *p=shared_new(bytes,SHARED_PAGED,2*groups);
    if(!p){kfree(g);return -N_ERR_NO_MEMORY;}
    p->count=count;p->pager=g;g->memory=p;object_ref(&g->object); /* the memory's reference */
    uint64_t irq=irq_save();
    if((g->next=pagers))pagers->prev=g;
    pagers=g;
    irq_restore(irq);
    struct handle *memory=NULL,*pager;
    int64_t r=shared_open(p,true,&memory); /* on failure the memory goes, and with it the pager */
    if(r<0)return r;
    if(!(pager=handle_open(&g->object,true))){handle_close(memory);return -N_ERR_NO_MEMORY;}
    int number=handle_install(memory);
    if(number<0){handle_close(pager);return number;}
    if(copy_to_user(umemory,&number,sizeof number)<0){handle_remove(number);handle_close(pager);return -N_ERR_BAD_ADDRESS;}
    int result=handle_install(pager);
    if(result<0)handle_remove(number);
    return result;
}
static int64_t sys_pager_take(int h,struct n_pager_fault *uout,size_t max,int64_t timeout_ms) {
    struct handle *f;int64_t r=0;
    struct pager *g=pager_get(h,&f,&r);
    if(!g)return r;
    if(!max){handle_close(f);return -N_ERR_INVALID;}
    struct n_pager_fault out[32];uint64_t ids[32];size_t k=0;
    uint64_t irq=irq_save();
    for(;;){
        for(struct pager_fault *q=g->faults;q && k<MIN(max,ARRAY_SIZE(out));q=q->next){
            if(!q->queued)continue;
            out[k]=(struct n_pager_fault){.offset=(uint64_t)q->page*PAGE_SIZE,.access=q->access,.app=q->app};
            ids[k++]=q->id;q->queued=false;g->queued--;
        }
        if(k){r=(int64_t)k;break;}
        if(!g->memory){r=-N_ERR_CLOSED;break;}
        if(current_task->killed){r=-N_ERR_STOPPED;break;}
        if(!timeout_ms){r=-N_ERR_TIMEOUT;break;}
        if(timeout_ms<0)wq_wait(&g->taker);
        else if(!wq_wait_timeout(&g->taker,(uint64_t)timeout_ms) && !g->queued){r=-N_ERR_TIMEOUT;break;}
    }
    irq_restore(irq);
    if(k && copy_to_user(uout,out,k*sizeof *out)<0){
        /* the pager did not get them: they wait in the queue for its next take */
        irq=irq_save();
        for(struct pager_fault *q=g->faults;q;q=q->next)
            for(size_t i=0;i<k;i++)if(q->id==ids[i] && q->state==PAGER_PENDING && !q->queued){q->queued=true;g->queued++;}
        irq_restore(irq);
        r=-N_ERR_BAD_ADDRESS;
    }
    handle_close(f);return r;
}
/* One run of n_pager_supply: [offset, offset+bytes) of the memory bound to the pages of source
   from source_offset on. Pages of RAM only (write-back DMA RAM included), not paged or program
   memory, and from a root or a view, not a composite (its parts are supplied themselves);
   read/write access needs a writable source. Each page holds the source it came through, so
   revoking that source (or a view it descends from) unbinds the page (paged_revoke): a source
   revoked meanwhile binds nothing more. */
static int64_t pager_supply_run(struct pager *g,const struct n_pager_supply *run) {
    struct shared_pages *p=g->memory;
    if(!p)return -N_ERR_CLOSED;
    if(!run->bytes || (run->offset|run->bytes|run->source_offset)&(PAGE_SIZE-1) || run->offset>p->bytes ||
       run->bytes>p->bytes-run->offset || (run->access!=N_MEMORY_READ && run->access!=N_MEMORY_READ_WRITE))
        return -N_ERR_INVALID;
    struct handle *sh=handle_get(run->source);
    if(!sh)return -N_ERR_NO_HANDLE;
    struct shared_pages *src=shared_of(sh);
    int64_t r=0;
    if(!src)r=-N_ERR_WRONG_KIND;
    else if(run->source_offset>src->bytes || run->bytes>src->bytes-run->source_offset)r=-N_ERR_INVALID;
    else if(run->access==N_MEMORY_READ_WRITE && !sh->writable)r=-N_ERR_DENIED;
    else if(shared_root(src)->kind==SHARED_COMPOSITE)r=-N_ERR_DENIED;
    else if(shared_cancelled(src) || shared_device_gone(src,NULL))r=-N_ERR_CLOSED;
    size_t first=run->offset/PAGE_SIZE,count=run->bytes/PAGE_SIZE,from=run->source_offset/PAGE_SIZE;
    while(r>=0 && count){
        size_t n=MIN(count,(size_t)PAGER_BATCH),dropped=0;bool unmap=false,protect=false;
        struct object *sources[PAGER_BATCH];
        uint64_t irq=irq_save();
        /* checked again with each batch: a revocation between two finds what was bound before */
        if(g->memory!=p || shared_cancelled(src)){r=-N_ERR_CLOSED;n=0;}
        else for(struct shared_scope *s=src->scope;s && !s->bound;s=s->parent)s->bound=true;
        for(size_t i=0;i<n;i++){
            size_t index=from+i;uint8_t cache;
            struct shared_pages *leaf=shared_leaf(src,&index,&cache,NULL,NULL);
            bool ram=leaf->kind==SHARED_RAM || (leaf->kind==SHARED_RANGE && leaf->owned) ||
                     (leaf->kind==SHARED_DEMAND && !leaf->source);
            if(!ram || cache!=N_CACHE_WRITE_BACK){r=-N_ERR_DENIED;n=i;break;}
            uint64_t frame=shared_frame(leaf,index,true);
            struct object **source;uint64_t *entry=paged_slot(p,first+i,true,&source);
            if(!frame || !entry){r=-N_ERR_NO_MEMORY;n=i;break;}
            uint64_t now=frame|PAGED_BOUND|(run->access==N_MEMORY_READ_WRITE?PAGED_WRITE:0);
            bool same=(*entry&PTE_ADDR)==frame && (*entry&PAGED_BOUND);
            if(same){
                protect|=(*entry&PAGED_WRITE) && !(now&PAGED_WRITE);
                *entry=now;
                if(*source==sh->object)continue;
            }
            /* a bound page lets its old source go (bound to another frame, it is unmapped first):
               it answers to the source it came through last */
            if(*entry){sources[dropped++]=*source;unmap|=!same;}
            *entry=now;*source=sh->object;object_ref(sh->object);
            if(leaf->dma)leaf->maps++;
        }
        irq_restore(irq);
        /* a page bound elsewhere before is unmapped everywhere before its old frame goes */
        if(unmap)paged_unmap(p,first,n,false);
        else if(protect)paged_unmap(p,first,n,true);
        if(dropped)paged_drop(sources,dropped);
        irq=irq_save();pager_settle(g,first,n,PAGER_SUPPLIED);irq_restore(irq);
        first+=n;from+=n;count-=n;
    }
    handle_close(sh);return r;
}
static int64_t sys_pager_supply(int h,const struct n_pager_supply *uruns,size_t count) {
    struct handle *f;int64_t r=0;
    struct pager *g=pager_get(h,&f,&r);
    if(!g)return r;
    if(!count)r=-N_ERR_INVALID;
    for(size_t i=0;r>=0 && i<count;i++){
        struct n_pager_supply run;
        if(copy_from_user(&run,&uruns[i],sizeof run)<0){r=-N_ERR_BAD_ADDRESS;break;}
        r=pager_supply_run(g,&run);
        if(need_resched)schedule();
    }
    handle_close(f);return r<0?r:0;
}
/* access 0: the pages lose their frames; N_MEMORY_READ: their write access. */
static int64_t sys_pager_revoke(int h,uint64_t offset,uint64_t bytes,uint32_t access) {
    struct handle *f;int64_t r=0;
    struct pager *g=pager_get(h,&f,&r);
    if(!g)return r;
    struct shared_pages *p=g->memory;
    if(!p)r=-N_ERR_CLOSED;
    else if(!bytes || (offset|bytes)&(PAGE_SIZE-1) || offset>p->bytes || bytes>p->bytes-offset ||
            (access && access!=N_MEMORY_READ))r=-N_ERR_INVALID;
    else if(!access)paged_unbind(p,offset/PAGE_SIZE,bytes/PAGE_SIZE,NULL);
    else for(size_t first=offset/PAGE_SIZE,count=bytes/PAGE_SIZE;count;){
        size_t n=MIN(count,(size_t)PAGER_BATCH);bool any=false;
        uint64_t irq=irq_save();
        for(size_t i=0;i<n;i++){
            struct object **source;uint64_t *entry=paged_slot(p,first+i,false,&source);
            if(entry && (*entry&PAGED_WRITE)){*entry&=~PAGED_WRITE;any=true;}
        }
        irq_restore(irq);
        if(any)paged_unmap(p,first,n,true);
        first+=n;count-=n;
    }
    handle_close(f);return r;
}
/* The faults of [offset, offset+bytes) fail: their threads see an access violation. */
static int64_t sys_pager_fail(int h,uint64_t offset,uint64_t bytes) {
    struct handle *f;int64_t r=0;
    struct pager *g=pager_get(h,&f,&r);
    if(!g)return r;
    if(!bytes || (offset|bytes)&(PAGE_SIZE-1) || offset+bytes<offset)r=-N_ERR_INVALID;
    else {uint64_t irq=irq_save();pager_settle(g,offset/PAGE_SIZE,bytes/PAGE_SIZE,PAGER_FAILED);irq_restore(irq);}
    handle_close(f);return r;
}

/* Read-only ELF segments: every task running a program maps one set of frames
 * for them, so concurrent instances (a worker pool) hold one copy of their code
 * and constants, and a start of a program that is still or was recently running
 * (a compiler's driver, then its compiler and its linker) neither reads nor
 * copies them again. A segment's frames come on first touch: the pages around
 * the touched one are read from the document (PROGRAM_AROUND), so a large
 * program costs what it runs of itself, not its size. A program is its
 * document's content, known by its identity (store_document_identity): a start
 * of a changed document reads it anew, and the tasks running the older content
 * keep theirs. Pages are read through a READ opening, which keeps reading the
 * content it opened even when the document is changed, moved or removed. A
 * program joins the cache when a start has mapped all of it. Programs that no
 * task runs stay, the most recently started first, while they hold at most a
 * sixteenth of memory and an eighth stays free; an app that asks for more memory
 * than is free gets theirs. An idle program closes its openings, so it holds no
 * removed or replaced content on C:; the next start gives its own. */
struct program_segment {struct program_segment *next;struct shared_pages *pages;uint64_t offset,va,filesz,memsz;bool exec;};
enum {PROGRAM_NEW,PROGRAM_CACHED,PROGRAM_RETIRED};
struct program {
    struct program *next;
    uint64_t document;void *identity;size_t identity_bytes;
    struct program_segment *segments;
    size_t starts; /* starts holding it */
    int state;
};
static struct program *programs; /* the cache, the most recently started first */
static void program_free(struct program *p) {
    while(p->segments){struct program_segment *s=p->segments;p->segments=s->next;object_unref(&s->pages->object);kfree(s);}
    kfree(p->identity);kfree(p);
}
/* Whether no task maps it, reads a page of it, or starts it. */
static bool program_idle(const struct program *p) {
    if(p->starts)return false;
    for(const struct program_segment *s=p->segments;s;s=s->next)
        if(s->pages->object.refs!=1)return false;
    return true;
}
/* Its frames, when it is idle; else 0. */
static uint64_t program_idle_pages(const struct program *p) {
    uint64_t pages=0;
    if(!program_idle(p))return 0;
    for(const struct program_segment *s=p->segments;s;s=s->next)pages+=s->pages->resident;
    return pages;
}
/* The frames of all idle programs: free for the asking. */
static uint64_t programs_idle(void) {
    uint64_t pages=0,irq=irq_save();
    for(const struct program *p=programs;p;p=p->next)pages+=program_idle_pages(p);
    irq_restore(irq);
    return pages;
}
/* Programs no task runs leave the cache, the least recently started first, while
 * they hold more than a sixteenth of memory, or fewer than an eighth of it and
 * `need` more pages are free. */
static void programs_trim(uint64_t need) {
    uint64_t budget=pmm_total_pages()/16,floor=pmm_total_pages()/8+need;
    for(;;){
        uint64_t idle=0,irq=irq_save();struct program **last=NULL,*gone=NULL;
        for(struct program **link=&programs;*link;link=&(*link)->next){
            uint64_t pages=program_idle_pages(*link);
            if(pages){idle+=pages;last=link;}
        }
        if(last && (idle>budget || pmm_free_pages()<floor)){gone=*last;*last=gone->next;gone->state=PROGRAM_RETIRED;}
        irq_restore(irq);
        if(!gone)break;
        program_free(gone);
    }
    /* The idle ones that stay let go of their openings (nothing can read a page of them). */
    for(;;){
        uint64_t irq=irq_save();struct handle *source=NULL;
        for(struct program *p=programs;p && !source;p=p->next){
            if(!program_idle(p))continue;
            for(struct program_segment *s=p->segments;s && !source;s=s->next)
                if((source=s->pages->source))s->pages->source=NULL;
        }
        irq_restore(irq);
        if(!source)return;
        handle_close(source);
    }
}
/* Whether pages more frames are free, once idle programs gave theirs back if not. */
static bool frames_free(uint64_t pages) {
    if(pages<=pmm_free_pages())return true;
    programs_trim(pages);
    return pages<=pmm_free_pages();
}
struct program *program_get(struct handle *doc) {
    struct n_entry entry;void *identity;size_t bytes;
    if(store_document_info(doc,&entry)<0 || store_document_identity(doc,&identity,&bytes)<0)return NULL;
    programs_trim(0);
    uint64_t irq=irq_save();struct program *p=NULL;
    for(struct program **link=&programs;*link;link=&(*link)->next)
        if((*link)->document==entry.id && (*link)->identity_bytes==bytes && !memcmp((*link)->identity,identity,bytes)){
            p=*link;*link=p->next;p->next=programs;programs=p; /* the most recently started */
            p->starts++;break;
        }
    irq_restore(irq);
    if(p){kfree(identity);return p;}
    if(!(p=kzalloc(sizeof *p))){kfree(identity);return NULL;}
    *p=(struct program){.document=entry.id,.identity=identity,.identity_bytes=bytes,.starts=1,.state=PROGRAM_NEW};
    return p;
}
void program_put(struct program *p,bool complete) {
    if(!p)return;
    struct program *replaced=NULL;uint64_t irq=irq_save();
    if(p->state==PROGRAM_NEW && complete && p->segments){
        /* An older content of the document stays with the tasks running it. */
        for(struct program **link=&programs;*link;link=&(*link)->next)
            if((*link)->document==p->document){replaced=*link;*link=replaced->next;replaced->state=PROGRAM_RETIRED;break;}
        p->next=programs;programs=p;p->state=PROGRAM_CACHED;
    }
    bool gone=!--p->starts && p->state!=PROGRAM_CACHED;
    if(replaced && replaced->starts)replaced=NULL; /* its last start lets go of it */
    irq_restore(irq);
    if(replaced)program_free(replaced);
    if(gone)program_free(p);
}
int program_map(struct task *t,struct program *prog,struct handle *doc,uint64_t offset,uint64_t va,uint64_t filesz,
                uint64_t memsz,bool exec) {
    uint64_t start=ALIGN_DOWN(va,PAGE_SIZE),end=ALIGN_UP(va+memsz,PAGE_SIZE);
    struct shared_pages *p=NULL;struct program_segment *fresh=NULL;
    for(struct program_segment *s=prog->segments;s;s=s->next)
        if(s->offset==offset && s->va==va && s->filesz==filesz && s->memsz==memsz && s->exec==exec){p=s->pages;break;}
    if(!p){
        /* Others may map a cached program's frames already: only a new one gets more. */
        if(prog->state!=PROGRAM_NEW)return -N_ERR_INVALID;
        if(!(fresh=kmalloc(sizeof *fresh)))return -N_ERR_NO_MEMORY;
        int64_t r=shared_object(end-start,true,&p);
        if(r<0){kfree(fresh);return (int)r;}
        p->source_offset=offset;p->source_bytes=filesz;p->source_skip=va-start;
    }
    struct shared_mapping *m=kzalloc(sizeof *m);
    if(!m){
        if(fresh){shared_release(&p->object);kfree(fresh);}
        return -N_ERR_NO_MEMORY;
    }
    /* The frames a peer read already are mapped now; the others on first touch. */
    uint64_t flags=PTE_U|PTE_SHARED|PTE_SHARED_RO|(exec?0:pte_nx),irq;bool ok=true;
    for(size_t i=0;ok && p->resident && i<p->count;i++){
        irq=irq_save();
        uint64_t frame=shared_frame(p,i,false);
        ok=!frame || vmm_map_page(t->pml4,start+i*PAGE_SIZE,frame,flags);
        irq_restore(irq);
    }
    irq=irq_save();
    if(ok && !p->source)p->source=handle_ref(doc); /* a new segment's, or an idle one's: the same content */
    if(!ok){
        irq_restore(irq);
        vmm_user_free(t->pml4,start,end-start);kfree(m);
        if(fresh){shared_release(&p->object);kfree(fresh);}
        return -N_ERR_NO_MEMORY;
    }
    if(fresh){
        object_ref(&p->object); /* the program's */
        *fresh=(struct program_segment){.next=prog->segments,.pages=p,.offset=offset,.va=va,.filesz=filesz,.memsz=memsz,
                                        .exec=exec};
        prog->segments=fresh;
    }
    object_ref(&p->object); /* the mapping's */
    *m=(struct shared_mapping){.next=t->shared_mappings,.object=&p->object,.va=start,.flags=flags,.bytes=end-start};
    t->shared_mappings=m;irq_restore(irq);
    return 0;
}
/* Pages a first touch of a program segment reads together: the touched one and the others of its
   aligned 64 KiB that no task has read, in one read from the document. */
#define PROGRAM_AROUND 16
/* Reads page index of p and those around it from source and adopts those no peer adopted meanwhile.
   Called with no guard held, holding p and source: the read waits for the disk. */
static int program_fill(struct shared_pages *p,struct handle *source,size_t index) {
    size_t first=ALIGN_DOWN(index,(size_t)PROGRAM_AROUND),last=MIN(first+PROGRAM_AROUND,p->count);
    if(!frames_free(4096+(last-first)))first=index,last=index+1; /* memory is short: what is touched only */
    uint64_t irq=irq_save();
    for(uint64_t *slot;first<index && (slot=demand_slot(p,first,false)) && *slot;)first++;
    for(uint64_t *slot;last>index+1 && (slot=demand_slot(p,last-1,false)) && *slot;)last--;
    irq_restore(irq);
    size_t bytes=(last-first)*PAGE_SIZE;
    uint8_t *buf=kzalloc(bytes);
    if(!buf)return -N_ERR_NO_MEMORY;
    /* the window's part of [source_skip, source_skip + source_bytes) */
    uint64_t lo=MAX((uint64_t)first*PAGE_SIZE,(uint64_t)p->source_skip),
             hi=MIN((uint64_t)last*PAGE_SIZE,p->source_skip+p->source_bytes);
    int r=0;
    if(lo<hi){
        int64_t got=store_document_read(source,p->source_offset+(lo-p->source_skip),buf+(lo-first*PAGE_SIZE),hi-lo,false);
        r=got<0?(int)got:(uint64_t)got!=hi-lo?-N_ERR_IO:0;
    }
    for(size_t i=first;r>=0 && i<last;i++){
        uint64_t frame=pmm_alloc();
        if(!frame){if(i==index)r=-N_ERR_NO_MEMORY;continue;}
        memcpy(phys_to_virt(frame),buf+(i-first)*PAGE_SIZE,PAGE_SIZE);
        irq=irq_save();
        uint64_t *slot=demand_slot(p,i,true);
        bool adopted=slot && !*slot;
        if(adopted){*slot=frame;p->resident++;}
        irq_restore(irq);
        if(!adopted)pmm_free(frame);
        if(!slot && i==index)r=-N_ERR_NO_MEMORY;
    }
    kfree(buf);
    return r;
}
static uint64_t gone_device_page; /* shared_demand_fault: what revoked device mappings show */
void shared_revoke_claim(const struct pci_claim *c) {
    shared_cancel_builds(NULL,c);
    for(struct task *t=task_list;t;t=t->all_next){
        if(t->is_thread || !t->pml4 || t->pml4==kernel_pml4)continue;
        for(struct shared_mapping *m=t->shared_mappings;m;m=m->next){
            if(shared_device_gone(m->object->priv,c))vmm_user_free(t->pml4,m->va,m->bytes);
        }
    }
    uint64_t irq=irq_save();device_pins_drop(c);irq_restore(irq);
}
/* The PTE flags page `page` (counted from the mapping's start) of m is mapped with: the mapping's
   own, or the access n_memory_protect set there, which never exceeds them (shared_protect). BSP
   IRQ guard held. */
static uint64_t shared_mapping_flags(const struct shared_mapping *m,size_t page) {
    uint64_t leaf=m->access?m->access[page/PAGE_SIZE]:0;
    uint8_t access=leaf?((const uint8_t *)phys_to_virt(leaf))[page%PAGE_SIZE]:0;
    if(!access)return m->flags;
    uint64_t flags=access==N_MEMORY_READ_WRITE?m->flags:m->flags&~PTE_W;
    return access==N_MEMORY_READ_EXECUTE?flags&~PTE_NX:flags|pte_nx;
}
static size_t shared_access_leaves(const struct shared_mapping *m) {
    return (m->bytes/PAGE_SIZE+PAGE_SIZE-1)/PAGE_SIZE;
}
/* n_memory_protect of [va, va+bytes) (page aligned, not empty) with access (vp: its VM_ bits),
   which overlaps mapping m of task t: the range lies in m and asks no more than m grants. The
   access is kept for each page, also one not mapped now (paged memory, an untouched demand page,
   a page of a device gone), so every PTE a later fault installs keeps to it; the present pages
   change at once. BSP IRQ guard held. */
static int64_t shared_protect(struct task *t,struct shared_mapping *m,uint64_t va,uint64_t bytes,int access,int vp) {
    if(va<m->va || bytes>m->bytes-(va-m->va))return -N_ERR_NO_MEMORY; /* reaches past it: its guard page */
    if(shared_cancelled(m->object->priv))return -N_ERR_CLOSED;
    /* Write access only in a writable mapping. Execution never: shared memory is mapped without
       it (n_shared_map refuses N_MEMORY_READ_EXECUTE), and a program's code (PTE_SHARED_RO) keeps
       the access it was mapped with. Else an app could execute pages it, or another holder of the
       memory, writes through a second mapping. */
    if(((vp&VM_W) && !(m->flags&PTE_W)) || ((vp&VM_X) && (m->flags&(PTE_NX|PTE_SHARED_RO))))return -N_ERR_DENIED;
    if(!m->access && !(m->access=kzalloc(shared_access_leaves(m)*sizeof *m->access)))return -N_ERR_NO_MEMORY;
    size_t first=(va-m->va)/PAGE_SIZE,end=first+bytes/PAGE_SIZE;
    for(size_t i=first/PAGE_SIZE;i<=(end-1)/PAGE_SIZE;i++)
        if(!m->access[i] && !(m->access[i]=pmm_alloc_zeroed()))return -N_ERR_NO_MEMORY;
    for(size_t at=first;at<end;){
        size_t n=MIN(end-at,PAGE_SIZE-at%PAGE_SIZE);
        memset((uint8_t *)phys_to_virt(m->access[at/PAGE_SIZE])+at%PAGE_SIZE,access,n);
        at+=n;
    }
    return vmm_user_protect(t->pml4,va,bytes,vp,true);
}
/* A first touch of a reserved shared page, from user code or a kernel copy
 * on its behalf: allocate the object's frame (or adopt the one a peer already
 * created) and map it with the mapping's protection (or the page's own, set by
 * n_memory_protect). A program's page is read from its document first, with the
 * guard let go meanwhile (*slept): the mapping is looked up again after it. */
static bool shared_fault(uint64_t addr,bool write,bool *slept) {
    struct task *owner=task_process();
    if(!owner)return false;
    uint64_t page=ALIGN_DOWN(addr,PAGE_SIZE),present=vmm_get_pte(owner->pml4,page);
    if(present&PTE_P){
        /* A sibling mapped it first, or a write to a page without write access, which only a
           pager's page can be given (below); anything else stays a protection violation. */
        if(!write || (present&PTE_W))return true;
        if(!(present&PTE_SHARED))return false;
    }else{
        int reserved=vmm_demand_fault(owner->pml4,addr,write); /* a private reserved page */
        if(reserved>0)return true;
        if(reserved<0){
            kprintf("[memory] pid %d (%s): no frame for reserved page %p, %lu pages free\n",
                    owner->pid,owner->name,(void *)page,(unsigned long)pmm_free_pages());
            report_memory_exhaustion(owner,PAGE_SIZE);
            return false;
        }
    }
again:;
    uint64_t irq=irq_save();bool ok=false,starved=false;
    struct shared_pages *fill=NULL;struct handle *source=NULL;size_t at=0;
    for(struct shared_mapping *m=owner->shared_mappings;m;m=m->next){
        if(page<m->va || page-m->va>=m->bytes)continue;
        struct shared_pages *p=m->object->priv;
        if(shared_cancelled(p))break;
        size_t index=(page-m->va)/PAGE_SIZE;uint8_t cache;
        uint64_t flags=shared_mapping_flags(m,index);
        struct shared_pages *leaf=shared_leaf(p,&index,&cache,NULL,NULL);
        if(leaf->kind==SHARED_PAGED){
            if(write && !(flags&PTE_W))break;
            struct object **from;uint64_t *slot=paged_slot(leaf,index,false,&from);
            uint64_t entry=slot?*slot:0;
            if(entry && shared_cancelled((*from)->priv))entry=0; /* its source's revocation is unbinding it */
            if((entry&PAGED_BOUND) && (!write || (entry&PAGED_WRITE))){
                ok=vmm_map_page(owner->pml4,page,entry&PTE_ADDR,shared_page_flags(leaf,index,flags));
                break;
            }
            /* The pager is asked; the mapping is looked up again once it answered. */
            bool answered=pager_wait(leaf->pager,index,write);
            irq_restore(irq);
            if(!answered)return false;
            if(slept)*slept=true;
            goto again;
        }
        if(leaf->kind==SHARED_RANGE && !leaf->owned && pci_user_revoked(leaf->claim)){
            /* The device's driver ended. One kernel page, all-ones at first, stands in for every
               register and aperture page: reads see a function gone from the bus, writes (a
               doorbell) land nowhere that matters. */
            if(write && !(flags&PTE_W))break;
            if(!gone_device_page && (gone_device_page=pmm_alloc()))memset(phys_to_virt(gone_device_page),0xFF,PAGE_SIZE);
            starved=!gone_device_page;
            ok=gone_device_page && vmm_map_page(owner->pml4,page,gone_device_page,
                                                flags&~(PTE_PWT|PTE_PCD|PTE_PAT4K));
            break;
        }
        /* Claim loss removes the entire composite mapping. Its other RAM or
           live-device segments still refer to their own valid backing. */
        if((leaf->kind!=SHARED_DEMAND && !shared_device_gone(p,NULL)) || (write && !(flags&PTE_W)))break;
        if(vmm_get_pte(owner->pml4,page)&PTE_P){ok=!write || (vmm_get_pte(owner->pml4,page)&PTE_W);break;}
        if(leaf->source && !shared_frame(leaf,index,false)){
            /* A program's page no task has read: the object and its opening stay while it is read. */
            fill=leaf;at=index;source=handle_ref(leaf->source);
            object_ref(&fill->object);
            break;
        }
        uint64_t frame=shared_frame(p,(page-m->va)/PAGE_SIZE,true);
        starved=!frame;
        ok=frame && vmm_map_page(owner->pml4,page,frame,flags|cache_pte(cache));
        break;
    }
    irq_restore(irq);
    if(fill){
        int r=program_fill(fill,source,at);
        handle_close(source);
        object_unref(&fill->object);
        if(slept)*slept=true;
        if(r>=0)goto again; /* mapped from the frame now there, if the mapping still is */
        if(r==-N_ERR_NO_MEMORY)starved=true;
        else kprintf("[program] pid %d (%s): page %p could not be read from the program (%d)\n",
                     owner->pid,owner->name,(void *)page,r);
    }
    if(starved){
        kprintf("[shared] pid %d (%s): no frame for page %p, %lu pages free\n",
                owner->pid,owner->name,(void *)page,(unsigned long)pmm_free_pages());
        report_memory_exhaustion(owner,PAGE_SIZE);
    }
    return ok;
}
bool shared_demand_fault(uint64_t addr,bool write) { return shared_fault(addr,write,NULL); }
/* Maps a shared object the caller holds a handle to (its own, or one received
   as a channel attachment): there is no reaching into another app's handles. */
static int64_t sys_shared_map(int h,size_t bytes,int access) {
    if(!bytes || bytes>SIZE_MAX-(PAGE_SIZE-1) || (access!=N_MEMORY_READ && access!=N_MEMORY_READ_WRITE))return -N_ERR_INVALID;
    struct task *owner=task_process();struct handle *f=handle_get(h);
    if(!f)return -N_ERR_NO_HANDLE;
    struct shared_pages *p=shared_of(f);
    if(!p){handle_close(f);return -N_ERR_WRONG_KIND;}
    /* the handle's own rights bound the mapping's: a read-only handle maps read-only */
    if(access==N_MEMORY_READ_WRITE && !f->writable){handle_close(f);return -N_ERR_DENIED;}
    /* the mapping holds the object itself: the handle may be closed while it stays */
    object_ref(&p->object);handle_close(f);
    if(shared_cancelled(p) || shared_device_gone(p,NULL)){object_unref(&p->object);return -N_ERR_CLOSED;}
    uint64_t irq;
    bytes=ALIGN_UP(bytes,PAGE_SIZE);
    if(bytes!=p->bytes){object_unref(&p->object);return -N_ERR_INVALID;}
    struct shared_mapping *m=kzalloc(sizeof *m);
    if(!m){object_unref(&p->object);return -N_ERR_NO_MEMORY;}
    /* Reserve the VA range (reusing released ones) before publishing any PTE, so
       no other reservation can choose this partially mapped span. */
    irq=irq_save();
    int64_t pinned=shared_device_pin(p); /* device pages keep one memory type */
    uint64_t va=pinned<0?0:mmap_reserve(owner,bytes);
    if(!va){
        if(pinned>=0)shared_device_unpin(p);
        irq_restore(irq);object_unref(&p->object);kfree(m);return pinned<0?pinned:-N_ERR_NO_MEMORY;
    }
    size_t mapped=0;
    uint64_t flags=PTE_U|PTE_SHARED|pte_nx|(access==N_MEMORY_READ_WRITE?PTE_W:PTE_SHARED_RO);
    /* Pin DMA cache types before installing the first PTE, including every
       selected composite segment; rollback drops the same references. */
    shared_dma_maps(p,true);
    *m=(struct shared_mapping){.next=shared_building,.object=&p->object,.va=va,.flags=flags,.bytes=bytes,
                               .building_owner=owner,.cancelled=shared_cancelled(p) || shared_device_gone(p,NULL)};
    shared_building=m;irq_restore(irq);
    /* Demand pages not yet touched by any peer stay unmapped until a fault. */
    for(;mapped<p->count;mapped++){
        size_t index=mapped;uint8_t cache;
        irq=irq_save();
        if(m->cancelled || current_task->killed){irq_restore(irq);break;}
        shared_leaf(p,&index,&cache,NULL,NULL);
        uint64_t frame=shared_frame(p,mapped,false);
        bool ok=!frame || vmm_map_page(owner->pml4,va+mapped*PAGE_SIZE,frame,
                                       shared_page_flags(p,mapped,flags)|cache_pte(cache));
        irq_restore(irq);if(!ok)break;
        if(need_resched)schedule();
    }
    /* A cancellation during construction cannot miss an unpublished mapping
       and leave it live afterward. Publication and this final check are one
       BSP critical section. */
    irq=irq_save();
    int64_t result=m->cancelled || shared_cancelled(p) || shared_device_gone(p,NULL)?-N_ERR_CLOSED:
                   current_task->killed?-N_ERR_STOPPED:-N_ERR_NO_MEMORY;
    if(mapped!=p->count || result!=-N_ERR_NO_MEMORY){
        while(m->revoking)wq_wait(&m->build_wait);
        m->cancelled=m->revoking=true;
        irq_restore(irq);
        vmm_user_free(owner->pml4,va,bytes);
        irq=irq_save();shared_build_remove(m);shared_dma_maps(p,false);shared_device_unpin(p);
        m->revoking=false;wq_wake_all(&m->build_wait);irq_restore(irq);
        mmap_hole_add(owner,va,bytes+PAGE_SIZE);
        object_unref(&p->object);kfree(m);return result;
    }
    shared_build_remove(m);m->building_owner=NULL;m->next=owner->shared_mappings;
    owner->shared_mappings=m;irq_restore(irq);
    return (int64_t)va;
}
/* Removes a mapping's pages (and the all-ones pages a revoked device range may have received)
   and drops its object. */
static void shared_unmap(struct task *owner,struct shared_mapping *m) {
    /* Keep every DMA cache pin through PTE removal and the completed TLB
       shootdown. A preempting view cannot retype still-reachable old pages. */
    vmm_user_free(owner->pml4,m->va,m->bytes);
    uint64_t irq=irq_save();shared_dma_maps(m->object->priv,false);shared_device_unpin(m->object->priv);irq_restore(irq);
    object_unref(m->object);
    for(size_t i=0;m->access && i<shared_access_leaves(m);i++)if(m->access[i])pmm_free(m->access[i]);
    kfree(m->access);kfree(m);
}
void sys_shared_cleanup(struct task *owner) {
    while(owner->shared_mappings){struct shared_mapping *m=owner->shared_mappings;
        owner->shared_mappings=m->next;
        shared_unmap(owner,m);
    }
    mmap_holes_free(owner);
    programs_trim(0); /* its programs may be idle now */
}

/* Freed address space of the mmap region. Without reuse every map/unmap cycle
   consumes fresh addresses and the page tables covering them, so a process
   that keeps allocating and freeing large blocks leaks kernel memory. Every
   reservation keeps one unmapped guard page after itself; a hole includes it.
   The low region below 2 GiB keeps its own holes and frontier. */
struct mmap_hole { struct mmap_hole *next; uint64_t va, len; };
void mmap_hole_add(struct task *owner, uint64_t va, uint64_t len) {
    if (!owner || !len) return;
    bool low = va < USER_MMAP_BASE;
    struct mmap_hole **holes = low ? &owner->low_holes : &owner->mmap_holes;
    uint64_t *next = low ? &owner->low_next : &owner->mmap_next;
    struct mmap_hole *spare = kmalloc(sizeof *spare);
    uint64_t irq = irq_save();
    struct mmap_hole **link = holes, *prev = NULL;
    while (*link && (*link)->va + (*link)->len < va) { prev = *link; link = &(*link)->next; }
    struct mmap_hole *h = *link;
    if (h && h->va <= va + len) {
        /* Overlapping or adjacent: the union, swallowing later neighbours. */
        uint64_t end = MAX(h->va + h->len, va + len);
        h->va = MIN(h->va, va); h->len = end - h->va;
        while (h->next && h->next->va <= h->va + h->len) {
            struct mmap_hole *n = h->next;
            end = MAX(h->va + h->len, n->va + n->len); h->len = end - h->va;
            h->next = n->next; kfree(n);
        }
    } else if (spare) {
        *spare = (struct mmap_hole){ .next = h, .va = va, .len = len };
        *link = h = spare; spare = NULL;
    } else { irq_restore(irq); return; } /* the range stays unused, nothing breaks */
    /* The last hole simply lowers the reservation frontier. */
    if (!h->next && h->va + h->len >= *next) {
        *next = h->va;
        if (prev) prev->next = NULL; else *holes = NULL;
        kfree(h);
    }
    irq_restore(irq);
    kfree(spare);
}
/* First fit for len bytes plus their guard page; 0 when no hole fits. */
static uint64_t mmap_hole_take(struct mmap_hole **holes, uint64_t len) {
    uint64_t need = len + PAGE_SIZE, va = 0;
    uint64_t irq = irq_save();
    for (struct mmap_hole **link = holes; *link; link = &(*link)->next) {
        struct mmap_hole *h = *link;
        if (h->len < need) continue;
        va = h->va; h->va += need; h->len -= need;
        if (!h->len) { *link = h->next; kfree(h); }
        break;
    }
    irq_restore(irq);
    return va;
}
void mmap_holes_free(struct task *owner) {
    while (owner->mmap_holes) { struct mmap_hole *h = owner->mmap_holes; owner->mmap_holes = h->next; kfree(h); }
    while (owner->low_holes) { struct mmap_hole *h = owner->low_holes; owner->low_holes = h->next; kfree(h); }
}
/* len bytes and a guard page from a region's holes, else at its frontier below top. */
static uint64_t region_reserve(struct mmap_hole **holes, uint64_t *next, uint64_t top, uint64_t len) {
    if (!len || len > top) return 0;
    uint64_t va = mmap_hole_take(holes, len);
    if (va) return va;
    uint64_t irq = irq_save();
    va = *next;
    if (va >= top || len + PAGE_SIZE > top - va) va = 0;
    else *next = va + len + PAGE_SIZE; /* reserve before any page loop */
    irq_restore(irq);
    return va;
}
uint64_t mmap_reserve(struct task *owner, uint64_t len) {
    return region_reserve(&owner->mmap_holes, &owner->mmap_next, USER_STACK_TOP - USER_STACK_MAX, len);
}

/* Native access to page protection: readable, writable or executable, never
   writable and executable together. */
static int access_flags(int access) {
    switch (access) {
    case N_MEMORY_READ: return 0;
    case N_MEMORY_READ_WRITE: return VM_W;
    case N_MEMORY_READ_EXECUTE: return VM_X;
    default: return -1;
    }
}

/* Zeroed pages in the reservation region above the program (window buffers
   and shared mappings live there too), or below 2 GiB with N_MEMORY_LOW.
   Beyond a few pages they receive frames on first touch, so a process pays
   only for what it uses; a reservation larger than all free memory still
   fails here. These finite BSP helpers do not sleep. Kernel-mode timer IRQs
   request a reschedule but cannot switch their continuation (idt.c). Keep IRQ
   delivery enabled for large allocations; each PTE publication has its own
   short guard. */
#define RESERVE_EAGER (64 * 1024)
static int64_t sys_memory_reserve(size_t len, int access) {
    struct task *t = task_process();
    int vp = access_flags(access & ~N_MEMORY_LOW);
    /* No size cap of its own: free memory and the address region bound it. */
    if (len == 0 || len > SIZE_MAX - (PAGE_SIZE - 1) || vp < 0) return -N_ERR_INVALID;
    len = ALIGN_UP(len, PAGE_SIZE);
    if (!frames_free(len / PAGE_SIZE)) { report_memory_exhaustion(t, len); return -N_ERR_NO_MEMORY; }
    uint64_t va = access & N_MEMORY_LOW ? region_reserve(&t->low_holes, &t->low_next, USER_LOW_TOP, len)
                                        : mmap_reserve(t, len);
    if (!va) return -N_ERR_NO_MEMORY;
    int r = len <= RESERVE_EAGER ? vmm_user_alloc(t->pml4, va, len, vp) : vmm_user_reserve(t->pml4, va, len, vp);
    if (r < 0) {
        vmm_user_free(t->pml4, va, len);
        mmap_hole_add(t, va, len + PAGE_SIZE);
        report_memory_exhaustion(t, len);
        return -N_ERR_NO_MEMORY;
    }
    return (int64_t)va;
}

/* Releases reserved pages or a whole shared mapping; window buffers stay with their window. */
static int64_t sys_memory_release(uint64_t va, size_t len) {
    struct task *t = task_process();
    bool low = va < USER_MMAP_BASE;
    uint64_t base = low ? t->low_base : USER_MMAP_BASE, frontier = low ? t->low_next : t->mmap_next;
    if (va & (PAGE_SIZE - 1) || va < base || va + len > frontier || va + len < va) return -N_ERR_INVALID;
    if(len>SIZE_MAX-(PAGE_SIZE-1))return -N_ERR_INVALID;
    len=ALIGN_UP(len,PAGE_SIZE);
    if(shared_build_overlap(t,va,len))return -N_ERR_NOT_READY;
    struct shared_mapping **link=&t->shared_mappings;
    while(*link){struct shared_mapping *m=*link;
        if(va<m->va+m->bytes && m->va<va+len){
            if(va!=m->va || len!=m->bytes)return -N_ERR_INVALID;
            *link=m->next;shared_unmap(t,m);
            /* sys_shared_map reserved the range and its guard page: reuse them */
            if(!low)mmap_hole_add(t,va,len+PAGE_SIZE);
            return 0;
        }link=&m->next;
    }
    for (uint64_t a = va; a < va + len; a += PAGE_SIZE)
        if (vmm_get_pte(t->pml4, a) & PTE_SHARED) return -N_ERR_INVALID; /* a window buffer */
    vmm_user_free(t->pml4, va, len);
    /* The unmapped page right after the range is the reservation's guard. */
    uint64_t end = va + len;
    bool guard = end < frontier && !(vmm_get_leaf(t->pml4, end) & (PTE_P | PTE_DEMAND)); /* not an untouched reserved page */
    for (struct shared_mapping *m = t->shared_mappings; guard && m; m = m->next)
        if (end >= m->va && end < m->va + m->bytes) guard = false; /* an untouched demand page */
    mmap_hole_add(t, va, guard ? len + PAGE_SIZE : len);
    return 0;
}

/* Private pages take the access as asked; the pages of a shared mapping keep within the
   mapping's, also those a fault maps later (shared_protect). */
static int64_t sys_memory_protect(uint64_t va, size_t len, int access) {
    int vp = access_flags(access);
    if (vp < 0 || va & (PAGE_SIZE - 1) || va >= USER_TOP || va + len > USER_TOP || va + len < va) return -N_ERR_INVALID;
    struct task *t = task_process();
    if (!len) return 0;
    uint64_t end = ALIGN_UP(va + len, PAGE_SIZE), irq = irq_save();
    int64_t r = -N_ERR_NOT_READY;
    if (!shared_build_overlap(t, va, len)) {
        struct shared_mapping *m = t->shared_mappings;
        while (m && !(va < m->va + m->bytes && m->va < end)) m = m->next;
        r = m ? shared_protect(t, m, va, end - va, access, vp) : vmm_user_protect(t->pml4, va, len, vp, false);
    }
    irq_restore(irq);
    return r;
}

/* Copies the system log's retained text. */
static int64_t sys_system_log(char *buf, size_t n) {
    if (!user_ok_w(buf, n)) return -N_ERR_BAD_ADDRESS;
    return klog_read(buf, n);
}

/* GPU requests can now sleep, and their images no longer have a small fixed
   extent. Page-sized user operations revalidate after every possible switch.
   Keep this local to GPU IO: other APIs retain their established atomicity. */
static int gpu_user_span(void *pointer,size_t bytes,bool write) {
    uint64_t base=(uint64_t)pointer;
    if(base>=USER_TOP || bytes>USER_TOP-base)return -N_ERR_BAD_ADDRESS;
    for(size_t at=0;at<bytes;){
        size_t piece=MIN(bytes-at,(size_t)PAGE_SIZE);
        bool valid=write?user_ok_w((uint8_t *)pointer+at,piece):user_ok((uint8_t *)pointer+at,piece);
        if(!valid)return -N_ERR_BAD_ADDRESS;
        at+=piece;
        if(need_resched)schedule();
        if(current_task->killed)return -N_ERR_STOPPED;
    }
    return 0;
}

/* ---- native ABI (docs/nocturne-native-abi.md) ---- */

/* The caller's handle h when is(h) holds: a reference to close, or NULL with *error set. */
static struct handle *handle_get_as(int h, bool (*is)(struct handle *), int64_t *error) {
    struct handle *handle = handle_get(h);
    *error = !handle ? -N_ERR_NO_HANDLE : !is(handle) ? -N_ERR_WRONG_KIND : 0;
    if (*error && handle) { handle_close(handle); handle = NULL; }
    return handle;
}

/* Copy user bytes in bounded pieces: no long non-preemptible copy, and a
   sibling unmapping the source meanwhile is reported, not faulted on. */
int copy_from_user(void *dst, const void *usrc, size_t n) {
    for (size_t done = 0; done < n;) {
        size_t piece = MIN(n - done, (size_t)16384);
        uint64_t flags = irq_save();
        bool ok = user_ok((const uint8_t *)usrc + done, piece);
        if (ok) memcpy((uint8_t *)dst + done, (const uint8_t *)usrc + done, piece);
        irq_restore(flags);
        if (!ok) return -N_ERR_BAD_ADDRESS;
        done += piece;
    }
    return 0;
}

int copy_to_user(void *udst, const void *src, size_t n) {
    uint64_t flags = irq_save();
    bool ok = user_ok_w(udst, n);
    if (ok) memcpy(udst, src, n);
    irq_restore(flags);
    return ok ? 0 : -N_ERR_BAD_ADDRESS;
}
static int gpu_user_copy(void *destination,const void *source,size_t bytes,bool output) {
    for(size_t at=0;at<bytes;){
        size_t piece=MIN(bytes-at,(size_t)PAGE_SIZE);
        int r=output?copy_to_user((uint8_t *)destination+at,(const uint8_t *)source+at,piece):
                     copy_from_user((uint8_t *)destination+at,(const uint8_t *)source+at,piece);
        if(r<0)return r;
        at+=piece;
        if(need_resched)schedule();
        if(current_task->killed)return -N_ERR_STOPPED;
    }
    return 0;
}

static int64_t sys_handle_close(int h) {
    struct handle *handle = handle_get(h);
    if (!handle) return -N_ERR_NO_HANDLE;
    /* Only an explicit close commits a replacement: an app that ends with the
       handle open leaves the old document as it was. */
    int committed = store_document_is(handle) ? store_document_commit(handle, N_COMMIT_WAIT) : 0;
    handle_close(handle);
    int closed = handle_remove(h);
    return committed < 0 ? committed : closed;
}

static int64_t sys_handle_kind(int h) {
    uint64_t flags = irq_save();
    struct handle *handle = handle_peek(h);
    int kind = handle ? handle_kind(handle) : -N_ERR_NO_HANDLE;
    irq_restore(flags);
    return kind;
}

/* Documents: the store checks the handle's kind and access itself. */
static int64_t sys_document_open(const char *upath, int access) {
    char *path;
    int r = user_string(upath, &path);
    if (r < 0) return r;
    struct handle *h;
    r = store_document_open(path, access, true, &h);
    kfree(path);
    return r < 0 ? r : handle_install(h);
}

static int64_t sys_document_io(int h, uint64_t off, void *buf, size_t n, bool write) {
    struct handle *d = handle_get(h);
    if (!d) return -N_ERR_NO_HANDLE;
    int64_t r = write ? store_document_write(d, off, buf, n, true) : store_document_read(d, off, buf, n, true);
    handle_close(d);
    return r;
}

static int64_t sys_document_size(int h) {
    struct handle *d = handle_get(h);
    if (!d) return -N_ERR_NO_HANDLE;
    int64_t r = store_document_size(d);
    handle_close(d);
    return r;
}

/* An app's struct n_entry is the first bytes of the kernel's (common/abi.h): the calls fill the
   whole entry and copy as much of it as the app's record holds. */
_Static_assert(__builtin_offsetof(struct n_entry, id) == N_ENTRY_FIRST_BYTES, "the first n_entry ends before id");
static int entry_bytes_check(size_t bytes) {
    if (bytes < N_ENTRY_FIRST_BYTES) return -N_ERR_INVALID;
    return bytes > sizeof(struct n_entry) ? -N_ERR_UNSUPPORTED : 0; /* a later record than this kernel's */
}

static int64_t sys_document_info(int h, void *uout, size_t bytes) {
    int r = entry_bytes_check(bytes);
    if (r < 0) return r;
    struct handle *d = handle_get(h);
    if (!d) return -N_ERR_NO_HANDLE;
    struct n_entry e;
    r = store_document_info(d, &e);
    handle_close(d);
    return r < 0 ? r : copy_to_user(uout, &e, bytes);
}

static int64_t sys_document_set_size(int h, uint64_t size) {
    struct handle *d = handle_get(h);
    if (!d) return -N_ERR_NO_HANDLE;
    int64_t r = store_document_set_size(d, size);
    handle_close(d);
    return r;
}

static int64_t sys_document_claim(int h, bool claim, bool wait) {
    struct handle *d = handle_get(h);
    if (!d) return -N_ERR_NO_HANDLE;
    int64_t r = claim ? store_document_claim(d, wait) : store_document_release(d);
    handle_close(d);
    return r;
}

static int64_t sys_document_commit(int h, int how) {
    struct handle *d = handle_get(h);
    if (!d) return -N_ERR_NO_HANDLE;
    int64_t r = store_document_commit(d, how);
    handle_close(d);
    return r;
}

static int64_t sys_document_save(const char *upath, const void *udata, size_t n, bool append) {
    char *path;
    int r = user_string(upath, &path);
    if (r < 0) return r;
    r = append ? store_append(path, udata, n, true) : store_save(path, udata, n, true);
    kfree(path);
    return r;
}

static int64_t sys_entry_info(const char *upath, void *uout, size_t bytes) {
    int r = entry_bytes_check(bytes);
    if (r < 0) return r;
    char *path;
    r = user_string(upath, &path);
    if (r < 0) return r;
    struct n_entry e;
    r = store_entry_info(path, true, &e);
    kfree(path);
    return r < 0 ? r : copy_to_user(uout, &e, bytes);
}

static int64_t sys_folder_list(const char *upath, void *uout, size_t max, uint64_t start, size_t bytes) {
    int r = entry_bytes_check(bytes);
    if (r < 0) return r;
    if (max > SIZE_MAX / bytes) return -N_ERR_INVALID;
    char *path;
    r = user_string(upath, &path);
    if (r < 0) return r;
    int64_t n = store_folder_list(path, start, uout, max, bytes, true);
    kfree(path);
    return n;
}

static int64_t sys_entry_remove(const char *upath) {
    char *path;
    int r = user_string(upath, &path);
    if (r < 0) return r;
    r = store_entry_remove(path, true);
    kfree(path);
    return r;
}

static int64_t sys_entry_move(const char *ufrom, const char *uto, bool replace) {
    char *from, *to = NULL;
    int r = user_string(ufrom, &from);
    if (r < 0) return r;
    r = user_string(uto, &to);
    if (r >= 0) r = store_entry_move(from, to, replace, true);
    kfree(from);
    kfree(to);
    return r;
}

static int64_t sys_folder_create(const char *upath) {
    char *path;
    int r = user_string(upath, &path);
    if (r < 0) return r;
    r = store_folder_create(path, true);
    kfree(path);
    return r;
}

static int64_t sys_entry_watch(const char *upath, uint64_t what) {
    if (what != N_WATCH_NAME && what != N_WATCH_FOLDER) return -N_ERR_INVALID; /* the upper bits too */
    char *path;
    int r = user_string(upath, &path);
    if (r < 0) return r;
    int64_t h = store_entry_watch(path, (uint32_t)what, true);
    kfree(path);
    return h;
}

static int64_t sys_channel_pair(int32_t *uout) {
    if (!user_ok_w(uout, 2 * sizeof *uout)) return -N_ERR_BAD_ADDRESS;
    struct handle *a, *b;
    int e = message_channel_pair(&a, &b);
    if (e < 0) return e;
    uint64_t flags = irq_save();
    int ha = handle_install_locked(a), hb = ha < 0 ? ha : handle_install_locked(b);
    int error = hb < 0 ? hb : !user_ok_w(uout, 2 * sizeof *uout) ? -N_ERR_BAD_ADDRESS : 0;
    if (error) {
        /* the table's references come back to us: each end is closed once */
        if (hb >= 0) handle_take_locked(hb);
        if (ha >= 0) handle_take_locked(ha);
        irq_restore(flags);
        handle_close(a);
        handle_close(b);
        return error;
    }
    uout[0] = ha;
    uout[1] = hb;
    irq_restore(flags);
    return 0;
}

/* The byte streams given to apps as their input, output and log when no app is at the other end
   (the console, the system log, nothing; sys/stream.h): a send is all of it, a receive takes
   what is there, without message boundaries or attachments (docs/nocturne-native-abi.md §4). */
static bool byte_stream(struct handle *h) {
    return h->object->ops->read || h->object->ops->write;
}

static int64_t sys_channel_send(int h, const void *udata, size_t n, int attach) {
    struct handle *f = handle_get(h);
    if (!f) return -N_ERR_NO_HANDLE;
    struct handle *att = NULL;
    int64_t r = 0;
    if (attach >= 0) {
        att = handle_get(attach);
        if (!att) r = -N_ERR_NO_HANDLE;
        /* documents and shared pages only: a channel end sent through its own
           channel would keep itself alive */
        else if (handle_kind(att) != N_KIND_DOCUMENT && handle_kind(att) != N_KIND_SHARED) r = -N_ERR_WRONG_KIND;
    }
    if (r < 0) {
    } else if (message_channel_is(f)) {
        r = message_channel_send(f, udata, n, att);
    } else if (byte_stream(f)) {
        r = att || !f->object->ops->write ? -N_ERR_UNSUPPORTED : f->object->ops->write(f, udata, n, true);
    } else {
        r = -N_ERR_WRONG_KIND;
    }
    handle_close(att);
    handle_close(f);
    return r;
}

static int64_t sys_channel_receive(int h, void *ubuf, size_t cap, int flags, void *uoutput, bool credentials) {
    size_t output_size=credentials?sizeof(struct n_channel_message):sizeof(int32_t);
    if ((credentials && !uoutput) || (uoutput && !user_ok_w(uoutput,output_size))) return -N_ERR_BAD_ADDRESS;
    struct handle *f = handle_get(h);
    if (!f) return -N_ERR_NO_HANDLE;
    bool wait = flags & N_CHANNEL_WAIT, peek = flags & N_CHANNEL_PEEK, discard = flags & N_CHANNEL_DISCARD;
    if (flags & ~(N_CHANNEL_WAIT | N_CHANNEL_PEEK | N_CHANNEL_DISCARD) || (peek && discard)) { handle_close(f); return -N_ERR_INVALID; }
    int64_t r;
    struct handle *att = NULL;
    struct n_channel_message message={.attached=-1};
    if (message_channel_is(f)) {
        r = message_channel_receive(f, ubuf, cap, wait, peek, discard, &att, &message.sender_app);
    } else if (byte_stream(f)) {
        if (credentials || peek || discard || !f->object->ops->read) r = -N_ERR_UNSUPPORTED;
        else if (!cap) r = 0;
        else {
            r = f->object->ops->read(f, ubuf, cap, true, wait);
            if (r == 0) r = -N_ERR_CLOSED; /* nothing more will come */
        }
    } else {
        r = -N_ERR_WRONG_KIND;
    }
    handle_close(f);
    if (r >= 0 && uoutput && (credentials || !peek)) {
        if (att && !peek) {
            message.attached=(int32_t)handle_install(att);att=NULL;
            if (message.attached<0) r=message.attached;
        }
        if (r>=0 && copy_to_user(uoutput,credentials?(const void *)&message:(const void *)&message.attached,output_size)<0) {
            if (message.attached>=0) handle_remove(message.attached);
            r=-N_ERR_BAD_ADDRESS;
        }
    }
    handle_close(att); /* the caller did not ask for attachments */
    return r;
}

static int64_t sys_app_launch(const char *upath, const struct n_launch *ul) {
    char *path;
    int r = user_string(upath, &path);
    if (r < 0) return r;
    struct n_launch l = {0};
    char **argv = NULL, **settings = NULL;
    struct handle **handles = NULL, *app = NULL;
    size_t argc = 0, bytes = 0;
    int64_t ret = copy_from_user(&l, ul, sizeof l);
    const size_t limit = USER_STACK_MAX / 2; /* what the new app's stack can hold */
    if (ret < 0) goto out;
    if (l.flags) { ret = -N_ERR_INVALID; goto out; }
    if (l.argument_count > limit / 8 || l.setting_count > limit / 8 || l.handle_count > 4096) {
        l = (struct n_launch){0};
        ret = -N_ERR_TOO_MANY_ARGUMENTS;
        goto out;
    }
    argc = l.argument_count ? l.argument_count : 1;
    argv = kzalloc(argc * sizeof *argv);
    settings = kzalloc((l.setting_count + 1) * sizeof *settings);
    handles = kzalloc((l.handle_count + 1) * sizeof *handles);
    if (!argv || !settings || !handles) { ret = -N_ERR_NO_MEMORY; goto out; }
    if (!l.argument_count) {
        argv[0] = strdup(path);
        if (!argv[0]) { ret = -N_ERR_NO_MEMORY; goto out; }
    }
    for (size_t i = 0; i < l.argument_count; i++) {
        const char *ua;
        if ((r = copy_from_user(&ua, &l.arguments[i], sizeof ua)) < 0) { ret = r; goto out; }
        if ((r = user_string(ua, &argv[i])) < 0) { ret = r; goto out; }
        bytes += strlen(argv[i]) + 1;
    }
    for (size_t i = 0; i < l.setting_count; i++) {
        const char *us;
        if ((r = copy_from_user(&us, &l.settings[i], sizeof us)) < 0) { ret = r; goto out; }
        if ((r = user_string(us, &settings[i])) < 0) { ret = r; goto out; }
        bytes += strlen(settings[i]) + 1;
        if (!strchr(settings[i], '=')) { ret = -N_ERR_INVALID; goto out; }
    }
    if (bytes > limit) { ret = -N_ERR_TOO_MANY_ARGUMENTS; goto out; }
    for (size_t i = 0; i < l.handle_count; i++) {
        int32_t h;
        if ((r = copy_from_user(&h, &l.handles[i], sizeof h)) < 0) { ret = r; goto out; }
        if (h < 0) continue;
        if (!(handles[i] = handle_get(h))) { ret = -N_ERR_NO_HANDLE; goto out; }
    }
    if ((r = proc_app_create(&app)) < 0) { ret = r; goto out; }
    struct proc_launch launch = {.argc = (int)argc, .argv = argv, .setting_count = (int)l.setting_count,
                                 .settings = settings, .handles = handles, .handle_count = l.handle_count,
                                 .parent = task_process(), .app = app->object};
    r = proc_launch(path, &launch);
    if (r < 0) ret = r;
    else { ret = handle_install(app); app = NULL; }
out:
    handle_close(app);
    if (argv) for (size_t i = 0; i < argc; i++) kfree(argv[i]);
    if (settings) for (size_t i = 0; i < l.setting_count; i++) kfree(settings[i]);
    if (handles) for (size_t i = 0; i < l.handle_count; i++) handle_close(handles[i]);
    kfree(argv);
    kfree(settings);
    kfree(handles);
    kfree(path);
    return ret;
}

static int64_t sys_app_result(int h, int64_t timeout_ms, int *ustatus) {
    int64_t r;
    struct handle *app = handle_get_as(h, proc_app_is, &r);
    if (!app) return r;
    int status = 0;
    r = proc_app_result(app, timeout_ms < 0 ? UINT64_MAX : (uint64_t)timeout_ms, &status);
    handle_close(app);
    if (!r && ustatus) r = copy_to_user(ustatus, &status, sizeof status);
    return r;
}

static int64_t sys_app_stop(int h, int flags) {
    int64_t r;
    struct handle *app = handle_get_as(h, proc_app_is, &r);
    if (!app) return r;
    int pid = proc_app_pid(app);
    handle_close(app);
    if (flags & ~N_APP_STOP_TREE) return -N_ERR_INVALID;
    if (!pid) return 0; /* it has already ended */
    if (flags & N_APP_STOP_TREE) stop_tree(pid, 1);
    else task_kill(pid);
    return 0;
}

static int64_t sys_app_open(int64_t id) {
    if (id <= 0 || id > 0x7fffffff) return -N_ERR_NO_APP;
    struct handle *app;
    int r = proc_app_open((int)id, &app);
    return r < 0 ? r : handle_install(app);
}

static int64_t sys_app_id(int h) {
    int64_t r;
    struct handle *app = handle_get_as(h, proc_app_is, &r);
    if (!app) return r;
    r = proc_app_id(app);
    handle_close(app);
    return r;
}

/* A handle whose messages are added to the system log (the serial port and the text console). */
static int64_t sys_system_log_open(void) {
    struct handle *h = stream_system_log_open();
    return h ? handle_install(h) : -N_ERR_NO_MEMORY;
}

/* A stream of its own on the sound mixer; N_ERR_BUSY when every stream is taken. */
static int64_t sys_audio_open(void) {
    struct handle *h;
    int r = audio_stream_open(&h);
    return r < 0 ? r : handle_install(h);
}

static int64_t sys_audio_submit(int h, const void *frames, uint64_t count, uint32_t flags) {
    if (flags & ~N_AUDIO_WAIT) return -N_ERR_INVALID;
    int64_t r;
    struct handle *stream = handle_get_as(h, audio_stream_is, &r);
    if (!stream) return r;
    r = audio_stream_submit(stream, frames, count, flags & N_AUDIO_WAIT);
    handle_close(stream);
    return r;
}

static int64_t sys_audio_status(int h, struct n_audio_status *out) {
    int64_t r;
    struct handle *stream = handle_get_as(h, audio_stream_is, &r);
    if (!stream) return r;
    r = audio_stream_status(stream, out);
    handle_close(stream);
    return r;
}

static int64_t sys_audio_flush(int h) {
    int64_t r;
    struct handle *stream = handle_get_as(h, audio_stream_is, &r);
    if (!stream) return r;
    r = audio_stream_flush(stream);
    handle_close(stream);
    return r;
}

/* The sound's way out (struct n_audio_output): the volume and muting set as set says, then all of
   it read back. */
static int64_t sys_audio_output(struct n_audio_output *uio, uint32_t set) {
    struct n_audio_output io = {0};
    if (set & ~(N_AUDIO_SET_VOLUME | N_AUDIO_SET_MUTED)) return -N_ERR_INVALID;
    if (set && copy_from_user(&io, uio, sizeof io)) return -N_ERR_BAD_ADDRESS;
    if (((set & N_AUDIO_SET_VOLUME) && (io.volume < 0 || io.volume > 100)) ||
        ((set & N_AUDIO_SET_MUTED) && io.muted > 1))
        return -N_ERR_INVALID;
    if (set & N_AUDIO_SET_VOLUME) audio_set_volume(io.volume);
    if (set & N_AUDIO_SET_MUTED) audio_set_muted(io.muted != 0);
    memset(&io, 0, sizeof io);
    io.volume = audio_volume();
    io.muted = audio_muted();
    const char *device = audio_output();
    if (device) strlcpy(io.device, device, sizeof io.device);
    return copy_to_user(uio, &io, sizeof io) ? -N_ERR_BAD_ADDRESS : 0;
}

/* Fills the buffer from the kernel's entropy pool in pieces, each copied out
   under the user-memory check. */
static int64_t sys_random(void *ubuf, size_t n) {
    uint8_t piece[256];
    for (size_t done = 0; done < n;) {
        size_t k = MIN(n - done, sizeof piece);
        entropy_get(piece, k);
        if (copy_to_user((uint8_t *)ubuf + done, piece, k) < 0) { memset(piece, 0, sizeof piece); return -N_ERR_BAD_ADDRESS; }
        done += k;
    }
    memset(piece, 0, sizeof piece);
    return (int64_t)n;
}

static int64_t sys_events_create(void) {
    struct handle *h;
    int r = events_create(&h);
    return r < 0 ? r : handle_install(h);
}

static int64_t sys_events_watch(int q, int h, uint32_t interest, uint64_t token) {
    const uint32_t known = N_EVENT_RECEIVABLE | N_EVENT_SENDABLE | N_EVENT_CLOSED | N_EVENT_APP_ENDED | N_EVENT_SAVED;
    if (!interest || (interest & ~known)) return -N_ERR_INVALID;
    int64_t r;
    struct handle *queue = handle_get_as(q, events_is, &r);
    if (!queue) return r;
    struct event_watch *spare = kmalloc(events_watch_size());
    if (!spare) { handle_close(queue); return -N_ERR_NO_MEMORY; }
    uint64_t flags = irq_save();
    struct handle *watched = handle_peek(h);
    r = watched ? events_watch_locked(queue, watched, interest, token, &spare) : -N_ERR_NO_HANDLE;
    irq_restore(flags);
    kfree(spare);
    handle_close(queue);
    return r;
}

static int64_t sys_events_watch_address(int q, uint64_t address, uint32_t expected, uint64_t token) {
    if ((address & 3) || !user_ok((void *)address, sizeof(uint32_t))) return -N_ERR_BAD_ADDRESS;
    int64_t r;
    struct handle *queue = handle_get_as(q, events_is, &r);
    if (!queue) return r;
    r = events_watch_address(queue, (volatile uint32_t *)address, expected, token);
    handle_close(queue);
    return r;
}

static int64_t sys_events_timer(int q, uint64_t ms, int repeat, uint64_t token) {
    int64_t r;
    struct handle *queue = handle_get_as(q, events_is, &r);
    if (!queue) return r;
    r = events_timer(queue, ms, repeat != 0, token);
    handle_close(queue);
    return r;
}

static int64_t sys_events_unwatch(int q, uint64_t token) {
    int64_t r;
    struct handle *queue = handle_get_as(q, events_is, &r);
    if (!queue) return r;
    r = events_unwatch(queue, token);
    handle_close(queue);
    return r;
}

static int64_t sys_events_wait(int q, struct n_event *uout, size_t max, int64_t timeout_ms) {
    int64_t r;
    timeout_ms = events_wait_resume(timeout_ms); /* before anything can end the call */
    struct handle *queue = handle_get_as(q, events_is, &r);
    if (!queue) return r;
    r = events_wait(queue, uout, max, timeout_ms);
    handle_close(queue);
    return r;
}

/* sys_channel_receive's checks, in its order, before a message channel end's AP poll. */
static bool channel_receive_ap(struct regs *r, struct handle *h, bool credentials, int64_t *result) {
    void *uoutput = (void *)r->r8;
    int flags = (int)r->r10;
    if (flags & ~(N_CHANNEL_WAIT | N_CHANNEL_PEEK | N_CHANNEL_DISCARD) ||
        ((flags & N_CHANNEL_PEEK) && (flags & N_CHANNEL_DISCARD)) || (credentials && !uoutput)) return false;
    if (uoutput) {
        if (!vmm_ap_user_begin(uoutput, credentials ? sizeof(struct n_channel_message) : sizeof(int32_t), true))
            return false;
        vmm_ap_uaccess_end();
    }
    return message_channel_is(h) && message_channel_receive_ap(h, flags & N_CHANNEL_WAIT, result);
}

enum ap_syscall syscall_ap(struct regs *r) {
    int64_t result;
    switch (r->rax) {
    case SYS_CPU_INDEX: result = cpu_current_index(); break;
    case SYS_THREAD_ID: result = current_task->pid; break;
    case SYS_APP_SELF: result = task_process()->pid; break;
    case SYS_UPTIME: result = (int64_t)uptime_ms(); break;
    case SYS_TIME: result = time_now(); break;
    case SYS_SLEEP: return sleep_ap(r->rdi);
    case SYS_USER_LANGUAGES: /* every request a browser makes asks for them */
        if (!user_languages_ap((char *)r->rdi, r->rsi, &result)) return AP_SYSCALL_BSP;
        break;
    case SYS_TCP_SEND: case SYS_TCP_SEND_UNTIL: case SYS_TCP_RECV:
        return net_ap_syscall(r) ? AP_SYSCALL_DONE : AP_SYSCALL_BSP;
    case SYS_AUDIO_STATUS: case SYS_CHANNEL_RECEIVE: case SYS_CHANNEL_RECEIVE_FROM: case SYS_WIN_RECEIVE:
    case SYS_WIN_STATE: case SYS_EVENTS_WAIT: case SYS_APP_RESULT: {
        if (current_task->killed) return AP_SYSCALL_BSP; /* the BSP ends the process */
        ap_read_begin();
        struct handle *h = handle_ap_peek((int)r->rdi);
        enum ap_syscall served;
        if (r->rax == SYS_EVENTS_WAIT)
            served = events_wait_ap(h, (struct n_event *)r->rsi, (size_t)r->rdx, (int64_t)r->r10, &result);
        else {
            bool done = r->rax == SYS_AUDIO_STATUS ? audio_stream_status_ap(h, (struct n_audio_status *)r->rsi, &result)
                      : r->rax == SYS_WIN_RECEIVE ? wm_receive_ap(h, (size_t)r->rdx, (int64_t)r->r10, &result)
                      : r->rax == SYS_WIN_STATE ? wm_state_ap(h, &result)
                      : r->rax == SYS_APP_RESULT ? proc_app_result_ap(h, (int64_t)r->rsi, &result)
                      : channel_receive_ap(r, h, r->rax == SYS_CHANNEL_RECEIVE_FROM, &result);
            served = done ? AP_SYSCALL_DONE : AP_SYSCALL_BSP;
        }
        ap_read_end();
        if (served != AP_SYSCALL_DONE) return served;
        break;
    }
    default: return AP_SYSCALL_BSP;
    }
    r->rax = (uint64_t)result;
    return AP_SYSCALL_DONE;
}

void syscall_dispatch(struct regs *r) {
    /* An AP trap may be queued as a BSP continuation when a sibling aborts
       its process. Do not execute that pending syscall after it is killed:
       a blocking syscall could enroll a new wait after task_kill's wake and
       never reach the return-to-user kill check. */
    if (current_task->killed) task_exit(current_task->kill_code);
    sti();
    uint64_t a = r->rdi, b = r->rsi, c = r->rdx, d = r->r10, e = r->r8;
    int64_t ret = -N_ERR_UNSUPPORTED;
    switch (r->rax) {
    case SYS_EXIT: task_exit_app((int)a);
    case SYS_AUDIO_FLUSH: ret = sys_audio_flush((int)a); break;
    case SYS_SLEEP: sleep_resume(a); ret = 0; break;
    case SYS_UPTIME: ret = uptime_ms(); break;
    case SYS_TIME: ret = time_now(); break;
    case SYS_SHARED_CREATE: ret = sys_shared_create(a,false); break;
    case SYS_SHARED_RESERVE: ret = sys_shared_create(a,true); break;
    case SYS_SHARED_MAP: ret = sys_shared_map((int)a,b,(int)c); break;
    case SYS_SHARED_VIEW: ret = sys_shared_view((int)a,b,c,(uint32_t)d,(uint32_t)e); break;
    case SYS_SHARED_COMPOSE: ret = sys_shared_compose((const struct n_shared_range *)a,b,(uint32_t)c); break;
    case SYS_SHARED_REVOKE: ret = sys_shared_revoke((int)a); break;
    case SYS_PCI_CLAIM: {
        struct handle *claim = NULL;
        ret = pci_user_claim((uint8_t)a,(uint8_t)b,(uint8_t)c,&claim);
        ret = new_handle(ret,claim);
        break;
    }
    case SYS_PCI_BAR: ret = sys_pci_bar((int)a,(uint32_t)b); break;
    case SYS_PCI_GIVE_SCREEN: ret = sys_pci_give_screen((int)a,(int)b,(const struct n_scanout *)c); break;
    case SYS_PCI_ACPI_EVAL: ret = sys_pci_acpi_eval((int)a,(const struct n_acpi_eval *)b,(void *)c,d); break;
    case SYS_PCI_SCREEN_STATE: ret = sys_pci_screen_state((int)a,b,(struct n_screen_state *)c,(int64_t)d); break;
    case SYS_PCI_SCREEN_IMAGE: ret = sys_pci_screen_image((int)a,(int)b,(const struct n_scanout *)c); break;
    case SYS_PCI_SCREEN_MODES: ret = sys_pci_screen_modes((int)a,(const struct n_screen_modes *)b); break;
    case SYS_DMA_CREATE: ret = sys_dma_create((int)a,b,(uint32_t)c); break;
    case SYS_DMA_ADDRESSES: ret = sys_dma_addresses((int)a,b,(uint64_t *)c,d); break;
    case SYS_PCI_INFO: case SYS_PCI_CONFIG_READ: case SYS_PCI_CONFIG_WRITE:
    case SYS_PCI_INTERRUPTS: case SYS_PCI_INTERRUPT_TAKE: case SYS_PCI_HOLD_SCREEN: case SYS_PCI_SCREEN_VBLANK:
    case SYS_PCI_ACPI_NOTIFY_TAKE: case SYS_PCI_SCREEN_IMAGE_END: {
        struct handle *f = handle_get((int)a);
        if (!f) { ret = -N_ERR_NO_HANDLE; break; }
        if (r->rax == SYS_PCI_INFO) {
            struct n_pci_function info;
            ret = pci_user_info(f, &info);
            if (!ret) ret = copy_to_user((void *)b, &info, sizeof info);
        } else if (r->rax == SYS_PCI_CONFIG_READ) ret = pci_user_config_read(f,(uint32_t)b,(uint32_t)c,(uint32_t)d);
        else if (r->rax == SYS_PCI_CONFIG_WRITE) ret = pci_user_config_write(f,(uint32_t)b,(uint32_t)c,(uint32_t)d);
        else if (r->rax == SYS_PCI_INTERRUPTS) ret = pci_user_interrupts(f,(uint32_t)b);
        else if (r->rax == SYS_PCI_HOLD_SCREEN) ret = pci_user_hold_screen(f, b != 0);
        else if (r->rax == SYS_PCI_SCREEN_VBLANK) ret = pci_user_screen_vblank(f, b, c);
        else if (r->rax == SYS_PCI_ACPI_NOTIFY_TAKE) ret = pci_user_acpi_notify_take(f, (int64_t)b);
        else if (r->rax == SYS_PCI_SCREEN_IMAGE_END) ret = pci_user_screen_image_end(f, (int64_t)b);
        else ret = pci_user_interrupt_take(f,(int64_t)b);
        handle_close(f);
        break;
    }
    case SYS_MEMORY_RESERVE: ret = sys_memory_reserve(a, (int)b); break;
    case SYS_MEMORY_RELEASE: ret = sys_memory_release(a, b); break;
    case SYS_MEMORY_PROTECT: ret = sys_memory_protect(a, b, (int)c); break;
    case SYS_CHANNEL_LISTEN: ret = sys_channel_named((const char *)a,b,true); break;
    case SYS_CHANNEL_CONNECT: ret = sys_channel_named((const char *)a,b,false); break;
    case SYS_CHANNEL_ACCEPT: ret = sys_channel_accept((int)a); break;
    case SYS_PROCLIST: ret = sys_proclist((struct n_procinfo *)a, (int)b); break;
    case SYS_SYSINFO: ret = sys_sysinfo((struct n_sysinfo *)a); break;
    case SYS_CPU_INFO: {
        struct n_cpuinfo info;
        if (!user_ok_w((void *)a, sizeof info)) ret = -N_ERR_BAD_ADDRESS;
        else { smp_get_info(&info); memcpy((void *)a, &info, sizeof info); ret = 0; }
        break;
    }
    case SYS_CLOCK_INFO: {
        struct n_clockinfo info = {.version = 1};
        if (!user_ok_w((void *)a, sizeof info)) { ret = -N_ERR_BAD_ADDRESS; break; }
        uint32_t ca, cb, cc, cd, max;
        cpuid(0x80000000, 0, &max, &cb, &cc, &cd);
        cd = 0;
        if (max >= 0x80000007) cpuid(0x80000007, 0, &ca, &cb, &cc, &cd);
        if (!vm_clock_owned() && tsc_hz >= 1000000 && (cd & (1u << 8))) {
            info.flags = N_CLOCK_USER_TSC;
            info.tsc_hz = tsc_hz;
        }
        uint64_t flags = irq_save();
        uint64_t reference_phys = vm_clock_page();
        struct task *owner = task_process();
        if (reference_phys && !owner->clock_page &&
            owner->mmap_next <= USER_STACK_TOP - USER_STACK_MAX - 2 * PAGE_SIZE) {
            uint64_t va = owner->mmap_next;
            if (vmm_map_page(owner->pml4, va, reference_phys, PTE_P | PTE_U | PTE_SHARED | PTE_SHARED_RO | pte_nx)) {
                owner->clock_page = va;
                owner->mmap_next = va + 2 * PAGE_SIZE;
            }
        }
        if (owner->clock_page) {
            info.flags = N_CLOCK_HV_REFERENCE;
            info.reference_page = owner->clock_page;
        }
        timer_clock_sample(&info.uptime_ms, &info.tsc_sample, &info.reference_sample_100ns);
        memcpy((void *)a, &info, sizeof info);
        irq_restore(flags);
        ret = 0;
        break;
    }
    case SYS_THREAD_CREATE:
        if (!user_ok((void *)a, 1) || !user_ok((void *)c, 1) ||
            (vmm_get_pte(current_task->pml4, a) & PTE_NX) ||
            (vmm_get_pte(current_task->pml4, c) & PTE_NX)) ret = -N_ERR_BAD_ADDRESS;
        else ret = task_thread_create(c, a, b, d);
        break;
    case SYS_THREAD_JOIN: ret = task_thread_join((int)a); break;
    case SYS_THREAD_DETACH: ret = task_thread_detach((int)a); break;
    case SYS_THREAD_EXIT: task_exit(0);
    case SYS_THREAD_ID: ret = current_task->pid; break;
    case SYS_CPU_INDEX: ret = 0; break;
    case SYS_THREAD_TLS:
        if (!user_ok_w((void *)a, b) || b < 16 || b > PAGE_SIZE) ret = -N_ERR_BAD_ADDRESS;
        else { current_task->tls_base = a; wrmsr(0xC0000100, a); ret = 0; }
        break;
    case SYS_THREAD_STACK_BOUNDS: {
        if (!user_ok_w((void *)a, sizeof(uintptr_t)) || !user_ok_w((void *)b, sizeof(uintptr_t))) {
            ret = -N_ERR_BAD_ADDRESS; break;
        }
        /* Main stacks grow on demand within the VM's reserved interval.
           Native threads instead own their scheduler-allocated mapping. */
        uintptr_t low = current_task->user_stack_size ? current_task->user_stack : USER_STACK_TOP - USER_STACK_MAX;
        uintptr_t high = current_task->user_stack_size ? current_task->user_stack + current_task->user_stack_size : USER_STACK_TOP;
        memcpy((void *)a, &low, sizeof low);
        memcpy((void *)b, &high, sizeof high);
        ret = 0; break;
    }
    case SYS_WAIT_ADDRESS:
        if ((a & 3) || !user_ok((void *)a, sizeof(uint32_t))) ret = -N_ERR_BAD_ADDRESS;
        else ret = task_wait_address((volatile void *)a, (uint32_t)b, false, (unsigned)c);
        break;
    case SYS_WAIT_ADDRESS64:
        if ((a & 7) || !user_ok((void *)a, sizeof(uint64_t))) ret = -N_ERR_BAD_ADDRESS;
        else ret = task_wait_address((volatile void *)a, b, true, (unsigned)c);
        break;
    case SYS_WAKE_ADDRESS:
        if ((a & 3) || !user_ok((void *)a, sizeof(uint32_t))) ret = -N_ERR_BAD_ADDRESS;
        else ret = task_wake_address((volatile uint32_t *)a, (unsigned)b);
        break;
    case SYS_GPU_INFO: {
        struct n_gpu_info info;
        if (!user_ok_w((void *)a, sizeof info)) ret = -N_ERR_BAD_ADDRESS;
        else { gpu_get_info(&info); memcpy((void *)a, &info, sizeof info); ret = 0; }
        break;
    }
    case SYS_GPU_RENDER: {
        struct n_gpu_render request;
        if (!user_ok((void *)a, sizeof request)) { ret = -N_ERR_BAD_ADDRESS; break; }
        memcpy(&request, (void *)a, sizeof request);
        if (!n_gpu_frame_bytes(request.width,request.height)) {
            ret = -N_ERR_INVALID; break;
        }
        size_t bytes = n_gpu_frame_bytes(request.width,request.height);
        if (c < bytes) { ret = -N_ERR_INVALID; break; }
        ret=gpu_user_span((void *)b,bytes,true);if(ret<0)break;
        uint32_t *pixels=kmalloc(bytes);
        if(!pixels){ret=-N_ERR_NO_MEMORY;break;}
        ret=gpu_render(&request,pixels,bytes);
        if(!ret)ret=gpu_user_copy((void *)b,pixels,bytes,true);
        kfree(pixels);
        break;
    }
    case SYS_GPU_RENDER_BATCH: {
        /* Snapshot the complete finite request before the driver uses it.
           A caller changing its user buffer cannot grow a validated batch. */
        struct n_gpu_batch request;
        if (!user_ok((void *)a, sizeof request)) { ret = -N_ERR_BAD_ADDRESS; break; }
        memcpy(&request, (void *)a, sizeof request);
        if (!request.width || !request.height ||
            !n_gpu_frame_bytes(request.width,request.height) ||
            request.triangle_count > N_GPU_MAX_TRIANGLES) {
            ret = -N_ERR_INVALID; break;
        }
        size_t bytes = n_gpu_frame_bytes(request.width,request.height);
        if (c < bytes) { ret = -N_ERR_INVALID; break; }
        ret=gpu_user_span((void *)b,bytes,true);if(ret<0)break;
        uint32_t *pixels=kmalloc(bytes);
        if(!pixels){ret=-N_ERR_NO_MEMORY;break;}
        ret=gpu_render_batch(&request,pixels,bytes);
        if(!ret)ret=gpu_user_copy((void *)b,pixels,bytes,true);
        kfree(pixels);
        break;
    }
    case SYS_GPU_RENDER_LIST: {
        struct n_gpu_scene request;
        if(copy_from_user(&request,(const void *)a,sizeof request)<0){ret=-N_ERR_BAD_ADDRESS;break;}
        size_t bytes=n_gpu_frame_bytes(request.width,request.height);
        size_t vertices=(size_t)request.triangle_count*3,vertex_bytes=vertices*sizeof(struct n_gpu_vertex);
        if(!bytes || d<bytes || vertex_bytes/sizeof(struct n_gpu_vertex)!=vertices){ret=-N_ERR_INVALID;break;}
        ret=vertex_bytes?gpu_user_span((void *)b,vertex_bytes,false):0;
        if(!ret)ret=gpu_user_span((void *)c,bytes,true);
        if(ret<0)break;
        struct n_gpu_vertex *snapshot=vertex_bytes?kmalloc(vertex_bytes):NULL;
        uint32_t *pixels=kmalloc(bytes);
        if((vertex_bytes && !snapshot) || !pixels){kfree(snapshot);kfree(pixels);ret=-N_ERR_NO_MEMORY;break;}
        ret=vertex_bytes?gpu_user_copy(snapshot,(const void *)b,vertex_bytes,false):0;
        if(!ret)ret=gpu_render_list(&request,snapshot,pixels,bytes);
        if(!ret)ret=gpu_user_copy((void *)c,pixels,bytes,true);
        kfree(snapshot);kfree(pixels);
        break;
    }
    case SYS_GPU_BLIT: {
        struct n_gpu_blit request;
        if (!user_ok((void *)a, sizeof request)) { ret = -N_ERR_BAD_ADDRESS; break; }
        memcpy(&request, (void *)a, sizeof request);
        if (!request.width || !request.height ||
            !request.source_width || !request.source_height ||
            !n_gpu_frame_bytes(request.width,request.height) ||
            !n_gpu_frame_bytes(request.source_width,request.source_height) ||
            !request.source_w || !request.source_h ||
            request.source_x > request.source_width || request.source_y > request.source_height ||
            request.source_w > request.source_width - request.source_x ||
            request.source_h > request.source_height - request.source_y) {
            ret = -N_ERR_INVALID; break;
        }
        size_t source_bytes = n_gpu_frame_bytes(request.source_width,request.source_height);
        size_t output_bytes = n_gpu_frame_bytes(request.width,request.height);
        if (c < source_bytes || e < output_bytes) { ret = -N_ERR_INVALID; break; }
        ret=gpu_user_span((void *)b,source_bytes,false);
        if(!ret)ret=gpu_user_span((void *)d,output_bytes,true);
        if(ret<0)break;
        /* Snapshot input before any blocking resource/fence operation. Private
           output is copied only after completion and user-span revalidation:
           a sibling may unmap the caller's memory while this thread sleeps. */
        uint32_t *source=kmalloc(source_bytes),*pixels=kmalloc(output_bytes);
        if(!source || !pixels){kfree(source);kfree(pixels);ret=-N_ERR_NO_MEMORY;break;}
        ret=gpu_user_copy(source,(const void *)b,source_bytes,false);
        if(!ret)ret=gpu_blit(&request,source,source_bytes,pixels,output_bytes);
        if(!ret)ret=gpu_user_copy((void *)d,pixels,output_bytes,true);
        kfree(source);kfree(pixels);
        break;
    }
    case SYS_YIELD: schedule(); ret = 0; break;
    case SYS_POWER:
        /* the power thread ends this app with the others */
        pm_request(a == POWER_REBOOT ? PM_REBOOT : PM_POWER_OFF, false);
        while (!current_task->killed) sleep_ms(1000);
        ret = -N_ERR_STOPPED;
        break;
    case SYS_SYSTEM_LOG: ret = sys_system_log((char *)a, b); break;
    case SYS_PCILIST:
        if (!user_ok_w((void *)a, b * sizeof(struct n_pciinfo))) ret = -N_ERR_BAD_ADDRESS;
        else ret = pci_list((struct n_pciinfo *)a, (int)b);
        break;
    case SYS_HANDLE_CLOSE: ret = sys_handle_close((int)a); break;
    case SYS_HANDLE_KIND: ret = sys_handle_kind((int)a); break;
    case SYS_DOCUMENT_OPEN: ret = sys_document_open((const char *)a, (int)b); break;
    case SYS_DOCUMENT_READ_AT: ret = sys_document_io((int)a, b, (void *)c, d, false); break;
    case SYS_DOCUMENT_WRITE_AT: ret = sys_document_io((int)a, b, (void *)c, d, true); break;
    case SYS_DOCUMENT_SIZE: ret = sys_document_size((int)a); break;
    case SYS_DOCUMENT_SET_SIZE: ret = sys_document_set_size((int)a, b); break;
    case SYS_DOCUMENT_CLAIM: ret = sys_document_claim((int)a, true, b != 0); break;
    case SYS_DOCUMENT_RELEASE: ret = sys_document_claim((int)a, false, false); break;
    case SYS_DOCUMENT_SAVE: ret = sys_document_save((const char *)a, (const void *)b, c, false); break;
    case SYS_DOCUMENT_APPEND: ret = sys_document_save((const char *)a, (const void *)b, c, true); break;
    case SYS_DOCUMENT_COMMIT: ret = sys_document_commit((int)a, (int)b); break;
    case SYS_DOCUMENT_INFO: ret = sys_document_info((int)a, (void *)b, c); break;
    case SYS_ENTRY_INFO: ret = sys_entry_info((const char *)a, (void *)b, N_ENTRY_FIRST_BYTES); break;
    case SYS_ENTRY_INFO_SIZED: ret = sys_entry_info((const char *)a, (void *)b, c); break;
    case SYS_ENTRY_REMOVE: ret = sys_entry_remove((const char *)a); break;
    case SYS_ENTRY_MOVE: ret = sys_entry_move((const char *)a, (const char *)b, false); break;
    case SYS_ENTRY_REPLACE: ret = sys_entry_move((const char *)a, (const char *)b, true); break;
    case SYS_FOLDER_CREATE: ret = sys_folder_create((const char *)a); break;
    case SYS_FOLDER_LIST: ret = sys_folder_list((const char *)a, (void *)b, c, d, N_ENTRY_FIRST_BYTES); break;
    case SYS_FOLDER_LIST_SIZED: ret = sys_folder_list((const char *)a, (void *)b, c, d, e); break;
    case SYS_CHANNEL_PAIR: ret = sys_channel_pair((int32_t *)a); break;
    case SYS_CHANNEL_SEND: ret = sys_channel_send((int)a, (const void *)b, c, (int)d); break;
    case SYS_CHANNEL_RECEIVE: ret = sys_channel_receive((int)a, (void *)b, c, (int)d, (void *)e, false); break;
    case SYS_CHANNEL_RECEIVE_FROM: ret = sys_channel_receive((int)a, (void *)b, c, (int)d, (void *)e, true); break;
    case SYS_APP_LAUNCH: ret = sys_app_launch((const char *)a, (const struct n_launch *)b); break;
    case SYS_APP_RESULT: ret = sys_app_result((int)a, (int64_t)b, (int *)c); break;
    case SYS_APP_STOP: ret = sys_app_stop((int)a, (int)b); break;
    case SYS_APP_SELF: ret = task_process()->pid; break;
    case SYS_APP_PARENT: ret = task_process()->parent ? task_process()->parent->pid : 0; break;
    case SYS_EVENTS_CREATE: ret = sys_events_create(); break;
    case SYS_EVENTS_WATCH: ret = sys_events_watch((int)a, (int)b, (uint32_t)c, d); break;
    case SYS_EVENTS_UNWATCH: ret = sys_events_unwatch((int)a, b); break;
    case SYS_EVENTS_WATCH_ADDRESS: ret = sys_events_watch_address((int)a, b, (uint32_t)c, d); break;
    case SYS_EVENTS_TIMER: ret = sys_events_timer((int)a, b, (int)c, d); break;
    case SYS_EVENTS_WAIT: ret = sys_events_wait((int)a, (struct n_event *)b, c, (int64_t)d); break;
    case SYS_APP_OPEN: ret = sys_app_open((int64_t)a); break;
    case SYS_APP_ID: ret = sys_app_id((int)a); break;
    case SYS_SYSTEM_LOG_OPEN: ret = sys_system_log_open(); break;
    case SYS_AUDIO_OPEN: ret = sys_audio_open(); break;
    case SYS_AUDIO_SUBMIT: ret = sys_audio_submit((int)a, (const void *)b, c, (uint32_t)d); break;
    case SYS_AUDIO_STATUS: ret = sys_audio_status((int)a, (struct n_audio_status *)b); break;
    case SYS_AUDIO_OUTPUT: ret = sys_audio_output((struct n_audio_output *)a, (uint32_t)b); break;
    case SYS_RANDOM: ret = sys_random((void *)a, b); break;
    case SYS_USER_SETTINGS_WATCH: ret = user_settings_watch((uint32_t)a); break;
    case SYS_USER_SETTINGS_CHANGES: ret = user_settings_changes((int)a); break;
    case SYS_USER_LANGUAGES: ret = user_languages((char *)a, b); break;
    case SYS_USER_LANGUAGES_SET: ret = user_languages_set((const char *)a, b); break;
    case SYS_WATCH_TAKE: ret = watch_take((int)a); break;
    case SYS_ENTRY_WATCH: ret = sys_entry_watch((const char *)a, b); break;
    case SYS_PAGER_CREATE: ret = sys_pager_create(a, (int32_t *)b); break;
    case SYS_PAGER_TAKE: ret = sys_pager_take((int)a, (struct n_pager_fault *)b, c, (int64_t)d); break;
    case SYS_PAGER_SUPPLY: ret = sys_pager_supply((int)a, (const struct n_pager_supply *)b, c); break;
    case SYS_PAGER_REVOKE: ret = sys_pager_revoke((int)a, b, c, (uint32_t)d); break;
    case SYS_PAGER_FAIL: ret = sys_pager_fail((int)a, b, c); break;
    default:
        if (r->rax >= SYS_WIN_CREATE && r->rax < SYS_NET_INFO) ret = wm_syscall((int)r->rax, a, b, c, d, e);
        else if (r->rax >= SYS_NET_INFO && r->rax < SYS_NET_LIMIT) ret = net_syscall((int)r->rax, a, b, c, d, e);
        break;
    }
    r->rax = (uint64_t)ret;
}
