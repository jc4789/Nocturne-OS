/* System call dispatch (int 0x80). */
#include "kernel.h"
#include "arch/cpu.h"
#include "sys/sched.h"
#include "sys/syscall.h"
#include "sys/proc.h"
#include "fs/vfs.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "dev/timer.h"
#include "dev/fb.h"
#include "gui/wm.h"
#include "abi.h"

_Static_assert(sizeof(struct n_dirent) == sizeof(struct dirent), "dirent ABI");
_Static_assert(sizeof(struct n_stat) == sizeof(struct kstat), "stat ABI");

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
            return false;
        }
        if (!(pte & PTE_U)) return false;
        if (write && !(pte & PTE_W)) return false;
    }
    return true;
}

bool user_ok(const void *p, size_t n) { return user_access(p, n, false); }
bool user_ok_w(void *p, size_t n) { return user_access(p, n, true); }

int user_str(char *dst, const char *src, size_t max) {
    for (size_t i = 0; i < max; i++) {
        if (!user_ok(src + i, 1)) return -EFAULT;
        dst[i] = src[i];
        if (!dst[i]) return (int)i;
    }
    dst[max - 1] = 0;
    return -ENAMETOOLONG;
}

static int user_path(const char *upath, char *out) {
    char tmp[PATH_MAX_LEN];
    int r = user_str(tmp, upath, sizeof tmp);
    if (r < 0) return r;
    return vfs_normalize(current_task->cwd, tmp, out);
}

static struct file *getfd(int fd) {
    if (fd < 0 || fd >= MAX_FDS) return NULL;
    return current_task->fds[fd];
}

static int allocfd(struct file *f) {
    for (int i = 0; i < MAX_FDS; i++) {
        if (!current_task->fds[i]) {
            current_task->fds[i] = f;
            return i;
        }
    }
    return -EMFILE;
}

/* ---- handlers ---- */

static int64_t sys_read(int fd, void *buf, size_t n) {
    struct file *f = getfd(fd);
    if (!f) return -EBADF;
    if (!user_ok_w(buf, n)) return -EFAULT;
    return vfs_read(f, buf, n);
}

static int64_t sys_write(int fd, const void *buf, size_t n) {
    struct file *f = getfd(fd);
    if (!f) return -EBADF;
    if (!user_ok(buf, n)) return -EFAULT;
    return vfs_write(f, buf, n);
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
    struct file *f = getfd(fd);
    if (!f) return -EBADF;
    current_task->fds[fd] = NULL;
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
    struct file *f = getfd(fd);
    if (!f) return -EBADF;
    if (!user_ok_w(st, sizeof *st)) return -EFAULT;
    st->type = f->vn->tty ? VT_CHAR : f->vn->type;
    st->size = f->vn->size;
    st->mtime = f->vn->mtime;
    st->mode = f->vn->mode;
    return 0;
}

static int64_t sys_readdir(int fd, uint64_t idx, struct dirent *d) {
    struct file *f = getfd(fd);
    if (!f) return -EBADF;
    if (!user_ok_w(d, sizeof *d)) return -EFAULT;
    return vfs_readdir(f, idx, d);
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
    strlcpy(current_task->cwd, path, sizeof current_task->cwd);
    return 0;
}

static int64_t sys_getcwd(char *buf, size_t n) {
    size_t l = strlen(current_task->cwd) + 1;
    if (n < l) return -ERANGE;
    if (!user_ok_w(buf, l)) return -EFAULT;
    memcpy(buf, current_task->cwd, l);
    return l;
}

static int64_t sys_spawn(const char *upath, char **uargv, const int *ufdmap, int flags) {
    char path[PATH_MAX_LEN];
    int r = user_path(upath, path);
    if (r < 0) return r;
    char *argv[32];
    int argc = 0;
    int64_t ret;
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
    struct file *fds[3];
    for (int i = 0; i < 3; i++) {
        int src = i;
        if (ufdmap) {
            if (!user_ok(&ufdmap[i], 4)) { ret = -EFAULT; goto out; }
            src = ufdmap[i];
        }
        fds[i] = src >= 0 ? getfd(src) : NULL;
    }
    ret = proc_spawn(path, argc, argv, fds, current_task->cwd, (flags & SPAWN_DETACH) ? NULL : current_task);
out:
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

static int64_t sys_sbrk(int64_t inc) {
    struct task *t = current_task;
    uint64_t old = t->brk;
    if (inc == 0) return old;
    uint64_t nb = t->brk + inc;
    if (nb < t->brk_base || nb >= USER_MMAP_BASE) return -ENOMEM;
    if (inc > 0) {
        if (vmm_user_alloc(t->pml4, ALIGN_UP(old, PAGE_SIZE), ALIGN_UP(nb, PAGE_SIZE) - ALIGN_UP(old, PAGE_SIZE), VM_W) < 0)
            return -ENOMEM;
    } else {
        vmm_user_free(t->pml4, ALIGN_UP(nb, PAGE_SIZE), ALIGN_UP(old, PAGE_SIZE) - ALIGN_UP(nb, PAGE_SIZE));
    }
    t->brk = nb;
    return old;
}

static int64_t sys_pipe(int *ufds) {
    if (!user_ok_w(ufds, 8)) return -EFAULT;
    struct file *r, *w;
    int e = pipe_create(&r, &w);
    if (e < 0) return e;
    int a = allocfd(r);
    if (a < 0) {
        vfs_close(r);
        vfs_close(w);
        return a;
    }
    int b = allocfd(w);
    if (b < 0) {
        current_task->fds[a] = NULL;
        vfs_close(r);
        vfs_close(w);
        return b;
    }
    ufds[0] = a;
    ufds[1] = b;
    return 0;
}

static int64_t sys_dup2(int oldfd, int newfd) {
    struct file *f = getfd(oldfd);
    if (!f || newfd < 0 || newfd >= MAX_FDS) return -EBADF;
    if (oldfd == newfd) return newfd;
    if (current_task->fds[newfd]) vfs_close(current_task->fds[newfd]);
    current_task->fds[newfd] = vfs_dup(f);
    return newfd;
}

static int64_t sys_dup(int oldfd) {
    struct file *f = getfd(oldfd);
    if (!f) return -EBADF;
    int fd = allocfd(f);
    if (fd >= 0) vfs_dup(f);
    return fd;
}

static int64_t sys_poll(struct n_pollfd *ufds, int n, int timeout) {
    if (n < 0 || n > 64) return -EINVAL;
    if (!user_ok_w(ufds, n * sizeof(struct n_pollfd))) return -EFAULT;
    uint64_t deadline = timeout >= 0 ? uptime_ms() + timeout : 0;
    for (;;) {
        uint64_t fl = irq_save();
        int ready = 0;
        for (int i = 0; i < n; i++) {
            ufds[i].revents = 0;
            struct file *f = getfd(ufds[i].fd);
            if (!f) continue;
            if ((ufds[i].events & N_POLLIN) && vfs_can_read(f)) ufds[i].revents |= N_POLLIN;
            if ((ufds[i].events & N_POLLOUT) && vfs_can_write(f)) ufds[i].revents |= N_POLLOUT;
            if (ufds[i].revents) ready++;
        }
        if (ready || timeout == 0) {
            irq_restore(fl);
            return ready;
        }
        if (current_task->killed) {
            irq_restore(fl);
            return -EINTR;
        }
        if (timeout > 0) {
            uint64_t now = uptime_ms();
            if (now >= deadline) {
                irq_restore(fl);
                return 0;
            }
            wq_wait_timeout(&poll_wq, deadline - now);
        } else {
            wq_wait(&poll_wq);
        }
        irq_restore(fl);
    }
}

static int64_t sys_proclist(struct n_procinfo *out, int max) {
    if (max < 0 || !user_ok_w(out, (size_t)max * sizeof *out)) return -EFAULT;
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
    }
    return n;
}

static int64_t sys_sysinfo(struct n_sysinfo *si) {
    if (!user_ok_w(si, sizeof *si)) return -EFAULT;
    memset(si, 0, sizeof *si);
    si->total_mem = pmm_total_pages() * PAGE_SIZE;
    si->free_mem = pmm_free_pages() * PAGE_SIZE;
    si->heap_used = heap_used_bytes();
    si->uptime_ms = uptime_ms();
    int n = 0;
    for (struct task *t = task_list; t; t = t->all_next) n++;
    si->ntasks = n;
    si->fb_w = fb.width;
    si->fb_h = fb.height;
    if (wm_running()) wm_screen_size(&si->fb_w, &si->fb_h); /* the desktop may be a remote viewer's size */
    strlcpy(si->cpu, cpu_brand, sizeof si->cpu);
    strlcpy(si->os, OS_NAME " " OS_VERSION, sizeof si->os);
    return 0;
}

static int64_t sys_fcntl(int fd, int cmd, int arg) {
    struct file *f = getfd(fd);
    if (!f) return -EBADF;
    if (cmd == F_GETFL) return f->flags;
    if (cmd == F_SETTTY) {
        f->vn->tty = arg != 0;
        return 0;
    }
    if (cmd == F_SETFL) {
        f->flags = (f->flags & O_ACCMODE) | (arg & ~O_ACCMODE);
        return 0;
    }
    return -EINVAL;
}

static int64_t sys_killtree(int pid, int include_self) {
    /* kill every descendant of pid (used for Ctrl+C) */
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
    return killed;
}

/* anonymous memory in its own region above the heap (window buffers live there too) */
static int64_t sys_mmap(size_t len, int prot) {
    struct task *t = current_task;
    if (len == 0 || len > (1ULL << 32)) return -EINVAL;
    len = ALIGN_UP(len, PAGE_SIZE);
    uint64_t va = t->mmap_next;
    int vp = (prot & N_PROT_WRITE ? VM_W : 0) | (prot & N_PROT_EXEC ? VM_X : 0);
    if (vmm_user_alloc(t->pml4, va, len, vp) < 0) {
        vmm_user_free(t->pml4, va, len);
        return -ENOMEM;
    }
    t->mmap_next = va + len + PAGE_SIZE; /* leave a guard page */
    return (int64_t)va;
}

static int64_t sys_munmap(uint64_t va, size_t len) {
    struct task *t = current_task;
    if (va & (PAGE_SIZE - 1) || va < USER_MMAP_BASE || va + len > t->mmap_next || va + len < va) return -EINVAL;
    for (uint64_t a = va; a < va + len; a += PAGE_SIZE)
        if (vmm_get_pte(t->pml4, a) & PTE_SHARED) return -EINVAL; /* a window buffer */
    vmm_user_free(t->pml4, va, len);
    return 0;
}

static int64_t sys_mprotect(uint64_t va, size_t len, int prot) {
    if (va & (PAGE_SIZE - 1) || va >= USER_TOP || va + len > USER_TOP || va + len < va) return -EINVAL;
    int vp = (prot & N_PROT_WRITE ? VM_W : 0) | (prot & N_PROT_EXEC ? VM_X : 0);
    return vmm_user_protect(current_task->pml4, va, len, vp);
}

static int64_t sys_dmesg(char *buf, size_t n) {
    if (!user_ok_w(buf, n)) return -EFAULT;
    return klog_read(buf, n);
}

void syscall_dispatch(struct regs *r) {
    sti();
    uint64_t a = r->rdi, b = r->rsi, c = r->rdx, d = r->r10, e = r->r8;
    int64_t ret = -ENOSYS;
    switch (r->rax) {
    case SYS_EXIT: task_exit((int)a);
    case SYS_READ: ret = sys_read((int)a, (void *)b, c); break;
    case SYS_WRITE: ret = sys_write((int)a, (const void *)b, c); break;
    case SYS_OPEN: ret = sys_open((const char *)a, (int)b); break;
    case SYS_CLOSE: ret = sys_close((int)a); break;
    case SYS_LSEEK: {
        struct file *f = getfd((int)a);
        ret = f ? vfs_seek(f, (int64_t)b, (int)c) : -EBADF;
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
    case SYS_GETPID: ret = current_task->pid; break;
    case SYS_GETPPID: ret = current_task->parent ? current_task->parent->pid : 0; break;
    case SYS_KILL: ret = task_kill((int)a); break;
    case SYS_KILLTREE: ret = sys_killtree((int)a, (int)b); break;
    case SYS_SBRK: ret = sys_sbrk((int64_t)a); break;
    case SYS_SLEEP: sleep_ms(a); ret = 0; break;
    case SYS_UPTIME: ret = uptime_ms(); break;
    case SYS_TIME: ret = time_now(); break;
    case SYS_PIPE: ret = sys_pipe((int *)a); break;
    case SYS_DUP2: ret = sys_dup2((int)a, (int)b); break;
    case SYS_DUP: ret = sys_dup((int)a); break;
    case SYS_POLL: ret = sys_poll((struct n_pollfd *)a, (int)b, (int)c); break;
    case SYS_PROCLIST: ret = sys_proclist((struct n_procinfo *)a, (int)b); break;
    case SYS_SYSINFO: ret = sys_sysinfo((struct n_sysinfo *)a); break;
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
