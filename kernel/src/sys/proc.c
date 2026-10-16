/* User processes: ELF64 loading and spawning. */
#include "kernel.h"
#include "arch/cpu.h"
#include "sys/sched.h"
#include "sys/proc.h"
#include "sys/syscall.h"
#include "fs/vfs.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "mm/pmm.h"

struct elf64_ehdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} PACKED;

struct elf64_phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} PACKED;

#define PT_LOAD 1
#define PF_X    1
#define PF_W    2

extern void user_trampoline(void);

static void kpush(struct task *t, uint64_t v) {
    t->ksp -= 8;
    *(uint64_t *)t->ksp = v;
}

/* ---- app handles: an app's end is an event on its handle ---- */

struct app_record {
    int pid;
    int status;
    bool ended;
    struct wait_queue wait;
};

static bool app_can_read(struct vnode *v, struct file *f) {
    (void)f;
    return ((struct app_record *)v->priv)->ended;
}
static void app_release(struct vnode *v) {
    kfree(v->priv);
    kfree(v);
}
static struct vnode_ops app_ops = {.can_read = app_can_read, .release = app_release};

struct vnode *proc_app_new(void) {
    struct vnode *v = kzalloc(sizeof *v);
    struct app_record *rec = kzalloc(sizeof *rec);
    if (!v || !rec) { kfree(v); kfree(rec); return NULL; }
    strlcpy(v->name, "app", sizeof v->name);
    v->type = VT_APP;
    v->ops = &app_ops;
    v->priv = rec;
    v->unlinked = true; /* freed when the app has ended and its last handle is closed */
    return v;
}

/* The app's process is ending; BSP IRQ guard held. */
void proc_app_ended(struct vnode *v, int status) {
    struct app_record *rec = v->priv;
    rec->status = status;
    rec->ended = true;
    wq_wake_all(&rec->wait);
    vfs_poll_notify(v);
}

bool proc_app_is(struct vnode *v) { return v && v->ops == &app_ops; }

/* The app's identifier while it runs; 0 once it has ended. */
int proc_app_pid(struct vnode *v) {
    struct app_record *rec = v->priv;
    uint64_t flags = irq_save();
    int pid = rec->ended ? 0 : rec->pid;
    irq_restore(flags);
    return pid;
}

int proc_app_result(struct vnode *v, uint64_t timeout_ms, int *status) {
    struct app_record *rec = v->priv;
    uint64_t deadline = uptime_ms() + timeout_ms;
    uint64_t flags = irq_save();
    while (!rec->ended) {
        uint64_t now = uptime_ms();
        if (timeout_ms != UINT64_MAX && now >= deadline) {
            irq_restore(flags);
            return timeout_ms ? -N_ERR_TIMEOUT : -N_ERR_NOT_READY;
        }
        if (current_task->killed) { irq_restore(flags); return -N_ERR_STOPPED; }
        if (timeout_ms == UINT64_MAX) wq_wait(&rec->wait);
        else wq_wait_timeout(&rec->wait, deadline - now);
    }
    *status = rec->status;
    irq_restore(flags);
    return 0;
}

int proc_spawn(const char *path, int argc, char **argv, struct file *fds[3], const char *cwd,
               struct task *parent) {
    struct proc_launch l = {.argc = argc, .argv = argv, .handles = fds, .handle_count = fds ? 3 : 0,
                            .cwd = cwd, .parent = parent};
    return proc_launch(path, &l);
}

int proc_launch(const char *path, const struct proc_launch *l) {
    int argc = l->argc;
    char **argv = l->argv;
    uint8_t *img;
    uint64_t size;
    uint64_t *pointers = NULL;
    int r = vfs_read_whole(path, &img, &size);
    if (r < 0) return r;
    struct elf64_ehdr *eh = (struct elf64_ehdr *)img;
    if (size < sizeof *eh || memcmp(eh->ident, "\x7f" "ELF", 4) || eh->ident[4] != 2 || eh->machine != 0x3E ||
        eh->type != 2 || eh->phoff + (uint64_t)eh->phnum * sizeof(struct elf64_phdr) > size) {
        kfree(img);
        return -N_ERR_NOT_PROGRAM;
    }
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    struct task *t = task_alloc(base);
    if (!t) {
        kfree(img);
        return -N_ERR_NO_MEMORY;
    }
    t->is_user = true;
    t->pml4 = vmm_new_space();
    if (!t->pml4) goto fail;

    uint64_t top = 0;
    struct elf64_phdr *ph = (struct elf64_phdr *)(img + eh->phoff);
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type != PT_LOAD || ph[i].memsz == 0) continue;
        if (ph[i].vaddr >= USER_TOP || ph[i].vaddr + ph[i].memsz > USER_MMAP_BASE || ph[i].offset + ph[i].filesz > size)
            goto fail;
        int prot = (ph[i].flags & PF_W ? VM_W : 0) | (ph[i].flags & PF_X ? VM_X : 0);
        /* programs built by tcc before crt0's _start section was marked executable */
        if (!(prot & VM_X) && eh->entry >= ph[i].vaddr && eh->entry - ph[i].vaddr < ph[i].memsz) {
            kprintf("exec: %s: entry point is not executable (old tcc build?); allowing it\n", path);
            prot |= VM_X;
        }
        /* A read-only segment whose pages no other segment touches is shared
           with every task running the same bytes. */
        bool alone = !(prot & VM_W) && ph[i].filesz <= ph[i].memsz;
        uint64_t first = ALIGN_DOWN(ph[i].vaddr, PAGE_SIZE), last = ALIGN_UP(ph[i].vaddr + ph[i].memsz, PAGE_SIZE);
        for (int j = 0; alone && j < eh->phnum; j++)
            if (j != i && ph[j].type == PT_LOAD && ph[j].memsz &&
                ALIGN_DOWN(ph[j].vaddr, PAGE_SIZE) < last && ALIGN_UP(ph[j].vaddr + ph[j].memsz, PAGE_SIZE) > first)
                alone = false;
        if (alone && shared_program_segment(t, path, ph[i].vaddr, img + ph[i].offset, ph[i].filesz,
                                            ph[i].memsz, prot & VM_X) == 0) {
            if (ph[i].vaddr + ph[i].memsz > top) top = ph[i].vaddr + ph[i].memsz;
            continue;
        }
        if (vmm_user_alloc(t->pml4, ph[i].vaddr, ph[i].memsz, prot) < 0) goto fail;
        if (vmm_copy_to_space(t->pml4, ph[i].vaddr, img + ph[i].offset, ph[i].filesz) < 0) goto fail;
        if (ph[i].vaddr + ph[i].memsz > top) top = ph[i].vaddr + ph[i].memsz;
    }
    t->brk_base = t->brk = ALIGN_UP(top, PAGE_SIZE);
    t->mmap_next = USER_MMAP_BASE;
    t->low_base = t->low_next = MAX(USER_LOW_BASE, t->brk_base);

    /* User stack: the argument and setting strings, then their two
       null-terminated pointer arrays. They must fit well inside the stack. */
    int settings = l->setting_count;
    size_t strings = 0;
    for (int i = 0; i < argc; i++) strings += strlen(argv[i]) + 1;
    for (int i = 0; i < settings; i++) strings += strlen(l->settings[i]) + 1;
    size_t pointer_bytes = ((size_t)argc + 1 + (size_t)settings + 1) * 8;
    if (strings + pointer_bytes > USER_STACK_MAX / 2) { r = -N_ERR_TOO_MANY_ARGUMENTS; goto fail; }
    pointers = kmalloc(pointer_bytes);
    if (!pointers) { r = -N_ERR_NO_MEMORY; goto fail; }
    /* pre-fault what the start needs plus 64 KiB; the rest grows on demand */
    uint64_t prefault = ALIGN_UP(strings + pointer_bytes + 64 + 65536, PAGE_SIZE);
    if (vmm_user_alloc(t->pml4, USER_STACK_TOP - prefault, prefault, VM_W) < 0) { r = -N_ERR_NO_MEMORY; goto fail; }
    if (l->handle_count > t->fd_capacity && task_fd_reserve(t, l->handle_count) < 0) { r = -N_ERR_NO_MEMORY; goto fail; }
    uint64_t sp = USER_STACK_TOP;
    for (int i = settings - 1; i >= 0; i--) {
        size_t n = strlen(l->settings[i]) + 1;
        sp -= n;
        if (vmm_copy_to_space(t->pml4, sp, l->settings[i], n) < 0) goto fail;
        pointers[argc + 1 + i] = sp;
    }
    for (int i = argc - 1; i >= 0; i--) {
        size_t n = strlen(argv[i]) + 1;
        sp -= n;
        if (vmm_copy_to_space(t->pml4, sp, argv[i], n) < 0) goto fail;
        pointers[i] = sp;
    }
    pointers[argc] = 0;
    pointers[argc + 1 + settings] = 0;
    sp = ALIGN_DOWN(sp, 16);
    sp -= pointer_bytes;
    sp = ALIGN_DOWN(sp, 16);
    if (vmm_copy_to_space(t->pml4, sp, pointers, pointer_bytes) < 0) goto fail;
    uint64_t argv_ptr = sp, settings_ptr = sp + ((uint64_t)argc + 1) * 8;
    kfree(pointers);
    pointers = NULL;
    sp -= 8; /* fake return address slot keeps the ABI alignment */

    /* initial interrupt frame */
    struct regs frame;
    memset(&frame, 0, sizeof frame);
    frame.rip = eh->entry;
    frame.cs = USER_CS;
    frame.rflags = 0x202;
    frame.rsp = sp;
    frame.ss = USER_DS;
    frame.rdi = argc;
    frame.rsi = argv_ptr;
    frame.rdx = settings_ptr;
    t->ksp -= sizeof frame;
    memcpy((void *)t->ksp, &frame, sizeof frame);
    kpush(t, (uint64_t)user_trampoline);
    for (int i = 0; i < 6; i++) kpush(t, 0);

    for (size_t i = 0; i < l->handle_count; i++)
        if (l->handles[i]) t->fds[i] = vfs_dup(l->handles[i]);
    strlcpy(t->cwd, l->cwd ? l->cwd : "/", sizeof t->cwd);
    t->parent = l->parent;
    if (l->app) {
        /* The task's own reference keeps the record until it has reported. */
        vnode_ref(l->app);
        ((struct app_record *)l->app->priv)->pid = t->pid;
        t->app = l->app;
    }
    kfree(img);
    sched_make_ready(t);
    return t->pid;

fail:
    if (r >= 0) r = -N_ERR_NOT_PROGRAM;
    kfree(pointers);
    kfree(img);
    if (t->pml4) {
        sys_shared_cleanup(t);
        vmm_free_space(t->pml4);
    }
    t->pml4 = kernel_pml4;
    t->state = TASK_ZOMBIE;
    t->parent = NULL;
    /* nobody waits for it: free right away */
    {
        uint64_t f = irq_save();
        struct task **pp = &task_list;
        while (*pp && *pp != t) pp = &(*pp)->all_next;
        if (*pp) *pp = t->all_next;
        irq_restore(f);
        vfree(t->kstack, KSTACK_PAGES);
        if(t->fds!=t->initial_fds)kfree(t->fds);
        kfree(t);
    }
    return r;
}

int proc_spawn_simple(const char *path, const char *arg1, struct task *parent) {
    char *argv[3] = {(char *)path, (char *)arg1, NULL};
    struct file *fds[3] = {NULL, NULL, NULL};
    struct file *nul = NULL;
    if (vfs_open("/dev/null", O_RDWR, &nul) == 0) fds[0] = fds[1] = fds[2] = nul;
    int r = proc_spawn(path, arg1 ? 2 : 1, argv, fds, "/home", parent);
    if (nul) vfs_close(nul);
    return r;
}

uint64_t proc_user_pages(struct task *t) {
    if (!t->is_user || t->is_thread || t->pml4 == kernel_pml4) return 0;
    return vmm_user_pages(t->pml4);
}
