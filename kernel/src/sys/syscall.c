/* System call dispatch (int 0x80). */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/smp.h"
#include "arch/hyperv.h"
#include "sys/sched.h"
#include "sys/syscall.h"
#include "sys/proc.h"
#include "fs/vfs.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "dev/timer.h"
#include "dev/fb.h"
#include "dev/gpu.h"
#include "dev/audio.h"
#include "gui/wm.h"
#include "abi.h"
_Static_assert(PAGE_SIZE == N_PAGE_SIZE, "native mapping granule ABI");

_Static_assert(sizeof(struct n_dirent) == sizeof(struct dirent), "dirent ABI");
_Static_assert(sizeof(struct n_stat) == sizeof(struct kstat), "stat ABI");
_Static_assert(sizeof(struct n_gpu_blit) == 32, "GPU blit ABI");

extern char cpu_brand[64];
void power_off(void);
void power_reboot(void);
int pci_list(struct n_pciinfo *out, int max);
int64_t net_syscall(int num, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e);

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
    if (!max) return -ENAMETOOLONG;
    size_t i = 0;
    while (i < max) {
        uint64_t address = (uint64_t)src + i;
        if (address < (uint64_t)src || !user_ok((const void *)address, 1)) return -EFAULT;
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
    return -ENAMETOOLONG;
}

static int user_path(const char *upath, char *out) {
    char tmp[PATH_MAX_LEN];
    int r = user_str(tmp, upath, sizeof tmp);
    if (r < 0) return r;
    char cwd[sizeof task_process()->cwd];
    uint64_t flags = irq_save();
    memcpy(cwd, task_process()->cwd, sizeof cwd);
    irq_restore(flags);
    return vfs_normalize(cwd, tmp, out);
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

static int64_t sys_read(int fd, void *buf, size_t n) {
    struct file *f = getfd_ref(fd);
    if (!f) return -EBADF;
    int64_t result = vfs_read_user(f, buf, n);
    vfs_close(f);
    return result;
}

static int64_t sys_write(int fd, const void *buf, size_t n) {
    struct file *f = getfd_ref(fd);
    if (!f) return -EBADF;
    int64_t result = vfs_write_user(f, buf, n);
    vfs_close(f);
    return result;
}

static int64_t sys_open(const char *upath, int flags) {
    char path[PATH_MAX_LEN];
    int r = user_path(upath, path);
    if (r < 0) return r;
    struct file *f;
    r = vfs_open(path, flags, &f);
    if (r < 0) return r;
    int fd = allocfd(f);
    if (fd < 0) vfs_close(f);
    return fd;
}

static int64_t sys_close(int fd) {
    uint64_t flags = irq_save();
    struct file *f = getfd(fd);
    if (!f) { irq_restore(flags); return -EBADF; }
    task_process()->fds[fd] = NULL;
    vfs_poll_notify(f->vn);
    if((size_t)fd<task_process()->fd_hint)task_process()->fd_hint=(size_t)fd;
    irq_restore(flags);
    /* Final audio close may sleep. The detached slot's reference owns f. */
    vfs_close(f);
    return 0;
}

static int64_t sys_stat(const char *upath, struct kstat *st) {
    char path[PATH_MAX_LEN];
    int r = user_path(upath, path);
    if (r < 0) return r;
    if (!user_ok_w(st, sizeof *st)) return -EFAULT;
    return vfs_stat(path, st);
}

static int64_t sys_fstat(int fd, struct kstat *st) {
    struct file *f = getfd_ref(fd);
    if (!f) return -EBADF;
    if (!user_ok_w(st, sizeof *st)) { vfs_close(f); return -EFAULT; }
    uint64_t flags = irq_save();
    st->type = f->vn->tty ? VT_CHAR : f->vn->type;
    st->size = f->vn->size;
    st->mtime = f->vn->mtime;
    st->mode = f->vn->mode;
    irq_restore(flags);
    vfs_close(f);
    return 0;
}

static int64_t sys_readdir(int fd, uint64_t idx, struct dirent *d) {
    struct file *f = getfd_ref(fd);
    if (!f) return -EBADF;
    if (!user_ok_w(d, sizeof *d)) { vfs_close(f); return -EFAULT; }
    int64_t result = vfs_readdir(f, idx, d);
    vfs_close(f);
    return result;
}

static int64_t sys_mkdir(const char *upath) {
    char path[PATH_MAX_LEN];
    int r = user_path(upath, path);
    return r < 0 ? r : vfs_mkdir(path);
}

static int64_t sys_unlink(const char *upath) {
    char path[PATH_MAX_LEN];
    int r = user_path(upath, path);
    return r < 0 ? r : vfs_unlink(path);
}

static int64_t sys_rename(const char *uold, const char *unew) {
    char a[PATH_MAX_LEN], b[PATH_MAX_LEN];
    int r = user_path(uold, a);
    if (r < 0) return r;
    r = user_path(unew, b);
    if (r < 0) return r;
    struct kstat st;
    if (vfs_stat(a, &st) < 0) return -ENOENT;
    if (st.type != VT_FILE) return -EISDIR;
    uint8_t *data;
    uint64_t size;
    r = vfs_read_whole(a, &data, &size);
    if (r < 0) return r;
    r = vfs_write_whole(b, data, size);
    kfree(data);
    if (r < 0) return r;
    return vfs_unlink(a);
}

static int64_t sys_chdir(const char *upath) {
    char path[PATH_MAX_LEN];
    int r = user_path(upath, path);
    if (r < 0) return r;
    struct kstat st;
    if (vfs_stat(path, &st) < 0) return -ENOENT;
    if (st.type != VT_DIR) return -ENOTDIR;
    uint64_t flags = irq_save();
    strlcpy(task_process()->cwd, path, sizeof task_process()->cwd);
    irq_restore(flags);
    return 0;
}

static int64_t sys_getcwd(char *buf, size_t n) {
    char cwd[sizeof task_process()->cwd];
    uint64_t flags = irq_save();
    memcpy(cwd, task_process()->cwd, sizeof cwd);
    irq_restore(flags);
    size_t l = strlen(cwd) + 1;
    if (n < l) return -ERANGE;
    if (!user_ok_w(buf, l)) return -EFAULT;
    memcpy(buf, cwd, l);
    return l;
}

static int64_t sys_spawn(const char *upath, char **uargv, const int *ufdmap, int flags) {
    char path[PATH_MAX_LEN];
    int r = user_path(upath, path);
    if (r < 0) return r;
    char *argv[32];
    int argc = 0;
    int64_t ret;
    struct file *fds[3] = {NULL, NULL, NULL};
    if (uargv) {
        while (argc < 31) {
            if (!user_ok(&uargv[argc], 8)) { ret = -EFAULT; goto out; }
            char *ua = uargv[argc];
            if (!ua) break;
            char tmp[512];
            r = user_str(tmp, ua, sizeof tmp);
            if (r < 0) { ret = r; goto out; }
            argv[argc++] = strdup(tmp);
        }
    }
    if (argc == 0) argv[argc++] = strdup(path);
    int sources[3] = {0, 1, 2};
    for (int i = 0; i < 3; i++) {
        if (ufdmap) {
            if (!user_ok(&ufdmap[i], 4)) { ret = -EFAULT; goto out; }
            sources[i] = ufdmap[i];
        }
    }
    char cwd[sizeof task_process()->cwd];
    uint64_t irq_flags = irq_save();
    for (int i = 0; i < 3; i++) {
        fds[i] = getfd(sources[i]);
        if (fds[i]) vfs_dup(fds[i]);
    }
    memcpy(cwd, task_process()->cwd, sizeof cwd);
    irq_restore(irq_flags);
    ret = proc_spawn(path, argc, argv, fds, cwd, (flags & SPAWN_DETACH) ? NULL : task_process());
out:
    for (unsigned i = 0; i < 3; i++) if (fds[i]) vfs_close(fds[i]);
    for (int i = 0; i < argc; i++) kfree(argv[i]);
    return ret;
}

static int64_t sys_waitpid(int pid, int *ustatus, int flags) {
    if (ustatus && !user_ok_w(ustatus, 4)) return -EFAULT;
    int st = 0;
    int r = task_wait(pid, &st, flags & 1);
    if (r > 0 && ustatus) *ustatus = st;
    return r;
}

/* Physical memory ran out under a heap request: name the largest user
   processes once per interval, so the owner of the memory is visible. */
static void report_heap_exhaustion(struct task *t, int64_t inc) {
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
    kprintf("[memory] pid %d (%s): no pages for %ld more heap bytes, %lu pages free; largest:",
            t->pid, t->name, (long)inc, (unsigned long)pmm_free_pages());
    for (int i = 0; i < n; i++) kprintf(" %d/%s %luK", top[i].pid, top[i].name, (unsigned long)top[i].kb);
    kprintf("\n");
}

static int64_t sys_sbrk_locked(int64_t inc) {
    struct task *t = task_process();
    uint64_t old = t->brk;
    if (inc == 0) return old;
    uint64_t nb = t->brk + inc;
    if (nb < t->brk_base || nb >= USER_MMAP_BASE) return -ENOMEM;
    if (inc > 0) {
        uint64_t start = ALIGN_UP(old, PAGE_SIZE);
        uint64_t end = ALIGN_UP(nb, PAGE_SIZE);
        for (uint64_t page = start; page < end; page += PAGE_SIZE) {
            if (vmm_user_alloc(t->pml4, page, PAGE_SIZE, VM_W) < 0) {
                /* Only this attempt's completed prefix belongs to the rollback.
                   Preserve the old partial page, and do not scan the unallocated
                   tail of a request that can be much larger than physical RAM. */
                vmm_user_free(t->pml4, start, page - start);
                report_heap_exhaustion(t, inc);
                return -ENOMEM;
            }
        }
    } else {
        vmm_user_free(t->pml4, ALIGN_UP(nb, PAGE_SIZE), ALIGN_UP(old, PAGE_SIZE) - ALIGN_UP(nb, PAGE_SIZE));
    }
    t->brk = nb;
    return old;
}

static int64_t sys_sbrk(int64_t inc) {
    return sys_sbrk_locked(inc);
}

static int64_t sys_pipe(int *ufds) {
    if (!user_ok_w(ufds, 8)) return -EFAULT;
    struct file *r, *w;
    int e = pipe_create(&r, &w);
    if (e < 0) return e;
    uint64_t flags = irq_save();
    int a=allocfd_locked(r),b=a<0?a:allocfd_locked(w);
    int output_error=b<0?b:!user_ok_w(ufds,8)?-EFAULT:0;
    if (output_error) {
        if(b>=0)task_process()->fds[b]=NULL;
        if(a>=0){task_process()->fds[a]=NULL;task_process()->fd_hint=(size_t)a;}
        irq_restore(flags);
        vfs_close(r);
        vfs_close(w);
        return output_error;
    }
    ufds[0] = a;
    ufds[1] = b;
    irq_restore(flags);
    return 0;
}

/* Snapshot the explicit native key length once; never rescan mutable user
 * strings in the registry, and never constrain identity to a guessed path cap. */
static int64_t sys_channel_named(const char *ukey,size_t length,bool listen) {
    if(!length || length>SIZE_MAX-sizeof(struct file))return -EINVAL;
    char *key=kmalloc(length);if(!key)return -ENOMEM;
    struct file *file=NULL;uint64_t flags=irq_save();
    if(!user_ok(ukey,length)){irq_restore(flags);kfree(key);return -EFAULT;}
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
    struct file *listener=getfd_ref(fd);if(!listener)return -EBADF;
    struct file *server=NULL;uint64_t flags=irq_save();int slot=task_fd_find(task_process());
    int result=slot>=0?channel_accept(listener,&server):slot;
    if(!result)result=allocfd_locked(server);
    irq_restore(flags);vfs_close(listener);
    if(result<0 && server)vfs_close(server);
    return result;
}

static int64_t sys_dup2(int oldfd, int newfd) {
    uint64_t flags = irq_save();
    struct file *f = getfd(oldfd);
    if (!f || newfd < 0) { irq_restore(flags); return -EBADF; }
    if (oldfd == newfd) { irq_restore(flags); return newfd; }
    int error=task_fd_reserve(task_process(),(size_t)newfd+1);
    if(error){irq_restore(flags);return error;}
    struct file *previous = task_process()->fds[newfd];
    task_process()->fds[newfd] = vfs_dup(f);
    if (previous) vfs_poll_notify(previous->vn);
    irq_restore(flags);
    vfs_close(previous);
    return newfd;
}

static int64_t sys_dup(int oldfd) {
    struct file *f = getfd_ref(oldfd);
    if (!f) return -EBADF;
    int fd = allocfd(f);
    if (fd < 0) vfs_close(f);
    return fd;
}

static int64_t sys_poll(struct n_pollfd *ufds, int n, int timeout) {
    if (n < 0 || (size_t)n>SIZE_MAX/sizeof(struct n_pollfd)) return -EINVAL;
    size_t bytes=(size_t)n*sizeof(struct n_pollfd);
    struct n_pollfd local[64],*fds=n<=64?local:kmalloc(bytes);
    if(!fds)return -ENOMEM;
    uint64_t snapshot_flags=irq_save();
    if(!user_ok_w(ufds,bytes)){irq_restore(snapshot_flags);if(fds!=local)kfree(fds);return -EFAULT;}
    memcpy(fds,ufds,bytes);irq_restore(snapshot_flags);
    uint64_t deadline = timeout >= 0 ? uptime_ms() + timeout : 0;
    struct poll_wait wait = {0};
    int result=0;
    for (;;) {
        uint64_t fl = irq_save();
        uint64_t generation=wait.generation;
        int ready = 0;
        for (int i = 0; i < n; i++) {
            fds[i].revents = 0;
            struct file *f = getfd(fds[i].fd);
            if(f) {
                if ((fds[i].events & N_POLLIN) && vfs_can_read(f)) fds[i].revents |= N_POLLIN;
                if ((fds[i].events & N_POLLOUT) && vfs_can_write(f)) fds[i].revents |= N_POLLOUT;
                if (f->vn->ops && f->vn->ops->hangup && f->vn->ops->hangup(f->vn,f)) fds[i].revents |= N_POLLHUP;
                if (fds[i].revents) ready++;
                /* Checking and enrollment share the same IRQ guard. Avoid
                   allocating listeners for an already-ready descriptor. */
                else if (timeout != 0) vfs_poll_file(&wait, f);
            }
            /* Preserve atomic per-descriptor checks without letting a large
             * valid client list become one non-preemptible kernel operation. */
            irq_restore(fl);fl=irq_save();
        }
        bool expired=timeout>0 && uptime_ms()>=deadline;
        if (ready || timeout == 0 || expired || wait.error) {
            result=ready ? ready : wait.error ? wait.error : 0;
            /* A sibling may have unmapped the original array while this
             * syscall slept/scanned; no kernel copy targets stale user pages. */
            if(!user_ok_w(ufds,bytes))result=-EFAULT;
            else memcpy(ufds,fds,bytes);
            irq_restore(fl);
            break;
        }
        if (current_task->killed) {
            irq_restore(fl);
            result=-EINTR;break;
        }
        /* Closing/enqueuing during a released scan quantum cannot be lost
         * between readiness and wait enrollment. Retry only for a new event. */
        if(generation!=wait.generation){irq_restore(fl);vfs_poll_cleanup(&wait);continue;}
        if (timeout > 0) {
            uint64_t now = uptime_ms();
            if(now<deadline)wq_wait_timeout(&wait.wq, deadline - now);
        } else {
            wq_wait(&wait.wq);
        }
        irq_restore(fl);
        vfs_poll_cleanup(&wait);
    }
    vfs_poll_cleanup(&wait);
    if(fds!=local)kfree(fds);
    return result;
}

static int64_t sys_proclist(struct n_procinfo *out, int max) {
    if (max < 0 || !user_ok_w(out, (size_t)max * sizeof *out)) return -EFAULT;
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
    if (!user_ok_w(si, sizeof *si)) return -EFAULT;
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

static int64_t sys_fcntl(int fd, int cmd, int arg) {
    if (cmd == F_NLOCK || cmd == F_NUNLOCK || cmd == F_NTRYLOCK) {
        struct file *owned = getfd_ref(fd);
        if (!owned) return -EBADF;
        int result = cmd == F_NTRYLOCK ? vfs_try_lease(owned) : vfs_lease(owned, cmd == F_NLOCK);
        vfs_close(owned);
        return result;
    }
    uint64_t flags = irq_save();
    struct file *f = getfd(fd);
    int64_t result = -EINVAL;
    if (!f) result = -EBADF;
    else if (cmd == F_GETFL) result = f->flags;
    else if (cmd == F_SETTTY) {
        f->vn->tty = arg != 0;
        result = 0;
    } else if (cmd == F_SETFL) {
        f->flags = (f->flags & O_ACCMODE) | (arg & ~O_ACCMODE);
        result = 0;
    }
    irq_restore(flags);
    return result;
}

static int64_t sys_killtree(int pid, int include_self) {
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
#define SHARED_LEAF (PAGE_SIZE/sizeof(uint64_t))
struct shared_pages {size_t count;bool demand;uint64_t pages[];};
struct shared_mapping {struct shared_mapping *next;struct file *file;uint64_t va,flags;size_t bytes;};
static size_t shared_slots(const struct shared_pages *p) {
    return p->demand?(p->count+SHARED_LEAF-1)/SHARED_LEAF:p->count;
}
/* BSP-owned, called with interrupts disabled or from the fault handler. */
static uint64_t shared_frame(struct shared_pages *p,size_t index,bool create) {
    if(!p->demand)return p->pages[index];
    uint64_t *leaf=&p->pages[index/SHARED_LEAF];
    if(!*leaf){if(!create || !(*leaf=pmm_alloc_zeroed()))return 0;}
    uint64_t *slot=(uint64_t *)phys_to_virt(*leaf)+index%SHARED_LEAF;
    if(!*slot && create)*slot=pmm_alloc_zeroed();
    return *slot;
}
static void shared_release(struct vnode *v) {
    struct shared_pages *p=v->priv;
    if(p){
        for(size_t i=0;i<shared_slots(p);i++){
            if(!p->pages[i])continue;
            if(p->demand){uint64_t *leaf=phys_to_virt(p->pages[i]);
                for(size_t j=0;j<SHARED_LEAF;j++)if(leaf[j])pmm_free(leaf[j]);}
            pmm_free(p->pages[i]);
        }
        kfree(p);
    }
    kfree(v);
}
static struct vnode_ops shared_ops={.release=shared_release};
/* A file over a new shared object of `bytes` (page aligned). */
static int64_t shared_object(size_t bytes,bool demand,int flags,struct file **out) {
    if(!bytes || bytes>SIZE_MAX-(PAGE_SIZE-1))return -EINVAL;
    bytes=ALIGN_UP(bytes,PAGE_SIZE);size_t count=bytes/PAGE_SIZE;
    size_t slots=demand?(count+SHARED_LEAF-1)/SHARED_LEAF:count;
    if(slots>(SIZE_MAX-sizeof(struct shared_pages))/sizeof(uint64_t))return -EINVAL;
    if(!demand && count>pmm_free_pages())return -ENOMEM;
    struct vnode *v=kzalloc(sizeof *v);if(!v)return -ENOMEM;
    struct shared_pages *p=kzalloc(sizeof *p+slots*sizeof(uint64_t));
    if(!p){kfree(v);return -ENOMEM;}
    p->demand=demand;
    v->type=VT_SHARED;v->size=bytes;v->unlinked=true;v->ops=&shared_ops;v->priv=p;
    if(demand)p->count=count;
    else for(;p->count<count;p->count++){
        p->pages[p->count]=pmm_alloc_zeroed();
        if(!p->pages[p->count]){shared_release(v);return -ENOMEM;}
    }
    struct file *f=vfs_open_vnode(v,flags);
    if(!f){shared_release(v);return -ENOMEM;}
    *out=f;return 0;
}
static int64_t sys_shared_create(size_t bytes,bool demand) {
    struct file *f=NULL;int64_t r=shared_object(bytes,demand,O_RDWR,&f);
    if(r<0)return r;
    int fd=allocfd(f);if(fd<0)vfs_close(f);return fd;
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
    if(!m){vfs_close(f);return -ENOMEM;}
    struct shared_pages *p=f->vn->priv;
    uint64_t flags=PTE_U|PTE_SHARED|PTE_SHARED_RO|(exec?0:pte_nx);size_t mapped=0;
    for(;mapped<p->count;mapped++)if(!vmm_map_page(t->pml4,start+mapped*PAGE_SIZE,p->pages[mapped],flags))break;
    if(mapped!=p->count){vmm_user_free(t->pml4,start,mapped*PAGE_SIZE);vfs_close(f);kfree(m);return -ENOMEM;}
    irq=irq_save();*m=(struct shared_mapping){.next=t->shared_mappings,.file=f,.va=start,.flags=flags,.bytes=end-start};
    t->shared_mappings=m;irq_restore(irq);
    return 0;
}
/* A first touch of a reserved shared page, from user code or a kernel copy
 * on its behalf: allocate the object's frame (or adopt the one a peer already
 * created) and map it with the mapping's own protection. */
bool shared_demand_fault(uint64_t addr,bool write) {
    struct task *owner=task_process();
    if(!owner)return false;
    uint64_t page=ALIGN_DOWN(addr,PAGE_SIZE),irq=irq_save();bool ok=false,starved=false;
    for(struct shared_mapping *m=owner->shared_mappings;m;m=m->next){
        if(page<m->va || page-m->va>=m->bytes)continue;
        struct shared_pages *p=m->file->vn->priv;
        if(!p->demand || (write && !(m->flags&PTE_W)))break;
        if(vmm_get_pte(owner->pml4,page)&PTE_P){ok=true;break;}
        uint64_t frame=shared_frame(p,(page-m->va)/PAGE_SIZE,true);
        starved=!frame;
        ok=frame && vmm_map_page(owner->pml4,page,frame,m->flags);
        break;
    }
    irq_restore(irq);
    if(starved)kprintf("[shared] pid %d (%s): no frame for page %p, %lu pages free\n",
                       owner->pid,owner->name,(void *)page,(unsigned long)pmm_free_pages());
    return ok;
}
static int64_t sys_shared_map(int pid,int fd,size_t bytes,int prot) {
    if(!bytes || bytes>SIZE_MAX-(PAGE_SIZE-1) || (prot&~N_PROT_WRITE))return -EINVAL;
    struct task *owner=task_process();struct file *f=NULL;
    uint64_t irq=irq_save();struct task *peer=pid?task_find(pid):owner;
    if(peer && !peer->is_thread && peer->is_user && !peer->exiting &&
       (peer==owner || peer->parent==owner || owner->parent==peer) &&
       fd>=0 && (size_t)fd<peer->fd_capacity && peer->fds[fd] &&
       peer->fds[fd]->vn->type==VT_SHARED)f=vfs_dup(peer->fds[fd]);
    irq_restore(irq);if(!f)return -EBADF;
    bytes=ALIGN_UP(bytes,PAGE_SIZE);
    if(bytes!=f->vn->size){vfs_close(f);return -EINVAL;}
    struct shared_mapping *m=kzalloc(sizeof *m);
    if(!m){vfs_close(f);return -ENOMEM;}
    /* Reserve the VA range before publishing any PTE. No blocking handoff is
       made in this page loop, but the short BSP guard also keeps future thread
       mmap/clock/stack reservations from choosing this partially mapped span. */
    irq=irq_save();uint64_t va=owner->mmap_next;
    if(va>=USER_STACK_TOP-USER_STACK_MAX || bytes+PAGE_SIZE<bytes ||
       bytes+PAGE_SIZE>USER_STACK_TOP-USER_STACK_MAX-va){irq_restore(irq);vfs_close(f);kfree(m);return -ENOMEM;}
    uint64_t reserved_end=va+bytes+PAGE_SIZE;owner->mmap_next=reserved_end;irq_restore(irq);
    struct shared_pages *p=f->vn->priv;size_t mapped=0;
    uint64_t flags=PTE_U|PTE_SHARED|pte_nx|(prot&N_PROT_WRITE?PTE_W:PTE_SHARED_RO);
    /* Demand pages not yet touched by any peer stay unmapped until a fault. */
    for(;mapped<p->count;mapped++){
        irq=irq_save();uint64_t frame=shared_frame(p,mapped,false);irq_restore(irq);
        if(frame && !vmm_map_page(owner->pml4,va+mapped*PAGE_SIZE,frame,flags))break;
    }
    if(mapped!=p->count){vmm_user_free(owner->pml4,va,mapped*PAGE_SIZE);
        irq=irq_save();if(owner->mmap_next==reserved_end)owner->mmap_next=va;irq_restore(irq);
        vfs_close(f);kfree(m);return -ENOMEM;
    }
    irq=irq_save();*m=(struct shared_mapping){.next=owner->shared_mappings,.file=f,.va=va,.flags=flags,.bytes=bytes};
    owner->shared_mappings=m;irq_restore(irq);return (int64_t)va;
}
/* A sender offers its page reference in a message; the parent or child that
 * reads the message claims it into its own descriptor table. The ticket keeps
 * the pages alive even if the sender's own buffer is collected first, and is
 * released when claimed or when the sender exits. */
struct shared_ticket {struct shared_ticket *next;uint64_t id;struct file *file;};
static struct task *shared_peer(int pid) {
    struct task *owner=task_process(),*peer=pid?task_find(pid):owner;
    if(peer && !peer->is_thread && peer->is_user && !peer->exiting &&
       (peer==owner || peer->parent==owner || owner->parent==peer))return peer;
    return NULL;
}
static int64_t sys_shared_send(int fd) {
    struct file *f=getfd_ref(fd);
    if(!f)return -EBADF;
    if(f->vn->type!=VT_SHARED){vfs_close(f);return -EINVAL;}
    struct shared_ticket *t=kzalloc(sizeof *t);
    if(!t){vfs_close(f);return -ENOMEM;}
    struct task *owner=task_process();
    uint64_t irq=irq_save();
    t->id=++owner->shared_ticket_serial;t->file=f;t->next=owner->shared_tickets;owner->shared_tickets=t;
    irq_restore(irq);
    return (int64_t)t->id;
}
static int64_t sys_shared_receive(int pid,uint64_t id) {
    struct file *f=NULL;
    uint64_t irq=irq_save();struct task *peer=shared_peer(pid);
    if(peer)for(struct shared_ticket *t=peer->shared_tickets;t;t=t->next)if(t->id==id){f=vfs_dup(t->file);break;}
    irq_restore(irq);
    if(!f)return -EBADF;
    int fd=allocfd(f);
    if(fd<0){vfs_close(f);return fd;}
    struct shared_ticket *claimed=NULL;
    irq=irq_save();peer=shared_peer(pid);
    if(peer)for(struct shared_ticket **link=&peer->shared_tickets;*link;link=&(*link)->next)
        if((*link)->id==id){claimed=*link;*link=claimed->next;break;}
    irq_restore(irq);
    if(claimed){vfs_close(claimed->file);kfree(claimed);}
    return fd;
}
void sys_shared_cleanup(struct task *owner) {
    while(owner->shared_mappings){struct shared_mapping *m=owner->shared_mappings;
        owner->shared_mappings=m->next;
        vmm_user_free(owner->pml4,m->va,m->bytes);vfs_close(m->file);kfree(m);
    }
    while(owner->shared_tickets){struct shared_ticket *t=owner->shared_tickets;
        owner->shared_tickets=t->next;vfs_close(t->file);kfree(t);
    }
    mmap_holes_free(owner);
    program_segments_free(program_segments_unused());
}
static int64_t sys_address_watch_queue(void) {
    struct vnode *v=address_watch_queue_new();
    if(!v)return -ENOMEM;
    struct file *f=vfs_open_vnode(v,O_RDONLY);
    if(!f){v->ops->release(v);return -ENOMEM;}
    int fd=allocfd(f);if(fd<0)vfs_close(f);return fd;
}
static int64_t sys_address_watch(int fd,uint64_t address,uint32_t expected,uint64_t token) {
    if((address&3) || !user_ok((void *)address,sizeof(uint32_t)))return -EFAULT;
    struct file *f=getfd_ref(fd);
    if(!f)return -EBADF;
    int64_t r=address_watch_add(f->vn,(volatile uint32_t *)address,expected,token);
    vfs_close(f);return r;
}
static int64_t sys_address_unwatch(int fd,uint64_t token) {
    struct file *f=getfd_ref(fd);
    if(!f)return -EBADF;
    int64_t r=address_watch_remove(f->vn,token);
    vfs_close(f);return r;
}

/* Freed address space of the mmap region. Without reuse every map/unmap cycle
   consumes fresh addresses and the page tables covering them, so a process
   that keeps allocating and freeing large blocks leaks kernel memory. Every
   reservation keeps one unmapped guard page after itself; a hole includes it. */
struct mmap_hole { struct mmap_hole *next; uint64_t va, len; };
void mmap_hole_add(struct task *owner, uint64_t va, uint64_t len) {
    if (!owner || !len) return;
    struct mmap_hole *spare = kmalloc(sizeof *spare);
    uint64_t irq = irq_save();
    struct mmap_hole **link = &owner->mmap_holes, *prev = NULL;
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
    if (!h->next && h->va + h->len >= owner->mmap_next) {
        owner->mmap_next = h->va;
        if (prev) prev->next = NULL; else owner->mmap_holes = NULL;
        kfree(h);
    }
    irq_restore(irq);
    kfree(spare);
}
/* First fit for len bytes plus their guard page; 0 when no hole fits. */
static uint64_t mmap_hole_take(struct task *owner, uint64_t len) {
    uint64_t need = len + PAGE_SIZE, va = 0;
    uint64_t irq = irq_save();
    for (struct mmap_hole **link = &owner->mmap_holes; *link; link = &(*link)->next) {
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
}
uint64_t mmap_reserve(struct task *owner, uint64_t len) {
    if (!len || len > USER_STACK_TOP) return 0;
    uint64_t va = mmap_hole_take(owner, len);
    if (va) return va;
    uint64_t irq = irq_save();
    va = owner->mmap_next;
    if (va >= USER_STACK_TOP - USER_STACK_MAX ||
        len + PAGE_SIZE > USER_STACK_TOP - USER_STACK_MAX - va) va = 0;
    else owner->mmap_next = va + len + PAGE_SIZE; /* reserve before any page loop */
    irq_restore(irq);
    return va;
}

/* anonymous memory in its own region above the heap (window buffers live there too) */
static int64_t sys_mmap_locked(size_t len, int prot) {
    struct task *t = task_process();
    if (len == 0 || len > (1ULL << 32)) return -EINVAL;
    len = ALIGN_UP(len, PAGE_SIZE);
    int vp = (prot & N_PROT_WRITE ? VM_W : 0) | (prot & N_PROT_EXEC ? VM_X : 0);
    uint64_t va = mmap_reserve(t, len);
    if (!va) return -ENOMEM;
    if (vmm_user_alloc(t->pml4, va, len, vp) < 0) {
        vmm_user_free(t->pml4, va, len);
        mmap_hole_add(t, va, len + PAGE_SIZE);
        return -ENOMEM;
    }
    return (int64_t)va;
}

static int64_t sys_munmap_locked(uint64_t va, size_t len) {
    struct task *t = task_process();
    if (va & (PAGE_SIZE - 1) || va < USER_MMAP_BASE || va + len > t->mmap_next || va + len < va) return -EINVAL;
    if(len>SIZE_MAX-(PAGE_SIZE-1))return -EINVAL;
    len=ALIGN_UP(len,PAGE_SIZE);
    struct shared_mapping **link=&t->shared_mappings;
    while(*link){struct shared_mapping *m=*link;
        if(va<m->va+m->bytes && m->va<va+len){
            if(va!=m->va || len!=m->bytes)return -EINVAL;
            *link=m->next;vmm_user_free(t->pml4,va,len);vfs_close(m->file);kfree(m);return 0;
        }link=&m->next;
    }
    for (uint64_t a = va; a < va + len; a += PAGE_SIZE)
        if (vmm_get_pte(t->pml4, a) & PTE_SHARED) return -EINVAL; /* a window buffer */
    vmm_user_free(t->pml4, va, len);
    /* The unmapped page right after the range is the reservation's guard. */
    uint64_t end = va + len;
    bool guard = end < t->mmap_next && !(vmm_get_pte(t->pml4, end) & PTE_P);
    for (struct shared_mapping *m = t->shared_mappings; guard && m; m = m->next)
        if (end >= m->va && end < m->va + m->bytes) guard = false; /* an untouched demand page */
    mmap_hole_add(t, va, guard ? len + PAGE_SIZE : len);
    return 0;
}

static int64_t sys_mprotect_locked(uint64_t va, size_t len, int prot) {
    if (va & (PAGE_SIZE - 1) || va >= USER_TOP || va + len > USER_TOP || va + len < va) return -EINVAL;
    int vp = (prot & N_PROT_WRITE ? VM_W : 0) | (prot & N_PROT_EXEC ? VM_X : 0);
    return vmm_user_protect(current_task->pml4, va, len, vp);
}

/* These finite BSP helpers do not sleep. Kernel-mode timer IRQs request a
   reschedule but cannot switch their continuation (idt.c). Keep IRQ delivery
   enabled for large allocations; each PTE publication has its own short guard. */
static int64_t sys_mmap(size_t len, int prot) {
    return sys_mmap_locked(len, prot);
}
static int64_t sys_munmap(uint64_t va, size_t len) {
    return sys_munmap_locked(va, len);
}
static int64_t sys_mprotect(uint64_t va, size_t len, int prot) {
    return sys_mprotect_locked(va, len, prot);
}

static int64_t sys_dmesg(char *buf, size_t n) {
    if (!user_ok_w(buf, n)) return -EFAULT;
    return klog_read(buf, n);
}

void syscall_dispatch(struct regs *r) {
    /* An AP trap may be queued as a BSP continuation when a sibling aborts
       its process. Do not execute that pending syscall after it is killed:
       a blocking syscall could enroll a new wait after task_kill's wake and
       never reach the return-to-user kill check. */
    if (current_task->killed) task_exit(current_task->kill_code);
    sti();
    uint64_t a = r->rdi, b = r->rsi, c = r->rdx, d = r->r10, e = r->r8;
    int64_t ret = -ENOSYS;
    switch (r->rax) {
    case SYS_EXIT: task_exit((int)a);
    case SYS_READ: ret = sys_read((int)a, (void *)b, c); break;
    case SYS_WRITE: ret = sys_write((int)a, (const void *)b, c); break;
    case SYS_OPEN: ret = sys_open((const char *)a, (int)b); break;
    case SYS_CLOSE: ret = sys_close((int)a); break;
    case SYS_AUDIO_FLUSH: {
        struct file *f = getfd_ref((int)a);
        ret = f ? audio_flush_file(f) : -EBADF;
        vfs_close(f);
        break;
    }
    case SYS_LSEEK: {
        struct file *f = getfd_ref((int)a);
        ret = f ? vfs_seek(f, (int64_t)b, (int)c) : -EBADF;
        vfs_close(f);
        break;
    }
    case SYS_FTRUNCATE: {
        struct file *f = getfd_ref((int)a);
        ret = f ? vfs_truncate(f, b) : -EBADF;
        vfs_close(f);
        break;
    }
    case SYS_STAT: ret = sys_stat((const char *)a, (struct kstat *)b); break;
    case SYS_FSTAT: ret = sys_fstat((int)a, (struct kstat *)b); break;
    case SYS_READDIR: ret = sys_readdir((int)a, b, (struct dirent *)c); break;
    case SYS_MKDIR: ret = sys_mkdir((const char *)a); break;
    case SYS_UNLINK: ret = sys_unlink((const char *)a); break;
    case SYS_RENAME: ret = sys_rename((const char *)a, (const char *)b); break;
    case SYS_CHDIR: ret = sys_chdir((const char *)a); break;
    case SYS_GETCWD: ret = sys_getcwd((char *)a, b); break;
    case SYS_SPAWN: ret = sys_spawn((const char *)a, (char **)b, (const int *)c, (int)d); break;
    case SYS_WAITPID: ret = sys_waitpid((int)a, (int *)b, (int)c); break;
    case SYS_GETPID: ret = task_process()->pid; break;
    case SYS_GETPPID: ret = task_process()->parent ? task_process()->parent->pid : 0; break;
    case SYS_KILL: ret = task_kill((int)a); break;
    case SYS_KILLTREE: ret = sys_killtree((int)a, (int)b); break;
    case SYS_SBRK: ret = sys_sbrk((int64_t)a); break;
    case SYS_SLEEP: sleep_ms(a); ret = 0; break;
    case SYS_UPTIME: ret = uptime_ms(); break;
    case SYS_TIME: ret = time_now(); break;
    case SYS_PIPE: ret = sys_pipe((int *)a); break;
    case SYS_SHARED_CREATE: ret = sys_shared_create(a,false); break;
    case SYS_SHARED_RESERVE: ret = sys_shared_create(a,true); break;
    case SYS_SHARED_MAP: ret = sys_shared_map((int)a,(int)b,c,(int)d); break;
    case SYS_SHARED_SEND: ret = sys_shared_send((int)a); break;
    case SYS_SHARED_RECEIVE: ret = sys_shared_receive((int)a,b); break;
    case SYS_ADDRESS_WATCH_QUEUE: ret = sys_address_watch_queue(); break;
    case SYS_ADDRESS_WATCH: ret = sys_address_watch((int)a,b,(uint32_t)c,d); break;
    case SYS_ADDRESS_UNWATCH: ret = sys_address_unwatch((int)a,b); break;
    case SYS_CHANNEL_LISTEN: ret = sys_channel_named((const char *)a,b,true); break;
    case SYS_CHANNEL_CONNECT: ret = sys_channel_named((const char *)a,b,false); break;
    case SYS_CHANNEL_ACCEPT: ret = sys_channel_accept((int)a); break;
    case SYS_DUP2: ret = sys_dup2((int)a, (int)b); break;
    case SYS_DUP: ret = sys_dup((int)a); break;
    case SYS_POLL: ret = sys_poll((struct n_pollfd *)a, (int)b, (int)c); break;
    case SYS_PROCLIST: ret = sys_proclist((struct n_procinfo *)a, (int)b); break;
    case SYS_SYSINFO: ret = sys_sysinfo((struct n_sysinfo *)a); break;
    case SYS_CPU_INFO: {
        struct n_cpuinfo info;
        if (!user_ok_w((void *)a, sizeof info)) ret = -EFAULT;
        else { smp_get_info(&info); memcpy((void *)a, &info, sizeof info); ret = 0; }
        break;
    }
    case SYS_CLOCK_INFO: {
        struct n_clockinfo info = {.version = 1};
        if (!user_ok_w((void *)a, sizeof info)) { ret = -EFAULT; break; }
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
            (vmm_get_pte(current_task->pml4, c) & PTE_NX)) ret = -EFAULT;
        else ret = task_thread_create(c, a, b);
        break;
    case SYS_THREAD_JOIN: ret = task_thread_join((int)a); break;
    case SYS_THREAD_EXIT: task_exit(0);
    case SYS_THREAD_ID: ret = current_task->pid; break;
    case SYS_CPU_INDEX: ret = 0; break;
    case SYS_THREAD_TLS:
        if (!user_ok_w((void *)a, b) || b < 16 || b > PAGE_SIZE) ret = -EFAULT;
        else { current_task->tls_base = a; wrmsr(0xC0000100, a); ret = 0; }
        break;
    case SYS_THREAD_STACK_BOUNDS: {
        if (!user_ok_w((void *)a, sizeof(uintptr_t)) || !user_ok_w((void *)b, sizeof(uintptr_t))) {
            ret = -EFAULT; break;
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
        if ((a & 3) || !user_ok((void *)a, sizeof(uint32_t))) ret = -EFAULT;
        else ret = task_wait_address((volatile void *)a, (uint32_t)b, false, (unsigned)c);
        break;
    case SYS_WAIT_ADDRESS64:
        if ((a & 7) || !user_ok((void *)a, sizeof(uint64_t))) ret = -EFAULT;
        else ret = task_wait_address((volatile void *)a, b, true, (unsigned)c);
        break;
    case SYS_WAKE_ADDRESS:
        if ((a & 3) || !user_ok((void *)a, sizeof(uint32_t))) ret = -EFAULT;
        else ret = task_wake_address((volatile uint32_t *)a, (unsigned)b);
        break;
    case SYS_GPU_INFO: {
        struct n_gpu_info info;
        if (!user_ok_w((void *)a, sizeof info)) ret = -EFAULT;
        else { gpu_get_info(&info); memcpy((void *)a, &info, sizeof info); ret = 0; }
        break;
    }
    case SYS_GPU_RENDER: {
        struct n_gpu_render request;
        if (!user_ok((void *)a, sizeof request)) { ret = -EFAULT; break; }
        memcpy(&request, (void *)a, sizeof request);
        if (!request.width || !request.height || request.width > N_GPU_MAX_SIDE || request.height > N_GPU_MAX_SIDE) {
            ret = -EINVAL; break;
        }
        size_t bytes = (size_t)request.width * request.height * sizeof(uint32_t);
        if (c < bytes) { ret = -EINVAL; break; }
        if (!user_ok_w((void *)b, bytes)) { ret = -EFAULT; break; }
        ret = gpu_render(&request, (uint32_t *)b, bytes);
        break;
    }
    case SYS_GPU_RENDER_BATCH: {
        /* Snapshot the complete finite request before the driver uses it.
           A caller changing its user buffer cannot grow a validated batch. */
        struct n_gpu_batch request;
        if (!user_ok((void *)a, sizeof request)) { ret = -EFAULT; break; }
        memcpy(&request, (void *)a, sizeof request);
        if (!request.width || !request.height ||
            request.width > N_GPU_MAX_SIDE || request.height > N_GPU_MAX_SIDE ||
            request.triangle_count > N_GPU_MAX_TRIANGLES) {
            ret = -EINVAL; break;
        }
        size_t bytes = (size_t)request.width * request.height * sizeof(uint32_t);
        if (c < bytes) { ret = -EINVAL; break; }
        if (!user_ok_w((void *)b, bytes)) { ret = -EFAULT; break; }
        ret = gpu_render_batch(&request, (uint32_t *)b, bytes);
        break;
    }
    case SYS_GPU_BLIT: {
        struct n_gpu_blit request;
        if (!user_ok((void *)a, sizeof request)) { ret = -EFAULT; break; }
        memcpy(&request, (void *)a, sizeof request);
        if (!request.width || !request.height ||
            !request.source_width || !request.source_height ||
            request.width > N_GPU_MAX_SIDE || request.height > N_GPU_MAX_SIDE ||
            request.source_width > N_GPU_MAX_SIDE || request.source_height > N_GPU_MAX_SIDE ||
            !request.source_w || !request.source_h ||
            request.source_x > request.source_width || request.source_y > request.source_height ||
            request.source_w > request.source_width - request.source_x ||
            request.source_h > request.source_height - request.source_y) {
            ret = -EINVAL; break;
        }
        size_t source_bytes = (size_t)request.source_width * request.source_height * sizeof(uint32_t);
        size_t output_bytes = (size_t)request.width * request.height * sizeof(uint32_t);
        if (c < source_bytes || e < output_bytes) { ret = -EINVAL; break; }
        if (!user_ok((void *)b, source_bytes) || !user_ok_w((void *)d, output_bytes)) {
            ret = -EFAULT; break;
        }
        /* The BSP-owned driver snapshots all source pixels before using the
           device or touching output. No extra megabyte allocation per call;
           input/output/request aliases are safe after the copies complete. */
        ret = gpu_blit(&request, (const uint32_t *)b, source_bytes,
                       (uint32_t *)d, output_bytes);
        break;
    }
    case SYS_YIELD: schedule(); ret = 0; break;
    case SYS_POWER:
        if (a == POWER_REBOOT) power_reboot();
        else power_off();
        ret = -EIO;
        break;
    case SYS_FCNTL: ret = sys_fcntl((int)a, (int)b, (int)c); break;
    case SYS_DMESG: ret = sys_dmesg((char *)a, b); break;
    case SYS_MMAP: ret = sys_mmap(a, (int)b); break;
    case SYS_MUNMAP: ret = sys_munmap(a, b); break;
    case SYS_MPROTECT: ret = sys_mprotect(a, b, (int)c); break;
    case SYS_PCILIST:
        if (!user_ok_w((void *)a, b * sizeof(struct n_pciinfo))) ret = -EFAULT;
        else ret = pci_list((struct n_pciinfo *)a, (int)b);
        break;
    default:
        if (r->rax >= SYS_WIN_CREATE && r->rax < SYS_NET_INFO) ret = wm_syscall((int)r->rax, a, b, c, d, e);
        else if (r->rax >= SYS_NET_INFO && r->rax < SYS_MAX) ret = net_syscall((int)r->rax, a, b, c, d, e);
        break;
    }
    r->rax = (uint64_t)ret;
}
