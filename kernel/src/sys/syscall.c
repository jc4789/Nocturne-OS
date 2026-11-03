/* System call dispatch (SYSCALL, arch/isr.asm). */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/smp.h"
#include "sys/address_wait.h"
#include "sys/process.h"
#include "sys/sched.h"
#include "sys/syscall.h"
#include "sys/proc.h"
#include "sys/authority.h"
#include "dev/refclock.h"
#include "sys/clock.h"
#include "sys/thread.h"
#include "sys/wait.h"
#include "sys/events.h"
#include "sys/log.h"
#include "sys/object.h"
#include "sys/channel.h"
#include "sys/stream.h"
#include "sys/watch.h"
#include "fs/store.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "mm/shared.h"
#include "mm/space.h"
#include "dev/fb.h"
#include "dev/gpu.h"
#include "dev/audio.h"
#include "dev/entropy.h"
#include "dev/pci.h"
#include "dev/pciuser.h"
#include "arch/pat.h"
#include "gui/wm.h"
#include "gui/screenmode.h"
#include "display_abi.h"
#include "net/net.h"
#include "dev/pm.h"
#include "abi.h"
#include "authority_abi.h"
#include "settings_abi.h"
#include "sys/settings.h"
#include "sys/entry.h"
/* sys/object.c: whether the handle is a capability that gives want. */

_Static_assert(PAGE_SIZE == N_PAGE_SIZE, "native mapping granule ABI");

_Static_assert(sizeof(struct n_gpu_blit) == 32, "GPU blit ABI");
_Static_assert(SYS_WATCH_TAKE == 207 && SYS_AUDIO_DEVICE_WATCH < SYS_MAX, "every system call number is below SYS_MAX");
_Static_assert(sizeof(struct n_pager_fault) == 16 && sizeof(struct n_pager_supply) == 32, "pager ABI");

int64_t net_syscall(int num, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e);

/* ---- user memory access ---- */

size_t user_copy(void *dst, const void *src, size_t n); /* arch/isr.asm: the bytes it left */

/* Is [p, p+n) user memory the process may read (or, with write, also write)? Missing pages are
   made present (a stack that grows, a reserved or shared page), and a pager's page may be given
   write access. A program page read from its document lets the CPU go while the disk works: a
   sibling may unmap what was checked before it, so the check starts over. */
static bool user_access(const void *p, size_t n, bool write) {
    uint64_t a = (uint64_t)p;
    if (n == 0) return true;
    if (a >= USER_TOP || a + n > USER_TOP || a + n < a) return false;
    struct process *owner = process_current();
    uint64_t pg = ALIGN_DOWN(a, PAGE_SIZE);
    while (pg < a + n) {
        uint64_t pte = vmm_get_pte(owner->vmm_space, pg);
        if (!(pte & PTE_P)) {
            if (pg >= USER_STACK_TOP - USER_STACK_MAX && pg < USER_STACK_TOP) {
                if (vmm_user_alloc(owner->vmm_space, pg, PAGE_SIZE, VM_W) < 0) return false;
                pg += PAGE_SIZE;
                continue;
            }
            bool slept = false;
            if (!shared_fault(pg, write, &slept)) return false;
            if (slept) {
                pg = ALIGN_DOWN(a, PAGE_SIZE);
                continue;
            }
            pte = vmm_get_pte(owner->vmm_space, pg);
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
        if (thread_current()->killed) { r = -N_ERR_STOPPED; break; }
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

struct proc_list {
    struct n_procinfo *list;
    size_t n, room;
    struct n_identity who; /* 一覧を求めた者：触れてよいアプリとシステムのアプリだけが見える */
};

/* thread_walk: t's line, while there is room. */
static void proclist_one(struct thread *t, void *arg) {
    struct proc_list *l = arg;
    if (l->n == l->room) return;
    struct process *p = t->process, *main = process_of_main(t);
    if (p && p->identity.cls != N_CLASS_SYSTEM && !identity_may_reach(&l->who, &p->identity)) return;
    struct n_procinfo *info = &l->list[l->n++];
    info->pid = t->tid;
    /* an app's other thread is shown under its app; an app under the app that launched it */
    info->ppid = !p ? 0 : !main ? p->pid : p->parent ? p->parent->pid : 0;
    info->state = t->sched.state;
    info->is_user = p != NULL;
    strlcpy(info->name, t->name, sizeof info->name);
    info->cpu_ns = sched_thread_cpu_ns(t);
    info->mem_kb = main ? process_pages(main) * 4 : 0;
    info->start_ns = t->start_ns;
    if (p) {
        info->app = p->pid;
        info->user = p->identity.user;
        info->session = p->identity.session;
        info->cls = p->identity.cls;
    }
}

static int64_t sys_proclist(struct n_procinfo *out, int max) {
    if (max < 0) return -N_ERR_BAD_ADDRESS;
    /* the copy may sleep: the list is read into the kernel first */
    struct proc_list l = {.room = MIN((size_t)max, thread_count()), .who = *identity_current()};
    if (!l.room) return 0;
    if (!(l.list = kzalloc(l.room * sizeof *l.list))) return -N_ERR_NO_MEMORY;
    thread_walk(proclist_one, &l);
    int r = copy_to_user(out, l.list, l.n * sizeof *l.list);
    kfree(l.list);
    return r < 0 ? r : (int64_t)l.n;
}


static int64_t sys_sysinfo(struct n_sysinfo *usi) {
    struct n_sysinfo info = {0}, *si = &info;
    si->total_mem = pmm_total_pages() * PAGE_SIZE;
    /* What an app can have: idle programs give their frames to one that asks (frames_free), and
       the frames set aside for the APs' first touches are no one's yet. */
    si->free_mem = (pmm_free_pages() + programs_idle()) * PAGE_SIZE;
    si->heap_used = heap_used_bytes();
    si->uptime_ns = clock_now_ns();
    si->ntasks = thread_count();
    si->fb_w = fb.width;
    si->fb_h = fb.height;
    if (wm_running()) wm_screen_size(&si->fb_w, &si->fb_h); /* the desktop may be a remote viewer's size */
    strlcpy(si->cpu, cpu_features.brand, sizeof si->cpu);
    strlcpy(si->os, OS_NAME " " OS_VERSION, sizeof si->os);
    return copy_to_user(usi, si, sizeof *si);
}

/* The caller's handle number for a new object (r: the result of the call that opened it). */
static int64_t new_handle(int64_t r,struct handle *h) {
    return r<0?r:handle_install(h);
}

/* An output's buffers a screen is drawn into, or given back (dev/pciuser.c). */
static int64_t sys_pci_give_screen(int h,uint32_t output,const struct n_screen_buffers *ub) {
    struct n_screen_buffers b;
    if(ub && copy_from_user(&b,ub,sizeof b))return -N_ERR_BAD_ADDRESS;
    struct handle *pci=handle_get(h);
    if(!pci)return -N_ERR_NO_HANDLE;
    int64_t r=pci_user_give_screen(pci,output,ub?&b:NULL);
    handle_close(pci);
    return r;
}
/* The flips the screens ask of a display driver's outputs (dev/pciuser.c), copied out after. */
#define SCREEN_FLIPS_MAX 64u
static int64_t sys_pci_screen_flips(int h,struct n_screen_flip *uout,size_t max,uint64_t deadline) {
    if(!max)return -N_ERR_INVALID;
    max=MIN(max,(size_t)SCREEN_FLIPS_MAX);
    if(!user_ok_w(uout,max*sizeof *uout))return -N_ERR_BAD_ADDRESS;
    struct n_screen_flip flips[SCREEN_FLIPS_MAX];
    struct handle *pci=handle_get(h);
    if(!pci)return -N_ERR_NO_HANDLE;
    int64_t r=pci_user_screen_flips(pci,flips,max,deadline);
    handle_close(pci);
    if(r>0 && copy_to_user(uout,flips,(size_t)r*sizeof *flips))r=-N_ERR_BAD_ADDRESS;
    return r;
}
/* A vertical blank of a display driver's output (dev/pciuser.c). */
static int64_t sys_pci_screen_vblank(int h,const struct n_screen_vblank *uv) {
    struct n_screen_vblank v;
    if(copy_from_user(&v,uv,sizeof v))return -N_ERR_BAD_ADDRESS;
    struct handle *pci=handle_get(h);
    if(!pci)return -N_ERR_NO_HANDLE;
    int64_t r=pci_user_screen_vblank(pci,&v);
    handle_close(pci);
    return r;
}
/* A surface a display driver may show in an output's buffers' place, for the panic screen. */
static int64_t sys_pci_screen_image(int h,uint32_t output,int surface,const struct n_scanout *udesc) {
    struct n_scanout desc;
    if(copy_from_user(&desc,udesc,sizeof desc))return -N_ERR_BAD_ADDRESS;
    struct handle *pci=handle_get(h);
    if(!pci)return -N_ERR_NO_HANDLE;
    struct handle *s=handle_get(surface);
    int64_t r=s?pci_user_screen_image(pci,output,s,&desc):-N_ERR_NO_HANDLE;
    handle_close(s);handle_close(pci);
    return r;
}
/* What an output's screen should show (dev/pciuser.c, gui/wm.c). The state is copied out even
   when the time ran out. */
static int64_t sys_pci_screen_state(int h,uint32_t output,uint64_t seen,struct n_screen_state *uout,uint64_t deadline) {
    if(!user_ok_w(uout,sizeof *uout))return -N_ERR_BAD_ADDRESS;
    struct handle *pci=handle_get(h);
    if(!pci)return -N_ERR_NO_HANDLE;
    struct n_screen_state *st=kmalloc(sizeof *st);
    int64_t r=st?pci_user_screen_state(pci,output,seen,st,deadline):-N_ERR_NO_MEMORY;
    handle_close(pci);
    if((r==0||r==-N_ERR_TIMEOUT)&&copy_to_user(uout,st,sizeof *st))r=-N_ERR_BAD_ADDRESS;
    kfree(st);
    return r;
}
/* The monitor on a display driver's output and the modes it can show there (dev/pciuser.c,
   gui/screenmode.c). */
static int64_t sys_pci_screen_modes(int h,uint32_t output,const struct n_screen_modes *um) {
    struct n_screen_modes *m=kmalloc(sizeof *m);
    if(!m)return -N_ERR_NO_MEMORY;
    int64_t r=copy_from_user(m,um,sizeof *m)?-N_ERR_BAD_ADDRESS:0;
    struct handle *pci=r?NULL:handle_get(h);
    if(!r)r=pci?pci_user_screen_modes(pci,output,m):-N_ERR_NO_HANDLE;
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
/* ---- shared memory and the address space ----
   The calls' arguments and copies; the work is mm's (mm/shared.h, mm/space.h). */

/* A claimed function's memory BAR. */
static int64_t sys_pci_bar(int h,uint32_t bar) {
    struct handle *pci=handle_get(h),*range=NULL;
    if(!pci)return -N_ERR_NO_HANDLE;
    int64_t r=shared_bar(pci,bar,&range);
    handle_close(pci);
    return new_handle(r,range);
}
static int64_t sys_dma_create(int h,size_t bytes,uint32_t flags) {
    struct handle *pci=handle_get(h),*dma=NULL;
    if(!pci)return -N_ERR_NO_HANDLE;
    int64_t r=shared_dma_create(pci,bytes,flags,&dma);
    handle_close(pci);
    return new_handle(r,dma);
}
/* Bus addresses of a DMA object's (or a device range's) pages, also through views. */
static int64_t sys_dma_addresses(int h,size_t first,uint64_t *uout,size_t count) {
    struct handle *object=handle_get(h);
    if(!object)return -N_ERR_NO_HANDLE;
    int64_t pages=shared_dma_pages(object,first,count),r=pages<0?pages:0;
    for(size_t done=0;r>=0 && done<(size_t)pages;){
        uint64_t piece[64];size_t k=MIN((size_t)pages-done,ARRAY_SIZE(piece));
        shared_dma_frames(object,first+done,piece,k);
        if(copy_to_user(uout+done,piece,k*sizeof *piece)<0){r=-N_ERR_BAD_ADDRESS;break;}
        done+=k;r=(int64_t)done;
    }
    handle_close(object);return r;
}
static int64_t sys_shared_create(size_t bytes,bool demand) {
    struct handle *h=NULL;
    int64_t r=shared_create(bytes,demand,&h);
    return new_handle(r,h);
}
static int64_t sys_shared_view(int h,size_t offset,size_t bytes,uint32_t cache,uint32_t maximum) {
    struct handle *f=handle_get(h),*view=NULL;
    if(!f)return -N_ERR_NO_HANDLE;
    int64_t r=shared_view(f,offset,bytes,cache,maximum,&view);
    handle_close(f);
    return new_handle(r,view);
}
static int64_t sys_shared_compose(const struct n_shared_range *uranges,size_t count,uint32_t access) {
    if(!count || (access!=N_MEMORY_READ && access!=N_MEMORY_READ_WRITE))return -N_ERR_INVALID;
    if(count>SIZE_MAX/sizeof(struct shared_slice) || count>SIZE_MAX/sizeof(*uranges))return -N_ERR_TOO_LARGE;
    if(count>(UINTPTR_MAX-(uintptr_t)uranges)/sizeof(*uranges))return -N_ERR_BAD_ADDRESS;
    struct shared_slice *slices=kzalloc(count*sizeof *slices);
    if(!slices)return -N_ERR_NO_MEMORY;
    int64_t r=0;
    for(size_t i=0;r>=0 && i<count;i++){
        struct n_shared_range range;
        if((r=copy_from_user(&range,&uranges[i],sizeof range))<0)break;
        if(range.reserved || !range.bytes || (range.offset|range.bytes)&(PAGE_SIZE-1))r=-N_ERR_INVALID;
        else if(!(slices[i].handle=handle_get(range.shared)))r=-N_ERR_NO_HANDLE;
        slices[i].offset=range.offset;slices[i].bytes=range.bytes;
    }
    if(r>=0){
        struct handle *composite=NULL;
        r=shared_compose(slices,count,access,&composite);
        r=new_handle(r,composite);
    }
    for(size_t i=0;i<count;i++)handle_close(slices[i].handle);
    kfree(slices);return r;
}
static int64_t sys_shared_revoke(int h) {
    struct handle *f=handle_get(h);
    if(!f)return -N_ERR_NO_HANDLE;
    int64_t r=shared_revoke(f);
    handle_close(f);return r;
}
/* Maps a shared object the caller holds a handle to (its own, or one received as a channel
   attachment): there is no reaching into another app's handles. */
static int64_t sys_shared_map(int h,size_t bytes,int access) {
    if(!bytes || bytes>SIZE_MAX-(PAGE_SIZE-1) || (access!=N_MEMORY_READ && access!=N_MEMORY_READ_WRITE))return -N_ERR_INVALID;
    struct handle *f=handle_get(h);
    if(!f)return -N_ERR_NO_HANDLE;
    int64_t r=shared_map(f,bytes,access);
    handle_close(f);return r;
}
static int64_t sys_pager_create(size_t bytes,int32_t *umemory) {
    struct handle *memory,*pager;
    int64_t r=pager_create(bytes,&memory,&pager);
    if(r<0)return r;
    int number=handle_install(memory);
    if(number<0){handle_close(pager);return number;}
    if(copy_to_user(umemory,&number,sizeof number)<0){handle_remove(number);handle_close(pager);return -N_ERR_BAD_ADDRESS;}
    int result=handle_install(pager);
    if(result<0)handle_remove(number);
    return result;
}
/* The pager behind handle h, for its creator only: *out holds the handle, or the error. */
static int64_t pager_handle(int h,struct handle **out) {
    struct handle *f=handle_get(h);
    if(!f)return -N_ERR_NO_HANDLE;
    int64_t r=pager_allowed(f);
    if(r<0)handle_close(f);
    else *out=f;
    return r;
}
static int64_t sys_pager_take(int h,struct n_pager_fault *uout,size_t max,uint64_t deadline) {
    struct handle *f;
    int64_t r=pager_handle(h,&f);
    if(r<0)return r;
    struct n_pager_fault out[32];uint64_t ids[32];
    r=pager_take(f,out,ids,MIN(max,ARRAY_SIZE(out)),deadline);
    if(r>0 && copy_to_user(uout,out,(size_t)r*sizeof *out)<0){
        pager_untake(f,ids,(size_t)r);
        r=-N_ERR_BAD_ADDRESS;
    }
    handle_close(f);return r;
}
static int64_t sys_pager_supply(int h,const struct n_pager_supply *uruns,size_t count) {
    struct handle *f;
    int64_t r=pager_handle(h,&f);
    if(r<0)return r;
    if(!count)r=-N_ERR_INVALID;
    for(size_t i=0;r>=0 && i<count;i++){
        struct n_pager_supply run;
        if(copy_from_user(&run,&uruns[i],sizeof run)<0)r=-N_ERR_BAD_ADDRESS;
        else r=pager_supply(f,&run);
    }
    handle_close(f);return r<0?r:0;
}
static int64_t sys_pager_revoke(int h,uint64_t offset,uint64_t bytes,uint32_t access) {
    struct handle *f;
    int64_t r=pager_handle(h,&f);
    if(r<0)return r;
    r=pager_revoke(f,offset,bytes,access);
    handle_close(f);return r;
}
static int64_t sys_pager_fail(int h,uint64_t offset,uint64_t bytes) {
    struct handle *f;
    int64_t r=pager_handle(h,&f);
    if(r<0)return r;
    r=pager_fail(f,offset,bytes);
    handle_close(f);return r;
}

/* The system log's newest text, at most n bytes of it (no more than the rings hold). */
static int64_t sys_system_log(char *buf, size_t n) {
    n = MIN(n, log_capacity());
    char *text = kmalloc(n ? n : 1);
    struct log_cursor *c = log_cursor_new(0);
    if (!text || !c) {
        kfree(text);
        log_cursor_free(c);
        return -N_ERR_NO_MEMORY;
    }
    size_t got = log_newest(c, text, n);
    log_cursor_free(c);
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
        if(thread_current()->killed)return -N_ERR_STOPPED;
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
        if(thread_current()->killed)return -N_ERR_STOPPED;
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

/* A private region (fs/store.h): whoever holds the right, by its number and its user's (0: the
   machine's). access 0 is N_DOCUMENT_EDIT. */
static int64_t sys_store_private_open(int cap, uint64_t region, uint64_t user, int access) {
    int r = cap_check(cap, N_CAP_STORE_PRIVATE);
    if (r < 0) return r;
    if (region > UINT32_MAX || user > UINT32_MAX) return -N_ERR_INVALID;
    struct handle *h;
    r = store_private_open((uint32_t)region, (uint32_t)user, access ? access : N_DOCUMENT_EDIT, &h);
    return r < 0 ? r : handle_install(h);
}

static int64_t sys_entry_set_owner(const char *upath, uint32_t owner) {
    char *path;
    int r = user_string(upath, &path);
    if (r < 0) return r;
    r = store_entry_set_owner(path, owner, true);
    kfree(path);
    return r;
}

static int64_t sys_document_io(int h, uint64_t off, void *buf, size_t n, bool write) {
    struct handle *d = handle_get(h);
    if (!d) return -N_ERR_NO_HANDLE;
    int64_t r = write ? store_document_write(d, off, buf, n, true) : store_document_read(d, off, buf, n, true);
    handle_close(d);
    return r;
}

static int64_t sys_image_map(int h, struct n_image *uout) {
    struct handle *doc = handle_get(h);
    if (!doc) return -N_ERR_NO_HANDLE;
    struct n_image image;
    int64_t r = handle_kind(doc) != N_KIND_DOCUMENT ? -N_ERR_WRONG_KIND : doc->rights & HANDLE_WRITE ? -N_ERR_DENIED :
                proc_image_map(doc, &image);
    handle_close(doc);
    if (r >= 0 && copy_to_user(uout, &image, sizeof image)) r = -N_ERR_BAD_ADDRESS;
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
        /* 文書、共有の頁、能力、channel の端だけ（端の輪は message_put が断る） */
        else if (handle_kind(att) != N_KIND_DOCUMENT && handle_kind(att) != N_KIND_SHARED &&
                 handle_kind(att) != N_KIND_CAPABILITY && !message_channel_is(att))
            r = -N_ERR_WRONG_KIND;
    }
    if (r < 0) {
    } else if (message_channel_is(f)) {
        r = message_channel_send(f, udata, n, att, CLOCK_NEVER);
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
        r = message_channel_receive(f, ubuf, cap, wait ? CLOCK_NEVER : 0, peek, discard, &att, &message.sender_app);
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
            if (message_channel_is(att)) message_channel_handed(att, identity_current());
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

static int64_t sys_channel_peer(int h, struct n_identity *out) {
    struct handle *f = handle_get(h);
    if (!f) return -N_ERR_NO_HANDLE;
    if (!message_channel_is(f)) {
        handle_close(f);
        return -N_ERR_WRONG_KIND;
    }
    struct n_identity who;
    message_channel_peer(f, &who);
    handle_close(f);
    return copy_to_user(out, &who, sizeof who);
}

static int64_t sys_app_launch(const char *upath, const struct n_launch *ul) {
    char *path;
    int r = user_string(upath, &path);
    if (r < 0) return r;
    struct n_launch l = {0};
    char **argv = NULL, **environment = NULL, *folder = NULL;
    int32_t *terminals = NULL;
    struct handle **handles = NULL, *app = NULL;
    size_t argc = 0, bytes = 0;
    int64_t ret = copy_from_user(&l, ul, sizeof l);
    const size_t limit = USER_STACK_MAX / 2; /* what the new app's stack can hold */
    if (ret < 0) goto out;
    if ((l.flags & ~N_LAUNCH_SAME_PROGRAM) || l.reserved) { ret = -N_ERR_INVALID; goto out; }
    struct n_identity identity;
    if ((r = identity_for_child(&l, path, &identity)) < 0) { ret = r; goto out; }
    if (l.argument_count > limit / 8 || l.environment_count > limit / 8 ||
        l.handle_count > 4096 || l.terminal_count > 4096) {
        l = (struct n_launch){0};
        ret = -N_ERR_TOO_MANY_ARGUMENTS;
        goto out;
    }
    argc = l.argument_count ? l.argument_count : 1;
    argv = kzalloc(argc * sizeof *argv);
    environment = kzalloc((l.environment_count + 1) * sizeof *environment);
    handles = kzalloc((l.handle_count + 1) * sizeof *handles);
    terminals = kzalloc((l.terminal_count + 1) * sizeof *terminals);
    if (!argv || !environment || !handles || !terminals) { ret = -N_ERR_NO_MEMORY; goto out; }
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
    for (size_t i = 0; i < l.environment_count; i++) {
        const char *us;
        if ((r = copy_from_user(&us, &l.environment[i], sizeof us)) < 0) { ret = r; goto out; }
        if ((r = user_string(us, &environment[i])) < 0) { ret = r; goto out; }
        bytes += strlen(environment[i]) + 1;
        if (!strchr(environment[i], '=')) { ret = -N_ERR_INVALID; goto out; }
    }
    if (l.folder) {
        if ((r = user_string(l.folder, &folder)) < 0) { ret = r; goto out; }
        bytes += strlen(folder) + 1;
    }
    if (l.terminal_count &&
        (r = copy_from_user(terminals, l.terminals, l.terminal_count * sizeof *terminals)) < 0) { ret = r; goto out; }
    if (bytes > limit) { ret = -N_ERR_TOO_MANY_ARGUMENTS; goto out; }
    for (size_t i = 0; i < l.handle_count; i++) {
        int32_t h;
        if ((r = copy_from_user(&h, &l.handles[i], sizeof h)) < 0) { ret = r; goto out; }
        if (h < 0) continue;
        if (!(handles[i] = handle_get(h))) { ret = -N_ERR_NO_HANDLE; goto out; }
    }
    /* 0, 1 and 2 are a program's input, output and log: what the launcher does not give reads and
       writes nothing, and no later handle of the program takes those numbers. */
    if (l.handle_count < 3) {
        struct handle **room = kzalloc(3 * sizeof *room);
        if (!room) { ret = -N_ERR_NO_MEMORY; goto out; }
        for (size_t i = 0; i < l.handle_count; i++) room[i] = handles[i];
        kfree(handles);
        handles = room;
        l.handle_count = 3;
    }
    for (size_t i = 0; i < 3; i++)
        if (!handles[i] && !(handles[i] = stream_null_open())) { ret = -N_ERR_NO_MEMORY; goto out; }
    if ((r = proc_app_create(&app)) < 0) { ret = r; goto out; }
    struct proc_launch launch = {.argc = (int)argc, .argv = argv, .environment_count = (int)l.environment_count,
                                 .environment = environment, .folder = folder, .terminals = terminals,
                                 .terminal_count = l.terminal_count, .interrupts = l.interrupts,
                                 .handles = handles, .handle_count = l.handle_count,
                                 .parent = process_current(), .app = app->object, .identity = identity};
    r = proc_launch(path, &launch);
    if (r < 0) ret = r;
    else { ret = handle_install(app); app = NULL; }
out:
    handle_close(app);
    if (argv) for (size_t i = 0; i < argc; i++) kfree(argv[i]);
    if (environment) for (size_t i = 0; i < l.environment_count; i++) kfree(environment[i]);
    if (handles) for (size_t i = 0; i < l.handle_count; i++) handle_close(handles[i]);
    kfree(argv);
    kfree(environment);
    kfree(folder);
    kfree(terminals);
    kfree(handles);
    kfree(path);
    return ret;
}

static int64_t sys_app_result(int h, uint64_t deadline, int *ustatus) {
    int64_t r;
    struct handle *app = handle_get_as(h, proc_app_is, &r);
    if (!app) return r;
    int status = 0;
    uint64_t asked = clock_now_ns();
    r = proc_app_result(app, deadline, &status);
    if (r == -N_ERR_TIMEOUT && deadline <= asked) r = -N_ERR_NOT_READY; /* a look that does not wait */
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
    if (flags & N_APP_STOP_TREE) process_kill_tree(pid, true);
    else process_kill(pid);
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

/* A handle whose messages are added to the system log (which the serial port and the boot screen
   show). 本文を受けるのは ADMIN 以上の開いた handle だけ。 */
static int64_t sys_system_log_open(void) {
    struct handle *h = stream_system_log_open(identity_at_least(N_CLASS_ADMIN));
    return h ? handle_install(h) : -N_ERR_NO_MEMORY;
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
    struct event_source *spare = kmalloc(events_watch_size());
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

/* A timer due at a moment on the monotonic clock, then every period ns on that grid (0: once). */
static int64_t sys_events_timer(int q, uint64_t due, uint64_t period, uint64_t token) {
    int64_t r;
    struct handle *queue = handle_get_as(q, events_is, &r);
    if (!queue) return r;
    r = events_timer(queue, due, period, token);
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

static int64_t sys_events_wait(int q, struct n_event *uout, size_t max, uint64_t deadline) {
    int64_t r;
    struct handle *queue = handle_get_as(q, events_is, &r);
    if (!queue) return r;
    r = events_wait(queue, uout, max, deadline);
    handle_close(queue);
    return r;
}

void syscall_dispatch(struct regs *r) {
    /* A thread a sibling killed does not start a call it may then wait in. */
    struct thread *self = thread_current();
    if (self->killed) thread_exit(self->kill_code);
    sti();
    if (!entry_came_from(self->process, r->rip, r->rax)) {
        /* The number belongs to the entry image: a program that holds its own SYSCALL ends. */
        kprintf("[crash] pid %d (%s): call %lu entered the kernel at rip=%p, not by its slot of the entry image\n",
                self->tid, self->name, r->rax, (void *)r->rip);
        process_exit(128 + 13);
    }
    uint64_t a = r->rdi, b = r->rsi, c = r->rdx, d = r->r10, e = r->r8;
    int64_t ret = -N_ERR_UNSUPPORTED;
    switch (r->rax) {
    case SYS_EXIT: process_exit((int)a);
    case SYS_AUDIO_DEVICE_RUN: ret = audio_syscall_run((int)a, b != 0); break;
    case SYS_SLEEP: sleep_until(a); ret = 0; break;
    case SYS_SHARED_CREATE: ret = sys_shared_create(a,false); break;
    case SYS_SHARED_RESERVE: ret = sys_shared_create(a,true); break;
    case SYS_SHARED_MAP: ret = sys_shared_map((int)a,b,(int)c); break;
    case SYS_SHARED_VIEW: ret = sys_shared_view((int)a,b,c,(uint32_t)d,(uint32_t)e); break;
    case SYS_SHARED_COMPOSE: ret = sys_shared_compose((const struct n_shared_range *)a,b,(uint32_t)c); break;
    case SYS_SHARED_REVOKE: ret = sys_shared_revoke((int)a); break;
    case SYS_PCI_CLAIM: {
        struct handle *claim = NULL;
        ret = cap_check((int)a, N_CAP_PCI_CLAIM);
        if (ret < 0) break;
        ret = pci_user_claim((uint32_t)b,&claim);
        ret = new_handle(ret,claim);
        break;
    }
    case SYS_PCI_BAR: ret = sys_pci_bar((int)a,(uint32_t)b); break;
    case SYS_PCI_GIVE_SCREEN: ret = sys_pci_give_screen((int)a,(uint32_t)b,(const struct n_screen_buffers *)c); break;
    case SYS_PCI_SCREEN_FLIPS: ret = sys_pci_screen_flips((int)a,(struct n_screen_flip *)b,(size_t)c,d); break;
    case SYS_PCI_SCREEN_VBLANK: ret = sys_pci_screen_vblank((int)a,(const struct n_screen_vblank *)b); break;
    case SYS_PCI_ACPI_EVAL: ret = sys_pci_acpi_eval((int)a,(const struct n_acpi_eval *)b,(void *)c,d); break;
    case SYS_PCI_SCREEN_STATE: ret = sys_pci_screen_state((int)a,(uint32_t)b,c,(struct n_screen_state *)d,e); break;
    case SYS_PCI_SCREEN_IMAGE: ret = sys_pci_screen_image((int)a,(uint32_t)b,(int)c,(const struct n_scanout *)d); break;
    case SYS_PCI_SCREEN_MODES: ret = sys_pci_screen_modes((int)a,(uint32_t)b,(const struct n_screen_modes *)c); break;
    case SYS_DMA_CREATE: ret = sys_dma_create((int)a,b,(uint32_t)c); break;
    case SYS_DMA_ADDRESSES: ret = sys_dma_addresses((int)a,b,(uint64_t *)c,d); break;
    case SYS_PCI_INFO: case SYS_PCI_CONFIG_READ: case SYS_PCI_CONFIG_WRITE:
    case SYS_PCI_INTERRUPTS: case SYS_PCI_INTERRUPT_TAKE: case SYS_PCI_HOLD_SCREEN:
    case SYS_PCI_ACPI_NOTIFY_TAKE: case SYS_PCI_SCREEN_IMAGE_END: {
        struct handle *f = handle_get((int)a);
        if (!f) { ret = -N_ERR_NO_HANDLE; break; }
        if (r->rax == SYS_PCI_INFO) {
            struct n_pci_function info;
            ret = pci_user_info(f, &info);
            if (!ret) ret = copy_to_user((void *)b, &info, sizeof info);
        } else if (r->rax == SYS_PCI_CONFIG_READ) ret = pci_user_config_read(f,b,(uint32_t)c,(uint32_t)d);
        else if (r->rax == SYS_PCI_CONFIG_WRITE) ret = pci_user_config_write(f,(uint32_t)b,(uint32_t)c,(uint32_t)d);
        else if (r->rax == SYS_PCI_INTERRUPTS) ret = pci_user_interrupts(f,(uint32_t)b);
        else if (r->rax == SYS_PCI_HOLD_SCREEN) ret = pci_user_hold_screen(f, b != 0);
        else if (r->rax == SYS_PCI_ACPI_NOTIFY_TAKE) ret = pci_user_acpi_notify_take(f, b);
        else if (r->rax == SYS_PCI_SCREEN_IMAGE_END) ret = pci_user_screen_image_end(f, (int64_t)b);
        else ret = pci_user_interrupt_take(f, b, (uint64_t *)c, (uint32_t)d);
        handle_close(f);
        break;
    }
    case SYS_MEMORY_RESERVE: ret = memory_reserve(a, (int)b); break;
    case SYS_MEMORY_RELEASE: ret = memory_release(a, b); break;
    case SYS_MEMORY_PROTECT: ret = memory_protect(a, b, (int)c); break;
    case SYS_IDENTITY_SELF: ret = sys_identity_self((struct n_identity *)a); break;
    case SYS_CHANNEL_PEER: ret = sys_channel_peer((int)a, (struct n_identity *)b); break;
    case SYS_CAP_DERIVE: ret = sys_cap_derive((int)a, b); break;
    case SYS_CAP_KIND: ret = sys_cap_kind((int)a); break;
    case SYS_CAP_FIND: ret = sys_cap_find(a); break;
    case SYS_SERVICE_REGISTER: ret = sys_service_register((int)a); break;
    case SYS_SERVICE_CONNECT: ret = sys_service_connect((const char *)a, b); break;
    case SYS_PROCLIST: ret = sys_proclist((struct n_procinfo *)a, (int)b); break;
    case SYS_SYSINFO: ret = sys_sysinfo((struct n_sysinfo *)a); break;
    case SYS_CPU_INFO: {
        struct n_cpuinfo info;
        smp_get_info(&info);
        ret = copy_to_user((void *)a, &info, sizeof info);
        break;
    }
    case SYS_THREAD_CREATE:
        if (!user_ok((void *)a, 1) || !user_ok((void *)c, 1) ||
            (vmm_get_pte(self->process->vmm_space, a) & PTE_NX) ||
            (vmm_get_pte(self->process->vmm_space, c) & PTE_NX)) ret = -N_ERR_BAD_ADDRESS;
        else ret = thread_create(c, a, b, d);
        break;
    case SYS_THREAD_JOIN: ret = thread_join((int)a); break;
    case SYS_THREAD_DETACH: ret = thread_detach((int)a); break;
    case SYS_THREAD_EXIT: thread_exit(0);
    case SYS_THREAD_ID: ret = self->tid; break;
    case SYS_THREAD_SCHEDULE: ret = thread_schedule((int)a, (uint32_t)b, c); break;
    case SYS_APP_CPU_TIME: ret = (int64_t)process_cpu_ns(self->process); break;
    case SYS_THREAD_STACK_BOUNDS: {
        /* Main stacks grow on demand within the VM's reserved interval.
           Other threads instead own the stack thread_create reserved for them. */
        uintptr_t low = self->user_stack_size ? self->user_stack : USER_STACK_TOP - USER_STACK_MAX;
        uintptr_t high = self->user_stack_size ? self->user_stack + self->user_stack_size : USER_STACK_TOP;
        ret = copy_to_user((void *)a, &low, sizeof low);
        if (!ret) ret = copy_to_user((void *)b, &high, sizeof high);
        break;
    }
    case SYS_WAIT_ADDRESS:
        if ((a & 3) || !user_ok((void *)a, sizeof(uint32_t))) ret = -N_ERR_BAD_ADDRESS;
        else ret = address_wait((volatile void *)a, (uint32_t)b, false, c);
        break;
    case SYS_WAIT_ADDRESS64:
        if ((a & 7) || !user_ok((void *)a, sizeof(uint64_t))) ret = -N_ERR_BAD_ADDRESS;
        else ret = address_wait((volatile void *)a, b, true, c);
        break;
    case SYS_WAKE_ADDRESS:
        if ((a & 3) || !user_ok((void *)a, sizeof(uint32_t))) ret = -N_ERR_BAD_ADDRESS;
        else ret = address_wake((volatile uint32_t *)a, (unsigned)b);
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
    case SYS_IMAGE_MAP: ret = sys_image_map((int)a, (struct n_image *)b); break;
    case SYS_SESSION_STOP:
        ret = cap_check((int)a, N_CAP_IDENTITY_GRANT);
        if (ret < 0) break;
        ret = !b || b > UINT32_MAX ? -N_ERR_INVALID : process_stop_session((uint32_t)b);
        break;
    case SYS_POWER:
        ret = cap_check((int)a, N_CAP_POWER);
        if (ret < 0) break;
        if (b > POWER_HIBERNATE || c) {
            ret = -N_ERR_INVALID;
            break;
        }
        pm_request((enum pm_action)b);
        if (b == POWER_SLEEP || b == POWER_HIBERNATE) {
            ret = 0;
            break;
        }
        /* the power thread ends this app with the others */
        while (!self->killed) sleep_until(CLOCK_NEVER); /* the kill wakes it */
        ret = -N_ERR_STOPPED;
        break;
    case SYS_POWER_STATE:
        ret = cap_check((int)a, N_CAP_SHELL);
        if (ret >= 0) ret = pm_state_syscall((void *)b, c);
        break;
    case SYS_POWER_WATCH:
        ret = cap_check((int)a, N_CAP_SHELL);
        if (ret >= 0) ret = pm_watch_syscall();
        break;
    case SYS_POWER_EVENTS:
        ret = cap_check((int)a, N_CAP_POWER);
        if (ret >= 0) ret = pm_events_syscall((void *)b, c);
        break;
    case SYS_POWER_EVENTS_WATCH:
        ret = cap_check((int)a, N_CAP_POWER);
        if (ret >= 0) ret = pm_events_watch_syscall();
        break;
    case SYS_POWER_SAY:
        ret = cap_check((int)a, N_CAP_POWER);
        if (ret >= 0) ret = pm_say_syscall((const void *)b, c);
        break;
    case SYS_POWER_WINDOWS: {
        ret = cap_check((int)a, N_CAP_POWER);
        if (ret < 0) break;
        char title[sizeof ((struct n_power_state *)0)->detail] = "";
        if (b) wm_close_windows();
        ret = wm_window_count(title, sizeof title);
        if (d && copy_to_user((void *)c, title, MIN((size_t)d, sizeof title))) ret = -N_ERR_BAD_ADDRESS;
        break;
    }
    case SYS_POWER_WINDOWS_WATCH:
        ret = cap_check((int)a, N_CAP_POWER);
        if (ret >= 0) ret = wm_windows_watch();
        break;
    case SYS_SCREEN_MODE_APPLY: {
        ret = cap_check((int)a, N_CAP_DISPLAY);
        if (ret < 0) break;
        struct n_screen_mode mode;
        ret = copy_from_user(&mode, (const void *)c, sizeof mode) ? -N_ERR_BAD_ADDRESS : screen_modes_apply((unsigned)b, &mode);
        break;
    }
    case SYS_SCREEN_ARRANGE: {
        ret = cap_check((int)a, N_CAP_DISPLAY);
        if (ret < 0) break;
        struct n_screen_arrange arrange;
        ret = copy_from_user(&arrange, (const void *)c, sizeof arrange) ? -N_ERR_BAD_ADDRESS : wm_screen_arrange((unsigned)b, &arrange);
        break;
    }
    case SYS_SYSTEM_LOG: ret = identity_at_least(N_CLASS_ADMIN) ? sys_system_log((char *)a, b) : -N_ERR_DENIED; break;
    case SYS_HAL_LIST: ret = hal_syscall_list((struct n_hal_device *)a, b); break;
    case SYS_HAL_WATCH: ret = hal_syscall_watch(); break;
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
    case SYS_APP_RESULT: ret = sys_app_result((int)a, b, (int *)c); break;
    case SYS_APP_STOP: ret = sys_app_stop((int)a, (int)b); break;
    case SYS_APP_SELF: ret = self->process->pid; break;
    case SYS_APP_PARENT: {
        uint64_t f = spin_lock(&thread_lock); /* the family's links */
        struct process *parent = self->process->parent;
        ret = parent ? parent->pid : 0;
        spin_unlock(&thread_lock, f);
        break;
    }
    case SYS_EVENTS_CREATE: ret = sys_events_create(); break;
    case SYS_EVENTS_WATCH: ret = sys_events_watch((int)a, (int)b, (uint32_t)c, d); break;
    case SYS_EVENTS_UNWATCH: ret = sys_events_unwatch((int)a, b); break;
    case SYS_EVENTS_WATCH_ADDRESS: ret = sys_events_watch_address((int)a, b, (uint32_t)c, d); break;
    case SYS_EVENTS_TIMER: ret = sys_events_timer((int)a, b, c, d); break;
    case SYS_EVENTS_WAIT: ret = sys_events_wait((int)a, (struct n_event *)b, c, d); break;
    case SYS_APP_OPEN: ret = sys_app_open((int64_t)a); break;
    case SYS_APP_ID: ret = sys_app_id((int)a); break;
    case SYS_SYSTEM_LOG_OPEN: ret = sys_system_log_open(); break;
    case SYS_AUDIO_DEVICE_OPEN: ret = audio_syscall_open((int)a, (uint32_t)b, (const struct n_audio_device_format *)c, (struct n_audio_device_ring *)d); break;
    case SYS_AUDIO_DEVICE_LIST: ret = audio_syscall_list((int)a, (struct n_audio_device *)b, c); break;
    case SYS_AUDIO_DEVICE_STATUS: ret = audio_syscall_status((int)a, (struct n_audio_device_status *)b); break;
    case SYS_AUDIO_DEVICE_WATCH: ret = audio_syscall_watch((int)a); break;
    case SYS_RANDOM: ret = sys_random((void *)a, b); break;
    case SYS_WATCH_TAKE: ret = watch_take((int)a); break;
    case SYS_ENTRY_WATCH: ret = sys_entry_watch((const char *)a, b); break;
    case SYS_PAGER_CREATE: ret = sys_pager_create(a, (int32_t *)b); break;
    case SYS_PAGER_TAKE: ret = sys_pager_take((int)a, (struct n_pager_fault *)b, c, d); break;
    case SYS_PAGER_SUPPLY: ret = sys_pager_supply((int)a, (const struct n_pager_supply *)b, c); break;
    case SYS_PAGER_REVOKE: ret = sys_pager_revoke((int)a, b, c, (uint32_t)d); break;
    case SYS_PAGER_FAIL: ret = sys_pager_fail((int)a, b, c); break;
    case SYS_STORE_PRIVATE_OPEN: ret = sys_store_private_open((int)a, b, c, (int)d); break;
    case SYS_SETTINGS_APPLY: ret = settings_syscall_apply((int)a, (uint32_t)b, (uint32_t)c, (const void *)d, e); break;
    case SYS_SETTINGS_EFFECTIVE: ret = settings_syscall_effective((uint32_t)a, (uint32_t)b, (void *)c, d); break;
    case SYS_SETTINGS_DESCRIBE: ret = settings_syscall_describe((uint32_t)a, (uint32_t)b, (void *)c); break;
    case SYS_SETTINGS_WATCH: ret = settings_watch((uint32_t)a); break;
    case SYS_CLOCK_WALL_STEP: ret = cap_check((int)a, N_CAP_CLOCK); if (ret >= 0) { clock_set_wall((int64_t)b); ret = 0; } break;
    case SYS_CLOCK_WALL_SLEW:
        ret = cap_check((int)a, N_CAP_CLOCK);
        if (ret >= 0) {
            clock_adjust_wall((int64_t)b, (uint32_t)c);
            ret = 0;
        }
        break;
    case SYS_RTC_WRITE: ret = cap_check((int)a, N_CAP_CLOCK); if (ret >= 0) ret = rtc_write((int64_t)b); break;
    case SYS_ENTRY_SET_OWNER: ret = sys_entry_set_owner((const char *)a, (uint32_t)b); break;
    default:
        if (r->rax >= SYS_WIN_CREATE && r->rax < SYS_NET_FIRST) ret = wm_syscall((int)r->rax, a, b, c, d, e);
        else if (r->rax >= SYS_NET_FIRST && r->rax < SYS_NET_LIMIT) ret = net_syscall((int)r->rax, a, b, c, d, e);
        break;
    }
    r->rax = (uint64_t)ret;
}
