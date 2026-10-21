/* System call dispatch (int 0x80). */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/smp.h"
#include "arch/hyperv.h"
#include "sys/sched.h"
#include "sys/syscall.h"
#include "sys/proc.h"
#include "sys/events.h"
#include "fs/vfs.h"
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
#include "dev/pm.h"
#include "abi.h"
_Static_assert(PAGE_SIZE == N_PAGE_SIZE, "native mapping granule ABI");

_Static_assert(sizeof(struct n_gpu_blit) == 32, "GPU blit ABI");

extern char cpu_brand[64];
int pci_list(struct n_pciinfo *out, int max);
void vmm_user_pages_split(uint64_t pml4, uint64_t out[6]);
int64_t net_syscall(int num, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e);
static int copy_to_user(void *udst, const void *src, size_t n);
static int copy_from_user(void *dst, const void *usrc, size_t n);

/* ---- user memory access ---- */

/* Is [p, p+n) user memory the process may read (or, with write, also write)? Missing stack
   pages are faulted in. The kernel must not write to a read-only user page itself: CR0.WP is on,
   so that would be a kernel page fault. */
static bool user_access(const void *p, size_t n, bool write) {
    uint64_t a = (uint64_t)p;
    if (n == 0) return true;
    if (a >= USER_TOP || a + n > USER_TOP || a + n < a) return false;
    struct task *t = current_task;
    for (uint64_t pg = ALIGN_DOWN(a, PAGE_SIZE); pg < a + n; pg += PAGE_SIZE) {
        uint64_t pte = vmm_get_pte(t->pml4, pg);
        if (!(pte & PTE_P)) {
            if (pg >= USER_STACK_TOP - USER_STACK_MAX && pg < USER_STACK_TOP) {
                if (vmm_user_alloc(t->pml4, pg, PAGE_SIZE, VM_W) < 0) return false;
                continue;
            }
            if (!shared_demand_fault(pg, write)) return false;
            pte = vmm_get_pte(t->pml4, pg);
        }
        if (!(pte & PTE_U)) return false;
        if (write && !(pte & PTE_W)) return false;
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


/* Raw lookup is allowed only inside a BSP IRQ guard. Never carry this
   borrowed pointer across validation, preemption or a blocking operation. */
static struct file *getfd(int fd) {
    if (fd < 0 || (size_t)fd >= task_process()->fd_capacity) return NULL;
    return task_process()->fds[fd];
}

static struct file *getfd_ref(int fd) {
    uint64_t flags = irq_save();
    struct file *f = getfd(fd);
    if (f) vfs_dup(f);
    irq_restore(flags);
    return f;
}

static int allocfd_locked(struct file *f) {
    return task_fd_alloc(task_process(),f);
}

/* Consumes the caller's reference only on successful publication. */
static int allocfd(struct file *f) {
    uint64_t flags = irq_save();
    int fd = allocfd_locked(f);
    irq_restore(flags);
    return fd;
}

/* ---- handlers ---- */

static int64_t handle_drop(int fd) {
    uint64_t flags = irq_save();
    struct file *f = getfd(fd);
    if (!f) { irq_restore(flags); return -N_ERR_NO_HANDLE; }
    task_process()->fds[fd] = NULL;
    vfs_poll_notify(f->vn);
    if((size_t)fd<task_process()->fd_hint)task_process()->fd_hint=(size_t)fd;
    irq_restore(flags);
    /* Final audio close may sleep. The detached slot's reference owns f. */
    vfs_close(f);
    return 0;
}

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
    if(!length || length>SIZE_MAX-sizeof(struct file))return -N_ERR_INVALID;
    char *key=kmalloc(length);if(!key)return -N_ERR_NO_MEMORY;
    struct file *file=NULL;uint64_t flags=irq_save();
    if(!user_ok(ukey,length)){irq_restore(flags);kfree(key);return -N_ERR_BAD_ADDRESS;}
    memcpy(key,ukey,length);
    int slot=task_fd_find(task_process());
    int result=slot>=0?(listen?channel_listen(key,length,&file):channel_connect(key,length,&file)):slot;
    if(!result)result=allocfd_locked(file);
    irq_restore(flags);kfree(key);
    /* The slot was checked and published in one BSP IRQ guard. If that
     * invariant changes, closing the unpublished reference remains safe. */
    if(result<0 && file)vfs_close(file);
    return result;
}
static int64_t sys_channel_accept(int fd) {
    struct file *listener=getfd_ref(fd);if(!listener)return -N_ERR_NO_HANDLE;
    struct file *server=NULL;uint64_t flags=irq_save();int slot=task_fd_find(task_process());
    int result=slot>=0?channel_accept(listener,&server):slot;
    if(!result)result=allocfd_locked(server);
    irq_restore(flags);vfs_close(listener);
    if(result<0 && server)vfs_close(server);
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

static int64_t sys_sysinfo(struct n_sysinfo *si) {
    if (!user_ok_w(si, sizeof *si)) return -N_ERR_BAD_ADDRESS;
    memset(si, 0, sizeof *si);
    si->total_mem = pmm_total_pages() * PAGE_SIZE;
    si->free_mem = pmm_free_pages() * PAGE_SIZE;
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
 * (shared_revoke_claim). DMA RAM goes back to the allocator only once the device was seen
 * stopped, or while its driver still runs (freeing what its device still uses is the driver's
 * bug; there is no IOMMU to stop it). DMA RAM has one memory type for all its mappings and the
 * direct map: write-back until the first uncached or write-combining view while nothing maps
 * it, then that type for good (x86 forbids one page with two types). */
#define SHARED_LEAF (PAGE_SIZE/sizeof(uint64_t))
enum {SHARED_RAM,SHARED_DEMAND,SHARED_RANGE,SHARED_VIEW,SHARED_COMPOSITE};
/* Physical views remain flattened. Revocation ancestry is independent of the
   backing, so creating a child never escapes an issuer's cancellation scope.
   References are released iteratively, not through an unbounded C call stack. */
struct shared_scope {struct shared_scope *parent;size_t refs;int issuer;bool revoked;};
/* Composite segments point only at backing roots, never at another composite.
   Cancellation references are separate, so flattening cannot discard a view's
   issuer or a nested composite's lifetime. end is the exclusive logical page. */
struct shared_part {struct file *file;size_t first,end;uint8_t cache;};
struct shared_pages {
    size_t count;uint8_t kind,cache;bool dma,owned,wc_allowed;
    struct shared_scope *scope;
    size_t maps;              /* a DMA root: mapping references, including composite segments */
    struct pci_claim *claim;  /* a BAR or DMA root: the claim it was made through */
    uint64_t base;struct file *parent;size_t first; /* a range's first frame; a view's object and first page */
    struct shared_part *parts;size_t part_count;
    struct shared_scope **dependencies;size_t dependency_count;
    uint64_t pages[];
};
struct shared_mapping {
    struct shared_mapping *next;struct file *file;uint64_t va,flags;size_t bytes;
    struct task *building_owner;bool cancelled,revoking;struct wait_queue build_wait;
};
static struct shared_mapping *shared_building; /* BSP-owned, before returning a VA */
static size_t shared_slots(const struct shared_pages *p) {
    return p->kind==SHARED_DEMAND?(p->count+SHARED_LEAF-1)/SHARED_LEAF:p->kind==SHARED_RAM?p->count:0;
}
static struct shared_pages *shared_root(struct shared_pages *p) {
    while(p->kind==SHARED_VIEW)p=p->parent->vn->priv;
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
                                        struct file **backing,size_t *span) {
    *cache=p->cache;
    while(p->kind==SHARED_VIEW){
        *index+=p->first;if(backing)*backing=p->parent;p=p->parent->vn->priv;
    }
    if(p->kind==SHARED_COMPOSITE){
        struct shared_part *part=shared_part_at(p,*index);
        size_t start=part==p->parts?0:part[-1].end;
        if(span)*span=part->end-*index;
        *index=part->first+*index-start;*cache=part->cache;
        if(backing)*backing=part->file;p=part->file->vn->priv;
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
        struct shared_pages *leaf=root->parts[i].file->vn->priv;
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
            bool match=scope?shared_depends(m->file->vn->priv,scope):shared_device_gone(m->file->vn->priv,claim);
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
/* BSP-owned, called with interrupts disabled or from the fault handler. */
static uint64_t shared_frame(struct shared_pages *p,size_t index,bool create) {
    uint8_t cache;p=shared_leaf(p,&index,&cache,NULL,NULL);
    if(p->kind==SHARED_RAM)return p->pages[index];
    if(p->kind==SHARED_RANGE)return p->base+index*PAGE_SIZE;
    uint64_t *leaf=&p->pages[index/SHARED_LEAF];
    if(!*leaf){if(!create || !(*leaf=pmm_alloc_zeroed()))return 0;}
    uint64_t *slot=(uint64_t *)phys_to_virt(*leaf)+index%SHARED_LEAF;
    if(!*slot && create)*slot=pmm_alloc_zeroed();
    return *slot;
}
static void shared_release(struct vnode *v) {
    struct shared_pages *p=v->priv;
    if(p){
        bool keep=p->dma && !pci_user_dma_reusable(p->claim); /* the device may still write there */
        if(keep)kprintf("[dma] %lu pages stay out of use: their device was not seen to stop\n",(unsigned long)p->count);
        else if(p->dma && p->cache!=N_CACHE_WRITE_BACK)
            vmm_direct_type(p->kind==SHARED_RAM?p->pages:NULL,p->base,p->count,0);
        for(size_t i=0;!keep && i<shared_slots(p);i++){
            if(!p->pages[i])continue;
            if(p->kind==SHARED_DEMAND){uint64_t *leaf=phys_to_virt(p->pages[i]);
                for(size_t j=0;j<SHARED_LEAF;j++)if(leaf[j])pmm_free(leaf[j]);}
            pmm_free(p->pages[i]);
        }
        if(p->kind==SHARED_RANGE && p->owned && !keep)for(size_t i=0;i<p->count;i++)pmm_free(p->base+i*PAGE_SIZE);
        if(p->parent)vfs_close(p->parent);
        for(size_t i=0;i<p->part_count;i++)vfs_close(p->parts[i].file);
        for(size_t i=0;i<p->dependency_count;i++)scope_drop(p->dependencies[i]);
        kfree(p->parts);kfree(p->dependencies);
        scope_drop(p->scope);
        pci_user_drop(p->claim);
        kfree(p);
    }
    kfree(v);
}
static struct vnode_ops shared_ops={.release=shared_release};
/* An empty object record of `bytes` with `slots` page slots, write-back until told otherwise. */
static struct shared_pages *shared_new(size_t bytes,uint8_t kind,size_t slots,struct vnode **out) {
    if(slots>(SIZE_MAX-sizeof(struct shared_pages))/sizeof(uint64_t))return NULL;
    struct vnode *v=kzalloc(sizeof *v);if(!v)return NULL;
    struct shared_pages *p=kzalloc(sizeof *p+slots*sizeof(uint64_t));
    if(!p){kfree(v);return NULL;}
    p->kind=kind;p->cache=N_CACHE_WRITE_BACK;
    v->type=VT_SHARED;v->size=bytes;v->unlinked=true;v->ops=&shared_ops;v->priv=p;
    *out=v;return p;
}
static int64_t shared_open(struct vnode *v,int flags,struct file **out) {
    struct file *f=vfs_open_vnode(v,flags);
    if(!f){shared_release(v);return -N_ERR_NO_MEMORY;}
    *out=f;return 0;
}
/* A file over a new shared object of `bytes` (page aligned). */
static int64_t shared_object(size_t bytes,bool demand,int flags,struct file **out) {
    if(!bytes || bytes>SIZE_MAX-(PAGE_SIZE-1))return -N_ERR_INVALID;
    bytes=ALIGN_UP(bytes,PAGE_SIZE);size_t count=bytes/PAGE_SIZE;
    size_t slots=demand?(count+SHARED_LEAF-1)/SHARED_LEAF:count;
    if(!demand && count>pmm_free_pages())return -N_ERR_NO_MEMORY;
    struct vnode *v;struct shared_pages *p=shared_new(bytes,demand?SHARED_DEMAND:SHARED_RAM,slots,&v);
    if(!p)return -N_ERR_NO_MEMORY;
    if(demand)p->count=count;
    else for(;p->count<count;p->count++){
        p->pages[p->count]=pmm_alloc_zeroed();
        if(!p->pages[p->count]){shared_release(v);return -N_ERR_NO_MEMORY;}
    }
    return shared_open(v,flags,out);
}
static int64_t new_handle(int64_t r,struct file *f) {
    if(r<0)return r;
    int fd=allocfd(f);if(fd<0)vfs_close(f);return fd;
}
static int64_t sys_shared_create(size_t bytes,bool demand) {
    struct file *f=NULL;
    int64_t r=shared_object(bytes,demand,O_RDWR,&f);
    return new_handle(r,f);
}
static uint64_t cache_pte(unsigned cache) {
    if(cache==N_CACHE_WRITE_COMBINE && pat_supported())return PTE_PWT|PTE_PAT4K; /* PAT index 5 */
    if(cache==N_CACHE_WRITE_COMBINE || cache==N_CACHE_UNCACHED)return PTE_PCD|PTE_PWT; /* index 3 */
    return 0;
}

/* A claimed function's memory BAR: device memory, uncached unless a view says otherwise. */
static int64_t sys_pci_bar(int h,uint32_t bar) {
    struct file *pci=getfd_ref(h);
    if(!pci)return -N_ERR_NO_HANDLE;
    uint64_t address=0,size=0;int64_t r=pci_user_bar(pci,bar,&address,&size);
    bool wc_allowed=pci_user_bar_prefetchable(pci,bar);
    if(r>=0 && size>SIZE_MAX)r=-N_ERR_TOO_LARGE;
    struct vnode *v;struct shared_pages *p=r<0?NULL:shared_new(size,SHARED_RANGE,0,&v);
    if(p)p->claim=pci_user_hold(pci);
    vfs_close(pci);
    if(r<0)return r;
    if(!p)return -N_ERR_NO_MEMORY;
    p->base=address;p->count=size/PAGE_SIZE;p->cache=N_CACHE_UNCACHED;p->wc_allowed=wc_allowed;
    struct file *f=NULL;
    r=shared_open(v,O_RDWR,&f);
    return new_handle(r,f);
}
/* Zeroed RAM a claimed function may reach by DMA. A contiguous run is allocated whole; pages
   below 4 GiB come from the low end of memory, where the allocator's run search begins. */
static int64_t sys_dma_create(int h,size_t bytes,uint32_t flags) {
    struct file *pci=getfd_ref(h);
    if(!pci)return -N_ERR_NO_HANDLE;
    struct pci_claim *claim=pci_user_hold(pci);vfs_close(pci);
    if(!claim)return -N_ERR_WRONG_KIND;
    int64_t r=0;
    if(flags&~(N_DMA_CONTIGUOUS|N_DMA_32BIT) || !bytes || bytes>SIZE_MAX-(PAGE_SIZE-1))r=-N_ERR_INVALID;
    else if(ALIGN_UP(bytes,PAGE_SIZE)/PAGE_SIZE>pmm_free_pages())r=-N_ERR_NO_MEMORY;
    if(r<0){pci_user_drop(claim);return r;}
    bytes=ALIGN_UP(bytes,PAGE_SIZE);size_t count=bytes/PAGE_SIZE;
    uint64_t limit=flags&N_DMA_32BIT?0x100000000ull:UINT64_MAX;
    struct vnode *v;struct shared_pages *p;
    if(flags&N_DMA_CONTIGUOUS){
        uint64_t base=pmm_alloc_contig(count);
        if(!base){pci_user_drop(claim);return -N_ERR_NO_MEMORY;}
        if(base+bytes>limit || !(p=shared_new(bytes,SHARED_RANGE,0,&v))){
            for(size_t i=0;i<count;i++)pmm_free(base+i*PAGE_SIZE);
            pci_user_drop(claim);return -N_ERR_NO_MEMORY;
        }
        p->claim=claim;
        memset(phys_to_virt(base),0,bytes);
        p->base=base;p->count=count;p->owned=true;
    }else{
        if(!(p=shared_new(bytes,SHARED_RAM,count,&v))){pci_user_drop(claim);return -N_ERR_NO_MEMORY;}
        p->claim=claim;
        for(;p->count<count;p->count++){
            uint64_t frame=limit==UINT64_MAX?pmm_alloc():pmm_alloc_contig(1);
            if(frame && frame+PAGE_SIZE>limit){pmm_free(frame);frame=0;}
            if(!frame){shared_release(v);return -N_ERR_NO_MEMORY;}
            memset(phys_to_virt(frame),0,PAGE_SIZE);
            p->pages[p->count]=frame;
        }
    }
    p->dma=true;
    struct file *f=NULL;
    r=shared_open(v,O_RDWR,&f);
    return new_handle(r,f);
}
/* Bus addresses of a DMA object's (or a device range's) pages, also through views. */
static int64_t sys_dma_addresses(int h,size_t first,uint64_t *uout,size_t count) {
    struct file *f=getfd_ref(h);
    if(!f)return -N_ERR_NO_HANDLE;
    int64_t r=0;struct shared_pages *p=f->vn->priv;
    if(f->vn->type!=VT_SHARED)r=-N_ERR_WRONG_KIND;
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
    vfs_close(f);return r;
}
/* Part of a shared object as a new one with its own caching. Device memory only uncached or
   write-combining, and not after its claim ended; DMA RAM in its one type (the first other
   type asked while nothing maps it becomes that type); other objects only as they are. */
static int64_t sys_shared_view(int h,size_t offset,size_t bytes,uint32_t cache,uint32_t maximum) {
    struct file *f=getfd_ref(h);
    if(!f)return -N_ERR_NO_HANDLE;
    int64_t r=0;
    if(f->vn->type!=VT_SHARED)r=-N_ERR_WRONG_KIND;
    else if(!bytes || (offset|bytes)&(PAGE_SIZE-1) || offset>f->vn->size || bytes>f->vn->size-offset ||
            cache>N_CACHE_UNCACHED || (maximum && maximum!=N_MEMORY_READ && maximum!=N_MEMORY_READ_WRITE))r=-N_ERR_INVALID;
    if(r<0){vfs_close(f);return r;}
    struct shared_pages *src=f->vn->priv,*root=shared_root(src);
    if(shared_cancelled(src) || shared_device_gone(src,NULL)){vfs_close(f);return -N_ERR_CLOSED;}
    int access=f->flags&O_ACCMODE;
    if(maximum==N_MEMORY_READ_WRITE && access==O_RDONLY){vfs_close(f);return -N_ERR_DENIED;}
    if(maximum==N_MEMORY_READ)access=O_RDONLY;
    bool device=root->kind==SHARED_RANGE && !root->owned;
    if(root->kind==SHARED_COMPOSITE){
        /* A default view preserves each segment's type. An explicit single
           type must describe all selected pages; it cannot silently collapse
           heterogeneous BAR/RAM attributes into a different cache alias. */
        if(cache!=N_CACHE_DEFAULT)for(size_t at=offset/PAGE_SIZE,left=bytes/PAGE_SIZE;left;){
            size_t page=at,span;uint8_t type;
            shared_leaf(src,&page,&type,NULL,&span);
            if(cache!=type){vfs_close(f);return -N_ERR_DENIED;}
            span=MIN(span,left);at+=span;left-=span;
        }
    }else if(root->dma){
        if(cache==N_CACHE_DEFAULT)cache=root->cache;
        else if(cache!=root->cache){
            if(root->cache!=N_CACHE_WRITE_BACK || root->maps){vfs_close(f);return -N_ERR_DENIED;}
            if(!vmm_direct_type(root->kind==SHARED_RAM?root->pages:NULL,root->base,root->count,cache_pte(cache))){
                vfs_close(f);return -N_ERR_NO_MEMORY;
            }
            root->cache=(uint8_t)cache;
        }
    }else if(cache==N_CACHE_DEFAULT)cache=src->cache;
    else if(device?(cache==N_CACHE_WRITE_BACK || (cache==N_CACHE_WRITE_COMBINE && !root->wc_allowed)):cache!=src->cache){vfs_close(f);return -N_ERR_DENIED;}
    struct shared_scope *scope=kzalloc(sizeof *scope);
    if(!scope){vfs_close(f);return -N_ERR_NO_MEMORY;}
    scope->refs=1;scope->issuer=task_process()->pid;scope->parent=src->scope;
    if(scope->parent)scope->parent->refs++;
    struct vnode *v;struct shared_pages *p=shared_new(bytes,SHARED_VIEW,0,&v);
    if(!p){scope_drop(scope);vfs_close(f);return -N_ERR_NO_MEMORY;}
    p->scope=scope;
    p->count=bytes/PAGE_SIZE;p->first=offset/PAGE_SIZE;p->cache=(uint8_t)cache;p->parent=f; /* the view keeps our reference */
    if(src->kind==SHARED_VIEW){
        /* A view of a view points at the root object, so a chain of views (which any app can
           build, closing each parent) never grows the recursion in shared_frame and release. */
        p->parent=vfs_dup(src->parent);p->first+=src->first;vfs_close(f);
    }
    struct file *view=NULL;
    r=shared_open(v,access,&view);
    return new_handle(r,view);
}
static int64_t sys_shared_compose(const struct n_shared_range *uranges,size_t count,uint32_t access) {
    if(!count || (access!=N_MEMORY_READ && access!=N_MEMORY_READ_WRITE))return -N_ERR_INVALID;
    struct compose_input {struct file *file;size_t first,count;};
    if(count>SIZE_MAX/sizeof(struct compose_input) || count>SIZE_MAX/sizeof(*uranges))return -N_ERR_TOO_LARGE;
    if(count>(UINTPTR_MAX-(uintptr_t)uranges)/sizeof(*uranges))return -N_ERR_BAD_ADDRESS;
    struct compose_input *inputs=kzalloc(count*sizeof *inputs);
    if(!inputs)return -N_ERR_NO_MEMORY;
    int64_t r=0;size_t bytes=0,parts=0,dependencies=0;
    struct vnode *v=NULL;struct shared_pages *p=NULL;
    for(size_t i=0;i<count;i++){
        struct n_shared_range range;
        if((r=copy_from_user(&range,&uranges[i],sizeof range))<0)goto out;
        if(range.reserved || !range.bytes || (range.offset|range.bytes)&(PAGE_SIZE-1)){
            r=-N_ERR_INVALID;goto out;
        }
        struct file *f=inputs[i].file=getfd_ref(range.shared);
        if(!f){r=-N_ERR_NO_HANDLE;goto out;}
        if(f->vn->type!=VT_SHARED){r=-N_ERR_WRONG_KIND;goto out;}
        if(range.offset>f->vn->size || range.bytes>f->vn->size-range.offset){r=-N_ERR_INVALID;goto out;}
        if(access==N_MEMORY_READ_WRITE && (f->flags&O_ACCMODE)==O_RDONLY){r=-N_ERR_DENIED;goto out;}
        struct shared_pages *src=f->vn->priv,*root=shared_root(src);
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
    p=shared_new(bytes,SHARED_COMPOSITE,0,&v);
    if(!p){r=-N_ERR_NO_MEMORY;goto out;}
    p->count=bytes/PAGE_SIZE;
    p->parts=kmalloc(parts*sizeof *p->parts);
    if(dependencies)p->dependencies=kmalloc(dependencies*sizeof *p->dependencies);
    p->scope=kzalloc(sizeof *p->scope);
    if(p->scope){p->scope->refs=1;p->scope->issuer=task_process()->pid;}
    if(!p->parts || (dependencies && !p->dependencies) || !p->scope){r=-N_ERR_NO_MEMORY;goto out;}
    size_t end=0;
    for(size_t i=0;i<count;i++){
        struct shared_pages *src=inputs[i].file->vn->priv,*root=shared_root(src);
        if(src->scope){src->scope->refs++;p->dependencies[p->dependency_count++]=src->scope;}
        for(size_t j=0;j<root->dependency_count;j++){
            root->dependencies[j]->refs++;p->dependencies[p->dependency_count++]=root->dependencies[j];
        }
        for(size_t at=inputs[i].first,left=inputs[i].count;left;){
            size_t page=at,span;uint8_t cache;struct file *backing=inputs[i].file;
            shared_leaf(src,&page,&cache,&backing,&span);
            span=MIN(span,left);end+=span;
            p->parts[p->part_count++]=(struct shared_part){.file=vfs_dup(backing),.first=page,.end=end,.cache=cache};
            at+=span;left-=span;
        }
    }
    if(shared_cancelled(p) || shared_device_gone(p,NULL)){r=-N_ERR_CLOSED;goto out;}
    {
        struct file *composite=NULL;
        r=shared_open(v,access==N_MEMORY_READ?O_RDONLY:O_RDWR,&composite);
        v=NULL;p=NULL; /* shared_open consumes the vnode even on failure */
        r=new_handle(r,composite);
    }
out:
    if(v)shared_release(v);
    for(size_t i=0;i<count;i++)if(inputs[i].file)vfs_close(inputs[i].file);
    kfree(inputs);return r;
}
static int64_t sys_shared_revoke(int h) {
    struct file *f=getfd_ref(h);
    if(!f)return -N_ERR_NO_HANDLE;
    if(f->vn->type!=VT_SHARED){vfs_close(f);return -N_ERR_WRONG_KIND;}
    struct shared_scope *scope=((struct shared_pages *)f->vn->priv)->scope;
    if(!scope || scope->issuer!=task_process()->pid){vfs_close(f);return -N_ERR_DENIED;}
    /* Revoked is published before any PTE removal, including SMP shootdown.
       A concurrent fault cannot restore a page while cancellation runs. */
    scope->revoked=true;
    shared_cancel_builds(scope,NULL);
    for(struct task *t=task_list;t;t=t->all_next){
        if(t->is_thread || !t->pml4 || t->pml4==kernel_pml4)continue;
        for(struct shared_mapping *m=t->shared_mappings;m;m=m->next)
            if(shared_depends(m->file->vn->priv,scope))
                vmm_user_free(t->pml4,m->va,m->bytes);
    }
    vfs_close(f);return 0;
}
/* Read-only ELF segments: every task running the same program bytes maps one
 * set of frames, so concurrent instances (a worker pool) hold one copy of their
 * code and constants. A spawn compares its file bytes with the cached frames
 * and makes a new object when the program file changed. The cache's own
 * reference is dropped once no task maps an object. */
struct program_segment {struct program_segment *next;struct file *file;uint64_t va,filesz,memsz;bool exec;char path[];};
static struct program_segment *program_segments;
/* Unlinks the objects only the cache still references; the caller closes them. */
static struct program_segment *program_segments_unused(void) {
    struct program_segment *unused=NULL;uint64_t irq=irq_save();
    for(struct program_segment **link=&program_segments;*link;){
        struct program_segment *s=*link;
        if(s->file->refs==1){*link=s->next;s->next=unused;unused=s;}else link=&s->next;
    }
    irq_restore(irq);return unused;
}
static void program_segments_free(struct program_segment *s) {
    while(s){struct program_segment *n=s->next;vfs_close(s->file);kfree(s);s=n;}
}
static bool program_segment_matches(struct file *f,uint64_t offset,const uint8_t *bytes,uint64_t filesz) {
    struct shared_pages *p=f->vn->priv;
    for(uint64_t at=0;at<filesz;){
        uint64_t page=(offset+at)/PAGE_SIZE,within=(offset+at)%PAGE_SIZE,chunk=MIN(filesz-at,PAGE_SIZE-within);
        if(memcmp((uint8_t *)phys_to_virt(p->pages[page])+within,bytes+at,chunk))return false;
        at+=chunk;
    }
    return true;
}
int shared_program_segment(struct task *t,const char *path,uint64_t va,const uint8_t *bytes,
                           uint64_t filesz,uint64_t memsz,bool exec) {
    uint64_t start=ALIGN_DOWN(va,PAGE_SIZE),end=ALIGN_UP(va+memsz,PAGE_SIZE),offset=va-start;
    size_t path_len=strlen(path);struct file *f=NULL;
    program_segments_free(program_segments_unused());
    uint64_t irq=irq_save();
    for(struct program_segment *s=program_segments;s;s=s->next)
        if(s->va==va && s->filesz==filesz && s->memsz==memsz && s->exec==exec && !strcmp(s->path,path)){f=vfs_dup(s->file);break;}
    irq_restore(irq);
    if(f && !program_segment_matches(f,offset,bytes,filesz)){vfs_close(f);f=NULL;}
    if(!f){
        int64_t r=shared_object(end-start,false,O_RDONLY,&f);
        if(r<0)return (int)r;
        struct shared_pages *p=f->vn->priv;
        for(uint64_t at=0;at<filesz;){
            uint64_t page=(offset+at)/PAGE_SIZE,within=(offset+at)%PAGE_SIZE,chunk=MIN(filesz-at,PAGE_SIZE-within);
            memcpy((uint8_t *)phys_to_virt(p->pages[page])+within,bytes+at,chunk);at+=chunk;
        }
        struct program_segment *s=kmalloc(sizeof *s+path_len+1);
        if(s){
            *s=(struct program_segment){.file=vfs_dup(f),.va=va,.filesz=filesz,.memsz=memsz,.exec=exec};
            memcpy(s->path,path,path_len+1);
            /* An object for older bytes of this program stays with its tasks only. */
            struct program_segment *stale=NULL;irq=irq_save();
            for(struct program_segment **link=&program_segments;*link;link=&(*link)->next)
                if((*link)->va==va && !strcmp((*link)->path,path)){stale=*link;*link=stale->next;stale->next=NULL;break;}
            s->next=program_segments;program_segments=s;irq_restore(irq);
            program_segments_free(stale);
        }
    }
    struct shared_mapping *m=kzalloc(sizeof *m);
    if(!m){vfs_close(f);return -N_ERR_NO_MEMORY;}
    struct shared_pages *p=f->vn->priv;
    uint64_t flags=PTE_U|PTE_SHARED|PTE_SHARED_RO|(exec?0:pte_nx);size_t mapped=0;
    for(;mapped<p->count;mapped++)if(!vmm_map_page(t->pml4,start+mapped*PAGE_SIZE,p->pages[mapped],flags))break;
    if(mapped!=p->count){vmm_user_free(t->pml4,start,mapped*PAGE_SIZE);vfs_close(f);kfree(m);return -N_ERR_NO_MEMORY;}
    irq=irq_save();*m=(struct shared_mapping){.next=t->shared_mappings,.file=f,.va=start,.flags=flags,.bytes=end-start};
    t->shared_mappings=m;irq_restore(irq);
    return 0;
}
static uint64_t gone_device_page; /* shared_demand_fault: what revoked device mappings show */
void shared_revoke_claim(const struct pci_claim *c) {
    shared_cancel_builds(NULL,c);
    for(struct task *t=task_list;t;t=t->all_next){
        if(t->is_thread || !t->pml4 || t->pml4==kernel_pml4)continue;
        for(struct shared_mapping *m=t->shared_mappings;m;m=m->next){
            if(shared_device_gone(m->file->vn->priv,c))vmm_user_free(t->pml4,m->va,m->bytes);
        }
    }
}
/* A first touch of a reserved shared page, from user code or a kernel copy
 * on its behalf: allocate the object's frame (or adopt the one a peer already
 * created) and map it with the mapping's own protection. */
bool shared_demand_fault(uint64_t addr,bool write) {
    struct task *owner=task_process();
    if(!owner)return false;
    int reserved=vmm_demand_fault(owner->pml4,addr,write); /* a private reserved page */
    if(reserved>0)return true;
    if(reserved<0){
        kprintf("[memory] pid %d (%s): no frame for reserved page %p, %lu pages free\n",
                owner->pid,owner->name,(void *)ALIGN_DOWN(addr,PAGE_SIZE),(unsigned long)pmm_free_pages());
        report_memory_exhaustion(owner,PAGE_SIZE);
        return false;
    }
    uint64_t page=ALIGN_DOWN(addr,PAGE_SIZE),irq=irq_save();bool ok=false,starved=false;
    for(struct shared_mapping *m=owner->shared_mappings;m;m=m->next){
        if(page<m->va || page-m->va>=m->bytes)continue;
        struct shared_pages *p=m->file->vn->priv;
        if(shared_cancelled(p))break;
        size_t index=(page-m->va)/PAGE_SIZE;uint8_t cache;
        struct shared_pages *leaf=shared_leaf(p,&index,&cache,NULL,NULL);
        if(leaf->kind==SHARED_RANGE && !leaf->owned && pci_user_revoked(leaf->claim)){
            /* The device's driver ended. One kernel page, all-ones at first, stands in for every
               register and aperture page: reads see a function gone from the bus, writes (a
               doorbell) land nowhere that matters. */
            if(write && !(m->flags&PTE_W))break;
            if(!gone_device_page && (gone_device_page=pmm_alloc()))memset(phys_to_virt(gone_device_page),0xFF,PAGE_SIZE);
            starved=!gone_device_page;
            ok=gone_device_page && vmm_map_page(owner->pml4,page,gone_device_page,
                                                m->flags&~(PTE_PWT|PTE_PCD|PTE_PAT4K));
            break;
        }
        /* Claim loss removes the entire composite mapping. Its other RAM or
           live-device segments still refer to their own valid backing. */
        if((leaf->kind!=SHARED_DEMAND && !shared_device_gone(p,NULL)) || (write && !(m->flags&PTE_W)))break;
        if(vmm_get_pte(owner->pml4,page)&PTE_P){ok=true;break;}
        uint64_t frame=shared_frame(p,(page-m->va)/PAGE_SIZE,true);
        starved=!frame;
        ok=frame && vmm_map_page(owner->pml4,page,frame,m->flags|cache_pte(cache));
        break;
    }
    irq_restore(irq);
    if(starved){
        kprintf("[shared] pid %d (%s): no frame for page %p, %lu pages free\n",
                owner->pid,owner->name,(void *)page,(unsigned long)pmm_free_pages());
        report_memory_exhaustion(owner,PAGE_SIZE);
    }
    return ok;
}
/* Maps a shared object the caller holds a handle to (its own, or one received
   as a channel attachment): there is no reaching into another app's handles. */
static int64_t sys_shared_map(int h,size_t bytes,int access) {
    if(!bytes || bytes>SIZE_MAX-(PAGE_SIZE-1) || (access!=N_MEMORY_READ && access!=N_MEMORY_READ_WRITE))return -N_ERR_INVALID;
    struct task *owner=task_process();struct file *f=getfd_ref(h);
    if(!f)return -N_ERR_NO_HANDLE;
    if(f->vn->type!=VT_SHARED){vfs_close(f);return -N_ERR_WRONG_KIND;}
    /* the handle's own rights bound the mapping's: a read-only handle maps read-only */
    if(access==N_MEMORY_READ_WRITE && (f->flags&O_ACCMODE)==O_RDONLY){vfs_close(f);return -N_ERR_DENIED;}
    struct shared_pages *p=f->vn->priv;
    if(shared_cancelled(p) || shared_device_gone(p,NULL)){vfs_close(f);return -N_ERR_CLOSED;}
    uint64_t irq;
    bytes=ALIGN_UP(bytes,PAGE_SIZE);
    if(bytes!=f->vn->size){vfs_close(f);return -N_ERR_INVALID;}
    struct shared_mapping *m=kzalloc(sizeof *m);
    if(!m){vfs_close(f);return -N_ERR_NO_MEMORY;}
    /* Reserve the VA range (reusing released ones) before publishing any PTE, so
       no other reservation can choose this partially mapped span. */
    irq=irq_save();
    uint64_t va=mmap_reserve(owner,bytes);
    if(!va){irq_restore(irq);vfs_close(f);kfree(m);return -N_ERR_NO_MEMORY;}
    size_t mapped=0;
    uint64_t flags=PTE_U|PTE_SHARED|pte_nx|(access==N_MEMORY_READ_WRITE?PTE_W:PTE_SHARED_RO);
    /* Pin DMA cache types before installing the first PTE, including every
       selected composite segment; rollback drops the same references. */
    shared_dma_maps(p,true);
    *m=(struct shared_mapping){.next=shared_building,.file=f,.va=va,.flags=flags,.bytes=bytes,
                               .building_owner=owner,.cancelled=shared_cancelled(p) || shared_device_gone(p,NULL)};
    shared_building=m;irq_restore(irq);
    /* Demand pages not yet touched by any peer stay unmapped until a fault. */
    for(;mapped<p->count;mapped++){
        size_t index=mapped;uint8_t cache;
        irq=irq_save();
        if(m->cancelled || current_task->killed){irq_restore(irq);break;}
        shared_leaf(p,&index,&cache,NULL,NULL);
        uint64_t frame=shared_frame(p,mapped,false);
        bool ok=!frame || vmm_map_page(owner->pml4,va+mapped*PAGE_SIZE,frame,flags|cache_pte(cache));
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
        irq=irq_save();shared_build_remove(m);shared_dma_maps(p,false);
        m->revoking=false;wq_wake_all(&m->build_wait);irq_restore(irq);
        mmap_hole_add(owner,va,bytes+PAGE_SIZE);
        vfs_close(f);kfree(m);return result;
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
    uint64_t irq=irq_save();shared_dma_maps(m->file->vn->priv,false);irq_restore(irq);
    vfs_close(m->file);kfree(m);
}
void sys_shared_cleanup(struct task *owner) {
    while(owner->shared_mappings){struct shared_mapping *m=owner->shared_mappings;
        owner->shared_mappings=m->next;
        shared_unmap(owner,m);
    }
    mmap_holes_free(owner);
    program_segments_free(program_segments_unused());
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
    if (len == 0 || len > (1ULL << 32) || vp < 0) return -N_ERR_INVALID;
    len = ALIGN_UP(len, PAGE_SIZE);
    if (len / PAGE_SIZE > pmm_free_pages()) { report_memory_exhaustion(t, len); return -N_ERR_NO_MEMORY; }
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

static int64_t sys_memory_protect(uint64_t va, size_t len, int access) {
    int vp = access_flags(access);
    if (vp < 0 || va & (PAGE_SIZE - 1) || va >= USER_TOP || va + len > USER_TOP || va + len < va) return -N_ERR_INVALID;
    if(shared_build_overlap(task_process(),va,len))return -N_ERR_NOT_READY;
    return vmm_user_protect(current_task->pml4, va, len, vp);
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

/* Paths are complete: there is no working directory to resolve against. */
static int native_path(const char *upath, char *out) {
    char tmp[PATH_MAX_LEN];
    int r = user_str(tmp, upath, sizeof tmp);
    if (r < 0) return r;
    if (tmp[0] != '/') return -N_ERR_INVALID;
    return vfs_normalize("/", tmp, out);
}

/* Publish a new handle; on failure the caller's reference is dropped. */
static int64_t install(struct file *f) {
    int h = allocfd(f);
    if (h < 0) vfs_close(f);
    return h;
}

/* Copy user bytes in bounded pieces: no long non-preemptible copy, and a
   sibling unmapping the source meanwhile is reported, not faulted on. */
static int copy_from_user(void *dst, const void *usrc, size_t n) {
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

static int copy_to_user(void *udst, const void *src, size_t n) {
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
    struct file *f = getfd_ref(h);
    if (!f) return -N_ERR_NO_HANDLE;
    /* Only an explicit close commits a replacement: an app that ends with the
       handle open leaves the old document as it was. */
    int committed = vfs_document_commit(f);
    vfs_close(f);
    int closed = (int)handle_drop(h);
    return committed < 0 ? committed : closed;
}

static int64_t sys_handle_kind(int h) {
    uint64_t flags = irq_save();
    struct file *f = getfd(h);
    int kind = f ? vfs_kind(f->vn) : -N_ERR_NO_HANDLE;
    irq_restore(flags);
    return kind;
}

static int64_t sys_document_open(const char *upath, int access) {
    char path[PATH_MAX_LEN];
    int r = native_path(upath, path);
    if (r < 0) return r;
    struct file *f;
    r = vfs_document_open(path, access, &f);
    return r < 0 ? r : install(f);
}

static int64_t sys_document_io(int h, uint64_t off, void *buf, size_t n, bool write) {
    struct file *f = getfd_ref(h);
    if (!f) return -N_ERR_NO_HANDLE;
    int64_t r = write ? vfs_document_write_user(f, off, buf, n) : vfs_document_read_user(f, off, buf, n);
    vfs_close(f);
    return r;
}

static struct file *document_ref(int h, int64_t *error) {
    struct file *f = getfd_ref(h);
    *error = !f ? -N_ERR_NO_HANDLE : f->vn->type != VT_FILE ? -N_ERR_WRONG_KIND : 0;
    if (*error && f) { vfs_close(f); f = NULL; }
    return f;
}

static int64_t sys_document_size(int h) {
    int64_t r;
    struct file *f = document_ref(h, &r);
    if (!f) return r;
    r = (int64_t)f->vn->size;
    vfs_close(f);
    return r;
}

static int64_t sys_document_set_size(int h, uint64_t size) {
    int64_t r;
    struct file *f = document_ref(h, &r);
    if (!f) return r;
    r = (f->flags & O_ACCMODE) == O_RDONLY ? -N_ERR_DENIED : vfs_truncate(f, size);
    vfs_close(f);
    return r;
}

static int64_t sys_document_claim(int h, bool claim, bool wait) {
    int64_t r;
    struct file *f = document_ref(h, &r);
    if (!f) return r;
    r = !claim ? vfs_lease(f, false) : wait ? vfs_lease(f, true) : vfs_try_lease(f);
    vfs_close(f);
    return r;
}

static int64_t sys_document_save(const char *upath, const void *udata, size_t n) {
    char path[PATH_MAX_LEN];
    int r = native_path(upath, path);
    if (r < 0) return r;
    uint8_t *data = n ? kmalloc(n) : NULL;
    if (n && !data) return -N_ERR_NO_MEMORY;
    r = copy_from_user(data, udata, n);
    if (r < 0) { kfree(data); return r; }
    return vfs_replace(path, data, n);
}

static int64_t sys_document_append(const char *upath, const void *udata, size_t n) {
    char path[PATH_MAX_LEN];
    int r = native_path(upath, path);
    return r < 0 ? r : vfs_append_user(path, udata, n);
}

static void entry_from(struct n_entry *e, const struct dirent *d) {
    memset(e, 0, sizeof *e);
    strlcpy(e->name, d->name, sizeof e->name);
    e->kind = d->type == VT_DIR ? N_KIND_FOLDER : d->type == VT_FILE ? N_KIND_DOCUMENT : N_KIND_DEVICE;
    e->size = d->size;
    e->modified = d->mtime;
}

static int64_t sys_entry_info(const char *upath, struct n_entry *uout) {
    char path[PATH_MAX_LEN];
    int r = native_path(upath, path);
    if (r < 0) return r;
    struct dirent d;
    memset(&d, 0, sizeof d);
    r = vfs_entry_info(path, &d);
    if (r < 0) return r;
    struct n_entry e;
    entry_from(&e, &d);
    return copy_to_user(uout, &e, sizeof e);
}

static int64_t sys_folder_list(const char *upath, struct n_entry *uout, size_t max, uint64_t start) {
    char path[PATH_MAX_LEN];
    int r = native_path(upath, path);
    if (r < 0) return r;
    if (max > SIZE_MAX / sizeof *uout) return -N_ERR_INVALID;
    enum { BATCH = 16 };
    struct dirent *batch = kmalloc(BATCH * sizeof *batch);
    struct n_entry *entries = kmalloc(BATCH * sizeof *entries);
    int64_t done = 0;
    if (!batch || !entries) done = -N_ERR_NO_MEMORY;
    while (done >= 0 && (size_t)done < max) {
        size_t want = MIN(max - (size_t)done, (size_t)BATCH);
        memset(batch, 0, want * sizeof *batch);
        int n = vfs_folder_list(path, start + (uint64_t)done, batch, want);
        if (n < 0) { if (!done) done = n; break; }
        for (int i = 0; i < n; i++) entry_from(&entries[i], &batch[i]);
        if (copy_to_user(uout + done, entries, (size_t)n * sizeof *entries) < 0) { done = -N_ERR_BAD_ADDRESS; break; }
        done += n;
        if ((size_t)n < want) break;
    }
    kfree(batch);
    kfree(entries);
    return done;
}

static int64_t sys_entry_remove(const char *upath) {
    char path[PATH_MAX_LEN];
    int r = native_path(upath, path);
    return r < 0 ? r : vfs_unlink(path);
}

static int64_t sys_entry_move(const char *ufrom, const char *uto) {
    char from[PATH_MAX_LEN], to[PATH_MAX_LEN];
    int r = native_path(ufrom, from);
    if (r < 0) return r;
    r = native_path(uto, to);
    return r < 0 ? r : vfs_move(from, to);
}

static int64_t sys_folder_create(const char *upath) {
    char path[PATH_MAX_LEN];
    int r = native_path(upath, path);
    return r < 0 ? r : vfs_mkdir(path);
}

static int64_t sys_channel_pair(int32_t *uout) {
    if (!user_ok_w(uout, 2 * sizeof *uout)) return -N_ERR_BAD_ADDRESS;
    struct file *a, *b;
    int e = message_channel_pair(&a, &b);
    if (e < 0) return e;
    uint64_t flags = irq_save();
    int ha = allocfd_locked(a), hb = ha < 0 ? ha : allocfd_locked(b);
    int error = hb < 0 ? hb : !user_ok_w(uout, 2 * sizeof *uout) ? -N_ERR_BAD_ADDRESS : 0;
    if (error) {
        if (hb >= 0) task_process()->fds[hb] = NULL;
        if (ha >= 0) { task_process()->fds[ha] = NULL; task_process()->fd_hint = (size_t)ha; }
        irq_restore(flags);
        vfs_close(a);
        vfs_close(b);
        return error;
    }
    uout[0] = ha;
    uout[1] = hb;
    irq_restore(flags);
    return 0;
}

/* Byte pipes, byte channels and the character devices given to apps as their
   input, output and log (console, kernel log, null) remain until the old ABI
   is removed (stage 5): on them a send is a write and a receive takes what is
   there, without message boundaries or attachments. */
static bool byte_channel(struct file *f) {
    return f->vn->type == VT_CHAR ||
           (f->vn->type == VT_CHANNEL && !message_channel_is(f));
}

static int64_t sys_channel_send(int h, const void *udata, size_t n, int attach) {
    struct file *f = getfd_ref(h);
    if (!f) return -N_ERR_NO_HANDLE;
    struct file *att = NULL;
    int64_t r = 0;
    if (attach >= 0) {
        att = getfd_ref(attach);
        if (!att) r = -N_ERR_NO_HANDLE;
        /* documents and shared pages only: a channel end sent through its own
           channel would keep itself alive */
        else if (att->vn->type != VT_FILE && att->vn->type != VT_SHARED) r = -N_ERR_WRONG_KIND;
    }
    if (r < 0) {
    } else if (message_channel_is(f)) {
        r = message_channel_send(f, udata, n, att);
    } else if (byte_channel(f)) {
        r = att ? -N_ERR_UNSUPPORTED : vfs_write_user(f, udata, n);
    } else {
        r = -N_ERR_WRONG_KIND;
    }
    if (att) vfs_close(att);
    vfs_close(f);
    return r;
}

static int64_t sys_channel_receive(int h, void *ubuf, size_t cap, int flags, void *uoutput, bool credentials) {
    size_t output_size=credentials?sizeof(struct n_channel_message):sizeof(int32_t);
    if ((credentials && !uoutput) || (uoutput && !user_ok_w(uoutput,output_size))) return -N_ERR_BAD_ADDRESS;
    struct file *f = getfd_ref(h);
    if (!f) return -N_ERR_NO_HANDLE;
    bool wait = flags & N_CHANNEL_WAIT, peek = flags & N_CHANNEL_PEEK, discard = flags & N_CHANNEL_DISCARD;
    if (flags & ~(N_CHANNEL_WAIT | N_CHANNEL_PEEK | N_CHANNEL_DISCARD) || (peek && discard)) { vfs_close(f); return -N_ERR_INVALID; }
    int64_t r;
    struct file *att = NULL;
    struct n_channel_message message={.attached=-1};
    if (message_channel_is(f)) {
        r = message_channel_receive(f, ubuf, cap, wait, peek, discard, &att, &message.sender_app);
    } else if (byte_channel(f)) {
        if (credentials || peek || discard) r = -N_ERR_UNSUPPORTED;
        else if (!cap) r = 0;
        else {
            uint64_t irq = irq_save();
            bool ready = vfs_can_read(f);
            irq_restore(irq);
            r = !ready && !wait ? -N_ERR_NOT_READY : vfs_read_user(f, ubuf, cap);
            if (r == 0) r = -N_ERR_CLOSED; /* the writer has gone */
        }
    } else {
        r = -N_ERR_WRONG_KIND;
    }
    vfs_close(f);
    if (r >= 0 && uoutput && (credentials || !peek)) {
        if (att && !peek) {
            message.attached=(int32_t)install(att);att=NULL;
            if (message.attached<0) r=message.attached;
        }
        if (r>=0 && copy_to_user(uoutput,credentials?(const void *)&message:(const void *)&message.attached,output_size)<0) {
            if (message.attached>=0) handle_drop(message.attached);
            r=-N_ERR_BAD_ADDRESS;
        }
    }
    if (att) vfs_close(att); /* the caller did not ask for attachments */
    return r;
}

/* A user string of any length up to max, copied into the kernel. */
static char *user_strdup(const char *u, char *scratch, size_t max, int *error) {
    int n = user_str(scratch, u, max);
    if (n < 0) { *error = n; return NULL; }
    char *s = strdup(scratch);
    if (!s) *error = -N_ERR_NO_MEMORY;
    return s;
}

static int64_t sys_app_launch(const char *upath, const struct n_launch *ul) {
    char path[PATH_MAX_LEN];
    int r = native_path(upath, path);
    if (r < 0) return r;
    struct n_launch l;
    r = copy_from_user(&l, ul, sizeof l);
    if (r < 0) return r;
    if (l.flags) return -N_ERR_INVALID;
    const size_t limit = USER_STACK_MAX / 2; /* what the new app's stack can hold */
    if (l.argument_count > limit / 8 || l.setting_count > limit / 8 || l.handle_count > 4096)
        return -N_ERR_TOO_MANY_ARGUMENTS;
    size_t argc = l.argument_count ? l.argument_count : 1;
    char **argv = kzalloc(argc * sizeof *argv);
    char **settings = kzalloc((l.setting_count + 1) * sizeof *settings);
    struct file **handles = kzalloc((l.handle_count + 1) * sizeof *handles);
    char *scratch = kmalloc(65536);
    struct file *app_file = NULL;
    int64_t ret = 0;
    size_t bytes = 0;
    if (!argv || !settings || !handles || !scratch) { ret = -N_ERR_NO_MEMORY; goto out; }
    if (!l.argument_count) {
        argv[0] = strdup(path);
        if (!argv[0]) { ret = -N_ERR_NO_MEMORY; goto out; }
    }
    for (size_t i = 0; i < l.argument_count; i++) {
        const char *ua;
        if ((r = copy_from_user(&ua, &l.arguments[i], sizeof ua)) < 0) { ret = r; goto out; }
        if (!(argv[i] = user_strdup(ua, scratch, 65536, &r))) { ret = r; goto out; }
        bytes += strlen(argv[i]) + 1;
    }
    for (size_t i = 0; i < l.setting_count; i++) {
        const char *us;
        if ((r = copy_from_user(&us, &l.settings[i], sizeof us)) < 0) { ret = r; goto out; }
        if (!(settings[i] = user_strdup(us, scratch, 65536, &r))) { ret = r; goto out; }
        bytes += strlen(settings[i]) + 1;
        if (!strchr(settings[i], '=')) { ret = -N_ERR_INVALID; goto out; }
    }
    if (bytes > limit) { ret = -N_ERR_TOO_MANY_ARGUMENTS; goto out; }
    for (size_t i = 0; i < l.handle_count; i++) {
        int32_t h;
        if ((r = copy_from_user(&h, &l.handles[i], sizeof h)) < 0) { ret = r; goto out; }
        if (h < 0) continue;
        if (!(handles[i] = getfd_ref(h))) { ret = -N_ERR_NO_HANDLE; goto out; }
    }
    struct vnode *app = proc_app_new();
    if (!app || !(app_file = vfs_open_vnode(app, O_RDONLY))) {
        if (app) app->ops->release(app);
        ret = -N_ERR_NO_MEMORY;
        goto out;
    }
    struct proc_launch launch = {.argc = (int)argc, .argv = argv, .setting_count = (int)l.setting_count,
                                 .settings = settings, .handles = handles, .handle_count = l.handle_count,
                                 .parent = task_process(), .app = app};
    r = proc_launch(path, &launch);
    if (r < 0) ret = r;
    else { ret = install(app_file); app_file = NULL; }
out:
    if (app_file) vfs_close(app_file);
    if (argv) for (size_t i = 0; i < argc; i++) kfree(argv[i]);
    if (settings) for (size_t i = 0; i < l.setting_count; i++) kfree(settings[i]);
    if (handles) for (size_t i = 0; i < l.handle_count; i++) if (handles[i]) vfs_close(handles[i]);
    kfree(argv);
    kfree(settings);
    kfree(handles);
    kfree(scratch);
    return ret;
}

static struct file *app_ref(int h, int64_t *error) {
    struct file *f = getfd_ref(h);
    *error = !f ? -N_ERR_NO_HANDLE : !proc_app_is(f->vn) ? -N_ERR_WRONG_KIND : 0;
    if (*error && f) { vfs_close(f); f = NULL; }
    return f;
}

static int64_t sys_app_result(int h, int64_t timeout_ms, int *ustatus) {
    int64_t r;
    struct file *f = app_ref(h, &r);
    if (!f) return r;
    int status = 0;
    r = proc_app_result(f->vn, timeout_ms < 0 ? UINT64_MAX : (uint64_t)timeout_ms, &status);
    vfs_close(f);
    if (!r && ustatus) r = copy_to_user(ustatus, &status, sizeof status);
    return r;
}

static int64_t sys_app_stop(int h, int flags) {
    int64_t r;
    struct file *f = app_ref(h, &r);
    if (!f) return r;
    int pid = proc_app_pid(f->vn);
    vfs_close(f);
    if (flags & ~N_APP_STOP_TREE) return -N_ERR_INVALID;
    if (!pid) return 0; /* it has already ended */
    if (flags & N_APP_STOP_TREE) stop_tree(pid, 1);
    else task_kill(pid);
    return 0;
}

static int64_t sys_app_open(int64_t id) {
    if (id <= 0 || id > 0x7fffffff) return -N_ERR_NO_APP;
    struct file *f;
    int r = proc_app_open((int)id, &f);
    return r < 0 ? r : install(f);
}

static int64_t sys_app_id(int h) {
    int64_t r;
    struct file *f = app_ref(h, &r);
    if (!f) return r;
    r = proc_app_pid(f->vn);
    vfs_close(f);
    return r;
}

/* A handle whose messages are added to the system log (and the serial port). */
static int64_t sys_system_log_open(void) {
    struct file *f;
    int r = vfs_open("/dev/kmsg", O_WRONLY, &f);
    return r < 0 ? r : install(f);
}

/* A stream of its own on the sound mixer; N_ERR_BUSY when every stream is taken. */
static int64_t sys_audio_open(void) {
    struct file *f;
    int r = audio_stream_open(&f);
    return r < 0 ? r : install(f);
}

static struct file *audio_ref(int h, int64_t *error) {
    struct file *f = getfd_ref(h);
    *error = !f ? -N_ERR_NO_HANDLE : !audio_stream_is(f) ? -N_ERR_WRONG_KIND : 0;
    if (*error && f) { vfs_close(f); f = NULL; }
    return f;
}

static int64_t sys_audio_submit(int h, const void *frames, uint64_t count, uint32_t flags) {
    if (flags & ~N_AUDIO_WAIT) return -N_ERR_INVALID;
    int64_t r;
    struct file *f = audio_ref(h, &r);
    if (!f) return r;
    r = audio_submit_file(f, frames, count, flags & N_AUDIO_WAIT);
    vfs_close(f);
    return r;
}

static int64_t sys_audio_status(int h, struct n_audio_status *out) {
    int64_t r;
    struct file *f = audio_ref(h, &r);
    if (!f) return r;
    r = audio_status_file(f, out);
    vfs_close(f);
    return r;
}

/* The master volume, 0 to 100; percent < 0 only reads it. */
static int64_t sys_audio_volume(int64_t percent) {
    if (percent > 100) return -N_ERR_INVALID;
    if (percent >= 0) audio_set_volume((int)percent);
    return audio_volume();
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

static struct file *events_ref(int q, int64_t *error) {
    struct file *f = getfd_ref(q);
    *error = !f ? -N_ERR_NO_HANDLE : !events_is(f) ? -N_ERR_WRONG_KIND : 0;
    if (*error && f) { vfs_close(f); f = NULL; }
    return f;
}

static int64_t sys_events_create(void) {
    struct vnode *v = events_new();
    if (!v) return -N_ERR_NO_MEMORY;
    struct file *f = vfs_open_vnode(v, O_RDONLY);
    if (!f) { v->ops->release(v); return -N_ERR_NO_MEMORY; }
    return install(f);
}

static int64_t sys_events_watch(int q, int h, uint32_t interest, uint64_t token) {
    const uint32_t known = N_EVENT_RECEIVABLE | N_EVENT_SENDABLE | N_EVENT_CLOSED | N_EVENT_APP_ENDED;
    if (!interest || (interest & ~known)) return -N_ERR_INVALID;
    int64_t r;
    struct file *queue = events_ref(q, &r);
    if (!queue) return r;
    struct event_watch *spare = kmalloc(events_watch_size());
    if (!spare) { vfs_close(queue); return -N_ERR_NO_MEMORY; }
    uint64_t flags = irq_save();
    struct file *f = getfd(h);
    r = f ? events_watch_locked(queue, f, interest, token, &spare) : -N_ERR_NO_HANDLE;
    irq_restore(flags);
    kfree(spare);
    vfs_close(queue);
    return r;
}

static int64_t sys_events_watch_address(int q, uint64_t address, uint32_t expected, uint64_t token) {
    if ((address & 3) || !user_ok((void *)address, sizeof(uint32_t))) return -N_ERR_BAD_ADDRESS;
    int64_t r;
    struct file *queue = events_ref(q, &r);
    if (!queue) return r;
    r = events_watch_address(queue, (volatile uint32_t *)address, expected, token);
    vfs_close(queue);
    return r;
}

static int64_t sys_events_timer(int q, uint64_t ms, int repeat, uint64_t token) {
    int64_t r;
    struct file *queue = events_ref(q, &r);
    if (!queue) return r;
    r = events_timer(queue, ms, repeat != 0, token);
    vfs_close(queue);
    return r;
}

static int64_t sys_events_unwatch(int q, uint64_t token) {
    int64_t r;
    struct file *queue = events_ref(q, &r);
    if (!queue) return r;
    r = events_unwatch(queue, token);
    vfs_close(queue);
    return r;
}

static int64_t sys_events_wait(int q, struct n_event *uout, size_t max, int64_t timeout_ms) {
    int64_t r;
    struct file *queue = events_ref(q, &r);
    if (!queue) return r;
    r = events_wait(queue, uout, max, timeout_ms);
    vfs_close(queue);
    return r;
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
    case SYS_AUDIO_FLUSH: {
        struct file *f = getfd_ref((int)a);
        ret = f ? audio_flush_file(f) : -N_ERR_NO_HANDLE;
        vfs_close(f);
        break;
    }
    case SYS_SLEEP: sleep_ms(a); ret = 0; break;
    case SYS_UPTIME: ret = uptime_ms(); break;
    case SYS_TIME: ret = time_now(); break;
    case SYS_SHARED_CREATE: ret = sys_shared_create(a,false); break;
    case SYS_SHARED_RESERVE: ret = sys_shared_create(a,true); break;
    case SYS_SHARED_MAP: ret = sys_shared_map((int)a,b,(int)c); break;
    case SYS_SHARED_VIEW: ret = sys_shared_view((int)a,b,c,(uint32_t)d,(uint32_t)e); break;
    case SYS_SHARED_COMPOSE: ret = sys_shared_compose((const struct n_shared_range *)a,b,(uint32_t)c); break;
    case SYS_SHARED_REVOKE: ret = sys_shared_revoke((int)a); break;
    case SYS_PCI_CLAIM: {
        struct file *f = NULL;
        ret = pci_user_claim((uint8_t)a,(uint8_t)b,(uint8_t)c,&f);
        ret = new_handle(ret,f);
        break;
    }
    case SYS_PCI_BAR: ret = sys_pci_bar((int)a,(uint32_t)b); break;
    case SYS_DMA_CREATE: ret = sys_dma_create((int)a,b,(uint32_t)c); break;
    case SYS_DMA_ADDRESSES: ret = sys_dma_addresses((int)a,b,(uint64_t *)c,d); break;
    case SYS_PCI_INFO: case SYS_PCI_CONFIG_READ: case SYS_PCI_CONFIG_WRITE:
    case SYS_PCI_INTERRUPTS: case SYS_PCI_INTERRUPT_TAKE: {
        struct file *f = getfd_ref((int)a);
        if (!f) { ret = -N_ERR_NO_HANDLE; break; }
        if (r->rax == SYS_PCI_INFO) {
            struct n_pci_function info;
            ret = pci_user_info(f, &info);
            if (!ret) ret = copy_to_user((void *)b, &info, sizeof info);
        } else if (r->rax == SYS_PCI_CONFIG_READ) ret = pci_user_config_read(f,(uint32_t)b,(uint32_t)c,(uint32_t)d);
        else if (r->rax == SYS_PCI_CONFIG_WRITE) ret = pci_user_config_write(f,(uint32_t)b,(uint32_t)c,(uint32_t)d);
        else if (r->rax == SYS_PCI_INTERRUPTS) ret = pci_user_interrupts(f,(uint32_t)b);
        else ret = pci_user_interrupt_take(f,(int64_t)b);
        vfs_close(f);
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
        if (!hv.present && tsc_hz >= 1000000 && (cd & (1u << 8))) {
            info.flags = N_CLOCK_USER_TSC;
            info.tsc_hz = tsc_hz;
        }
        uint64_t flags = irq_save();
        uint64_t reference_phys = hv_reference_tsc_page();
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
        info.tsc_sample = rdtsc();
        info.uptime_ms = uptime_ms();
        if (info.reference_page) hv_reference_time(&info.reference_sample_100ns);
        memcpy((void *)a, &info, sizeof info);
        irq_restore(flags);
        ret = 0;
        break;
    }
    case SYS_THREAD_CREATE:
        if (!user_ok((void *)a, 1) || !user_ok((void *)c, 1) ||
            (vmm_get_pte(current_task->pml4, a) & PTE_NX) ||
            (vmm_get_pte(current_task->pml4, c) & PTE_NX)) ret = -N_ERR_BAD_ADDRESS;
        else ret = task_thread_create(c, a, b);
        break;
    case SYS_THREAD_JOIN: ret = task_thread_join((int)a); break;
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
    case SYS_DOCUMENT_SAVE: ret = sys_document_save((const char *)a, (const void *)b, c); break;
    case SYS_DOCUMENT_APPEND: ret = sys_document_append((const char *)a, (const void *)b, c); break;
    case SYS_ENTRY_INFO: ret = sys_entry_info((const char *)a, (struct n_entry *)b); break;
    case SYS_ENTRY_REMOVE: ret = sys_entry_remove((const char *)a); break;
    case SYS_ENTRY_MOVE: ret = sys_entry_move((const char *)a, (const char *)b); break;
    case SYS_FOLDER_CREATE: ret = sys_folder_create((const char *)a); break;
    case SYS_FOLDER_LIST: ret = sys_folder_list((const char *)a, (struct n_entry *)b, c, d); break;
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
    case SYS_AUDIO_VOLUME: ret = sys_audio_volume((int64_t)a); break;
    case SYS_RANDOM: ret = sys_random((void *)a, b); break;
    default:
        if (r->rax >= SYS_WIN_CREATE && r->rax < SYS_NET_INFO) ret = wm_syscall((int)r->rax, a, b, c, d, e);
        else if (r->rax >= SYS_NET_INFO && r->rax < SYS_NET_LIMIT) ret = net_syscall((int)r->rax, a, b, c, d, e);
        break;
    }
    r->rax = (uint64_t)ret;
}
