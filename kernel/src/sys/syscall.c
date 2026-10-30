/* System call dispatch (SYSCALL, arch/isr.asm). */
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
#include "sys/watch.h"
#include "fs/store.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "dev/fb.h"
#include "dev/gpu.h"
#include "dev/audio.h"
#include "dev/entropy.h"
#include "dev/pci.h"
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

int pci_list(struct n_pciinfo *out, int max);
void vmm_user_pages_split(uint64_t pml4, uint64_t out[6]);
int64_t net_syscall(int num, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e);

/* ---- user memory access ---- */

static bool shared_fault(uint64_t addr, bool write, bool *slept);
size_t user_copy(void *dst, const void *src, size_t n); /* arch/isr.asm: the bytes it left */

/* Is [p, p+n) user memory the process may read (or, with write, also write)? Missing pages are
   made present (a stack that grows, a reserved or shared page), and a pager's page may be given
   write access. A program page read from its document lets the CPU go while the disk works: a
   sibling may unmap what was checked before it, so the check starts over. */
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
            if (!(pte & PTE_SHARED)) return false;
            if (!shared_fault(pg, true, &slept)) return false;
            if (slept) pg = ALIGN_DOWN(a, PAGE_SIZE);
            continue;
        }
        pg += PAGE_SIZE;
    }
    return true;
}

bool user_ok(const void *p, size_t n) { return user_access(p, n, false); }
bool user_ok_w(void *p, size_t n) { return user_access(p, n, true); }

/* The check, then the copy (user_copy), which stops short where a page went after the check, or,
   with interrupts off, where one is not there: the rest is checked and copied once more. */
static int user_transfer(void *dst, const void *src, size_t n, const void *user, bool write) {
    for (unsigned pass = 0; n; pass++) {
        if (pass == 2 || !user_access(user, n, write)) return -N_ERR_BAD_ADDRESS;
        size_t done = n - user_copy(dst, src, n);
        dst = (uint8_t *)dst + done;
        src = (const uint8_t *)src + done;
        user = (const uint8_t *)user + done;
        n -= done;
    }
    return 0;
}

int copy_from_user(void *dst, const void *usrc, size_t n) { return user_transfer(dst, usrc, n, usrc, false); }
int copy_to_user(void *udst, const void *src, size_t n) { return user_transfer(udst, src, n, udst, true); }

int user_str(char *dst, const char *src, size_t max) {
    if (!max) return -N_ERR_NAME_TOO_LONG;
    size_t i = 0;
    while (i < max) {
        uint64_t address = (uint64_t)src + i;
        if (address < (uint64_t)src) return -N_ERR_BAD_ADDRESS;
        /* Permission is page-granular: a piece never reaches into the page after the one that
           holds the terminator. */
        size_t piece = MIN(max - i, PAGE_SIZE - (address & (PAGE_SIZE - 1)));
        if (copy_from_user(dst + i, (const void *)address, piece) < 0) return -N_ERR_BAD_ADDRESS;
        for (size_t end = i + piece; i < end; i++)
            if (!dst[i]) return (int)i;
    }
    dst[max - 1] = 0;
    return -N_ERR_NAME_TOO_LONG;
}

int user_string(const char *u, char **out) {
    /* its length first, a page at a time, then a copy of that many bytes */
    char *page = kmalloc(PAGE_SIZE);
    if (!page) return -N_ERR_NO_MEMORY;
    size_t n = 0;
    int r = 0;
    for (;;) {
        uint64_t address = (uint64_t)u + n;
        size_t piece = PAGE_SIZE - (address & (PAGE_SIZE - 1)), i = 0;
        if (address < (uint64_t)u || copy_from_user(page, (const void *)address, piece) < 0) { r = -N_ERR_BAD_ADDRESS; break; }
        while (i < piece && page[i]) i++;
        n += i;
        if (i < piece) break;
        if (current_task->killed) { r = -N_ERR_STOPPED; break; }
    }
    kfree(page);
    if (r < 0) return r;
    char *s = kmalloc(n + 1);
    if (!s) return -N_ERR_NO_MEMORY;
    /* a sibling may change or unmap it meanwhile: the copy is checked again */
    r = copy_from_user(s, u, n);
    if (r < 0) { kfree(s); return r; }
    s[n] = 0;
    *out = s;
    return 0;
}


/* ---- handlers ---- */

/* Physical memory ran out under a memory request: name the largest user
   processes once per interval, so the owner of the memory is visible. */
static void report_memory_exhaustion(struct task *t, int64_t inc) {
    static uint64_t last_report; /* any CPU: the one that moves it on reports */
    uint64_t now = clock_now_ns(), last = __atomic_load_n(&last_report, __ATOMIC_RELAXED);
    if (last && now - last < 10 * NS_PER_S) return;
    if (!__atomic_compare_exchange_n(&last_report, &last, now, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    struct { int pid; char name[32]; uint64_t kb; } top[8];
    int n = 0;
    uint64_t flags = spin_lock(&task_lock);
    for (struct task *p = task_list; p; p = p->all_next) {
        if (!p->is_user || p->is_thread) continue;
        uint64_t kb = proc_user_pages(p) * 4;
        int at = n < 8 ? n++ : 8;
        while (at > 0 && top[at - 1].kb < kb) { if (at < 8) top[at] = top[at - 1]; at--; }
        if (at < 8) { top[at].pid = p->pid; strlcpy(top[at].name, p->name, sizeof top[at].name); top[at].kb = kb; }
    }
    spin_unlock(&task_lock, flags);
    kprintf("[memory] pid %d (%s): no pages for %ld more bytes, %lu pages free; largest:",
            t->pid, t->name, (long)inc, (unsigned long)pmm_free_pages());
    for (int i = 0; i < n; i++) kprintf(" %d/%s %luK", top[i].pid, top[i].name, (unsigned long)top[i].kb);
    kprintf("\n");
    /* Where the private pages sit, for every process: many middling ones can
       outweigh the largest. task_lock keeps each space: task_exit drops it under the lock. */
    flags = spin_lock(&task_lock);
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
    spin_unlock(&task_lock, flags);
}

/* The key is read once, its length given: the registry never reads the app's memory again. */
static int64_t sys_channel_named(const char *ukey,size_t length,bool listen) {
    if(!length)return -N_ERR_INVALID;
    char *key=kmalloc(length);if(!key)return -N_ERR_NO_MEMORY;
    struct handle *h=NULL;
    int result=copy_from_user(key,ukey,length);
    if(!result)result=listen?channel_listen(key,length,&h):channel_connect(key,length,&h);
    kfree(key);
    return result<0?result:handle_install(h);
}
static int64_t sys_channel_accept(int index) {
    struct handle *listener=handle_get(index);if(!listener)return -N_ERR_NO_HANDLE;
    struct handle *server=NULL;
    int result=channel_accept(listener,&server);
    handle_close(listener);
    return result<0?result:handle_install(server);
}

static int64_t sys_proclist(struct n_procinfo *out, int max) {
    if (max < 0) return -N_ERR_BAD_ADDRESS;
    /* the copy may sleep: the list is read into the kernel first */
    uint64_t flags = spin_lock(&task_lock);
    int room = 0;
    for (struct task *t = task_list; t && room < max; t = t->all_next) room++;
    spin_unlock(&task_lock, flags);
    if (!room) return 0;
    struct n_procinfo *list = kzalloc((size_t)room * sizeof *list);
    if (!list) return -N_ERR_NO_MEMORY;
    int n = 0;
    flags = spin_lock(&task_lock);
    for (struct task *t = task_list; t && n < room; t = t->all_next) {
        struct n_procinfo *p = &list[n++];
        p->pid = t->pid;
        p->ppid = t->parent ? t->parent->pid : 0;
        p->state = t->state;
        p->is_user = t->is_user;
        strlcpy(p->name, t->name, sizeof p->name);
        p->cpu_ms = sched_task_cpu_ns(t) / NS_PER_MS;
        p->mem_kb = proc_user_pages(t) * 4;
        p->start_ms = t->start_ns / NS_PER_MS;
        p->wait_phys = t->wait_phys;
    }
    spin_unlock(&task_lock, flags);
    int r = copy_to_user(out, list, (size_t)n * sizeof *list);
    kfree(list);
    return r < 0 ? r : n;
}

static uint64_t programs_idle(void);

static int64_t sys_sysinfo(struct n_sysinfo *usi) {
    struct n_sysinfo info = {0}, *si = &info;
    si->total_mem = pmm_total_pages() * PAGE_SIZE;
    /* What an app can have: idle programs give their frames to one that asks (frames_free), and
       the frames set aside for the APs' first touches are no one's yet. */
    si->free_mem = (pmm_free_pages() + programs_idle()) * PAGE_SIZE;
    si->heap_used = heap_used_bytes();
    si->uptime_ms = clock_now_ns() / NS_PER_MS;
    uint64_t flags = spin_lock(&task_lock);
    int n = 0;
    for (struct task *t = task_list; t; t = t->all_next) n++;
    spin_unlock(&task_lock, flags);
    si->ntasks = n;
    si->fb_w = fb.width;
    si->fb_h = fb.height;
    if (wm_running()) wm_screen_size(&si->fb_w, &si->fb_h); /* the desktop may be a remote viewer's size */
    strlcpy(si->cpu, cpu_features.brand, sizeof si->cpu);
    strlcpy(si->os, OS_NAME " " OS_VERSION, sizeof si->os);
    return copy_to_user(usi, si, sizeof *si);
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
   cleared), so its revocation looks through the paged memories too. refs is an atomic count;
   revoked and bound change under shared_lock, and revoked is read without it too (atomically). */
struct shared_scope {struct shared_scope *parent;size_t refs;int issuer;bool revoked,bound;};
/* Composite segments point only at backing roots, never at another composite.
   Cancellation references are separate, so flattening cannot discard a view's
   issuer or a nested composite's lifetime. end is the exclusive logical page. */
struct shared_part {struct object *object;size_t first,end;uint8_t cache;};
struct shared_pages {
    size_t count,bytes;uint8_t kind,cache;bool dma,owned,wc_allowed;
    struct shared_scope *scope;
    size_t maps;              /* a DMA root: mapping references, including composite segments (shared_lock) */
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
/* One mapping of a shared object into its owner's space, from the moment its range is reserved
   (building, while sys_shared_map installs its pages) until its pages went (leaving, while it is
   taken down). It is in its owner's list (shared_mappings) and in shared_all, which revocations
   walk. busy counts those working on its pages without shared_lock (shared_withdraw); it leaves
   shared_all only once busy is 0 (idle wakes its remover), so they find it, its range and its
   owner's space as they were. cancelled: a revocation reached it while it was built: it fails.
   access: NULL, or the leaves (frames, 0 for none yet) of one byte per page holding the access
   n_memory_protect set there (0: the mapping's own); every PTE installed later keeps to it.
   Under shared_lock, except that its place in its owner's list, building and leaving change with
   the owner's space lock held too; the other fields are set before it is listed. */
struct shared_mapping {
    struct shared_mapping *next;              /* its owner's list */
    struct shared_mapping *all_next,*all_prev; /* shared_all */
    struct object *object;uint64_t va,flags;size_t bytes;
    struct task *owner;
    bool building,leaving,cancelled;
    unsigned busy;
    struct wait_queue idle;
    uint64_t *access;
};
/* The shared pages' state that changes after it is made, on any CPU: every mapping's place in
   shared_all and its fields (above); the frames of demand and paged memory and their leaves,
   resident, a DMA root's cache and maps, a program segment's source, a scope's revoked and bound;
   device_pins; the pagers and their faults; the programs. Order: a process's space lock (sched.h)
   before it; after it an object's lock (object_notify), wait queues', the page tables' (vmm_*),
   pmm's. Held, nothing sleeps, copies user memory, waits for the disk or allocates from the heap:
   records are allocated before and freed after it. */
static struct spinlock shared_lock;
static struct shared_mapping *shared_all;
static void shared_all_link(struct shared_mapping *m) {
    m->all_prev=NULL;m->all_next=shared_all;
    if(shared_all)shared_all->all_prev=m;
    shared_all=m;
}
static void shared_all_unlink(struct shared_mapping *m) {
    if(m->all_prev)m->all_prev->all_next=m->all_next;else shared_all=m->all_next;
    if(m->all_next)m->all_next->all_prev=m->all_prev;
    m->all_next=m->all_prev=NULL;
}
/* Its owner's space lock and shared_lock held: m leaves its owner's list. */
static void shared_owner_unlink(struct shared_mapping *m) {
    struct shared_mapping **link=&m->owner->shared_mappings;
    while(*link!=m)link=&(*link)->next;
    *link=m->next;m->next=NULL;
}
static size_t shared_slots(const struct shared_pages *p) {
    return p->kind==SHARED_DEMAND?(p->count+SHARED_LEAF-1)/SHARED_LEAF:p->kind==SHARED_RAM?p->count:0;
}
static struct shared_pages *shared_root(struct shared_pages *p) {
    while(p->kind==SHARED_VIEW)p=p->parent->priv;
    return p;
}
static bool scope_revoked(const struct shared_scope *s) {
    for(;s;s=s->parent)if(__atomic_load_n(&s->revoked,__ATOMIC_ACQUIRE))return true;
    return false;
}
static bool scope_within(const struct shared_scope *s,const struct shared_scope *ancestor) {
    for(;s;s=s->parent)if(s==ancestor)return true;
    return false;
}
static void scope_hold(struct shared_scope *s) { __atomic_add_fetch(&s->refs,1,__ATOMIC_RELAXED); }
static void scope_drop(struct shared_scope *s) {
    while(s && !__atomic_sub_fetch(&s->refs,1,__ATOMIC_ACQ_REL)){struct shared_scope *parent=s->parent;kfree(s);s=parent;}
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
/* Every mapping, also one being built or taken down, that reaches the revoked scope, the device of
   the claim that ended, or else object: its pages [offset, offset+bytes) from its start (bytes 0:
   all of them) are unmapped, or with write_only lose write access, shot down before this returns.
   A revocation also fails a mapping still being built (cancelled): the builder looks at cancelled
   under shared_lock before each page it installs, so none comes back after this. The page tables
   change without shared_lock, the mapping pinned (busy) meanwhile; a mapping listed after this
   began (at the head) read the state its caller changed before. */
static void shared_withdraw(const struct shared_scope *scope,const struct pci_claim *claim,const struct object *object,
                            uint64_t offset,uint64_t bytes,bool write_only) {
    uint64_t lock=spin_lock(&shared_lock);
    for(struct shared_mapping *m=shared_all;m;){
        bool match=scope?shared_depends(m->object->priv,scope):claim?shared_device_gone(m->object->priv,claim):
                   m->object==object;
        if(!match){m=m->all_next;continue;}
        if((scope || claim) && m->building)m->cancelled=true;
        m->busy++;
        uint64_t pml4=m->owner->pml4,va=m->va+offset,n=bytes?bytes:m->bytes;
        spin_unlock(&shared_lock,lock);
        if(write_only)vmm_user_write_protect(pml4,va,n);
        else vmm_user_free(pml4,va,n);
        lock=spin_lock(&shared_lock);
        struct shared_mapping *next=m->all_next;
        if(!--m->busy)wq_wake_all(&m->idle);
        m=next;
    }
    spin_unlock(&shared_lock,lock);
}
/* shared_lock held. */
static void shared_dma_maps(struct shared_pages *p,bool add) {
    for(size_t at=0;at<p->count;){
        size_t page=at,span;uint8_t cache;
        struct shared_pages *leaf=shared_leaf(p,&page,&cache,NULL,&span);
        if(leaf->dma){if(add)leaf->maps++;else {ASSERT(leaf->maps);leaf->maps--;}}
        at+=MIN(span,p->count-at);
    }
}
/* A demand object's slot for page index, its table made when create; NULL when there is none.
   shared_lock held. */
static uint64_t *demand_slot(struct shared_pages *p,size_t index,bool create) {
    uint64_t *leaf=&p->pages[index/SHARED_LEAF];
    if(!*leaf && (!create || !(*leaf=pmm_alloc_zeroed())))return NULL;
    return (uint64_t *)phys_to_virt(*leaf)+index%SHARED_LEAF;
}
/* Paged memory: the entry of page index (frame | PAGED_BOUND | PAGED_WRITE, 0 when unbound) and,
   in *source, the slot of the object its frame was supplied from; NULL when the leaves are not
   there (made when create). shared_lock held. */
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
   is mapped without write access, as a read-only shared page n_memory_protect cannot change.
   shared_lock held. */
static uint64_t shared_page_flags(struct shared_pages *p,size_t index,uint64_t flags) {
    if(p->kind!=SHARED_PAGED || (paged_entry(p,index)&PAGED_WRITE))return flags;
    return (flags&~PTE_W)|PTE_SHARED_RO;
}
/* The frame of page index of p (a demand page's made with create; 0: none). shared_lock held. */
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
        if(p->kind==SHARED_RANGE && p->owned && !keep)pmm_free_contig(p->base,p->count);
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
    if(cache==N_CACHE_WRITE_COMBINE)return PTE_PWT|PTE_PAT4K; /* PAT index 5 */
    if(cache==N_CACHE_UNCACHED)return PTE_PCD|PTE_PWT; /* index 3 */
    return 0;
}

/* Device memory has one memory type at a time. x86 leaves a physical page reached through
 * linear addresses of different types undefined (Intel SDM vol. 3A, PAT: aliasing is not
 * supported), so every mapping of a device range pins the type of its pages and a mapping asking
 * another type of a pinned page is refused while the first lives. The boot screen stays
 * write-combined in the direct map for as long as the system runs. A pin belongs to the claim
 * its range came through and goes with that claim (shared_revoke_claim): no mapping reaches
 * those pages after. Under shared_lock. */
struct device_pin {struct device_pin *next;const struct pci_claim *claim;uint64_t start,end;size_t refs;uint8_t cache;};
static struct device_pin *device_pins;
/* One change of the pins under shared_lock: the records it may take for new pins (allocated
   before), the records it let go and a refused type, both dealt with after (pin_work_end). */
struct pin_work {
    struct device_pin *spares,*gone;
    int64_t error;
    uint64_t start,end;uint8_t cache,other; /* the refused mapping, when error is -N_ERR_DENIED */
};
static void pin_work_spares(struct pin_work *w,size_t n) {
    for(;n;n--){
        struct device_pin *d=kmalloc(sizeof *d);
        if(!d)return; /* a pin that finds no record fails with -N_ERR_NO_MEMORY */
        d->next=w->spares;w->spares=d;
    }
}
static const char *cache_name(uint8_t cache) {
    return cache==N_CACHE_WRITE_COMBINE?"write-combined":cache==N_CACHE_UNCACHED?"uncached":"write-back";
}
/* After shared_lock: the refusal said, the records freed; the error. */
static int64_t pin_work_end(struct pin_work *w) {
    if(w->error==-N_ERR_DENIED){
        struct task *t=task_process();
        kprintf("[shared] pid %d (%s): device memory in %#lx-%#lx is mapped %s; a %s mapping is refused\n",
                t?t->pid:0,t?t->name:"kernel",(unsigned long)w->start,(unsigned long)w->end-1,cache_name(w->other),
                cache_name(w->cache));
    }
    for(int list=0;list<2;list++)
        for(struct device_pin *d=list?w->gone:w->spares,*next;d;d=next){next=d->next;kfree(d);}
    w->spares=w->gone=NULL;
    return w->error;
}
/* The device segments p may map (one per BAR or range part): the most records pinning it takes. */
static size_t device_segments(struct shared_pages *p) {
    struct shared_pages *root=shared_root(p);
    if(root->kind!=SHARED_COMPOSITE)return root->kind==SHARED_RANGE && !root->owned;
    size_t n=0;
    for(size_t i=0;i<root->part_count;i++){
        struct shared_pages *leaf=root->parts[i].object->priv;
        n+=leaf->kind==SHARED_RANGE && !leaf->owned;
    }
    return n;
}
/* shared_lock held. */
static bool device_pin(const struct pci_claim *claim,uint64_t start,uint64_t end,uint8_t cache,struct pin_work *w) {
    uint64_t screen=ALIGN_DOWN(fb.phys,PAGE_SIZE),screen_end=ALIGN_UP(fb.phys+(uint64_t)fb.pitch*fb.height,PAGE_SIZE);
    uint8_t other=fb.addr && start<screen_end && screen<end && cache!=N_CACHE_WRITE_COMBINE?
                  N_CACHE_WRITE_COMBINE:N_CACHE_DEFAULT;
    struct device_pin *same=NULL;
    for(struct device_pin *d=device_pins;d && other==N_CACHE_DEFAULT;d=d->next){
        if(start<d->end && d->start<end && d->cache!=cache)other=d->cache;
        if(d->claim==claim && d->start==start && d->end==end && d->cache==cache)same=d;
    }
    if(other!=N_CACHE_DEFAULT){
        w->error=-N_ERR_DENIED;w->start=start;w->end=end;w->cache=cache;w->other=other;
        return false;
    }
    if(same){same->refs++;return true;}
    struct device_pin *d=w->spares;
    if(!d){w->error=-N_ERR_NO_MEMORY;return false;}
    w->spares=d->next;
    *d=(struct device_pin){.next=device_pins,.claim=claim,.start=start,.end=end,.refs=1,.cache=cache};
    device_pins=d;return true;
}
/* shared_lock held. */
static void device_unpin(const struct pci_claim *claim,uint64_t start,uint64_t end,uint8_t cache,struct pin_work *w) {
    for(struct device_pin **link=&device_pins;*link;link=&(*link)->next){
        struct device_pin *d=*link;
        if(d->claim!=claim || d->start!=start || d->end!=end || d->cache!=cache)continue;
        if(!--d->refs){*link=d->next;d->next=w->gone;w->gone=d;}
        return;
    }
    ASSERT(pci_user_revoked(claim)); /* its pins went with the claim */
}
/* Pins (add) or unpins the type of the first `limit` device segments p maps. Adding stops at a
   segment whose pages have another type, or without a record for it (w->error); the return is the
   number of segments done. shared_lock held. */
static size_t device_pins_walk(struct shared_pages *p,bool add,size_t limit,struct pin_work *w) {
    size_t done=0;
    for(size_t at=0;at<p->count && done<limit;){
        size_t page=at,span;uint8_t cache;
        struct shared_pages *leaf=shared_leaf(p,&page,&cache,NULL,&span);
        span=MIN(span,p->count-at);at+=span;
        if(leaf->kind!=SHARED_RANGE || leaf->owned)continue;
        uint64_t start=leaf->base+page*PAGE_SIZE,end=start+span*PAGE_SIZE;
        if(add && !device_pin(leaf->claim,start,end,cache,w))break;
        if(!add)device_unpin(leaf->claim,start,end,cache,w);
        done++;
    }
    return done;
}
/* shared_lock held: every device segment of p pinned, or none (the error). */
static int64_t shared_device_pin(struct shared_pages *p,struct pin_work *w) {
    size_t done=device_pins_walk(p,true,SIZE_MAX,w);
    if(w->error)device_pins_walk(p,false,done,w);
    return w->error;
}
static void shared_device_unpin(struct shared_pages *p,struct pin_work *w) { device_pins_walk(p,false,SIZE_MAX,w); }
/* shared_lock held. */
static void device_pins_drop(const struct pci_claim *claim,struct pin_work *w) {
    for(struct device_pin **link=&device_pins;*link;){
        struct device_pin *d=*link;
        if(d->claim==claim){*link=d->next;d->next=w->gone;w->gone=d;}else link=&d->next;
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
    if(bar && !root->wc_allowed)return -N_ERR_DENIED;
    size_t first=offset/PAGE_SIZE,pages=(offset+bytes-1)/PAGE_SIZE-first+1;
    uint64_t *frames=kmalloc(pages*sizeof *frames);
    if(!frames)return -N_ERR_NO_MEMORY;
    struct pin_work pins={0};
    if(bar)pin_work_spares(&pins,1);
    uint64_t lock=spin_lock(&shared_lock);
    /* a DMA root's type is read with its maps: a view changes it only while nothing maps it */
    uint8_t cache=bar?N_CACHE_WRITE_COMBINE:root->cache;
    if(!bar && cache==N_CACHE_WRITE_BACK)pins.error=-N_ERR_DENIED;
    else{
        for(size_t i=0;i<pages;i++)frames[i]=shared_frame(p,first+i,false);
        uint64_t start=frames[0],end=frames[pages-1]+PAGE_SIZE; /* a BAR's pages are one run */
        if(!bar || device_pin(root->claim,start,end,cache,&pins)){
            if(root->dma)shared_dma_maps(p,true);
            object_ref(surface->object);
        }
    }
    spin_unlock(&shared_lock,lock);
    int64_t error=pin_work_end(&pins);
    if(error){kfree(frames);return error;}
    *out=(struct shared_scanout){.object=surface->object,.frames=frames,.pages=pages,
                                 .first_byte=offset%PAGE_SIZE,.type=cache_pte(cache)};
    return 0;
}
void shared_scanout_close(struct shared_scanout *s) {
    if(!s->object)return;
    struct shared_pages *p=s->object->priv,*root=shared_root(p);
    struct pin_work pins={0};
    uint64_t lock=spin_lock(&shared_lock);
    if(root->dma)shared_dma_maps(p,false);
    else device_unpin(root->claim,s->frames[0],s->frames[s->pages-1]+PAGE_SIZE,N_CACHE_WRITE_COMBINE,&pins);
    spin_unlock(&shared_lock,lock);
    pin_work_end(&pins);
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
    uint64_t deadline=syscall_deadline(timeout_ms);
    if(!user_ok_w(uout,sizeof *uout))return -N_ERR_BAD_ADDRESS;
    struct handle *pci=handle_get(h);
    if(!pci)return -N_ERR_NO_HANDLE;
    struct n_screen_state *st=kmalloc(sizeof *st);
    int64_t r=st?pci_user_screen_state(pci,seen,st,deadline):-N_ERR_NO_MEMORY;
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
/* Zeroed RAM a claimed function may reach by DMA, within the device's and the IOMMU's address
   width. A contiguous run is allocated whole. */
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
    unsigned bits=MIN(flags&N_DMA_32BIT?32u:64u,pci_user_dma_bits(claim));
    struct shared_pages *p;
    if(flags&N_DMA_CONTIGUOUS){
        uint64_t base=pmm_alloc_contig(count,bits);
        if(!base){pci_user_drop(claim);return -N_ERR_NO_MEMORY;}
        if(!(p=shared_new(bytes,SHARED_RANGE,0))){
            pmm_free_contig(base,count);
            pci_user_drop(claim);return -N_ERR_NO_MEMORY;
        }
        p->claim=claim;
        memset(phys_to_virt(base),0,bytes);
        p->base=base;p->count=count;p->owned=true;
    }else{
        if(!(p=shared_new(bytes,SHARED_RAM,count))){pci_user_drop(claim);return -N_ERR_NO_MEMORY;}
        p->claim=claim;
        for(;p->count<count;p->count++){
            uint64_t frame=pmm_alloc_contig(1,bits);
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
        uint64_t lock=spin_lock(&shared_lock);
        for(size_t i=0;i<k;i++)piece[i]=shared_frame(p,first+done+i,false);
        spin_unlock(&shared_lock,lock);
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
           heterogeneous BAR/RAM attributes into a different cache alias.
           A DMA segment's type is its root's, read under shared_lock. */
        uint64_t lock=spin_lock(&shared_lock);
        if(cache!=N_CACHE_DEFAULT)for(size_t at=offset/PAGE_SIZE,left=bytes/PAGE_SIZE;left;){
            size_t page=at,span;uint8_t type;
            shared_leaf(src,&page,&type,NULL,&span);
            if(cache!=type){r=-N_ERR_DENIED;break;}
            span=MIN(span,left);at+=span;left-=span;
        }
        spin_unlock(&shared_lock,lock);
    }else if(root->dma){
        /* The type changes only while nothing maps the root (maps), checked and changed together:
           a mapping made meanwhile counts first, or finds the new type. */
        uint64_t lock=spin_lock(&shared_lock);
        if(cache==N_CACHE_DEFAULT)cache=root->cache;
        else if(cache!=root->cache){
            if(root->cache!=N_CACHE_WRITE_BACK || root->maps)r=-N_ERR_DENIED;
            else if(!vmm_direct_type(root->kind==SHARED_RAM?root->pages:NULL,root->base,root->count,cache_pte(cache)))
                r=-N_ERR_NO_MEMORY;
            else root->cache=(uint8_t)cache;
        }
        spin_unlock(&shared_lock,lock);
    }else if(cache==N_CACHE_DEFAULT)cache=src->cache;
    else if(device?(cache==N_CACHE_WRITE_BACK || (cache==N_CACHE_WRITE_COMBINE && !root->wc_allowed)):cache!=src->cache)r=-N_ERR_DENIED;
    if(r<0){handle_close(f);return r;}
    struct shared_scope *scope=kzalloc(sizeof *scope);
    if(!scope){handle_close(f);return -N_ERR_NO_MEMORY;}
    scope->refs=1;scope->issuer=task_process()->pid;scope->parent=src->scope;
    if(scope->parent)scope_hold(scope->parent);
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
        if(src->scope){scope_hold(src->scope);p->dependencies[p->dependency_count++]=src->scope;}
        for(size_t j=0;j<root->dependency_count;j++){
            scope_hold(root->dependencies[j]);p->dependencies[p->dependency_count++]=root->dependencies[j];
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
    /* Revoked is published under shared_lock before any PTE goes: a fault, a supply or a mapping
       built after it finds the scope revoked; what was there before, shared_withdraw finds. */
    uint64_t lock=spin_lock(&shared_lock);
    __atomic_store_n(&scope->revoked,true,__ATOMIC_RELEASE);
    spin_unlock(&shared_lock,lock);
    shared_withdraw(scope,NULL,NULL,0,0,false);
    lock=spin_lock(&shared_lock);
    bool bound=scope->bound;
    spin_unlock(&shared_lock,lock);
    if(bound)paged_revoke(scope); /* pages a pager supplied from within it */
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
/* Under shared_lock, but creator (set when it is made). pager_ready reads queued and memory
   without it, atomically. */
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
static struct pager *pagers; /* shared_lock */
#define PAGER_BATCH 128
/* shared_lock held: f leaves g's faults for *gone, which its caller frees after the lock. */
static void pager_unlink(struct pager *g,struct pager_fault *f,struct pager_fault **gone) {
    struct pager_fault **link=&g->faults;
    while(*link!=f)link=&(*link)->next;
    *link=f->next;
    if(f->queued)__atomic_sub_fetch(&g->queued,1,__ATOMIC_RELAXED);
    f->next=*gone;*gone=f;
}
static void pager_faults_free(struct pager_fault *f) {
    while(f){struct pager_fault *next=f->next;kfree(f);f=next;}
}
/* Faults of [first, first+count) that the memory now answers (supplied: their access is bound;
   failed: all of them) leave the queue and wake their threads; those no thread waits for go to
   *gone. shared_lock held. */
static void pager_settle(struct pager *g,size_t first,size_t count,uint8_t state,struct pager_fault **gone) {
    bool any=false;
    for(struct pager_fault *f=g->faults,*next;f;f=next){
        next=f->next;
        if(f->state!=PAGER_PENDING || f->page<first || f->page-first>=count)continue;
        if(state==PAGER_SUPPLIED){
            uint64_t entry=g->memory?paged_entry(g->memory,f->page):0;
            if(!(entry&PAGED_BOUND) || (f->access==N_MEMORY_READ_WRITE && !(entry&PAGED_WRITE)))continue;
        }
        f->state=state;any=true;
        if(f->queued){f->queued=false;__atomic_sub_fetch(&g->queued,1,__ATOMIC_RELAXED);}
        if(!f->waiters)pager_unlink(g,f,gone);
    }
    if(any){wq_wake_all(&g->waiters);object_notify(&g->object);}
}
/* The calling thread waits for the pager to answer page `page` of its memory with the access it
   needs. Called with shared_lock taken (lock: spin_lock's flags), which it gives back: 1 once the
   pager supplied something there, 0 when it let the lock go only to allocate the fault's record
   (*spare, which the next call takes), both for the caller to look again; -1 when the fault
   failed, the pager ended, the thread is being stopped or no memory was left for the record. */
static int pager_wait(struct pager *g,size_t page,bool write,uint64_t lock,struct pager_fault **spare) {
    uint32_t access=write?N_MEMORY_READ_WRITE:N_MEMORY_READ;
    if(g->ended || current_task->killed){spin_unlock(&shared_lock,lock);return -1;}
    struct pager_fault *f=g->faults,**tail=&g->faults,*gone=NULL;
    for(;f;tail=&f->next,f=f->next) /* one per page and access: a binding for reading answers a read while a write waits */
        if(f->page==page && f->state==PAGER_PENDING && f->access==access)break;
    if(!f){
        if(!*spare){
            spin_unlock(&shared_lock,lock);
            return (*spare=kzalloc(sizeof **spare))?0:-1;
        }
        f=*spare;*spare=NULL;
        *f=(struct pager_fault){.id=++g->next_id,.page=page,.access=access,.app=(uint32_t)task_process()->pid,
                               .state=PAGER_PENDING,.queued=true};
        *tail=f;__atomic_add_fetch(&g->queued,1,__ATOMIC_RELAXED);
        wq_wake_all(&g->taker);object_notify(&g->object);
    }
    f->waiters++;object_ref(&g->object);
    while(f->state==PAGER_PENDING && !g->ended && !current_task->killed){
        wq_wait_locked(&g->waiters,&shared_lock);
        spin_take(&shared_lock);
    }
    bool supplied=f->state==PAGER_SUPPLIED;
    if(!--f->waiters && (f->state!=PAGER_PENDING || g->ended || current_task->killed))pager_unlink(g,f,&gone);
    spin_unlock(&shared_lock,lock);
    pager_faults_free(gone);
    object_unref(&g->object);
    return supplied?1:-1;
}
/* Every mapping of paged memory p, also one being made: its pages [first, first+count) lose their
   frames (write_only: their write access), shot down before this returns. */
static void paged_unmap(struct shared_pages *p,size_t first,size_t count,bool write_only) {
    shared_withdraw(NULL,NULL,&p->object,(uint64_t)first*PAGE_SIZE,(uint64_t)count*PAGE_SIZE,write_only);
}
/* Drops the references bound pages held on their sources, once nothing reaches the frames. */
static void paged_drop(struct object **sources,size_t count) {
    uint64_t lock=spin_lock(&shared_lock);
    for(size_t i=0;i<count;i++){struct shared_pages *s=shared_root(sources[i]->priv);if(s->dma){ASSERT(s->maps);s->maps--;}}
    spin_unlock(&shared_lock,lock);
    for(size_t i=0;i<count;i++)object_unref(sources[i]);
}
/* Unbinds [first, first+count) of p (with scope: only the pages supplied from an object within it):
   unmapped everywhere first, then the frames are let go. */
static void paged_unbind(struct shared_pages *p,size_t first,size_t count,const struct shared_scope *scope) {
    while(count){
        size_t n=MIN(count,(size_t)PAGER_BATCH),dropped=0;
        struct object *sources[PAGER_BATCH];
        uint64_t lock=spin_lock(&shared_lock);
        for(size_t i=0;i<n;i++){
            struct object **source;uint64_t *entry=paged_slot(p,first+i,false,&source);
            if(!entry){i+=SHARED_LEAF-1-(first+i)%SHARED_LEAF;continue;} /* no leaves: nothing bound there */
            if(!*entry || (scope && !shared_depends((*source)->priv,scope)))continue;
            sources[dropped++]=*source;*entry=0;*source=NULL;
        }
        spin_unlock(&shared_lock,lock);
        if(dropped){paged_unmap(p,first,n,false);paged_drop(sources,dropped);}
        first+=n;count-=n;
    }
}
/* The first pager from g on whose memory a reference can still be taken, its memory held: one
   whose last reference went is on its way out of the list (paged_release runs after the count
   reached 0), and is not taken up again. shared_lock held. */
static struct pager *pager_hold_from(struct pager *g) {
    while(g && !object_ref_live(&g->memory->object))g=g->next;
    return g;
}
/* n_shared_revoke of a scope pages were supplied from within: they are unbound in every paged
   memory, unmapped everywhere before their frames go. Nothing is bound from the scope once it is
   revoked (pager_supply_run), so none comes back meanwhile. Each memory is held while its pages
   go, which keeps its pager in the list: the next one is taken before it is let go. */
static void paged_revoke(const struct shared_scope *scope) {
    uint64_t lock=spin_lock(&shared_lock);
    for(struct pager *g=pager_hold_from(pagers);g;){
        struct shared_pages *p=g->memory;
        spin_unlock(&shared_lock,lock);
        paged_unbind(p,0,p->count,scope);
        lock=spin_lock(&shared_lock);
        g=pager_hold_from(g->next);
        spin_unlock(&shared_lock,lock);
        object_unref(&p->object); /* its last reference may take its pager out of the list */
        lock=spin_lock(&shared_lock);
    }
    spin_unlock(&shared_lock,lock);
}
/* The memory's last reference went (shared_release): no mapping is left to unmap. */
static void paged_release(struct shared_pages *p) {
    struct pager *g=p->pager;
    uint64_t lock=spin_lock(&shared_lock);
    if(g->prev)g->prev->next=g->next;
    else if(pagers==g)pagers=g->next;
    if(g->next)g->next->prev=g->prev;
    g->next=g->prev=NULL;
    __atomic_store_n(&g->memory,NULL,__ATOMIC_RELEASE); /* no supply or take reaches its leaves now */
    spin_unlock(&shared_lock,lock);
    paged_unbind(p,0,p->count,NULL);
    for(size_t i=0;i<2*((p->count+SHARED_LEAF-1)/SHARED_LEAF);i++)if(p->pages[i])pmm_free(p->pages[i]);
    struct pager_fault *gone=NULL;
    lock=spin_lock(&shared_lock);
    for(struct pager_fault *f=g->faults;f;f=f->next)f->state=PAGER_FAILED;
    while(g->faults && !g->faults->waiters)pager_unlink(g,g->faults,&gone);
    wq_wake_all(&g->waiters);wq_wake_all(&g->taker);object_notify(&g->object);
    spin_unlock(&shared_lock,lock);
    pager_faults_free(gone);
    object_unref(&g->object);
}
static uint32_t pager_ready(struct handle *h) {
    struct pager *g=h->object->priv;
    return (__atomic_load_n(&g->queued,__ATOMIC_RELAXED)?N_EVENT_RECEIVABLE:0)|
           (__atomic_load_n(&g->memory,__ATOMIC_ACQUIRE)?0:N_EVENT_CLOSED);
}
/* The pager's handle closed: what waits fails, and what comes later fails at once. */
static void pager_close(struct handle *h) {
    struct pager *g=h->object->priv;
    struct pager_fault *gone=NULL;
    uint64_t lock=spin_lock(&shared_lock);
    g->ended=true;
    for(struct pager_fault *f=g->faults,*next;f;f=next){
        next=f->next;f->state=PAGER_FAILED;
        if(!f->waiters)pager_unlink(g,f,&gone);
    }
    wq_wake_all(&g->waiters);
    spin_unlock(&shared_lock,lock);
    pager_faults_free(gone);
}
static void pager_release(struct object *o) {
    struct pager *g=o->priv;
    pager_faults_free(g->faults);
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
/* g's memory with a reference of the caller's, or NULL once it went. */
static struct shared_pages *pager_memory(struct pager *g) {
    uint64_t lock=spin_lock(&shared_lock);
    struct shared_pages *p=g->memory;
    if(p && !object_ref_live(&p->object))p=NULL;
    spin_unlock(&shared_lock,lock);
    return p;
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
    uint64_t lock=spin_lock(&shared_lock);
    if((g->next=pagers))pagers->prev=g;
    pagers=g;
    spin_unlock(&shared_lock,lock);
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
    uint64_t deadline=syscall_deadline(timeout_ms);
    uint64_t lock=spin_lock(&shared_lock);
    for(;;){
        for(struct pager_fault *q=g->faults;q && k<MIN(max,ARRAY_SIZE(out));q=q->next){
            if(!q->queued)continue;
            out[k]=(struct n_pager_fault){.offset=(uint64_t)q->page*PAGE_SIZE,.access=q->access,.app=q->app};
            ids[k++]=q->id;q->queued=false;__atomic_sub_fetch(&g->queued,1,__ATOMIC_RELAXED);
        }
        if(k){r=(int64_t)k;break;}
        if(!g->memory){r=-N_ERR_CLOSED;break;}
        if(current_task->killed){r=-N_ERR_STOPPED;break;}
        if(clock_now_ns()>=deadline){r=-N_ERR_TIMEOUT;break;}
        wq_wait_locked_until(&g->taker,&shared_lock,deadline);
        spin_take(&shared_lock);
    }
    spin_unlock(&shared_lock,lock);
    if(k && copy_to_user(uout,out,k*sizeof *out)<0){
        /* the pager did not get them: they wait in the queue for its next take */
        lock=spin_lock(&shared_lock);
        for(struct pager_fault *q=g->faults;q;q=q->next)
            for(size_t i=0;i<k;i++)
                if(q->id==ids[i] && q->state==PAGER_PENDING && !q->queued){q->queued=true;__atomic_add_fetch(&g->queued,1,__ATOMIC_RELAXED);}
        spin_unlock(&shared_lock,lock);
        r=-N_ERR_BAD_ADDRESS;
    }
    handle_close(f);return r;
}
/* One run of n_pager_supply into p, g's memory (held by the caller): [offset, offset+bytes) of it
   bound to the pages of source from source_offset on. Pages of RAM only (write-back DMA RAM
   included), not paged or program memory, and from a root or a view, not a composite (its parts
   are supplied themselves); read/write access needs a writable source. Each page holds the source
   it came through, so revoking that source (or a view it descends from) unbinds the page
   (paged_revoke): a source revoked meanwhile binds nothing more. */
static int64_t pager_supply_run(struct pager *g,struct shared_pages *p,const struct n_pager_supply *run) {
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
        struct pager_fault *gone=NULL;
        uint64_t lock=spin_lock(&shared_lock);
        /* checked again with each batch, with the binding: a revocation between two finds what
           was bound before, and none is bound after it */
        if(shared_cancelled(src)){r=-N_ERR_CLOSED;n=0;}
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
        spin_unlock(&shared_lock,lock);
        /* a page bound elsewhere before is unmapped everywhere before its old frame goes */
        if(unmap)paged_unmap(p,first,n,false);
        else if(protect)paged_unmap(p,first,n,true);
        if(dropped)paged_drop(sources,dropped);
        lock=spin_lock(&shared_lock);
        pager_settle(g,first,n,PAGER_SUPPLIED,&gone);
        spin_unlock(&shared_lock,lock);
        pager_faults_free(gone);
        first+=n;from+=n;count-=n;
    }
    handle_close(sh);return r;
}
static int64_t sys_pager_supply(int h,const struct n_pager_supply *uruns,size_t count) {
    struct handle *f;int64_t r=0;
    struct pager *g=pager_get(h,&f,&r);
    if(!g)return r;
    struct shared_pages *p=count?pager_memory(g):NULL;
    if(!count)r=-N_ERR_INVALID;
    else if(!p)r=-N_ERR_CLOSED;
    for(size_t i=0;r>=0 && i<count;i++){
        struct n_pager_supply run;
        if(copy_from_user(&run,&uruns[i],sizeof run)<0){r=-N_ERR_BAD_ADDRESS;break;}
        r=pager_supply_run(g,p,&run);
        if(need_resched)schedule();
    }
    if(p)object_unref(&p->object);
    handle_close(f);return r<0?r:0;
}
/* access 0: the pages lose their frames; N_MEMORY_READ: their write access. */
static int64_t sys_pager_revoke(int h,uint64_t offset,uint64_t bytes,uint32_t access) {
    struct handle *f;int64_t r=0;
    struct pager *g=pager_get(h,&f,&r);
    if(!g)return r;
    struct shared_pages *p=pager_memory(g);
    if(!p)r=-N_ERR_CLOSED;
    else if(!bytes || (offset|bytes)&(PAGE_SIZE-1) || offset>p->bytes || bytes>p->bytes-offset ||
            (access && access!=N_MEMORY_READ))r=-N_ERR_INVALID;
    else if(!access)paged_unbind(p,offset/PAGE_SIZE,bytes/PAGE_SIZE,NULL);
    else for(size_t first=offset/PAGE_SIZE,count=bytes/PAGE_SIZE;count;){
        size_t n=MIN(count,(size_t)PAGER_BATCH);bool any=false;
        uint64_t lock=spin_lock(&shared_lock);
        for(size_t i=0;i<n;i++){
            struct object **source;uint64_t *entry=paged_slot(p,first+i,false,&source);
            if(entry && (*entry&PAGED_WRITE)){*entry&=~PAGED_WRITE;any=true;}
        }
        spin_unlock(&shared_lock,lock);
        if(any)paged_unmap(p,first,n,true);
        first+=n;count-=n;
    }
    if(p)object_unref(&p->object);
    handle_close(f);return r;
}
/* The faults of [offset, offset+bytes) fail: their threads see an access violation. */
static int64_t sys_pager_fail(int h,uint64_t offset,uint64_t bytes) {
    struct handle *f;int64_t r=0;
    struct pager *g=pager_get(h,&f,&r);
    if(!g)return r;
    if(!bytes || (offset|bytes)&(PAGE_SIZE-1) || offset+bytes<offset)r=-N_ERR_INVALID;
    else {
        struct pager_fault *gone=NULL;
        uint64_t lock=spin_lock(&shared_lock);
        pager_settle(g,offset/PAGE_SIZE,bytes/PAGE_SIZE,PAGER_FAILED,&gone);
        spin_unlock(&shared_lock,lock);
        pager_faults_free(gone);
    }
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
 * removed or replaced content on C:; the next start gives its own.
 * The cache, a program's starts and state, and its segments' sources are under shared_lock. A
 * program's segments are its one start's while it is new (not in the cache) and stay as they are
 * once it is cached. */
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
/* Whether no task maps it, reads a page of it, or starts it. shared_lock held, which keeps the
   answer true: a program no start holds gets no mapping (program_map runs for a start), and a page
   is read only through a mapping. */
static bool program_idle(const struct program *p) {
    if(p->starts)return false;
    for(const struct program_segment *s=p->segments;s;s=s->next)
        if(__atomic_load_n(&s->pages->object.refs,__ATOMIC_RELAXED)!=1)return false;
    return true;
}
/* Its frames, when it is idle; else 0. shared_lock held. */
static uint64_t program_idle_pages(const struct program *p) {
    uint64_t pages=0;
    if(!program_idle(p))return 0;
    for(const struct program_segment *s=p->segments;s;s=s->next)pages+=s->pages->resident;
    return pages;
}
/* The frames of all idle programs: free for the asking. */
static uint64_t programs_idle(void) {
    uint64_t pages=0,lock=spin_lock(&shared_lock);
    for(const struct program *p=programs;p;p=p->next)pages+=program_idle_pages(p);
    spin_unlock(&shared_lock,lock);
    return pages;
}
/* Programs no task runs leave the cache, the least recently started first, while
 * they hold more than a sixteenth of memory, or fewer than an eighth of it and
 * `need` more pages are free. */
static void programs_trim(uint64_t need) {
    uint64_t budget=pmm_total_pages()/16,floor=pmm_total_pages()/8+need;
    for(;;){
        uint64_t idle=0,lock=spin_lock(&shared_lock);struct program **last=NULL,*gone=NULL;
        for(struct program **link=&programs;*link;link=&(*link)->next){
            uint64_t pages=program_idle_pages(*link);
            if(pages){idle+=pages;last=link;}
        }
        if(last && (idle>budget || pmm_free_pages()<floor)){gone=*last;*last=gone->next;gone->state=PROGRAM_RETIRED;}
        spin_unlock(&shared_lock,lock);
        if(!gone)break;
        program_free(gone);
    }
    /* The idle ones that stay let go of their openings (nothing can read a page of them). */
    for(;;){
        uint64_t lock=spin_lock(&shared_lock);struct handle *source=NULL;
        for(struct program *p=programs;p && !source;p=p->next){
            if(!program_idle(p))continue;
            for(struct program_segment *s=p->segments;s && !source;s=s->next)
                if((source=s->pages->source))s->pages->source=NULL;
        }
        spin_unlock(&shared_lock,lock);
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
    uint64_t lock=spin_lock(&shared_lock);struct program *p=NULL;
    for(struct program **link=&programs;*link;link=&(*link)->next)
        if((*link)->document==entry.id && (*link)->identity_bytes==bytes && !memcmp((*link)->identity,identity,bytes)){
            p=*link;*link=p->next;p->next=programs;programs=p; /* the most recently started */
            p->starts++;break;
        }
    spin_unlock(&shared_lock,lock);
    if(p){kfree(identity);return p;}
    if(!(p=kzalloc(sizeof *p))){kfree(identity);return NULL;}
    *p=(struct program){.document=entry.id,.identity=identity,.identity_bytes=bytes,.starts=1,.state=PROGRAM_NEW};
    return p;
}
void program_put(struct program *p,bool complete) {
    if(!p)return;
    struct program *replaced=NULL;uint64_t lock=spin_lock(&shared_lock);
    if(p->state==PROGRAM_NEW && complete && p->segments){
        /* An older content of the document stays with the tasks running it. */
        for(struct program **link=&programs;*link;link=&(*link)->next)
            if((*link)->document==p->document){replaced=*link;*link=replaced->next;replaced->state=PROGRAM_RETIRED;break;}
        p->next=programs;programs=p;p->state=PROGRAM_CACHED;
    }
    bool gone=!--p->starts && p->state!=PROGRAM_CACHED;
    if(replaced && replaced->starts)replaced=NULL; /* its last start lets go of it */
    spin_unlock(&shared_lock,lock);
    if(replaced)program_free(replaced);
    if(gone)program_free(p);
}
/* t is a process being started (sys/proc.c): no thread of it runs yet. */
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
    uint64_t flags=PTE_U|PTE_SHARED|PTE_SHARED_RO|(exec?0:PTE_NX),lock;bool ok=true;
    for(size_t i=0;ok && i<p->count;i++){
        lock=spin_lock(&shared_lock);
        bool any=p->resident;
        uint64_t frame=any?shared_frame(p,i,false):0;
        ok=!frame || vmm_map_page(t->pml4,start+i*PAGE_SIZE,frame,flags);
        spin_unlock(&shared_lock,lock);
        if(!any)break;
    }
    if(!ok){
        vmm_user_free(t->pml4,start,end-start);kfree(m);
        if(fresh){shared_release(&p->object);kfree(fresh);}
        return -N_ERR_NO_MEMORY;
    }
    lock=spin_lock(&t->space.lock);
    spin_take(&shared_lock);
    if(!p->source)p->source=handle_ref(doc); /* a new segment's, or an idle one's: the same content */
    if(fresh){
        object_ref(&p->object); /* the program's */
        *fresh=(struct program_segment){.next=prog->segments,.pages=p,.offset=offset,.va=va,.filesz=filesz,.memsz=memsz,
                                        .exec=exec};
        prog->segments=fresh;
    }
    object_ref(&p->object); /* the mapping's */
    *m=(struct shared_mapping){.next=t->shared_mappings,.object=&p->object,.va=start,.flags=flags,.bytes=end-start,
                               .owner=t};
    t->shared_mappings=m;shared_all_link(m);
    spin_give(&shared_lock);
    spin_unlock(&t->space.lock,lock);
    return 0;
}
/* Pages a first touch of a program segment reads together: the touched one and the others of its
   aligned 64 KiB that no task has read, in one read from the document. */
#define PROGRAM_AROUND 16
/* Reads page index of p and those around it from source and adopts those no peer adopted meanwhile.
   Called with no lock held, holding p and source: the read waits for the disk. */
static int program_fill(struct shared_pages *p,struct handle *source,size_t index) {
    size_t first=ALIGN_DOWN(index,(size_t)PROGRAM_AROUND),last=MIN(first+PROGRAM_AROUND,p->count);
    if(!frames_free(4096+(last-first)))first=index,last=index+1; /* memory is short: what is touched only */
    uint64_t lock=spin_lock(&shared_lock);
    for(uint64_t *slot;first<index && (slot=demand_slot(p,first,false)) && *slot;)first++;
    for(uint64_t *slot;last>index+1 && (slot=demand_slot(p,last-1,false)) && *slot;)last--;
    spin_unlock(&shared_lock,lock);
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
        lock=spin_lock(&shared_lock);
        uint64_t *slot=demand_slot(p,i,true);
        bool adopted=slot && !*slot;
        if(adopted){*slot=frame;p->resident++;}
        spin_unlock(&shared_lock,lock);
        if(!adopted)pmm_free(frame);
        if(!slot && i==index)r=-N_ERR_NO_MEMORY;
    }
    kfree(buf);
    return r;
}
static uint64_t gone_device_page; /* shared_fault: what revoked device mappings show (shared_lock) */
/* The claim ended (dev/pciuser.c marked it released before): every mapping that reaches its
   device's ranges loses their pages, one being built fails, and the pins go. A fault there finds
   the claim released and maps gone_device_page. */
void shared_revoke_claim(const struct pci_claim *c) {
    shared_withdraw(NULL,c,NULL,0,0,false);
    struct pin_work pins={0};
    uint64_t lock=spin_lock(&shared_lock);
    device_pins_drop(c,&pins);
    spin_unlock(&shared_lock,lock);
    pin_work_end(&pins);
}
/* The PTE flags page `page` (counted from the mapping's start) of m is mapped with: the mapping's
   own, or the access n_memory_protect set there, which never exceeds them (shared_protect).
   shared_lock held. */
static uint64_t shared_mapping_flags(const struct shared_mapping *m,size_t page) {
    uint64_t leaf=m->access?m->access[page/PAGE_SIZE]:0;
    uint8_t access=leaf?((const uint8_t *)phys_to_virt(leaf))[page%PAGE_SIZE]:0;
    if(!access)return m->flags;
    uint64_t flags=access==N_MEMORY_READ_WRITE?m->flags:m->flags&~PTE_W;
    return access==N_MEMORY_READ_EXECUTE?flags&~PTE_NX:flags|PTE_NX;
}
static size_t shared_access_leaves(const struct shared_mapping *m) {
    return (m->bytes/PAGE_SIZE+PAGE_SIZE-1)/PAGE_SIZE;
}
/* n_memory_protect of [va, va+bytes) (page aligned, not empty) with access (vp: its VM_ bits),
   which overlaps mapping m of task t: the range lies in m and asks no more than m grants. The
   access is kept for each page, also one not mapped now (paged memory, an untouched demand page,
   a page of a device gone), so every PTE a later fault installs keeps to it; the present pages
   change at once. No lock held: the caller's space operation over the range (sys_memory_protect)
   keeps m listed and its range unchanged meanwhile. A fault that maps a page reads the access
   with shared_lock held and maps it before it lets go: it maps the page with the access set here,
   or before vmm_user_protect changes it. */
static int64_t shared_protect(struct task *t,struct shared_mapping *m,uint64_t va,uint64_t bytes,int access,int vp) {
    if(va<m->va || bytes>m->bytes-(va-m->va))return -N_ERR_NO_MEMORY; /* reaches past it: its guard page */
    if(shared_cancelled(m->object->priv))return -N_ERR_CLOSED;
    /* Write access only in a writable mapping. Execution never: shared memory is mapped without
       it (n_shared_map refuses N_MEMORY_READ_EXECUTE), and a program's code (PTE_SHARED_RO) keeps
       the access it was mapped with. Else an app could execute pages it, or another holder of the
       memory, writes through a second mapping. */
    if(((vp&VM_W) && !(m->flags&PTE_W)) || ((vp&VM_X) && (m->flags&(PTE_NX|PTE_SHARED_RO))))return -N_ERR_DENIED;
    uint64_t lock=spin_lock(&shared_lock),*spare=NULL;
    bool missing=!m->access;
    spin_unlock(&shared_lock,lock);
    if(missing && !(spare=kzalloc(shared_access_leaves(m)*sizeof *spare)))return -N_ERR_NO_MEMORY;
    size_t first=(va-m->va)/PAGE_SIZE,end=first+bytes/PAGE_SIZE;
    for(size_t i=first/PAGE_SIZE;i<=(end-1)/PAGE_SIZE;i++){
        lock=spin_lock(&shared_lock);
        if(!m->access){m->access=spare;spare=NULL;} /* else a protection of another part made them */
        bool ok=m->access[i] || (m->access[i]=pmm_alloc_zeroed());
        spin_unlock(&shared_lock,lock);
        if(!ok){kfree(spare);return -N_ERR_NO_MEMORY;}
    }
    kfree(spare);
    for(size_t at=first;at<end;){
        size_t n=MIN(end-at,PAGE_SIZE-at%PAGE_SIZE);
        lock=spin_lock(&shared_lock);
        memset((uint8_t *)phys_to_virt(m->access[at/PAGE_SIZE])+at%PAGE_SIZE,access,n);
        spin_unlock(&shared_lock,lock);
        at+=n;
    }
    return vmm_user_protect(t->pml4,va,bytes,vp,true);
}
/* A first touch of a reserved shared page, from user code or a kernel copy
 * on its behalf: allocate the object's frame (or adopt the one a peer already
 * created) and map it with the mapping's protection (or the page's own, set by
 * n_memory_protect). The mapping is looked up and the page mapped with shared_lock held, so a
 * revocation or a removal that changes the mapping after finds the page. A mapping still being
 * built or being taken down maps nothing: the touch fails. A program's page is read from its
 * document, and a pager's page waits for its pager, with no lock held (*slept): the mapping is
 * looked up again after. */
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
    struct pager_fault *spare=NULL; /* a record pager_wait let the lock go for */
again:;
    uint64_t lock=spin_lock(&shared_lock);bool ok=false,starved=false;
    struct shared_pages *fill=NULL;struct handle *source=NULL;size_t at=0;
    for(struct shared_mapping *m=owner->shared_mappings;m;m=m->next){
        if(page<m->va || page-m->va>=m->bytes)continue;
        if(m->building || m->leaving)break;
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
            int answered=pager_wait(leaf->pager,index,write,lock,&spare);
            if(answered<0){kfree(spare);return false;}
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
    spin_unlock(&shared_lock,lock);
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
    kfree(spare);
    if(starved){
        kprintf("[shared] pid %d (%s): no frame for page %p, %lu pages free\n",
                owner->pid,owner->name,(void *)page,(unsigned long)pmm_free_pages());
        report_memory_exhaustion(owner,PAGE_SIZE);
    }
    return ok;
}
bool shared_demand_fault(uint64_t addr,bool write) { return shared_fault(addr,write,NULL); }

/* A process's space (the fields of struct task in sched.h, under its space lock). Freed address
   space of a region goes back to it: without reuse every map/unmap cycle consumes fresh addresses
   and the page tables covering them, so a process that keeps allocating and freeing large blocks
   leaks kernel memory. Every reservation keeps one unmapped guard page after itself; a hole
   includes it. The low region below 2 GiB keeps its own holes and frontier. A range is the
   reserver's from the moment it leaves the holes or the frontier. A thread that changes a range it
   holds (a reservation until its pages are there, a release, a protection, a shared mapping taken
   down) lists an operation over it first, under the space lock: another change of a range that
   overlaps it waits until it is done, so a release or protection never meets a range half made,
   half gone or given out again. The space lock is held only briefly (no sleep, no copy, no page
   table work but one page's): the page tables change with it let go. */
struct mmap_hole { struct mmap_hole *next; uint64_t va, len; };
struct space_op { struct space_op *next; uint64_t va, end; };
static void holes_free(struct mmap_hole *h) {
    while (h) { struct mmap_hole *next = h->next; kfree(h); h = next; }
}
/* Space lock held: [va, va+len) is free again in its region. A new record comes from *spare
   (without one the range stays unused, which breaks nothing); the records merged away and a last
   hole that lowers the frontier go to *gone, freed after the lock. Nothing comes back once the
   space ended. */
static void hole_add_locked(struct task *owner, uint64_t va, uint64_t len, struct mmap_hole **spare,
                            struct mmap_hole **gone) {
    if (!len || owner->space_ended) return;
    bool low = va < USER_MMAP_BASE;
    struct mmap_hole **holes = low ? &owner->low_holes : &owner->mmap_holes;
    uint64_t *next = low ? &owner->low_next : &owner->mmap_next;
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
            h->next = n->next; n->next = *gone; *gone = n;
        }
    } else if (*spare) {
        struct mmap_hole *s = *spare;
        *spare = NULL;
        *s = (struct mmap_hole){ .next = h, .va = va, .len = len };
        *link = h = s;
    } else return;
    /* The last hole simply lowers the reservation frontier. */
    if (!h->next && h->va + h->len >= *next) {
        *next = h->va;
        if (prev) prev->next = NULL; else *holes = NULL;
        h->next = *gone; *gone = h;
    }
}
/* Any CPU, no lock held: [va, va+len) of owner's space is free again (a thread's stack, a
   window's buffers). */
void mmap_hole_add(struct task *owner, uint64_t va, uint64_t len) {
    if (!owner || !len) return;
    struct mmap_hole *spare = kmalloc(sizeof *spare), *gone = NULL;
    uint64_t lock = spin_lock(&owner->space.lock);
    hole_add_locked(owner, va, len, &spare, &gone);
    spin_unlock(&owner->space.lock, lock);
    kfree(spare);
    holes_free(gone);
}
/* Space lock held: len bytes and a guard page from a region's holes (first fit), else at its
   frontier below top; 0 when neither has room. A hole used up goes to *gone. */
static uint64_t region_take_locked(struct mmap_hole **holes, uint64_t *next, uint64_t top, uint64_t len,
                                   struct mmap_hole **gone) {
    if (!len || len > top) return 0;
    uint64_t need = len + PAGE_SIZE;
    for (struct mmap_hole **link = holes; *link; link = &(*link)->next) {
        struct mmap_hole *h = *link;
        if (h->len < need) continue;
        uint64_t va = h->va;
        h->va += need; h->len -= need;
        if (!h->len) { *link = h->next; h->next = *gone; *gone = h; }
        return va;
    }
    uint64_t va = *next;
    if (va >= top || need > top - va) return 0;
    *next = va + need;
    return va;
}
/* Space lock held: len bytes and a guard page from the low region or the mmap region; 0 when it
   has no room or the space ended. */
static uint64_t space_take_locked(struct task *owner, bool low, uint64_t len, struct mmap_hole **gone) {
    if (owner->space_ended) return 0;
    return low ? region_take_locked(&owner->low_holes, &owner->low_next, USER_LOW_TOP, len, gone)
               : region_take_locked(&owner->mmap_holes, &owner->mmap_next, USER_STACK_TOP - USER_STACK_MAX, len, gone);
}
/* Any CPU, no lock held. */
uint64_t mmap_reserve(struct task *owner, uint64_t len) {
    struct mmap_hole *gone = NULL;
    uint64_t lock = spin_lock(&owner->space.lock);
    uint64_t va = space_take_locked(owner, false, len, &gone);
    spin_unlock(&owner->space.lock, lock);
    holes_free(gone);
    return va;
}
/* The app's end: its holes go, and its space takes no reservation or hole again. */
static void mmap_holes_free(struct task *owner) {
    uint64_t lock = spin_lock(&owner->space.lock);
    struct mmap_hole *high = owner->mmap_holes, *low = owner->low_holes;
    owner->mmap_holes = owner->low_holes = NULL;
    owner->space_ended = true;
    spin_unlock(&owner->space.lock, lock);
    holes_free(high);
    holes_free(low);
}
/* Space lock held: whether an operation is at part of [va, end). */
static bool space_busy(const struct task *owner, uint64_t va, uint64_t end) {
    for (const struct space_op *o = owner->space_ops; o; o = o->next)
        if (va < o->end && o->va < end) return true;
    return false;
}
/* Space lock held, taken with spin_lock: returns once no operation is at part of [va, end), the
   lock held again; it is let go while the thread waits. */
static void space_wait(struct task *owner, uint64_t va, uint64_t end) {
    while (space_busy(owner, va, end)) {
        wq_wait_locked(&owner->space, &owner->space.lock);
        spin_take(&owner->space.lock);
    }
}
/* Space lock held. */
static void space_op_add(struct task *owner, struct space_op *op, uint64_t va, uint64_t end) {
    *op = (struct space_op){ .next = owner->space_ops, .va = va, .end = end };
    owner->space_ops = op;
}
static void space_op_remove(struct task *owner, struct space_op *op) {
    struct space_op **link = &owner->space_ops;
    while (*link != op) link = &(*link)->next;
    *link = op->next;
}
/* No lock held: the operation is done, [va, va+len) going back to its region with it (len 0:
   nothing), and the threads waiting for its range look again. */
static void space_op_finish(struct task *owner, struct space_op *op, uint64_t va, uint64_t len) {
    struct mmap_hole *spare = len ? kmalloc(sizeof *spare) : NULL, *gone = NULL;
    uint64_t lock = spin_lock(&owner->space.lock);
    hole_add_locked(owner, va, len, &spare, &gone);
    space_op_remove(owner, op);
    spin_unlock(&owner->space.lock, lock);
    wq_wake_all(&owner->space);
    kfree(spare);
    holes_free(gone);
}
/* Space lock held: whether part of [va, end) was never given out or is free again: at or past a
   region's frontier, or in one of its holes. Outside the two regions nothing is handed out. */
static bool space_free_overlap(const struct task *owner, uint64_t va, uint64_t end) {
    for (int low = 0; low < 2; low++) {
        if (low && !owner->low_base) continue;
        uint64_t top = low ? USER_LOW_TOP : USER_STACK_TOP - USER_STACK_MAX;
        uint64_t next = low ? owner->low_next : owner->mmap_next;
        if (MAX(va, next) < MIN(end, top)) return true;
        for (const struct mmap_hole *h = low ? owner->low_holes : owner->mmap_holes; h; h = h->next)
            if (va < h->va + h->len && h->va < end) return true;
    }
    return false;
}
/* Space lock held: the first mapping that overlaps [va, end), or NULL. */
static struct shared_mapping *space_mapping(const struct task *owner, uint64_t va, uint64_t end) {
    struct shared_mapping *m = owner->shared_mappings;
    while (m && !(va < m->va + m->bytes && m->va < end)) m = m->next;
    return m;
}

static void shared_unmap(struct task *owner,struct shared_mapping *m,struct space_op *op,bool hole);
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
    bytes=ALIGN_UP(bytes,PAGE_SIZE);
    if(bytes!=p->bytes){object_unref(&p->object);return -N_ERR_INVALID;}
    struct shared_mapping *m=kzalloc(sizeof *m);
    if(!m){object_unref(&p->object);return -N_ERR_NO_MEMORY;}
    uint64_t flags=PTE_U|PTE_SHARED|PTE_NX|(access==N_MEMORY_READ_WRITE?PTE_W:PTE_SHARED_RO);
    *m=(struct shared_mapping){.object=&p->object,.flags=flags,.bytes=bytes,.owner=owner,.building=true};
    struct pin_work pins={0};struct mmap_hole *gone=NULL;
    pin_work_spares(&pins,device_segments(p));
    /* The range is reserved (reusing released ones), the device pages' types pinned, the DMA
       types held and the mapping listed at once, before any PTE: no other reservation can choose
       the partially mapped span, and a revocation from here on finds the mapping. */
    uint64_t va=0,lock=spin_lock(&owner->space.lock);
    spin_take(&shared_lock);
    if(!shared_device_pin(p,&pins) && !(va=space_take_locked(owner,false,bytes,&gone)))shared_device_unpin(p,&pins);
    if(va){
        shared_dma_maps(p,true);
        m->va=va;m->cancelled=shared_cancelled(p) || shared_device_gone(p,NULL);
        m->next=owner->shared_mappings;owner->shared_mappings=m;
        shared_all_link(m);
    }
    spin_give(&shared_lock);
    spin_unlock(&owner->space.lock,lock);
    holes_free(gone);
    int64_t pinned=pin_work_end(&pins);
    if(!va){object_unref(&p->object);kfree(m);return pinned<0?pinned:-N_ERR_NO_MEMORY;}
    /* Demand pages not yet touched by any peer stay unmapped until a fault. */
    size_t mapped=0;
    for(;mapped<p->count;mapped++){
        size_t index=mapped;uint8_t cache;
        lock=spin_lock(&shared_lock);
        if(m->cancelled || current_task->killed){spin_unlock(&shared_lock,lock);break;}
        shared_leaf(p,&index,&cache,NULL,NULL);
        uint64_t frame=shared_frame(p,mapped,false);
        bool ok=!frame || vmm_map_page(owner->pml4,va+mapped*PAGE_SIZE,frame,
                                       shared_page_flags(p,mapped,flags)|cache_pte(cache));
        spin_unlock(&shared_lock,lock);
        if(!ok)break;
        if(need_resched)schedule();
    }
    /* A revocation during construction sets cancelled under shared_lock; looked at with the end
       of building under the same lock, it cannot be missed and leave the mapping live after. */
    lock=spin_lock(&owner->space.lock);
    spin_take(&shared_lock);
    int64_t result=m->cancelled || shared_cancelled(p) || shared_device_gone(p,NULL)?-N_ERR_CLOSED:
                   current_task->killed?-N_ERR_STOPPED:-N_ERR_NO_MEMORY;
    bool failed=mapped!=p->count || result!=-N_ERR_NO_MEMORY;
    struct space_op op;
    m->building=false;
    if(failed){m->leaving=true;space_op_add(owner,&op,va,va+bytes);}
    spin_give(&shared_lock);
    spin_unlock(&owner->space.lock,lock);
    if(!failed)return (int64_t)va;
    shared_unmap(owner,m,&op,true);
    return result;
}
/* Takes mapping m down. Its caller set leaving and listed op over its range, both with its
   owner's space lock and shared_lock held, and holds neither now. Its pages go (and the all-ones
   pages a revoked device range may have received); once no revocation works on it, it leaves
   shared_all, then its owner's list, its pins and DMA types go back, and its object is dropped.
   hole: its range and guard page go back to the mmap region. */
static void shared_unmap(struct task *owner,struct shared_mapping *m,struct space_op *op,bool hole) {
    struct mmap_hole *spare=hole?kmalloc(sizeof *spare):NULL,*gone=NULL;
    struct pin_work pins={0};
    /* Keep every DMA cache pin through PTE removal and the completed TLB
       shootdown. A view cannot retype still-reachable old pages. */
    vmm_user_free(owner->pml4,m->va,m->bytes);
    uint64_t lock=spin_lock(&shared_lock);
    while(m->busy){
        wq_wait_locked(&m->idle,&shared_lock);
        spin_take(&shared_lock);
    }
    shared_all_unlink(m);
    spin_give(&shared_lock);
    spin_take(&owner->space.lock);
    spin_take(&shared_lock);
    shared_owner_unlink(m);
    shared_dma_maps(m->object->priv,false);
    shared_device_unpin(m->object->priv,&pins);
    if(hole)hole_add_locked(owner,m->va,m->bytes+PAGE_SIZE,&spare,&gone);
    space_op_remove(owner,op);
    spin_give(&shared_lock);
    spin_unlock(&owner->space.lock,lock);
    wq_wake_all(&owner->space);
    kfree(spare);holes_free(gone);
    pin_work_end(&pins);
    object_unref(m->object);
    for(size_t i=0;m->access && i<shared_access_leaves(m);i++)if(m->access[i])pmm_free(m->access[i]);
    kfree(m->access);kfree(m);
}
/* The app's end, or a start that failed: every mapping goes, then the space's records, and nothing
   is reserved in it again. No thread of the app runs, so none builds or takes down a mapping;
   revocations elsewhere may still be at work on its mappings (shared_unmap waits for them). */
void sys_shared_cleanup(struct task *owner) {
    for(;;){
        struct space_op op;
        uint64_t lock=spin_lock(&owner->space.lock);
        spin_take(&shared_lock);
        struct shared_mapping *m=owner->shared_mappings;
        if(m){
            ASSERT(!m->building && !m->leaving);
            m->leaving=true;space_op_add(owner,&op,m->va,m->va+m->bytes);
        }
        spin_give(&shared_lock);
        spin_unlock(&owner->space.lock,lock);
        if(!m)break;
        shared_unmap(owner,m,&op,false);
    }
    mmap_holes_free(owner);
    programs_trim(0); /* its programs may be idle now */
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

/* Zeroed pages in the reservation region above the program (window buffers,
   thread stacks and shared mappings live there too), or below 2 GiB with
   N_MEMORY_LOW. Beyond a few pages they receive frames on first touch, so a
   process pays only for what it uses; a reservation larger than all free memory
   still fails here. An operation over the range and its guard page keeps a
   sibling's release or protection of them out until the pages are there. */
#define RESERVE_EAGER (64 * 1024)
static int64_t sys_memory_reserve(size_t len, int access) {
    struct task *t = task_process();
    int vp = access_flags(access & ~N_MEMORY_LOW);
    /* No size cap of its own: free memory and the address region bound it. */
    if (len == 0 || len > SIZE_MAX - (PAGE_SIZE - 1) || vp < 0) return -N_ERR_INVALID;
    len = ALIGN_UP(len, PAGE_SIZE);
    if (!frames_free(len / PAGE_SIZE)) { report_memory_exhaustion(t, len); return -N_ERR_NO_MEMORY; }
    struct space_op op;struct mmap_hole *gone = NULL;
    uint64_t lock = spin_lock(&t->space.lock);
    uint64_t va = space_take_locked(t, access & N_MEMORY_LOW, len, &gone);
    if (va) space_op_add(t, &op, va, va + len + PAGE_SIZE);
    spin_unlock(&t->space.lock, lock);
    holes_free(gone);
    if (!va) return -N_ERR_NO_MEMORY;
    int r = len <= RESERVE_EAGER ? vmm_user_alloc(t->pml4, va, len, vp) : vmm_user_reserve(t->pml4, va, len, vp);
    if (r < 0) {
        vmm_user_free(t->pml4, va, len);
        space_op_finish(t, &op, va, len + PAGE_SIZE);
        report_memory_exhaustion(t, len);
        return -N_ERR_NO_MEMORY;
    }
    space_op_finish(t, &op, 0, 0);
    return (int64_t)va;
}

/* Releases reserved pages or a whole shared mapping; window buffers stay with their window. Only
   what the space gave out and still holds: a range with a free part fails (a second release of it
   too). A release of a range another thread is changing waits for it. */
static int64_t sys_memory_release(uint64_t va, size_t len) {
    struct task *t = task_process();
    bool low = va < USER_MMAP_BASE;
    uint64_t base = low ? t->low_base : USER_MMAP_BASE, top = low ? USER_LOW_TOP : USER_STACK_TOP - USER_STACK_MAX;
    if (len > SIZE_MAX - (PAGE_SIZE - 1)) return -N_ERR_INVALID;
    uint64_t end = va + ALIGN_UP(len, PAGE_SIZE);
    if (va & (PAGE_SIZE - 1) || va < base || end > top || end < va) return -N_ERR_INVALID;
    if (end == va) return 0;
    struct space_op op;
    int64_t r = 0;
    uint64_t lock = spin_lock(&t->space.lock);
    space_wait(t, va, end);
    /* an overlapping mapping is not leaving now: its remover's operation was waited for */
    bool unreserved = space_free_overlap(t, va, end);
    struct shared_mapping *m = unreserved ? NULL : space_mapping(t, va, end);
    if (unreserved) r = -N_ERR_INVALID;
    else if (m && m->building) r = -N_ERR_NOT_READY;
    else if (m && (va != m->va || end - va != m->bytes)) r = -N_ERR_INVALID;
    else {
        if (m) { spin_take(&shared_lock); m->leaving = true; spin_give(&shared_lock); }
        space_op_add(t, &op, va, end);
    }
    spin_unlock(&t->space.lock, lock);
    if (r < 0) return r;
    /* sys_shared_map reserved the range and its guard page: they are reused */
    if (m) { shared_unmap(t, m, &op, !low); return 0; }
    for (uint64_t a = va; a < end; a += PAGE_SIZE)
        if (vmm_get_pte(t->pml4, a) & PTE_SHARED) { space_op_finish(t, &op, 0, 0); return -N_ERR_INVALID; } /* a window buffer */
    vmm_user_free(t->pml4, va, end - va);
    struct mmap_hole *spare = kmalloc(sizeof *spare), *gone = NULL;
    lock = spin_lock(&t->space.lock);
    /* The unmapped page right after the range is the reservation's guard, unless it was handed
       out: an untouched reserved page, a shared mapping's page (also an untouched demand one), or
       a range another thread is reserving or changing. */
    bool guard = end < (low ? t->low_next : t->mmap_next) && !space_busy(t, end, end + PAGE_SIZE) &&
                 !(vmm_get_leaf(t->pml4, end) & (PTE_P | PTE_DEMAND)) && !space_mapping(t, end, end + PAGE_SIZE);
    hole_add_locked(t, va, end - va + (guard ? PAGE_SIZE : 0), &spare, &gone);
    space_op_remove(t, &op);
    spin_unlock(&t->space.lock, lock);
    wq_wake_all(&t->space);
    kfree(spare);
    holes_free(gone);
    return 0;
}

/* Private pages take the access as asked; the pages of a shared mapping keep within the
   mapping's, also those a fault maps later (shared_protect). A range with a part that was never
   given out or is free again fails as an unmapped one does; a protection of a range another
   thread is changing waits for it. */
static int64_t sys_memory_protect(uint64_t va, size_t len, int access) {
    int vp = access_flags(access);
    if (vp < 0 || va & (PAGE_SIZE - 1) || va >= USER_TOP || va + len > USER_TOP || va + len < va) return -N_ERR_INVALID;
    struct task *t = task_process();
    if (!len) return 0;
    uint64_t end = ALIGN_UP(va + len, PAGE_SIZE);
    struct space_op op;
    int64_t r = 0;
    uint64_t lock = spin_lock(&t->space.lock);
    space_wait(t, va, end);
    struct shared_mapping *m = space_mapping(t, va, end);
    if (m && m->building) r = -N_ERR_NOT_READY;
    else if (!m && space_free_overlap(t, va, end)) r = -N_ERR_NO_MEMORY;
    else space_op_add(t, &op, va, end);
    spin_unlock(&t->space.lock, lock);
    if (r < 0) return r;
    r = m ? shared_protect(t, m, va, end - va, access, vp) : vmm_user_protect(t->pml4, va, len, vp, false);
    space_op_finish(t, &op, 0, 0);
    return r;
}

/* The process's mapping of the hypervisor's clock page (read-only, a guard page after it), made
   at its first ask; 0 without one. */
static uint64_t clock_page_of(struct task *owner) {
    uint64_t va = __atomic_load_n(&owner->clock_page, __ATOMIC_ACQUIRE);
    if (va) return va;
    uint64_t reference = vm_clock_page();
    if (!reference) return 0;
    struct mmap_hole *spare = kmalloc(sizeof *spare), *gone = NULL;
    uint64_t lock = spin_lock(&owner->space.lock);
    if (!(va = owner->clock_page) && (va = space_take_locked(owner, false, PAGE_SIZE, &gone))) {
        if (vmm_map_page(owner->pml4, va, reference, PTE_P | PTE_U | PTE_SHARED | PTE_SHARED_RO | PTE_NX))
            __atomic_store_n(&owner->clock_page, va, __ATOMIC_RELEASE);
        else {
            hole_add_locked(owner, va, 2 * PAGE_SIZE, &spare, &gone);
            va = 0;
        }
    }
    spin_unlock(&owner->space.lock, lock);
    kfree(spare);
    holes_free(gone);
    return va;
}

/* Copies the system log's retained text. */
static int64_t sys_system_log(char *buf, size_t n) {
    n = MIN(n, (size_t)KLOG_SIZE);
    char *text = kmalloc(n ? n : 1);
    if (!text) return -N_ERR_NO_MEMORY;
    size_t got = klog_read(text, n);
    int r = copy_to_user(buf, text, got);
    kfree(text);
    return r < 0 ? r : (int64_t)got;
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
    struct handle *handle = handle_get(h);
    if (!handle) return -N_ERR_NO_HANDLE;
    int kind = handle_kind(handle);
    handle_close(handle);
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
    int ia, ib;
    e = handle_install_pair(a, b, &ia, &ib);
    if (e < 0) {
        handle_close(a);
        handle_close(b);
        return e;
    }
    int32_t pair[2] = {ia, ib};
    if (copy_to_user(uout, pair, sizeof pair) < 0) {
        handle_remove(ia);
        handle_remove(ib);
        return -N_ERR_BAD_ADDRESS;
    }
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
    r = proc_app_result(app, syscall_deadline(timeout_ms), &status);
    if (r == -N_ERR_TIMEOUT && !timeout_ms) r = -N_ERR_NOT_READY; /* a look that does not wait */
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
    if (flags & N_APP_STOP_TREE) task_kill_tree(pid, true);
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
    struct handle *watched = handle_get(h);
    r = watched ? events_watch(queue, watched, interest, token, &spare) : -N_ERR_NO_HANDLE;
    handle_close(watched);
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
    r = events_timer(queue, ms < CLOCK_NEVER / NS_PER_MS ? ms * NS_PER_MS : CLOCK_NEVER, repeat != 0, token);
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
    struct handle *queue = handle_get_as(q, events_is, &r);
    if (!queue) return r;
    r = events_wait(queue, uout, max, syscall_deadline(timeout_ms));
    handle_close(queue);
    return r;
}

void syscall_dispatch(struct regs *r) {
    /* A thread a sibling killed does not start a call it may then wait in. */
    if (current_task->killed) task_exit(current_task->kill_code);
    sti();
    uint64_t a = r->rdi, b = r->rsi, c = r->rdx, d = r->r10, e = r->r8;
    int64_t ret = -N_ERR_UNSUPPORTED;
    switch (r->rax) {
    case SYS_EXIT: task_exit_app((int)a);
    case SYS_AUDIO_FLUSH: ret = sys_audio_flush((int)a); break;
    case SYS_SLEEP: sleep_until(syscall_deadline((int64_t)a)); ret = 0; break;
    case SYS_UPTIME: ret = (int64_t)(clock_now_ns() / NS_PER_MS); break;
    case SYS_TIME: ret = clock_wall_seconds(); break;
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
        else if (r->rax == SYS_PCI_ACPI_NOTIFY_TAKE) ret = pci_user_acpi_notify_take(f, syscall_deadline((int64_t)b));
        else if (r->rax == SYS_PCI_SCREEN_IMAGE_END) ret = pci_user_screen_image_end(f, (int64_t)b);
        else ret = pci_user_interrupt_take(f,syscall_deadline((int64_t)b));
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
        smp_get_info(&info);
        ret = copy_to_user((void *)a, &info, sizeof info);
        break;
    }
    case SYS_CLOCK_INFO: {
        /* A whole millisecond of the clock and what the program's own counter (the clock's source)
           read at it: the program carries the clock on from there by itself. */
        struct n_clockinfo info = {.version = 1};
        info.uptime_ms = clock_now_ns() / NS_PER_MS;
        uint64_t at = info.uptime_ms * NS_PER_MS, reference_ns, reference;
        if (clock_hypervisor_at(at, &reference_ns)) {
            if ((reference = clock_page_of(task_process()))) {
                info.flags = N_CLOCK_HV_REFERENCE;
                info.reference_page = reference;
                info.reference_sample_100ns = reference_ns / 100;
            }
        } else if (clock_tsc_at(at, &info.tsc_sample)) {
            info.flags = N_CLOCK_USER_TSC;
            info.tsc_hz = clock_tsc_hz();
        }
        ret = copy_to_user((void *)a, &info, sizeof info);
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
    case SYS_THREAD_STACK_BOUNDS: {
        /* Main stacks grow on demand within the VM's reserved interval.
           Native threads instead own their scheduler-allocated mapping. */
        uintptr_t low = current_task->user_stack_size ? current_task->user_stack : USER_STACK_TOP - USER_STACK_MAX;
        uintptr_t high = current_task->user_stack_size ? current_task->user_stack + current_task->user_stack_size : USER_STACK_TOP;
        ret = copy_to_user((void *)a, &low, sizeof low);
        if (!ret) ret = copy_to_user((void *)b, &high, sizeof high);
        break;
    }
    case SYS_WAIT_ADDRESS:
        if ((a & 3) || !user_ok((void *)a, sizeof(uint32_t))) ret = -N_ERR_BAD_ADDRESS;
        else ret = task_wait_address((volatile void *)a, (uint32_t)b, false, address_deadline((unsigned)c));
        break;
    case SYS_WAIT_ADDRESS64:
        if ((a & 7) || !user_ok((void *)a, sizeof(uint64_t))) ret = -N_ERR_BAD_ADDRESS;
        else ret = task_wait_address((volatile void *)a, b, true, address_deadline((unsigned)c));
        break;
    case SYS_WAKE_ADDRESS:
        if ((a & 3) || !user_ok((void *)a, sizeof(uint32_t))) ret = -N_ERR_BAD_ADDRESS;
        else ret = task_wake_address((volatile uint32_t *)a, (unsigned)b);
        break;
    case SYS_GPU_INFO: {
        struct n_gpu_info info;
        gpu_get_info(&info);
        ret = copy_to_user((void *)a, &info, sizeof info);
        break;
    }
    case SYS_GPU_RENDER: {
        struct n_gpu_render request;
        if (copy_from_user(&request, (const void *)a, sizeof request) < 0) { ret = -N_ERR_BAD_ADDRESS; break; }
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
        if (copy_from_user(&request, (const void *)a, sizeof request) < 0) { ret = -N_ERR_BAD_ADDRESS; break; }
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
        if (copy_from_user(&request, (const void *)a, sizeof request) < 0) { ret = -N_ERR_BAD_ADDRESS; break; }
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
    case SYS_YIELD: sched_yield(); ret = 0; break;
    case SYS_POWER:
        /* the power thread ends this app with the others */
        pm_request(a == POWER_REBOOT ? PM_REBOOT : PM_POWER_OFF, false);
        while (!current_task->killed) sleep_until(CLOCK_NEVER); /* the kill wakes it */
        ret = -N_ERR_STOPPED;
        break;
    case SYS_SYSTEM_LOG: ret = sys_system_log((char *)a, b); break;
    case SYS_PCILIST:
    {
        int room = (int)MIN(b, (uint64_t)pci_count());
        struct n_pciinfo *list = room ? kzalloc((size_t)room * sizeof *list) : NULL;
        if (room && !list) { ret = -N_ERR_NO_MEMORY; break; }
        int n = pci_list(list, room);
        ret = copy_to_user((void *)a, list, (size_t)n * sizeof *list);
        if (!ret) ret = n;
        kfree(list);
        break;
    }
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
    case SYS_AUDIO_OUTPUT_WATCH: ret = audio_output_watch(); break;
    case SYS_RANDOM: ret = sys_random((void *)a, b); break;
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
