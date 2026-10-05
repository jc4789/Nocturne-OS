/* User processes: ELF64 loading and spawning. */
#include "kernel.h"
#include "arch/cpu.h"
#include "sys/sched.h"
#include "sys/proc.h"
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

extern void user_trampoline(void);

static void kpush(struct task *t, uint64_t v) {
    t->ksp -= 8;
    *(uint64_t *)t->ksp = v;
}

int proc_spawn(const char *path, int argc, char **argv, struct file *fds[3], const char *cwd,
               struct task *parent) {
    uint8_t *img;
    uint64_t size;
    int r = vfs_read_whole(path, &img, &size);
    if (r < 0) return r;
    struct elf64_ehdr *eh = (struct elf64_ehdr *)img;
    if (size < sizeof *eh || memcmp(eh->ident, "\x7f" "ELF", 4) || eh->ident[4] != 2 || eh->machine != 0x3E ||
        eh->type != 2 || eh->phoff + (uint64_t)eh->phnum * sizeof(struct elf64_phdr) > size) {
        kfree(img);
        return -ENOEXEC;
    }
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    struct task *t = task_alloc(base);
    if (!t) {
        kfree(img);
        return -ENOMEM;
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
        if (vmm_user_alloc(t->pml4, ph[i].vaddr, ph[i].memsz, true) < 0) goto fail;
        if (vmm_copy_to_space(t->pml4, ph[i].vaddr, img + ph[i].offset, ph[i].filesz) < 0) goto fail;
        if (ph[i].vaddr + ph[i].memsz > top) top = ph[i].vaddr + ph[i].memsz;
    }
    t->brk_base = t->brk = ALIGN_UP(top, PAGE_SIZE);
    t->mmap_next = USER_MMAP_BASE;

    /* user stack: pre-fault the top 64 KiB, the rest grows on demand */
    if (vmm_user_alloc(t->pml4, USER_STACK_TOP - 65536, 65536, true) < 0) goto fail;
    uint64_t sp = USER_STACK_TOP;
    uint64_t uargv[64];
    if (argc > 63) argc = 63;
    for (int i = argc - 1; i >= 0; i--) {
        size_t l = strlen(argv[i]) + 1;
        sp -= l;
        if (vmm_copy_to_space(t->pml4, sp, argv[i], l) < 0) goto fail;
        uargv[i] = sp;
    }
    uargv[argc] = 0;
    sp = ALIGN_DOWN(sp, 16);
    sp -= (argc + 1) * 8;
    sp = ALIGN_DOWN(sp, 16);
    if (vmm_copy_to_space(t->pml4, sp, uargv, (argc + 1) * 8) < 0) goto fail;
    uint64_t argv_ptr = sp;
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
    t->ksp -= sizeof frame;
    memcpy((void *)t->ksp, &frame, sizeof frame);
    kpush(t, (uint64_t)user_trampoline);
    for (int i = 0; i < 6; i++) kpush(t, 0);

    for (int i = 0; i < 3; i++)
        if (fds && fds[i]) t->fds[i] = vfs_dup(fds[i]);
    strlcpy(t->cwd, cwd ? cwd : "/", sizeof t->cwd);
    t->parent = parent;
    kfree(img);
    sched_make_ready(t);
    return t->pid;

fail:
    kfree(img);
    if (t->pml4) vmm_free_space(t->pml4);
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
        kfree(t);
    }
    return -ENOEXEC;
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
    if (!t->is_user || t->pml4 == kernel_pml4) return 0;
    uint64_t n = 0;
    uint64_t *l4 = phys_to_virt(t->pml4);
    for (int a = 0; a < 256; a++) {
        if (!(l4[a] & PTE_P)) continue;
        uint64_t *l3 = phys_to_virt(l4[a] & PTE_ADDR);
        for (int b = 0; b < 512; b++) {
            if (!(l3[b] & PTE_P)) continue;
            uint64_t *l2 = phys_to_virt(l3[b] & PTE_ADDR);
            for (int c = 0; c < 512; c++) {
                if (!(l2[c] & PTE_P)) continue;
                uint64_t *l1 = phys_to_virt(l2[c] & PTE_ADDR);
                for (int d = 0; d < 512; d++)
                    if ((l1[d] & PTE_P) && !(l1[d] & PTE_SHARED)) n++;
            }
        }
    }
    return n;
}
