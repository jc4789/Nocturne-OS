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
#include "gui/wm.h"
#include "abi.h"
_Static_assert(PAGE_SIZE == N_PAGE_SIZE, "native mapping granule ABI");

_Static_assert(sizeof(struct n_dirent) == offsetof(struct dirent, mtime), "dirent ABI");
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
    if (!f) return -N_ERR_NO_HANDLE;
    int64_t result = vfs_read_user(f, buf, n);
    vfs_close(f);
    return result;
}

static int64_t sys_write(int fd, const void *buf, size_t n) {
    struct file *f = getfd_ref(fd);
    if (!f) return -N_ERR_NO_HANDLE;
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
    if (!f) { irq_restore(flags); return -N_ERR_NO_HANDLE; }
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
    if (!user_ok_w(st, sizeof *st)) return -N_ERR_BAD_ADDRESS;
    return vfs_stat(path, st);
}

static int64_t sys_fstat(int fd, struct kstat *st) {
    struct file *f = getfd_ref(fd);
    if (!f) return -N_ERR_NO_HANDLE;
    if (!user_ok_w(st, sizeof *st)) { vfs_close(f); return -N_ERR_BAD_ADDRESS; }
    uint64_t flags = irq_save();
    st->type = f->vn->tty ? VT_CHAR : f->vn->type;
    st->size = f->vn->size;
    st->mtime = f->vn->mtime;
    st->mode = f->vn->mode;
    irq_restore(flags);
    vfs_close(f);
    return 0;
}

static int64_t sys_readdir(int fd, uint64_t idx, struct n_dirent *ud) {
    struct file *f = getfd_ref(fd);
    if (!f) return -N_ERR_NO_HANDLE;
    struct dirent d;
    memset(&d, 0, sizeof d);
    int64_t result = vfs_readdir(f, idx, &d);
    vfs_close(f);
    if (result > 0) {
        uint64_t flags = irq_save();
        if (!user_ok_w(ud, sizeof *ud)) result = -N_ERR_BAD_ADDRESS;
        else memcpy(ud, &d, sizeof *ud);
        irq_restore(flags);
    }
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
    if (vfs_stat(a, &st) < 0) return -N_ERR_NOT_FOUND;
    if (st.type != VT_FILE) return -N_ERR_IS_FOLDER;
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
    if (vfs_stat(path, &st) < 0) return -N_ERR_NOT_FOUND;
    if (st.type != VT_DIR) return -N_ERR_NOT_FOLDER;
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
    if (n < l) return -N_ERR_TOO_LARGE;
    if (!user_ok_w(buf, l)) return -N_ERR_BAD_ADDRESS;
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
            if (!user_ok(&uargv[argc], 8)) { ret = -N_ERR_BAD_ADDRESS; goto out; }
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
            if (!user_ok(&ufdmap[i], 4)) { ret = -N_ERR_BAD_ADDRESS; goto out; }
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
    if (ustatus && !user_ok_w(ustatus, 4)) return -N_ERR_BAD_ADDRESS;
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
    if (nb < t->brk_base || nb > t->low_base) return -N_ERR_NO_MEMORY;
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
                return -N_ERR_NO_MEMORY;
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
    if (!user_ok_w(ufds, 8)) return -N_ERR_BAD_ADDRESS;
    struct file *r, *w;
    int e = pipe_create(&r, &w);
    if (e < 0) return e;
    uint64_t flags = irq_save();
    int a=allocfd_locked(r),b=a<0?a:allocfd_locked(w);
    int output_error=b<0?b:!user_ok_w(ufds,8)?-N_ERR_BAD_ADDRESS:0;
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

static int64_t sys_dup2(int oldfd, int newfd) {
    uint64_t flags = irq_save();
    struct file *f = getfd(oldfd);
    if (!f || newfd < 0) { irq_restore(flags); return -N_ERR_NO_HANDLE; }
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
    if (!f) return -N_ERR_NO_HANDLE;
    int fd = allocfd(f);
    if (fd < 0) vfs_close(f);
    return fd;
}

static int64_t sys_poll(struct n_pollfd *ufds, int n, int timeout) {
    if (n < 0 || (size_t)n>SIZE_MAX/sizeof(struct n_pollfd)) return -N_ERR_INVALID;
    size_t bytes=(size_t)n*sizeof(struct n_pollfd);
    struct n_pollfd local[64],*fds=n<=64?local:kmalloc(bytes);
    if(!fds)return -N_ERR_NO_MEMORY;
    uint64_t snapshot_flags=irq_save();
    if(!user_ok_w(ufds,bytes)){irq_restore(snapshot_flags);if(fds!=local)kfree(fds);return -N_ERR_BAD_ADDRESS;}
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
            if(!user_ok_w(ufds,bytes))result=-N_ERR_BAD_ADDRESS;
            else memcpy(ufds,fds,bytes);
            irq_restore(fl);
            break;
        }
        if (current_task->killed) {
            irq_restore(fl);
            result=-N_ERR_STOPPED;break;
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

static int64_t sys_fcntl(int fd, int cmd, int arg) {
    if (cmd == F_NLOCK || cmd == F_NUNLOCK || cmd == F_NTRYLOCK) {
        struct file *owned = getfd_ref(fd);
        if (!owned) return -N_ERR_NO_HANDLE;
        int result = cmd == F_NTRYLOCK ? vfs_try_lease(owned) : vfs_lease(owned, cmd == F_NLOCK);
        vfs_close(owned);
        return result;
    }
    uint64_t flags = irq_save();
    struct file *f = getfd(fd);
    int64_t result = -N_ERR_INVALID;
    if (!f) result = -N_ERR_NO_HANDLE;
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
    if(!bytes || bytes>SIZE_MAX-(PAGE_SIZE-1))return -N_ERR_INVALID;
    bytes=ALIGN_UP(bytes,PAGE_SIZE);size_t count=bytes/PAGE_SIZE;
    size_t slots=demand?(count+SHARED_LEAF-1)/SHARED_LEAF:count;
    if(slots>(SIZE_MAX-sizeof(struct shared_pages))/sizeof(uint64_t))return -N_ERR_INVALID;
    if(!demand && count>pmm_free_pages())return -N_ERR_NO_MEMORY;
    struct vnode *v=kzalloc(sizeof *v);if(!v)return -N_ERR_NO_MEMORY;
    struct shared_pages *p=kzalloc(sizeof *p+slots*sizeof(uint64_t));
    if(!p){kfree(v);return -N_ERR_NO_MEMORY;}
    p->demand=demand;
    v->type=VT_SHARED;v->size=bytes;v->unlinked=true;v->ops=&shared_ops;v->priv=p;
    if(demand)p->count=count;
    else for(;p->count<count;p->count++){
        p->pages[p->count]=pmm_alloc_zeroed();
        if(!p->pages[p->count]){shared_release(v);return -N_ERR_NO_MEMORY;}
    }
    struct file *f=vfs_open_vnode(v,flags);
    if(!f){shared_release(v);return -N_ERR_NO_MEMORY;}
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
    if(!m){vfs_close(f);return -N_ERR_NO_MEMORY;}
    struct shared_pages *p=f->vn->priv;
    uint64_t flags=PTE_U|PTE_SHARED|PTE_SHARED_RO|(exec?0:pte_nx);size_t mapped=0;
    for(;mapped<p->count;mapped++)if(!vmm_map_page(t->pml4,start+mapped*PAGE_SIZE,p->pages[mapped],flags))break;
    if(mapped!=p->count){vmm_user_free(t->pml4,start,mapped*PAGE_SIZE);vfs_close(f);kfree(m);return -N_ERR_NO_MEMORY;}
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
    if(!bytes || bytes>SIZE_MAX-(PAGE_SIZE-1) || (prot&~N_PROT_WRITE))return -N_ERR_INVALID;
    struct task *owner=task_process();struct file *f=NULL;
    uint64_t irq=irq_save();struct task *peer=pid?task_find(pid):owner;
    if(peer && !peer->is_thread && peer->is_user && !peer->exiting &&
       (peer==owner || peer->parent==owner || owner->parent==peer) &&
       fd>=0 && (size_t)fd<peer->fd_capacity && peer->fds[fd] &&
       peer->fds[fd]->vn->type==VT_SHARED)f=vfs_dup(peer->fds[fd]);
    irq_restore(irq);if(!f)return -N_ERR_NO_HANDLE;
    bytes=ALIGN_UP(bytes,PAGE_SIZE);
    if(bytes!=f->vn->size){vfs_close(f);return -N_ERR_INVALID;}
    struct shared_mapping *m=kzalloc(sizeof *m);
    if(!m){vfs_close(f);return -N_ERR_NO_MEMORY;}
    /* Reserve the VA range before publishing any PTE. No blocking handoff is
       made in this page loop, but the short BSP guard also keeps future thread
       mmap/clock/stack reservations from choosing this partially mapped span. */
    irq=irq_save();uint64_t va=owner->mmap_next;
    if(va>=USER_STACK_TOP-USER_STACK_MAX || bytes+PAGE_SIZE<bytes ||
       bytes+PAGE_SIZE>USER_STACK_TOP-USER_STACK_MAX-va){irq_restore(irq);vfs_close(f);kfree(m);return -N_ERR_NO_MEMORY;}
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
        vfs_close(f);kfree(m);return -N_ERR_NO_MEMORY;
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
    if(!f)return -N_ERR_NO_HANDLE;
    if(f->vn->type!=VT_SHARED){vfs_close(f);return -N_ERR_INVALID;}
    struct shared_ticket *t=kzalloc(sizeof *t);
    if(!t){vfs_close(f);return -N_ERR_NO_MEMORY;}
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
    if(!f)return -N_ERR_NO_HANDLE;
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
    if(!v)return -N_ERR_NO_MEMORY;
    struct file *f=vfs_open_vnode(v,O_RDONLY);
    if(!f){v->ops->release(v);return -N_ERR_NO_MEMORY;}
    int fd=allocfd(f);if(fd<0)vfs_close(f);return fd;
}
static int64_t sys_address_watch(int fd,uint64_t address,uint32_t expected,uint64_t token) {
    if((address&3) || !user_ok((void *)address,sizeof(uint32_t)))return -N_ERR_BAD_ADDRESS;
    struct file *f=getfd_ref(fd);
    if(!f)return -N_ERR_NO_HANDLE;
    int64_t r=address_watch_add(f->vn,(volatile uint32_t *)address,expected,token);
    vfs_close(f);return r;
}
static int64_t sys_address_unwatch(int fd,uint64_t token) {
    struct file *f=getfd_ref(fd);
    if(!f)return -N_ERR_NO_HANDLE;
    int64_t r=address_watch_remove(f->vn,token);
    vfs_close(f);return r;
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

/* anonymous memory in its own region above the heap (window buffers live there too) */
static int64_t sys_mmap_locked(size_t len, int prot, int place) {
    struct task *t = task_process();
    if (len == 0 || len > (1ULL << 32) || (place & ~N_MMAP_LOW)) return -N_ERR_INVALID;
    len = ALIGN_UP(len, PAGE_SIZE);
    int vp = (prot & N_PROT_WRITE ? VM_W : 0) | (prot & N_PROT_EXEC ? VM_X : 0);
    uint64_t va = place & N_MMAP_LOW ? region_reserve(&t->low_holes, &t->low_next, USER_LOW_TOP, len)
                                     : mmap_reserve(t, len);
    if (!va) return -N_ERR_NO_MEMORY;
    if (vmm_user_alloc(t->pml4, va, len, vp) < 0) {
        vmm_user_free(t->pml4, va, len);
        mmap_hole_add(t, va, len + PAGE_SIZE);
        return -N_ERR_NO_MEMORY;
    }
    return (int64_t)va;
}

static int64_t sys_munmap_locked(uint64_t va, size_t len) {
    struct task *t = task_process();
    bool low = va < USER_MMAP_BASE;
    uint64_t base = low ? t->low_base : USER_MMAP_BASE, frontier = low ? t->low_next : t->mmap_next;
    if (va & (PAGE_SIZE - 1) || va < base || va + len > frontier || va + len < va) return -N_ERR_INVALID;
    if(len>SIZE_MAX-(PAGE_SIZE-1))return -N_ERR_INVALID;
    len=ALIGN_UP(len,PAGE_SIZE);
    struct shared_mapping **link=&t->shared_mappings;
    while(*link){struct shared_mapping *m=*link;
        if(va<m->va+m->bytes && m->va<va+len){
            if(va!=m->va || len!=m->bytes)return -N_ERR_INVALID;
            *link=m->next;vmm_user_free(t->pml4,va,len);vfs_close(m->file);kfree(m);return 0;
        }link=&m->next;
    }
    for (uint64_t a = va; a < va + len; a += PAGE_SIZE)
        if (vmm_get_pte(t->pml4, a) & PTE_SHARED) return -N_ERR_INVALID; /* a window buffer */
    vmm_user_free(t->pml4, va, len);
    /* The unmapped page right after the range is the reservation's guard. */
    uint64_t end = va + len;
    bool guard = end < frontier && !(vmm_get_pte(t->pml4, end) & PTE_P);
    for (struct shared_mapping *m = t->shared_mappings; guard && m; m = m->next)
        if (end >= m->va && end < m->va + m->bytes) guard = false; /* an untouched demand page */
    mmap_hole_add(t, va, guard ? len + PAGE_SIZE : len);
    return 0;
}

static int64_t sys_mprotect_locked(uint64_t va, size_t len, int prot) {
    if (va & (PAGE_SIZE - 1) || va >= USER_TOP || va + len > USER_TOP || va + len < va) return -N_ERR_INVALID;
    int vp = (prot & N_PROT_WRITE ? VM_W : 0) | (prot & N_PROT_EXEC ? VM_X : 0);
    return vmm_user_protect(current_task->pml4, va, len, vp);
}

/* These finite BSP helpers do not sleep. Kernel-mode timer IRQs request a
   reschedule but cannot switch their continuation (idt.c). Keep IRQ delivery
   enabled for large allocations; each PTE publication has its own short guard. */
static int64_t sys_mmap(size_t len, int prot, int place) {
    return sys_mmap_locked(len, prot, place);
}
static int64_t sys_munmap(uint64_t va, size_t len) {
    return sys_munmap_locked(va, len);
}
static int64_t sys_mprotect(uint64_t va, size_t len, int prot) {
    return sys_mprotect_locked(va, len, prot);
}

static int64_t sys_dmesg(char *buf, size_t n) {
    if (!user_ok_w(buf, n)) return -N_ERR_BAD_ADDRESS;
    return klog_read(buf, n);
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

static int64_t sys_handle_close(int h) {
    struct file *f = getfd_ref(h);
    if (!f) return -N_ERR_NO_HANDLE;
    /* Only an explicit close commits a replacement: an app that ends with the
       handle open leaves the old document as it was. */
    int committed = vfs_document_commit(f);
    vfs_close(f);
    int closed = (int)sys_close(h);
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
    return f->vn->type == VT_PIPE || f->vn->type == VT_CHAR ||
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

static int64_t sys_channel_receive(int h, void *ubuf, size_t cap, int flags, int32_t *uattached) {
    if (uattached && !user_ok_w(uattached, sizeof *uattached)) return -N_ERR_BAD_ADDRESS;
    struct file *f = getfd_ref(h);
    if (!f) return -N_ERR_NO_HANDLE;
    bool wait = flags & N_CHANNEL_WAIT, peek = flags & N_CHANNEL_PEEK;
    int64_t r;
    struct file *att = NULL;
    if (message_channel_is(f)) {
        r = message_channel_receive(f, ubuf, cap, wait, peek, &att);
    } else if (byte_channel(f)) {
        if (peek) r = -N_ERR_UNSUPPORTED;
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
    if (r >= 0 && !peek && uattached) {
        int32_t handle = att ? (int32_t)install(att) : -1;
        att = NULL;
        if (copy_to_user(uattached, &handle, sizeof handle) < 0 && handle >= 0) sys_close(handle);
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
                                 .cwd = "/", .parent = task_process(), .app = app};
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
    if (flags & N_APP_STOP_TREE) sys_killtree(pid, 1);
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
    case SYS_EXIT: task_exit((int)a);
    case SYS_READ: ret = sys_read((int)a, (void *)b, c); break;
    case SYS_WRITE: ret = sys_write((int)a, (const void *)b, c); break;
    case SYS_OPEN: ret = sys_open((const char *)a, (int)b); break;
    case SYS_CLOSE: ret = sys_close((int)a); break;
    case SYS_AUDIO_FLUSH: {
        struct file *f = getfd_ref((int)a);
        ret = f ? audio_flush_file(f) : -N_ERR_NO_HANDLE;
        vfs_close(f);
        break;
    }
    case SYS_LSEEK: {
        struct file *f = getfd_ref((int)a);
        ret = f ? vfs_seek(f, (int64_t)b, (int)c) : -N_ERR_NO_HANDLE;
        vfs_close(f);
        break;
    }
    case SYS_FTRUNCATE: {
        struct file *f = getfd_ref((int)a);
        ret = f ? vfs_truncate(f, b) : -N_ERR_NO_HANDLE;
        vfs_close(f);
        break;
    }
    case SYS_STAT: ret = sys_stat((const char *)a, (struct kstat *)b); break;
    case SYS_FSTAT: ret = sys_fstat((int)a, (struct kstat *)b); break;
    case SYS_READDIR: ret = sys_readdir((int)a, b, (struct n_dirent *)c); break;
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
        if (!request.width || !request.height || request.width > N_GPU_MAX_SIDE || request.height > N_GPU_MAX_SIDE) {
            ret = -N_ERR_INVALID; break;
        }
        size_t bytes = (size_t)request.width * request.height * sizeof(uint32_t);
        if (c < bytes) { ret = -N_ERR_INVALID; break; }
        if (!user_ok_w((void *)b, bytes)) { ret = -N_ERR_BAD_ADDRESS; break; }
        ret = gpu_render(&request, (uint32_t *)b, bytes);
        break;
    }
    case SYS_GPU_RENDER_BATCH: {
        /* Snapshot the complete finite request before the driver uses it.
           A caller changing its user buffer cannot grow a validated batch. */
        struct n_gpu_batch request;
        if (!user_ok((void *)a, sizeof request)) { ret = -N_ERR_BAD_ADDRESS; break; }
        memcpy(&request, (void *)a, sizeof request);
        if (!request.width || !request.height ||
            request.width > N_GPU_MAX_SIDE || request.height > N_GPU_MAX_SIDE ||
            request.triangle_count > N_GPU_MAX_TRIANGLES) {
            ret = -N_ERR_INVALID; break;
        }
        size_t bytes = (size_t)request.width * request.height * sizeof(uint32_t);
        if (c < bytes) { ret = -N_ERR_INVALID; break; }
        if (!user_ok_w((void *)b, bytes)) { ret = -N_ERR_BAD_ADDRESS; break; }
        ret = gpu_render_batch(&request, (uint32_t *)b, bytes);
        break;
    }
    case SYS_GPU_BLIT: {
        struct n_gpu_blit request;
        if (!user_ok((void *)a, sizeof request)) { ret = -N_ERR_BAD_ADDRESS; break; }
        memcpy(&request, (void *)a, sizeof request);
        if (!request.width || !request.height ||
            !request.source_width || !request.source_height ||
            request.width > N_GPU_MAX_SIDE || request.height > N_GPU_MAX_SIDE ||
            request.source_width > N_GPU_MAX_SIDE || request.source_height > N_GPU_MAX_SIDE ||
            !request.source_w || !request.source_h ||
            request.source_x > request.source_width || request.source_y > request.source_height ||
            request.source_w > request.source_width - request.source_x ||
            request.source_h > request.source_height - request.source_y) {
            ret = -N_ERR_INVALID; break;
        }
        size_t source_bytes = (size_t)request.source_width * request.source_height * sizeof(uint32_t);
        size_t output_bytes = (size_t)request.width * request.height * sizeof(uint32_t);
        if (c < source_bytes || e < output_bytes) { ret = -N_ERR_INVALID; break; }
        if (!user_ok((void *)b, source_bytes) || !user_ok_w((void *)d, output_bytes)) {
            ret = -N_ERR_BAD_ADDRESS; break;
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
        ret = -N_ERR_IO;
        break;
    case SYS_FCNTL: ret = sys_fcntl((int)a, (int)b, (int)c); break;
    case SYS_DMESG: ret = sys_dmesg((char *)a, b); break;
    case SYS_MMAP: ret = sys_mmap(a, (int)b, (int)c); break;
    case SYS_MUNMAP: ret = sys_munmap(a, b); break;
    case SYS_MPROTECT: ret = sys_mprotect(a, b, (int)c); break;
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
    case SYS_CHANNEL_RECEIVE: ret = sys_channel_receive((int)a, (void *)b, c, (int)d, (int32_t *)e); break;
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
    default:
        if (r->rax >= SYS_WIN_CREATE && r->rax < SYS_NET_INFO) ret = wm_syscall((int)r->rax, a, b, c, d, e);
        else if (r->rax >= SYS_NET_INFO && r->rax < SYS_NET_LIMIT) ret = net_syscall((int)r->rax, a, b, c, d, e);
        break;
    }
    r->rax = (uint64_t)ret;
}
